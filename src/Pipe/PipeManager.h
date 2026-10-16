#pragma once

#include "Steam/Structs.h"
#include "Steam/Types.h"
#include <chrono>

namespace PipeManager {

    // Called after steamclient processes a pipe handshake. Resolves the caller
    // once, caches the process snapshot, then lets each Pipe feature react.
    void OnHandshake(CPipeClient* pipe);

    // Returns true if the pipe belongs to an external tool process (e.g. extract_tickets.exe).
    // Optionally associates appId with the tool pipe session if known.
    [[nodiscard]] bool IsToolPipe(const CPipeClient* pipe, AppId_t appId = k_uAppIdInvalid);

    // Returns true if an external tool process is currently active or extracting for the specified appId.
    [[nodiscard]] bool IsToolActiveForApp(AppId_t appId);

    // Returns true if an external tool was connected or active for the specified appId within the given window.
    [[nodiscard]] bool WasToolActiveRecently(AppId_t appId, std::chrono::milliseconds window = std::chrono::seconds(3));

} // namespace PipeManager
