#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>
#include <windows.h>

namespace {
    // SEH crash fuse: if CheckAppOwnership ever crashes, injection is permanently
    // disabled for this session. Steam stays alive, unlock just stops working.
    std::atomic<bool> g_injectDisabled{false};
    static DWORD g_injectExCode = 0;
    static void* g_injectExAddr = nullptr;

    static LONG WINAPI InjectionFilter(EXCEPTION_POINTERS* ep) {
        g_injectExCode = ep->ExceptionRecord->ExceptionCode;
        g_injectExAddr = ep->ExceptionRecord->ExceptionAddress;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // ── Trivially destructible wrapper for process-lifetime globals ──
    // Prevents CRT atexit static destruction crashes during ExitProcess
    // when secondary threads are terminated mid-operation by the OS.
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

    RESOLVE_FUNC(CUtlMemoryGrow,               void,  CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*, g_pCPackageInfo, void* pThis, uint32 packageId, uint64 accessToken);
    
    std::mutex                                  g_packageInfoMutex;
    std::atomic<void*>                          g_pCUser{nullptr};
    std::atomic<PackageInfo*>                   g_pInjectedPackageInfo{nullptr};
    std::atomic<bool>                           g_licenseInitialized{false};
    std::atomic<bool>                           g_licenseRefreshPending{false};
    std::atomic<bool>                           g_forceRequeryOnce{false};
    std::shared_mutex                           g_sharedLicensesMutex;
    NoDestructor<std::unordered_set<AppId_t>>   g_sharedLicenses;
    std::atomic<bool>                           g_hasSharedLicenses{false};
    std::atomic<Hooks_Package::LicenseChangeObserver> g_licenseChangeObserver{nullptr};

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    bool MarkLicenseAsChangedAndProcessUpdates() {
        void* pUser = g_pCUser.load(std::memory_order_acquire);
        const auto fnMark = oMarkLicenseAsChanged;
        const auto fnProcess = oProcessPendingLicenseUpdates;
        if (!pUser || !fnMark || !fnProcess) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        fnMark(pUser, kInjectedPackageId, true);
        fnProcess(pUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    bool TryProcessPendingLicenseRefresh() {
        if (!g_licenseRefreshPending.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        if (!MarkLicenseAsChangedAndProcessUpdates()) {
            g_licenseRefreshPending.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    template<typename T>
    bool CUtlMemoryGrowWrap(CUtlVector<T>* pVec, uint32 numToAdd) {
        if (!pVec || numToAdd == 0) {
            return false;
        }
        if (static_cast<int32>(pVec->m_Memory.m_nGrowSize) < 0) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: external buffer cannot grow (m_nGrowSize={})",
                             static_cast<int32>(pVec->m_Memory.m_nGrowSize));
            return false;
        }
        if (numToAdd > (std::numeric_limits<uint32>::max)() - pVec->m_Size) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: capacity overflow (size={}, add={})", pVec->m_Size, numToAdd);
            return false;
        }

        const uint32 requiredCapacity = pVec->m_Size + numToAdd;
        if (requiredCapacity > static_cast<uint32>((std::numeric_limits<int>::max)())) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: requiredCapacity {} exceeds int32 max", requiredCapacity);
            return false;
        }

        const uint32 currentAlloc = pVec->m_Memory.m_pMemory ? pVec->m_Memory.m_nAllocationCount : 0u;
        if (currentAlloc >= requiredCapacity) {
            return true;
        }

        const auto fnGrow = oCUtlMemoryGrow;
        if (!fnGrow) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: oCUtlMemoryGrow not ready, cannot grow");
            return false;
        }

        const int growDelta = static_cast<int>(requiredCapacity - currentAlloc);
        fnGrow(reinterpret_cast<CUtlVector<AppId_t>*>(pVec), growDelta);
        return pVec->m_Memory.m_pMemory != nullptr &&
               pVec->m_Memory.m_nAllocationCount >= requiredCapacity;
    }

    [[nodiscard]] bool AppendUniqueAppIdsToPackage(PackageInfo* pPkg,
                                                   std::span<const AppId_t> candidateIds,
                                                   uint32_t* outAddedCount = nullptr) {
        if (outAddedCount) {
            *outAddedCount = 0;
        }
        if (!pPkg || candidateIds.empty()) {
            return true;
        }

        const AppId_t* existingData = pPkg->AppIdVec.m_Memory.m_pMemory;
        const uint32 existingSize = existingData ? pPkg->AppIdVec.m_Size : 0u;
        const std::span<const AppId_t> existingSpan(existingData, existingSize);

        std::vector<AppId_t> toAdd;
        toAdd.reserve(candidateIds.size());
        for (AppId_t id : candidateIds) {
            if (id == 0 || id == k_uAppIdInvalid) {
                continue;
            }
            // Officially owned and family shared apps have commercial package backing, do not inject into fake Package 0
            if (LuaConfig::IsOwned(id) || Hooks_Package::IsSharedLicenseCached(id)) {
                continue;
            }
            if (std::ranges::find(existingSpan, id) == existingSpan.end() &&
                std::ranges::find(toAdd, id) == toAdd.end()) {
                toAdd.push_back(id);
            }
        }

        if (toAdd.empty()) {
            return true;
        }

        const uint32 oldSize = existingSize;
        const uint32 numToAdd = static_cast<uint32>(toAdd.size());
        if (!CUtlMemoryGrowWrap(&pPkg->AppIdVec, numToAdd)) {
            return false;
        }

        for (uint32 i = 0; i < numToAdd; ++i) {
            pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = toAdd[i];
        }
        pPkg->AppIdVec.m_Size = oldSize + numToAdd;
        if (outAddedCount) {
            *outAddedCount = numToAdd;
        }
        return true;
    }


    [[nodiscard]] bool AppendUniqueDepotIdsToPackage(PackageInfo* pPkg,
                                                     std::span<const DepotId_t> candidateIds,
                                                     uint32_t* outAddedCount = nullptr) {
        if (outAddedCount) {
            *outAddedCount = 0;
        }
        if (!pPkg || candidateIds.empty()) {
            return true;
        }

        const DepotId_t* existingData = pPkg->DepotIdVec.m_Memory.m_pMemory;
        const uint32 existingSize = existingData ? pPkg->DepotIdVec.m_Size : 0u;
        const std::span<const DepotId_t> existingSpan(existingData, existingSize);

        std::vector<DepotId_t> toAdd;
        toAdd.reserve(candidateIds.size());
        for (DepotId_t id : candidateIds) {
            if (id == 0 || id == k_uAppIdInvalid) {
                continue;
            }
            if (LuaConfig::IsOwned(id)) {
                continue;
            }
            if (std::ranges::find(existingSpan, id) == existingSpan.end() &&
                std::ranges::find(toAdd, id) == toAdd.end()) {
                toAdd.push_back(id);
            }
        }

        if (toAdd.empty()) {
            return true;
        }

        const uint32 oldSize = existingSize;
        const uint32 numToAdd = static_cast<uint32>(toAdd.size());
        if (!CUtlMemoryGrowWrap(&pPkg->DepotIdVec, numToAdd)) {
            return false;
        }

        for (uint32 i = 0; i < numToAdd; ++i) {
            pPkg->DepotIdVec.m_Memory.m_pMemory[oldSize + i] = toAdd[i];
        }
        pPkg->DepotIdVec.m_Size = oldSize + numToAdd;
        if (outAddedCount) {
            *outAddedCount = numToAdd;
        }
        return true;
    }

    bool InitFakeLicenseOnce(PackageInfo* pPkg) {
        if (!pPkg) {
            return false;
        }

        {
            std::lock_guard lock(g_packageInfoMutex);
            if (g_licenseInitialized.load(std::memory_order_relaxed)) {
                return true;
            }

            // check package status before injecting
            if (pPkg->Status != EPackageStatus::Available) {
                LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
                return false;
            }

            // Inject all configured apps and depots from config into the fake license.
            const std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
            if (!appIds.empty()) {
                const uint32 oldAppSize = pPkg->AppIdVec.m_Memory.m_pMemory ? pPkg->AppIdVec.m_Size : 0u;
                uint32 addedAppCount = 0;
                if (!AppendUniqueAppIdsToPackage(pPkg, appIds, &addedAppCount)) {
                    LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow AppId vector", kInjectedPackageId);
                    return false;
                }
                if (addedAppCount > 0) {
                    LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): added {} apps, oldAppSize={}", kInjectedPackageId, addedAppCount, oldAppSize);
                }

                const uint32 oldDepotSize = pPkg->DepotIdVec.m_Memory.m_pMemory ? pPkg->DepotIdVec.m_Size : 0u;
                uint32 addedDepotCount = 0;
                if (!AppendUniqueDepotIdsToPackage(pPkg, appIds, &addedDepotCount)) {
                    LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow DepotId vector", kInjectedPackageId);
                    return false;
                }
                if (addedDepotCount > 0) {
                    LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): added {} depots, oldDepotSize={}", kInjectedPackageId, addedDepotCount, oldDepotSize);
                }
            }

            g_pInjectedPackageInfo.store(pPkg, std::memory_order_release);
            g_licenseInitialized.store(true, std::memory_order_release);
            g_licenseRefreshPending.store(true, std::memory_order_release);
        }

        TryProcessPendingLicenseRefresh();
        return true;
    }

    bool TryInitFakeLicenseOnce() {
        if (g_licenseInitialized.load(std::memory_order_acquire)) {
            return true;
        }
        void* pPackageMgr = g_pCPackageInfo;
        const auto fnGetPackageInfo = oGetPackageInfo;
        if (pPackageMgr && fnGetPackageInfo) {
            PackageInfo* pPkg = fnGetPackageInfo(pPackageMgr, kInjectedPackageId, kInjectedPkgAccessToken);
            if (!pPkg) {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
                return false;
            }
            return InitFakeLicenseOnce(pPkg);
        }
        return false;
    }

    struct LicenseClassification {
        bool isTrulyOwned = false;
        bool isShared = false;
    };

    inline LicenseClassification EvaluateOwnership(bool result, const AppOwnership& own) {
        const bool hasRealPackageId = (own.PackageId != 0) &&
                                      (own.PackageId != kInjectedPackageId) &&
                                      (own.PackageId != k_uPackageIdInvalid);

        const bool isOfficialShared = (own.bFamilyShared || own.bBorrowed) &&
                                      !own.bLicenseExpired;

        const bool isTrulyOwned = result &&
                                  hasRealPackageId &&
                                  (own.ExistInPackageNums >= 1) &&
                                  own.bOwnsLicense &&
                                  !own.bLicenseExpired &&
                                  !isOfficialShared;
        if (isTrulyOwned) {
            return { .isTrulyOwned = true, .isShared = false };
        }

        if (isOfficialShared) {
            return { .isTrulyOwned = false, .isShared = true };
        }

        return {};
    }

    inline void RecordLicenseClassification(AppId_t appId, const LicenseClassification& lc) {
        if (appId == 0 || appId == k_uAppIdInvalid) {
            return;
        }

        // Principle 1: Truly owned apps have the absolute highest priority!
        // If current account truly owns the app, never strip owned status even if in family shared pools.
        if (lc.isTrulyOwned) {
            LuaConfig::MarkOwned(appId);
            if (g_hasSharedLicenses.load(std::memory_order_acquire)) {
                std::unique_lock wlock(g_sharedLicensesMutex);
                if (g_sharedLicenses->erase(appId) > 0) {
                    g_hasSharedLicenses.store(!g_sharedLicenses->empty(), std::memory_order_release);
                }
            }
            return;
        }

        // Once confirmed as truly owned, never downgrade due to subsequent shared license enumeration.
        if (LuaConfig::IsOwned(appId)) {
            return;
        }

        if (lc.isShared) {
            bool inserted = false;
            {
                if (g_hasSharedLicenses.load(std::memory_order_acquire)) {
                    std::shared_lock rlock(g_sharedLicensesMutex);
                    if (g_sharedLicenses->contains(appId)) {
                        return;
                    }
                }
                std::unique_lock wlock(g_sharedLicensesMutex);
                inserted = g_sharedLicenses->insert(appId).second;
                if (inserted) {
                    g_hasSharedLicenses.store(true, std::memory_order_release);
                }
            }

            // Dual-guarantee self-healing: if this shared app was tentatively injected into Package 0, remove it now!
            // Notice: executed outside of g_sharedLicensesMutex to prevent AB-BA lock inversion with g_packageInfoMutex.
            if (inserted) {
                PackageInfo* pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
                if (pPkg) {
                    std::lock_guard lock(g_packageInfoMutex);
                    if (pPkg->AppIdVec.m_Memory.m_pMemory) {
                        pPkg->AppIdVec.FindAndFastRemove(appId);
                    }
                    if (pPkg->DepotIdVec.m_Memory.m_pMemory) {
                        pPkg->DepotIdVec.FindAndFastRemove(appId);
                    }
                }
            }
        } else {
            if (LuaConfig::IsOwned(appId)) {
                LuaConfig::UnmarkOwned(appId);
            }
        }
    }

    // Core ownership injection logic, called under SEH protection.
    // Only POD locals — no C++ objects — so __try is legal (/EHsc).
    static bool CheckAppOwnershipBody(void* /*pObj*/, AppId_t appId, AppOwnership* pOwn, bool origResult) {
        AppOwnership localOwn{};
        AppOwnership* pEffectiveOwn = pOwn ? pOwn : &localOwn;

        bool result = origResult;
        TryInitFakeLicenseOnce();

        if (!g_licenseInitialized.load(std::memory_order_acquire) && g_forceRequeryOnce.exchange(false, std::memory_order_acq_rel)) {
            LOG_PACKAGE_INFO("CheckAppOwnership: priming license re-query after late hook install");
            MarkLicenseAsChangedAndProcessUpdates();
        }

        TryProcessPendingLicenseRefresh();

        const auto lc = EvaluateOwnership(result, *pEffectiveOwn);
        RecordLicenseClassification(appId, lc);
        const bool hasValidLicense = lc.isTrulyOwned || lc.isShared;

        if (lc.isShared) {
            if (pEffectiveOwn->bLicenseLocked) {
                LOG_PACKAGE_DEBUG("CheckAppOwnership: Clearing bLicenseLocked for shared AppId={}", appId);
                pEffectiveOwn->bLicenseLocked = false;
            }
            pEffectiveOwn->bOwnsLicense = true;
            result = true;
        }

        if (LuaConfig::HasDepot(appId, false)) {
            if (!pOwn) {
                return true;
            }
            if (hasValidLicense) {
                pOwn->ReleaseState = EAppReleaseState::Released;
                return result;
            }
            pOwn->PackageId         = kInjectedPackageId;
            pOwn->ReleaseState      = EAppReleaseState::Released;
            pOwn->bOwnsLicense      = true;
            pOwn->bFreeLicense      = false;
            pOwn->bFamilyShared     = false;
            pOwn->bBorrowed         = false;
            pOwn->bLicenseLocked    = false;
            pOwn->bIsPermanent      = true;
            pOwn->bLicensePermanent = true;
            return true;
        }

        if (!result && pOwn) {
            pOwn->bOwnsLicense = false;
        }

        return result;
    }

    // SEH wrapper in a POD-only function — no C++ objects, so __try is legal under /EHsc.
    static bool CheckAppOwnershipSEH(void* pObj, AppId_t appId, AppOwnership* pOwn, bool origResult, bool* crashed) {
        *crashed = false;
        bool out = origResult;
        __try {
            out = CheckAppOwnershipBody(pObj, appId, pOwn, origResult);
        } __except (InjectionFilter(GetExceptionInformation())) {
            g_injectDisabled.store(true, std::memory_order_release);
            *crashed = true;
            return origResult;
        }
        return out;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        const auto origCheckAppOwnership = oCheckAppOwnership;
        if (!origCheckAppOwnership) {
            return false;
        }

        void* currentCUser = g_pCUser.load(std::memory_order_acquire);
        if (pObj && currentCUser != pObj) {
            void* prevCUser = g_pCUser.exchange(pObj, std::memory_order_acq_rel);
            if (prevCUser != pObj) {
                LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", pObj);
                if (prevCUser != nullptr) {
                    Hooks_Misc::SetActiveAccountID(0);
                    LuaConfig::ClearOwned();
                    std::unique_lock wlock(g_sharedLicensesMutex);
                    g_sharedLicenses->clear();
                    g_hasSharedLicenses.store(false, std::memory_order_release);
                }
            }
        }

        bool result = origCheckAppOwnership(pObj, appId, pOwn);

        // Fused out — return original result, never touch memory again.
        if (g_injectDisabled.load(std::memory_order_relaxed)) return result;

        bool crashed = false;
        bool out = CheckAppOwnershipSEH(pObj, appId, pOwn, result, &crashed);
        if (crashed) {
            LOG_PACKAGE_ERROR("CheckAppOwnership: crashed (code=0x{:X} addr={}), injection disabled for session",
                              g_injectExCode, g_injectExAddr);
        }
        return out;
    }

    static LicenseClassification ClassifyAppLicense(AppId_t appId) {
        if (appId == 0 || appId == k_uAppIdInvalid) return {};

        if (LuaConfig::IsOwned(appId)) {
            return { .isTrulyOwned = true, .isShared = false };
        }

        if (g_hasSharedLicenses.load(std::memory_order_acquire)) {
            std::shared_lock lock(g_sharedLicensesMutex);
            if (g_sharedLicenses->contains(appId)) {
                return { .isTrulyOwned = false, .isShared = true };
            }
        }

        void* pUser = g_pCUser.load(std::memory_order_acquire);
        const auto origCheckAppOwnership = oCheckAppOwnership;
        if (!pUser || !origCheckAppOwnership) return {};

        AppOwnership own{};
        bool result = origCheckAppOwnership(pUser, appId, &own);
        const auto lc = EvaluateOwnership(result, own);
        RecordLicenseClassification(appId, lc);
        return lc;
    }
}

namespace Hooks_Package {
    void Install() {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNHOOK_END();
    }



    void NotifyLicenseChanged() {
        if (g_injectDisabled.load(std::memory_order_relaxed)) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injection disabled after crash, skipping");
            return;
        }

        PackageInfo* pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
        if (!pPkg) {
            TryInitFakeLicenseOnce();
            pPkg = g_pInjectedPackageInfo.load(std::memory_order_acquire);
        }
        if (!pPkg) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        // ── Compute net additions & removals against current LuaConfig state ──
        std::vector<AppId_t> removals = LuaConfig::TakePendingRemovals();
        std::vector<AppId_t> additions = LuaConfig::TakePendingAdditions();
        uint32_t removedCount = 0;
        std::unordered_set<AppId_t> addedIds;
        std::vector<AppId_t> uniqueAdditions;

        if (!additions.empty()) {
            addedIds.reserve(additions.size());
            uniqueAdditions.reserve(additions.size());
            for (AppId_t id : additions) {
                if (id != 0 && id != k_uAppIdInvalid &&
                    !addedIds.contains(id) &&
                    LuaConfig::HasDepot(id, false)) {
                    addedIds.insert(id);
                    uniqueAdditions.push_back(id);
                }
            }
        }

        std::vector<AppId_t> netRemovals;
        if (!removals.empty()) {
            std::unordered_set<AppId_t> seenRemovals;
            seenRemovals.reserve(removals.size());
            netRemovals.reserve(removals.size());
            for (AppId_t id : removals) {
                if (id != 0 && id != k_uAppIdInvalid &&
                    !addedIds.contains(id) &&
                    seenRemovals.insert(id).second &&
                    !LuaConfig::HasDepot(id, false)) {
                    netRemovals.push_back(id);
                }
            }
        }

        {
            std::lock_guard lock(g_packageInfoMutex);
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} net removals (from {} raw)", netRemovals.size(), removals.size());
            if (pPkg->AppIdVec.m_Memory.m_pMemory) {
                for (AppId_t id : netRemovals) {
                    while (pPkg->AppIdVec.FindAndFastRemove(id)) {
                        ++removedCount;
                        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {}", id);
                    }
                }
            }
            if (pPkg->DepotIdVec.m_Memory.m_pMemory) {
                for (AppId_t id : netRemovals) {
                    while (pPkg->DepotIdVec.FindAndFastRemove(id)) {
                        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed DepotId {}", id);
                    }
                }
            }

            // ── Add depots that are newly loaded ──
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} additions", uniqueAdditions.size());
            if (!uniqueAdditions.empty()) {
                uint32 addedToPkgCount = 0;
                if (!AppendUniqueAppIdsToPackage(pPkg, uniqueAdditions, &addedToPkgCount)) {
                    LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow AppId vector for additions");
                }
                uint32 addedDepotCount = 0;
                if (!AppendUniqueDepotIdsToPackage(pPkg, uniqueAdditions, &addedDepotCount)) {
                    LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow DepotId vector for additions");
                }
            }
        }

        if (uniqueAdditions.empty() && netRemovals.empty()) {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
            return;
        }

        // Mark package 0 as changed and trigger library refresh.
        g_licenseRefreshPending.store(true, std::memory_order_release);
        if (!TryProcessPendingLicenseRefresh()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed, refresh pending");
        } else {
            LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", uniqueAdditions.size(), removedCount);
        }

        // Notify subscribers (e.g. SteamUI) via observer callback rather than direct coupling.
        // Never touch MarkAppChange from this (FileWatcher) thread.
        if (auto observer = g_licenseChangeObserver.load(std::memory_order_acquire)) {
            observer(uniqueAdditions, netRemovals);
        }
    }

    bool IsAppTrulyOwned(AppId_t appId) {
        return ClassifyAppLicense(appId).isTrulyOwned;
    }

    bool IsSharedLicense(AppId_t appId) {
        return ClassifyAppLicense(appId).isShared;
    }

    bool HasValidLicense(AppId_t appId) {
        const auto license = ClassifyAppLicense(appId);
        return license.isTrulyOwned || license.isShared;
    }

    bool IsSharedLicenseCached(AppId_t appId) noexcept {
        if (appId == 0 || appId == k_uAppIdInvalid) {
            return false;
        }
        if (g_hasSharedLicenses.load(std::memory_order_acquire)) {
            std::shared_lock lock(g_sharedLicensesMutex);
            return g_sharedLicenses->contains(appId);
        }
        return false;
    }

    void RecordFamilySharedApp(AppId_t appId) {
        if (appId == 0 || appId == k_uAppIdInvalid) return;
        RecordLicenseClassification(appId, { .isTrulyOwned = false, .isShared = true });
    }

    void SetLicenseChangeObserver(LicenseChangeObserver observer) noexcept {
        g_licenseChangeObserver.store(observer, std::memory_order_release);
    }

    void RequestRequeryOnce() {
        g_forceRequeryOnce.store(true, std::memory_order_release);
    }
}
