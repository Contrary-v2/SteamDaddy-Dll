#include "Config.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/ManifestClient.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <mutex>
#include <shared_mutex>

namespace Config {
namespace {

    struct Snapshot {
        // [PROVIDER-FIX] Default provider changed from "manifestdex" to "20770407"
        // because 20770407 is now the first entry in kProviders[] and is therefore
        // the preferred source.  manifestdex remains available as the third fallback.
        std::string manifestProvider = "20770407";
        // [PROVIDER-FIX] New field: mirrors [manifest] failover in the TOML file.
        // true = FetchWithFailover() (ordered circular fallback); false = FetchActive() only.
        bool manifestFailover = true;
        ManifestTimeouts manifestTimeouts;
        LogLevel logLevel = LogLevel::Debug;
        std::string logDir;
        std::vector<std::string> luaPaths;
        std::string remoteUrlTemplate;
        std::string remoteOrder = "jsdelivr-first";
        bool statsEnableApi = true;
        std::vector<InjectDll> injectDlls;
        CloudSettings cloud;
        bool manifestLockOwnedGames = false;
        bool manifestAutoSyncOnUpdate = true;
    };

    std::shared_mutex g_mutex;
    bool g_loadedOnce = false;

    uint32_t manifestTimeoutResolve = 5000;
    uint32_t manifestTimeoutConnect = 5000;
    uint32_t manifestTimeoutSend    = 10000;
    uint32_t manifestTimeoutRecv    = 10000;
    LogLevel logLevel = LogLevel::Debug;
    std::string logDir;
    std::vector<std::string> luaPaths;
    std::string remoteUrlTemplate;
    std::string remoteOrder = "jsdelivr-first";
    bool statsEnableApi = true;
    std::vector<InjectDll> injectDlls;
    bool cloudEnabled = false;
    std::string cloudLibrary;
    bool manifestLockOwnedGames = false;
    bool manifestAutoSyncOnUpdate = true;

    const char* ToString(LogLevel level) {
        switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        }
        return "???";
    }

    const std::unordered_set<AppId_t>& GetDefaultAntiCheatAppids() {
        static const std::unordered_set<AppId_t> kDefaults = {
            // Valve VAC
            730, 570, 440, 550, 1422450, 240, 300,
            // EAC
            1172470, 252490, 381210, 2073850, 230410, 236390, 976730, 1240440,
            // BattlEye / ACE / Ricochet / nProtect
            578080, 359550, 1085660, 1938090, 2195250, 2669320, 553850
        };
        return kDefaults;
    }

    Snapshot MakeDefaultSnapshot(const std::string& configPath) {
        Snapshot snapshot;
        const char* storageDir = GetStorageDirectory();
        if (storageDir && storageDir[0] != '\0') {
            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                OSTPlatform::Encoding::PathFromUtf8(storageDir) / "steamdaddy");
        } else {
            std::filesystem::path p = OSTPlatform::Encoding::PathFromUtf8(configPath);
            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(p.parent_path() / "steamdaddy");
        }
        return snapshot;
    }

    void ApplySnapshot(const Snapshot& snapshot) {
        manifestTimeoutResolve = snapshot.manifestTimeouts.resolve;
        manifestTimeoutConnect = snapshot.manifestTimeouts.connect;
        manifestTimeoutSend    = snapshot.manifestTimeouts.send;
        manifestTimeoutRecv    = snapshot.manifestTimeouts.recv;
        logLevel               = snapshot.logLevel;
        logDir                 = snapshot.logDir;
        luaPaths               = snapshot.luaPaths;
        remoteUrlTemplate      = snapshot.remoteUrlTemplate;
        remoteOrder            = snapshot.remoteOrder;
        statsEnableApi         = snapshot.statsEnableApi;
        injectDlls             = snapshot.injectDlls;
        cloudEnabled           = snapshot.cloud.enabled;
        cloudLibrary           = snapshot.cloud.library;
        manifestLockOwnedGames = snapshot.manifestLockOwnedGames;
        manifestAutoSyncOnUpdate = snapshot.manifestAutoSyncOnUpdate;
    }

    // [PROVIDER-FIX] Accepts the full snapshot so we can wire SetFailoverEnabled
    // alongside SetProvider in a single call.  All three apply sites now call this
    // instead of the old two-liner so the failover flag is always pushed.
    void ApplyManifestProvider(const Snapshot& snapshot) {
        // Push the failover switch BEFORE SetProvider so that if SetProvider triggers
        // any immediate fetch (it shouldn't, but defensively) it sees the right switch.
        ManifestClient::SetFailoverEnabled(snapshot.manifestFailover);

        if (!ManifestClient::SetProvider(snapshot.manifestProvider)) {
            LOG_WARN("Unknown manifest.url \"{}\", falling back to \"20770407\"",
                     snapshot.manifestProvider);
            // [PROVIDER-FIX] Fallback changed from "manifestdex" to "20770407"
            // to match the new default preferred provider.
            ManifestClient::SetProvider("20770407");
        }
    }

    LoadResult ApplySnapshotLocked(const Snapshot& snapshot) {
        std::unique_lock lock(g_mutex);
        LoadResult result;
        result.luaPathsChanged = luaPaths != snapshot.luaPaths;
        ApplySnapshot(snapshot);
        g_loadedOnce = true;
        result.applied = true;
        return result;
    }

} // namespace

    LoadResult Load(const std::string& configPath) {
        Snapshot snapshot = MakeDefaultSnapshot(configPath);
        std::error_code ec;
        if (!std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(configPath), ec)) {
            LOG_INFO("Config file not found, using defaults");
            ApplyManifestProvider(snapshot);
            LoadResult result = ApplySnapshotLocked(snapshot);
            // [PROVIDER-FIX] Log now includes manifest.failover so the operator can
            // confirm the switch state at startup.
            LOG_INFO("Config loaded: manifest.url={} manifest.failover={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ManifestClient::IsFailoverEnabled(),
                     ToString(GetLogLevel()),
                     (uint32_t)GetLuaPaths().size(),
                     GetStatsEnableApi(),
                     GetRemoteUrlTemplate().empty() ? "<default>" : GetRemoteUrlTemplate());
            return result;
        }

        try {
            auto tbl = toml::parse_file(OSTPlatform::Encoding::PathFromUtf8(configPath).wstring());

            // [manifest]
            if (auto manifest = tbl["manifest"].as_table()) {
                if (auto val = (*manifest)["url"].value<std::string>()) {
                    snapshot.manifestProvider = *val;
                }
                // [PROVIDER-FIX] Read [manifest] failover bool.
                // Absent key keeps the struct default (true = failover on).
                if (auto val = (*manifest)["failover"].value<bool>()) {
                    snapshot.manifestFailover = *val;
                }
                if (auto val = (*manifest)["timeout_resolve_ms"].value<int64_t>()) {
                    if (*val > 0 && *val <= 300000)
                        snapshot.manifestTimeouts.resolve = static_cast<uint32_t>(*val);
                }
                if (auto val = (*manifest)["timeout_connect_ms"].value<int64_t>()) {
                    if (*val > 0 && *val <= 300000)
                        snapshot.manifestTimeouts.connect = static_cast<uint32_t>(*val);
                }
                if (auto val = (*manifest)["timeout_send_ms"].value<int64_t>()) {
                    if (*val > 0 && *val <= 300000)
                        snapshot.manifestTimeouts.send = static_cast<uint32_t>(*val);
                }
                if (auto val = (*manifest)["timeout_recv_ms"].value<int64_t>()) {
                    if (*val > 0 && *val <= 300000)
                        snapshot.manifestTimeouts.recv = static_cast<uint32_t>(*val);
                }
                if (auto val = (*manifest)["lock_owned_games"].value<bool>())
                    snapshot.manifestLockOwnedGames = *val;
                if (auto val = (*manifest)["auto_sync_on_update"].value<bool>())
                    snapshot.manifestAutoSyncOnUpdate = *val;
            }

            // [log] or [logging]
            auto log = tbl["log"].as_table();
            if (!log) {
                log = tbl["logging"].as_table();
            }
            if (log) {
                if (auto val = (*log)["level"].value<std::string>()) {
                    if (*val == "trace")           snapshot.logLevel = LogLevel::Trace;
                    else if (*val == "debug")       snapshot.logLevel = LogLevel::Debug;
                    else if (*val == "info")        snapshot.logLevel = LogLevel::Info;
                    else if (*val == "warn")        snapshot.logLevel = LogLevel::Warn;
                    else if (*val == "error")       snapshot.logLevel = LogLevel::Error;
                }
                if (auto val = (*log)["dir"].value<std::string>()) {
                    std::filesystem::path p = OSTPlatform::Encoding::PathFromUtf8(*val);
                    if (p.is_relative()) {
                        const char* storageDir = GetStorageDirectory();
                        if (storageDir && storageDir[0] != '\0') {
                            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                                (OSTPlatform::Encoding::PathFromUtf8(storageDir) / p).lexically_normal());
                        } else {
                            snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(
                                (OSTPlatform::Encoding::PathFromUtf8(configPath).parent_path() / p).lexically_normal());
                        }
                    } else {
                        snapshot.logDir = OSTPlatform::Encoding::PathToUtf8(p.lexically_normal());
                    }
                }
            }

            // [lua]
            if (auto lua = tbl["lua"].as_table()) {
                if (auto arr = (*lua)["paths"].as_array()) {
                    for (auto& elem : *arr) {
                        if (auto str = elem.value<std::string>()) {
                            snapshot.luaPaths.push_back(*str);
                        }
                    }
                }
            }

            // [remote]
            if (auto remote = tbl["remote"].as_table()) {
                if (auto val = (*remote)["url_template"].value<std::string>()) {
                    snapshot.remoteUrlTemplate = *val;
                }
                if (auto val = (*remote)["order"].value<std::string>()) {
                    snapshot.remoteOrder = *val;
                }
            }

            // [stats]
            if (auto stats = tbl["stats"].as_table()) {
                if (auto val = (*stats)["enable_api"].value<bool>()) {
                    snapshot.statsEnableApi = *val;
                }
            }

            // Global injection exclusion list: read primarily from [injects], with fallbacks to [exclude_appids], [inject_blacklist], or [inject]
            bool hasGlobalExclude = false;
            std::unordered_set<AppId_t> globalExcludeAppids;
            auto parseExclude = [&](const toml::table* tblNode) {
                if (!tblNode) return;
                auto readArr = [&](std::string_view key) {
                    if (auto ids = (*tblNode)[key].as_array()) {
                        hasGlobalExclude = true;
                        for (auto& id : *ids) {
                            if (auto v = id.value<int64_t>(); v && *v > 0 && *v <= UINT32_MAX) {
                                globalExcludeAppids.insert(static_cast<AppId_t>(*v));
                            }
                        }
                    }
                };
                readArr("exclude_appids");
                readArr("appids");
            };

            if (auto inj = tbl["injects"].as_table()) {
                parseExclude(inj);
            } else if (auto bl = tbl["exclude_appids"].as_table()) {
                parseExclude(bl);
            } else if (auto bl2 = tbl["inject_blacklist"].as_table()) {
                parseExclude(bl2);
            } else if (auto bl3 = tbl["inject"].as_table()) {
                parseExclude(bl3);
            } else if (auto arr = tbl["exclude_appids"].as_array()) {
                hasGlobalExclude = true;
                for (auto& id : *arr) {
                    if (auto v = id.value<int64_t>(); v && *v > 0 && *v <= UINT32_MAX) {
                        globalExcludeAppids.insert(static_cast<AppId_t>(*v));
                    }
                }
            }

            // [[inject]]
            if (auto arr = tbl["inject"].as_array()) {
                std::filesystem::path configDir = OSTPlatform::Encoding::PathFromUtf8(configPath).parent_path();
                for (auto& node : *arr) {
                    auto t = node.as_table();
                    if (!t) continue;
                    auto path = (*t)["path"].value<std::string>();
                    if (!path || path->empty()) continue;

                    // Relative paths resolve next to opensteamtool.toml, DLL dir, or steam.exe
                    std::filesystem::path full = OSTPlatform::Encoding::PathFromUtf8(*path);
                    if (full.is_relative()) {
                        std::filesystem::path candidate = configDir / full;
                        std::error_code ec;
                        if (std::filesystem::exists(candidate, ec)) {
                            full = candidate;
                        } else if (DllDir[0] != '\0' && std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(DllDir) / full, ec)) {
                            full = OSTPlatform::Encoding::PathFromUtf8(DllDir) / full;
                        } else if (SteamInstallPath[0] != '\0' && std::filesystem::exists(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / full, ec)) {
                            full = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / full;
                        } else {
                            full = candidate;
                        }
                    }
                    full = full.lexically_normal();
                    std::error_code ec;
                    if (!std::filesystem::exists(full, ec)) {
                        LOG_WARN("inject dll not found: {}", OSTPlatform::Encoding::PathToUtf8(full));
                        continue;
                    }

                    InjectDll dll;
                    dll.path = OSTPlatform::Encoding::PathToUtf8(full);
                    if (auto val = (*t)["when_cmdline"].value<std::string>()) dll.whenCmdline = *val;
                    if (auto val = (*t)["all_games"].value<bool>())           dll.allGames   = *val;
                    else                                                      dll.allGames   = false;
                    if (auto ids = (*t)["when_appids"].as_array()) {
                        for (auto& id : *ids) {
                            if (auto v = id.value<int64_t>(); v && *v > 0 && *v <= UINT32_MAX) {
                                dll.whenAppids.insert(static_cast<AppId_t>(*v));
                            }
                        }
                    }

                    // Anti-cheat / compatibility exclusion list from global [injects]:
                    // If explicitly configured in [injects], use it strictly (no default merging).
                    // Otherwise (unconfigured), fall back to built-in default anti-cheat blacklist.
                    dll.excludeAppids = hasGlobalExclude ? globalExcludeAppids : GetDefaultAntiCheatAppids();

                    // Whitelist priority: if an AppID is explicitly specified in when_appids,
                    // whitelist takes precedence over exclusion list, so it will not be excluded for this rule.
                    for (AppId_t whitelistedId : dll.whenAppids) {
                        dll.excludeAppids.erase(whitelistedId);
                    }

                    snapshot.injectDlls.push_back(std::move(dll));
                }
            }

            // [cloud]
            if (auto cloud = tbl["cloud"].as_table()) {
                if (auto val = (*cloud)["enabled"].value<bool>())
                    snapshot.cloud.enabled = *val;
                if (auto val = (*cloud)["library"].value<std::string>())
                    snapshot.cloud.library = *val;
            }

            ApplyManifestProvider(snapshot);
            LoadResult result = ApplySnapshotLocked(snapshot);
            // [PROVIDER-FIX] Log now includes manifest.failover.
            LOG_INFO("Config loaded: manifest.url={} manifest.failover={} log.level={} lua.paths={} stats.enable_api={} remote.url_template={}",
                     ManifestClient::ActiveProviderName(),
                     ManifestClient::IsFailoverEnabled(),
                     ToString(snapshot.logLevel),
                     (uint32_t)snapshot.luaPaths.size(),
                     snapshot.statsEnableApi,
                     snapshot.remoteUrlTemplate.empty() ? "<default>" : snapshot.remoteUrlTemplate);
            return result;

        } catch (const toml::parse_error& e) {
            LOG_WARN("Config parse error: {}", e.what());
        } catch (...) {
            LOG_WARN("Config load failed");
        }
        bool shouldApplyDefault = false;
        {
            std::unique_lock lock(g_mutex);
            shouldApplyDefault = !g_loadedOnce;
        }
        if (shouldApplyDefault) {
            ApplyManifestProvider(snapshot);
            std::unique_lock lock(g_mutex);
            const bool luaChanged = luaPaths != snapshot.luaPaths;
            ApplySnapshot(snapshot);
            g_loadedOnce = true;
            return {true, luaChanged};
        }
        return {};
    }

    ManifestTimeouts GetManifestTimeouts() {
        std::shared_lock lock(g_mutex);
        return {
            manifestTimeoutResolve,
            manifestTimeoutConnect,
            manifestTimeoutSend,
            manifestTimeoutRecv,
        };
    }

    LogLevel GetLogLevel() {
        std::shared_lock lock(g_mutex);
        return logLevel;
    }

    std::string GetLogDir() {
        std::shared_lock lock(g_mutex);
        return logDir;
    }

    std::vector<std::string> GetLuaPaths() {
        std::shared_lock lock(g_mutex);
        return luaPaths;
    }

    std::string GetRemoteUrlTemplate() {
        std::shared_lock lock(g_mutex);
        return remoteUrlTemplate;
    }

    std::string GetRemoteOrder() {
        std::shared_lock lock(g_mutex);
        return remoteOrder;
    }

    bool GetStatsEnableApi() {
        std::shared_lock lock(g_mutex);
        return statsEnableApi;
    }

    CloudSettings GetCloudSettings() {
        std::shared_lock lock(g_mutex);
        return {
            cloudEnabled,
            cloudLibrary,
        };
    }

    std::vector<InjectDll> GetInjectDlls() {
        std::shared_lock lock(g_mutex);
        return injectDlls;
    }

    bool GetManifestLockOwnedGames() {
        std::shared_lock lock(g_mutex);
        return manifestLockOwnedGames;
    }

    bool GetManifestAutoSyncOnUpdate() {
        std::shared_lock lock(g_mutex);
        return manifestAutoSyncOnUpdate;
    }

}
