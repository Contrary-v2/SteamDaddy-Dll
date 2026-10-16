#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "IPCMessages.gen.h"
#include "PendingAPICalls.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Tickets/EticketClient.h"
#include "Pipe/PipeManager.h"
#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Utils/Logging/Log.h"
#include "Hooks_Misc.h"
#include "Utils/Config/LuaConfig.h"
#include "Hooks_Package.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    using namespace IPCMessages::IClientUser;

    template <typename T>
    class NoDestructor final {
        alignas(T) std::byte m_storage[sizeof(T)];

    public:
        template <typename... Args>
        explicit NoDestructor(Args&&... args) {
            std::construct_at(reinterpret_cast<T*>(m_storage), std::forward<Args>(args)...);
        }

        ~NoDestructor() = default;

        NoDestructor(const NoDestructor&) = delete;
        NoDestructor& operator=(const NoDestructor&) = delete;
        NoDestructor(NoDestructor&&) = delete;
        NoDestructor& operator=(NoDestructor&&) = delete;

        [[nodiscard]] T* get() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
        [[nodiscard]] const T* get() const noexcept { return std::launder(reinterpret_cast<const T*>(m_storage)); }
        [[nodiscard]] T& operator*() noexcept { return *get(); }
        [[nodiscard]] const T& operator*() const noexcept { return *get(); }
        [[nodiscard]] T* operator->() noexcept { return get(); }
        [[nodiscard]] const T* operator->() const noexcept { return get(); }
    };

    // Fresh, nonce-bound etickets minted on-demand in RequestEncryptedAppTicket
    // (keyed by appId) and consumed by GetEncryptedAppTicket on the same launch.
    // Lets the strict-Denuvo path serve a ticket matching the launch nonce while
    // keeping GetEncryptedAppTicket's credential-store serve as the fallback.
    std::mutex g_freshEticketMutex;
    NoDestructor<std::unordered_map<AppId_t, std::vector<uint8_t>>> g_freshEticket;

    AppId_t ResolvePipeOrOnlineFixAppId(CPipeClient* pipe) {
        AppId_t appId = Hooks_Misc::ResolvePipeAppId();
        if (appId == 0 && pipe && !pipe->m_bInProcess) {
            appId = PipeManager::DenuvoAuth::GetAuthorizedAppId(pipe);
        }
        return appId;
    }

    // [Post-Handler]: IClientUser::GetSteamID
    void HandlerPost_IClientUser_GetSteamID(CPipeClient* pipe,CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        const AppId_t appId = ResolvePipeOrOnlineFixAppId(pipe);
        if (appId == 0 || !LuaConfig::HasDepot(appId, false)) return;

        // Genuinely owned and family shared apps: OST must NEVER touch or spoof SteamID,
        // unless Scheme 2 (-dauth2 / dauth2(appid)) is explicitly requested.
        // Preserves player's authentic identity, local save directory, and native Steam Cloud.
        if (Hooks_Package::HasValidLicense(appId) && !LuaConfig::IsDAuth2(appId)) return;

        GetSteamIDResp resp{pWrite};
        if (!resp.ok()) return;

        // Only spoof SteamID when the pipe belongs to a Denuvo process currently
        // within its adaptive authorization window (startup grace period or ticket lease).
        // Outside the authorization window, the user's real SteamID is preserved so that
        // save paths (ISteamUser::GetUserDataFolder / userdata/<RealAccountID>/) remain
        // bound to the user's actual account rather than the pool account.
        if (!PipeManager::DenuvoAuth::IsAuthorizedPipe(pipe)) {
            LOG_IPC_TRACE("IClientUser::GetSteamID: AppId={} outside auth window, keeping real SteamID {}",
                          appId, resp.DebugString());
            return;
        }

        const uint64 spoofed = AppTicket::GetTicketSteamID(appId);
        if (!spoofed) {
            return;
        }

        LOG_IPC_DEBUG("IClientUser::GetSteamID: AppId={} [Auth-Window Active] Original: {} -> Spoofed: 0x{:X}({})", 
                      appId, resp.DebugString(), spoofed, spoofed);
        resp.set_returnValue(spoofed);
    }

    // [Post-Handler]: IClientUser::GetAppOwnershipTicketExtendedData
    void HandlerPost_IClientUser_GetAppOwnershipTicketExtendedData(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        GetAppOwnershipTicketExtendedDataReq req{pRead};
        if (!req.ok()) return;

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData:{}", req.DebugString());
        if (req.cbMaxTicket() < 0 || req.cbMaxTicket() > INT32_MAX - 32) return;

        AppId_t appId = req.unAppID() == Hooks_Misc::kOnlineFixAppId ? Hooks_Misc::ResolvePipeAppId() : req.unAppID();
        if (appId == 0 && pipe && !pipe->m_bInProcess) {
            appId = PipeManager::DenuvoAuth::GetAuthorizedAppId(pipe);
        }
        if (appId == 0) return;

        GetAppOwnershipTicketExtendedDataResp origResp{pWrite, static_cast<size_t>(req.cbMaxTicket())};
        const bool origTicketValid = origResp.ok() && origResp.returnValue() > 0;

        // If the account has a valid license (truly owned or family shared), Steam client
        // natively manages tickets. OST does not forge or inject tickets for genuine/shared games,
        // unless Scheme 2 (-dauth2 / dauth2(appid)) is explicitly requested.
        // If a Lua file exists (e.g. for manifest locking), sync the genuine ticket to Lua.
        if (Hooks_Package::HasValidLicense(appId) && !LuaConfig::IsDAuth2(appId)) {
            if (origTicketValid) {
                auto ticketSpan = origResp.pTicket();
                if (!ticketSpan.empty()) {
                    const size_t ticketSize = (std::min)(ticketSpan.size(), static_cast<size_t>(origResp.returnValue()));
                    if (PipeManager::IsToolPipe(pipe, appId)) {
                        LOG_IPC_INFO("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} requested by tool pipe — skipping Lua disk sync", appId);
                        AppTicket::WriteAppOwnershipTicket(appId, std::vector<uint8_t>(ticketSpan.data(), ticketSpan.data() + ticketSize));
                    } else {
                        PipeManager::DenuvoAuth::SyncAppTicketToLua(appId, ticketSpan.data(), ticketSize);
                    }
                }
            }
            return;
        }

        // If Steam's genuine implementation already returned a valid ticket,
        // leave it untouched and pass through cleanly.
        if (origTicketValid) {
            if (!PipeManager::IsToolPipe(pipe, appId)) {
                PipeManager::DenuvoAuth::OnOwnershipTicketRequested(pipe, appId);
            }
            return;
        }

        if (!LuaConfig::HasDepot(appId, false)) return;

        AppTicket::AppOwnershipTicket ticket{};
        // Refresh the Denuvo authorization lease window when an ownership ticket is requested.
        if (!PipeManager::IsToolPipe(pipe, appId)) {
            PipeManager::DenuvoAuth::OnOwnershipTicketRequested(pipe, appId);
        }
        
        AppTicket::AppTicketSource ticketSource;
        if (PipeManager::DenuvoAuth::IsAuthorizedPipe(pipe) || PipeManager::DenuvoAuth::IsDenuvoPipe(pipe)) {
            // For Denuvo processes, only accept genuine tickets from in-memory cache.
            // Never fall back to local forged tickets (which carry the secondary account's
            // SteamID and cause Denuvo cross-check Error 54).
            ticketSource = AppTicket::AppTicketSource::MemoryOnly;
        } else {
            // Outside the auth window for non-Denuvo games: prefer memory-cached ticket
            // over ForgeOnly.
            LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} not in authorization window, cached ticket preferred", appId);
            ticketSource = AppTicket::AppTicketSource::MemoryThenForge;
        }        
        if (!AppTicket::GetAppOwnershipTicket(appId, ticket, ticketSource)) return;

        if (ticket.data.size() > static_cast<size_t>(req.cbMaxTicket())) {
            LOG_IPC_WARN("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} ticket too large ({} bytes) for buffer ({} bytes)",
                         appId, ticket.data.size(), req.cbMaxTicket());
            return;
        }

        const uint32 requiredCapacity = static_cast<uint32>(sizeof(uint32))           // EIPCResult
                                      + static_cast<uint32>(sizeof(uint32))           // returnValue
                                      + static_cast<uint32>(req.cbMaxTicket())        // pTicket
                                      + static_cast<uint32>(sizeof(uint32) * 4);      // piAppId, piSteamId, piSignature, pcbSignature
        if (pWrite && pWrite->m_Put < static_cast<int>(requiredCapacity)) {
            if (!Hooks_Misc::EnsureBufferCapacity(pWrite, requiredCapacity, true)) {
                return;
            }
        }

        GetAppOwnershipTicketExtendedDataResp resp{pWrite, static_cast<size_t>(req.cbMaxTicket())};
        if (!resp.ok()) return;

        if (!resp.set_pTicket(ticket.data)) return;
        resp.set_returnValue(ticket.totalSize);
        resp.set_piAppId(ticket.appIdOffset);
        resp.set_piSteamId(ticket.steamIdOffset);
        resp.set_piSignature(ticket.signatureOffset);
        resp.set_pcbSignature(ticket.signatureSize);

        LOG_IPC_DEBUG("IClientUser::GetAppOwnershipTicketExtendedData: AppId={} {}", 
                        appId,resp.DebugString());
    }

    // [Post-Handler]: IClientUser::RequestEncryptedAppTicket
    // Reads the hAsyncCall steamclient already wrote into the response,
    // so we know which AppId to mint an eticket for in GetAPICallResult.
    void HandlerPost_IClientUser_RequestEncryptedAppTicket(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        RequestEncryptedAppTicketResp resp{pWrite};
        if (!resp.ok()) return;

        const AppId_t appId = ResolvePipeOrOnlineFixAppId(pipe);
        if (appId == 0 || !LuaConfig::HasDepot(appId, false)) return;

        // Genuinely owned and family shared apps: Steam natively handles encrypted tickets.
        if (Hooks_Package::HasValidLicense(appId) && !LuaConfig::IsDAuth2(appId)) {
            std::lock_guard<std::mutex> lock(g_freshEticketMutex);
            g_freshEticket->erase(appId);
            return;
        }

        // Signal encrypted ticket requested asynchronously (binds AppId early with 0ms identity pollution).
        if (!PipeManager::IsToolPipe(pipe, appId)) {
            PipeManager::DenuvoAuth::OnEncryptedTicketRequested(pipe, appId);
        }

        bool haveFresh = false;
        // Strict Denuvo passes a per-launch nonce (pData) here and rejects a
        // stale/cached ticket (88500012). Try an on-demand mint bound to that
        // exact nonce; cache it for GetEncryptedAppTicket. Any failure falls
        // through to the static credential store below.
        {
            RequestEncryptedAppTicketReq req{pRead};
            std::span<const uint8_t> nonce;
            if (req.ok()) nonce = req.pData();
            // Whatever account the credential store's current static ticket already
            // belongs to (0 if none) — lets the backend pin the mint to that
            // SAME account instead of risking a different pool pick.
            const uint64_t existingSteamId = AppTicket::GetTicketSteamID(appId);
            // Mint a fresh eticket whenever the in-memory cache already has a ticket
            // for this app (existingSteamId != 0). The minted eticket is pinned to
            // the same pool account via existingSteamId, which matches GetSteamID's
            // spoof (also sourced from the cache via MemoryThenForge)
            // — no error-54 risk. This fixes error 05 for games launched more than
            // 30 min after activation (stored ticket expired, fresh mint is current).
            if (existingSteamId != 0) {
                if (auto fresh = EticketClient::FetchFreshEticket(appId, nonce, existingSteamId)) {
                    std::lock_guard<std::mutex> lock(g_freshEticketMutex);
                    (*g_freshEticket)[appId] = std::move(*fresh);
                    haveFresh = true;
                } else {
                    std::lock_guard<std::mutex> lock(g_freshEticketMutex);
                    g_freshEticket->erase(appId);
                }
            } else {
                std::lock_guard<std::mutex> lock(g_freshEticketMutex);
                g_freshEticket->erase(appId);
            }
        }

        if (!haveFresh && !AppTicket::HasCachedEncryptedTicket(appId)) {
            LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} - no cached eticket, skip", appId);
            return;
        }

        const SteamAPICall_t hAsyncCall = resp.returnValue();
        PendingAPICalls::RecordEncryptedTicket(hAsyncCall, appId);
        LOG_IPC_DEBUG("RequestEncryptedAppTicket: AppId={} hAsyncCall=0x{:X} - recorded (fresh={})",
                      appId, hAsyncCall, haveFresh);
    }

    // [Post-Handler]: IClientUser::GetEncryptedAppTicket
    void HandlerPost_IClientUser_GetEncryptedAppTicket(CPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        const AppId_t appId = ResolvePipeOrOnlineFixAppId(pipe);
        if (appId == 0 || !LuaConfig::HasDepot(appId, false)) return;

        // Genuinely owned and family shared apps: Steam natively handles genuine tickets.
        if (Hooks_Package::HasValidLicense(appId) && !LuaConfig::IsDAuth2(appId)) {
            std::lock_guard<std::mutex> lock(g_freshEticketMutex);
            g_freshEticket->erase(appId);
            return;
        }

        // 1. If Steam client returned a genuine encrypted ticket, pass through cleanly:
        GetEncryptedAppTicketResp existingResp{pWrite};
        if (existingResp.ok() && existingResp.returnValue()) {
            auto ticketSpan = existingResp.pTicket();
            if (!ticketSpan.empty() || existingResp.pcbTicket() > 0) {
                if (!PipeManager::IsToolPipe(pipe, appId)) {
                    PipeManager::DenuvoAuth::OnEncryptedTicketDelivered(pipe, appId);
                }
                return;
            }
        }

        // 2. Fallback to cached or freshly minted tickets (offline / unowned accounts)
        std::vector<uint8_t> ticket;
        {
            std::lock_guard<std::mutex> lock(g_freshEticketMutex);
            auto it = g_freshEticket->find(appId);
            if (it != g_freshEticket->end()) ticket = it->second;
        }
        const bool fromFresh = !ticket.empty();
        if (ticket.empty()) {
            ticket = AppTicket::GetCachedEncryptedTicket(appId);
        }
        if (ticket.empty() || ticket.size() > static_cast<size_t>(INT32_MAX - 16)) {
            LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - no valid cached eticket, skip", appId);
            return;
        }
        LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} serving source={}", appId, fromFresh ? "fresh" : "store");

        uint32 ticketSize = static_cast<uint32>(ticket.size());
        const uint32 requiredCapacity = static_cast<uint32>(sizeof(uint32))   // EIPCResult
                                      + static_cast<uint32>(sizeof(bool))     // returnValue
                                      + static_cast<uint32>(sizeof(uint32))   // pcbTicket
                                      + ticketSize;                           // pTicket
        if (pWrite && pWrite->m_Put < static_cast<int>(requiredCapacity)) {
            if (!Hooks_Misc::EnsureBufferCapacity(pWrite, requiredCapacity, true)) {
                LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} - failed to ensure buffer size", appId);
                return;
            }
        }

        GetEncryptedAppTicketResp resp{pWrite};
        if (!resp.ok()) return;

        if (!resp.set_pTicket(ticket)) {
            LOG_IPC_WARN("GetEncryptedAppTicket: AppId={} failed to write ticket payload (ticket_bytes={})",
                         appId, ticket.size());
            return;
        }
        resp.set_returnValue(true);

        // Arms the activation delivery pulse (2500ms) upon successful ticket delivery:
        if (!PipeManager::IsToolPipe(pipe, appId)) {
            PipeManager::DenuvoAuth::OnEncryptedTicketDelivered(pipe, appId);
        }

        LOG_IPC_DEBUG("GetEncryptedAppTicket: AppId={} {}", appId, resp.DebugString());
    }

} // namespace

namespace Hooks_IPC_ISteamUser {
    void Register() {
        constexpr IPCHandlerEntry UserEntries[] = {
            ADD_IPC_POST_HANDLER(IClientUser, GetSteamID),
            ADD_IPC_POST_HANDLER(IClientUser, GetAppOwnershipTicketExtendedData),
            ADD_IPC_POST_HANDLER(IClientUser, RequestEncryptedAppTicket),
            ADD_IPC_POST_HANDLER(IClientUser, GetEncryptedAppTicket),
        };
        Hooks_IPC::RegisterHandlers(UserEntries);
    }
}
