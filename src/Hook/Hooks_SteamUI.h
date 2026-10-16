#pragma once

#include "Steam/Types.h"

#include <span>

// Hooks targeting steamui.dll:

namespace Hooks_SteamUI {
    void Install();
    void Uninstall();
    void DetachWorkerThreads();

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
    // Cancels a queued removal when the app is added again before the UI drains it.
    void CancelRemoval(AppId_t appId);
    // Queues an appId for addition/restoration in the library UI
    void QueueAddition(AppId_t appId);
    // Batch queues appIds for addition/restoration in the library UI with O(N) complexity
    void QueueAdditions(std::span<const AppId_t> appIds);
    // Checks if an appId is currently marked as removed in the UI
    [[nodiscard]] bool IsRemoved(AppId_t appId);
    // Triggers an immediate background rescan of libraryfolders.vdf for installed apps
    void TriggerInstalledScanner();
}
