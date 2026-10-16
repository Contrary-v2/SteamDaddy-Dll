#include "SteamAuthService.h"
#include "AppInfoParser.h"
#include "I18n.h"
#include "Log.h"
#include "SteamCmClient.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_set>
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
    struct ScopedStringWiper {
        std::string& str;
        explicit ScopedStringWiper(std::string& s) noexcept : str(s) {}
        ~ScopedStringWiper() noexcept { SecureWipeString(str); }
        ScopedStringWiper(const ScopedStringWiper&) = delete;
        ScopedStringWiper& operator=(const ScopedStringWiper&) = delete;
    };

    constexpr bool IsUrlUnreserved(unsigned char uc) noexcept {
        return (uc >= 'A' && uc <= 'Z') ||
               (uc >= 'a' && uc <= 'z') ||
               (uc >= '0' && uc <= '9') ||
               uc == '-' || uc == '_' || uc == '.' || uc == '~';
    }

    constexpr size_t ComputeUrlEncodedSize(std::string_view str) noexcept {
        size_t len = 0;
        for (char c : str) {
            len += IsUrlUnreserved(static_cast<unsigned char>(c)) ? 1 : 3;
        }
        return len;
    }

    void AppendUrlEncoded(std::string& dest, std::string_view str) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        for (char c : str) {
            const auto uc = static_cast<unsigned char>(c);
            if (IsUrlUnreserved(uc)) {
                dest.push_back(c);
            } else {
                dest.push_back('%');
                dest.push_back(kHex[(uc >> 4) & 0x0F]);
                dest.push_back(kHex[uc & 0x0F]);
            }
        }
    }

    std::string UrlEncode(std::string_view str) {
        std::string escaped;
        escaped.reserve(ComputeUrlEncodedSize(str));
        AppendUrlEncoded(escaped, str);
        return escaped;
    }

    SteamLoginResult RunDualTrack2FaLoop(
        SteamAuthService& authService,
        const SteamAuthSession& session,
        std::string_view accountName,
        bool hasDeviceCode,
        bool hasDeviceConfirmation,
        bool hasEmailCode,
        std::string_view emailAddress) {

        SteamLoginResult result;
        result.accountName = accountName;
        result.steamId = session.steamId;

        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int modalW = std::clamp(w - 12, 64, 84);
        const int modalH = 10;
        const int top = (h - modalH) / 2;
        const int left = (w - modalW) / 2;
        const size_t innerW = static_cast<size_t>(modalW - 8);

        std::string title = hasEmailCode
            ? std::string(TR(MsgKey::GuardEmailTitle))
            : std::string(TR(MsgKey::GuardMobileTitle));

        std::string prompt;
        if (hasEmailCode) {
            prompt = emailAddress.empty()
                ? std::string(TR(MsgKey::GuardEmailTitle))
                : TR_FMT(MsgKey::GuardEmailPrompt, MaskEmail(emailAddress));
        } else if (hasDeviceConfirmation || hasDeviceCode) {
            prompt = std::string(TR(MsgKey::GuardMobileDualPrompt));
        } else {
            prompt = std::string(TR(MsgKey::GuardMobilePrompt));
        }

        TuiEngine::ClearScreen();
        TuiEngine::DrawBox(top, left, modalW, modalH, title);
        TuiEngine::PrintBounded(top + 2, left + 4, prompt, innerW, "\x1b[1;37m");

        TuiEngine::DrawFooter(TR(MsgKey::Guard2FaFooter));

        std::string code;
        ScopedStringWiper codeWiper{code};
        std::string statusMsg;
        std::string statusStyle = "\x1b[90m";
        bool inputAllowed = true;

        constexpr int kMaxDurationMs = 120000; // 2 minutes
        constexpr int kPollIntervalMs = 1500;  // 1.5s
        const auto startTime = std::chrono::steady_clock::now();
        auto lastPollTime = startTime;

        auto getElapsedMs = [&]() -> int {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count());
        };

        auto drawInputAndStatus = [&]() {
            if (inputAllowed) {
                TuiEngine::MoveCursor(top + 4, left + 4);
                std::string displayCode = std::format("[ {:<8} ]", code + "_");
                std::cout << "\x1b[1;30;47m" << displayCode << "\x1b[0m";
                SecureWipeString(displayCode);
            }
            TuiEngine::MoveCursor(top + 6, left + 4);
            std::string currentStatus = statusMsg;
            if (currentStatus.empty()) {
                if (hasDeviceConfirmation) {
                    currentStatus = TR_FMT(MsgKey::Guard2FaWaitingMobile, getElapsedMs() / 1000, kMaxDurationMs / 1000);
                    statusStyle = "\x1b[90m";
                } else {
                    currentStatus = std::string(TR(MsgKey::Guard2FaEnterCodeHint));
                    statusStyle = "\x1b[90m";
                }
            }
            std::cout << statusStyle << TuiEngine::Pad(TuiEngine::TruncateToWidth(currentStatus, innerW), innerW) << "\x1b[0m";
            std::cout.flush();
        };

        drawInputAndStatus();

        while (getElapsedMs() < kMaxDurationMs) {
            auto keyOpt = TuiEngine::PollKey(50);

            if (keyOpt) {
                KeyEvent ev = *keyOpt;
                if (ev.code == KeyCode::Escape) {
                    result.cancelled = true;
                    result.errorMessage = std::string(TR(MsgKey::ErrUserCancelledInput));
                    LOG_INFO("SteamAuth", MsgKey::LogAuthEscCancel2Fa);
                    return result;
                }

                if (inputAllowed) {
                    if (ev.code == KeyCode::Backspace) {
                        if (!code.empty()) {
                            code.pop_back();
                            statusMsg.clear();
                            drawInputAndStatus();
                        }
                    } else if (ev.code == KeyCode::Char) {
                        std::string toAdd = !ev.text.empty() ? ev.text : (ev.ch != 0 ? std::string(1, ev.ch) : "");
                        bool changed = false;
                        for (char c : toAdd) {
                            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                                if (code.size() < 8) {
                                    code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                                    changed = true;
                                }
                            }
                        }
                        SecureWipeString(toAdd);
                        if (changed) {
                            statusMsg.clear();
                            drawInputAndStatus();
                        }
                    } else if (ev.code == KeyCode::Enter) {
                        if (!code.empty()) {
                            statusMsg = std::string(TR(MsgKey::Guard2FaSubmitting));
                            statusStyle = "\x1b[1;33m";
                            drawInputAndStatus();

                            int codeType = hasEmailCode ? 2 : 3;
                            if (authService.SubmitSteamGuardCode(session, code, codeType)) {
                                auto pollRes = authService.PollAuthSessionOnce(session, 2500);
                                if (pollRes) {
                                    result.success = true;
                                    result.refreshToken = std::move(pollRes->first);
                                    result.accessToken = std::move(pollRes->second);
                                    SecureWipeString(pollRes->first);
                                    SecureWipeString(pollRes->second);
                                    LOG_INFO("SteamAuth", MsgKey::LogAuth2FaCodeVerified);
                                    return result;
                                }
                            }
                            SecureWipeString(code);
                            statusMsg = std::string(TR(MsgKey::GuardCodeIncorrect));
                            statusStyle = "\x1b[1;31m";
                            drawInputAndStatus();
                        } else if (hasDeviceConfirmation) {
                            statusMsg = std::string(TR(MsgKey::Guard2FaCheckingMobile));
                            statusStyle = "\x1b[1;36m";
                            drawInputAndStatus();

                            auto pollRes = authService.PollAuthSessionOnce(session, 2500);
                            if (pollRes) {
                                LOG_INFO("SteamAuth", MsgKey::LogAuthMobileApprovedEnter);
                                result.success = true;
                                result.refreshToken = std::move(pollRes->first);
                                result.accessToken = std::move(pollRes->second);
                                SecureWipeString(pollRes->first);
                                SecureWipeString(pollRes->second);
                                return result;
                            }
                            statusMsg = std::string(TR(MsgKey::Guard2FaNotApprovedYet));
                            statusStyle = "\x1b[1;33m";
                            drawInputAndStatus();
                        }
                    }
                }
            }

            const auto now = std::chrono::steady_clock::now();
            const auto sinceLastPollMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPollTime).count();
            if (hasDeviceConfirmation && sinceLastPollMs >= kPollIntervalMs) {
                lastPollTime = now;
                drawInputAndStatus();

                auto pollRes = authService.PollAuthSessionOnce(session, 2500);
                if (pollRes) {
                    LOG_INFO("SteamAuth", MsgKey::LogAuthMobileApprovedZeroKey);
                    result.success = true;
                    result.refreshToken = std::move(pollRes->first);
                    result.accessToken = std::move(pollRes->second);
                    SecureWipeString(pollRes->first);
                    SecureWipeString(pollRes->second);
                    return result;
                }
            }
        }

        result.errorMessage = std::string(TR(MsgKey::ErrAuthTimeoutOrCancelled));
        LOG_WARN("SteamAuth", "{}", result.errorMessage);
        return result;
    }
} // namespace

SteamAuthService::SteamAuthService() = default;

std::optional<SteamRsaKey> SteamAuthService::GetPasswordRsaKey(std::string_view accountName) {
    std::string url = "https://api.steampowered.com/IAuthenticationService/GetPasswordRSAPublicKey/v1?account_name=" + UrlEncode(accountName);
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthRequestingRsa, MaskAccount(accountName));

    HttpResponse resp = m_http.Get(url);
    if (!resp.IsSuccess()) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthGetRsaFailed, resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    auto modOpt = JsonHelper::GetString(resp.body, "publickey_mod");
    auto expOpt = JsonHelper::GetString(resp.body, "publickey_exp");
    auto timeOpt = JsonHelper::GetUInt64(resp.body, "timestamp");

    if (!modOpt || !expOpt || !timeOpt) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthParseRsaFailed);
        return std::nullopt;
    }

    SteamRsaKey rsaKey;
    rsaKey.modHex = *modOpt;
    rsaKey.expHex = *expOpt;
    rsaKey.timestamp = *timeOpt;
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthGetRsaSuccess, rsaKey.timestamp);
    return rsaKey;
}

std::optional<SteamAuthSession> SteamAuthService::BeginAuthSession(
    std::string_view accountName,
    const SecureString& password,
    const SteamRsaKey& rsaKey) {

    std::string encryptedPassword = EncryptPasswordWithRSA(password, rsaKey.modHex, rsaKey.expHex);
    ScopedStringWiper encPwdWiper{encryptedPassword};
    if (encryptedPassword.empty()) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthRsaEncryptFailed);
        return std::nullopt;
    }

    std::string deviceName = "OpenSteamTool";
#if defined(_WIN32)
    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD compLen = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(compName, &compLen) && compLen > 0) {
        std::string utf8Comp = WideToUtf8(std::wstring_view(compName, compLen));
        if (!utf8Comp.empty()) {
            deviceName = std::move(utf8Comp);
        }
    }
#endif

    const std::string tsStr = std::to_string(rsaKey.timestamp);
    std::string postData;
    postData.reserve(140 + ComputeUrlEncodedSize(accountName) +
                     ComputeUrlEncodedSize(encryptedPassword) +
                     tsStr.size() +
                     ComputeUrlEncodedSize(deviceName));
    postData.append("account_name=");
    AppendUrlEncoded(postData, accountName);
    postData.append("&encrypted_password=");
    AppendUrlEncoded(postData, encryptedPassword);
    postData.append("&encryption_timestamp=");
    postData.append(tsStr);
    postData.append("&remember_login=true&platform_type=1&persistence=1&website_id=Client&device_friendly_name=");
    AppendUrlEncoded(postData, deviceName);
    ScopedStringWiper postDataWiper{postData};

    std::string url = "https://api.steampowered.com/IAuthenticationService/BeginAuthSessionViaCredentials/v1";
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthBeginSession, MaskAccount(accountName));

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    ScopedStringWiper respWiper{resp.body};
    if (!resp.IsSuccess() || (resp.steamEResult != 0 && resp.steamEResult != 1)) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthBeginSessionHttpFailed, resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    auto clientIdOpt = JsonHelper::GetString(resp.body, "client_id");
    auto requestIdOpt = JsonHelper::GetString(resp.body, "request_id");
    auto steamIdOpt = JsonHelper::GetUInt64(resp.body, "steamid");

    if (!clientIdOpt || !requestIdOpt) {
        LOG_ERROR("SteamAuth", MsgKey::LogAuthMissingClientOrReqId, resp.statusCode);
        return std::nullopt;
    }

    SteamAuthSession session;
    session.clientId = *clientIdOpt;
    session.requestId = *requestIdOpt;
    session.steamId = steamIdOpt.value_or(0);
    session.allowedConfirmations = JsonHelper::GetConfirmations(resp.body);

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthSessionCreated,
              MaskSteamId(session.steamId), session.allowedConfirmations.size());
    return session;
}

bool SteamAuthService::SubmitSteamGuardCode(
    const SteamAuthSession& session,
    std::string_view code,
    int codeType) {

    const std::string steamIdStr = std::to_string(session.steamId);
    const std::string codeTypeStr = std::to_string(codeType);
    std::string postData;
    postData.reserve(48 + ComputeUrlEncodedSize(session.clientId) +
                     steamIdStr.size() +
                     ComputeUrlEncodedSize(code) +
                     codeTypeStr.size());
    postData.append("client_id=");
    AppendUrlEncoded(postData, session.clientId);
    postData.append("&steamid=");
    postData.append(steamIdStr);
    postData.append("&code=");
    AppendUrlEncoded(postData, code);
    postData.append("&code_type=");
    postData.append(codeTypeStr);
    ScopedStringWiper postDataWiper{postData};

    std::string url = "https://api.steampowered.com/IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1";
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthSubmitting2Fa, codeType);

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded", {}, 5000);
    ScopedStringWiper respWiper{resp.body};
    if (!resp.IsSuccess() || (resp.steamEResult != 0 && resp.steamEResult != 1)) {
        LOG_WARN("SteamAuth", MsgKey::LogAuthSubmit2FaHttpFailed, resp.statusCode, resp.errorMessage);
        return false;
    }

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthSubmit2FaSuccess);
    return true;
}

std::optional<std::pair<std::string, std::string>> SteamAuthService::PollAuthSessionOnce(
    const SteamAuthSession& session,
    int timeoutMs) {

    std::string postData;
    postData.reserve(32 + ComputeUrlEncodedSize(session.clientId) + ComputeUrlEncodedSize(session.requestId));
    postData.append("client_id=");
    AppendUrlEncoded(postData, session.clientId);
    postData.append("&request_id=");
    AppendUrlEncoded(postData, session.requestId);
    ScopedStringWiper postDataWiper{postData};

    std::string url = "https://api.steampowered.com/IAuthenticationService/PollAuthSessionStatus/v1";

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded", {}, timeoutMs);
    ScopedStringWiper respWiper{resp.body};
    if (!resp.IsSuccess()) {
        return std::nullopt;
    }

    auto refreshOpt = JsonHelper::GetString(resp.body, "refresh_token");
    auto accessOpt = JsonHelper::GetString(resp.body, "access_token");

    if (refreshOpt && !refreshOpt->empty()) {
        std::pair<std::string, std::string> out{std::move(*refreshOpt), accessOpt ? std::move(*accessOpt) : std::string{}};
        SecureWipeString(*refreshOpt);
        if (accessOpt) SecureWipeString(*accessOpt);
        return out;
    }
    if (refreshOpt) SecureWipeString(*refreshOpt);
    if (accessOpt) SecureWipeString(*accessOpt);
    return std::nullopt;
}

SteamLoginResult SteamAuthService::PollAuthSession(
    const SteamAuthSession& session,
    std::string_view accountName,
    int maxAttempts,
    int delayMs) {

    SteamLoginResult result;
    result.accountName = accountName;
    result.steamId = session.steamId;

    if (TuiEngine::IsActive()) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int modalW = std::clamp(w - 12, 60, 80);
        const int modalH = 6;
        const int top = (h - modalH) / 2, left = (w - modalW) / 2;
        TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ConnectingTitle));
        TuiEngine::MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << TR(MsgKey::GuardDeviceDetail) << "\x1b[0m";
        std::cout.flush();
    }

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (TuiEngine::IsActive()) {
            int w = 80, h = 25;
            TuiEngine::GetScreenSize(w, h);
            const int modalW = std::clamp(w - 12, 60, 80);
            const int modalH = 6;
            const int top = (h - modalH) / 2, left = (w - modalW) / 2;
            std::string pollMsg = std::format("{} ({}/{}s)", TR(MsgKey::ConnectingGatewayMsg), (attempt + 1) * delayMs / 1000, (maxAttempts * delayMs) / 1000);
            TuiEngine::PrintBounded(top + 2, left + 4, pollMsg, static_cast<size_t>(modalW - 8), "\x1b[1;36m");
            std::cout.flush();
        }

        // Poll immediately on attempt == 0; only sleep before subsequent attempts
        if (attempt > 0) {
            const auto delayStart = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - delayStart).count() < delayMs) {
                auto keyOpt = TuiEngine::PollKey(50);
                if (keyOpt && keyOpt->code == KeyCode::Escape) {
                    TuiEngine::FlushInputBuffer();
                    result.cancelled = true;
                    result.errorMessage = std::string(TR(MsgKey::ErrUserCancelledPoll));
                    LOG_INFO("SteamAuth", MsgKey::LogAuthEscCancelPoll);
                    return result;
                }
            }
        }

        auto tokens = PollAuthSessionOnce(session, 2500);
        if (tokens) {
            result.success = true;
            result.refreshToken = std::move(tokens->first);
            result.accessToken = std::move(tokens->second);
            SecureWipeString(tokens->first);
            SecureWipeString(tokens->second);
            LOG_DEBUG("SteamAuth", MsgKey::LogAuthPollSuccess,
                      MaskToken(result.refreshToken), MaskToken(result.accessToken));
            return result;
        }
    }

    result.errorMessage = std::string(TR(MsgKey::ErrAuthTimeoutOrCancelled));
    LOG_WARN("SteamAuth", "{}", result.errorMessage);
    return result;
}

std::optional<std::string> SteamAuthService::RefreshAccessToken(
    uint64_t steamId,
    std::string_view refreshToken,
    std::string* outNewRefreshToken) {

    if (outNewRefreshToken) {
        SecureWipeString(*outNewRefreshToken);
    }

    if (steamId == 0) {
        steamId = GetJwtSteamId(refreshToken);
    }

    const std::string steamIdStr = std::to_string(steamId);
    std::string postData;
    postData.reserve(48 + ComputeUrlEncodedSize(refreshToken) + steamIdStr.size());
    postData.append("refresh_token=");
    AppendUrlEncoded(postData, refreshToken);
    postData.append("&steamid=");
    postData.append(steamIdStr);
    postData.append("&renewal_type=1");
    ScopedStringWiper postDataWiper{postData};

    std::string url = "https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1";
    LOG_DEBUG("SteamAuth", MsgKey::LogAuthRefreshingAccess, MaskSteamId(steamId));

    HttpResponse resp = m_http.Post(url, postData, "application/x-www-form-urlencoded");
    ScopedStringWiper respWiper{resp.body};
    if (!resp.IsSuccess()) {
        LOG_WARN("SteamAuth", MsgKey::LogAuthRefreshHttpFailed, resp.statusCode, resp.errorMessage);
        return std::nullopt;
    }

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthGenAccessHttpOk, resp.statusCode);

    auto accessOpt = JsonHelper::GetString(resp.body, "access_token");
    auto refreshOpt = JsonHelper::GetString(resp.body, "refresh_token");

    if (refreshOpt && !refreshOpt->empty()) {
        if (outNewRefreshToken) {
            *outNewRefreshToken = std::move(*refreshOpt);
        }
        SecureWipeString(*refreshOpt);
    }

    if (accessOpt && !accessOpt->empty()) {
        LOG_DEBUG("SteamAuth", MsgKey::LogAuthRefreshSuccess, MaskToken(*accessOpt));
        return accessOpt;
    }

    LOG_WARN("SteamAuth", MsgKey::LogAuthNoAccessInResp, resp.statusCode);
    return std::nullopt;
}

std::vector<OwnedGameInfo> SteamAuthService::FetchOwnedGames(
    uint64_t steamId,
    std::string_view accessToken,
    SteamCmClient* cmClient) {

    const std::string steamIdStr = std::to_string(steamId);
    const std::string_view langCode = I18n::GetSteamLanguageCode();

    std::string url;
    url.reserve(160 + ComputeUrlEncodedSize(accessToken) + steamIdStr.size() + langCode.size());
    url.append("https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?access_token=");
    AppendUrlEncoded(url, accessToken);
    url.append("&steamid=");
    url.append(steamIdStr);
    url.append("&include_appinfo=1&include_played_free_games=0&language=");
    url.append(langCode);
    ScopedStringWiper urlWiper{url};

    LOG_DEBUG("SteamAuth", MsgKey::LogAuthFetchingOwnedGames);
    HttpResponse resp = m_http.Get(url);
    ScopedStringWiper respWiper{resp.body};

    std::vector<OwnedGameInfo> games;
    if (resp.IsSuccess()) {
        games = JsonHelper::ParseOwnedGames(resp.body);
        LOG_DEBUG("SteamAuth", MsgKey::LogAuthFetchedOwnedGames, games.size());
    } else {
        LOG_WARN("SteamAuth", MsgKey::LogAuthFetchOwnedGamesFailed, resp.statusCode, resp.errorMessage);
    }

    // 查询当前账号的 Steam 家庭组 (IFamilyGroupsService)
    std::string familyUrl;
    familyUrl.reserve(140 + ComputeUrlEncodedSize(accessToken));
    familyUrl.append("https://api.steampowered.com/IFamilyGroupsService/GetFamilyGroupForUser/v1/?access_token=");
    AppendUrlEncoded(familyUrl, accessToken);
    familyUrl.append("&include_family_group_response=true");
    ScopedStringWiper familyUrlWiper{familyUrl};

    HttpResponse famResp = m_http.Get(familyUrl);
    ScopedStringWiper famRespWiper{famResp.body};
    if (famResp.IsSuccess()) {
        auto familyGroupIdOpt = JsonHelper::GetString(famResp.body, "family_groupid");
        if (!familyGroupIdOpt || familyGroupIdOpt->empty() || *familyGroupIdOpt == "0") {
            auto famGroupNum = JsonHelper::GetUInt64(famResp.body, "family_groupid");
            if (famGroupNum && *famGroupNum > 0) {
                familyGroupIdOpt = std::to_string(*famGroupNum);
            }
        }

        if (familyGroupIdOpt && !familyGroupIdOpt->empty() && *familyGroupIdOpt != "0") {
            LOG_INFO("SteamAuth", MsgKey::LogAuthDetectedFamilyGroup, MaskGroupId(*familyGroupIdOpt));
            std::string sharedUrl;
            sharedUrl.reserve(180 + ComputeUrlEncodedSize(accessToken) + familyGroupIdOpt->size() + langCode.size());
            sharedUrl.append("https://api.steampowered.com/IFamilyGroupsService/GetSharedLibraryApps/v1/?access_token=");
            AppendUrlEncoded(sharedUrl, accessToken);
            sharedUrl.append("&family_groupid=");
            AppendUrlEncoded(sharedUrl, *familyGroupIdOpt);
            sharedUrl.append("&include_own=false&include_non_games=false&language=");
            sharedUrl.append(langCode);
            ScopedStringWiper sharedUrlWiper{sharedUrl};

            HttpResponse sharedResp = m_http.Get(sharedUrl);
            ScopedStringWiper sharedRespWiper{sharedResp.body};
            if (sharedResp.IsSuccess()) {
                auto sharedGames = JsonHelper::ParseSharedLibraryApps(sharedResp.body);
                LOG_INFO("SteamAuth", MsgKey::LogAuthFoundSharedGames, sharedGames.size());

                std::unordered_set<uint32_t> existing;
                for (const auto& g : games) {
                    existing.insert(g.appId);
                }

                // Collect all shared game AppIDs not already owned directly
                std::unordered_set<uint32_t> sharedAppIds;
                std::unordered_set<uint32_t> missingNameAppIds;
                for (const auto& sg : sharedGames) {
                    if (!existing.contains(sg.appId)) {
                        sharedAppIds.insert(sg.appId);
                        if (sg.name.empty()) {
                            missingNameAppIds.insert(sg.appId);
                        }
                    }
                }

                std::unordered_map<uint32_t, std::string> resolvedNames;
                if (!sharedAppIds.empty()) {
                    auto steamPathOpt = FindSteamInstallPath();
                    if (steamPathOpt && !steamPathOpt->empty()) {
                        // Fast local appinfo.vdf lookup (<1ms) prioritizing name_localized for all shared games
                        resolvedNames = ParseAppNames(*steamPathOpt, sharedAppIds);
                    }

                    std::vector<uint32_t> stillMissing;
                    for (uint32_t mId : missingNameAppIds) {
                        if (!resolvedNames.contains(mId) || resolvedNames[mId].empty()) {
                            stillMissing.push_back(mId);
                        }
                    }

                    // High-speed batch PICS resolution via CM Client (50 apps/batch, ~100ms, immune to Store HTTP 429)
                    if (!stillMissing.empty() && cmClient && cmClient->EnsureConnected()) {
                        LOG_INFO("SteamAuth", MsgKey::LogAuthBatchResolvingSharedNames, stillMissing.size());
                        auto picsNames = cmClient->RequestPicsAppNames(stillMissing);
                        for (auto& [pAppId, pName] : picsNames) {
                            if (!pName.empty()) {
                                resolvedNames[pAppId] = std::move(pName);
                            }
                        }
                    }
                }

                for (auto& sg : sharedGames) {
                    if (!existing.contains(sg.appId)) {
                        existing.insert(sg.appId);
                        auto it = resolvedNames.find(sg.appId);
                        if (it != resolvedNames.end() && !it->second.empty()) {
                            sg.name = it->second;
                        } else if (sg.name.empty()) {
                            sg.name = "App " + std::to_string(sg.appId);
                        }
                        sg.isShared = true;
                        games.push_back(std::move(sg));
                    }
                }
            } else {
                LOG_WARN("SteamAuth", MsgKey::LogAuthFetchSharedGamesFailed, sharedResp.statusCode, sharedResp.errorMessage);
            }
        }
    }

    return games;
}

SteamLoginResult SteamAuthService::LoginWithCredentials(
    std::string_view accountName,
    const SecureString& password) {

    SteamLoginResult failResult;
    failResult.accountName = accountName;

    auto rsaKey = GetPasswordRsaKey(accountName);
    if (!rsaKey) {
        failResult.errorMessage = std::string(TR(MsgKey::ErrRsaKeyFailed));
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    auto session = BeginAuthSession(accountName, password, *rsaKey);
    if (!session) {
        failResult.errorMessage = std::string(TR(MsgKey::ErrAuthSessionFailed));
        LOG_WARN("SteamAuth", "{}", failResult.errorMessage);
        return failResult;
    }

    bool hasDeviceCode = false;
    bool hasDeviceConfirmation = false;
    bool hasEmailCode = false;
    std::string emailAddress;

    for (const auto& conf : session->allowedConfirmations) {
        LOG_INFO("SteamAuth", MsgKey::LogAuth2FaRequired, conf.type,
                 conf.type == 2 ? MaskEmail(conf.associatedMessage) : conf.associatedMessage);
        if (conf.type == 3) {
            hasDeviceCode = true;
        } else if (conf.type == 4) {
            hasDeviceConfirmation = true;
        } else if (conf.type == 2) {
            hasEmailCode = true;
            if (!conf.associatedMessage.empty()) {
                emailAddress = conf.associatedMessage;
            }
        }
    }

    SteamLoginResult result;
    if (hasDeviceCode || hasDeviceConfirmation || hasEmailCode) {
        if (TuiEngine::IsActive()) {
            result = RunDualTrack2FaLoop(*this, *session, accountName,
                                         hasDeviceCode, hasDeviceConfirmation,
                                         hasEmailCode, emailAddress);
        } else {
            if (hasDeviceCode || hasEmailCode) {
                std::string promptMsg = hasEmailCode
                    ? (emailAddress.empty() ? std::string(TR(MsgKey::GuardEmailTitle)) : TR_FMT(MsgKey::GuardEmailPrompt, MaskEmail(emailAddress)))
                    : std::string(TR(MsgKey::GuardMobilePrompt));
                std::cout << "[2FA] " << promptMsg << " ";
                std::string code;
                ScopedStringWiper cliCodeWiper{code};
                if (std::cin >> code && !code.empty()) {
                    SubmitSteamGuardCode(*session, code, hasDeviceCode ? 3 : 2);
                }
            }
            result = PollAuthSession(*session, accountName);
        }
    } else {
        result = PollAuthSession(*session, accountName);
    }
    if (!result.success) {
        if (!result.cancelled) {
            LOG_WARN("SteamAuth", "{}", result.errorMessage);
        }
        return result;
    }

    if (result.steamId == 0) {
        result.steamId = GetJwtSteamId(result.refreshToken);
    }

    CachedAccount cached;
    cached.accountName = std::string{accountName};
    cached.steamId = result.steamId;
    cached.refreshToken = result.refreshToken;
    cached.accessToken = result.accessToken;
    cached.lastLoginTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    TokenStorage::UpsertAccount(cached);
    return result;
}

} // namespace OST::ExtractTickets
