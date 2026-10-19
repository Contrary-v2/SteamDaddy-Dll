#pragma once

#include "AppInfoParser.h"
#include "Crypto.h"
#include "SteamSession.h"
#include "SteamWire.h"
#include "VdfParser.h"
#include "WinHttpTransport.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OST::ExtractTickets {

struct DepotKeyQuery {
    uint32_t depotId{0};
    uint32_t appId{0};
};

struct ExtractedAppCredentials {
    uint32_t appId{0};
    std::optional<std::vector<uint8_t>> appOwnershipTicket;
    std::optional<std::vector<uint8_t>> encryptedAppTicket;
    std::vector<DepotKeyInfo> depotKeys;
    std::vector<DlcInfo> dlcs;
    std::unordered_map<uint32_t, uint64_t> appTokens;
};

class SteamCmClient {
public:
    SteamCmClient();
    ~SteamCmClient();

    // Connects to Steam CM WebSocket and executes CMsgClientLogon with refreshToken
    [[nodiscard]] bool ConnectAndLogon(uint64_t steamId, std::string_view refreshToken, std::string_view accessToken = "");

    // Disconnects and shuts down WebSocket session
    void Disconnect();

    // Requests server-signed AppOwnershipTicket (eMsg 5560)
    [[nodiscard]] std::optional<std::vector<uint8_t>> RequestAppOwnershipTicket(uint32_t appId);

    // Requests EncryptedAppTicket (eMsg 5527)
    [[nodiscard]] std::optional<std::vector<uint8_t>> RequestEncryptedAppTicket(uint32_t appId);

    // Requests 32-byte AES depot decryption keys for depot queries (eMsg 5438) with two-tier fallback
    [[nodiscard]] std::vector<DepotKeyInfo> RequestDepotKeys(
        const std::vector<DepotKeyQuery>& queries, uint32_t fallbackAppId = 0);

    // Backward-compatible overload querying all depots with a single appId
    [[nodiscard]] std::vector<DepotKeyInfo> RequestDepotKeys(
        uint32_t appId, const std::vector<uint32_t>& depotIds);

    // Requests 64-bit PICS access tokens for app IDs (eMsg 8901)
    [[nodiscard]] std::unordered_map<uint32_t, uint64_t> RequestAppTokens(const std::vector<uint32_t>& appIds);

    // Requests PICS AppInfo product metadata (eMsg 8903)
    [[nodiscard]] std::optional<ParsedAppInfoData> RequestPicsProductInfo(uint32_t appId, uint64_t accessToken = 0);

    // Batch requests PICS AppInfo product metadata (eMsg 8903)
    [[nodiscard]] std::vector<ParsedAppInfoData> RequestPicsProductInfoBatch(
        const std::vector<uint32_t>& appIds,
        const std::unordered_map<uint32_t, uint64_t>& appTokens = {});

    // Fast batch resolution of official app/DLC names via Steam CM PICS (eMsg 8903)
    [[nodiscard]] std::unordered_map<uint32_t, std::string> RequestPicsAppNames(
        const std::vector<uint32_t>& appIds, bool* outSuccess = nullptr);

    // Resolves manifest request code via CM ServiceMethod or legacy RPC
    [[nodiscard]] std::string FetchManifestRequestCode(
        uint32_t appId, uint32_t depotId, const std::string& manifestId, bool* outAccessDenied = nullptr);

    // Dynamically queries active Steam CDN servers from GetServersForSteamPipe
    [[nodiscard]] static std::vector<std::string> GetCdnServers(uint32_t cellId = 0);

    // Downloads manifest binary directly from Steam CDN via GetManifestRequestCode
    [[nodiscard]] std::optional<std::string> DownloadManifestOnline(
        uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& destDir,
        bool* outAccessDenied = nullptr, WinHttpTransport* sharedHttp = nullptr);

    // Downloads and decompresses manifest binary directly from Steam CDN via an already resolved reqCode
    [[nodiscard]] static std::optional<std::string> DownloadManifestPayload(
        uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& reqCode,
        const std::string& destDir, const std::vector<std::string>& cdnServers, WinHttpTransport* http = nullptr);

    // Promotes a validated fast CDN server to the head of the cached server list
    static void PromoteWorkingCdnServer(std::string_view server);

    using ProgressCallback = std::function<void(std::string_view stage, size_t current, size_t total, uint32_t depotId)>;

    // High-level extraction pipeline for a single target AppID
    [[nodiscard]] ExtractedAppCredentials ExtractFullCredentials(
        uint32_t appId,
        bool forceEticket = false,
        ProgressCallback onProgress = nullptr);

    void SetAccessToken(std::string_view token);
    void SetRefreshToken(std::string_view token);
    [[nodiscard]] std::optional<std::string> RefreshAccessTokenViaCm(std::string* outNewRefreshToken = nullptr);
    [[nodiscard]] uint64_t GetSteamId() const noexcept { return m_steamId; }
    [[nodiscard]] int32_t GetLastLogonEResult() const noexcept { return m_lastLogonEResult; }
    [[nodiscard]] bool IsConnected() const noexcept { return m_ws.IsConnected() && m_isLoggedOn; }
    [[nodiscard]] bool EnsureConnected();

private:
    [[nodiscard]] bool SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId = 0, std::string_view targetJobName = {});
    [[nodiscard]] bool ReadMatchingMsg(
        uint32_t expectedEMsg,
        std::vector<uint8_t>& outBody,
        uint32_t timeoutMs = 8000,
        int32_t* outEResult = nullptr,
        uint64_t expectedJobId = 0);
    [[nodiscard]] bool SendAndReceiveWithRetry(
        ESteamMsg reqEMsg,
        const ProtoWriter& reqBody,
        uint32_t respEMsg,
        std::vector<uint8_t>& outBody,
        uint32_t timeoutMs = 8000,
        int32_t* outEResult = nullptr,
        std::string_view targetJobName = {},
        uint64_t* outJobId = nullptr);
    void UnpackMultiMsg(std::span<const uint8_t> bodySpan);

    struct QueuedMsg {
        uint32_t eMsg{0};
        uint64_t targetJobId{0};
        int32_t eresult{1};
        std::vector<uint8_t> body;

        QueuedMsg() = default;
        QueuedMsg(uint32_t e, uint64_t job, int32_t res, std::vector<uint8_t>&& b) noexcept
            : eMsg(e), targetJobId(job), eresult(res), body(std::move(b)) {
            SecureWipeBuffer(b);
        }
        ~QueuedMsg() noexcept {
            SecureWipeBuffer(body);
        }
        QueuedMsg(const QueuedMsg&) = default;
        QueuedMsg(QueuedMsg&& other) noexcept
            : eMsg(other.eMsg), targetJobId(other.targetJobId), eresult(other.eresult), body(std::move(other.body)) {
            SecureWipeBuffer(other.body);
        }
        QueuedMsg& operator=(const QueuedMsg& other) {
            if (this != &other) {
                SecureWipeBuffer(body);
                eMsg = other.eMsg;
                targetJobId = other.targetJobId;
                eresult = other.eresult;
                body = other.body;
            }
            return *this;
        }
        QueuedMsg& operator=(QueuedMsg&& other) noexcept {
            if (this != &other) {
                SecureWipeBuffer(body);
                eMsg = other.eMsg;
                targetJobId = other.targetJobId;
                eresult = other.eresult;
                body = std::move(other.body);
                SecureWipeBuffer(other.body);
            }
            return *this;
        }
    };

    WebSocketClient m_ws;
    std::vector<QueuedMsg> m_msgQueue;
    std::string m_refreshToken;
    std::string m_accessToken;
    uint64_t m_steamId{0};
    uint64_t m_nextJobId{100};
    int32_t m_clientSessionId{0};
    uint32_t m_cellId{0};
    int32_t m_lastLogonEResult{1};
    bool m_isLoggedOn{false};
    bool m_reconnecting{false};
};

} // namespace OST::ExtractTickets
