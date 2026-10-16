#pragma once

#include "Steam/Structs.h"
#include "Steam/Types.h"

#include <string>

// Catch-all for lightweight info-capture int3 traps and launch-time rewrites
// that don't fit a dedicated category — currently:
//   * GetAppIDForCurrentPipe  -> captures the SteamEngine pointer
//   * SpawnProcess            -> OnlineFix detection + 480 rewrite
//   * GetAppDataFromAppInfo   -> captures the CAppInfoCache pointer
namespace Hooks_Misc {
    inline constexpr AppId_t kOnlineFixAppId = 480;

    void Install();
    void Uninstall();

    // Returns the AppId for the current Steam pipe via the captured engine
    // pointer, or 0 if we haven't yet observed the host calling
    // GetAppIDForCurrentPipe.
    [[nodiscard]] AppId_t GetAppIDForCurrentPipeWrap();

    // True while a -onlinefix game is the active spawn.
    [[nodiscard]] bool IsOnlineFixActive();

    // True once SteamNetworkingSockets (IPC interface 46) has been observed.
    [[nodiscard]] bool IsNetworkingSocketsActive();

    // Call when the game uses SteamNetworkingSockets (IPC interface 46).
    void NotifyNetworkingSocketsUsed();

    // True once P2P started — GetAppID reports 480; before, the real appid.
    [[nodiscard]] bool ShouldReportOnlineFixAppId();

    // Grow a CUtlBuffer to at least 'newCapacity' bytes and set m_Put = newCapacity.
    // Uses CUtlBuffer::EnsureCapacity from steamclient, resolved on first call.
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity, bool updatePut = false);

    // Resolve the current pipe's AppID: maps 480 -> realAppId when OnlineFix is active,
    // and strictly returns 0 if the current pipe is unbound/non-game (no global fallback).
    [[nodiscard]] AppId_t ResolvePipeAppId();

    // Resolve the real appid: uses ResolvePipeAppId(), and falls back to the active
    // OnlineFix realAppId when called from pipe-less contexts (e.g. CM network thread).
    [[nodiscard]] AppId_t ResolveAppId();

    // Atomically reset OnlineFix state (called when Steam signals exit sync completion
    // or when a non-onlinefix game launches).
    void ResetOnlineFixState();

    // Get localized game name via GetAppDataFromAppInfo (cached).
    [[nodiscard]] std::string GetGameNameByAppID(AppId_t appId);

}
