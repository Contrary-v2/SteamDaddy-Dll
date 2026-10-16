#include "AppTicket.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Config/LuaConfig.h"
#include <windows.h>
#include "Utils/Logging/Log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
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

        enum class DonorKind { None, Memory, Registry };
        static DonorKind g_donorKind = DonorKind::None;
        static AppId_t g_donorAppId = 0;

        static bool ReadDonor(DonorKind kind, AppId_t donorAppId, std::vector<uint8_t>& out) {
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
            }
            return IsUsableDonor(out);
        }

        static bool FindDonorAppTicket(AppId_t excludeAppId, std::vector<uint8_t>& out, AppId_t& donorAppId) {
            out.clear();
            donorAppId = 0;
            if (g_donorKind != DonorKind::None && g_donorAppId != excludeAppId) {
                if (ReadDonor(g_donorKind, g_donorAppId, out)) {
                    donorAppId = g_donorAppId;
                    return true;
                }
                g_donorKind = DonorKind::None;
                g_donorAppId = 0;
            }
            const AppId_t memCandidates[] = { kLocalAppTicketSourceAppId, kSysVerifyAppId };
            for (AppId_t cand : memCandidates) {
                if (cand == excludeAppId) continue;
                if (ReadDonor(DonorKind::Memory, cand, out)) {
                    g_donorKind = DonorKind::Memory;
                    g_donorAppId = cand;
                    donorAppId = cand;
                    return true;
                }
            }
            HKEY hApps = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegAppsPath, 0, KEY_ENUMERATE_SUB_KEYS, &hApps) != ERROR_SUCCESS) {
                return false;
            }
            wchar_t nameBuf[32];
            for (DWORD index = 0; ; ++index) {
                DWORD nameLen = static_cast<DWORD>(std::size(nameBuf));
                if (RegEnumKeyExW(hApps, index, nameBuf, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                    break;
                }
                const unsigned long id = wcstoul(nameBuf, nullptr, 10);
                if (id == 0 || id > UINT32_MAX || static_cast<AppId_t>(id) == excludeAppId) continue;
                if (ReadDonor(DonorKind::Registry, static_cast<AppId_t>(id), out)) {
                    RegCloseKey(hApps);
                    g_donorKind = DonorKind::Registry;
                    g_donorAppId = static_cast<AppId_t>(id);
                    donorAppId = static_cast<AppId_t>(id);
                    LOG_INFO("FindDonorAppTicket: donor ticket found in registry: appid={} ({} bytes)", donorAppId, out.size());
                    return true;
                }
            }
            RegCloseKey(hApps);
            return false;
        }


        struct AppTicketEntry {
            std::vector<uint8_t> appTicket;
            std::vector<uint8_t> eTicket;
        };

        std::atomic<SourceTicketProvider> g_sourceTicketProvider{nullptr};
        std::shared_mutex g_ticketMutex;
        std::unordered_map<AppId_t, AppTicketEntry> g_tickets;
    } // namespace

    void SetSourceTicketProvider(SourceTicketProvider provider) noexcept {
        g_sourceTicketProvider.store(provider, std::memory_order_release);
    }

    std::vector<uint8_t> GetSteamConfigStoreTicket(AppId_t appId) {
        if (appId == 0) return {};
        if (auto provider = g_sourceTicketProvider.load(std::memory_order_acquire)) {
            return provider(appId);
        }
        return {};
    }

    std::vector<uint8_t> GetCachedAppOwnershipTicket(AppId_t appId) {
        if (appId == 0) return {};
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.appTicket.empty()) {
            LOG_INFO("Successfully retrieved App Ownership Ticket, AppId: {}, Ticket Size: {}",
                     appId, it->second.appTicket.size());
            return it->second.appTicket;
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

        ticket.data = ForgeLocalAppOwnershipTicket(appId);
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
        std::shared_lock lock(g_ticketMutex);
        auto it = g_tickets.find(appId);
        if (it != g_tickets.end() && !it->second.eTicket.empty()) {
            LOG_INFO("Successfully retrieved Encrypted App Ticket, AppId: {}, Ticket Size: {}",
                     appId, it->second.eTicket.size());
            return it->second.eTicket;
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
        auto ticket = ForgeLocalAppOwnershipTicket(appId);
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
