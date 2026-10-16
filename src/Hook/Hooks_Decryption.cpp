#include "Hooks_Decryption.h"
#include "HookMacros.h"
#include "Steam/Enums.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <atomic>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

namespace {

    std::atomic<void*> g_pConfigStoreLocal{nullptr};

    HOOK_FUNC(ConfigStoreGetBinary, int32, void* pObject, EConfigStore eConfigStore, const char* KeyName, char* Key, uint32 KeySize) {
        const auto origGetBinary = oConfigStoreGetBinary;
        if (!origGetBinary) {
            return 0;
        }
        if (!KeyName) {
            return origGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
        }

        if (eConfigStore == k_EConfigStoreUserLocal && pObject) {
            void* currentLocal = g_pConfigStoreLocal.load(std::memory_order_acquire);
            if (currentLocal != pObject) {
                g_pConfigStoreLocal.store(pObject, std::memory_order_release);
                LOG_DECRYPTIONKEY_DEBUG("ConfigStoreGetBinary: captured local ConfigStore at {}", pObject);
            }
        }

        std::string_view name(KeyName);
        LOG_DECRYPTIONKEY_DEBUG("ConfigStore::GetBinary called for pObject={}, eConfigStore={}, KeyName='{}'", 
                                    pObject, static_cast<uint32>(eConfigStore), name);

        // Expected shape: ".../<DepotId>\DecryptionKey" or ".../<DepotId>/DecryptionKey"
        constexpr std::string_view kDecryptionKeySuffix = "DecryptionKey";
        if (name.size() > kDecryptionKeySuffix.size() + 1 && name.ends_with(kDecryptionKeySuffix)) {
            const size_t sep = name.size() - kDecryptionKeySuffix.size() - 1;
            if (name[sep] == '\\' || name[sep] == '/') {
                const size_t start = name.find_last_of("\\/", sep - 1);
                const size_t idStart = (start == std::string_view::npos) ? 0 : start + 1;
                if (idStart < sep) {
                    const std::string_view idStr = name.substr(idStart, sep - idStart);

                    AppId_t depotId = 0;
                    auto [ptr, ec] = std::from_chars(idStr.data(), idStr.data() + idStr.size(), depotId);
                    if (ec == std::errc{} && ptr == idStr.data() + idStr.size() &&
                        depotId != 0 && depotId != k_uAppIdInvalid) {
                        if (const auto& key = LuaConfig::GetDecryptionKey(depotId);
                            !key.empty() && key.size() <= static_cast<size_t>((std::numeric_limits<int32>::max)())) {
                            if (Key && KeySize >= key.size()) {
                                LOG_DECRYPTIONKEY_INFO("Providing decryption key for depot {}: {}", depotId,
                                                       spdlog::to_hex(key.data(), key.data() + key.size()));
                                std::memcpy(Key, key.data(), key.size());
                                return static_cast<int32>(key.size());
                            }
                            LOG_DECRYPTIONKEY_WARN("Decryption key for depot {} is too large ({} bytes) for buffer ({} bytes)",
                                                    depotId, key.size(), KeySize);
                        }
                    }
                }
            }
        }
        return origGetBinary(pObject, eConfigStore, KeyName, Key, KeySize);
    }

    [[nodiscard]] std::vector<uint8_t> ReadConfigStoreLocalBinary(const char* keyName) {
        if (!keyName) {
            return {};
        }

        void* pLocal = g_pConfigStoreLocal.load(std::memory_order_acquire);
        const auto origGetBinary = oConfigStoreGetBinary;
        if (!pLocal || !origGetBinary) {
            LOG_DECRYPTIONKEY_WARN("GetConfigStoreLocalBinary: ConfigStoreGetBinary not ready, cannot get binary value");
            return {};
        }

        constexpr size_t kInitialBufferSize = 4096;
        constexpr int32  kMaxSafeBufferSize  = 1024 * 1024; // 1MB safe ceiling

        uint8_t stackBuf[kInitialBufferSize];
        int32 result = origGetBinary(pLocal, k_EConfigStoreUserLocal,
                                     keyName,
                                     reinterpret_cast<char*>(stackBuf),
                                     static_cast<uint32>(sizeof(stackBuf)));
        if (result <= 0) {
            LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: failed to read KeyName='{}'", keyName);
            return {};
        }

        if (result <= static_cast<int32>(sizeof(stackBuf))) {
            std::vector<uint8_t> value(stackBuf, stackBuf + result);
            LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: got value for KeyName='{}' ({} bytes)",
                                    keyName, value.size());
            return value;
        }

        if (result > kMaxSafeBufferSize) {
            LOG_DECRYPTIONKEY_WARN("GetConfigStoreLocalBinary: requested size {} exceeds maximum safe limit {} for KeyName='{}'",
                                   result, kMaxSafeBufferSize, keyName);
            return {};
        }

        LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: buffer truncated ({}/{}), resizing and retrying for KeyName='{}'",
                               sizeof(stackBuf), result, keyName);
        std::vector<uint8_t> value(static_cast<size_t>(result));
        result = origGetBinary(pLocal, k_EConfigStoreUserLocal,
                               keyName,
                               reinterpret_cast<char*>(value.data()),
                               static_cast<uint32>(value.size()));
        if (result <= 0 || result > static_cast<int32>(value.size())) {
            LOG_DECRYPTIONKEY_WARN("GetConfigStoreLocalBinary: retry failed or returned invalid size {} for KeyName='{}'",
                                   result, keyName);
            return {};
        }

        value.resize(static_cast<size_t>(result));
        LOG_DECRYPTIONKEY_DEBUG("GetConfigStoreLocalBinary: got value for KeyName='{}' ({} bytes)",
                                keyName, value.size());
        return value;
    }
}

namespace Hooks_Decryption {
    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(ConfigStoreGetBinary);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(ConfigStoreGetBinary);
        UNHOOK_END();
    }

    std::vector<uint8_t> GetCacheAppOwnershipTicket(AppId_t appId) {
        if (appId == 0 || appId == k_uAppIdInvalid) {
            return {};
        }

        constexpr std::string_view kPrefix = "apptickets\\";
        char keyName[32];
        std::memcpy(keyName, kPrefix.data(), kPrefix.size());
        auto [ptr, ec] = std::to_chars(keyName + kPrefix.size(), keyName + sizeof(keyName) - 1, appId);
        if (ec != std::errc{}) {
            return {};
        }
        *ptr = '\0';

        std::vector<uint8_t> ticket = ReadConfigStoreLocalBinary(keyName);
        if (ticket.empty()) {
            LOG_DECRYPTIONKEY_DEBUG("no cached ticket for AppId {}", appId);
            return ticket;
        }
        LOG_DECRYPTIONKEY_DEBUG("got cached ticket for AppId {} ({} bytes)", appId, ticket.size());
        return ticket;
    }
}
