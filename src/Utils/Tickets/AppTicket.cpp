#include "AppTicket.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Config/LuaConfig.h"
#include "dllmain.h"
#include <windows.h>
#include "Utils/Logging/Log.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace AppTicket {
    namespace {
        constexpr AppId_t kLocalAppTicketSourceAppId = 7;
        constexpr AppId_t kSysVerifyAppId = 228980;
        constexpr size_t kDonorMaxSize = 65536;
        constexpr const wchar_t* kRegAppsPath = L"Software\\Valve\\Steam\\Apps";

        static bool IsUsableDonor(const std::vector<uint8_t>& t) {
            if (t.size() <= kAppTicketSignatureSize || t.size() > kDonorMaxSize) return false;
            uint32_t head = 0;
            memcpy(&head, t.data(), sizeof(head));
            return head != 0;
        }

        // The low 32 bits of the SteamID in the ticket are the account ID; use it to tell whose ticket this is.
        // Layout matches GetSpoofSteamID: [Size][Version][SteamID].
        static uint32_t DonorAccountId(const std::vector<uint8_t>& t) {
            if (t.size() < kSteamIdTicketMinimumSize) return 0;
            uint64_t steamId = 0;
            memcpy(&steamId, t.data() + 8, sizeof(steamId));
            return static_cast<uint32_t>(steamId & 0xFFFFFFFFull);
        }

        // The currently logged-in account. Returns 0 when unavailable; the caller then falls back to a pure structural check.
        // Switching accounts always restarts Steam, so the process never sees a second value and no cache invalidation is needed.
        static uint32_t ActiveAccountId() {
            uint32_t accountId = 0;
            std::wstring universe;
            if (OSTPlatform::SteamCredentialStore::GetActiveUser(accountId, universe) !=
                OSTPlatform::SteamCredentialStore::Status::Ok) {
                return 0;
            }
            return accountId;
        }

        enum class DonorKind { None, Memory, Registry, LocalConfig };
        static DonorKind g_donorKind = DonorKind::None;
        static AppId_t g_donorAppId = 0;

        static bool ParseLocalConfigAppTickets(const std::string& data, uint32_t wantAccount,
                                               AppId_t excludeAppId,
                                               std::vector<uint8_t>& out, AppId_t& appId) {
            const size_t marker = data.find("\"apptickets\"");
            if (marker == std::string::npos) return false;

            const size_t open = data.find('{', marker);
            if (open == std::string::npos) return false;

            size_t close = std::string::npos;
            int depth = 0;
            for (size_t i = open; i < data.size(); ++i) {
                if (data[i] == '{') {
                    ++depth;
                } else if (data[i] == '}') {
                    if (--depth == 0) { close = i; break; }
                }
            }
            if (close == std::string::npos) return false;

            size_t pos = open;
            auto NextQuoted = [&](std::string& value) -> bool {
                const size_t a = data.find('"', pos);
                if (a == std::string::npos || a >= close) return false;
                const size_t b = data.find('"', a + 1);
                if (b == std::string::npos || b >= close) return false;
                value.assign(data, a + 1, b - a - 1);
                pos = b + 1;
                return true;
            };
            auto HexNibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return c - 'A' + 10;
            };

            std::string key;
            std::string hex;
            while (NextQuoted(key) && NextQuoted(hex)) {
                if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;
                if (hex.size() < 2 || (hex.size() % 2) != 0) continue;

                bool allHex = true;
                for (char c : hex) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) { allHex = false; break; }
                }
                if (!allHex) continue;
                if (hex.size() / 2 <= kAppTicketSignatureSize) continue;

                std::vector<uint8_t> bytes;
                bytes.reserve(hex.size() / 2);
                for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                    bytes.push_back(static_cast<uint8_t>((HexNibble(hex[i]) << 4) | HexNibble(hex[i + 1])));
                }
                if (!IsUsableDonor(bytes)) continue;

                const AppId_t parsedApp = static_cast<AppId_t>(strtoul(key.c_str(), nullptr, 10));
                if (parsedApp == excludeAppId) continue;
                if (wantAccount != 0) {
                    const uint32_t ticketAccount = DonorAccountId(bytes);
                    if (ticketAccount != 0 && ticketAccount != wantAccount) {
                        LOG_DEBUG("LocalConfigDonor: skip appid {} base (account {} != {})",
                                  parsedApp, ticketAccount, wantAccount);
                        continue;
                    }
                }

                out = std::move(bytes);
                appId = parsedApp;
                return true;
            }
            return false;
        }

        static bool ReadLocalConfigDonor(uint32_t accountId, AppId_t excludeAppId,
                                         std::vector<uint8_t>& out, AppId_t& appId) {
            out.clear();
            appId = 0;
            if (accountId == 0 || SteamInstallPath[0] == '\0') return false;

            const std::string path = std::string(SteamInstallPath) + "\\userdata\\" +
                                     std::to_string(accountId) + "\\config\\localconfig.vdf";
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                LOG_DEBUG("LocalConfigDonor: cannot open {}", path);
                return false;
            }

            const std::string data((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
            if (data.empty()) return false;

            if (!ParseLocalConfigAppTickets(data, accountId, excludeAppId, out, appId)) {
                LOG_DEBUG("LocalConfigDonor: no usable ticket in {} ({} bytes)", path, data.size());
                return false;
            }
            LOG_DEBUG("LocalConfigDonor: picked appid {} ({} bytes) from account {}",
                      appId, out.size(), accountId);
            return true;
        }

        static bool ReadDonor(DonorKind kind, AppId_t donorAppId, AppId_t excludeAppId,
                              std::vector<uint8_t>& out) {
            out.clear();
            if (donorAppId == 0) return false;
            if (kind == DonorKind::Memory) {
                out = GetSteamConfigStoreTicket(donorAppId);
                if (out.empty()) out = GetCachedAppOwnershipTicket(donorAppId);
            } else if (kind == DonorKind::Registry) {
                std::vector<uint8_t> t;
                if (OSTPlatform::SteamCredentialStore::GetAppTicket(donorAppId, t) ==
                    OSTPlatform::SteamCredentialStore::Status::Ok) {
                    out = std::move(t);
                }
            } else if (kind == DonorKind::LocalConfig) {
                std::vector<uint8_t> t;
                AppId_t appId = 0;
                if (!ReadLocalConfigDonor(ActiveAccountId(), excludeAppId, t, appId)) return false;
                if (appId != donorAppId) return false;
                out = std::move(t);
            }
            return IsUsableDonor(out);
        }

        static bool FindDonorAppTicket(AppId_t excludeAppId, std::vector<uint8_t>& out, AppId_t& donorAppId) {
            const uint32_t activeAccount = ActiveAccountId();

            auto TryAccept = [&](DonorKind kind, AppId_t cand) -> bool {
                std::vector<uint8_t> ticket;
                if (!ReadDonor(kind, cand, excludeAppId, ticket)) return false;
                if (activeAccount != 0) {
                    const uint32_t donorAccount = DonorAccountId(ticket);
                    if (donorAccount != 0 && donorAccount != activeAccount) return false;
                }
                out = std::move(ticket);
                donorAppId = cand;
                return true;
            };

            if (g_donorKind != DonorKind::None && g_donorAppId != excludeAppId) {
                if (TryAccept(g_donorKind, g_donorAppId)) return true;
                g_donorKind = DonorKind::None;
                g_donorAppId = 0;
            }

            auto TryMemCandidates = [&](bool requireOwn) -> bool {
                const AppId_t memCandidates[] = { kLocalAppTicketSourceAppId, kSysVerifyAppId };
                for (AppId_t cand : memCandidates) {
                    if (cand == excludeAppId) continue;
                    if (!requireOwn) {
                        if (TryAccept(DonorKind::Memory, cand)) {
                            g_donorKind = DonorKind::Memory;
                            g_donorAppId = cand;
                            LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from memory appid={} ({} bytes, own_ticket={})",
                                     cand, out.size(), activeAccount != 0 && DonorAccountId(out) == activeAccount);
                            return true;
                        }
                        continue;
                    }
                    std::vector<uint8_t> ticket;
                    if (!ReadDonor(DonorKind::Memory, cand, excludeAppId, ticket)) continue;
                    if (activeAccount == 0 || DonorAccountId(ticket) != activeAccount) continue;
                    out = std::move(ticket);
                    donorAppId = cand;
                    g_donorKind = DonorKind::Memory;
                    g_donorAppId = cand;
                    LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from memory appid={} ({} bytes, own_ticket=true)",
                             cand, out.size());
                    return true;
                }
                return false;
            };

            auto TryLocalConfig = [&]() -> bool {
                if (activeAccount == 0) return false;
                std::vector<uint8_t> ticket;
                AppId_t cand = 0;
                if (!ReadLocalConfigDonor(activeAccount, excludeAppId, ticket, cand)) return false;
                out = std::move(ticket);
                donorAppId = cand;
                g_donorKind = DonorKind::LocalConfig;
                g_donorAppId = cand;
                LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from localconfig appid={} ({} bytes, account={})",
                         cand, out.size(), activeAccount);
                return true;
            };

            auto TryRegistry = [&](bool requireOwn) -> bool {
                HKEY hApps = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegAppsPath, 0, KEY_ENUMERATE_SUB_KEYS,
                                  &hApps) != ERROR_SUCCESS) {
                    return false;
                }
                wchar_t nameBuf[32];
                for (DWORD index = 0;; ++index) {
                    DWORD nameLen = static_cast<DWORD>(std::size(nameBuf));
                    if (RegEnumKeyExW(hApps, index, nameBuf, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                        break;
                    }
                    const unsigned long id = wcstoul(nameBuf, nullptr, 10);
                    if (id == 0 || id > UINT32_MAX || static_cast<AppId_t>(id) == excludeAppId) continue;
                    const AppId_t cand = static_cast<AppId_t>(id);
                    std::vector<uint8_t> ticket;
                    if (!ReadDonor(DonorKind::Registry, cand, excludeAppId, ticket)) continue;
                    if (requireOwn && (activeAccount == 0 || DonorAccountId(ticket) != activeAccount)) continue;
                    out = std::move(ticket);
                    donorAppId = cand;
                    g_donorKind = DonorKind::Registry;
                    g_donorAppId = cand;
                    RegCloseKey(hApps);
                    LOG_INFO("ForgeLocalAppOwnershipTicket: forge donor from registry appid={} ({} bytes, own_ticket={})",
                             cand, out.size(), requireOwn);
                    return true;
                }
                RegCloseKey(hApps);
                return false;
            };

            if (activeAccount != 0) {
                if (TryMemCandidates(true)) return true;
                if (TryLocalConfig()) return true;
                if (TryRegistry(true)) return true;
            }
            if (TryMemCandidates(false)) return true;
            if (TryRegistry(false)) return true;

            return false;
        }


        struct AppTicketEntry {
            std::vector<uint8_t> appTicket;
            std::vector<uint8_t> eTicket;
        };

        std::atomic<SourceTicketProvider> g_sourceTicketProvider{nullptr};
        std::atomic<BaselineTicketProvider> g_baselineTicketProvider{nullptr};
        std::shared_mutex g_ticketMutex;
        std::unordered_map<AppId_t, AppTicketEntry> g_tickets;
    } // namespace

    void SetSourceTicketProvider(SourceTicketProvider provider) noexcept {
        g_sourceTicketProvider.store(provider, std::memory_order_release);
    }

    void SetBaselineTicketProvider(BaselineTicketProvider provider) noexcept {
        g_baselineTicketProvider.store(provider, std::memory_order_release);
    }

    std::vector<uint8_t> GetSteamConfigStoreTicket(AppId_t appId) {
        if (appId == 0) return {};
        if (auto provider = g_sourceTicketProvider.load(std::memory_order_acquire)) {
            return provider(appId);
        }
        return {};
    }

    std::vector<uint8_t> GetReliableBaselineTicket() {
        if (auto provider = g_baselineTicketProvider.load(std::memory_order_acquire)) {
            return provider();
        }
        return {};
    }

    std::vector<uint8_t> GetCachedAppOwnershipTicket(AppId_t appId) {
        if (appId == 0) return {};
        {
            std::shared_lock lock(g_ticketMutex);
            auto it = g_tickets.find(appId);
            if (it != g_tickets.end() && !it->second.appTicket.empty()) {
                LOG_INFO("Successfully retrieved App Ownership Ticket, AppId: {}, Ticket Size: {}",
                         appId, it->second.appTicket.size());
                return it->second.appTicket;
            }
        }
        auto credTicket = GetAppOwnershipTicketFromCredentialStore(appId);
        if (!credTicket.empty()) {
            std::unique_lock lock(g_ticketMutex);
            g_tickets[appId].appTicket = credTicket;
            return credTicket;
        }
        LOG_TRACE("Read App Ownership Ticket for AppId {}: ticket unavailable", appId);
        return {};
    }

    // Pure functional forging of an ownership ticket from a trusted baseline source ticket
    std::vector<uint8_t> ForgeTicketFromSource(AppId_t targetAppId, std::span<const uint8_t> source) noexcept {
        if (targetAppId == 0 || source.size() <= kAppTicketSignatureSize) {
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        if (signedSize < kAppTicketMinimumValidSize) {
            return {};
        }

        std::vector<uint8_t> ticket;
        ticket.resize(source.size() + sizeof(AppId_t));

        auto* dst = ticket.data();
        std::memcpy(dst, source.data(), signedSize);
        std::memcpy(dst + signedSize, &targetAppId, sizeof(AppId_t));
        std::memcpy(dst + signedSize + sizeof(AppId_t), source.data() + signedSize, kAppTicketSignatureSize);

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, Physical Size: {}, Total Size: {}",
                 targetAppId, ticket.size(), source.size());
        return ticket;
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability:
    static std::vector<uint8_t> ForgeLocalAppOwnershipTicket(AppId_t appId) {
        if (appId == 0) return {};
        std::vector<uint8_t> source;
        AppId_t donorAppId = 0;
        if (!FindDonorAppTicket(appId, source, donorAppId)) {
            LOG_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: no source appticket (no usable donor)", appId);
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin(), source.begin() + signedSize);

        const uint8_t* appIdBytes = reinterpret_cast<const uint8_t*>(&appId);
        ticket.insert(ticket.end(), appIdBytes, appIdBytes + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin() + signedSize, source.end());

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, SourceAppId: {}, Physical Size: {}, Total Size: {}",
                 appId, donorAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source) {
        ticket = {};
        if (appId == 0) return false;
        
        if (source == AppTicketSource::MemoryOnly || source == AppTicketSource::MemoryThenForge) {
            ticket.data = GetCachedAppOwnershipTicket(appId);
            if (!ticket.data.empty() && ticket.data.size() >= kSteamIdTicketMinimumSize) {
                ticket.totalSize = static_cast<uint32>(ticket.data.size());
                ticket.appIdOffset = kAppTicketAppIdOffset;
                ticket.steamIdOffset = kAppTicketSteamIdOffset;
                std::memcpy(&ticket.signatureOffset, ticket.data.data(), sizeof(uint32));
                ticket.signatureSize = kAppTicketSignatureSize;
                if (ticket.signatureOffset > ticket.totalSize ||
                    ticket.signatureSize > (ticket.totalSize - ticket.signatureOffset) ||
                    (ticket.appIdOffset + sizeof(AppId_t)) > ticket.totalSize) {
                    LOG_WARN("AppTicket: corrupted signature or layout bounds for appId={}, sigOffset={}, sigSize={}, totalSize={}",
                             appId, ticket.signatureOffset, ticket.signatureSize, ticket.totalSize);
                    return false;
                }
                return true;
            }
        }

        if (source == AppTicketSource::MemoryOnly) return false;

        // Try baseline ladder first
        const std::vector<uint8_t> baseline = GetReliableBaselineTicket();
        if (!baseline.empty()) {
            ticket.data = ForgeTicketFromSource(appId, baseline);
        }
        if (ticket.data.empty()) {
            ticket.data = ForgeLocalAppOwnershipTicket(appId);
        }
        if (ticket.data.empty()) return false;

        ticket.totalSize = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize = kAppTicketSignatureSize;
        return true;
    }

    std::vector<uint8_t> GetCachedEncryptedTicket(AppId_t appId) {
        if (appId == 0) return {};
        {
            std::shared_lock lock(g_ticketMutex);
            auto it = g_tickets.find(appId);
            if (it != g_tickets.end() && !it->second.eTicket.empty()) {
                LOG_INFO("Successfully retrieved Encrypted App Ticket, AppId: {}, Ticket Size: {}",
                         appId, it->second.eTicket.size());
                return it->second.eTicket;
            }
        }
        auto credTicket = GetEncryptedTicketFromCredentialStore(appId);
        if (!credTicket.empty()) {
            std::unique_lock lock(g_ticketMutex);
            g_tickets[appId].eTicket = credTicket;
            return credTicket;
        }
        LOG_TRACE("Read Encrypted App Ticket for AppId {}: ticket unavailable", appId);
        return {};
    }

    bool HasCachedEncryptedTicket(AppId_t appId) {
        if (appId == 0) return false;
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        return it != g_tickets.end() && !it->second.eTicket.empty();
    }

    bool WriteAppOwnershipTicket(AppId_t appId, std::span<const uint8_t> data) {
        if (appId == 0 || data.size() < kSteamIdTicketMinimumSize) {
            LOG_WARN("WriteAppOwnershipTicket: invalid parameters (appId={}, size={})", appId, data.size());
            return false;
        }

        // Fast-path: if the identical ticket is already cached in memory, skip redundant write
        {
            std::shared_lock lock(g_ticketMutex);
            auto it = g_tickets.find(appId);
            if (it != g_tickets.end() &&
                it->second.appTicket.size() == data.size() &&
                std::memcmp(it->second.appTicket.data(), data.data(), data.size()) == 0) {
                LOG_DEBUG("WriteAppOwnershipTicket: AppId {} ticket already up-to-date, skipping redundant write", appId);
                return true;
            }
        }

        {
            std::unique_lock lock(g_ticketMutex);
            auto& entry = g_tickets[appId];
            entry.appTicket.assign(data.begin(), data.end());
        }
        std::vector<uint8_t> vec(data.begin(), data.end());
        OSTPlatform::SteamCredentialStore::WriteAppTicket(appId, vec);
        LOG_INFO("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    std::vector<uint8_t> GetAppOwnershipTicketFromCredentialStore(AppId_t appId) {
        if (appId == 0) return {};
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetAppTicket(appId, ticket);
        if (status == OSTPlatform::SteamCredentialStore::Status::Ok && !ticket.empty()) {
            LOG_INFO("Successfully retrieved AppTicket from credential store, AppId: {}, Size: {}", appId, ticket.size());
            return ticket;
        }
        return {};
    }

    std::vector<uint8_t> GetEncryptedTicketFromCredentialStore(AppId_t appId) {
        if (appId == 0) return {};
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetETicket(appId, ticket);
        if (status == OSTPlatform::SteamCredentialStore::Status::Ok && !ticket.empty()) {
            LOG_INFO("Successfully retrieved ETicket from credential store, AppId: {}, Size: {}", appId, ticket.size());
            return ticket;
        }
        return {};
    }

    bool WriteSteamID(AppId_t appId, uint64_t steamId) {
        return OSTPlatform::SteamCredentialStore::WriteSteamId(appId, steamId) == OSTPlatform::SteamCredentialStore::Status::Ok;
    }

    uint64_t GetSpoofSteamID(AppId_t appId) {
        if (!LuaConfig::HasDepot(appId)) return 0;
        uint64_t steamId = 0;
        if (OSTPlatform::SteamCredentialStore::GetSteamId(appId, steamId) == OSTPlatform::SteamCredentialStore::Status::Ok && steamId != 0) {
            return steamId;
        }
        auto ticket = GetAppOwnershipTicketFromCredentialStore(appId);
        if (ticket.empty()) ticket = GetCachedAppOwnershipTicket(appId);
        return ExtractSteamIdFromTicketBytes(ticket);
    }

    uint64_t GetForgeSteamID(AppId_t appId) {
        if (!LuaConfig::HasDepot(appId)) return 0;
        const auto baseline = GetReliableBaselineTicket();
        std::vector<uint8_t> ticket;
        if (!baseline.empty()) {
            ticket = ForgeTicketFromSource(appId, baseline);
        }
        if (ticket.empty()) {
            ticket = ForgeLocalAppOwnershipTicket(appId);
        }
        return ExtractSteamIdFromTicketBytes(ticket);
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        return WriteAppOwnershipTicket(appId, std::span<const uint8_t>(data));
    }

    bool RemoveAppOwnershipTicket(AppId_t appId) {
        if (appId == 0) return false;

        std::unique_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end()) {
            it->second.appTicket.clear();
        }
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, std::span<const uint8_t> data) {
        if (appId == 0 || data.empty()) return false;

        // Fast-path: if the identical ticket is already cached in memory, skip redundant write
        {
            std::shared_lock lock(g_ticketMutex);
            auto it = g_tickets.find(appId);
            if (it != g_tickets.end() &&
                it->second.eTicket.size() == data.size() &&
                std::memcmp(it->second.eTicket.data(), data.data(), data.size()) == 0) {
                LOG_DEBUG("WriteEncryptedTicket: AppId {} ticket already up-to-date, skipping redundant write", appId);
                return true;
            }
        }

        {
            std::unique_lock lock(g_ticketMutex);
            auto& entry = g_tickets[appId];
            entry.eTicket.assign(data.begin(), data.end());
        }
        std::vector<uint8_t> vec(data.begin(), data.end());
        OSTPlatform::SteamCredentialStore::WriteETicket(appId, vec);
        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        return WriteEncryptedTicket(appId, std::span<const uint8_t>(data));
    }

    bool RemoveEncryptedTicket(AppId_t appId) {
        if (appId == 0) return false;

        std::unique_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end()) {
            it->second.eTicket.clear();
        }
        return true;
    }

    bool ClearCachedTickets(AppId_t appId) {
        if (appId == 0) return false;

        std::unique_lock lock(g_ticketMutex);
        return g_tickets.erase(appId) > 0;
    }

    uint64_t ExtractSteamIdFromTicketBytes(std::span<const uint8_t> ticket) noexcept {
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        if (ticket.size() < kSteamIdTicketMinimumSize) return 0;
        uint64_t steamId = 0;
        std::memcpy(&steamId, ticket.data() + kAppTicketSteamIdOffset, sizeof(uint64_t));
        return steamId;
    }

    uint64_t ExtractSteamIdFromTicketBytes(const uint8_t* data, size_t size) noexcept {
        if (!data || size == 0) return 0;
        return ExtractSteamIdFromTicketBytes(std::span<const uint8_t>(data, size));
    }

    uint64_t ExtractSteamIdFromTicketBytes(const std::vector<uint8_t>& ticket) noexcept {
        return ExtractSteamIdFromTicketBytes(std::span<const uint8_t>(ticket));
    }

    uint64_t GetTicketSteamID(AppId_t appId) {
        if (appId == 0) return 0;
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.appTicket.empty()) {
            return ExtractSteamIdFromTicketBytes(it->second.appTicket);
        }
        return 0;
    }
}
