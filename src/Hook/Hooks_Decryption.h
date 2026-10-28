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
    [[nodiscard]] std::vector<uint8_t> GetReliableSourceTicket();

    // In-memory ticket cache (passively harvested from ConfigStore reads + eMsg 858)
    void CacheAppOwnershipTicket(AppId_t appId, const uint8_t* data, size_t size);
    [[nodiscard]] std::vector<uint8_t> GetMemCachedAppOwnershipTicket(AppId_t appId);
    [[nodiscard]] std::vector<uint8_t> GetAnyMemCachedAppOwnershipTicket();
    [[nodiscard]] std::vector<uint8_t> WaitForCachedTicket(AppId_t appId, uint32_t timeoutMs);
}
