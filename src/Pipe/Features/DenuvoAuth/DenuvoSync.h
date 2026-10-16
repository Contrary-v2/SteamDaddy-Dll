#pragma once

#include "Pipe/PipeTypes.h"
#include <string>

namespace PipeManager::DenuvoAuth {

    // Checks whether cmdLine contains a specific argument (case-insensitive, token boundary aware).
    [[nodiscard]] bool HasCmdLineArg(std::string_view cmdLine, std::string_view arg) noexcept;
    [[nodiscard]] bool HasCmdLineArg(const char* cmdLine, const char* arg);

    // Checks whether cmdLine contains any variant of -nodenuvo (-nodenuvo, -no-denuvo, -no_denuvo, /nodenuvo...).
    [[nodiscard]] bool HasNoDenuvoArg(const char* cmdLine);

    // Checks whether cmdLine contains any variant of -forcedenuvo (-forcedenuvo, -force-denuvo, -force_denuvo, /forcedenuvo...).
    [[nodiscard]] bool HasForcedDenuvoArg(const char* cmdLine);

    // Checks whether cmdLine contains any variant of -dauth2 (-dauth2, --dauth2, -dauth-2, -dauth_2, /dauth2...).
    [[nodiscard]] bool HasDAuth2Arg(const char* cmdLine);

    // Checks whether cmdLine contains any variant of -lua (-lua, --lua, -d+, /lua, /d+).
    [[nodiscard]] bool HasLuaArg(const char* cmdLine);

    // Called when CUser_SpawnProcess is intercepted in Hooks_Misc.
    // Checks the zero-operation gatekeeper (!hasLua && !hasDPlus -> returns immediately).
    // If -d+ is present, records the intent and triggers sync/generation if authorized.
    void OnSpawnProcess(AppId_t appId, const char* pExePath, const char* cmdLine);

    // Checks if the given app was launched with -d+ in this session.
    [[nodiscard]] bool IsDPlusLaunch(AppId_t appId);

    // Clears the -d+ launch flag for the given app.
    void ClearDPlusLaunch(AppId_t appId);

    // Core synchronization / package generation function (-lua / genuine ownership).
    // Guaranteed:
    // 1. Never creates credentials/ or writes SteamID.txt to disk!
    // 2. Locks all installed manifests in <AppId>.lua (inserts/uncomments/updates setManifestid).
    // 3. Directly embeds setAppTicket in <AppId>.lua for offline Denuvo identity and Capcom Error 54 immunity.
    bool SyncOrGenerate(AppId_t appId, const std::string& exePath, bool allowGenerate);

    // Synchronizes the genuine AppTicket into <AppId>.lua.
    // - If <AppId>.lua exists: uncomments commented setAppTicket, updates if different,
    //   or appends if missing.
    // - Updates in-memory credential store and re-parses configuration immediately.
    bool SyncAppTicketToLua(AppId_t appId, const uint8_t* pTicketData, size_t ticketSize);

} // namespace PipeManager::DenuvoAuth
