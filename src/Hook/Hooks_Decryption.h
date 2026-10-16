#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <vector>

namespace Hooks_Decryption {
    // LoadDepotDecryptionKey hook: serves user-provided decryption keys for
    // depots configured via Lua.
    void Install();
    void Uninstall();

    [[nodiscard]] std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId);
}
