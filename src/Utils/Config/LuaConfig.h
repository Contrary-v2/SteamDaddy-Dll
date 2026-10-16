#ifndef LUACONFIG_H
#define LUACONFIG_H

#include "Steam/Types.h"

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LuaConfig{
    [[nodiscard]] bool HasDepot(AppId_t appId, bool excludeOwned = true);
    [[nodiscard]] bool IsOwned(AppId_t appId);
    void MarkOwned(AppId_t appId);
    void UnmarkOwned(AppId_t appId);
    void ClearOwned();
    [[nodiscard]] std::vector<AppId_t> GetAllDepotIds();
    [[nodiscard]] std::vector<uint8> GetDecryptionKey(AppId_t appId);
    [[nodiscard]] uint64_t GetAccessToken(AppId_t appId);
    // Legacy third-party CD key override for an app, if the user set one via
    // setlegacycdkey(appid, "KEY"); nullopt when no override is configured.
    [[nodiscard]] std::optional<std::string> GetLegacyCDKey(AppId_t appId);
    [[nodiscard]] uint64_t GetStatSteamId(AppId_t appId);
    void PrewarmStatSteamId(AppId_t appId);
    [[nodiscard]] bool pinApp(AppId_t appId);
    [[nodiscard]] uint32_t GetPurchaseTime(AppId_t appId);

    struct ManifestOverride {
          uint64_t gid;
          uint64_t size;
    };
    [[nodiscard]] std::unordered_map<uint64_t, ManifestOverride> GetManifestOverrides();

    void ParseFile(const std::string& filePath, bool rebuildSnapshot = true);
    void UnloadFile(const std::string& filePath, bool isPermanentRemoval = false, bool rebuildSnapshot = true);
    uint32_t UnloadDirectory(const std::string& directory);
    // Returns and clears the list of depot IDs removed/added since last call.
    [[nodiscard]] std::vector<AppId_t> TakePendingRemovals();
    [[nodiscard]] std::vector<AppId_t> TakePendingAdditions();
    void ParseDirectory(const std::string& directory);
    void ReloadDirectories(const std::vector<std::string>& directories, bool clearPendingAdditions = false);

    // Returns the file path of the Lua configuration file associated with the given appId,
    // or empty string if not tracked.
    [[nodiscard]] std::string FindLuaFileForAppId(AppId_t appId);

    // Returns a list of all distinct AppIds configured across all loaded Lua files.
    [[nodiscard]] std::vector<AppId_t> GetConfiguredAppIds();

    // RCU non-blocking snapshot: Returns a thread-safe shared_ptr to the cached list of configured AppIds.
    [[nodiscard]] std::shared_ptr<const std::vector<AppId_t>> GetConfiguredAppIdsSnapshot();

    // Resolves the Steam\depotcache directory (always points to Steam install path, even in portable mode).
    [[nodiscard]] std::string GetSteamDepotcacheDir();
    // Recursively copies all *.manifest files from directory into Steam\depotcache (skipping duplicates).
    uint32_t SyncManifests(const std::string& directory, const std::string& targetDepotcacheDir = "");
    // Copies a single manifest file into Steam\depotcache (skipping duplicates).
    bool CopyManifestToDepotcache(const std::string& manifestFilePath, const std::string& targetDepotcacheDir = "");

    [[nodiscard]] bool HasManifestCodeFunc() noexcept;
    [[nodiscard]] bool CallManifestFetchCode(uint64_t gid, uint64_t* outCode);

    [[nodiscard]] bool HasManifestCodeFuncEx() noexcept;
    [[nodiscard]] bool CallManifestFetchCodeEx(uint64_t app_id, uint64_t depot_id, uint64_t gid, uint64_t* outCode);

    // Returns the appid configured for a process exe name via addprocess(), or
    // k_uAppIdInvalid if none. Used by PipeManager to identify games that don't
    // export SteamAppId (e.g. launcher-spawned child processes).
    [[nodiscard]] AppId_t GetAppIdForProcess(std::string_view imageName);

    // Returns true if the appid was marked via forcedenuvo(), or launched with -forcedenuvo,
    // bypassing ProtectionScan in DenuvoAuth (for games where the heuristic fails).
    [[nodiscard]] bool IsForcedDenuvo(AppId_t appId);

    // Records whether -forcedenuvo command-line launch argument was passed for appId.
    void SetCmdLineForcedDenuvo(AppId_t appId, bool active);

    // Returns true if the appid was marked via nodenuvo() / disallowdenuvo(),
    // or launched with -nodenuvo, completely skipping ProtectionScan and Denuvo authorization.
    [[nodiscard]] bool IsNoDenuvo(AppId_t appId);

    // Records whether -nodenuvo command-line launch argument was passed for appId.
    void SetCmdLineNoDenuvo(AppId_t appId, bool active);

    // Returns true if the appid was marked via dauth2(), or launched with -dauth2,
    // activating Scheme 2 (adaptive time window & ticket lease) instead of default upstream scheme.
    [[nodiscard]] bool IsDAuth2(AppId_t appId);

    // Records whether -dauth2 command-line launch argument was passed for appId.
    void SetCmdLineDAuth2(AppId_t appId, bool active);

    // On-demand eticket backend URL set via seteticketurl() in Lua config.
    // Empty string means the feature is disabled and EticketClient falls
    // back to the static credential-store ticket (original behaviour).
    [[nodiscard]] std::string GetEticketUrl();

    // Shuts down the Lua state and cleans up resources on clean dynamic unload.
    void Shutdown();
}

#endif // LUACONFIG_H
