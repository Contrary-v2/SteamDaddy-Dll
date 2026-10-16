#pragma once

#include "Crypto.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OST::ExtractTickets {

struct CachedAccount {
    std::string accountName;
    uint64_t steamId{0};
    std::string refreshToken;
    std::string accessToken;
    int64_t lastLoginTime{0};
    bool isInvalid{false};       // 字段 6: 是否已被服务端证实失效 (DPAPI 持久化)
    std::string alias;           // 字段 7: 用户自定义多语言备注标签

    CachedAccount() = default;
    ~CachedAccount() noexcept {
        SecureWipeString(refreshToken);
        SecureWipeString(accessToken);
    }
    CachedAccount(const CachedAccount&) = default;
    CachedAccount(CachedAccount&& other) noexcept
        : accountName(std::move(other.accountName)),
          steamId(other.steamId),
          refreshToken(std::move(other.refreshToken)),
          accessToken(std::move(other.accessToken)),
          lastLoginTime(other.lastLoginTime),
          isInvalid(other.isInvalid),
          alias(std::move(other.alias)) {
        SecureWipeString(other.refreshToken);
        SecureWipeString(other.accessToken);
    }
    CachedAccount& operator=(const CachedAccount& other) {
        if (this != &other) {
            SecureWipeString(refreshToken);
            SecureWipeString(accessToken);
            accountName = other.accountName;
            steamId = other.steamId;
            refreshToken = other.refreshToken;
            accessToken = other.accessToken;
            lastLoginTime = other.lastLoginTime;
            isInvalid = other.isInvalid;
            alias = other.alias;
        }
        return *this;
    }
    CachedAccount& operator=(CachedAccount&& other) noexcept {
        if (this != &other) {
            SecureWipeString(refreshToken);
            SecureWipeString(accessToken);
            accountName = std::move(other.accountName);
            steamId = other.steamId;
            refreshToken = std::move(other.refreshToken);
            accessToken = std::move(other.accessToken);
            lastLoginTime = other.lastLoginTime;
            isInvalid = other.isInvalid;
            alias = std::move(other.alias);
            SecureWipeString(other.refreshToken);
            SecureWipeString(other.accessToken);
        }
        return *this;
    }
};

class TokenStorage {
public:
    // Loads all cached accounts from encrypted local storage
    [[nodiscard]] static std::vector<CachedAccount> LoadAccounts();

    // Saves list of accounts to encrypted local storage
    static bool SaveAccounts(const std::vector<CachedAccount>& accounts);

    // Inserts or updates an account entry with alias merge protection
    static bool UpsertAccount(const CachedAccount& account);

    // Updates alias for a specific account (shortcut [r])
    // If newAlias is empty or whitespace-only, clears the alias
    static bool UpdateAccountAlias(uint64_t steamId, std::string_view newAlias);
    static bool UpdateAccountAlias(std::string_view accountName, std::string_view newAlias);

    // Updates the isInvalid flag for an account when revoked or expired
    static bool MarkAccountInvalid(uint64_t steamId, bool isInvalid);
    static bool MarkAccountInvalid(std::string_view accountName, bool isInvalid);

    // Deletes single account by steamId or name (shortcut [d])
    static bool DeleteAccount(uint64_t steamId);
    static bool DeleteAccount(std::string_view accountName);

    // Thoroughly wipes and removes all credential cache files (shortcut [x])
    static bool WipeAll();

    // Cleanses an alias string (strips \t \r \n, trims spaces, clamps byte length)
    [[nodiscard]] static std::string SanitizeAlias(std::string_view raw);

    // Returns the canonical path of the encrypted cache file
    [[nodiscard]] static std::filesystem::path GetStorageFilePath();
};

} // namespace OST::ExtractTickets
