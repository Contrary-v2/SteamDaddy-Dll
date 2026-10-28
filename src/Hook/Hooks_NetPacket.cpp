#include "Hooks_NetPacket.h"
#include "Hooks_Decryption.h"
#include "Hooks_Package.h"
#include "Utils/SteamMetadata/ManifestClient.h"
#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Tickets/LegacyCDKey.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Tickets/EticketClient.h"
#include "Pipe/PipeManager.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/Config/LuaConfig.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <excpt.h>
#endif

#include "steam_messages.pb.h"

// ════════════════════════════════════════════════════════════════
//  Shared infrastructure
// ════════════════════════════════════════════════════════════════
namespace {

    constexpr uint32 kMaxBodySize    = 65536;
    constexpr uint32 kMaxHdrSize     = 1024;
    constexpr uint32 kMaxPacketSize  = sizeof(MsgHdr) + kMaxHdrSize + kMaxBodySize;
    constexpr size_t kPacketPoolSize = 32;
    constexpr uint64 kInvalidJobId   = (std::numeric_limits<uint64>::max)();

    // ── Trivially destructible wrapper for process-lifetime globals ──
    // Prevents CRT atexit static destruction crashes during ExitProcess
    // when secondary threads are terminated mid-operation by the OS.
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

    // ── Deterministic FIFO eviction helper for bounded maps ──────────
    template <typename Map>
    void EvictOldestBySeq(Map& map) noexcept {
        if (map.empty()) return;
        auto oldest = std::ranges::min_element(map, [](const auto& a, const auto& b) noexcept {
            return a.second.seq < b.second.seq;
        });
        map.erase(oldest);
    }

    // ── High-performance, cacheline-aligned ring buffer pool ─────────
    template <size_t PoolSize, size_t SlotSize>
    class PacketBufferPool {
        static_assert((PoolSize & (PoolSize - 1)) == 0, "PoolSize must be power of 2");
        alignas(64) uint8 m_storage[PoolSize][SlotSize]{};
        std::atomic<uint32> m_index{0};

    public:
        [[nodiscard]] std::span<uint8> AcquireSlot() noexcept {
            const uint32 idx = m_index.fetch_add(1, std::memory_order_relaxed) & static_cast<uint32>(PoolSize - 1);
            return std::span<uint8>(m_storage[idx], SlotSize);
        }
    };

    static PacketBufferPool<kPacketPoolSize, kMaxPacketSize> g_RecvPool;
    static PacketBufferPool<kPacketPoolSize, kMaxPacketSize> g_SendPool;

    // ── RAII Carrier Scope Guard (Guarantees no-fail restoration) ───
    class [[nodiscard]] CarrierScopeGuard final {
        CNetPacket* const m_carrier;
        const uint32      m_dataOff;
        const uint32      m_sizeOff;
        uint8* const      m_origPubData;
        const uint32      m_origCubData;

    public:
        CarrierScopeGuard(CNetPacket* carrier, uint8* borrowedData, uint32 borrowedSize) noexcept
            : m_carrier(carrier),
              m_dataOff(NetPacketLayout::s_dataOffset.load(std::memory_order_relaxed)),
              m_sizeOff(NetPacketLayout::s_sizeOffset.load(std::memory_order_relaxed)),
              m_origPubData(carrier ? carrier->PubData(m_dataOff) : nullptr),
              m_origCubData(carrier ? carrier->CubData(m_sizeOff) : 0)
        {
            if (carrier) {
                carrier->SetPubData(borrowedData, m_dataOff);
                carrier->SetCubData(borrowedSize, m_sizeOff);
            }
        }

        ~CarrierScopeGuard() noexcept {
            if (m_carrier) {
                m_carrier->SetPubData(m_origPubData, m_dataOff);
                m_carrier->SetCubData(m_origCubData, m_sizeOff);
            }
        }

        CarrierScopeGuard(const CarrierScopeGuard&) = delete;
        CarrierScopeGuard& operator=(const CarrierScopeGuard&) = delete;
        CarrierScopeGuard(CarrierScopeGuard&&) = delete;
        CarrierScopeGuard& operator=(CarrierScopeGuard&&) = delete;
    };

    // ── Shutdown & Active Hook Call Drain Tracking ──────────────────
    std::atomic<bool>   g_ShuttingDown{false};
    std::atomic<uint32> g_ActiveHookCalls{0};

    class [[nodiscard]] HookScopeGuard final {
        bool m_entered = false;

    public:
        HookScopeGuard() noexcept {
            if (g_ShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            g_ActiveHookCalls.fetch_add(1, std::memory_order_acq_rel);
            if (g_ShuttingDown.load(std::memory_order_acquire)) {
                g_ActiveHookCalls.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            m_entered = true;
        }

        ~HookScopeGuard() noexcept {
            if (m_entered) {
                g_ActiveHookCalls.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        [[nodiscard]] bool Entered() const noexcept { return m_entered; }

        HookScopeGuard(const HookScopeGuard&) = delete;
        HookScopeGuard& operator=(const HookScopeGuard&) = delete;
        HookScopeGuard(HookScopeGuard&&) = delete;
        HookScopeGuard& operator=(HookScopeGuard&&) = delete;
    };

    // ── EMsg -> name lookup ──────────────────────────────────────────
    RESOLVE_FUNC(PchMsgNameFromEMsg, char*, EMsg eMsg);
    [[nodiscard]] inline const char* MsgName(EMsg eMsg) noexcept {
        if (oPchMsgNameFromEMsg) return oPchMsgNameFromEMsg(eMsg);
        return "?";
    }

    // ── Packet layout & zero-copy view unpacking ────────────────────
    struct UnpackedPacket {
        MsgHdr                 rawHdr{};
        EMsg                   eMsg = static_cast<EMsg>(0);
        std::span<const uint8> hdr;
        std::span<const uint8> body;
    };

    [[nodiscard]] inline std::optional<UnpackedPacket> UnpackRaw(const uint8* data, uint32 size) noexcept {
        if (!data || size < sizeof(MsgHdr) || size > static_cast<uint32>((std::numeric_limits<int>::max)())) {
            return std::nullopt;
        }

        MsgHdr rawHdr{};
        std::memcpy(&rawHdr, data, sizeof(MsgHdr));
        if (!(rawHdr.eMsg & kMsgHdrProtoFlag)) {
            return std::nullopt;
        }

        const uint32 cbHdr = rawHdr.headerLength;
        if (cbHdr > size - static_cast<uint32>(sizeof(MsgHdr))) {
            return std::nullopt;
        }

        const uint32 off = static_cast<uint32>(sizeof(MsgHdr)) + cbHdr;
        return UnpackedPacket{
            .rawHdr = rawHdr,
            .eMsg   = static_cast<EMsg>(rawHdr.eMsg & ~kMsgHdrProtoFlag),
            .hdr    = std::span<const uint8>(data + sizeof(MsgHdr), cbHdr),
            .body   = std::span<const uint8>(data + off, size - off)
        };
    }

#if defined(_MSC_VER)
    __declspec(noinline) static bool SafeProbeMemory(const void* ptr, size_t size) noexcept {
        if (!ptr || size == 0) return false;
        __try {
            volatile const char* p = reinterpret_cast<volatile const char*>(ptr);
            [[maybe_unused]] volatile char v = p[0];
            constexpr size_t kPageSize = 4096;
            for (size_t offset = kPageSize; offset < size; offset += kPageSize) {
                v = p[offset];
            }
            v = p[size - 1];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
#else
    static bool SafeProbeMemory(const void* ptr, size_t size) noexcept {
        return ptr != nullptr && size > 0;
    }
#endif

    void ProbeNetPacketLayout(CNetPacket* pPacket) noexcept {
        if (!pPacket) return;

        static std::mutex s_probeMutex;
        std::unique_lock lock(s_probeMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }

        if (NetPacketLayout::s_resolved.load(std::memory_order_relaxed)) {
            return;
        }

        struct Candidate {
            uint32 dataOff;
            uint32 sizeOff;
        };

        constexpr Candidate kCandidates[] = {
            { 16, 24 },
            { 8,  16 },
            { 24, 32 },
            { 32, 40 },
            { 40, 48 },
        };

        for (const auto& cand : kCandidates) {
            if (!SafeProbeMemory(pPacket, cand.sizeOff + sizeof(uint32))) {
                continue;
            }

            const uint8* pubData = *reinterpret_cast<uint8* const*>(
                reinterpret_cast<const uint8*>(pPacket) + cand.dataOff);
            const uint32 cubData = *reinterpret_cast<const uint32*>(
                reinterpret_cast<const uint8*>(pPacket) + cand.sizeOff);

            const auto addr = reinterpret_cast<uintptr_t>(pubData);
            if (addr < 0x10000 || addr >= 0x7FFFFFFEFFFFULL) {
                continue;
            }

            if (cubData < sizeof(MsgHdr) || cubData > kMaxPacketSize) {
                continue;
            }

            if (!SafeProbeMemory(pubData, cubData)) {
                continue;
            }

            const auto pkt = UnpackRaw(pubData, cubData);
            if (!pkt) {
                continue;
            }

            const uint32 cleanEMsg = static_cast<uint32>(pkt->eMsg);
            if (cleanEMsg == 0 || cleanEMsg >= 20000) {
                continue;
            }

            if (pkt->hdr.empty() || pkt->hdr.size() > kMaxHdrSize) {
                continue;
            }

            // Confirmed candidate verified with valid proto packet
            NetPacketLayout::s_dataOffset.store(cand.dataOff, std::memory_order_relaxed);
            NetPacketLayout::s_sizeOffset.store(cand.sizeOff, std::memory_order_relaxed);
            NetPacketLayout::s_resolved.store(true, std::memory_order_release);
            LOG_NETPACKET_INFO("CNetPacket dynamic layout resolved: dataOff={}, sizeOff={}",
                              cand.dataOff, cand.sizeOff);
            break;
        }
    }

    // ── Direct-to-slot transactional packet commit helpers ──────────
    [[nodiscard]] bool CommitSendPacket(const UnpackedPacket& pkt,
                                        const google::protobuf::MessageLite& newBodyMsg,
                                        std::span<uint8>& outFrame)
    {
        const size_t bodySizeLong = newBodyMsg.ByteSizeLong();
        if (pkt.hdr.size() > kMaxHdrSize || bodySizeLong > kMaxBodySize) {
            LOG_NETPACKET_WARN("CommitSendPacket: size overflow (hdr={}, body={})", pkt.hdr.size(), bodySizeLong);
            return false;
        }

        const uint32 cbHdr     = static_cast<uint32>(pkt.hdr.size());
        const uint32 cbBody    = static_cast<uint32>(bodySizeLong);
        const uint32 totalSize = static_cast<uint32>(sizeof(MsgHdr)) + cbHdr + cbBody;

        std::span<uint8> slot = g_SendPool.AcquireSlot();
        uint8* const bodyDst  = slot.data() + sizeof(MsgHdr) + cbHdr;
        if (cbBody > 0 && !newBodyMsg.SerializeToArray(bodyDst, static_cast<int>(cbBody))) {
            LOG_NETPACKET_WARN("CommitSendPacket: SerializeToArray failed (cbBody={})", cbBody);
            return false;
        }

        auto* const outMsgHdr = reinterpret_cast<MsgHdr*>(slot.data());
        outMsgHdr->eMsg = pkt.rawHdr.eMsg;
        outMsgHdr->headerLength = cbHdr;
        if (cbHdr > 0) {
            std::memcpy(slot.data() + sizeof(MsgHdr), pkt.hdr.data(), cbHdr);
        }

        outFrame = slot.subspan(0, totalSize);
        return true;
    }

    [[nodiscard]] bool CommitRecvPacket(CNetPacket* pPacket,
                                        const UnpackedPacket& pkt,
                                        const google::protobuf::MessageLite* newHdrMsg,
                                        const google::protobuf::MessageLite* newBodyMsg,
                                        bool clearBody = false)
    {
        if (!pPacket) return false;

        const size_t hdrSizeLong  = newHdrMsg ? newHdrMsg->ByteSizeLong() : pkt.hdr.size();
        const size_t bodySizeLong = clearBody ? 0u
                                  : (newBodyMsg ? newBodyMsg->ByteSizeLong() : pkt.body.size());

        if (hdrSizeLong > kMaxHdrSize || bodySizeLong > kMaxBodySize) {
            LOG_NETPACKET_WARN("CommitRecvPacket: size overflow (hdr={}, body={})", hdrSizeLong, bodySizeLong);
            return false;
        }

        const uint32 finalCbHdr  = static_cast<uint32>(hdrSizeLong);
        const uint32 finalCbBody = static_cast<uint32>(bodySizeLong);
        const uint32 totalSize   = static_cast<uint32>(sizeof(MsgHdr)) + finalCbHdr + finalCbBody;

        std::span<uint8> slot = g_RecvPool.AcquireSlot();

        uint8* const hdrDst = slot.data() + sizeof(MsgHdr);
        if (newHdrMsg) {
            if (finalCbHdr > 0 && !newHdrMsg->SerializeToArray(hdrDst, static_cast<int>(finalCbHdr))) {
                LOG_NETPACKET_WARN("CommitRecvPacket: failed to serialize header (cbHdr={})", finalCbHdr);
                return false;
            }
        } else if (finalCbHdr > 0) {
            std::memcpy(hdrDst, pkt.hdr.data(), finalCbHdr);
        }

        uint8* const bodyDst = hdrDst + finalCbHdr;
        if (clearBody) {
            // 0-byte body payload
        } else if (newBodyMsg) {
            if (finalCbBody > 0 && !newBodyMsg->SerializeToArray(bodyDst, static_cast<int>(finalCbBody))) {
                LOG_NETPACKET_WARN("CommitRecvPacket: failed to serialize body (cbBody={})", finalCbBody);
                return false;
            }
        } else if (finalCbBody > 0) {
            std::memcpy(bodyDst, pkt.body.data(), finalCbBody);
        }

        auto* const outMsgHdr = reinterpret_cast<MsgHdr*>(slot.data());
        outMsgHdr->eMsg = pkt.rawHdr.eMsg;
        outMsgHdr->headerLength = finalCbHdr;

        pPacket->SetPubData(slot.data());
        pPacket->SetCubData(totalSize);
        return true;
    }

    // ── Domain AppId extraction & OnlineFix mapping helpers ─────────
    [[nodiscard]] constexpr AppId_t ExtractSteamAppId(uint64 rawGameId) noexcept {
        CGameID gid{};
        gid.m_ulGameID = rawGameId;
        return gid.AppID(true);
    }

    [[nodiscard]] inline AppId_t ResolveOnlineFixAppId(AppId_t appId) noexcept {
        if (appId == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
            const AppId_t resolved = Hooks_Misc::ResolveAppId();
            if (resolved != 0 && resolved != kOnlineFixAppId) {
                return resolved;
            }
        }
        return appId;
    }

    // ── Hash constants for target_job_name dispatch ─────────────────
    constexpr uint32 HASH_JOB_NotifyRunningApps      = Fnv1aHash("FamilyGroupsClient.NotifyRunningApps#1");
    constexpr uint32 HASH_JOB_GetUserStats           = Fnv1aHash("Player.GetUserStats#1");
    constexpr uint32 HASH_JOB_ClientGetLastPlayedTimes = Fnv1aHash("Player.ClientGetLastPlayedTimes#1");
    constexpr uint32 HASH_JOB_GetManifestRequestCode = Fnv1aHash("ContentServerDirectory.GetManifestRequestCode#1");

} // anonymous namespace


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_AccessToken
//
//  Outgoing: CMsgClientPICSProductInfoRequest (eMsg 8903)
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_AccessToken {

    [[nodiscard]] bool HandleSend(const UnpackedPacket& pkt, std::span<uint8>& outFrame)
    {
        if (pkt.body.empty()) return false;

        CMsgClientPICSProductInfoRequest req;
        if (!req.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
            LOG_PICS_WARN("Failed to ParseFromArray CMsgClientPICSProductInfoRequest");
            return false;
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest original body:\n{}", req.DebugString());

        int injected = 0, noToken = 0, notAddAppId = 0;
        for (auto& app : *req.mutable_apps()) {
            const AppId_t appId = app.appid();
            if (LuaConfig::HasDepot(appId) && !Hooks_Package::HasValidLicense(appId)) {
                const uint64_t token = LuaConfig::GetAccessToken(appId);
                if (token != 0) {
                    if (app.has_access_token() && app.access_token() == token) {
                        continue;
                    }
                    LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: inject appid={}: {} -> {}", appId,
                               app.has_access_token() ? std::to_string(app.access_token()) : "absent",
                               token);
                    app.set_access_token(token);
                    ++injected;
                } else {
                    LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: skip appid={}: in depot, no token configured", appId);
                    ++noToken;
                }
            } else {
                ++notAddAppId;
            }
        }

        if (injected == 0) {
            LOG_PICS_TRACE("CMsgClientPICSProductInfoRequest: no apps need token injection, skip");
            return false;
        }

        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: injected={} no_token={} not_in_add_appid={} total={}",
                   injected, noToken, notAddAppId, req.apps_size());

        if (!CommitSendPacket(pkt, req, outFrame)) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: Failed to commit modified request");
            return false;
        }

        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: modified body: {}", req.DebugString());
        return true;
    }

} // namespace Hooks_NetPacket_AccessToken


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_UserStats
//
//  Outgoing: CPlayer_GetUserStats_Request  (eMsg 151 -> target: Player.GetUserStats#1)
//            CMsgClientGetUserStats        (eMsg 818)
//  Incoming: CPlayer_GetUserStats_Response (eMsg 147 ← target: Player.GetUserStats#1)
//            CMsgClientGetUserStatsResponse(eMsg 819)
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_UserStats {

    struct PendingStatsJob {
        AppId_t     appId = 0;
        std::string shaSchema;
        uint64      seq = 0;
    };

    // jobid_source -> PendingStatsJob mapping (eMsg 151 request -> eMsg 147 response)
    std::mutex g_JobMutex;
    uint64     g_JobSeq = 0;
    NoDestructor<std::unordered_map<uint64, PendingStatsJob>> g_JobIdToPendingStats;

    // ── Send: CPlayer_GetUserStats_Request (eMsg 151) ──────────
    [[nodiscard]] bool HandleSend_GetUserStats(const UnpackedPacket& pkt,
                                               const CMsgProtoBufHeader& hdr,
                                               std::span<uint8>& outFrame)
    {
        if (pkt.body.empty()) return false;

        CPlayer_GetUserStats_Request req;
        if (!req.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to ParseFromArray");
            return false;
        }
        if (!req.has_appid()) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: missing appid");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: original body:\n{}", req.DebugString());

        const AppId_t origAppId = req.appid();
        const AppId_t appId     = ResolveOnlineFixAppId(origAppId);
        const bool isOnlineFix  = (appId != origAppId);
        if (isOnlineFix) {
            req.set_appid(appId);
        }

        // Truly owned and family shared games must pass through to official servers untouched
        if (Hooks_Package::HasValidLicense(appId)) {
            return isOnlineFix ? CommitSendPacket(pkt, req, outFrame) : false;
        }

        // Only manage apps that are in LuaConfig depot or CloudRedirect
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: appid={} is not managed", appId);
            return false;
        }

        const bool hasShaSchema = req.has_sha_schema() && !req.sha_schema().empty();
        const std::string shaSchema = hasShaSchema ? req.sha_schema() : "";

        // Save jobid_source -> PendingStatsJob for the response handler
        if (hdr.has_jobid_source()) {
            const uint64 jobId = hdr.jobid_source();
            if (jobId != 0 && jobId != kInvalidJobId) {
                std::lock_guard<std::mutex> lock(g_JobMutex);
                if (!g_JobIdToPendingStats->contains(jobId) && g_JobIdToPendingStats->size() >= 256) {
                    EvictOldestBySeq(*g_JobIdToPendingStats);
                }
                (*g_JobIdToPendingStats)[jobId] = PendingStatsJob{
                    .appId     = appId,
                    .shaSchema = shaSchema,
                    .seq       = ++g_JobSeq
                };
                LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: stored jobid={} -> appid={}, sha_len={}",
                    jobId, appId, shaSchema.size());
            }
        }

        // When sha_schema is present, the client already has the schema definitions cached locally.
        // DO NOT spoof steamid! Spoofing steamid causes Valve to return AccessDenied with an empty body,
        // which strips the schema and causes achievements to disappear entirely in the UI.
        if (hasShaSchema) {
            if (isOnlineFix) {
                return CommitSendPacket(pkt, req, outFrame);
            }
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: sha_schema is present, preserve client request");
            return false;
        }

        // First time retrieval without sha_schema: borrow donor steamid to fetch full schema from Valve
        const uint64_t newSteamId = LuaConfig::GetStatSteamId(appId);
        if (newSteamId != 0) {
            req.set_steamid(newSteamId);
        } else if (!isOnlineFix) {
            return false;
        }

        if (!CommitSendPacket(pkt, req, outFrame)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to encode");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CPlayer_GetUserStats_Response (eMsg 147) ─────────
    //     Header: set eresult=OK. Body: strip donor stats, overlay CR achievements.
    void HandleRecv_GetUserStatsResponse(CNetPacket* pPacket,
                                         const UnpackedPacket& pkt,
                                         const CMsgProtoBufHeader& inHdr)
    {
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original header:\n{}", inHdr.DebugString());

        // Look up pending stats info via jobid_target -> jobid_source match
        PendingStatsJob pending;
        bool hasPending = false;
        if (inHdr.has_jobid_target()) {
            const uint64 jobId = inHdr.jobid_target();
            if (jobId != 0 && jobId != kInvalidJobId) {
                std::lock_guard<std::mutex> lock(g_JobMutex);
                auto it = g_JobIdToPendingStats->find(jobId);
                if (it != g_JobIdToPendingStats->end()) {
                    pending = std::move(it->second);
                    hasPending = true;
                    LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: matched jobid={} -> appid={}", jobId, pending.appId);
                    g_JobIdToPendingStats->erase(it);
                }
            }
        }

        // Truly owned / family shared games and unmanaged games must pass untouched
        if (!hasPending || Hooks_Package::HasValidLicense(pending.appId) ||
            (!LuaConfig::HasDepot(pending.appId) && !CloudRedirectHost::IsApp(pending.appId))) {
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: unmanaged appid {} (or no match), skip", pending.appId);
            return;
        }

        const AppId_t appId = pending.appId;

        CPlayer_GetUserStats_Response resp;
        if (!pkt.body.empty()) {
            if (!resp.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
                LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to ParseFromArray original response");
                resp.Clear();
            }
        }
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original body:\n{}", resp.DebugString());

        // Protect sha_schema: if Valve's response lacks sha_schema (e.g. AccessDenied empty body),
        // restore the sha_schema that the client originally requested so local schema cache remains active!
        if ((!resp.has_sha_schema() || resp.sha_schema().empty()) && !pending.shaSchema.empty()) {
            resp.set_sha_schema(pending.shaSchema);
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: restored sha_schema (len={}) for app {}",
                pending.shaSchema.size(), appId);
        }

        resp.clear_stats();

        // Inject CloudRedirect achievements
        CloudRedirectHost::AchievementBlock blocks[64]{};
        const uint32_t n = (std::min)(CloudRedirectHost::GetAchievements(appId, std::span(blocks)), 64u);
        if (n > 0) {
            uint32_t crc = 0;
            for (uint32_t i = 0; i < n; i++) {
                auto* s = resp.add_stats();
                s->set_stat_id(blocks[i].statId);
                s->set_stat_value(blocks[i].bits);
                for (uint32_t bit = 0; bit < 32; bit++) {
                    if ((blocks[i].bits & (1u << bit)) != 0) {
                        auto* ut = s->add_unlock_times();
                        ut->set_achievement_bit(bit);
                        ut->set_unlock_time(blocks[i].unlockTimes[bit]);
                    }
                }
                crc ^= blocks[i].bits ^ blocks[i].statId;
            }
            resp.set_crc_stats(crc);
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: injected {} CR achievement blocks for app {}", n, appId);
        } else {
            resp.clear_crc_stats();
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: cleared stats (no CR achievements for app {})", appId);
        }

        CMsgProtoBufHeader hdrMsg = inHdr;
        hdrMsg.set_eresult(static_cast<int32_t>(k_EResultOK));
        if (hdrMsg.has_error_message()) {
            hdrMsg.clear_error_message();
        }

        if (CommitRecvPacket(pPacket, pkt, &hdrMsg, &resp)) {
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: successfully committed modified header and body");
        }
    }

    // ── Send: CMsgClientGetUserStats (eMsg 818) ────────────────
    [[nodiscard]] bool HandleSend_ClientGetUserStats(const UnpackedPacket& pkt,
                                                     std::span<uint8>& outFrame)
    {
        if (pkt.body.empty()) return false;

        CMsgClientGetUserStats req;
        if (!req.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to ParseFromArray");
            return false;
        }
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: original body:\n{}", req.DebugString());

        if (!req.has_game_id()) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: missing game_id");
            return false;
        }
        const AppId_t origAppId = ExtractSteamAppId(req.game_id());
        const AppId_t appId     = ResolveOnlineFixAppId(origAppId);
        const bool isOnlineFix  = (appId != origAppId);
        if (isOnlineFix) {
            CGameID gid{};
            gid.m_ulGameID = req.game_id();
            gid.SetAppID(appId);
            req.set_game_id(gid.m_ulGameID);
        }
        if (Hooks_Package::HasValidLicense(appId)) {
            return isOnlineFix ? CommitSendPacket(pkt, req, outFrame) : false;
        }
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: appid={} is not managed", appId);
            return false;
        }
        if (req.schema_local_version() != -1) {
            req.set_schema_local_version(-1);
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: forced schema_local_version to -1");
        }

        const uint64_t newSteamId = LuaConfig::GetStatSteamId(appId);
        if (newSteamId != 0) {
            req.set_steam_id_for_user(newSteamId);
        }

        if (!CommitSendPacket(pkt, req, outFrame)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to CommitSendPacket");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CMsgClientGetUserStatsResponse (eMsg 819) ────────
    //     Clear donor stats, overlay CR achievements, patch eresult->OK.
    void HandleRecv_ClientGetUserStatsResponse(CNetPacket* pPacket,
                                               const UnpackedPacket& pkt)
    {
        if (pkt.body.empty()) return;

        CMsgClientGetUserStatsResponse resp;
        if (!resp.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size())))
            return;
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: original body:\n{}", resp.DebugString());
        if (!resp.has_game_id()) {
            return;
        }

        const AppId_t appId = ResolveOnlineFixAppId(ExtractSteamAppId(resp.game_id()));
        if (Hooks_Package::HasValidLicense(appId)) {
            return;
        }
        if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: appid={} not managed, skip", appId);
            return;
        }

        resp.clear_stats();
        resp.clear_achievement_blocks();
        resp.set_eresult(k_EResultOK);

        // Overlay CR's cloud-synced achievement state
        CloudRedirectHost::AchievementBlock blocks[64]{};
        const uint32_t n = (std::min)(CloudRedirectHost::GetAchievements(appId, std::span(blocks)), 64u);
        if (n > 0) {
            uint32_t crc = 0;
            for (uint32_t i = 0; i < n; i++) {
                auto* s = resp.add_stats();
                s->set_stat_id(blocks[i].statId);
                s->set_stat_value(blocks[i].bits);
                auto* ab = resp.add_achievement_blocks();
                ab->set_achievement_id(blocks[i].statId);
                for (uint32_t bit = 0; bit < 32; bit++)
                    ab->add_unlock_time(blocks[i].unlockTimes[bit]);
                crc ^= blocks[i].bits ^ blocks[i].statId;
            }
            resp.set_crc_stats(crc);
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: injected {} CR achievement blocks for app {}", n, appId);
        } else {
            resp.clear_crc_stats();
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: cleared stats/achievements (no CR data for app {})", appId);
        }

        CMsgProtoBufHeader hdrMsg;
        bool hdrChanged = false;
        if (!pkt.hdr.empty() &&
            hdrMsg.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())) &&
            hdrMsg.has_eresult() && hdrMsg.eresult() != k_EResultOK)
        {
            hdrMsg.set_eresult(k_EResultOK);
            if (hdrMsg.has_error_message()) {
                hdrMsg.clear_error_message();
            }
            hdrChanged = true;
        }

        if (CommitRecvPacket(pPacket, pkt, hdrChanged ? &hdrMsg : nullptr, &resp)) {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: modified body:\n{}", resp.DebugString());
        }
    }

    // ── Recv: CMsgClientStoreUserStatsResponse (eMsg 821) ────────
    void HandleRecv_ClientStoreUserStatsResponse(CNetPacket* pPacket,
                                                 const UnpackedPacket& pkt)
    {
        try {
            if (pkt.body.empty()) return;

            CMsgClientStoreUserStatsResponse resp;
            if (!resp.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size())))
                return;
            LOG_ACHIEVEMENT_DEBUG("ClientStoreUserStats response: original body:\n{}", resp.DebugString());

            if (!resp.has_game_id())
                return;

            const AppId_t appId = ResolveOnlineFixAppId(ExtractSteamAppId(resp.game_id()));

            if (!LuaConfig::HasDepot(appId) && !CloudRedirectHost::IsApp(appId)) {
                return;
            }
            if (Hooks_Package::HasValidLicense(appId)) {
                return;
            }

            CMsgProtoBufHeader hdrMsg;
            bool hdrChanged = false;
            if (!pkt.hdr.empty() &&
                hdrMsg.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())) &&
                hdrMsg.has_eresult() && hdrMsg.eresult() != k_EResultOK)
            {
                hdrMsg.set_eresult(k_EResultOK);
                if (hdrMsg.has_error_message()) {
                    hdrMsg.clear_error_message();
                }
                hdrChanged = true;
                LOG_ACHIEVEMENT_DEBUG("ClientStoreUserStats response: patched header eresult to OK for app {}", appId);
            }

            bool bodyChanged = false;
            if (resp.eresult() != k_EResultOK) {
                resp.set_eresult(k_EResultOK);
                bodyChanged = true;
            }
            if (resp.has_stats_failed_validation()) {
                resp.clear_stats_failed_validation();
                bodyChanged = true;
            }

            if (hdrChanged || bodyChanged) {
                if (CommitRecvPacket(pPacket, pkt, hdrChanged ? &hdrMsg : nullptr, bodyChanged ? &resp : nullptr)) {
                    LOG_ACHIEVEMENT_DEBUG("ClientStoreUserStats response: committed patched response for app {}", appId);
                }
            }

            // Dual-stage notification: confirm stats store off-thread (coalesces with 820 send)
            CloudRedirectHost::NotifyStatsStored(appId);
        } catch (const std::exception& e) {
            LOG_ACHIEVEMENT_ERROR("ClientStoreUserStats response caught exception: {}", e.what());
        } catch (...) {
            LOG_ACHIEVEMENT_ERROR("ClientStoreUserStats response caught unknown exception");
        }
    }

} // namespace Hooks_NetPacket_UserStats

// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Playtime
//
//  Incoming: CPlayer_GetLastPlayedTimes_Response (eMsg 147 ← target: Player.ClientGetLastPlayedTimes#1)
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Playtime {

    void HandleRecv_ClientGetLastPlayedTimesResponse(CNetPacket* pPacket,
                                                     const UnpackedPacket& pkt)
    {
        if (!CloudRedirectHost::IsActive()) return;

        CPlayer_GetLastPlayedTimes_Response resp;
        if (!pkt.body.empty()) {
            if (!resp.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
                LOG_NETPACKET_WARN("Player::ClientGetLastPlayedTimes response: failed to ParseFromArray");
                return;
            }
        }

        // Map existing games by appid for in-place update or deduplication
        std::unordered_map<int32_t, CPlayer_GetLastPlayedTimes_Response::Game*> existing;
        if (resp.games_size() > 0) {
            existing.reserve(static_cast<size_t>(resp.games_size()));
            for (int i = 0; i < resp.games_size(); ++i) {
                existing[resp.games(i).appid()] = resp.mutable_games(i);
            }
        }

        bool modified = false;
        const auto appIds = CloudRedirectHost::GetRedirectedAppIds();
        for (const AppId_t appId : appIds) {
            // Strictly protect officially owned and family shared assets
            if (Hooks_Package::HasValidLicense(appId)) {
                continue;
            }

            CloudRedirectHost::PlaytimeInfo pt{};
            if (!CloudRedirectHost::GetPlaytime(appId, &pt)) {
                continue;
            }
            if (pt.minutesForever == 0 && pt.lastPlayedTime == 0) {
                continue;
            }

            auto it = existing.find(static_cast<int32_t>(appId));
            auto* g = (it != existing.end()) ? it->second : resp.add_games();
            g->set_appid(static_cast<int32_t>(appId));
            g->set_playtime_forever(static_cast<int32_t>(pt.minutesForever));
            g->set_playtime_2weeks(static_cast<int32_t>(pt.minutesLastTwoWeeks));
            g->set_last_playtime(pt.lastPlayedTime);
            if (pt.playtimeWindows) g->set_playtime_windows_forever(static_cast<int32_t>(pt.playtimeWindows));
            if (pt.playtimeMac)     g->set_playtime_mac_forever(static_cast<int32_t>(pt.playtimeMac));
            if (pt.playtimeLinux)   g->set_playtime_linux_forever(static_cast<int32_t>(pt.playtimeLinux));

            modified = true;
            LOG_NETPACKET_DEBUG("Player::ClientGetLastPlayedTimes response: merged appid={} (forever={}m, last={})",
                                appId, pt.minutesForever, pt.lastPlayedTime);
        }

        if (modified) {
            if (CommitRecvPacket(pPacket, pkt, nullptr, &resp)) {
                LOG_NETPACKET_INFO("Player::ClientGetLastPlayedTimes response: successfully committed merged playtime data");
            }
        }
    }

} // namespace Hooks_NetPacket_Playtime




// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_OwnershipTicket
//
//  Incoming: MsgClientGetAppOwnershipTicketResponse (eMsg 858).
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_OwnershipTicket {

    void HandleRecv(CNetPacket* pPacket, const UnpackedPacket& pkt)
    {
        if (pkt.body.empty()) return;

        CMsgClientGetAppOwnershipTicketResponse resp;
        if (!resp.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
            LOG_NETPACKET_WARN("OwnershipTicketResponse[858]: failed to ParseFromArray (cbBody={})", pkt.body.size());
            return;
        }
        if (!resp.has_app_id() || resp.app_id() == 0) {
            return;
        }

        const AppId_t appId = resp.app_id();

        // Steam already returned a valid ticket (account owns it) — sync to Lua if present.
        if (resp.eresult() == k_EResultOK) {
            if (resp.has_ticket() && !resp.ticket().empty()) {
                const auto& t = resp.ticket();
                const auto span = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(t.data()), t.size());
                // Passively cache into in-memory ticket store for forge fallback
                Hooks_Decryption::CacheAppOwnershipTicket(appId, span.data(), span.size());
                if (PipeManager::IsToolActiveForApp(appId)) {
                    LOG_NETPACKET_INFO("OwnershipTicketResponse[858]: appId={} requested by active tool — skipping Lua disk sync", appId);
                    AppTicket::WriteAppOwnershipTicket(appId, span);
                } else {
                    PipeManager::DenuvoAuth::SyncAppTicketToLua(appId, span.data(), span.size());
                }
            }
            return;
        }

        // INV-01 & INV-02: Pass through owned / family-shared games and unmanaged games
        if (!LuaConfig::HasDepot(appId) || Hooks_Package::HasValidLicense(appId)) {
            return;
        }

        const int32 origEresult = resp.eresult();

        // Prefer the cached ticket when it is already valid: that
        // ensures GetAppOwnershipTicketExtendedData and the 858 response hand
        // Denuvo the identical bytes.
        auto stored = AppTicket::GetCachedAppOwnershipTicket(appId);
        const uint64_t existingSteamId = AppTicket::ExtractSteamIdFromTicketBytes(stored);

        std::vector<uint8_t> ticketBytes;
        if (existingSteamId != 0) {
            ticketBytes = std::move(stored);
        } else {
            auto minted = EticketClient::FetchOwnershipTicket(appId, {}, 0);
            if (!minted || AppTicket::ExtractSteamIdFromTicketBytes(*minted) == 0) {
                LOG_NETPACKET_WARN("OwnershipTicketResponse[858]: appid={} eresult={} but no valid owner ticket available",
                                   appId, origEresult);
                return;
            }
            ticketBytes = std::move(*minted);
            // Write newly minted ticket back to single-source in-memory cache so IPC hooks see identical bytes
            AppTicket::WriteAppOwnershipTicket(appId, ticketBytes);
        }

        resp.set_ticket(ticketBytes.data(), ticketBytes.size());
        resp.set_eresult(k_EResultOK);

        CMsgProtoBufHeader hdrMsg;
        bool hdrChanged = false;
        if (!pkt.hdr.empty() &&
            hdrMsg.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())) &&
            hdrMsg.has_eresult() && hdrMsg.eresult() != k_EResultOK)
        {
            hdrMsg.set_eresult(k_EResultOK);
            if (hdrMsg.has_error_message()) {
                hdrMsg.clear_error_message();
            }
            hdrChanged = true;
        }

        if (CommitRecvPacket(pPacket, pkt, hdrChanged ? &hdrMsg : nullptr, &resp)) {
            LOG_NETPACKET_INFO("OwnershipTicketResponse[858]: spoofed appid={} ticket_bytes={} (orig eresult={} -> OK)",
                               appId, ticketBytes.size(), origEresult);
        }
    }

} // namespace Hooks_NetPacket_OwnershipTicket


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_FamilySharing
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_FamilySharing {

    // Sanitizes FamilyGroupsClient.NotifyRunningApps RPC notification:
    // Retains the valid family_groupid while clearing running_apps entries to 0.
    // Steam client receives a structurally sound RPC message informing it that
    // 0 apps are currently running, actively clearing library lock state.
    void HandleRecv_NotifyRunningApps(CNetPacket* pPacket, const UnpackedPacket& pkt)
    {
        if (pkt.body.empty()) {
            return;
        }

        CFamilyGroupsClient_NotifyRunningApps_Notification notify;
        if (notify.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
            const int runningCount = notify.running_apps_size();
            if (runningCount == 0) {
                return;
            }

            notify.clear_running_apps();

            if (CommitRecvPacket(pPacket, pkt, nullptr, &notify)) {
                LOG_NETPACKET_INFO("FamilySharing: Sanitized NotifyRunningApps (family_groupid {}, cleared {} running apps)",
                                   notify.family_groupid(), runningCount);
                return;
            }
        }

        LOG_NETPACKET_WARN("FamilySharing: Failed to sanitize NotifyRunningApps, passing through untouched");
    }

    // Clears incoming lock/stop notifications (eMsg 9405 / 9406) to suppress kick timers
    void ClearBody(CNetPacket* pPacket, const UnpackedPacket& pkt)
    {
        LOG_NETPACKET_DEBUG("FamilySharing: Clearing incoming family sharing lock/stop notification...");
        (void)CommitRecvPacket(pPacket, pkt, nullptr, nullptr, true);
    }

} // namespace Hooks_NetPacket_FamilySharing


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Manifest
//
//  Outgoing: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 151)
//  Incoming: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 147)
//
//  Launches a detached worker thread via OSTPlatform::Thread::StartDetached
//  on send; the recv handler waits up to 12s (in 50ms interruptible slices)
//  and patches both header (eresult=OK) and body (manifest_request_code).
//  Valid codes are cached with a 90-second TTL (Valve request codes rotate
//  every 5 minutes).
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Manifest {

    struct FetchResult {
        uint64                                code = 0;
        std::chrono::steady_clock::time_point fetchedAt{};
    };

    struct CacheEntry {
        uint64                                code = 0;
        std::chrono::steady_clock::time_point cachedAt{};
        uint64                                seq  = 0;
    };

    struct InFlightEntry {
        std::shared_future<FetchResult> future;
        uint64                          seq = 0;
    };

    struct PendingManifestJob {
        uint64                          manifestGid = 0;
        uint64                          cachedCode  = 0;
        std::shared_future<FetchResult> future;
        uint64                          seq         = 0;
    };

    std::mutex                        g_CodeMutex;
    uint64                            g_ManifestSeq = 0;
    std::atomic<uint32>               g_ActiveFetchThreads{0};
    NoDestructor<std::unordered_map<uint64, CacheEntry>>         g_ManifestCodeCache; // manifestGid -> CacheEntry
    NoDestructor<std::unordered_map<uint64, InFlightEntry>>      g_InFlightFutures;   // manifestGid -> InFlightEntry
    NoDestructor<std::unordered_map<uint64, PendingManifestJob>> g_PendingJobs;       // jobId -> PendingManifestJob

    constexpr uint32 kMaxActiveFetchThreads = 32;
    constexpr uint32 kMaxWaitMilliseconds   = 12000;
    constexpr uint32 kWaitSliceMilliseconds = 50;
    constexpr auto   kCacheTtl              = std::chrono::seconds(90);

    void CacheValidCodeLocked(uint64 manifestGid, uint64 code,
                              std::chrono::steady_clock::time_point timestamp)
    {
        if (code == 0) return;
        auto it = g_ManifestCodeCache->find(manifestGid);
        if (it != g_ManifestCodeCache->end()) {
            if (it->second.cachedAt > timestamp) {
                return;
            }
            it->second = CacheEntry{
                .code     = code,
                .cachedAt = timestamp,
                .seq      = ++g_ManifestSeq
            };
            return;
        }
        if (g_ManifestCodeCache->size() >= 512) {
            EvictOldestBySeq(*g_ManifestCodeCache);
        }
        (*g_ManifestCodeCache)[manifestGid] = CacheEntry{
            .code     = code,
            .cachedAt = timestamp,
            .seq      = ++g_ManifestSeq
        };
    }

    struct InFlightInspectResult {
        std::unordered_map<uint64, InFlightEntry>::iterator nextIt;
        uint64                                              harvestedCode = 0;
        bool                                                stillInFlight = false;
    };

    // Inspects an in-flight entry: if still running, returns stillInFlight=true without erasing;
    // if completed/invalid, harvests valid non-expired code into g_ManifestCodeCache and erases the entry.
    [[nodiscard]] InFlightInspectResult InspectInFlightLocked(
        std::unordered_map<uint64, InFlightEntry>::iterator itFlight,
        std::chrono::steady_clock::time_point now)
    {
        if (itFlight == g_InFlightFutures->end()) {
            return { .nextIt = itFlight, .harvestedCode = 0, .stillInFlight = false };
        }
        if (!itFlight->second.future.valid()) {
            return { .nextIt = g_InFlightFutures->erase(itFlight), .harvestedCode = 0, .stillInFlight = false };
        }
        if (itFlight->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return { .nextIt = std::next(itFlight), .harvestedCode = 0, .stillInFlight = true };
        }

        const uint64 gid = itFlight->first;
        FetchResult res{};
        try {
            res = itFlight->second.future.get();
        } catch (...) {
            res = {};
        }
        auto nextIt = g_InFlightFutures->erase(itFlight);

        if (res.code != 0 && (now - res.fetchedAt) <= kCacheTtl) {
            CacheValidCodeLocked(gid, res.code, res.fetchedAt);
            return { .nextIt = nextIt, .harvestedCode = res.code, .stillInFlight = false };
        }
        return { .nextIt = nextIt, .harvestedCode = 0, .stillInFlight = false };
    }

    bool HandleSend(std::span<const uint8> body, const CMsgProtoBufHeader& hdr)
    {
        if (body.empty()) return false;

        CContentServerDirectory_GetManifestRequestCode_Request req;
        if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
            LOG_MANIFEST_WARN("GetManifestRequestCode: failed to parse request");
            return false;
        }
        if (!req.has_depot_id() || !req.has_manifest_id()) return false;

        const uint32 depotId     = req.depot_id();
        const uint64 manifestGid = req.manifest_id();
        if (depotId == 0 || manifestGid == 0) return false;
        if (!LuaConfig::HasDepot(depotId) || Hooks_Package::HasValidLicense(depotId)) return false;

        if (!hdr.has_jobid_source()) {
            LOG_MANIFEST_WARN("GetManifestRequestCode: missing jobid_source in header");
            return false;
        }

        const uint64 jobId = hdr.jobid_source();
        if (jobId == 0 || jobId == kInvalidJobId) return false;

        const uint32 appId = req.has_app_id() ? req.app_id() : 0;

        LOG_MANIFEST_DEBUG("GetManifestRequestCode send: depot={} gid={} jobid={} app_id={}",
                           depotId, manifestGid, jobId, appId);

        std::shared_ptr<std::promise<FetchResult>> prom;
        uint64 flightSeq = 0;
        uint64 jobSeq    = 0;
        {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            const auto now = std::chrono::steady_clock::now();

            if (!g_PendingJobs->contains(jobId) && g_PendingJobs->size() >= 256) {
                EvictOldestBySeq(*g_PendingJobs);
            }

            // Bounded pruning & harvesting of completed in-flight futures
            if (g_InFlightFutures->size() >= 64) {
                for (auto it = g_InFlightFutures->begin(); it != g_InFlightFutures->end(); ) {
                    it = InspectInFlightLocked(it, now).nextIt;
                }
                while (g_InFlightFutures->size() >= 64) {
                    EvictOldestBySeq(*g_InFlightFutures);
                }
            }

            // 1. Check if already cached and within 90s TTL
            auto itCache = g_ManifestCodeCache->find(manifestGid);
            if (itCache != g_ManifestCodeCache->end()) {
                if (itCache->second.code != 0 && (now - itCache->second.cachedAt) <= kCacheTtl) {
                    (*g_PendingJobs)[jobId] = PendingManifestJob{
                        .manifestGid = manifestGid,
                        .cachedCode  = itCache->second.code,
                        .future      = {},
                        .seq         = ++g_ManifestSeq
                    };
                    LOG_MANIFEST_DEBUG("GetManifestRequestCode send: cache hit for gid={} (code={})",
                                       manifestGid, itCache->second.code);
                    return false;
                }
                g_ManifestCodeCache->erase(itCache);
            }

            // 2. Check if an async fetch for this manifest is already in flight (or just finished)
            if (auto itFlight = g_InFlightFutures->find(manifestGid); itFlight != g_InFlightFutures->end()) {
                const auto [nextIt, harvestedCode, stillInFlight] = InspectInFlightLocked(itFlight, now);
                if (harvestedCode != 0) {
                    (*g_PendingJobs)[jobId] = PendingManifestJob{
                        .manifestGid = manifestGid,
                        .cachedCode  = harvestedCode,
                        .future      = {},
                        .seq         = ++g_ManifestSeq
                    };
                    LOG_MANIFEST_DEBUG("GetManifestRequestCode send: harvested completed fetch for gid={} (code={})",
                                       manifestGid, harvestedCode);
                    return false;
                }
                if (stillInFlight) {
                    (*g_PendingJobs)[jobId] = PendingManifestJob{
                        .manifestGid = manifestGid,
                        .cachedCode  = 0,
                        .future      = itFlight->second.future,
                        .seq         = ++g_ManifestSeq
                    };
                    LOG_MANIFEST_DEBUG("GetManifestRequestCode send: attaching to in-flight fetch for gid={}", manifestGid);
                    return false;
                }
            }

            // 3. Prepare detached async fetch with bounded concurrency
            if (g_ShuttingDown.load(std::memory_order_acquire) ||
                g_ActiveFetchThreads.load(std::memory_order_relaxed) >= kMaxActiveFetchThreads)
            {
                LOG_MANIFEST_WARN("GetManifestRequestCode send: skipping fetch (shuttingDown={} activeThreads={})",
                                  g_ShuttingDown.load(std::memory_order_relaxed),
                                  g_ActiveFetchThreads.load(std::memory_order_relaxed));
                return false;
            }

            prom = std::make_shared<std::promise<FetchResult>>();
            auto sharedFuture = prom->get_future().share();
            g_ActiveFetchThreads.fetch_add(1, std::memory_order_acq_rel);

            flightSeq = ++g_ManifestSeq;
            (*g_InFlightFutures)[manifestGid] = InFlightEntry{
                .future = sharedFuture,
                .seq    = flightSeq
            };

            jobSeq = ++g_ManifestSeq;
            (*g_PendingJobs)[jobId] = PendingManifestJob{
                .manifestGid = manifestGid,
                .cachedCode  = 0,
                .future      = sharedFuture,
                .seq         = jobSeq
            };
        }

        // Launch detached thread outside g_CodeMutex; the worker thread never touches
        // g_CodeMutex or any global container, eliminating static destruction races.
        const bool started = OSTPlatform::Thread::StartDetached([prom, manifestGid, depotId, appId]() noexcept {
            FetchResult res{};
            try {
                ManifestClient::FetchManifestRequestCode(manifestGid, &res.code, appId, depotId);
                res.fetchedAt = std::chrono::steady_clock::now();
            } catch (...) {
                res = {};
            }
            try {
                prom->set_value(res);
            } catch (...) {}
            g_ActiveFetchThreads.fetch_sub(1, std::memory_order_acq_rel);
        });

        if (!started) {
            try {
                prom->set_value(FetchResult{});
            } catch (...) {}
            g_ActiveFetchThreads.fetch_sub(1, std::memory_order_acq_rel);

            std::lock_guard<std::mutex> lock(g_CodeMutex);
            if (auto itF = g_InFlightFutures->find(manifestGid); itF != g_InFlightFutures->end() && itF->second.seq == flightSeq) {
                g_InFlightFutures->erase(itF);
            }
            if (auto itJ = g_PendingJobs->find(jobId); itJ != g_PendingJobs->end() && itJ->second.seq == jobSeq) {
                g_PendingJobs->erase(itJ);
            }
            LOG_MANIFEST_ERROR("GetManifestRequestCode send: StartDetached failed for gid={}", manifestGid);
        }

        return false; // Don't modify the outgoing request body
    }

    void HandleRecv(CNetPacket* pPacket,
                    const UnpackedPacket& pkt,
                    const CMsgProtoBufHeader& inHdr)
    {
        if (!inHdr.has_jobid_target()) return;
        const uint64 jobId = inHdr.jobid_target();
        if (jobId == 0 || jobId == kInvalidJobId) return;

        PendingManifestJob job;
        {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            auto it = g_PendingJobs->find(jobId);
            if (it == g_PendingJobs->end()) return;
            job = std::move(it->second);
            g_PendingJobs->erase(it); // Always clean up immediately
        }

        // Fast-path: if official Steam server already returned k_EResultOK with a valid
        // non-zero manifest_request_code (e.g., owned base depot or free weekend),
        // cache the official code and pass through in 0ms without waiting on backend HTTP!
        if (inHdr.has_eresult() && inHdr.eresult() == static_cast<int32_t>(k_EResultOK) && !pkt.body.empty()) {
            CContentServerDirectory_GetManifestRequestCode_Response origResp;
            if (origResp.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size())) &&
                origResp.has_manifest_request_code() && origResp.manifest_request_code() != 0)
            {
                const uint64 officialCode = origResp.manifest_request_code();
                {
                    std::lock_guard<std::mutex> lock(g_CodeMutex);
                    CacheValidCodeLocked(job.manifestGid, officialCode, std::chrono::steady_clock::now());
                    g_InFlightFutures->erase(job.manifestGid);
                }
                LOG_MANIFEST_DEBUG("GetManifestRequestCode recv: official server returned valid code={} for gid={}, passing through",
                                   officialCode, job.manifestGid);
                return;
            }
        }

        uint64 code = job.cachedCode;
        if (code == 0) {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            const auto now = std::chrono::steady_clock::now();
            auto it = g_ManifestCodeCache->find(job.manifestGid);
            if (it != g_ManifestCodeCache->end()) {
                if (it->second.code != 0 && (now - it->second.cachedAt) <= kCacheTtl) {
                    code = it->second.code;
                } else {
                    g_ManifestCodeCache->erase(it);
                }
            }
        }

        if (code == 0 && job.future.valid()) {
            try {
                constexpr uint32 kMaxSlices = kMaxWaitMilliseconds / kWaitSliceMilliseconds;
                std::future_status status = std::future_status::timeout;
                for (uint32 slice = 0; slice < kMaxSlices; ++slice) {
                    if (g_ShuttingDown.load(std::memory_order_acquire)) {
                        return;
                    }
                    status = job.future.wait_for(std::chrono::milliseconds(kWaitSliceMilliseconds));
                    if (status == std::future_status::ready) {
                        break;
                    }
                }

                if (status != std::future_status::ready) {
                    LOG_MANIFEST_WARN("GetManifestRequestCode recv: HTTP timed out ({}ms) for jobid={}, gid={}, passing through",
                                      kMaxWaitMilliseconds, jobId, job.manifestGid);
                    return;
                }

                const FetchResult res = job.future.get();
                code = res.code;
                {
                    std::lock_guard<std::mutex> lock(g_CodeMutex);
                    if (code != 0) {
                        CacheValidCodeLocked(job.manifestGid, code,
                                             res.fetchedAt.time_since_epoch().count() != 0
                                                 ? res.fetchedAt
                                                 : std::chrono::steady_clock::now());
                    }
                    auto itFlight = g_InFlightFutures->find(job.manifestGid);
                    if (itFlight != g_InFlightFutures->end() &&
                        (!itFlight->second.future.valid() ||
                         itFlight->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
                    {
                        g_InFlightFutures->erase(itFlight);
                    }
                }
            } catch (const std::exception& e) {
                LOG_MANIFEST_ERROR("GetManifestRequestCode recv: future wait/get failed: {}", e.what());
                code = 0;
            } catch (...) {
                LOG_MANIFEST_ERROR("GetManifestRequestCode recv: future wait/get failed with unknown exception");
                code = 0;
            }
        }

        if (!code) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: HTTP returned 0 for jobid={}, gid={}", jobId, job.manifestGid);
            return;
        }

        LOG_MANIFEST_DEBUG("GetManifestRequestCode recv: injecting code={} for jobid={}, gid={}",
                           code, jobId, job.manifestGid);

        // Header: set eresult=OK and purge any error_message from official rejection
        CMsgProtoBufHeader hdr = inHdr;
        hdr.set_eresult(static_cast<int32_t>(k_EResultOK));
        if (hdr.has_error_message()) {
            hdr.clear_error_message();
        }

        // Body: set manifest_request_code
        CContentServerDirectory_GetManifestRequestCode_Response resp;
        resp.set_manifest_request_code(code);

        if (!CommitRecvPacket(pPacket, pkt, &hdr, &resp)) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: failed to CommitRecvPacket for jobid={}, gid={}",
                              jobId, job.manifestGid);
        }
    }

} // namespace Hooks_NetPacket_Manifest


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_RichPresence
//
//  Outgoing: CMsgClientGamesPlayed   (eMsg 742 / 5410)
//  Incoming: CMsgClientPersonaState  (eMsg 766)
//
//  The server drops friend-side broadcasts for unowned AppIds, so the
//  banner stays on the last cached state.  Cache real self-pushes and
//  re-deliver a patched copy through oRecvPkt by borrowing the next
//  carrier packet's data pointer.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_RichPresence {

    std::mutex           g_RPMutex;
    std::atomic<AppId_t> g_PlayingAppId{0};
    std::atomic<uint64>  g_LocalSteamId{0};

    // Most recent self-PersonaState bytes captured from a real server push.
    // Reused as the template every game launch.
    uint8   g_SelfHdr [kMaxHdrSize];
    uint32  g_cbSelfHdr      = 0;
    uint8   g_SelfBody[kMaxBodySize];
    uint32  g_cbSelfBody     = 0;
    bool    g_HaveSelfCached = false;

    // Manufactured PersonaState packet (eMsg 766) ready to inject.
    uint8             g_InjectPkt[kMaxPacketSize];
    uint32            g_cbInjectPkt = 0;
    std::atomic<bool> g_InjectPending{false};

    // Rich presence KVs per AppId, captured from outbound
    // CMsgClientRichPresenceUpload.  Per-AppId so a multi-game stack
    // does not conflate KV state.
    NoDestructor<std::unordered_map<AppId_t, std::vector<std::pair<std::string, std::string>>>> g_RPKvsByAppId;

    // Validates and captures the local individual SteamID, supporting account switching
    // and rejecting pre-login anonymous individual placeholders (AccountID == 0).
    void TryCaptureLocalSteamId(uint64 rawSteamId)
    {
        if (rawSteamId == 0 || g_LocalSteamId.load(std::memory_order_relaxed) == rawSteamId) {
            return;
        }

        const CSteamID candidate(rawSteamId);
        if (!candidate.IsValid() ||
            candidate.GetEAccountType() != k_EAccountTypeIndividual ||
            candidate.GetAccountID() == 0)
        {
            return;
        }

        bool changed = false;
        {
            std::lock_guard lock(g_RPMutex);
            const uint64 prev = g_LocalSteamId.load(std::memory_order_relaxed);
            if (prev != rawSteamId) {
                g_LocalSteamId.store(rawSteamId, std::memory_order_release);
                g_HaveSelfCached = false;
                g_cbSelfHdr      = 0;
                g_cbSelfBody     = 0;
                g_InjectPending.store(false, std::memory_order_release);
                g_cbInjectPkt    = 0;
                if (prev != 0) {
                    g_PlayingAppId.store(0, std::memory_order_release);
                    g_RPKvsByAppId->clear();
                }
                changed = true;
                LOG_RICHPRESENCE_INFO("Captured valid local SteamID 0x{:X} (accountId={})",
                                      rawSteamId, candidate.GetAccountID());
            }
        }

        if (changed) {
            CloudRedirectHost::SetAccountId(candidate.GetAccountID());
        }
    }

    // Walk Steam's binary KV1 stream (a top-level "RP" struct around
    // string KVs) and collect every string KV at any depth.  Type 0x00
    // starts a struct, 0x01 a string KV, 0x08 ends a struct.  String KVs
    // are null-terminated key + null-terminated value.
    static void ExtractStringKVs(std::span<const uint8> data,
                                 std::vector<std::pair<std::string, std::string>>& out)
    {
        size_t pos = 0;
        int depth = 0;
        auto readCStr = [&](std::string& s) -> bool {
            const size_t start = pos;
            while (pos < data.size() && data[pos] != 0) ++pos;
            if (pos >= data.size()) return false;
            s.assign(reinterpret_cast<const char*>(data.data() + start), pos - start);
            ++pos;
            return true;
        };
        while (pos < data.size()) {
            const uint8 type = data[pos++];
            if (type == 0x08) {
                if (depth > 0) { --depth; continue; }
                break;
            }
            if (type == 0x00) {
                if (depth >= 32) return;
                std::string name;
                if (!readCStr(name)) return;
                ++depth;
            } else if (type == 0x01) {
                std::string key, value;
                if (!readCStr(key) || !readCStr(value)) return;
                if (out.size() < 64) {
                    out.emplace_back(std::move(key), std::move(value));
                }
            } else {
                return;
            }
        }
    }

    // Patch the self Friend entry with the appid (0 = stopped) and per-app
    // KVs.  Mask status_flags's RichPresence bit (0x1000) on appid + empty
    // KVs so a freshly launched game's first inject does not wipe the UI's
    // m_mapRichPresence, which is rebuilt from rich_presence() whenever
    // that bit is set.
    // Pure leaf-lock helper: caller resolves gameName before acquiring g_RPMutex.
    static void ApplyGameFields(CMsgClientPersonaState& msg,
                                CMsgClientPersonaState::Friend* entry,
                                AppId_t appid,
                                std::string_view gameName,
                                bool updateStatusFlags = true)
    {
        // EClientPersonaStateFlag::k_EClientPersonaStateFlagRichPresence
        constexpr uint32 kStatusFlagRichPresence = 0x1000;
        bool desiredRpFlag = false;

        if (appid) {
            entry->set_game_played_app_id(appid);
            entry->set_gameid(static_cast<uint64>(appid));
            if (!gameName.empty()) {
                entry->set_game_name(gameName.data(), gameName.size());
            }
            entry->clear_rich_presence();
            auto it = g_RPKvsByAppId->find(appid);
            const bool hasKvs = (it != g_RPKvsByAppId->end()) && !it->second.empty();
            if (hasKvs) {
                for (const auto& [k, v] : it->second) {
                    auto* kv = entry->add_rich_presence();
                    kv->set_key(k);
                    kv->set_value(v);
                }
            }
            desiredRpFlag = hasKvs;
        } else {
            entry->clear_game_played_app_id();
            entry->clear_gameid();
            entry->clear_game_name();
            entry->clear_rich_presence();
            desiredRpFlag = true;
        }

        if (updateStatusFlags) {
            const uint32 curFlags = msg.status_flags();
            const uint32 desiredFlags = desiredRpFlag
                ? (curFlags | kStatusFlagRichPresence)
                : (curFlags & ~kStatusFlagRichPresence);
            msg.set_status_flags(desiredFlags);
        }
    }

    static bool BuildInject(AppId_t appid, std::string_view gameName)
    {
        if (!g_HaveSelfCached) return false;

        CMsgClientPersonaState msg;
        if (!msg.ParseFromArray(g_SelfBody, static_cast<int>(g_cbSelfBody))) return false;

        const uint64 localSteamId = g_LocalSteamId.load(std::memory_order_relaxed);
        if (localSteamId == 0) return false;

        // Find our entry (a self-push always contains it).
        CMsgClientPersonaState::Friend* entry = nullptr;
        for (int i = 0; i < msg.friends_size(); ++i) {
            auto* f = msg.mutable_friends(i);
            if (f->has_friendid() && f->friendid() == localSteamId) {
                entry = f;
                break;
            }
        }
        if (!entry) return false;

        // Strip any co-batched friend entries from the template so re-injecting
        // never replays stale friend state into the Steam client.
        if (msg.friends_size() > 1) {
            CMsgClientPersonaState::Friend selfCopy = *entry;
            msg.clear_friends();
            entry = msg.add_friends();
            *entry = std::move(selfCopy);
        }

        ApplyGameFields(msg, entry, appid, gameName);

        const uint32 hdrSize  = g_cbSelfHdr;
        const size_t bodyLong = msg.ByteSizeLong();
        if (hdrSize > kMaxHdrSize || bodyLong > kMaxBodySize) {
            LOG_RICHPRESENCE_WARN("Inject packet too large (hdr={}, body={})", hdrSize, bodyLong);
            return false;
        }
        const uint32 bodySize = static_cast<uint32>(bodyLong);
        const uint32 total    = static_cast<uint32>(sizeof(MsgHdr)) + hdrSize + bodySize;
        if (total > sizeof(g_InjectPkt)) {
            LOG_RICHPRESENCE_WARN("Inject packet too large ({} bytes)", total);
            return false;
        }

        auto* mhdr = reinterpret_cast<MsgHdr*>(g_InjectPkt);
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgClientPersonaState) | kMsgHdrProtoFlag);
        mhdr->headerLength = hdrSize;
        if (hdrSize > 0) {
            std::memcpy(g_InjectPkt + sizeof(MsgHdr), g_SelfHdr, hdrSize);
        }
        if (!msg.SerializeToArray(g_InjectPkt + sizeof(MsgHdr) + hdrSize, static_cast<int>(bodySize)))
            return false;

        g_cbInjectPkt = total;
        LOG_RICHPRESENCE_INFO("Built inject for appid {} ({} bytes)", appid, total);
        return true;
    }

    // Decode outbound CMsgClientRichPresenceUpload into per-AppId KVs
    // and stage a fresh PersonaState inject.
    void TrackRPSend(std::span<const uint8> body)
    {
        if (body.empty()) return;

        const AppId_t playingAppId = g_PlayingAppId.load(std::memory_order_relaxed);
        if (playingAppId == 0) return;

        CMsgClientRichPresenceUpload up;
        if (!up.ParseFromArray(body.data(), static_cast<int>(body.size()))) return;
        if (!up.has_rich_presence_kv()) return;

        const std::string& kv = up.rich_presence_kv();
        std::vector<std::pair<std::string, std::string>> kvs;
        ExtractStringKVs(
            std::span<const uint8>(reinterpret_cast<const uint8*>(kv.data()), kv.size()),
            kvs);
        LOG_RICHPRESENCE_DEBUG("RP upload appid={}: kv_bytes={} extracted={} pairs",
            playingAppId, kv.size(), kvs.size());

        // Resolve game name outside g_RPMutex to keep g_RPMutex a pure leaf lock
        const std::string gameName = Hooks_Misc::GetGameNameByAppID(playingAppId);

        std::lock_guard lock(g_RPMutex);
        if (!g_RPKvsByAppId->contains(playingAppId) && g_RPKvsByAppId->size() >= 64) {
            g_RPKvsByAppId->erase(g_RPKvsByAppId->begin());
        }
        (*g_RPKvsByAppId)[playingAppId] = std::move(kvs);
        if (g_PlayingAppId.load(std::memory_order_relaxed) == playingAppId) {
            if (BuildInject(playingAppId, gameName)) {
                g_InjectPending.store(true, std::memory_order_release);
            }
        }
    }

    void TrackSend(const CMsgClientGamesPlayed& msg, std::span<const uint8> hdr)
    {
        if (!hdr.empty()) {
            CMsgProtoBufHeader hdrMsg;
            if (hdrMsg.ParseFromArray(hdr.data(), static_cast<int>(hdr.size())) &&
                hdrMsg.has_steamid() && hdrMsg.steamid() != 0)
            {
                TryCaptureLocalSteamId(hdrMsg.steamid());
            }
        }

        // Steam stacks running games in games_played; the banner follows
        // the tail (most recently launched).  Mirror that — only the
        // tail's appid drives our inject.
        AppId_t topmost = 0;
        if (msg.games_played_size() > 0) {
            topmost = ExtractSteamAppId(
                msg.games_played(msg.games_played_size() - 1).game_id());
        }

        // For -onlinefix games, map topmost 480 to real AppId so CloudRedirectHost
        // can track and sync cloud saves for the real game.
        const AppId_t effectiveTopmost = ResolveOnlineFixAppId(topmost);

        // INV-01 & INV-02: Owned / family-shared games let the server's natural
        // PersonaState broadcast paint the UI and use official Steam Cloud.
        const bool isOnlineFixApp = (topmost == kOnlineFixAppId &&
                                     effectiveTopmost != 0 &&
                                     effectiveTopmost != kOnlineFixAppId);
        const bool shouldTrack = (effectiveTopmost != 0 &&
                                  effectiveTopmost != kOnlineFixAppId &&
                                  !Hooks_Package::HasValidLicense(effectiveTopmost) &&
                                  (isOnlineFixApp ||
                                   LuaConfig::HasDepot(effectiveTopmost) ||
                                   CloudRedirectHost::IsApp(effectiveTopmost)));

        const AppId_t newTracked = shouldTrack ? effectiveTopmost : 0;

        const AppId_t oldTracked = g_PlayingAppId.exchange(newTracked);
        if (oldTracked == newTracked) return;

        if (oldTracked != 0) {
            try {
                CloudRedirectHost::NotifyAppRunning(oldTracked, false);
            } catch (const std::exception& e) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, false) failed: {}", oldTracked, e.what());
            } catch (...) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, false) failed with unknown exception", oldTracked);
            }
        }
        if (newTracked != 0) {
            try {
                CloudRedirectHost::NotifyAppRunning(newTracked, true);
            } catch (const std::exception& e) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, true) failed: {}", newTracked, e.what());
            } catch (...) {
                LOG_RICHPRESENCE_ERROR("NotifyAppRunning({}, true) failed with unknown exception", newTracked);
            }
            if (!Hooks_Package::HasValidLicense(newTracked)) {
                LuaConfig::PrewarmStatSteamId(newTracked);
            }
        }

        // Resolve game name outside g_RPMutex to enforce leaf-lock discipline
        const std::string newTrackedName = (newTracked != 0)
            ? Hooks_Misc::GetGameNameByAppID(newTracked)
            : std::string{};

        std::lock_guard lock(g_RPMutex);
        if (g_PlayingAppId.load(std::memory_order_relaxed) != newTracked) {
            return;
        }
        g_InjectPending.store(false, std::memory_order_release);
        g_cbInjectPkt = 0;
        if (newTracked != 0) {
            LOG_RICHPRESENCE_INFO("Tracking topmost appid {}", newTracked);
            if (BuildInject(newTracked, newTrackedName)) {
                g_InjectPending.store(true, std::memory_order_release);
            }
        } else if (msg.games_played_size() == 0) {
            // Stack went empty — inject a clear so the cache reverts.
            LOG_RICHPRESENCE_DEBUG("GamesPlayed empty, scheduling cache clear");
            if (BuildInject(0, {})) {
                g_InjectPending.store(true, std::memory_order_release);
            }
        } else {
            // Topmost is owned; let the server's broadcast paint it.
            // Skipping the clear-inject here avoids a brief "Online"
            // flicker between our drop and the server's push.
            LOG_RICHPRESENCE_DEBUG("Topmost is appid {} (owned); deferring to server", topmost);
        }
    }

    // Cache real self-pushes as the inject template; if we are tracking
    // an unowned game, patch the live message in place so a periodic
    // refresh does not overwrite the injected game info.
    void HandleRecv(CNetPacket* pPacket, const UnpackedPacket& pkt)
    {
        if (pkt.body.empty()) return;

        CMsgClientPersonaState msg;
        if (!msg.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) return;

        uint64 localSteamId = g_LocalSteamId.load(std::memory_order_relaxed);
        CMsgClientPersonaState::Friend* selfEntry = nullptr;
        int selfIndex = -1;
        if (localSteamId != 0) {
            for (int i = 0; i < msg.friends_size(); ++i) {
                auto* f = msg.mutable_friends(i);
                if (f->has_friendid() && f->friendid() == localSteamId) {
                    selfEntry = f;
                    selfIndex = i;
                    break;
                }
            }
        }

        // If selfEntry wasn't matched (e.g., first login push before any outbound packet
        // or account switch), inspect the header's steamid and rescan if updated.
        if (!selfEntry && !pkt.hdr.empty()) {
            CMsgProtoBufHeader hdrMsg;
            if (hdrMsg.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())) &&
                hdrMsg.has_steamid() && hdrMsg.steamid() != 0)
            {
                TryCaptureLocalSteamId(hdrMsg.steamid());
                const uint64 updatedId = g_LocalSteamId.load(std::memory_order_relaxed);
                if (updatedId != 0 && updatedId != localSteamId) {
                    localSteamId = updatedId;
                    for (int i = 0; i < msg.friends_size(); ++i) {
                        auto* f = msg.mutable_friends(i);
                        if (f->has_friendid() && f->friendid() == localSteamId) {
                            selfEntry = f;
                            selfIndex = i;
                            break;
                        }
                    }
                }
            }
        }

        // 99% of PersonaState packets are friend broadcasts — exit without locking g_RPMutex!
        if (!selfEntry) return;

        LOG_RICHPRESENCE_DEBUG(
            "Recv self PersonaState: status_flags=0x{:X} friends_size={}",
            msg.status_flags(), msg.friends_size());

        const AppId_t playingAppId = g_PlayingAppId.load(std::memory_order_relaxed);
        const std::string gameName = (playingAppId != 0)
            ? Hooks_Misc::GetGameNameByAppID(playingAppId)
            : std::string{};

        bool patched = false;
        {
            std::lock_guard lock(g_RPMutex);
            if (pkt.hdr.size() <= sizeof(g_SelfHdr)) {
                if (msg.friends_size() == 1 && pkt.body.size() <= sizeof(g_SelfBody)) {
                    if (!pkt.hdr.empty()) {
                        std::memcpy(g_SelfHdr, pkt.hdr.data(), pkt.hdr.size());
                    }
                    std::memcpy(g_SelfBody, pkt.body.data(), pkt.body.size());
                    g_cbSelfHdr      = static_cast<uint32>(pkt.hdr.size());
                    g_cbSelfBody     = static_cast<uint32>(pkt.body.size());
                    g_HaveSelfCached = true;
                } else if (msg.friends_size() > 1) {
                    // Isolate only selfEntry into g_SelfBody so co-batched friend lists
                    // (even > 64KB on large accounts) never fail caching or bloat BuildInject.
                    CMsgClientPersonaState selfTemplate;
                    if (msg.has_status_flags()) {
                        selfTemplate.set_status_flags(msg.status_flags());
                    }
                    *selfTemplate.add_friends() = *selfEntry;
                    const size_t tplSize = selfTemplate.ByteSizeLong();
                    if (tplSize > 0 && tplSize <= sizeof(g_SelfBody) &&
                        selfTemplate.SerializeToArray(g_SelfBody, static_cast<int>(tplSize)))
                    {
                        if (!pkt.hdr.empty()) {
                            std::memcpy(g_SelfHdr, pkt.hdr.data(), pkt.hdr.size());
                        }
                        g_cbSelfHdr      = static_cast<uint32>(pkt.hdr.size());
                        g_cbSelfBody     = static_cast<uint32>(tplSize);
                        g_HaveSelfCached = true;
                    }
                }
            }

            if (playingAppId != 0 && g_PlayingAppId.load(std::memory_order_relaxed) == playingAppId) {
                bool needDedicatedInject = false;
                if (msg.friends_size() == 1) {
                    ApplyGameFields(msg, selfEntry, playingAppId, gameName, true);
                } else if (BuildInject(playingAppId, gameName) &&
                           selfIndex >= 0 && selfIndex < msg.friends_size())
                {
                    // Co-batched packet: strip selfEntry so its status_flags and payload
                    // are isolated from other friends; deliver self via dedicated TryInject.
                    msg.mutable_friends()->DeleteSubrange(selfIndex, 1);
                    needDedicatedInject = true;
                } else {
                    ApplyGameFields(msg, selfEntry, playingAppId, gameName, false);
                }

                if (CommitRecvPacket(pPacket, pkt, nullptr, &msg)) {
                    if (needDedicatedInject) {
                        g_InjectPending.store(true, std::memory_order_release);
                    } else {
                        g_InjectPending.store(false, std::memory_order_release);
                        g_cbInjectPkt = 0;
                    }
                    patched = true;
                } else {
                    g_InjectPending.store(false, std::memory_order_release);
                    g_cbInjectPkt = 0;
                }
            }
        }

        if (patched) {
            LOG_RICHPRESENCE_INFO("Patched live self push with appid {}", playingAppId);
        }
    }

    // Deliver the pending manufactured PersonaState by borrowing the
    // carrier's data pointer for one oRecvPkt call, then restore.
    void TryInject(void* pThis, CNetPacket* pCarrier,
                   bool (*invokeOriginal)(void*, CNetPacket*))
    {
        if (!g_InjectPending.load(std::memory_order_acquire)) return;
        if (!NetPacketLayout::s_resolved.load(std::memory_order_acquire)) return;

        uint32 injectSize = 0;
        std::span<uint8> slot;
        {
            std::lock_guard lock(g_RPMutex);
            if (!g_InjectPending.load(std::memory_order_relaxed) || g_cbInjectPkt == 0) return;
            g_InjectPending.store(false, std::memory_order_release);
            injectSize = g_cbInjectPkt;
            slot = g_RecvPool.AcquireSlot();
            std::memcpy(slot.data(), g_InjectPkt, injectSize);
        }

        {
            CarrierScopeGuard guard(pCarrier, slot.data(), injectSize);
            invokeOriginal(pThis, pCarrier);
        }
        LOG_RICHPRESENCE_INFO("Delivered manufactured self-PersonaState ({} bytes)", injectSize);
    }

} // namespace Hooks_NetPacket_RichPresence


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_OnlineFix
//
//  Outgoing: CMsgClientGamesPlayed (eMsg 742 / 5410)
//
//  When a game launched with -onlinefix reports appid 480, replace
//  game_extra_info with the real game's localized name so friends
//  see the correct title.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_OnlineFix {

    [[nodiscard]] bool HandleSend(const UnpackedPacket& pkt,
                                  std::span<uint8>& outFrame)
    {
        if (pkt.body.empty()) return false;

        CMsgClientGamesPlayed msg;
        if (!msg.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size()))) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to parse CMsgClientGamesPlayed");
            return false;
        }
        LOG_ONLINEFIX_DEBUG("OnlineFix: original body:\n{}", msg.DebugString());

        Hooks_NetPacket_RichPresence::TrackSend(msg, pkt.hdr);

        const uint64 localSteamId = Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed);
        const uint32 localAccountId = (localSteamId != 0) ? CSteamID(localSteamId).GetAccountID() : 0u;

        bool patched = false;
        for (int i = 0; i < msg.games_played_size(); ++i) {
            auto* game = msg.mutable_games_played(i);
            const AppId_t appid = ExtractSteamAppId(game->game_id());

            // SpawnProcess rewrites pGameID to 480, so game_id is already 480.
            // Fill game_extra_info with the real game name when OnlineFix is active.
            if (appid == kOnlineFixAppId && Hooks_Misc::IsOnlineFixActive()) {
                const AppId_t realAppId = Hooks_Misc::ResolveAppId();
                if (realAppId && realAppId != kOnlineFixAppId) {
                    std::string name = Hooks_Misc::GetGameNameByAppID(realAppId);
                    if (!name.empty()) {
                        game->set_game_extra_info(name);
                        patched = true;
                        LOG_ONLINEFIX_INFO("OnlineFix: 480 -> name '{}' (real appid {})",
                            name, realAppId);
                    }
                }
            }

            // Family sharing concurrency / anti-lock protection:
            // Mask lender's owner_id to 1 so Steam server does not lock out the lender.
            // INV-01: Never mask own-account ownership; when localAccountId is 0, only mask confirmed shared licenses.
            if (game->has_owner_id() && game->owner_id() != 0 && game->owner_id() != 1) {
                const bool isExternalLender = (localAccountId != 0)
                    ? (game->owner_id() != localAccountId && !Hooks_Package::IsAppTrulyOwned(appid))
                    : Hooks_Package::IsSharedLicense(appid);
                if (isExternalLender) {
                    LOG_NETPACKET_INFO("FamilySharing: Masking lender owner_id {} -> 1 for game_id {}",
                                       game->owner_id(), game->game_id());
                    game->set_owner_id(1);
                    patched = true;
                }
            }
        }

        if (!patched) return false;

        if (!CommitSendPacket(pkt, msg, outFrame)) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to CommitSendPacket");
            return false;
        }

        LOG_ONLINEFIX_DEBUG("OnlineFix: modified body:\n{}", msg.DebugString());
        return true;
    }

} // namespace Hooks_NetPacket_OnlineFix


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Cloud
//
//  Steam Cloud save redirection via CloudRedirect (cloud_redirect.dll).
//
//  Outgoing: ServiceMethodCallFromClient (eMsg 151) with target_job_name
//            "Cloud.*" for an addappid()-unlocked game.
//  Incoming: a synthesized ServiceMethodResponse (eMsg 147) carrying the
//            answer produced by CloudRedirect, correlated by jobid.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Cloud {

    std::mutex                                   g_queueMutex;
    NoDestructor<std::deque<std::vector<uint8>>> g_pending; // ready-to-inject 147 packets
    std::atomic<bool>                            g_hasPending{false};

    // ── minimal top-level protobuf varint field reader ──────────
    // Avoids pulling the Cloud.* request message definitions into the
    // proto set just to read one appid field.
    [[nodiscard]] static bool ReadVarint(std::span<const uint8> d, size_t& pos, uint64& out) noexcept {
        out = 0;
        int shift = 0;
        while (pos < d.size()) {
            const uint8 b = d[pos++];
            out |= static_cast<uint64>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
            if (shift >= 64) return false;
        }
        return false;
    }

    [[nodiscard]] static bool FindVarintField(std::span<const uint8> d, uint32 target, uint64& out) noexcept {
        size_t pos = 0;
        while (pos < d.size()) {
            uint64 tag;
            if (!ReadVarint(d, pos, tag)) return false;
            const uint32 field    = static_cast<uint32>(tag >> 3);
            const uint32 wireType = static_cast<uint32>(tag & 7);
            if (field == target && wireType == 0)
                return ReadVarint(d, pos, out);

            switch (wireType) {
            case 0: { uint64 tmp; if (!ReadVarint(d, pos, tmp)) return false; break; }
            case 1: if (d.size() - pos < 8) return false; pos += 8; break;
            case 5: if (d.size() - pos < 4) return false; pos += 4; break;
            case 2: {
                uint64 len;
                if (!ReadVarint(d, pos, len)) return false;
                if (len > d.size() - pos) return false;
                pos += static_cast<size_t>(len);
                break;
            }
            default: return false;   // groups (3/4) — bail
            }
        }
        return false;
    }

    // appid lives in field 1 of every Cloud.* request except
    // ClientCommitFileUpload, where it is field 2 (mirrors CloudRedirect's
    // CloudRpcUtils::ExtractAppId).
    [[nodiscard]] uint32 ExtractAppId(std::string_view jobName, std::span<const uint8> body) noexcept {
        const uint32 fieldNum = (jobName == "Cloud.ClientCommitFileUpload#1") ? 2u : 1u;
        uint64 v = 0;
        if (FindVarintField(body, fieldNum, v) && v <= (std::numeric_limits<uint32>::max)())
            return static_cast<uint32>(v);
        return 0;
    }

    // Returns true when CloudRedirect handled the request — the caller must
    // then suppress the outbound frame.
    bool HandleSend(std::string_view jobName,
                    std::span<const uint8> body,
                    const CMsgProtoBufHeader& reqHdr)
    {
        if (!CloudRedirectHost::IsActive()) return false;

        const uint32 rawAppId = ExtractAppId(jobName, body);
        const uint32 appId    = ResolveOnlineFixAppId(rawAppId);
        if (appId != rawAppId) {
            LOG_NETPACKET_INFO("Cloud RPC {}: mapped OnlineFix AppID {} -> {}", jobName, rawAppId, appId);
        }
        if (appId == 0) return false;

        // INV-01: Truly owned and family-shared games must use official Steam Cloud untouched
        if (Hooks_Package::HasValidLicense(appId)) return false;
        if (!CloudRedirectHost::IsApp(appId)) return false;

        const uint64 localSteamId = Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed);
        const uint32 accountId    = (localSteamId != 0) ? CSteamID(localSteamId).GetAccountID() : 0u;
        if (accountId == 0) return false;

        std::span<uint8> scratchSlot = g_SendPool.AcquireSlot().subspan(0, kMaxBodySize);
        uint32  respLen  = 0;
        int32_t eresult  = 2;   // EResult::Fail
        if (!CloudRedirectHost::HandleCloudRpc(reqHdr.target_job_name().c_str(), appId, accountId,
                                               body.data(), static_cast<uint32>(body.size()),
                                               scratchSlot.data(), static_cast<uint32>(scratchSlot.size()),
                                               &respLen, &eresult)) {
            return false;   // not a namespace app / unrecognised — let it pass through
        }

        // Build the 147 ServiceMethodResponse correlated to the request job.
        CMsgProtoBufHeader respHdr;
        if (reqHdr.has_jobid_source()) respHdr.set_jobid_target(reqHdr.jobid_source());
        respHdr.set_eresult(eresult);
        respHdr.set_target_job_name(reqHdr.target_job_name());

        const size_t cbRespHdrLong = respHdr.ByteSizeLong();
        if (cbRespHdrLong > kMaxHdrSize || respLen > kMaxBodySize) {
            LOG_NETPACKET_WARN("Cloud: {} response too large (hdr={}, body={}), passing through",
                               jobName, cbRespHdrLong, respLen);
            return false;
        }
        const uint32 cbRespHdr = static_cast<uint32>(cbRespHdrLong);
        const uint32 total     = static_cast<uint32>(sizeof(MsgHdr)) + cbRespHdr + respLen;
        if (total > kMaxPacketSize) {
            LOG_NETPACKET_WARN("Cloud: {} response too large ({} bytes), passing through",
                               jobName, total);
            return false;
        }

        std::vector<uint8> pkt(total);
        auto* mhdr = reinterpret_cast<MsgHdr*>(pkt.data());
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgServiceMethodResponse) | kMsgHdrProtoFlag);
        mhdr->headerLength = cbRespHdr;
        if (!respHdr.SerializeToArray(pkt.data() + sizeof(MsgHdr), static_cast<int>(cbRespHdr)))
            return false;
        if (respLen)
            std::memcpy(pkt.data() + sizeof(MsgHdr) + cbRespHdr, scratchSlot.data(), respLen);

        {
            std::lock_guard lk(g_queueMutex);
            if (g_pending->size() >= 128) {
                LOG_NETPACKET_WARN("Cloud: queue capacity reached (128), dropping oldest pending response to prevent hang");
                g_pending->pop_front();
            }
            g_pending->push_back(std::move(pkt));
            g_hasPending.store(true, std::memory_order_release);
        }
        LOG_NETPACKET_DEBUG("Cloud: handled {} app={} -> queued {}-byte response (eresult={})",
                            jobName, appId, total, eresult);
        return true;
    }

    // Deliver any queued cloud responses by borrowing the carrier packet for
    // one oRecvPkt call each (same trick as RichPresence::TryInject). Runs on
    // the network thread from inside the RecvPkt hook.
    void Drain(void* pThis, CNetPacket* pCarrier,
               bool (*invokeOriginal)(void*, CNetPacket*))
    {
        if (!g_hasPending.load(std::memory_order_acquire)) return;
        if (!NetPacketLayout::s_resolved.load(std::memory_order_acquire)) return;

        constexpr size_t kMaxBurstDrain = 8;
        for (size_t i = 0; i < kMaxBurstDrain; ++i) {
            std::vector<uint8> pkt;
            {
                std::lock_guard lk(g_queueMutex);
                if (g_pending->empty()) {
                    g_hasPending.store(false, std::memory_order_release);
                    return;
                }
                pkt = std::move(g_pending->front());
                g_pending->pop_front();
                if (g_pending->empty()) {
                    g_hasPending.store(false, std::memory_order_release);
                }
            }

            if (pkt.empty() || pkt.size() > kMaxPacketSize) continue;

            std::span<uint8> slot = g_RecvPool.AcquireSlot();
            std::memcpy(slot.data(), pkt.data(), pkt.size());

            {
                CarrierScopeGuard guard(pCarrier, slot.data(), static_cast<uint32>(pkt.size()));
                invokeOriginal(pThis, pCarrier);
            }
            LOG_NETPACKET_DEBUG("Cloud: delivered {}-byte response", pkt.size());
        }
    }

} // namespace Hooks_NetPacket_Cloud

// ════════════════════════════════════════════════════════════════
//  Legacy CD-Key local resolution
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_LegacyKey {

    static std::mutex                     g_queueMutex;
    static std::deque<std::vector<uint8>> g_pending;

    bool HandleSend(const uint8* pubData, uint32 cubData) {
        if (cubData < sizeof(ExtendedMsgHdr) + sizeof(MsgClientGetLegacyGameKey))
            return false;

        const auto* reqHdr  = reinterpret_cast<const ExtendedMsgHdr*>(pubData);
        const auto* reqBody = reinterpret_cast<const MsgClientGetLegacyGameKey*>(
                                  pubData + sizeof(ExtendedMsgHdr));
        const AppId_t appId     = reqBody->m_unAppId;
        const uint32  accountId = static_cast<uint32>(reqHdr->m_ulSteamID & 0xFFFFFFFFull);

        std::optional<std::string> key = LegacyCDKey::Resolve(appId, accountId);
        if (!key) return false;

        const uint32 cchKey = static_cast<uint32>(key->size()) + 1;
        const uint32 total  = sizeof(ExtendedMsgHdr)
                            + sizeof(MsgClientGetLegacyGameKeyResponse) + cchKey;
        if (total > kMaxPacketSize) {
            LOG_NETPACKET_WARN("LegacyKey: app={} response too large ({} bytes), passing through",
                               appId, total);
            return false;
        }

        std::vector<uint8> pkt(total);
        auto* rh = reinterpret_cast<ExtendedMsgHdr*>(pkt.data());
        *rh = *reqHdr;
        rh->eMsg          = k_EMsgClientGetLegacyGameKeyResponse;
        rh->m_JobIDTarget = reqHdr->m_JobIDSource;
        rh->m_JobIDSource = k_GIDNil;

        auto* rb = reinterpret_cast<MsgClientGetLegacyGameKeyResponse*>(
                       pkt.data() + sizeof(ExtendedMsgHdr));
        rb->m_unAppId = appId;
        rb->m_eResult = k_EResultOK;
        rb->m_cchKey  = cchKey;
        memcpy(pkt.data() + sizeof(ExtendedMsgHdr) + sizeof(MsgClientGetLegacyGameKeyResponse),
               key->c_str(), cchKey);

        {
            std::lock_guard lk(g_queueMutex);
            if (g_pending.size() < 64)
                g_pending.push_back(std::move(pkt));
        }
        LOG_NETPACKET_DEBUG("LegacyKey: app={} answered locally ({} bytes, key '{}')",
                            appId, total, *key);
        return true;
    }

    void Drain(void* pThis, CNetPacket* pCarrier,
               bool (*invokeOriginal)(void*, CNetPacket*))
    {
        if (!NetPacketLayout::s_resolved.load(std::memory_order_acquire)) return;

        for (;;) {
            std::vector<uint8> pkt;
            {
                std::lock_guard lk(g_queueMutex);
                if (g_pending.empty()) return;
                pkt = std::move(g_pending.front());
                g_pending.pop_front();
            }

            if (pkt.empty() || pkt.size() > kMaxPacketSize) continue;

            std::span<uint8> slot = g_RecvPool.AcquireSlot();
            std::memcpy(slot.data(), pkt.data(), pkt.size());

            {
                CarrierScopeGuard guard(pCarrier, slot.data(), static_cast<uint32>(pkt.size()));
                invokeOriginal(pThis, pCarrier);
            }
            LOG_NETPACKET_DEBUG("LegacyKey: delivered {}-byte response", pkt.size());
        }
    }

    void Clear() {
        std::lock_guard lk(g_queueMutex);
        g_pending.clear();
    }

} // namespace Hooks_NetPacket_LegacyKey

// ════════════════════════════════════════════════════════════════
//  Ownership ticket on-demand fetch (eMsg 857/858).
//
//  Captures the WebSocket send context from outgoing frames, then
//  originates eMsg 857 (ClientGetAppOwnershipTicket) requests when
//  no ticket is in memory cache or ConfigStore.  The eMsg 858 recv
//  handler in RecvJob already caches arriving tickets via
//  Hooks_Decryption::CacheAppOwnershipTicket; WaitForCachedTicket
//  blocks until one arrives or times out.
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_TicketFetch {

    using SendFrameFn = bool(__fastcall*)(void*, EWebSocketOpCode, uint8*, uint32);

    std::mutex   g_ContextMutex;
    void*        g_pWebSocket = nullptr;
    SendFrameFn  g_sendFrame  = nullptr;
    std::vector<uint8> g_HdrTemplate;

    std::mutex                      g_Mutex;
    std::unordered_set<AppId_t>     g_Failed;
    uint64 g_NextJobId = 0x7E52'0000'0000'0000ull;

    void CaptureContext(void* pObject, SendFrameFn sendFrame,
                        const uint8* pHdr, uint32 cbHdr)
    {
        std::lock_guard<std::mutex> lock(g_ContextMutex);
        if (pObject)  g_pWebSocket = pObject;
        if (sendFrame) g_sendFrame = sendFrame;
        if (!g_HdrTemplate.empty() || !pHdr || cbHdr == 0) return;

        CMsgProtoBufHeader candidate;
        if (!candidate.ParseFromArray(pHdr, static_cast<int>(cbHdr)) ||
            !candidate.has_steamid() || candidate.steamid() == 0)
            return;

        g_HdrTemplate.assign(pHdr, pHdr + cbHdr);
    }

    bool Send(AppId_t appId)
    {
        std::vector<uint8> hdrTemplate;
        void* ws = nullptr;
        SendFrameFn send = nullptr;
        uint64 jobId = 0;
        {
            std::lock_guard<std::mutex> lock(g_ContextMutex);
            if (!g_pWebSocket || !g_sendFrame || g_HdrTemplate.empty()) {
                LOG_NETPACKET_WARN("OwnershipTicketFetch: no send context captured yet");
                return false;
            }
            hdrTemplate = g_HdrTemplate;
            ws   = g_pWebSocket;
            send = g_sendFrame;
        }
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            jobId = ++g_NextJobId;
        }

        CMsgProtoBufHeader hdr;
        if (!hdr.ParseFromArray(hdrTemplate.data(), static_cast<int>(hdrTemplate.size()))) {
            LOG_NETPACKET_WARN("OwnershipTicketFetch: header template failed to parse");
            return false;
        }
        hdr.clear_target_job_name();
        hdr.set_jobid_source(jobId);
        hdr.clear_jobid_target();

        // Build varint-encoded body: field 1 (app_id) as varint
        std::vector<uint8> body;
        uint32 value = appId;
        body.push_back(0x08); // field 1, wire type 0 (varint)
        while (value >= 0x80) {
            body.push_back(static_cast<uint8>(value) | 0x80);
            value >>= 7;
        }
        body.push_back(static_cast<uint8>(value));

        const uint32 cbHdr2 = static_cast<uint32>(hdr.ByteSizeLong());
        std::vector<uint8> frame(sizeof(MsgHdr) + cbHdr2 + body.size());
        auto* mhdr = reinterpret_cast<MsgHdr*>(frame.data());
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgClientGetAppOwnershipTicket) | kMsgHdrProtoFlag);
        mhdr->headerLength = cbHdr2;

        if (!hdr.SerializeToArray(frame.data() + sizeof(MsgHdr), static_cast<int>(cbHdr2))) {
            LOG_NETPACKET_WARN("OwnershipTicketFetch: failed to serialize header");
            return false;
        }
        std::memcpy(frame.data() + sizeof(MsgHdr) + cbHdr2, body.data(), body.size());

        LOG_NETPACKET_INFO("OwnershipTicketFetch send: app={} jobid={} ({} bytes)",
                           appId, jobId, frame.size());
        return send(ws, k_eWebSocketOpCode_Binary, frame.data(), static_cast<uint32>(frame.size()));
    }

    bool Request(AppId_t appId, uint32_t timeoutMs)
    {
        if (appId == 0) return false;
        if (!Hooks_Decryption::GetMemCachedAppOwnershipTicket(appId).empty()) return true;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            if (g_Failed.contains(appId)) return false;
        }
        if (!Send(appId)) return false;

        const auto ticket = Hooks_Decryption::WaitForCachedTicket(appId, timeoutMs);
        if (ticket.empty()) {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Failed.insert(appId);
            LOG_NETPACKET_WARN("OwnershipTicketFetch: no ticket for app {} within {} ms", appId, timeoutMs);
            return false;
        }

        LOG_NETPACKET_INFO("OwnershipTicketFetch: got ticket for app {} ({} bytes)", appId, ticket.size());
        return true;
    }

    void Reset() {
        {
            std::lock_guard<std::mutex> lock(g_ContextMutex);
            g_pWebSocket = nullptr;
            g_sendFrame  = nullptr;
            g_HdrTemplate.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            g_Failed.clear();
            g_NextJobId = 0x7E52'0000'0000'0000ull;
        }
    }

} // namespace Hooks_NetPacket_TicketFetch

// ════════════════════════════════════════════════════════════════
//  Dispatch
// ════════════════════════════════════════════════════════════════
namespace {

    bool SendServiceJob(const UnpackedPacket& pkt,
                        const CMsgProtoBufHeader& hdr,
                        std::span<uint8>& outFrame,
                        bool& outSuppress)
    {
        const std::string_view jobView(hdr.target_job_name());
        LOG_NETPACKET_DEBUG("Send target_job_name: {}", jobView);

        // Steam Cloud save redirection: hand every "Cloud.*" request to
        // CloudRedirect. If it answers, suppress the outbound frame (the
        // synthesized response is delivered from the RecvPkt hook).
        // ExitSyncDone/ConflictResolution are notifications that must reach
        // Steam's internal cloud state machine untouched.
        if (jobView.starts_with("Cloud.")) {
            if (jobView == "Cloud.SignalAppExitSyncDone#1") {
                if (Hooks_Misc::IsOnlineFixActive()) {
                    const uint32 exitAppId = Hooks_NetPacket_Cloud::ExtractAppId(jobView, pkt.body);
                    if (exitAppId == 0 || exitAppId == kOnlineFixAppId || exitAppId == Hooks_Misc::ResolveAppId()) {
                        Hooks_Misc::ResetOnlineFixState();
                    }
                }
                return false;
            }
            if (jobView == "Cloud.ClientConflictResolution#1")
                return false;
            if (Hooks_NetPacket_Cloud::HandleSend(jobView, pkt.body, hdr))
                outSuppress = true;
            return false;   // never body-replace a cloud frame
        }

        switch (Fnv1aHash(jobView)) {

        case HASH_JOB_GetUserStats:
            return Hooks_NetPacket_UserStats::HandleSend_GetUserStats(pkt, hdr, outFrame);

        case HASH_JOB_GetManifestRequestCode:
            return Hooks_NetPacket_Manifest::HandleSend(pkt.body, hdr);

        default:
            return false;
        }
    }

    void SendJob(const UnpackedPacket& pkt,
                 std::span<uint8>& outFrame,
                 bool& outReplaced,
                 bool& outSuppress)
    {
        outReplaced = false;
        outSuppress = false;
        outFrame    = {};

        if (!pkt.hdr.empty() &&
            pkt.eMsg != k_EMsgServiceMethodCallFromClient &&
            pkt.eMsg != k_EMsgClientGamesPlayed &&
            pkt.eMsg != k_EMsgClientGamesPlayedWithDataBlob &&
            Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed) == 0)
        {
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())) &&
                hdr.has_steamid() && hdr.steamid() != 0)
            {
                Hooks_NetPacket_RichPresence::TryCaptureLocalSteamId(hdr.steamid());
            }
        }

        LOG_NETPACKET_DEBUG("Send eMsg {}({}) (cbBody={}, cbHdr={})",
                        MsgName(pkt.eMsg), static_cast<uint32>(pkt.eMsg), pkt.body.size(), pkt.hdr.size());

        switch (pkt.eMsg) {

        case k_EMsgServiceMethodCallFromClient: {   // 151
            CMsgProtoBufHeader hdr;
            if (!pkt.hdr.empty() &&
                hdr.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())))
            {
                if (hdr.has_steamid() && hdr.steamid() != 0) {
                    Hooks_NetPacket_RichPresence::TryCaptureLocalSteamId(hdr.steamid());
                }
                if (hdr.has_target_job_name()) {
                    outReplaced = SendServiceJob(pkt, hdr, outFrame, outSuppress);
                }
            }
            return;
        }

        case k_EMsgClientPICSProductInfoRequest:     // 8903
            outReplaced = Hooks_NetPacket_AccessToken::HandleSend(pkt, outFrame);
            return;

        case k_EMsgClientGamesPlayed:                 // 742
        case k_EMsgClientGamesPlayedWithDataBlob:     // 5410
            outReplaced = Hooks_NetPacket_OnlineFix::HandleSend(pkt, outFrame);
            return;

        case k_EMsgClientRichPresenceUpload:           // 7501
            Hooks_NetPacket_RichPresence::TrackRPSend(pkt.body);
            return;

        case k_EMsgClientGetUserStats:               // 818
            outReplaced = Hooks_NetPacket_UserStats::HandleSend_ClientGetUserStats(pkt, outFrame);
            return;

        case k_EMsgClientStoreUserStats:              // 820
        case k_EMsgClientStoreUserStats2: {          // 5466
            AppId_t appId = 0;
            if (!pkt.body.empty()) {
                if (pkt.eMsg == k_EMsgClientStoreUserStats) {
                    CMsgClientStoreUserStats msg;
                    if (msg.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size())) && msg.has_game_id()) {
                        appId = ExtractSteamAppId(msg.game_id());
                    }
                } else {
                    CMsgClientStoreUserStats2 msg2;
                    if (msg2.ParseFromArray(pkt.body.data(), static_cast<int>(pkt.body.size())) && msg2.has_game_id()) {
                        appId = ExtractSteamAppId(msg2.game_id());
                    }
                }
            }
            if (appId == 0) return;

            appId = ResolveOnlineFixAppId(appId);
            if ((LuaConfig::HasDepot(appId) || CloudRedirectHost::IsApp(appId)) &&
                !Hooks_Package::HasValidLicense(appId)) {
                CloudRedirectHost::NotifyStatsStored(appId);
            }
            return;
        }

        default:
            return;
        }
    }

    void RecvServiceJob(CNetPacket* pPacket,
                        const UnpackedPacket& pkt,
                        const CMsgProtoBufHeader& hdr)
    {
        const std::string_view jobView(hdr.target_job_name());
        LOG_NETPACKET_DEBUG("Recv target_job_name: {}", jobView);

        switch (Fnv1aHash(jobView)) {

        case HASH_JOB_NotifyRunningApps:
            Hooks_NetPacket_FamilySharing::HandleRecv_NotifyRunningApps(pPacket, pkt);
            return;

        case HASH_JOB_GetUserStats:
            Hooks_NetPacket_UserStats::HandleRecv_GetUserStatsResponse(pPacket, pkt, hdr);
            return;

        case HASH_JOB_ClientGetLastPlayedTimes:
            Hooks_NetPacket_Playtime::HandleRecv_ClientGetLastPlayedTimesResponse(pPacket, pkt);
            return;

        case HASH_JOB_GetManifestRequestCode:
            Hooks_NetPacket_Manifest::HandleRecv(pPacket, pkt, hdr);
            return;

        default:
            break;
        }
    }

    void RecvJob(CNetPacket* pPacket, const UnpackedPacket& pkt)
    {
        if (pkt.eMsg == k_EMsgMulti) {
            LOG_NETPACKET_TRACE("Received k_EMsgMulti, skipping dispatch");
            return;
        }

        if (!pkt.hdr.empty() &&
            pkt.eMsg != k_EMsgServiceMethod &&
            pkt.eMsg != k_EMsgServiceMethodResponse &&
            pkt.eMsg != k_EMsgServiceMethodSendToClient &&
            pkt.eMsg != k_EMsgClientPersonaState &&
            Hooks_NetPacket_RichPresence::g_LocalSteamId.load(std::memory_order_relaxed) == 0)
        {
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())) &&
                hdr.has_steamid() && hdr.steamid() != 0)
            {
                Hooks_NetPacket_RichPresence::TryCaptureLocalSteamId(hdr.steamid());
            }
        }

        LOG_NETPACKET_DEBUG("Recv eMsg {}({}) (cbBody={}, cbHdr={})",
                        MsgName(pkt.eMsg), static_cast<uint32>(pkt.eMsg), pkt.body.size(), pkt.hdr.size());

        switch (pkt.eMsg) {

        case k_EMsgServiceMethod:                      // 146
        case k_EMsgServiceMethodResponse:              // 147
        case k_EMsgServiceMethodSendToClient: {        // 152
            CMsgProtoBufHeader hdr;
            if (!pkt.hdr.empty() &&
                hdr.ParseFromArray(pkt.hdr.data(), static_cast<int>(pkt.hdr.size())))
            {
                if (hdr.has_steamid() && hdr.steamid() != 0) {
                    Hooks_NetPacket_RichPresence::TryCaptureLocalSteamId(hdr.steamid());
                }
                if (hdr.has_target_job_name()) {
                    RecvServiceJob(pPacket, pkt, hdr);
                }
            }
            return;
        }

        case k_EMsgClientGetUserStatsResponse:     // 819
            Hooks_NetPacket_UserStats::HandleRecv_ClientGetUserStatsResponse(pPacket, pkt);
            return;

        case k_EMsgClientStoreUserStatsResponse:   // 821
            Hooks_NetPacket_UserStats::HandleRecv_ClientStoreUserStatsResponse(pPacket, pkt);
            return;

        case k_EMsgClientSharedLibraryLockStatus:      // 9405
        case k_EMsgClientSharedLibraryStopPlaying:     // 9406
            Hooks_NetPacket_FamilySharing::ClearBody(pPacket, pkt);
            return;

        case k_EMsgClientPersonaState:                 // 766
            Hooks_NetPacket_RichPresence::HandleRecv(pPacket, pkt);
            return;

        case k_EMsgClientGetAppOwnershipTicketResponse:   // 858
            Hooks_NetPacket_OwnershipTicket::HandleRecv(pPacket, pkt);
            return;

        default:
            return;
        }
    }

    // ════════════════════════════════════════════════════════════
    //  Hooks
    // ════════════════════════════════════════════════════════════

    HOOK_FUNC(BBuildAndAsyncSendFrame, bool,
              void* pObject, EWebSocketOpCode eWebSocketOpCode,
              uint8* pubData, uint32 cubData)
    {
        if (!oBBuildAndAsyncSendFrame) return false;

        HookScopeGuard hookGuard;
        if (!hookGuard.Entered() || eWebSocketOpCode != k_eWebSocketOpCode_Binary || !pubData || cubData < sizeof(MsgHdr)) {
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
        }

        // Intercept legacy product key request early before protobuf unpack
        if (cubData >= sizeof(MsgHdr)) {
            uint32 rawEMsg = 0;
            memcpy(&rawEMsg, pubData, sizeof(rawEMsg));
            if (!(rawEMsg & kMsgHdrProtoFlag) &&
                static_cast<EMsg>(rawEMsg) == k_EMsgClientGetLegacyGameKey &&
                Hooks_NetPacket_LegacyKey::HandleSend(pubData, cubData)) {
                return true;
            }
        }

        const auto pkt = UnpackRaw(pubData, cubData);
        if (!pkt) {
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
        }

        // Capture WebSocket context for originated eMsg 857 ticket requests
        Hooks_NetPacket_TicketFetch::CaptureContext(
            pObject, oBBuildAndAsyncSendFrame, nullptr, 0);
        if (!pkt->hdr.empty()) {
            Hooks_NetPacket_TicketFetch::CaptureContext(
                nullptr, nullptr, pkt->hdr.data(), static_cast<uint32>(pkt->hdr.size()));

            // Warm up ticket cache on first authenticated frame
            CMsgProtoBufHeader loginHdr;
            if (loginHdr.ParseFromArray(pkt->hdr.data(), static_cast<int>(pkt->hdr.size())) &&
                loginHdr.has_steamid() && loginHdr.steamid() != 0)
            {
                static std::once_flag ticketWarmOnce;
                std::call_once(ticketWarmOnce, [] {
                    OSTPlatform::Thread::StartDetached([] {
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        constexpr AppId_t kSteamClientAppId = 7;
                        if (!Hooks_Decryption::GetCacheAppOwnershipTicket(kSteamClientAppId).empty() ||
                            !Hooks_Decryption::GetMemCachedAppOwnershipTicket(kSteamClientAppId).empty())
                            return 0u;
                        Hooks_NetPacket_TicketFetch::Request(kSteamClientAppId, 4000);
                        return 0u;
                    });
                });
            }
        }

        std::span<uint8> modifiedFrame{};
        bool replaced = false;
        bool suppress = false;

        SendJob(*pkt, modifiedFrame, replaced, suppress);

        if (suppress) {
            // CloudRedirect answered this RPC locally; drop the frame.
            return true;
        }

        if (replaced && !modifiedFrame.empty()) {
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode,
                                            modifiedFrame.data(),
                                            static_cast<uint32>(modifiedFrame.size()));
        }

        return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
    }

    HOOK_FUNC(RecvPkt, void*, void* pThis, CNetPacket* pPacket)
    {
        if (!oRecvPkt) return nullptr;

        HookScopeGuard hookGuard;
        if (!hookGuard.Entered() || !pPacket) {
            return oRecvPkt(pThis, pPacket);
        }

        // 1. Resilient adaptive probing for CNetPacket dynamic layout offsets
        if (!NetPacketLayout::s_resolved.load(std::memory_order_acquire)) {
            ProbeNetPacketLayout(pPacket);
            if (!NetPacketLayout::s_resolved.load(std::memory_order_acquire)) {
                // Silent Fallback: pass through cleanly without any errors or popups
                return oRecvPkt(pThis, pPacket);
            }
        }

        // 2. Safe access via resolved dynamic offsets
        const uint8* pubData = pPacket->PubData();
        const uint32 cubData = pPacket->CubData();
        if (!pubData || cubData < sizeof(MsgHdr)) {
            return oRecvPkt(pThis, pPacket);
        }

        if (const auto pkt = UnpackRaw(pubData, cubData)) {
            RecvJob(pPacket, *pkt);
        }

        // Deliver manufactured/queued responses in lockless mode with RAII restoration
        Hooks_NetPacket_RichPresence::TryInject(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        Hooks_NetPacket_Cloud::Drain(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        Hooks_NetPacket_LegacyKey::Drain(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        return oRecvPkt(pThis, pPacket);
    }

} // anonymous namespace


namespace Hooks_NetPacket {
    bool RequestOwnershipTicket(AppId_t appId, uint32_t timeoutMs) {
        return Hooks_NetPacket_TicketFetch::Request(appId, timeoutMs);
    }

    void Install() {
        g_ShuttingDown.store(false, std::memory_order_release);
        RESOLVE_C(PchMsgNameFromEMsg);
        HOOK_BEGIN();
        INSTALL_HOOK_C(BBuildAndAsyncSendFrame);
        INSTALL_HOOK_C(RecvPkt);
        HOOK_END();
    }

    void Uninstall() {
        g_ShuttingDown.store(true, std::memory_order_seq_cst);

        // Wait up to 200ms for active hook callbacks (including any interrupted Manifest wait) to exit
        for (int i = 0; i < 200 && g_ActiveHookCalls.load(std::memory_order_acquire) != 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BBuildAndAsyncSendFrame);
        UNINSTALL_HOOK(RecvPkt);
        UNHOOK_END();

        oBBuildAndAsyncSendFrame = nullptr;
        oRecvPkt                 = nullptr;
        oPchMsgNameFromEMsg      = nullptr;

        {
            std::lock_guard lock(Hooks_NetPacket_UserStats::g_JobMutex);
            Hooks_NetPacket_UserStats::g_JobIdToPendingStats->clear();
            Hooks_NetPacket_UserStats::g_JobSeq = 0;
        }
        {
            std::lock_guard lock(Hooks_NetPacket_Manifest::g_CodeMutex);
            Hooks_NetPacket_Manifest::g_PendingJobs->clear();
            Hooks_NetPacket_Manifest::g_InFlightFutures->clear();
            Hooks_NetPacket_Manifest::g_ManifestCodeCache->clear();
            Hooks_NetPacket_Manifest::g_ManifestSeq = 0;
        }
        {
            std::lock_guard lock(Hooks_NetPacket_RichPresence::g_RPMutex);
            Hooks_NetPacket_RichPresence::g_RPKvsByAppId->clear();
            Hooks_NetPacket_RichPresence::g_PlayingAppId.store(0, std::memory_order_release);
            Hooks_NetPacket_RichPresence::g_LocalSteamId.store(0, std::memory_order_release);
            Hooks_NetPacket_RichPresence::g_InjectPending.store(false, std::memory_order_release);
            Hooks_NetPacket_RichPresence::g_cbInjectPkt    = 0;
            Hooks_NetPacket_RichPresence::g_cbSelfHdr      = 0;
            Hooks_NetPacket_RichPresence::g_cbSelfBody     = 0;
            Hooks_NetPacket_RichPresence::g_HaveSelfCached = false;
        }
        {
            std::lock_guard lock(Hooks_NetPacket_Cloud::g_queueMutex);
            Hooks_NetPacket_Cloud::g_pending->clear();
            Hooks_NetPacket_Cloud::g_hasPending.store(false, std::memory_order_release);
            Hooks_NetPacket_LegacyKey::Clear();
        }

        Hooks_NetPacket_TicketFetch::Reset();
        NetPacketLayout::Reset();

        for (int i = 0; i < 50 && Hooks_NetPacket_Manifest::g_ActiveFetchThreads.load(std::memory_order_acquire) != 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
