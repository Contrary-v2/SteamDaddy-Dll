#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "Steam/Types.h"

namespace Config {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    struct ManifestTimeouts {
        uint32_t resolve = 5000;
        uint32_t connect = 5000;
        uint32_t send    = 10000;
        uint32_t recv    = 10000;
    };

    // [[inject]] entry: a DLL loaded into a matching game process at the IPC handshake.
    struct InjectDll {
        std::string                 path;          // resolved absolute path
        std::string                 whenCmdline;   // substring required in the game command line
        std::unordered_set<AppId_t> whenAppids;    // appids this entry applies to
        std::unordered_set<AppId_t> excludeAppids; // appids to exclude when allGames is true
        bool                        allGames = false; // false: only Lua-unlocked games
    };

    struct CloudSettings {
        bool enabled = false;
        std::string library;
    };

    struct LoadResult {
        bool applied = false;
        bool luaPathsChanged = false;
    };

    LoadResult Load(const std::string& configPath);

    [[nodiscard]] ManifestTimeouts GetManifestTimeouts();
    [[nodiscard]] LogLevel GetLogLevel();
    [[nodiscard]] std::string GetLogDir();
    [[nodiscard]] std::vector<std::string> GetLuaPaths();
    [[nodiscard]] std::string GetRemoteUrlTemplate();
    [[nodiscard]] std::string GetRemoteOrder();
    [[nodiscard]] CloudSettings GetCloudSettings();
    [[nodiscard]] bool GetStatsEnableApi();
    [[nodiscard]] std::vector<InjectDll> GetInjectDlls();
    [[nodiscard]] bool GetManifestLockOwnedGames();
    [[nodiscard]] bool GetManifestAutoSyncOnUpdate();
}
