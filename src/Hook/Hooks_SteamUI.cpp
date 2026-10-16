#include "Hooks_SteamUI.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Hook/Hooks_Package.h"
#include "Pipe/PipeManager.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "OSTPlatform/include/Thread.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{


    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    template <typename T>
    class NoDestructor final {
        alignas(T) std::byte m_storage[sizeof(T)];

    public:
        template <typename... Args>
        explicit NoDestructor(Args&&... args) {
            std::construct_at(reinterpret_cast<T*>(m_storage), std::forward<Args>(args)...);
        }

        ~NoDestructor() = default;

        NoDestructor(const NoDestructor&) = delete;
        NoDestructor& operator=(const NoDestructor&) = delete;
        NoDestructor(NoDestructor&&) = delete;
        NoDestructor& operator=(NoDestructor&&) = delete;

        [[nodiscard]] T* get() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
        [[nodiscard]] const T* get() const noexcept { return std::launder(reinterpret_cast<const T*>(m_storage)); }
        [[nodiscard]] T& operator*() noexcept { return *get(); }
        [[nodiscard]] const T& operator*() const noexcept { return *get(); }
        [[nodiscard]] T* operator->() noexcept { return get(); }
        [[nodiscard]] const T* operator->() const noexcept { return get(); }
    };

    // Apps to drop from or restore to the library UI
    std::mutex g_removalMutex;
    NoDestructor<std::vector<AppId_t>> g_pendingRemovals;
    NoDestructor<std::vector<AppId_t>> g_pendingAdditions;
    NoDestructor<std::unordered_set<AppId_t>> g_removedAppIds;
    std::atomic<bool> g_hasRemovedAppIds{false};

    constexpr uint32_t k_EAppStateUpdatingMask =
        k_EAppStateDownloading         | // 0x00800000
        k_EAppStateStaging             | // 0x01000000
        k_EAppStateCommitting          | // 0x02000000
        k_EAppStateVerifyingStaged     | // 0x04000000
        k_EAppStateVerifyingInstalled  | // 0x00200000
        k_EAppStatePreallocating       | // 0x00400000
        k_EAppStateReconfiguring       | // 0x00100000
        k_EAppStateStopping            | // 0x08000000
        k_EAppStateUpdateRunning       | // 0x00000100
        k_EAppStateUpdatePaused        | // 0x00000200
        k_EAppStateUpdateStarted       | // 0x00000400
        k_EAppStateUpdateQueued         | // 0x00000008
        k_EAppStateUpdateRequired;       // 0x00000002

    constexpr uint32_t k_EAppStateActiveTransferMask =
        k_EAppStateDownloading         | // 0x00800000
        k_EAppStateStaging             | // 0x01000000
        k_EAppStateCommitting          | // 0x02000000
        k_EAppStateVerifyingStaged     | // 0x04000000
        k_EAppStateVerifyingInstalled  | // 0x00200000
        k_EAppStatePreallocating       | // 0x00400000
        k_EAppStateUpdateRunning;        // 0x00000100

    struct AppStateEntry {
        uint32_t stateFlags = 0;
        uint32_t changeNumber = 0;
    };

    // UI-thread confined state (RunFrame only; zero synchronization locks required)
    static NoDestructor<std::unordered_map<AppId_t, AppStateEntry>> g_trackedStates;
    static NoDestructor<std::vector<AppId_t>> g_activeUpdatingApps;

    // Cross-thread synchronization state (between UI thread RunFrame and AutoSyncWorkerPool)
    static NoDestructor<std::unordered_set<AppId_t>> g_inFlightSyncs;
    static std::mutex g_inFlightMutex;

    class AutoSyncWorkerPool {
    private:
        std::queue<AppId_t> m_taskQueue;
        std::mutex m_queueMutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_stopping{false};
        OSTPlatform::Thread::SafeThread m_workerThread;

        void WorkerLoop() {
            while (true) {
                AppId_t appId = 0;
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_cv.wait(lock, [this]() {
                        return m_stopping.load(std::memory_order_relaxed) || !m_taskQueue.empty();
                    });

                    if (m_stopping.load(std::memory_order_relaxed) && m_taskQueue.empty()) {
                        break;
                    }

                    appId = m_taskQueue.front();
                    m_taskQueue.pop();
                }

                // InFlight cleanup guard: instantiated immediately upon pop, RAII guarantees erasure on ALL exit paths
                struct InFlightGuard {
                    AppId_t id;
                    ~InFlightGuard() {
                        std::lock_guard<std::mutex> lock(g_inFlightMutex);
                        g_inFlightSyncs->erase(id);
                    }
                } guard{appId};

                // 500ms interruptible debounce to allow Steam to flush ACF and release file handles
                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);
                    m_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                        return m_stopping.load(std::memory_order_relaxed);
                    });
                    if (m_stopping.load(std::memory_order_relaxed)) {
                        break;
                    }
                }

                if (m_stopping.load(std::memory_order_relaxed)) {
                    break;
                }

                // 1. Account ownership check (thread-safe: read-lock on LuaConfig, atomic load on g_pCUser)
                if (!Hooks_Package::IsAppTrulyOwned(appId)) {
                    LOG_STEAMUI_DEBUG("AutoSync: appId={} is not genuinely owned by current account - skipping background sync", appId);
                    continue;
                }

                // 2. Lua configuration check (thread-safe: read-lock on LuaConfig)
                if (!LuaConfig::HasDepot(appId, false)) {
                    LOG_STEAMUI_DEBUG("AutoSync: appId={} is not configured with addappid - skipping background sync", appId);
                    continue;
                }

                LOG_STEAMUI_INFO("AutoSync: detected update completion for owned appId={}, triggering background sync", appId);
                try {
                    // Thread-safe: DenuvoSync serializes file writes via g_syncFileMutex & atomic line rewrites
                    PipeManager::DenuvoAuth::SyncOrGenerate(appId, "", false);
                } catch (const std::exception& ex) {
                    LOG_STEAMUI_ERROR("AutoSync: exception during SyncOrGenerate for appId={}: {}", appId, ex.what());
                } catch (...) {
                    LOG_STEAMUI_ERROR("AutoSync: unknown exception during SyncOrGenerate for appId={}", appId);
                }
            }
        }

    public:
        ~AutoSyncWorkerPool() {
            Detach();
        }

        void Start() {
            if (m_workerThread.joinable()) return;
            m_stopping.store(false, std::memory_order_relaxed);
            m_workerThread = std::thread(&AutoSyncWorkerPool::WorkerLoop, this);
        }

        void Stop() {
            m_stopping.store(true, std::memory_order_release);
            m_cv.notify_all();
            if (m_workerThread.joinable()) {
                m_workerThread.join();
            }
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                std::queue<AppId_t> empty;
                m_taskQueue.swap(empty);
            }
            {
                std::lock_guard<std::mutex> inFlightLock(g_inFlightMutex);
                g_inFlightSyncs->clear();
            }
        }

        void Detach() {
            m_stopping.store(true, std::memory_order_release);
            m_cv.notify_all();
            if (m_workerThread.joinable()) {
                m_workerThread.detach();
            }
        }

        bool Enqueue(AppId_t appId) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_stopping.load(std::memory_order_relaxed)) return false;
            m_taskQueue.push(appId);
            m_cv.notify_one();
            return true;
        }
    };
    static NoDestructor<AutoSyncWorkerPool> g_autoSyncWorkerPool;

    // Snapshot of installed + configured app IDs shared between scanner and UI thread
    static std::mutex g_installedMutex;
    static NoDestructor<std::shared_ptr<const std::vector<AppId_t>>> g_installedSnapshot;

    // Robust quote-aware comment stripper for VDF files (preserves // or # inside quotes)
    static std::string_view StripVdfComments(std::string_view sv) {
        bool inQuote = false;
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '\\' && inQuote && i + 1 < sv.size()) {
                ++i;
                continue;
            }
            if (sv[i] == '"') {
                inQuote = !inQuote;
                continue;
            }
            if (!inQuote) {
                if (sv[i] == '#' || (sv[i] == '/' && i + 1 < sv.size() && sv[i + 1] == '/')) {
                    sv = sv.substr(0, i);
                    break;
                }
            }
        }
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
            sv.remove_suffix(1);
        }
        return sv;
    }

    // Counts braces only when outside quoted string literals to prevent misinterpreting directory names
    static std::pair<int, int> CountUnquotedBraces(std::string_view sv) {
        int openBraces = 0;
        int closeBraces = 0;
        bool inQuote = false;
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '\\' && inQuote && i + 1 < sv.size()) {
                ++i;
                continue;
            }
            if (sv[i] == '"') {
                inQuote = !inQuote;
                continue;
            }
            if (!inQuote) {
                if (sv[i] == '{') ++openBraces;
                else if (sv[i] == '}') ++closeBraces;
            }
        }
        return {openBraces, closeBraces};
    }

    static void ScanInstalledAppIds() {
        try {
            std::string steamPath = SteamInstallPath;
            if (steamPath.empty()) return;

            std::filesystem::path libVdf =
                std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(steamPath)) / "steamapps" / "libraryfolders.vdf";
            std::ifstream file(libVdf);
            if (!file.is_open()) return;

            std::unordered_set<AppId_t> foundInstalled;
            std::string line;
            bool inAppsBlock = false;
            int braceDepth = 0;
            int appsDepth = -1;

            while (std::getline(file, line)) {
                std::string_view sv = line;
                if (sv.starts_with("\xEF\xBB\xBF")) {
                    sv.remove_prefix(3);
                }
                while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
                    sv.remove_prefix(1);
                }
                if (sv.empty()) continue;

                sv = StripVdfComments(sv);
                if (sv.empty()) continue;

                auto [openBraces, closeBraces] = CountUnquotedBraces(sv);
                braceDepth += openBraces;
                if (closeBraces > 0) {
                    braceDepth = std::max(0, braceDepth - closeBraces);
                    if (inAppsBlock && appsDepth >= 0 && braceDepth <= appsDepth) {
                        inAppsBlock = false;
                        appsDepth = -1;
                    }
                }

                if (!inAppsBlock) {
                    size_t firstQuote = sv.find('"');
                    if (firstQuote != std::string_view::npos) {
                        size_t secondQuote = sv.find('"', firstQuote + 1);
                        if (secondQuote != std::string_view::npos) {
                            std::string_view key = sv.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                            const bool isAppsKey = (key.size() == 4 &&
                                                    (key[0] == 'a' || key[0] == 'A') &&
                                                    (key[1] == 'p' || key[1] == 'P') &&
                                                    (key[2] == 'p' || key[2] == 'P') &&
                                                    (key[3] == 's' || key[3] == 'S'));
                            if (isAppsKey) {
                                size_t thirdQuote = sv.find('"', secondQuote + 1);
                                if (thirdQuote == std::string_view::npos || openBraces > 0) {
                                    inAppsBlock = true;
                                    appsDepth = (openBraces > 0) ? braceDepth - (openBraces - closeBraces) : braceDepth;
                                    if (closeBraces > 0 && braceDepth <= appsDepth) {
                                        inAppsBlock = false;
                                        appsDepth = -1;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    size_t firstQuote = sv.find('"');
                    if (firstQuote != std::string_view::npos) {
                        size_t secondQuote = sv.find('"', firstQuote + 1);
                        if (secondQuote != std::string_view::npos) {
                            std::string_view appIdStr = sv.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                            AppId_t appId = 0;
                            auto [p, ec] = std::from_chars(appIdStr.data(), appIdStr.data() + appIdStr.size(), appId);
                            if (ec == std::errc{} && p == appIdStr.data() + appIdStr.size() && appId != 0 && appId != k_uAppIdInvalid) {
                                foundInstalled.insert(appId);
                            }
                        }
                    }
                }
            }

            std::vector<AppId_t> filteredInstalled;
            if (!foundInstalled.empty()) {
                auto configuredSnapshot = LuaConfig::GetConfiguredAppIdsSnapshot();
                if (configuredSnapshot && !configuredSnapshot->empty()) {
                    filteredInstalled.reserve(std::min(foundInstalled.size(), size_t(1500)));
                    for (AppId_t appId : *configuredSnapshot) {
                        if (foundInstalled.contains(appId)) {
                            filteredInstalled.push_back(appId);
                        }
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_installedMutex);
                *g_installedSnapshot = std::make_shared<const std::vector<AppId_t>>(std::move(filteredInstalled));
            }
        } catch (const std::exception& ex) {
            LOG_STEAMUI_DEBUG("ScanInstalledAppIds: exception: {}", ex.what());
        } catch (...) {
        }
    }

    class InstalledScannerPool {
    private:
        OSTPlatform::Thread::SafeThread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::atomic<bool> m_stopping{false};
        std::atomic<bool> m_rescanRequested{false};

        void Loop() {
            while (!m_stopping.load(std::memory_order_relaxed)) {
                ScanInstalledAppIds();

                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::seconds(30), [this]() {
                    return m_stopping.load(std::memory_order_relaxed) ||
                           m_rescanRequested.exchange(false, std::memory_order_relaxed);
                });
            }
        }

    public:
        ~InstalledScannerPool() {
            Detach();
        }

        void Start() {
            if (m_thread.joinable()) return;
            m_stopping.store(false, std::memory_order_relaxed);
            m_thread = std::thread(&InstalledScannerPool::Loop, this);
        }

        void Stop() {
            m_stopping.store(true, std::memory_order_release);
            m_cv.notify_all();
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

        void Detach() {
            m_stopping.store(true, std::memory_order_release);
            m_cv.notify_all();
            if (m_thread.joinable()) {
                m_thread.detach();
            }
        }

        void TriggerRescan() {
            if (m_stopping.load(std::memory_order_relaxed)) return;
            m_rescanRequested.store(true, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_one();
        }
    };
    static NoDestructor<InstalledScannerPool> g_installedScannerPool;

    constexpr size_t k_SliceBatchSize = 64;
    static size_t s_timeSliceCursor = 0;

    static void Process1000GamesAutoSync(void* pController) noexcept {
        try {
            if (!Config::GetManifestAutoSyncOnUpdate() || !CAPTURE_READY(GetAppByID)) return;
            void* pCtrl = pController ? pController : g_pController;
            if (!pCtrl) return;

            // Zero locks held here! g_trackedStates and g_activeUpdatingApps are confined to the SteamUI render thread.
            // oGetAppByID is called with ZERO mutexes held, completely eliminating lock contention with Steam internals.

            auto safeEnqueue = [](AppId_t id) {
                bool inserted = false;
                {
                    std::lock_guard<std::mutex> lock(g_inFlightMutex);
                    inserted = g_inFlightSyncs->insert(id).second;
                }
                if (inserted) {
                    try {
                        if (!g_autoSyncWorkerPool->Enqueue(id)) {
                            std::lock_guard<std::mutex> lock(g_inFlightMutex);
                            g_inFlightSyncs->erase(id);
                        }
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(g_inFlightMutex);
                        g_inFlightSyncs->erase(id);
                    }
                }
            };

            // ── 通道一：活跃传输集检查（通常仅 0~1 款下载中游戏）──
            for (auto it = g_activeUpdatingApps->begin(); it != g_activeUpdatingApps->end(); ) {
                AppId_t appId = *it;
                CSteamApp* pApp = oGetAppByID(pCtrl, appId, false);
                if (!pApp) {
                    it = g_activeUpdatingApps->erase(it);
                    continue;
                }

                const uint32_t currentState = static_cast<uint32_t>(pApp->AppStateFlags);
                const bool isNowFullyInstalled = ((currentState & k_EAppStateUpdatingMask) == 0) &&
                                                 ((currentState & k_EAppStateFullyInstalled) != 0);

                if (isNowFullyInstalled) {
                    (*g_trackedStates)[appId].stateFlags = currentState;
                    (*g_trackedStates)[appId].changeNumber = pApp->ChangeNumber;
                    safeEnqueue(appId);
                    it = g_activeUpdatingApps->erase(it);
                } else if ((currentState & k_EAppStateActiveTransferMask) == 0) {
                    (*g_trackedStates)[appId].stateFlags = currentState;
                    it = g_activeUpdatingApps->erase(it);
                } else {
                    (*g_trackedStates)[appId].stateFlags = currentState;
                    ++it;
                }
            }

            // ── 通道二：时间片平摊步进（单帧固定 64 个轻量级已安装游戏探针）──
            std::shared_ptr<const std::vector<AppId_t>> installedSnapshot;
            {
                std::lock_guard<std::mutex> lock(g_installedMutex);
                installedSnapshot = *g_installedSnapshot;
            }
            if (!installedSnapshot || installedSnapshot->empty()) {
                return;
            }

            const auto& installedList = *installedSnapshot;
            if (s_timeSliceCursor >= installedList.size()) {
                s_timeSliceCursor = 0;
            }

            const size_t batchEnd = std::min(s_timeSliceCursor + k_SliceBatchSize, installedList.size());
            for (size_t i = s_timeSliceCursor; i < batchEnd; ++i) {
                AppId_t appId = installedList[i];
                if (appId == 0 || appId == k_uAppIdInvalid) continue;

                CSteamApp* pApp = oGetAppByID(pCtrl, appId, false);
                if (!pApp) continue;
                // Skip DLC/child apps; state tracking and manifest syncing are anchored on the base game
                if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId) continue;

                const uint32_t currentState = static_cast<uint32_t>(pApp->AppStateFlags);
                const uint32_t currentChangeNum = pApp->ChangeNumber;

                auto entryIt = g_trackedStates->find(appId);
                if (entryIt == g_trackedStates->end()) {
                    (*g_trackedStates)[appId] = { currentState, currentChangeNum };
                    if ((currentState & k_EAppStateActiveTransferMask) != 0) {
                        if (std::ranges::find(*g_activeUpdatingApps, appId) == g_activeUpdatingApps->end()) {
                            g_activeUpdatingApps->push_back(appId);
                        }
                    }
                    continue;
                }

                const uint32_t lastState = entryIt->second.stateFlags;
                const uint32_t lastChangeNum = entryIt->second.changeNumber;

                const bool wasUpdating = (lastState & k_EAppStateUpdatingMask) != 0;
                const bool wasRunning = (lastState & k_EAppStateAppRunning) != 0;
                const bool isNowFullyInstalled = ((currentState & k_EAppStateUpdatingMask) == 0) &&
                                                 ((currentState & k_EAppStateFullyInstalled) != 0);

                // 判定 1：常规状态机跳变完成
                if (wasUpdating && isNowFullyInstalled) {
                    entryIt->second = { currentState, currentChangeNum };
                    std::erase(*g_activeUpdatingApps, appId);
                    safeEnqueue(appId);
                }
                // 判定 2：秒级小补丁/错失 Updating 的版本号跃迁自愈兜底
                else if (isNowFullyInstalled && currentChangeNum != lastChangeNum) {
                    if (lastChangeNum == 0) {
                        entryIt->second.changeNumber = currentChangeNum;
                    } else if (wasRunning || (currentState & k_EAppStateAppRunning) != 0) {
                        // 游戏处于运行中或刚刚退出运行，ChangeNumber 变更是由于启动、游玩时长或云存档同步，绝非游戏清单更新
                        entryIt->second = { currentState, currentChangeNum };
                    } else if (PipeManager::WasToolActiveRecently(appId, std::chrono::seconds(3))) {
                        // 变更由外部提取工具（如 extract_tickets）短暂连接触发，严格跳过写盘同步
                        LOG_STEAMUI_DEBUG("AutoSync: appId={} state changed after recent tool extraction — skipping background sync", appId);
                        entryIt->second = { currentState, currentChangeNum };
                    } else {
                        entryIt->second = { currentState, currentChangeNum };
                        std::erase(*g_activeUpdatingApps, appId);
                        safeEnqueue(appId);
                    }
                }
                // 判定 3：捕获到游戏开始活跃传输，晋升至通道一以享受每帧直达监控
                else if ((currentState & k_EAppStateActiveTransferMask) != 0) {
                    if (std::ranges::find(*g_activeUpdatingApps, appId) == g_activeUpdatingApps->end()) {
                        g_activeUpdatingApps->push_back(appId);
                    }
                    entryIt->second.stateFlags = currentState;
                } else if (currentState != lastState) {
                    entryIt->second.stateFlags = currentState;
                }
            }

            s_timeSliceCursor = (batchEnd >= installedList.size()) ? 0 : batchEnd;
        } catch (...) {
        }
    }

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp)
        {
            if (pApp->OwnershipFlags & k_EAppOwnershipFlags_LicenseLocked)
            {
                pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                    pApp->OwnershipFlags & ~k_EAppOwnershipFlags_LicenseLocked);
            }

            if (LuaConfig::HasDepot(pApp->nAppID, false))
            {
                constexpr uint32_t kActiveLicenseMask =
                    k_EAppOwnershipFlags_OwnsLicense |
                    k_EAppOwnershipFlags_FreeLicense |
                    k_EAppOwnershipFlags_SharedLicense;
                if ((pApp->OwnershipFlags & kActiveLicenseMask) == 0)
                {
                    pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                        pApp->OwnershipFlags | k_EAppOwnershipFlags_OwnsLicense | k_EAppOwnershipFlags_LicensePermanent);
                }
                uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
                if (t)
                {
                    pApp->PurchasedTime = t;
                    LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                      pApp->PurchasedTime, pApp->nAppID);
                }
            }
            else
            {
                if (Hooks_SteamUI::IsRemoved(pApp->nAppID) && !Hooks_Package::HasValidLicense(pApp->nAppID))
                {
                    pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                    pApp->PurchasedTime = 0;
                    pApp->MasterSubAppID = 0;
                }
            }
        }

        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        if (!g_hasRemovedAppIds.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds->empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : *g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds->size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (!g_HooksInstalled.load(std::memory_order_relaxed))
        {
            return oCSteamUIAppControllerRunFrame(pController);
        }

        if (pController && !g_pController)
        {
            g_pController = pController;
        }

        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> drainingRemovals;
            std::vector<AppId_t> drainingAdditions;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                if (!g_pendingRemovals->empty()) {
                    drainingRemovals.swap(*g_pendingRemovals);
                }
                if (!g_pendingAdditions->empty()) {
                    drainingAdditions.swap(*g_pendingAdditions);
                }
            }

            if (!drainingAdditions.empty())
            {
                std::unordered_set<AppId_t> parentsToNotify;

                for (AppId_t appId : drainingAdditions)
                {
                    if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                    {
                        if (pApp->OwnershipFlags & k_EAppOwnershipFlags_LicenseLocked)
                        {
                            pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                                pApp->OwnershipFlags & ~k_EAppOwnershipFlags_LicenseLocked);
                        }
                        constexpr uint32_t kActiveLicenseMask =
                            k_EAppOwnershipFlags_OwnsLicense |
                            k_EAppOwnershipFlags_FreeLicense |
                            k_EAppOwnershipFlags_SharedLicense;
                        if ((pApp->OwnershipFlags & kActiveLicenseMask) == 0)
                        {
                            pApp->OwnershipFlags = static_cast<EAppOwnershipFlags>(
                                pApp->OwnershipFlags | k_EAppOwnershipFlags_OwnsLicense | k_EAppOwnershipFlags_LicensePermanent);
                        }

                        uint32_t t = LuaConfig::GetPurchaseTime(appId);
                        if (t)
                        {
                            pApp->PurchasedTime = t;
                        }

                        const bool isDlc = ((pApp->eProtoAppType & 32) != 0) ||
                                           (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid);

                        if (isDlc)
                        {
                            if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId)
                            {
                                parentsToNotify.insert(pApp->ParentAppID);
                            }
                        }
                    }

                    LOG_STEAMUI_TRACE("RunFrame: restoring added appId {}", appId);
                    oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
                }

                LOG_STEAMUI_DEBUG("RunFrame: restored {} pending Lua additions to SteamUI", drainingAdditions.size());

                for (AppId_t parentId : parentsToNotify)
                {
                    LOG_STEAMUI_INFO("RunFrame: notifying parent appId {} of DLC addition", parentId);
                    oMarkAppChange(g_pAppChangeSource, parentId, EAppChangeFlags::AppInfoOrConfig);
                }
            }

            if (!drainingRemovals.empty())
            {
                std::unordered_set<AppId_t> parentsToNotify;

                for (AppId_t appId : drainingRemovals)
                {
                    if (Hooks_Package::HasValidLicense(appId) || LuaConfig::HasDepot(appId, false))
                    {
                        LOG_STEAMUI_DEBUG("RunFrame: appId {} is still licensed (owned/shared) or active in config, skipping removal", appId);
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds->erase(appId);
                        g_hasRemovedAppIds.store(!g_removedAppIds->empty(), std::memory_order_release);
                        continue;
                    }

                    if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                    {
                        pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                        pApp->PurchasedTime = 0;
                        pApp->MasterSubAppID = 0;

                        const bool isDlc = ((pApp->eProtoAppType & 32) != 0) ||
                                           (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid);

                        if (isDlc)
                        {
                            if (pApp->ParentAppID != 0 && pApp->ParentAppID != k_uAppIdInvalid && pApp->ParentAppID != appId)
                            {
                                parentsToNotify.insert(pApp->ParentAppID);
                            }
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds->insert(appId);
                        g_hasRemovedAppIds.store(true, std::memory_order_release);
                    }

                    LOG_STEAMUI_INFO("RunFrame: removing appId {}", appId);
                    oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
                }

                for (AppId_t parentId : parentsToNotify)
                {
                    LOG_STEAMUI_INFO("RunFrame: notifying parent appId {} of DLC change", parentId);
                    oMarkAppChange(g_pAppChangeSource, parentId, EAppChangeFlags::AppInfoOrConfig);
                }
            }

            if (!drainingAdditions.empty() || !drainingRemovals.empty())
            {
                g_installedScannerPool->TriggerRescan();
            }
        }

        Process1000GamesAutoSync(pController);

        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        HOOK_END();

        g_trackedStates->reserve(1500);
        g_activeUpdatingApps->reserve(16);
        g_autoSyncWorkerPool->Start();
        g_installedScannerPool->Start();
    }

    void Uninstall()
    {
        g_installedScannerPool->Stop();
        g_autoSyncWorkerPool->Stop();
        g_trackedStates->clear();
        g_activeUpdatingApps->clear();
        s_timeSliceCursor = 0;

        UNHOOK_BEGIN();
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();

        {
            std::lock_guard<std::mutex> lock(g_removalMutex);
            g_pendingRemovals->clear();
            g_pendingAdditions->clear();
            g_removedAppIds->clear();
            g_hasRemovedAppIds.store(false, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> lock(g_installedMutex);
            g_installedSnapshot->reset();
        }
    }

    void DetachWorkerThreads()
    {
        g_installedScannerPool->Detach();
        g_autoSyncWorkerPool->Detach();
    }


    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(*g_pendingAdditions, appId);
        g_removedAppIds->insert(appId);
        g_hasRemovedAppIds.store(true, std::memory_order_release);
        if (std::ranges::find(*g_pendingRemovals, appId) == g_pendingRemovals->end()) {
            g_pendingRemovals->push_back(appId);
        }
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (!g_pendingRemovals->empty()) {
            std::erase(*g_pendingRemovals, appId);
        }
        if (g_hasRemovedAppIds.load(std::memory_order_relaxed)) {
            g_removedAppIds->erase(appId);
            g_hasRemovedAppIds.store(!g_removedAppIds->empty(), std::memory_order_release);
        }
    }

    void QueueAddition(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (!g_pendingRemovals->empty()) {
            std::erase(*g_pendingRemovals, appId);
        }
        if (g_hasRemovedAppIds.load(std::memory_order_relaxed)) {
            g_removedAppIds->erase(appId);
            g_hasRemovedAppIds.store(!g_removedAppIds->empty(), std::memory_order_release);
        }
        if (std::ranges::find(*g_pendingAdditions, appId) == g_pendingAdditions->end()) {
            g_pendingAdditions->push_back(appId);
        }
    }

    void QueueAdditions(std::span<const AppId_t> appIds)
    {
        if (appIds.empty()) return;
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingAdditions->insert(g_pendingAdditions->end(), appIds.begin(), appIds.end());
        std::ranges::sort(*g_pendingAdditions);
        const auto dup = std::ranges::unique(*g_pendingAdditions);
        g_pendingAdditions->erase(dup.begin(), dup.end());
        if (!g_pendingRemovals->empty() || !g_removedAppIds->empty()) {
            for (AppId_t id : appIds) {
                if (!g_pendingRemovals->empty()) {
                    std::erase(*g_pendingRemovals, id);
                }
                g_removedAppIds->erase(id);
            }
            g_hasRemovedAppIds.store(!g_removedAppIds->empty(), std::memory_order_release);
        }
    }

    bool IsRemoved(AppId_t appId)
    {
        if (!g_hasRemovedAppIds.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_removalMutex);
        return g_removedAppIds->contains(appId);
    }

    void TriggerInstalledScanner()
    {
        g_installedScannerPool->TriggerRescan();
    }
}
