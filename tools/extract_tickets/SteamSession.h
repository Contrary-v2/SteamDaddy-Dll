#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "VdfParser.h"
#include "steam.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace OST::ExtractTickets {

struct DlcInfo {
    uint32_t dlcId{0};
    std::string name;
};

[[nodiscard]] HMODULE LoadSteamClient64(const std::string& steamPath, std::string& loadedPath);
[[nodiscard]] ISteamClient* CreateSteamClient(HMODULE module);
[[nodiscard]] bool OpenSession(ISteamClient* client, HSteamPipe& pipe, HSteamUser& user);

[[nodiscard]] std::optional<std::vector<uint8_t>> ExtractAppOwnershipTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId);

[[nodiscard]] std::optional<std::vector<uint8_t>> ExtractEncryptedAppTicket(
    ISteamClient* client, HSteamPipe pipe, HSteamUser user, uint32_t appId);

[[nodiscard]] std::vector<DepotKeyInfo> ExtractDepotDecryptionKeys(
    const std::string& steamPath,
    uint32_t appId,
    ISteamClient* client,
    HSteamPipe pipe,
    HSteamUser user,
    std::vector<DlcInfo>& outDlcs);

enum class WorkerExitCode : uint32_t {
    Success = 0,
    ErrGeneral = 1,
    ErrNoSteamOrSession = 2,
    ErrNoLicenseOrPayload = 3,
    ErrDiskIO = 4,
    ErrTimeout = 5,
    ErrCrash = 6
};

[[nodiscard]] bool HasSubstantivePayload(
    const std::optional<std::vector<uint8_t>>& ownership,
    const std::optional<std::vector<uint8_t>>& encrypted,
    const std::vector<DepotKeyInfo>& depotKeys,
    const std::vector<DlcInfo>& dlcs,
    const std::unordered_map<uint32_t, uint64_t>& appTokens) noexcept;

} // namespace OST::ExtractTickets

