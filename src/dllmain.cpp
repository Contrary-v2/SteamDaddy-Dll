#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Hook/Hooks_Decryption.h"
#include "Hook/Hooks_Diversion.h"
#include "Hook/Hooks_Misc.h"
#include "Hook/Hooks_Package.h"
#include "Hook/Hooks_SteamUI.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/ManifestClient.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/StatsClient.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/Thread.h"

#include <cctype>
#include <cstdio>
#include <windows.h>

template <typename... Args>
static inline bool FormatPath(char (&dst)[kRuntimePathCapacity], const char* fmt, Args... args) noexcept {
    const int n = std::snprintf(dst, kRuntimePathCapacity, fmt, args...);
    if (n <= 0 || static_cast<size_t>(n) >= kRuntimePathCapacity) {
        dst[0] = '\0';
        return false;
    }
    return true;
}

// Prepare key runtime paths.
// Portable: Steam components (steamclient64.dll, steamui.dll, etc.) are located
// in Steam's real installation directory, while configuration and Lua scripts can be
// loaded from the portable DLL directory or fallback to Steam's installation directory.
static bool InitializeRuntimePaths(OSTPlatform::DynamicLibrary::ModuleHandle selfModule)
{
    using OSTPlatform::Encoding::PathFromUtf8;
    using OSTPlatform::Encoding::PathToUtf8;

    // 1. Locate Steam's actual install directory (where steam.exe and steamclient64.dll reside).
    // Injected into steam.exe: GetModuleDirectory(nullptr) returns the directory of steam.exe.
    auto steamExeDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(nullptr);
    std::string steamPath = PathToUtf8(steamExeDir);
    if (steamPath.empty()) {
        steamPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    }
    while (steamPath.size() > 1 && (steamPath.back() == '\\' || steamPath.back() == '/') &&
           !(steamPath.size() == 3 && steamPath[1] == ':')) {
        steamPath.pop_back();
    }
    if (steamPath.empty()) {
        return false;
    }
    if (!FormatPath(SteamInstallPath, "%s", steamPath.c_str()) ||
        !FormatPath(SteamclientPath,  "%s\\steamclient64.dll", SteamInstallPath) ||
        !FormatPath(SteamUIPath,      "%s\\steamui.dll",       SteamInstallPath)) {
        LOG_ERROR("Steam install path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // 2. Locate SteamDaddy DLL directory (portable mode support).
    auto dllDir = OSTPlatform::DynamicLibrary::GetModuleDirectory(selfModule);
    std::string dllPath = PathToUtf8(dllDir);
    if (dllPath.empty()) {
        dllPath = steamPath;
    }
    while (dllPath.size() > 1 && (dllPath.back() == '\\' || dllPath.back() == '/') &&
           !(dllPath.size() == 3 && dllPath[1] == ':')) {
        dllPath.pop_back();
    }
    if (!FormatPath(DllDir, "%s", dllPath.c_str())) {
        LOG_ERROR("DLL directory path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // Evaluate portable mode once after SteamInstallPath and DllDir are set.
    auto dllFsPath   = PathFromUtf8(DllDir).lexically_normal();
    auto steamFsPath = PathFromUtf8(SteamInstallPath).lexically_normal();
    if (!dllFsPath.has_filename() && dllFsPath != dllFsPath.root_path()) {
        dllFsPath = dllFsPath.parent_path();
    }
    if (!steamFsPath.has_filename() && steamFsPath != steamFsPath.root_path()) {
        steamFsPath = steamFsPath.parent_path();
    }
    std::error_code ecEquiv, ecBin;
    const bool inSteamDir = std::filesystem::equivalent(dllFsPath, steamFsPath, ecEquiv) ||
                           std::filesystem::equivalent(dllFsPath, steamFsPath / "bin", ecBin) ||
                           (_wcsicmp(dllFsPath.c_str(), steamFsPath.c_str()) == 0) ||
                           (_wcsicmp(dllFsPath.c_str(), (steamFsPath / "bin").c_str()) == 0);
    g_IsPortableMode.store(!inSteamDir, std::memory_order_release);

    if (!FormatPath(DiversionPath, "%s\\bin\\diversion64.dll", SteamInstallPath)) {
        LOG_ERROR("Diversion path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    // 3. Resolve config and lua directory:
    // Check DllDir first (portable folder), fallback to SteamInstallPath.
    // Checks for steamdaddy.toml first, falling back to opensteamtool.toml for backwards compatibility.
    auto resolveToml = [](const std::filesystem::path& dir) -> std::filesystem::path {
        std::error_code ec;
        auto p1 = dir / "steamdaddy.toml";
        if (std::filesystem::is_regular_file(p1, ec) && !ec) return p1;
        auto p2 = dir / "opensteamtool.toml";
        if (std::filesystem::is_regular_file(p2, ec) && !ec) return p2;
        return p1;
    };
    std::filesystem::path tomlFs = resolveToml(dllFsPath);
    std::string tomlPath = PathToUtf8(tomlFs);
    std::error_code ecToml;
    if (IsPortableMode() && (!std::filesystem::is_regular_file(tomlFs, ecToml) || ecToml)) {
        std::filesystem::path steamTomlFs = resolveToml(steamFsPath);
        std::error_code ecSteamToml;
        if (std::filesystem::is_regular_file(steamTomlFs, ecSteamToml) && !ecSteamToml) {
            tomlPath = PathToUtf8(steamTomlFs);
        }
    }

    const auto storageBase = PathFromUtf8(GetStorageDirectory());
    const auto luaFs = storageBase / "config" / "stplug-in";
    std::error_code ec;
    std::filesystem::create_directories(luaFs, ec);
    if (ec) {
        LOG_WARN("Failed to create default Lua directory (err={})", ec.value());
    }
    const std::string luaPath = PathToUtf8(luaFs);
    if (!FormatPath(ConfigPath, "%s", tomlPath.c_str()) ||
        !FormatPath(LuaDir,     "%s", luaPath.c_str())) {
        LOG_ERROR("Config or Lua directory path exceeds runtime buffer capacity ({})", kRuntimePathCapacity);
        return false;
    }

    return true;
}

static bool InitializeSteamComponents(OSTPlatform::DynamicLibrary::ModuleHandle /*selfModule*/)
{
    using OSTPlatform::Encoding::PathFromUtf8;

    // Load or adopt client module (diversion or fallback to real steamclient64.dll)
    if (!client_hModule.load(std::memory_order_acquire)) {
        if (g_IsDiversionActive.load(std::memory_order_acquire)) {
            auto h = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(DiversionPath));
            if (h) {
                auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
                client_hModule.compare_exchange_strong(expected, h);
                LOG_INFO("Loaded diversion module from {}", DiversionPath);
            }
        }
        if (!client_hModule.load(std::memory_order_acquire)) {
            auto h = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(SteamclientPath));
            if (!h) {
                LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                          SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
                return false;
            }
            auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
            client_hModule.compare_exchange_strong(expected, h);
            LOG_INFO("Loaded fallback steamclient64.dll from {} (Diversion inactive)", SteamclientPath);
            Hooks_Package::RequestRequeryOnce();
        }
    }

    if (!ui_hModule.load(std::memory_order_acquire)) {
        auto h = OSTPlatform::DynamicLibrary::Load(PathFromUtf8(SteamUIPath));
        if (!h) {
            LOG_ERROR("Load failed for steamui.dll: {} (err={})",
                      SteamUIPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            auto client = client_hModule.exchange(nullptr);
            if (client) {
                OSTPlatform::DynamicLibrary::Unload(client);
            }
            return false;
        }
        auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
        ui_hModule.compare_exchange_strong(expected, h);
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) noexcept {
    try {
        Log::Init(selfModule);
        LOG_INFO("SteamDaddy init thread started");

        if (!InitializeSteamComponents(selfModule)) {
            LOG_ERROR("InitializeSteamComponents failed");
            return 1;
        }

        Config::Load(ConfigPath);
        Log::InitModules();
        Log::InstallPlatformLogSink();
        SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

        std::vector<std::string> watchDirs;
        std::unordered_set<std::string> seenDirs;
        auto addWatchDir = [&](std::string_view dir) {
            if (dir.empty()) return;
            std::string norm = OSTPlatform::Encoding::PathToUtf8(
                OSTPlatform::Encoding::PathFromUtf8(dir).lexically_normal());
            while (norm.size() > 1 && (norm.back() == '\\' || norm.back() == '/') &&
                   !(norm.size() == 3 && norm[1] == ':')) {
                norm.pop_back();
            }
            std::string key = norm;
            for (char& ch : key) {
                ch = (ch == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (seenDirs.insert(std::move(key)).second) {
                watchDirs.push_back(std::move(norm));
            }
        };
        for (const auto& p : Config::GetLuaPaths()) {
            addWatchDir(p);
        }
        addWatchDir(LuaDir);
        // In portable mode, also watch Steam's config/stplug-in if it already exists
        if (IsPortableMode()) {
            const auto steamLuaFs = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / "config" / "stplug-in";
            std::error_code ecLua;
            if (std::filesystem::is_directory(steamLuaFs, ecLua) && !ecLua) {
                addWatchDir(OSTPlatform::Encoding::PathToUtf8(steamLuaFs));
            }
        }

        // Pre-parse Lua directory in Phase 0 (before PatternLoader / hooks),
        // ensuring Lua configuration is immutable and ready before any UI overview generation.
        try {
            for (const auto& dir : watchDirs) {
                LuaConfig::ParseDirectory(dir);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("LuaConfig::ParseDirectory exception in Phase 0: {}", ex.what());
        } catch (...) {
            LOG_ERROR("LuaConfig::ParseDirectory unknown exception in Phase 0");
        }

        // Load pattern files for steamclient64.dll and steamui.dll.
        // Each call computes the SHA-256 of the DLL on disk, checks the local
        // cache, and downloads from GitHub if needed.  Both calls are synchronous
        // but run on this worker thread, never under the loader lock.
        PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
        PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

        // IPC method metadata (funcHash, fencepost, argc, ...)
        IPCLoader::Load(SteamclientPath);

        // Wire up lower-layer callbacks to hook notifications (IoC decoupling)
        ConfigFileWatcher::SetLicenseChangedCallback(&Hooks_Package::NotifyLicenseChanged);
        LuaFileWatcher::SetLicenseChangedCallback(&Hooks_Package::NotifyLicenseChanged);
        AppTicket::SetSourceTicketProvider(&Hooks_Decryption::GetCacheAppOwnershipTicket);
        AppTicket::SetBaselineTicketProvider(&Hooks_Decryption::GetReliableSourceTicket);
        CloudRedirectHost::SetLicenseChecker(&Hooks_Package::HasValidLicense);
        CloudRedirectHost::SetOnlineFixAppResolver([]() -> uint32_t {
            return Hooks_Misc::IsOnlineFixActive() ? Hooks_Misc::ResolveAppId() : 0;
        });

        // Match the proven pre-Diversion startup ordering for library/UI hooks:
        // Lua is fully loaded before SteamUI can evaluate ownership/overview data.
        HookManager::InstallUIHooks();

        // Replay configured apps via batch queue for eventual consistency across late injections.
        if (auto configuredApps = LuaConfig::GetConfiguredAppIdsSnapshot()) {
            Hooks_SteamUI::QueueAdditions(*configuredApps);
        }

        // Awaken installed scanner after its worker has been started by Install().
        Hooks_SteamUI::TriggerInstalledScanner();

        HookManager::InstallClientHooks();

        // Surface any functions that FindPattern() could not locate.
        PatternLoader::ReportMissingFunctions();

        // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
        // [cloud].enabled is set and cloud_redirect.dll is present.
        CloudRedirectHost::Initialize(SteamInstallPath);

        g_HooksInstalled.store(true, std::memory_order_release);
        if (Hooks_Diversion::g_hClientHooksReadyEvent) {
            ::SetEvent(Hooks_Diversion::g_hClientHooksReadyEvent);
        }

        // Start watchers strictly after all hooks and subsystems are fully ready,
        // ensuring no dynamic file changes trigger notifications before callbacks/hooks exist.
        LuaFileWatcher::Start(watchDirs);
        ConfigFileWatcher::Start(ConfigPath, LuaDir);

        LOG_INFO("SteamDaddy init complete ({})",
                 g_IsDiversionActive.load(std::memory_order_acquire) ? "Diversion active" : "Diversion bypassed, using original steamclient64");

        // Only refresh an exact-SHA pattern cache when this session actually found
        // a requested function entry missing. The parsed map is not hot-reloaded.
        PatternLoader::RefreshMissingMetadata();
        return 0;
    } catch (const std::exception& ex) {
        LOG_ERROR("Unhandled exception in InitThread: {}", ex.what());
        return 1;
    } catch (...) {
        LOG_ERROR("Unhandled unknown exception in InitThread");
        return 1;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        auto selfMod = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule);

        // 1. Resolve key runtime paths early at process load time (0ms)
        InitializeRuntimePaths(selfMod);

        // 2. Synchronize diversion shadow module if needed
        Hooks_Diversion::EnsureDiversionFileReady(
            OSTPlatform::Encoding::Utf8ToWide(SteamclientPath),
            OSTPlatform::Encoding::Utf8ToWide(DiversionPath));

        // 3. Synchronously hook KERNELBASE!LoadLibraryExW at 0ms
        Hooks_Diversion::Install();

        // 4. Keep this module pinned so explicit FreeLibrary cannot unload code
        // while hooks and worker threads may still reference it.
        HMODULE pinnedModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DllMain), &pinnedModule);

        // 5. Hand off background work to InitThread
        OSTPlatform::Thread::StartDetached([selfMod] {
            return InitThread(selfMod);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_HooksInstalled.store(false, std::memory_order_release);
        if (Hooks_Diversion::g_hClientHooksReadyEvent) {
            ::ResetEvent(Hooks_Diversion::g_hClientHooksReadyEvent);
        }
        // During process termination (pvReserved != nullptr), OS terminates all threads
        // before calling DllMain; avoid joining dead threads or performing loader-lock unhooks.
        if (pvReserved == nullptr) {
            Hooks_Diversion::Uninstall();
            ConfigFileWatcher::Stop();
            LuaFileWatcher::Stop();
            HookManager::UninstallClientHooks();
            HookManager::UninstallUIHooks();
            client_hModule.store(nullptr, std::memory_order_release);
            ui_hModule.store(nullptr, std::memory_order_release);
            g_IsDiversionActive.store(false, std::memory_order_release);
            ConfigFileWatcher::SetLicenseChangedCallback(nullptr);
            LuaFileWatcher::SetLicenseChangedCallback(nullptr);
            AppTicket::SetSourceTicketProvider(nullptr);
            CloudRedirectHost::SetLicenseChecker(nullptr);
            CloudRedirectHost::SetOnlineFixAppResolver(nullptr);
            CloudRedirectHost::Shutdown();
            ManifestClient::Shutdown();
            StatsClient::Shutdown();
            LuaConfig::Shutdown();
            Log::Shutdown();
        } else {
            g_IsDiversionActive.store(false, std::memory_order_release);
            // Process termination (ExitProcess): The OS has already terminated all secondary
            // threads. Never join or wait on mutexes held by terminated threads under the loader
            // lock. Proactively detach all joinable worker threads so CRT static destruction
            // does not invoke std::terminate() -> abort().
            ConfigFileWatcher::Detach();
            LuaFileWatcher::Detach();
            HookManager::DetachWorkerThreads();
        }
    }

    return TRUE;
}
