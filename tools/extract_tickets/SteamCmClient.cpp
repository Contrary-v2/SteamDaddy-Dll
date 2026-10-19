#include "SteamCmClient.h"
#include "AppInfoParser.h"
#include "Crypto.h"
#include "I18n.h"
#include "JsonHelper.h"
#include "Log.h"
#include "TuiEngine.h"
#include "Utils.h"
#include "tinf.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace OST::ExtractTickets {

namespace {
    std::vector<std::string> s_cachedCdnServers;
    std::mutex s_cdnMutex;

    std::vector<std::string> s_cachedCmList;
    std::mutex s_cmListMutex;

    void PromoteWorkingCmServer(std::string_view endpoint) {
        if (endpoint.empty()) return;
        std::lock_guard lock(s_cmListMutex);
        auto it = std::find(s_cachedCmList.begin(), s_cachedCmList.end(), endpoint);
        if (it != s_cachedCmList.end() && it != s_cachedCmList.begin()) {
            std::rotate(s_cachedCmList.begin(), it, it + 1);
        }
    }

    std::vector<std::string> QueryCmWebSockets() {
        std::lock_guard lock(s_cmListMutex);
        if (!s_cachedCmList.empty()) {
            return s_cachedCmList;
        }

        std::vector<std::string> endpoints;
        WinHttpTransport http;
        LOG_DEBUG("SteamCM", MsgKey::LogCmQueryingCmList);
        HttpResponse resp = http.Get("https://api.steampowered.com/ISteamDirectory/GetCMList/v1/?cellid=0", {}, 6000);
        if (resp.IsSuccess()) {
            auto list = JsonHelper::GetStringArray(resp.body, "serverlist_websockets");
            for (const auto& item : list) {
                // Prefer 443 endpoints for standard TLS WebSocket
                if (item.find(":443") != std::string::npos) {
                    endpoints.push_back(item);
                }
            }
            if (endpoints.empty()) {
                endpoints = std::move(list);
            }
            LOG_DEBUG("SteamCM", MsgKey::LogCmGotWsEndpoints, endpoints.size());
        } else {
            LOG_WARN("SteamCM", MsgKey::LogCmQueryCmListFailed, resp.statusCode, resp.errorMessage);
        }

        if (endpoints.empty()) {
            endpoints = {
                "cmp1-ord1.steamserver.net:443",
                "cmp2-ord1.steamserver.net:443",
                "cmp1-iad1.steamserver.net:443",
                "cmp2-iad1.steamserver.net:443",
                "cmp1-sea1.steamserver.net:443",
                "cmp2-sea1.steamserver.net:443",
                "cmp1-lax1.steamserver.net:443",
                "cmp1-fra1.steamserver.net:443"
            };
            LOG_INFO("SteamCM", MsgKey::LogCmUsingFallbackCluster, endpoints.size());
        }

        s_cachedCmList = endpoints;
        return s_cachedCmList;
    }
} // namespace

SteamCmClient::SteamCmClient() = default;

SteamCmClient::~SteamCmClient() {
    Disconnect();
    SecureWipeString(m_refreshToken);
    SecureWipeString(m_accessToken);
}

void SteamCmClient::Disconnect() {
    if (m_ws.IsConnected()) {
        m_ws.Close();
    }
    m_msgQueue.clear();
    m_clientSessionId = 0;
    m_isLoggedOn = false;
}

bool SteamCmClient::SendProtoMsg(ESteamMsg eMsg, const ProtoWriter& body, uint64_t jobId, std::string_view targetJobName) {
    if (eMsg != ESteamMsg::ClientLogon) {
        if (!EnsureConnected()) return false;
    } else {
        if (!m_ws.IsConnected()) return false;
    }

    auto packet = PackSteamMsg(eMsg, m_steamId, jobId, body.Data(), m_clientSessionId, targetJobName);
    ScopedBufferWiper packetGuard{packet};

    LOG_TRACE("SteamCM", MsgKey::LogCmSendingEMsg, static_cast<uint32_t>(eMsg), packet.size());
    if (!m_ws.Send(packet, true)) {
        if (eMsg != ESteamMsg::ClientLogon && !m_reconnecting) {
            LOG_WARN("SteamCM", MsgKey::LogCmSendFailedReconnecting, static_cast<uint32_t>(eMsg));
            m_ws.Close();
            m_isLoggedOn = false;
            if (EnsureConnected()) {
                SecureWipeBuffer(packet);
                packet = PackSteamMsg(eMsg, m_steamId, jobId, body.Data(), m_clientSessionId, targetJobName);
                LOG_INFO("SteamCM", MsgKey::LogCmReconnectedResending, static_cast<uint32_t>(eMsg), jobId);
                return m_ws.Send(packet, true);
            }
        }
        return false;
    }
    return true;
}

void SteamCmClient::UnpackMultiMsg(std::span<const uint8_t> bodySpan) {
    ProtoReader reader(bodySpan);
    ProtoField field;
    uint32_t sizeUnzipped = 0;
    std::span<const uint8_t> messageBody;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1) { // size_unzipped
            sizeUnzipped = static_cast<uint32_t>(field.varintVal);
        } else if (field.fieldNumber == 2) { // message_body
            messageBody = field.bytesVal;
        }
    }

    if (messageBody.empty()) return;

    constexpr uint32_t kMaxMultiUnzippedSize = 64 * 1024 * 1024;
    if (sizeUnzipped > kMaxMultiUnzippedSize) {
        LOG_ERROR("SteamCM", MsgKey::LogCmUnpackMultiFailed, -1, sizeUnzipped, messageBody.size());
        return;
    }

    std::vector<uint8_t> payload;
    ScopedBufferWiper payloadWiper{payload};
    if (sizeUnzipped > 0) {
        payload.resize(sizeUnzipped);
        unsigned int destLen = sizeUnzipped;
        int res = TINF_DATA_ERROR;

        if (messageBody.size() >= 2 && messageBody[0] == 0x1F && messageBody[1] == 0x8B) {
            res = tinf_gzip_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
        }
        if (res != TINF_OK) {
            destLen = sizeUnzipped;
            res = tinf_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
        }
        while (res == TINF_BUF_ERROR && payload.size() < kMaxMultiUnzippedSize) {
            const size_t nextSize = std::min<size_t>(payload.size() * 2, kMaxMultiUnzippedSize);
            if (nextSize <= payload.size()) break;
            destLen = static_cast<unsigned int>(nextSize);
            SecureWipeBuffer(payload);
            payload.resize(destLen);
            if (messageBody.size() >= 2 && messageBody[0] == 0x1F && messageBody[1] == 0x8B) {
                res = tinf_gzip_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
            } else {
                res = tinf_uncompress(payload.data(), &destLen, messageBody.data(), static_cast<unsigned int>(messageBody.size()));
            }
        }

        if (res != TINF_OK) {
            LOG_ERROR("SteamCM", MsgKey::LogCmUnpackMultiFailed,
                      res, sizeUnzipped, messageBody.size());
            return;
        }
        payload.resize(destLen);
    } else {
        if (messageBody.size() > kMaxMultiUnzippedSize) {
            LOG_ERROR("SteamCM", MsgKey::LogCmUnpackMultiFailed, -1, 0, messageBody.size());
            return;
        }
        payload.assign(messageBody.begin(), messageBody.end());
    }

    constexpr size_t kMaxMsgQueueDepth = 512;
    size_t offset = 0;
    while (offset + 4 <= payload.size()) {
        uint32_t subSize = 0;
        std::memcpy(&subSize, payload.data() + offset, 4);
        offset += 4;
        if (offset + subSize > payload.size()) break;

        std::span<const uint8_t> subPacket(payload.data() + offset, subSize);
        offset += subSize;

        uint32_t subEMsg = 0;
        std::span<const uint8_t> subHdr;
        std::span<const uint8_t> subBody;
        if (UnpackSteamMsg(subPacket, subEMsg, subHdr, subBody)) {
            const auto hdr = ParseSteamMsgHeader(subHdr);
            if (hdr.clientSessionId != 0) {
                m_clientSessionId = hdr.clientSessionId;
            }
            if (m_steamId == 0 && hdr.steamId != 0) {
                m_steamId = hdr.steamId;
            }
            LOG_TRACE("SteamCM", MsgKey::LogCmMultiSubMsg,
                      subEMsg, hdr.eresult, hdr.targetJobId, subBody.size());

            if (subEMsg == static_cast<uint32_t>(ESteamMsg::ClientLoggedOff)) {
                int32_t offEresult = 0;
                ProtoReader offR(subBody);
                ProtoField offF;
                while (offR.ReadNext(offF)) {
                    if (offF.fieldNumber == 1) offEresult = static_cast<int32_t>(offF.varintVal);
                }
                LOG_WARN("SteamCM", MsgKey::LogCmClientLoggedOff, offEresult);
            }

            if (subEMsg == static_cast<uint32_t>(ESteamMsg::ClientHeartBeat)) {
                ProtoWriter hb;
                (void)SendProtoMsg(ESteamMsg::ClientHeartBeat, hb);
                continue;
            }

            if (m_msgQueue.size() >= kMaxMsgQueueDepth) {
                m_msgQueue.erase(m_msgQueue.begin());
            }
            m_msgQueue.emplace_back(subEMsg, hdr.targetJobId, hdr.eresult, std::vector<uint8_t>(subBody.begin(), subBody.end()));
        }
    }
}

bool SteamCmClient::ReadMatchingMsg(
    uint32_t expectedEMsg,
    std::vector<uint8_t>& outBody,
    uint32_t timeoutMs,
    int32_t* outEResult,
    uint64_t expectedJobId) {

    SecureWipeBuffer(outBody);
    auto startTime = std::chrono::steady_clock::now();

    auto isMatch = [&](uint32_t eMsg, uint64_t targetJobId) {
        if (eMsg != expectedEMsg) return false;
        if (expectedJobId != 0 && targetJobId != 0 && targetJobId != expectedJobId) return false;
        return true;
    };

    constexpr size_t kMaxMsgQueueDepth = 512;

    while (true) {
        // 1. Check if expected message already in queue
        for (auto it = m_msgQueue.begin(); it != m_msgQueue.end(); ++it) {
            if (isMatch(it->eMsg, it->targetJobId)) {
                if (outEResult) *outEResult = it->eresult;
                outBody = std::move(it->body);
                m_msgQueue.erase(it);
                return true;
            }
        }

        // 2. Check elapsed timeout
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
        if (elapsed >= static_cast<long long>(timeoutMs)) {
            LOG_WARN("SteamCM", MsgKey::LogCmWaitMsgTimeout, expectedEMsg, expectedJobId, timeoutMs);
            return false;
        }

        // 3. Receive next WebSocket frame
        std::vector<uint8_t> frame;
        ScopedBufferWiper frameWiper{frame};
        bool isBinary = false;
        uint32_t remainingMs = static_cast<uint32_t>(timeoutMs - elapsed);
        if (!m_ws.Receive(frame, isBinary, remainingMs)) {
            return false;
        }

        uint32_t eMsg = 0;
        std::span<const uint8_t> hdrSpan;
        std::span<const uint8_t> bodySpan;

        if (UnpackSteamMsg(frame, eMsg, hdrSpan, bodySpan)) {
            const auto hdr = ParseSteamMsgHeader(hdrSpan);
            if (hdr.clientSessionId != 0) {
                m_clientSessionId = hdr.clientSessionId;
            }
            if (m_steamId == 0 && hdr.steamId != 0) {
                m_steamId = hdr.steamId;
            }
            LOG_TRACE("SteamCM", MsgKey::LogCmRecvEMsg,
                      eMsg, hdr.eresult, hdr.targetJobId, bodySpan.size());

            if (eMsg == static_cast<uint32_t>(ESteamMsg::ClientLoggedOff)) {
                int32_t offEresult = 0;
                ProtoReader offR(bodySpan);
                ProtoField offF;
                while (offR.ReadNext(offF)) {
                    if (offF.fieldNumber == 1) offEresult = static_cast<int32_t>(offF.varintVal);
                }
                LOG_WARN("SteamCM", MsgKey::LogCmClientLoggedOff, offEresult);
            }

            // If heartbeat request from server, reply ClientHeartBeat
            if (eMsg == static_cast<uint32_t>(ESteamMsg::ClientHeartBeat)) {
                ProtoWriter hb;
                (void)SendProtoMsg(ESteamMsg::ClientHeartBeat, hb);
                continue;
            }

            // If multi-container message, unpack all sub-messages into queue
            if (eMsg == 1) { // k_EMsgMulti
                UnpackMultiMsg(bodySpan);
                for (auto it = m_msgQueue.begin(); it != m_msgQueue.end(); ++it) {
                    if (isMatch(it->eMsg, it->targetJobId)) {
                        if (outEResult) *outEResult = it->eresult;
                        outBody = std::move(it->body);
                        m_msgQueue.erase(it);
                        return true;
                    }
                }
                continue;
            }

            if (isMatch(eMsg, hdr.targetJobId)) {
                if (outEResult) *outEResult = hdr.eresult;
                outBody.assign(bodySpan.begin(), bodySpan.end());
                return true;
            }

            if (m_msgQueue.size() >= kMaxMsgQueueDepth) {
                m_msgQueue.erase(m_msgQueue.begin());
            }
            m_msgQueue.emplace_back(eMsg, hdr.targetJobId, hdr.eresult, std::vector<uint8_t>(bodySpan.begin(), bodySpan.end()));
        }
    }
}

bool SteamCmClient::SendAndReceiveWithRetry(
    ESteamMsg reqEMsg,
    const ProtoWriter& reqBody,
    uint32_t respEMsg,
    std::vector<uint8_t>& outBody,
    uint32_t timeoutMs,
    int32_t* outEResult,
    std::string_view targetJobName,
    uint64_t* outJobId) {

    uint64_t jobId = ++m_nextJobId;
    if (outJobId) *outJobId = jobId;

    if (SendProtoMsg(reqEMsg, reqBody, jobId, targetJobName)) {
        if (ReadMatchingMsg(respEMsg, outBody, timeoutMs, outEResult, jobId)) {
            return true;
        }
    }

    // If the WebSocket closed during send or receive (e.g. half-closed idle keep-alive socket), reconnect and retry once
    if (!m_ws.IsConnected() && !m_reconnecting) {
        m_isLoggedOn = false;
        if (EnsureConnected()) {
            jobId = ++m_nextJobId;
            if (outJobId) *outJobId = jobId;
            if (SendProtoMsg(reqEMsg, reqBody, jobId, targetJobName)) {
                return ReadMatchingMsg(respEMsg, outBody, timeoutMs, outEResult, jobId);
            }
        }
    }
    return false;
}

bool SteamCmClient::ConnectAndLogon(uint64_t steamId, std::string_view refreshToken, std::string_view accessToken) {
    std::string nextRefresh(refreshToken);
    std::string nextAccess(accessToken);

    Disconnect();
    m_steamId = steamId;
    SecureWipeString(m_refreshToken);
    SecureWipeString(m_accessToken);
    m_refreshToken = std::move(nextRefresh);
    m_accessToken = std::move(nextAccess);
    m_lastLogonEResult = 2; // k_EResultFail

    std::string_view tokenToUse = !m_refreshToken.empty() ? m_refreshToken : m_accessToken;
    if (tokenToUse.empty()) {
        LOG_ERROR("SteamCM", MsgKey::LogCmMissingTokens);
        return false;
    }

    if (m_steamId == 0) {
        m_steamId = GetJwtSteamId(tokenToUse);
    }

    auto cmList = QueryCmWebSockets();
    if (cmList.empty()) {
        LOG_ERROR("SteamCM", MsgKey::LogCmEmptyCmList);
        return false;
    }

    bool logonSuccess = false;
    const size_t tryLimit = std::min<size_t>(cmList.size(), 8);

    for (size_t i = 0; i < tryLimit; ++i) {
        Disconnect();

        const auto& endpoint = cmList[i];
        std::string wsUrl = std::format("wss://{}/cmsocket/", endpoint);
        LOG_DEBUG("SteamCM", MsgKey::LogCmConnectingNode, i + 1, tryLimit, wsUrl);

        if (TuiEngine::IsActive()) {
            if (!m_reconnecting) {
                int w = 80, h = 25;
                TuiEngine::GetScreenSize(w, h);
                const int modalW = std::clamp(w - 12, 64, 84);
                const int modalH = 6;
                const int top = (h - modalH) / 2, left = (w - modalW) / 2;
                std::string connMsg = TR_FMT(MsgKey::ConnectingNodeProgress, i + 1, tryLimit);
                TuiEngine::PrintBounded(top + 2, left + 4, connMsg, static_cast<size_t>(modalW - 8), "\x1b[1;36m");
                std::cout.flush();
            }
        } else {
            std::cout << TR_FMT(MsgKey::CliConnectingServer, i + 1, tryLimit, endpoint) << "\n";
        }

        if (!m_ws.Connect(wsUrl, 5000)) {
            LOG_WARN("SteamCM", MsgKey::LogCmConnectNodeFailed, endpoint);
            continue;
        }

        LOG_DEBUG("SteamCM", MsgKey::LogCmChannelReadyLoggingIn,
                  endpoint, MaskSteamId(m_steamId), MaskToken(tokenToUse));

        // Build CMsgClientLogon strictly adhering to Valve Protobuf specifications
        ProtoWriter logonBody;
        logonBody.WriteUInt32(1, 65580);                          // protocol_version
        logonBody.WriteUInt32(3, 0);                              // cell_id
        logonBody.WriteString(6, I18n::GetSteamLanguageCode());   // client_language
        logonBody.WriteUInt32(7, 16);                             // client_os_type (Windows 10/11)
        logonBody.WriteBool(8, true);                             // should_remember_password
        if (m_steamId != 0) {
            logonBody.WriteFixed64(22, m_steamId);                // client_supplied_steam_id
        }
        logonBody.WriteUInt32(33, 2);                             // chat_mode = 2 (NewSteamChat)
        logonBody.WriteBool(102, true);                           // supports_rate_limit_response
        logonBody.WriteString(108, tokenToUse);                   // access_token (Valve refresh_token)

        if (!SendProtoMsg(ESteamMsg::ClientLogon, logonBody)) {
            LOG_WARN("SteamCM", MsgKey::LogCmSendLogonFailed, endpoint);
            Disconnect();
            continue;
        }

        std::vector<uint8_t> respBody;
        ScopedBufferWiper respWiper{respBody};
        if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientLogonResponse), respBody, 5000)) {
            LOG_WARN("SteamCM", MsgKey::LogCmLogonTimeout, endpoint);
            Disconnect();
            continue;
        }

        ProtoReader reader(respBody);
        ProtoField field;
        int32_t eresult = 2; // k_EResultFail

        while (reader.ReadNext(field)) {
            if (field.fieldNumber == 1) { // eresult
                eresult = static_cast<int32_t>(field.varintVal);
            } else if (field.fieldNumber == 7) { // cell_id (CMsgClientLogonResponse field 7)
                m_cellId = static_cast<uint32_t>(field.varintVal);
            } else if (field.fieldNumber == 20) { // client_supplied_steamid
                if (field.fixed64Val != 0) {
                    m_steamId = field.fixed64Val;
                }
            }
        }

        m_lastLogonEResult = eresult;

        if (eresult == 1) { // 1 == k_EResultOK
            m_isLoggedOn = true;
            logonSuccess = true;
            PromoteWorkingCmServer(endpoint);
            LOG_INFO("SteamCM", MsgKey::LogCmLogonSuccess,
                     endpoint, MaskSteamId(m_steamId), m_clientSessionId, m_cellId);
            if (!TuiEngine::IsActive()) {
                std::cout << TR_FMT(MsgKey::CliLogonReady, m_steamId) << "\n";
            }
            break;
        }

        if (eresult == 48 || eresult == 20) { // 48: TryAnotherCM, 20: ServiceUnavailable
            LOG_WARN("SteamCM", MsgKey::LogCmNodeSwitchEResult,
                     endpoint, eresult, eresult == 48 ? "TryAnotherCM" : "ServiceUnavailable");
            Disconnect();
            continue;
        }

        std::string reason;
        switch (eresult) {
            case 2:  reason = std::string(TR(MsgKey::CmErrGenericFail)); break;
            case 5:  reason = std::string(TR(MsgKey::CmErrAccessDenied)); break;
            case 6:  reason = std::string(TR(MsgKey::CmErrLoggedInElsewhere)); break;
            case 8:  reason = std::string(TR(MsgKey::CmErrInvalidToken)); break;
            case 15: reason = std::string(TR(MsgKey::CmErrNoPermission)); break;
            case 25: reason = std::string(TR(MsgKey::CmErrLimitExceeded)); break;
            default: reason = TR_FMT(MsgKey::CmErrUnknownCode, eresult); break;
        }
        LOG_ERROR("SteamCM", MsgKey::LogCmLogonRejected, eresult, reason);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::CliCmLogonFailed, eresult, reason) << "\n";
        }
        Disconnect();
        return false;
    }

    if (!logonSuccess) {
        {
            std::lock_guard lock(s_cmListMutex);
            s_cachedCmList.clear();
        }
        LOG_ERROR("SteamCM", MsgKey::LogCmAllNodesFailed);
        Disconnect();
        return false;
    }

    return true;
}

void SteamCmClient::SetAccessToken(std::string_view token) {
    std::string nextAccess(token);
    SecureWipeString(m_accessToken);
    m_accessToken = std::move(nextAccess);
}

void SteamCmClient::SetRefreshToken(std::string_view token) {
    std::string nextRefresh(token);
    SecureWipeString(m_refreshToken);
    m_refreshToken = std::move(nextRefresh);
}

bool SteamCmClient::EnsureConnected() {
    if (IsConnected()) return true;
    if (m_reconnecting) return false;
    if (m_refreshToken.empty() && m_accessToken.empty()) return false;

    m_reconnecting = true;
    struct ReconnectGuard {
        bool& flag;
        ~ReconnectGuard() noexcept { flag = false; }
    } guard{m_reconnecting};

    LOG_INFO("SteamCM", MsgKey::LogCmAutoReconnecting);
    if (!ConnectAndLogon(m_steamId, m_refreshToken, m_accessToken)) {
        return false;
    }
    return true;
}

std::optional<std::string> SteamCmClient::RefreshAccessTokenViaCm(std::string* outNewRefreshToken) {
    if (outNewRefreshToken) {
        SecureWipeString(*outNewRefreshToken);
    }
    if (m_refreshToken.empty()) {
        return std::nullopt;
    }
    if (!EnsureConnected()) {
        return std::nullopt;
    }

    uint64_t targetSteamId = m_steamId != 0 ? m_steamId : GetJwtSteamId(m_refreshToken);
    if (targetSteamId == 0) {
        LOG_WARN("SteamCM", MsgKey::LogCmRefreshNoSteamId);
        return std::nullopt;
    }

    // CAuthentication_AccessToken_GenerateForApp_Request:
    //   optional string refresh_token = 1;
    //   optional fixed64 steamid = 2;
    //   optional ETokenRenewalType renewal_type = 3; (1 = k_ETokenRenewalType_Allow)
    ProtoWriter req;
    req.WriteString(1, m_refreshToken);
    req.WriteFixed64(2, targetSteamId);
    req.WriteInt32(3, 1); // k_ETokenRenewalType_Allow

    const uint64_t jobId = ++m_nextJobId;
    LOG_DEBUG("SteamCM", MsgKey::LogCmRequestingTokenRefresh, MaskSteamId(targetSteamId));
    if (!SendProtoMsg(ESteamMsg::ServiceMethodCallFromClient, req, jobId, "Authentication.GenerateAccessTokenForApp#1")) {
        return std::nullopt;
    }

    std::vector<uint8_t> respBody;
    ScopedBufferWiper respWiper{respBody};
    int32_t rpcResult = 2;
    if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ServiceMethodResponse), respBody, 1500, &rpcResult, jobId) &&
        !ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ServiceMethodSendToClient), respBody, 500, &rpcResult, jobId)) {
        LOG_WARN("SteamCM", MsgKey::LogCmRefreshRpcTimeout);
        return std::nullopt;
    }

    if (rpcResult != 1) {
        LOG_WARN("SteamCM", MsgKey::LogCmRefreshRpcFailed, rpcResult);
        return std::nullopt;
    }

    // CAuthentication_AccessToken_GenerateForApp_Response:
    //   optional string access_token = 1;
    //   optional string refresh_token = 2;
    ProtoReader reader(respBody);
    ProtoField field;
    std::string newAccessToken;
    std::string newRefreshToken;
    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1 && !field.bytesVal.empty()) {
            newAccessToken.assign(reinterpret_cast<const char*>(field.bytesVal.data()), field.bytesVal.size());
        } else if (field.fieldNumber == 2 && !field.bytesVal.empty()) {
            newRefreshToken.assign(reinterpret_cast<const char*>(field.bytesVal.data()), field.bytesVal.size());
        }
    }
    SecureWipeBuffer(respBody);

    if (!newRefreshToken.empty()) {
        SecureWipeString(m_refreshToken);
        m_refreshToken = newRefreshToken;
        if (outNewRefreshToken) {
            *outNewRefreshToken = std::move(newRefreshToken);
        }
        SecureWipeString(newRefreshToken);
        LOG_INFO("SteamCM", MsgKey::LogCmRefreshTokenRotated, MaskSteamId(targetSteamId));
    }

    if (!newAccessToken.empty()) {
        SecureWipeString(m_accessToken);
        m_accessToken = newAccessToken;
        LOG_DEBUG("SteamCM", MsgKey::LogCmAccessTokenRefreshed, MaskToken(newAccessToken));
        return newAccessToken;
    }

    return std::nullopt;
}

std::optional<std::vector<uint8_t>> SteamCmClient::RequestAppOwnershipTicket(uint32_t appId) {
    if (!EnsureConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", MsgKey::LogCmReqOwnershipTicket, appId);

    ProtoWriter body;
    body.WriteUInt32(1, appId); // app_id = 1

    std::vector<uint8_t> respBody;
    ScopedBufferWiper respWiper{respBody};
    // Expected response eMsg is 858 (CMsgClientGetAppOwnershipTicketResponse)
    if (!SendAndReceiveWithRetry(
            ESteamMsg::ClientGetAppOwnershipTicket,
            body,
            static_cast<uint32_t>(ESteamMsg::ClientGetAppOwnershipTicketResponse),
            respBody,
            8000)) {
        return std::nullopt;
    }

    ProtoReader reader(respBody);
    ProtoField field;
    int32_t eresult = 2;
    std::vector<uint8_t> ticket;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 1) { // eresult
            eresult = static_cast<int32_t>(field.varintVal);
        } else if (field.fieldNumber == 3) { // ticket bytes
            ticket.assign(field.bytesVal.begin(), field.bytesVal.end());
        }
    }

    if (eresult == 1 && !ticket.empty()) {
        LOG_INFO("SteamCM", MsgKey::LogCmGotOwnershipTicket,
                 appId, ticket.size(), MaskTicketHex(ticket));
        return ticket;
    } else {
        LOG_WARN("SteamCM", MsgKey::LogCmOwnershipTicketFailed, appId, eresult);
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> SteamCmClient::RequestEncryptedAppTicket(uint32_t appId) {
    if (!EnsureConnected()) return std::nullopt;

    LOG_DEBUG("SteamCM", MsgKey::LogCmReqEncryptedTicket, appId);

    ProtoWriter body;
    body.WriteUInt32(1, appId); // app_id = 1

    std::vector<uint8_t> respBody;
    ScopedBufferWiper respWiper{respBody};
    if (!SendAndReceiveWithRetry(
            ESteamMsg::ClientRequestEncryptedAppTicket,
            body,
            static_cast<uint32_t>(ESteamMsg::ClientRequestEncryptedAppTicketResponse),
            respBody,
            2500)) {
        return std::nullopt;
    }

    ProtoReader reader(respBody);
    ProtoField field;
    int32_t eresult = 2;
    std::vector<uint8_t> ticket;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 2) { // eresult
            eresult = static_cast<int32_t>(field.varintVal);
        } else if (field.fieldNumber == 3) { // encrypted_app_ticket submessage
            // The Steamworks SDK API (ISteamUser::GetEncryptedAppTicket / SteamEncryptedAppTicket_BDecryptTicket)
            // expects the entire serialized EncryptedAppTicket container (ticket_version_no, crc,
            // cb_encrypteduserdata, cb_encrypted_appownershipticket, and encrypted_ticket), matching
            // the exact binary format delivered by genuine Steam Client IPC.
            ProtoReader subReader(field.bytesVal);
            ProtoField subField;
            bool hasEncryptedTicket = false;
            while (subReader.ReadNext(subField)) {
                if (subField.fieldNumber == 5 && !subField.bytesVal.empty()) {
                    hasEncryptedTicket = true;
                    break;
                }
            }
            if (hasEncryptedTicket) {
                ticket.assign(field.bytesVal.begin(), field.bytesVal.end());
            }
        }
    }

    if (eresult == 1 && !ticket.empty()) {
        LOG_INFO("SteamCM", MsgKey::LogCmGotEncryptedTicket,
                 appId, ticket.size(), MaskTicketHex(ticket));
        return ticket;
    } else {
        LOG_DEBUG("SteamCM", MsgKey::LogCmEncryptedTicketDenied, eresult);
        return std::nullopt;
    }
}

std::vector<DepotKeyInfo> SteamCmClient::RequestDepotKeys(
    const std::vector<DepotKeyQuery>& queries, uint32_t fallbackAppId) {
    std::vector<DepotKeyInfo> keys;
    if (!EnsureConnected()) return keys;

    for (const auto& query : queries) {
        const uint32_t depotId = query.depotId;
        const uint32_t primaryAppId = (query.appId > 0) ? query.appId : fallbackAppId;

        auto tryQueryDepot = [&](uint32_t targetAppId, int32_t& outEresult, std::vector<uint8_t>& outKeyBytes) -> bool {
            LOG_DEBUG("SteamCM", MsgKey::LogCmReqDepotKey, depotId, targetAppId);

            ProtoWriter body;
            body.WriteUInt32(1, depotId);     // depot_id
            body.WriteUInt32(2, targetAppId); // app_id

            std::vector<uint8_t> respBody;
            ScopedBufferWiper respWiper{respBody};
            if (!SendAndReceiveWithRetry(
                    ESteamMsg::ClientGetDepotDecryptionKey,
                    body,
                    static_cast<uint32_t>(ESteamMsg::ClientGetDepotDecryptionKeyResponse),
                    respBody,
                    6000)) {
                outEresult = -1;
                return false;
            }

            ProtoReader reader(respBody);
            ProtoField field;
            outEresult = 2;
            outKeyBytes.clear();

            while (reader.ReadNext(field)) {
                if (field.fieldNumber == 1) { // eresult
                    outEresult = static_cast<int32_t>(field.varintVal);
                } else if (field.fieldNumber == 3) { // depot_encryption_key
                    outKeyBytes.assign(field.bytesVal.begin(), field.bytesVal.end());
                }
            }
            return true;
        };

        int32_t eresult = 2;
        std::vector<uint8_t> keyBytes;
        ScopedBufferWiper keyWiper{keyBytes};

        bool ok = tryQueryDepot(primaryAppId, eresult, keyBytes);

        // Anti-fragile two-tier fallback: if eresult == 9 (k_EResultFileNotFound), retry with fallbackAppId
        if (eresult == 9 && fallbackAppId > 0 && primaryAppId != fallbackAppId) {
            LOG_DEBUG("SteamCM", "Depot {} returned eresult=9 for AppID {}, retrying with fallback AppID {}",
                      depotId, primaryAppId, fallbackAppId);
            ok = tryQueryDepot(fallbackAppId, eresult, keyBytes);
        }

        if (ok && eresult == 1) {
            DepotKeyInfo info;
            info.depotId = depotId;
            info.dlcId = (primaryAppId != fallbackAppId) ? primaryAppId : 0;
            if (!keyBytes.empty()) {
                info.hexKey = ToHexString(keyBytes);
                LOG_INFO("SteamCM", MsgKey::LogCmGotDepotKey, depotId, MaskKeyHex(info.hexKey));
            } else {
                LOG_INFO("SteamCM", MsgKey::LogCmPublicUnencryptedDepot, depotId);
            }
            keys.push_back(std::move(info));
        } else {
            LOG_DEBUG("SteamCM", MsgKey::LogCmDepotKeyDenied, depotId, eresult);
        }

        // Polite rate pacing
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return keys;
}

std::vector<DepotKeyInfo> SteamCmClient::RequestDepotKeys(uint32_t appId, const std::vector<uint32_t>& depotIds) {
    std::vector<DepotKeyQuery> queries;
    queries.reserve(depotIds.size());
    for (uint32_t depotId : depotIds) {
        queries.push_back({depotId, appId});
    }
    return RequestDepotKeys(queries, appId);
}

std::unordered_map<uint32_t, uint64_t> SteamCmClient::RequestAppTokens(const std::vector<uint32_t>& appIds) {
    std::unordered_map<uint32_t, uint64_t> tokens;
    if (appIds.empty() || !EnsureConnected()) return tokens;

    ProtoWriter body;
    for (uint32_t appId : appIds) {
        body.WriteUInt32(2, appId); // appids = 2 (repeated)
    }

    std::vector<uint8_t> respBody;
    ScopedBufferWiper respWiper{respBody};
    if (!SendAndReceiveWithRetry(
            ESteamMsg::ClientPICSAccessTokenRequest,
            body,
            static_cast<uint32_t>(ESteamMsg::ClientPICSAccessTokenResponse),
            respBody,
            8000)) {
        return tokens;
    }

    ProtoReader reader(respBody);
    ProtoField field;

    while (reader.ReadNext(field)) {
        if (field.fieldNumber == 3) { // app_access_tokens (repeated)
            ProtoReader subReader(field.bytesVal);
            ProtoField subField;
            uint32_t aId = 0;
            uint64_t aToken = 0;
            while (subReader.ReadNext(subField)) {
                if (subField.fieldNumber == 1) aId = static_cast<uint32_t>(subField.varintVal);
                if (subField.fieldNumber == 2) aToken = subField.fixed64Val ? subField.fixed64Val : subField.varintVal;
            }
            if (aId > 0 && aToken > 0) {
                tokens[aId] = aToken;
                LOG_DEBUG("SteamCM", MsgKey::LogCmGotPicsToken, aId, MaskPicsToken(aToken));
            }
        } else if (field.fieldNumber == 4) { // app_denied_tokens (repeated)
            uint32_t deniedAppId = static_cast<uint32_t>(field.varintVal);
            LOG_DEBUG("SteamCM", MsgKey::LogCmPublicAppZeroToken, deniedAppId);
        }
    }

    return tokens;
}

std::vector<ParsedAppInfoData> SteamCmClient::RequestPicsProductInfoBatch(
    const std::vector<uint32_t>& appIds,
    const std::unordered_map<uint32_t, uint64_t>& appTokens) {

    std::vector<ParsedAppInfoData> results;
    if (appIds.empty() || !EnsureConnected()) return results;

    constexpr size_t kBatchSize = 25;
    for (size_t i = 0; i < appIds.size(); i += kBatchSize) {
        const size_t chunkEnd = std::min(i + kBatchSize, appIds.size());

        ProtoWriter body;
        for (size_t j = i; j < chunkEnd; ++j) {
            const uint32_t curAppId = appIds[j];
            uint64_t curToken = 0;
            if (auto it = appTokens.find(curAppId); it != appTokens.end()) {
                curToken = it->second;
            }

            ProtoWriter appInfoWriter;
            appInfoWriter.WriteUInt32(1, curAppId); // appid = 1
            if (curToken != 0) {
                appInfoWriter.WriteUInt64(2, curToken); // access_token = 2
            }
            appInfoWriter.WriteBool(3, false); // only_public = 3
            body.WriteBytes(2, appInfoWriter.Data()); // apps = 2
        }
        body.WriteBool(3, false); // meta_data_only = 3

        std::vector<uint8_t> respBody;
        ScopedBufferWiper respWiper{respBody};
        uint64_t jobId = 0;
        if (!SendAndReceiveWithRetry(
                ESteamMsg::ClientPICSProductInfoRequest,
                body,
                static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse),
                respBody,
                8000,
                nullptr,
                {},
                &jobId)) {
            continue;
        }

        for (int chunkIdx = 0; chunkIdx < 16; ++chunkIdx) {
            if (chunkIdx > 0) {
                if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse), respBody, 2000, nullptr, jobId)) {
                    break;
                }
            }

            bool responsePending = false;
            ProtoReader reader(respBody);
            ProtoField field;
            while (reader.ReadNext(field)) {
                if (field.fieldNumber == 1 && !field.bytesVal.empty()) { // apps (repeated AppInfo)
                    ProtoReader appReader(field.bytesVal);
                    ProtoField appField;
                    uint32_t rAppId = 0;
                    std::span<const uint8_t> buffer;
                    while (appReader.ReadNext(appField)) {
                        if (appField.fieldNumber == 1) { // appid
                            rAppId = static_cast<uint32_t>(appField.varintVal);
                        } else if (appField.fieldNumber == 5) { // buffer
                            buffer = appField.bytesVal;
                        }
                    }
                    if (rAppId != 0 && !buffer.empty()) {
                        LOG_DEBUG("SteamCM", MsgKey::LogCmRecvPicsAppInfo, buffer.size());
                        auto parsed = ParseBinaryVdfAppInfo(buffer, rAppId);
                        if (parsed) {
                            results.push_back(std::move(*parsed));
                        }
                    }
                } else if (field.fieldNumber == 6) { // response_pending
                    responsePending = (field.varintVal != 0);
                }
            }

            if (!responsePending) {
                break;
            }
        }
    }

    return results;
}

std::optional<ParsedAppInfoData> SteamCmClient::RequestPicsProductInfo(uint32_t appId, uint64_t accessToken) {
    std::unordered_map<uint32_t, uint64_t> tokens;
    if (accessToken != 0) {
        tokens[appId] = accessToken;
    }
    LOG_DEBUG("SteamCM", MsgKey::LogCmReqPicsProductInfo, appId, MaskPicsToken(accessToken));
    auto batch = RequestPicsProductInfoBatch({appId}, tokens);
    if (!batch.empty()) {
        return std::move(batch.front());
    }
    return std::nullopt;
}

std::unordered_map<uint32_t, std::string> SteamCmClient::RequestPicsAppNames(
    const std::vector<uint32_t>& appIds, bool* outSuccess) {

    if (outSuccess) *outSuccess = false;
    std::unordered_map<uint32_t, std::string> outNames;
    if (appIds.empty() || !EnsureConnected()) return outNames;

    bool anyBatchSucceeded = false;
    constexpr size_t kBatchSize = 50;
    for (size_t i = 0; i < appIds.size(); i += kBatchSize) {
        const size_t chunkEnd = std::min(i + kBatchSize, appIds.size());

        ProtoWriter body;
        for (size_t j = i; j < chunkEnd; ++j) {
            ProtoWriter appReq;
            appReq.WriteUInt32(1, appIds[j]); // appid = 1
            appReq.WriteBool(3, true);        // only_public = 3 (public common section contains name)
            body.WriteSubMessage(2, appReq);  // apps = 2 (repeated)
        }
        body.WriteBool(3, false); // meta_data_only = 3

        std::vector<uint8_t> respBody;
        ScopedBufferWiper respWiper{respBody};
        uint64_t jobId = 0;
        if (!SendAndReceiveWithRetry(
                ESteamMsg::ClientPICSProductInfoRequest,
                body,
                static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse),
                respBody,
                8000,
                nullptr,
                {},
                &jobId)) {
            continue;
        }
        anyBatchSucceeded = true;

        for (int chunkIdx = 0; chunkIdx < 16; ++chunkIdx) {
            if (chunkIdx > 0) {
                if (!ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ClientPICSProductInfoResponse),
                                     respBody, 2000, nullptr, jobId)) {
                    break;
                }
            }

            bool responsePending = false;
            ProtoReader reader(respBody);
            ProtoField field;
            while (reader.ReadNext(field)) {
                if (field.fieldNumber == 1 && !field.bytesVal.empty()) { // apps (repeated AppInfo)
                    ProtoReader appReader(field.bytesVal);
                    ProtoField appField;
                    uint32_t rAppId = 0;
                    std::span<const uint8_t> buffer;
                    while (appReader.ReadNext(appField)) {
                        if (appField.fieldNumber == 1) { // appid
                            rAppId = static_cast<uint32_t>(appField.varintVal);
                        } else if (appField.fieldNumber == 5) { // buffer
                            buffer = appField.bytesVal;
                        }
                    }
                    if (rAppId > 0 && !buffer.empty()) {
                        auto appInfo = ParseBinaryVdfAppInfo(buffer, rAppId);
                        if (appInfo && !appInfo->name.empty()) {
                            outNames[rAppId] = std::move(appInfo->name);
                        }
                    }
                } else if (field.fieldNumber == 6) { // response_pending
                    responsePending = (field.varintVal != 0);
                }
            }

            if (!responsePending) {
                break;
            }
        }
    }
    if (outSuccess) *outSuccess = anyBatchSucceeded;

    return outNames;
}

std::string SteamCmClient::FetchManifestRequestCode(
    uint32_t appId, uint32_t depotId, const std::string& manifestId, bool* outAccessDenied) {
    if (outAccessDenied) *outAccessDenied = false;
    if (manifestId.empty() || !IsValidManifestId(manifestId)) return "";

    uint64_t manifestGid = 0;
    const auto [ptr, ec] = std::from_chars(
        manifestId.data(), manifestId.data() + manifestId.size(), manifestGid);
    if (ec != std::errc{} || ptr != manifestId.data() + manifestId.size()) {
        return "";
    }

    // 0. Ensure active CM connection
    (void)EnsureConnected();

    std::string reqCode;

    ProtoWriter innerReq;
    innerReq.WriteUInt32(1, appId); // app_id
    innerReq.WriteUInt32(2, depotId); // depot_id
    innerReq.WriteUInt64(3, manifestGid); // manifest_id
    innerReq.WriteString(4, "public"); // app_branch
    // Note: Do not serialize field 5 (branch_password_hash) when branch is "public",
    // matching official steamclient64.dll assembly logic (0x13869e6e4 - 0x13869e81c).

    int32_t rpcResult = 0;
    bool gotReply = false;

    // Track 1: Unified RPC eMsg 151 (ServiceMethodCallFromClient)
    for (int attempt = 0; attempt < 2 && !gotReply && m_ws.IsConnected() && m_isLoggedOn; ++attempt) {
        const uint64_t jobId = ++m_nextJobId;
        LOG_DEBUG("SteamCM", MsgKey::LogCmReqManifestCodeRpc, depotId, manifestId);
        if (SendProtoMsg(ESteamMsg::ServiceMethodCallFromClient, innerReq, jobId, "ContentServerDirectory.GetManifestRequestCode#1")) {
            std::vector<uint8_t> respBody;
            ScopedBufferWiper respWiper{respBody};
            if (ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ServiceMethodResponse), respBody, 4000, &rpcResult, jobId) ||
                ReadMatchingMsg(static_cast<uint32_t>(ESteamMsg::ServiceMethodSendToClient), respBody, 2000, &rpcResult, jobId)) {
                gotReply = true;
                if (rpcResult == 1) { // k_EResultOK
                    ProtoReader respReader(respBody);
                    ProtoField field;
                    uint64_t codeVal = 0;
                    while (respReader.ReadNext(field)) {
                        if (field.fieldNumber == 1) { // manifest_request_code
                            codeVal = field.varintVal ? field.varintVal : field.fixed64Val;
                        }
                    }
                    if (codeVal != 0) {
                        reqCode = std::to_string(codeVal);
                        LOG_DEBUG("SteamCM", MsgKey::LogCmGotManifestCodeRpc, reqCode);
                        return reqCode;
                    }
                } else {
                    LOG_DEBUG("SteamCM", MsgKey::LogCmManifestCodeRpcDenied, rpcResult, depotId);
                    if (outAccessDenied) *outAccessDenied = true;
                    return ""; // Short-circuit: definitively not owned, never do redundant Track 2 fallback
                }
            } else if (!m_ws.IsConnected() && attempt == 0 && !m_reconnecting) {
                m_isLoggedOn = false;
                (void)EnsureConnected();
                continue;
            }
        }
        break;
    }

    // Track 2: Legacy wrapper eMsg 5594 (ClientServiceMethod) fallback (only if Track 1 received no response from CM)
    if (!gotReply && m_ws.IsConnected() && m_isLoggedOn) {
        ProtoWriter svcMsg;
        svcMsg.WriteString(1, "ContentServerDirectory.GetManifestRequestCode#1");
        svcMsg.WriteBytes(2, innerReq.Data());

        std::vector<uint8_t> respBody;
        ScopedBufferWiper respWiper{respBody};
        int32_t legacyResult = 0;
        LOG_DEBUG("SteamCM", MsgKey::LogCmReqManifestCodeLegacy, depotId, manifestId);
        if (SendAndReceiveWithRetry(
                ESteamMsg::ClientServiceMethod,
                svcMsg,
                static_cast<uint32_t>(ESteamMsg::ClientServiceMethodResponse),
                respBody,
                4000,
                &legacyResult)) {
            if (legacyResult != 1) {
                if (outAccessDenied) *outAccessDenied = true;
                return "";
            }
            ProtoReader respReader(respBody);
            ProtoField field;
            std::span<const uint8_t> innerBytes;
            while (respReader.ReadNext(field)) {
                if (field.fieldNumber == 2) { // serialized_method_response
                    innerBytes = field.bytesVal;
                }
            }
            if (!innerBytes.empty()) {
                ProtoReader innerReader(innerBytes);
                ProtoField innerField;
                uint64_t reqCodeNum = 0;
                while (innerReader.ReadNext(innerField)) {
                    if (innerField.fieldNumber == 1) { // manifest_request_code
                        reqCodeNum = innerField.varintVal ? innerField.varintVal : innerField.fixed64Val;
                    }
                }
                if (reqCodeNum != 0) {
                    reqCode = std::to_string(reqCodeNum);
                    LOG_DEBUG("SteamCM", MsgKey::LogCmGotManifestCodeLegacy, reqCode);
                    return reqCode;
                }
            }
        }
    }

    return "";
}

void SteamCmClient::PromoteWorkingCdnServer(std::string_view server) {
    if (server.empty()) return;
    std::lock_guard lock(s_cdnMutex);
    auto it = std::find(s_cachedCdnServers.begin(), s_cachedCdnServers.end(), server);
    if (it != s_cachedCdnServers.end() && it != s_cachedCdnServers.begin()) {
        std::rotate(s_cachedCdnServers.begin(), it, it + 1);
        LOG_INFO("SteamCM", MsgKey::LogCmPromotedCdnServer, server);
    }
}

std::vector<std::string> SteamCmClient::GetCdnServers(uint32_t cellId) {
    std::lock_guard lock(s_cdnMutex);

    if (!s_cachedCdnServers.empty()) {
        return s_cachedCdnServers;
    }

    WinHttpTransport http;
    std::string directoryUrl = std::format(
        "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id={}",
        cellId);
    HttpResponse resp = http.Get(directoryUrl);
    if (resp.IsSuccess() && !resp.body.empty()) {
        size_t pos = 0;
        while ((pos = resp.body.find("\"host\"", pos)) != std::string::npos) {
            size_t colon = resp.body.find(':', pos + 6);
            if (colon == std::string::npos) break;
            size_t q1 = resp.body.find('"', colon);
            if (q1 == std::string::npos) break;
            size_t q2 = resp.body.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string host = resp.body.substr(q1 + 1, q2 - q1 - 1);
            if (host.ends_with(".steamcontent.com") || host.ends_with(".steampowered.com")) {
                if (std::find(s_cachedCdnServers.begin(), s_cachedCdnServers.end(), host) == s_cachedCdnServers.end()) {
                    s_cachedCdnServers.push_back(std::move(host));
                    if (s_cachedCdnServers.size() >= 10) break;
                }
            }
            pos = q2 + 1;
        }
    }

    if (s_cachedCdnServers.empty()) {
        s_cachedCdnServers = {
            "valve.steamcontent.com",
            "cdn.steamcontent.com",
            "content1.steampowered.com",
            "content2.steampowered.com",
            "content.steampowered.com",
            "cache1-lax2.steamcontent.com"
        };
    }

    return s_cachedCdnServers;
}

std::optional<std::string> SteamCmClient::DownloadManifestPayload(
    uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& reqCode,
    const std::string& destDir, const std::vector<std::string>& cdnServers, WinHttpTransport* httpPtr) {

    if (appId == 0 || depotId == 0 || !IsValidManifestId(manifestId) || reqCode.empty() || reqCode == "0") {
        return std::nullopt;
    }

    std::string fileName = std::format("{}_{}.manifest", depotId, manifestId);
    std::filesystem::path localPath = Utf8Path(destDir) / fileName;
    const std::string localPathUtf8 = PathToUtf8(localPath);

    std::error_code checkEc;
    if (std::filesystem::is_regular_file(localPath, checkEc) && std::filesystem::file_size(localPath, checkEc) > 0) {
        LOG_INFO("SteamCM", MsgKey::LogCmManifestAlreadyExists, localPathUtf8);
        return localPathUtf8;
    }

    WinHttpTransport localHttp;
    if (!httpPtr) {
        localHttp.SetTimeouts(3000, 2500, 5000, 15000);
    }
    WinHttpTransport& http = httpPtr ? *httpPtr : localHttp;
    HttpResponse cdnResp;

    for (const auto& server : cdnServers) {
        std::string manifestUrl = std::format(
            "https://{}/depot/{}/manifest/{}/5/{}",
            server, depotId, manifestId, reqCode);

        LOG_DEBUG("SteamCM", MsgKey::LogCmDownloadingManifestCdn, manifestUrl);
        cdnResp = http.Get(manifestUrl);
        if (cdnResp.IsSuccess() && !cdnResp.body.empty()) {
            LOG_INFO("SteamCM", MsgKey::LogCmDownloadedManifestCdn, server, cdnResp.body.size());
            PromoteWorkingCdnServer(server);
            break;
        }
    }

    if (!cdnResp.IsSuccess() || cdnResp.body.empty()) {
        LOG_WARN("SteamCM", MsgKey::LogCmDownloadManifestFailed,
                 depotId, manifestId, cdnResp.statusCode, cdnResp.errorMessage);
        return std::nullopt;
    }

    std::error_code dirEc;
    std::filesystem::create_directories(Utf8Path(destDir), dirEc);
    if (dirEc) {
        LOG_WARN("SteamCM", MsgKey::LogCmCreateDirFailed, destDir, dirEc.message());
    }

    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(cdnResp.body.data()),
        cdnResp.body.size());

    std::vector<uint8_t> uncompressed;
    if (payload.size() >= 2 && payload[0] == 0x1F && payload[1] == 0x8B) {
        constexpr size_t kMaxManifestUncompressedSize = 256 * 1024 * 1024;
        uint32_t isize = 0;
        if (payload.size() >= 18) {
            std::memcpy(&isize, payload.data() + payload.size() - 4, sizeof(uint32_t));
        }
        size_t bufSize = (isize > 0 && isize < kMaxManifestUncompressedSize)
            ? (static_cast<size_t>(isize) + 4096)
            : (std::max<size_t>(payload.size() * 10, 1024 * 1024));
        bufSize = std::clamp<size_t>(bufSize, 1024 * 1024, kMaxManifestUncompressedSize);
        uncompressed.resize(bufSize);

        int res = TINF_DATA_ERROR;
        while (bufSize <= kMaxManifestUncompressedSize) {
            unsigned int destLen = static_cast<unsigned int>(bufSize);
            res = tinf_gzip_uncompress(uncompressed.data(), &destLen, payload.data(), static_cast<unsigned int>(payload.size()));
            if (res == TINF_OK) {
                uncompressed.resize(destLen);
                payload = uncompressed;
                LOG_DEBUG("SteamCM", MsgKey::LogCmManifestGzipOk, cdnResp.body.size(), destLen);
                break;
            }
            if (res == TINF_BUF_ERROR && bufSize < kMaxManifestUncompressedSize) {
                bufSize = std::min<size_t>(bufSize * 2, kMaxManifestUncompressedSize);
                uncompressed.resize(bufSize);
                continue;
            }
            break; // Fatal format error or size cap exceeded
        }

        if (res != TINF_OK) {
            LOG_WARN("SteamCM", MsgKey::LogCmManifestGzipFailed, res);
            return std::nullopt;
        }
    }

    if (!WriteBinaryFile(localPath, payload)) {
        LOG_WARN("SteamCM", MsgKey::LogCmSaveManifestFailed, localPathUtf8);
        return std::nullopt;
    }

    LOG_INFO("SteamCM", MsgKey::LogCmManifestSaved, fileName, payload.size());
    return localPathUtf8;
}

std::optional<std::string> SteamCmClient::DownloadManifestOnline(
    uint32_t appId, uint32_t depotId, const std::string& manifestId, const std::string& destDir,
    bool* outAccessDenied, WinHttpTransport* sharedHttp) {

    if (outAccessDenied) *outAccessDenied = false;
    if (appId == 0 || depotId == 0 || !IsValidManifestId(manifestId)) {
        return std::nullopt;
    }

    std::string fileName = std::format("{}_{}.manifest", depotId, manifestId);
    std::filesystem::path localPath = Utf8Path(destDir) / fileName;
    const std::string localPathUtf8 = PathToUtf8(localPath);

    std::error_code checkEc;
    if (std::filesystem::is_regular_file(localPath, checkEc) && std::filesystem::file_size(localPath, checkEc) > 0) {
        LOG_INFO("SteamCM", MsgKey::LogCmManifestAlreadyExists, localPathUtf8);
        return localPathUtf8;
    }

    std::string reqCode = FetchManifestRequestCode(appId, depotId, manifestId, outAccessDenied);
    if (reqCode.empty() || reqCode == "0") {
        if (!outAccessDenied || !*outAccessDenied) {
            LOG_WARN("SteamCM", MsgKey::LogCmNoManifestReqCode, depotId, manifestId);
        }
        return std::nullopt;
    }

    auto cdnServers = GetCdnServers(m_cellId);
    return DownloadManifestPayload(appId, depotId, manifestId, reqCode, destDir, cdnServers, sharedHttp);
}

ExtractedAppCredentials SteamCmClient::ExtractFullCredentials(uint32_t appId, bool forceEticket, ProgressCallback onProgress) {
    (void)forceEticket;
    if (onProgress) {
        onProgress("tickets", 0, 0, 0);
    }
    ExtractedAppCredentials creds;
    creds.appId = appId;

    // 1. AppOwnershipTicket (纯净拉取，自主拥有与家庭共享在无会话状态下原生直接下发)
    LOG_DEBUG("SteamCM", MsgKey::LogCmFullReqOwnership, appId);
    if (!TuiEngine::IsActive()) {
        std::cout << TR(MsgKey::CliExtractRequestTicket) << "\n";
    }
    creds.appOwnershipTicket = RequestAppOwnershipTicket(appId);
    if (creds.appOwnershipTicket) {
        LOG_INFO("SteamCM", MsgKey::LogCmFullGotOwnership, creds.appOwnershipTicket->size());
        if (!TuiEngine::IsActive()) {
            std::cout << TR_FMT(MsgKey::CliExtractTicketGot, creds.appOwnershipTicket->size()) << "\n";
        }
    } else {
        LOG_INFO("SteamCM", MsgKey::LogCmFullNoOwnershipFallback);
    }

    // 2. 纯官方云端向 Steam CM PICS 请求完整产品元数据 (包含全部 Depots、清单号、所有 DLC 列表与 DRM 标记)
    if (onProgress) {
        onProgress("meta", 0, 0, 0);
    }
    LOG_DEBUG("SteamCM", MsgKey::LogCmFullQueryPicsToken, appId);
    uint64_t appToken = 0;
    auto tokMap = RequestAppTokens({ appId });
    if (tokMap.contains(appId)) {
        appToken = tokMap[appId];
        if (appToken != 0) {
            creds.appTokens[appId] = appToken;
        }
    }

    LOG_DEBUG("SteamCM", MsgKey::LogCmFullReqPicsMeta, appId, MaskPicsToken(appToken));
    auto appInfoData = RequestPicsProductInfo(appId, appToken);

    // 3. EncryptedAppTicket (纯 CM 原生会话裁决: 无需外部网页爬虫或静态名单，直接向 Valve CM 原生索取)
    {
        creds.encryptedAppTicket = RequestEncryptedAppTicket(appId);
        if (creds.encryptedAppTicket) {
            LOG_INFO("SteamCM", MsgKey::LogCmFullGotEticket, creds.encryptedAppTicket->size());
            if (!TuiEngine::IsActive()) {
                std::cout << TR_FMT(MsgKey::CliExtractEticketGot, creds.encryptedAppTicket->size()) << "\n";
            }
            if (appInfoData) {
                appInfoData->requiresDenuvo = true;
            }
        } else {
            // 普通单机游戏或未配置加密票据密钥的游戏，CM 原生返回 Fail(2)，优雅静默跳过
            LOG_DEBUG("SteamCM", MsgKey::LogCmFullSkipEticketSession);
        }

        // 极端边界兜底：若前置未拿到所有权票据，在会话中顺带重试一次
        if (!creds.appOwnershipTicket) {
            creds.appOwnershipTicket = RequestAppOwnershipTicket(appId);
            if (creds.appOwnershipTicket) {
                LOG_INFO("SteamCM", MsgKey::LogCmFullGotOwnershipAfterSession, creds.appOwnershipTicket->size());
                if (!TuiEngine::IsActive()) {
                    std::cout << TR_FMT(MsgKey::CliExtractTicketGot, creds.appOwnershipTicket->size()) << "\n";
                }
            }
        }
    }

    std::vector<DepotKeyInfo> depotKeys;
    std::unordered_set<uint32_t> knownDlcSet;
    const std::string outDir = std::to_string(appId);
    size_t downloadedManifests = 0;

    struct ManifestTask {
        uint32_t depotId{0};
        std::string manifestId;
        std::string reqCode;
    };
    std::vector<ManifestTask> downloadTasks;
    std::future<std::vector<std::pair<uint32_t, std::string>>> manifestFuture;

    if (appInfoData) {
        LOG_INFO("SteamCM", MsgKey::LogCmFullGotAppInfoSummary,
                 appInfoData->depots.size(), appInfoData->dlcAppIds.size());

        // 4.1 识别所有 DLC（包含无独立 Depot 的逻辑 DLC，例如季票、豪华版升级包、皮肤包、原声音乐等）
        for (uint32_t dlcId : appInfoData->dlcAppIds) {
            if (dlcId != appId && !knownDlcSet.contains(dlcId)) {
                knownDlcSet.insert(dlcId);
                DlcInfo dInfo;
                dInfo.dlcId = dlcId;
                creds.dlcs.push_back(std::move(dInfo));
            }
        }

        // 4.2 提取官方 Depot 与关联清单 ID (GID)
        for (const auto& dInfo : appInfoData->depots) {
            if (dInfo.dlcId > 0 && dInfo.dlcId != appId && !knownDlcSet.contains(dInfo.dlcId)) {
                knownDlcSet.insert(dInfo.dlcId);
                DlcInfo dInfoObj;
                dInfoObj.dlcId = dInfo.dlcId;
                creds.dlcs.push_back(std::move(dInfoObj));
            }

            auto it = std::find_if(depotKeys.begin(), depotKeys.end(),
                                   [&](const DepotKeyInfo& k) { return k.depotId == dInfo.depotId; });
            if (it != depotKeys.end()) {
                if (it->manifestId.empty() && !dInfo.manifestId.empty()) {
                    it->manifestId = dInfo.manifestId;
                }
                if (it->dlcId == 0 && dInfo.dlcId > 0) {
                    it->dlcId = dInfo.dlcId;
                }
            } else {
                DepotKeyInfo newDk;
                newDk.depotId = dInfo.depotId;
                newDk.manifestId = dInfo.manifestId;
                newDk.dlcId = dInfo.dlcId;
                depotKeys.push_back(std::move(newDk));
            }
        }

        // 4.3 批量拉取全部 DLC 与原声音乐的 PICS 产品元数据（彻底解析 DLC 内部专属 Depot 与清单）
        std::vector<uint32_t> dlcsToQuery;
        for (uint32_t dlcId : appInfoData->dlcAppIds) {
            if (dlcId != appId) {
                dlcsToQuery.push_back(dlcId);
            }
        }

        if (!dlcsToQuery.empty()) {
            LOG_DEBUG("SteamCM", MsgKey::LogCmFullQueryPicsToken, dlcsToQuery.size());
            auto dlcTokens = RequestAppTokens(dlcsToQuery);
            for (const auto& [tId, tVal] : dlcTokens) {
                if (tVal != 0) {
                    creds.appTokens[tId] = tVal;
                }
            }

            auto dlcAppInfos = RequestPicsProductInfoBatch(dlcsToQuery, dlcTokens);
            for (auto& dlcParsed : dlcAppInfos) {
                // 回填官方名称
                const std::string& dName = !dlcParsed.localizedName.empty() ? dlcParsed.localizedName : dlcParsed.name;
                if (!dName.empty()) {
                    auto it = std::find_if(creds.dlcs.begin(), creds.dlcs.end(),
                                           [&](const DlcInfo& d) { return d.dlcId == dlcParsed.appId; });
                    if (it != creds.dlcs.end() && it->name.empty()) {
                        it->name = dName;
                    }
                }

                // 提取 DLC 专属物理 Depot 与清单 GID（强制继承 DLC 归属）
                for (const auto& dInfo : dlcParsed.depots) {
                    const uint32_t assocDlc = (dInfo.dlcId > 0) ? dInfo.dlcId : dlcParsed.appId;
                    auto it = std::find_if(depotKeys.begin(), depotKeys.end(),
                                           [&](const DepotKeyInfo& k) { return k.depotId == dInfo.depotId; });
                    if (it != depotKeys.end()) {
                        if (it->manifestId.empty() && !dInfo.manifestId.empty()) {
                            it->manifestId = dInfo.manifestId;
                        }
                        if (it->dlcId == 0) {
                            it->dlcId = assocDlc;
                        }
                    } else {
                        DepotKeyInfo newDk;
                        newDk.depotId = dInfo.depotId;
                        newDk.manifestId = dInfo.manifestId;
                        newDk.dlcId = assocDlc;
                        depotKeys.push_back(std::move(newDk));
                    }
                }
            }
        }

        // 4.4 纯官方云端向 Steam CM 查询真实物理 Depot 解密密钥 (先验授权鉴定，杜绝后续清单盲目撞门)
        std::vector<DepotKeyQuery> depotQueries;
        std::unordered_set<uint32_t> queriedDepotIds;
        for (const auto& dk : depotKeys) {
            // 严格剔除无独立 Depot 的纯逻辑 DLC 容器 (dlcId == depotId 或属于已知 DLC AppID 且无清单)
            if (dk.manifestId.empty() && (dk.depotId == dk.dlcId || knownDlcSet.contains(dk.depotId))) {
                continue;
            }
            // 严格剔除无任何有效清单的非 base 游戏条目 (未发布/已弃用/仅存在于私有分支的 Depot)
            if (dk.manifestId.empty() && dk.depotId != appId) {
                continue;
            }
            if (queriedDepotIds.insert(dk.depotId).second) {
                const uint32_t qAppId = (dk.dlcId > 0) ? dk.dlcId : appId;
                depotQueries.push_back({dk.depotId, qAppId});
            }
        }
        if (queriedDepotIds.insert(appId).second) {
            depotQueries.push_back({appId, appId});
        }

        LOG_DEBUG("SteamCM", MsgKey::LogCmFullQueryingDepotKeys, appId, depotQueries.size());
        if (!TuiEngine::IsActive()) {
            std::cout << TR(MsgKey::CliExtractQueryDepots) << "\n";
        }

        if (onProgress) {
            onProgress("keys", 0, 0, 0);
        }
        std::vector<DepotKeyInfo> cmKeys = RequestDepotKeys(depotQueries, appId);
        std::unordered_set<uint32_t> authorizedDepots;
        std::unordered_map<uint32_t, std::string> cmKeyMap;

        for (const auto& cmKey : cmKeys) {
            authorizedDepots.insert(cmKey.depotId);
            if (!cmKey.hexKey.empty()) {
                cmKeyMap[cmKey.depotId] = cmKey.hexKey;
            }
        }

        for (auto& dk : depotKeys) {
            if (dk.hexKey.empty()) {
                auto it = cmKeyMap.find(dk.depotId);
                if (it != cmKeyMap.end()) {
                    dk.hexKey = it->second;
                }
            }
        }

        for (const auto& cmKey : cmKeys) {
            if (std::none_of(depotKeys.begin(), depotKeys.end(),
                             [&](const DepotKeyInfo& k) { return k.depotId == cmKey.depotId; })) {
                depotKeys.push_back(cmKey);
            }
        }

        // 4.4 针对已授权物理 Depot 拉取官方清单文件 (免撞门两阶段流水线: CM 请求码收集 + 异步并发 CDN 下载)
        LOG_INFO("SteamCM", MsgKey::LogCmFullPullingManifests, authorizedDepots.size(), depotQueries.size());
        if (!TuiEngine::IsActive()) {
            std::cout << TR(MsgKey::CliExtractPullManifests) << "\n";
        }

        for (auto& dk : depotKeys) {
            if (IsValidManifestId(dk.manifestId)) {
                // 彻底杜绝撞门：仅对确认已授权的物理 Depot 请求清单
                if (!authorizedDepots.contains(dk.depotId)) {
                    LOG_DEBUG("SteamCM", MsgKey::LogCmFullSkipUnauthorizedDepot, dk.depotId);
                    continue;
                }

                // 本地磁盘快速检查：若已存在则直接秒级复用，完全跳过网络查询
                std::string fileName = std::format("{}_{}.manifest", dk.depotId, dk.manifestId);
                std::filesystem::path localPath = Utf8Path(outDir) / fileName;
                const std::string localPathUtf8 = PathToUtf8(localPath);
                std::error_code checkEc;
                if (std::filesystem::is_regular_file(localPath, checkEc) && std::filesystem::file_size(localPath, checkEc) > 0) {
                    LOG_INFO("SteamCM", MsgKey::LogCmManifestAlreadyExists, localPathUtf8);
                    dk.manifestFilePath = localPathUtf8;
                    ++downloadedManifests;
                    continue;
                }

                bool accessDenied = false;
                const uint32_t qAppId = (dk.dlcId > 0) ? dk.dlcId : appId;
                std::string reqCode = FetchManifestRequestCode(qAppId, dk.depotId, dk.manifestId, &accessDenied);
                if (!reqCode.empty() && reqCode != "0") {
                    downloadTasks.push_back({dk.depotId, dk.manifestId, std::move(reqCode)});
                }
            }
        }

        if (!downloadTasks.empty()) {
            const auto initialCdnServers = GetCdnServers(m_cellId);
            manifestFuture = std::async(std::launch::async, [tasks = std::move(downloadTasks), initialCdnServers, appId, outDir, onProgress]() {
                std::vector<std::pair<uint32_t, std::string>> results(tasks.size());
                std::atomic<size_t> nextIndex{0};
                std::atomic<size_t> completedCount{0};
                std::mutex progressMutex;
                const size_t numWorkers = std::min<size_t>(tasks.size(), 4);
                std::vector<std::thread> workers;
                workers.reserve(numWorkers);

                for (size_t w = 0; w < numWorkers; ++w) {
                    workers.emplace_back([&]() {
                        WinHttpTransport http;
                        http.SetTimeouts(3000, 2500, 5000, 15000);
                        while (true) {
                            size_t idx = nextIndex.fetch_add(1, std::memory_order_relaxed);
                            if (idx >= tasks.size()) break;
                            const auto& t = tasks[idx];
                            auto downloaded = DownloadManifestPayload(
                                appId, t.depotId, t.manifestId, t.reqCode, outDir, initialCdnServers, &http);
                            if (downloaded) {
                                results[idx] = {t.depotId, std::move(*downloaded)};
                            }
                            size_t done = completedCount.fetch_add(1, std::memory_order_relaxed) + 1;
                            if (onProgress) {
                                std::lock_guard lock(progressMutex);
                                onProgress("manifest", done, tasks.size(), t.depotId);
                            }
                        }
                    });
                }

                for (auto& worker : workers) {
                    if (worker.joinable()) worker.join();
                }
                return results;
            });
        }

        // 4.5 纯官方云端批量解析所有 DLC 名称 (Steam CM PICS 公开元数据查询)
        std::vector<uint32_t> missingDlcNames;
        for (const auto& d : creds.dlcs) {
            if (d.name.empty()) {
                missingDlcNames.push_back(d.dlcId);
            }
        }
        if (!missingDlcNames.empty()) {
            LOG_DEBUG("SteamCM", MsgKey::LogCmFullBatchQueryDlcNames, missingDlcNames.size());
            bool picsBatchOk = false;
            auto resolved = RequestPicsAppNames(missingDlcNames, &picsBatchOk);
            for (auto& d : creds.dlcs) {
                if (d.name.empty()) {
                    auto it = resolved.find(d.dlcId);
                    if (it != resolved.end() && !it->second.empty()) {
                        d.name = it->second;
                    }
                }
            }

            // 4.6 DLC 纯逻辑合法性认证与幽灵包切除门禁 (防误杀熔断 + 严格物理/元数据双空判定)
            if (picsBatchOk) {
                std::vector<uint32_t> purgedGhostIds;
                std::erase_if(creds.dlcs, [&](const DlcInfo& dlc) {
                    const bool hasPhysicalDepot = std::ranges::any_of(depotKeys, [&](const DepotKeyInfo& dk) noexcept {
                        return dk.depotId == dlc.dlcId || dk.dlcId == dlc.dlcId;
                    });
                    if (!hasPhysicalDepot && dlc.name.empty()) {
                        purgedGhostIds.push_back(dlc.dlcId);
                        LOG_DEBUG("SteamCM", MsgKey::LogCmFullPurgedGhostDlc, dlc.dlcId);
                        return true;
                    }
                    return false;
                });

                if (!purgedGhostIds.empty()) {
                    LOG_INFO("SteamCM", MsgKey::CliExtractPurgedGhostDlcs, purgedGhostIds.size());
                    if (!TuiEngine::IsActive()) {
                        std::cout << TR_FMT(MsgKey::CliExtractPurgedGhostDlcs, purgedGhostIds.size()) << "\n";
                    }
                    for (uint32_t gid : purgedGhostIds) {
                        knownDlcSet.erase(gid);
                    }
                }
            }
        }
    }

    // 5. 纯官方云端增量查询 64 位 PICS AccessTokens (AppID + 全部 DLC)
    if (onProgress) {
        onProgress("tokens", 0, 0, 0);
    }
    LOG_DEBUG("SteamCM", MsgKey::LogCmFullQueryingPicsTokens, appId);
    if (!TuiEngine::IsActive()) {
        std::cout << TR(MsgKey::CliExtractQueryAccessToken) << "\n";
    }

    std::unordered_set<uint32_t> targetAppIds;
    if (!creds.appTokens.contains(appId)) {
        targetAppIds.insert(appId);
    }
    for (const auto& dlc : creds.dlcs) {
        if (!creds.appTokens.contains(dlc.dlcId)) {
            targetAppIds.insert(dlc.dlcId);
        }
    }

    if (!targetAppIds.empty()) {
        std::vector<uint32_t> tokensToQuery(targetAppIds.begin(), targetAppIds.end());
        std::sort(tokensToQuery.begin(), tokensToQuery.end());
        auto cmTokens = RequestAppTokens(tokensToQuery);
        for (const auto& [tApp, tVal] : cmTokens) {
            if (tVal != 0) {
                creds.appTokens[tApp] = tVal;
            }
        }
    }

    if (!creds.appTokens.empty()) {
        LOG_INFO("SteamCM", MsgKey::LogCmFullExtractedPicsTokens, creds.appTokens.size());
        if (!TuiEngine::IsActive()) {
            std::cout << TR(MsgKey::CliExtractAccessTokenGot) << "\n";
        }
    }

    // 6. 等待并发 CDN 清单拉取任务收尾并合并路径结果
    if (manifestFuture.valid()) {
        if (onProgress) {
            onProgress("manifests_start", 0, downloadTasks.size(), 0);
        }
        auto asyncResults = manifestFuture.get();
        for (const auto& [depotId, path] : asyncResults) {
            if (!path.empty()) {
                for (auto& dk : depotKeys) {
                    if (dk.depotId == depotId) {
                        dk.manifestFilePath = path;
                        ++downloadedManifests;
                        break;
                    }
                }
            }
        }
    }

    if (downloadedManifests > 0 && !TuiEngine::IsActive()) {
        std::cout << TR_FMT(MsgKey::CliExtractManifestsGot, downloadedManifests) << "\n";
    }

    // 剔除无有效密钥、无清单文件且无有效清单ID的空项
    std::erase_if(depotKeys, [](const DepotKeyInfo& dk) {
        return dk.hexKey.empty() && dk.manifestFilePath.empty() && !IsValidManifestId(dk.manifestId);
    });

    std::sort(depotKeys.begin(), depotKeys.end(), [](const DepotKeyInfo& a, const DepotKeyInfo& b) {
        return a.depotId < b.depotId;
    });

    creds.depotKeys = std::move(depotKeys);

    if (!creds.depotKeys.empty()) {
        LOG_INFO("SteamCM", MsgKey::LogCmFullExtractedDepotKeys, creds.depotKeys.size());
        if (!TuiEngine::IsActive()) {
            std::cout << TR_FMT(MsgKey::CliExtractDepotKeysGot, creds.depotKeys.size()) << "\n";
        }
    }

    // 7. 全部凭据与清单提取完成
    if (onProgress) {
        onProgress("done", downloadedManifests, downloadTasks.size(), 0);
    }

    return creds;
}

} // namespace OST::ExtractTickets
