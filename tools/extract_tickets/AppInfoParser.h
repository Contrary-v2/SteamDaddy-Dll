#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OST::ExtractTickets {

struct AppDepotManifest {
    uint32_t depotId{0};
    std::string manifestId; // GID string
    uint32_t dlcId{0};      // associated DLC AppID if any
};

struct ParsedAppInfoData {
    uint32_t appId{0};
    std::string name;
    std::string localizedName;
    std::vector<AppDepotManifest> depots;
    std::vector<uint32_t> dlcAppIds;
    bool requiresDenuvo{false};
};

// Parses <steamPath>/appcache/appinfo.vdf to extract non-zero PICS AccessTokens.
// If targetAppIds is provided, only extracts tokens for AppIDs present in that set.
// Returns a map of AppID -> AccessToken.
[[nodiscard]] std::unordered_map<uint32_t, uint64_t> ParseAppInfoTokens(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>* targetAppIds = nullptr);

// Parses <steamPath>/appcache/appinfo.vdf to extract depots, manifests (GID), and DLCs for appId.
[[nodiscard]] std::optional<ParsedAppInfoData> ParseAppInfoDepots(
    const std::string& steamPath, uint32_t appId);

// Parses raw binary VDF buffer (e.g. from CM PICS response)
[[nodiscard]] std::optional<ParsedAppInfoData> ParseBinaryVdfAppInfo(
    std::span<const uint8_t> buffer, uint32_t appId);

// Fast batch resolution of app names from <steamPath>/appcache/appinfo.vdf
[[nodiscard]] std::unordered_map<uint32_t, std::string> ParseAppNames(
    const std::string& steamPath,
    const std::unordered_set<uint32_t>& targetAppIds);

} // namespace OST::ExtractTickets
