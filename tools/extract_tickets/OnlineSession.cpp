#include "OnlineSession.h"
#include "Crypto.h"
#include "GameListManager.h"
#include "I18n.h"
#include "Log.h"
#include "OutputWriter.h"
#include "SteamAuthService.h"
#include "SteamCmClient.h"
#include "TokenStorage.h"
#include "TuiEngine.h"
#include "Utils.h"

#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace OST::ExtractTickets {

namespace {
    std::string FormatTimestamp(int64_t t) {
        if (t <= 0) return std::string{TR(MsgKey::TimestampUnknown)};
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm tmVal{};
#if defined(_WIN32)
        localtime_s(&tmVal, &tt);
#else
        localtime_r(&tt, &tmVal);
#endif
        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}",
                           tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday,
                           tmVal.tm_hour, tmVal.tm_min);
    }

    [[nodiscard]] constexpr bool IsCredentialInvalidEResult(int32_t eresult) noexcept {
        switch (eresult) {
            case 5:  // k_EResultInvalidPassword
            case 8:  // k_EResultInvalidParam / InvalidToken
            case 15: // k_EResultAccessDenied
            case 27: // k_EResultExpired
            case 63: // k_EResultAccountLogonDenied
            case 79: // k_EResultRequirePasswordReEntry
            case 82: // k_EResultInvalidSignature
            case 85: // k_EResultAccountLoginDeniedNeedTwoFactor
                return true;
            default:
                return false;
        }
    }

    void RenderLevel2Tui(const std::vector<CachedAccount>& accounts, size_t selected, bool fullClear = false) {
        if (!TuiEngine::EnsureMinTerminalSize(76, 18)) {
            return;
        }
        TuiEngine::BeginFrame();
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        if (fullClear) {
            TuiEngine::ClearScreen();
        }

        TuiEngine::DrawHeader("extract_tickets",
                              TR_FMT(MsgKey::L2HeaderTagAccounts, accounts.size()));

        const int boxW = std::clamp(w - 4, 70, 110);
        const int maxBoxH = (std::min)(26, h - 2);
        const int boxH = std::clamp(h - 4, 14, maxBoxH);
        const int top = 2 + (h - 2 - boxH) / 2;
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, TR(MsgKey::L2BoxTitle).data(), /*clearInterior=*/false);

        const size_t innerW = static_cast<size_t>(boxW - 8);

        // Subtitle explanation
        TuiEngine::PrintBounded(top + 2, left + 4,
            TR(MsgKey::L2Subtitle),
            innerW, "\x1b[90m");

        // Item 0: [+] Login New Account (Default)
        std::string item0 = std::format("   {}", TR(MsgKey::L2ActionLogin));
        if (selected == 0) {
            item0[1] = '>';
            TuiEngine::PrintBounded(top + 4, left + 4, item0, innerW, "\x1b[30;107m");
        } else {
            TuiEngine::PrintBounded(top + 4, left + 4, item0, innerW, "\x1b[1;37m");
        }

        // Cached accounts with scrollable viewport
        const size_t maxVisibleAcc = static_cast<size_t>((std::max)(1, boxH - 10));
        size_t accScroll = 0;
        if (accounts.size() > maxVisibleAcc) {
            if (selected >= 1 && selected <= accounts.size()) {
                size_t accIdx = selected - 1;
                if (accIdx >= maxVisibleAcc) {
                    accScroll = accIdx - maxVisibleAcc + 1;
                }
            } else if (selected > accounts.size()) {
                accScroll = accounts.size() - maxVisibleAcc;
            }
        }
        size_t visibleCount = (std::min)(accounts.size() - accScroll, maxVisibleAcc);

        for (size_t v = 0; v < visibleCount; ++v) {
            size_t i = accScroll + v;
            const auto& acc = accounts[i];
            const size_t itemIdx = i + 1;

            std::string prefix = std::format("   [{}] ", itemIdx);
            if (selected == itemIdx) {
                prefix[1] = '>';
            }
            std::string timeStr = std::format("({})", FormatTimestamp(acc.lastLoginTime));
            std::string statusTag = acc.isInvalid ? std::string{TR(MsgKey::L2TagInvalidCredential)}
                                                  : std::string{};
            std::string rightPart = statusTag.empty()
                                        ? std::format("  {}", timeStr)
                                        : std::format("  {}  {}", timeStr, statusTag);
            size_t rightW = TuiEngine::GetDisplayWidth(rightPart);
            size_t prefixW = TuiEngine::GetDisplayWidth(prefix);

            std::string lineText;
            if (innerW > prefixW + rightW + 4) {
                size_t availForIdentity = innerW - prefixW - rightW;
                std::string identity;
                if (acc.alias.empty()) {
                    identity = TuiEngine::TruncateToWidth(acc.accountName, availForIdentity);
                } else {
                    std::string fullAlias = std::format(" [{}]", acc.alias);
                    size_t nameW = TuiEngine::GetDisplayWidth(acc.accountName);
                    size_t aliasW = TuiEngine::GetDisplayWidth(fullAlias);
                    if (nameW + aliasW <= availForIdentity) {
                        identity = acc.accountName + fullAlias;
                    } else {
                        size_t minNameW = (std::min)(nameW, static_cast<size_t>(10));
                        size_t maxNameW = (availForIdentity > 10) ? (availForIdentity - 8) : availForIdentity;
                        size_t allocName = std::clamp(nameW, minNameW, maxNameW);
                        std::string truncName = TuiEngine::TruncateToWidth(acc.accountName, allocName);
                        size_t usedNameW = TuiEngine::GetDisplayWidth(truncName);
                        std::string aliasDisp;
                        if (availForIdentity > usedNameW + 4) {
                            size_t availAlias = availForIdentity - usedNameW - 3;
                            std::string truncAlias = TuiEngine::TruncateToWidth(acc.alias, (availAlias > 3) ? (availAlias - 3) : 1);
                            aliasDisp = std::format(" [{}...]", truncAlias);
                        }
                        identity = truncName + aliasDisp;
                    }
                }
                std::string leftPart = prefix + identity;
                size_t leftW = TuiEngine::GetDisplayWidth(leftPart);
                size_t padSpaces = (innerW >= leftW + rightW) ? (innerW - leftW - rightW) : 0;
                lineText = leftPart + std::string(padSpaces, ' ') + rightPart;
            } else {
                std::string fallbackLeft = prefix + acc.accountName;
                size_t tagW = TuiEngine::GetDisplayWidth(statusTag);
                if (!statusTag.empty() && innerW > tagW + 2) {
                    std::string truncFb = TuiEngine::TruncateToWidth(fallbackLeft, innerW - tagW - 1);
                    size_t curW = TuiEngine::GetDisplayWidth(truncFb);
                    size_t pad = (innerW >= curW + tagW) ? (innerW - curW - tagW) : 0;
                    lineText = truncFb + std::string(pad, ' ') + statusTag;
                } else {
                    lineText = TuiEngine::TruncateToWidth(fallbackLeft, innerW);
                }
            }

            std::string itemStyle;
            if (selected == itemIdx) {
                itemStyle = acc.isInvalid ? "\x1b[30;43m" : "\x1b[30;107m";
            } else {
                itemStyle = acc.isInvalid ? "\x1b[1;33m" : "\x1b[37m";
            }
            TuiEngine::PrintBounded(top + 5 + static_cast<int>(v), left + 4, lineText, innerW, itemStyle);
        }

        // Optional wipe all
        if (!accounts.empty()) {
            const size_t wipeIdx = accounts.size() + 1;
            std::string wipeText = std::format("   {}", TR(MsgKey::L2ActionWipe));
            if (selected == wipeIdx) {
                wipeText[1] = '>';
                TuiEngine::PrintBounded(top + boxH - 5, left + 4, wipeText, innerW, "\x1b[1;37;41m");
            } else {
                TuiEngine::PrintBounded(top + boxH - 5, left + 4, wipeText, innerW, "\x1b[91m");
            }
        }

        // Bottom help card
        TuiEngine::MoveCursor(top + boxH - 4, left + 4);
        TuiEngine::PrintRaw(std::format("\x1b[90m{}\x1b[0m", std::string(innerW, '-')));

        std::string_view tip;
        std::string tipStyle = "\x1b[32m";
        if (selected == 0) {
            tip = TR(MsgKey::L2TipLogin);
            tipStyle = "\x1b[33m";
        } else if (selected <= accounts.size()) {
            const auto& curAcc = accounts[selected - 1];
            if (curAcc.isInvalid) {
                tip = curAcc.alias.empty() ? TR(MsgKey::L2TipInvalidAccountNoAlias) : TR(MsgKey::L2TipInvalidAccount);
                tipStyle = "\x1b[1;33m";
            } else {
                tip = curAcc.alias.empty() ? TR(MsgKey::L2TipCachedNoAlias) : TR(MsgKey::L2TipCached);
                tipStyle = "\x1b[32m";
            }
        } else {
            tip = TR(MsgKey::L2TipWipe);
            tipStyle = "\x1b[31m";
        }
        TuiEngine::PrintBounded(top + boxH - 3, left + 4, tip, innerW, tipStyle);

        if (selected >= 1 && selected <= accounts.size() && accounts[selected - 1].isInvalid) {
            TuiEngine::DrawFooter(TR(MsgKey::L2FooterInvalid), "\x1b[1;30;43m");
        } else {
            TuiEngine::DrawFooter(TR(MsgKey::L2Footer), "\x1b[1;30;47m");
        }
        TuiEngine::EndFrame();
    }

    void DrawLevel3QuickInputLine(int top, int left, int boxW, int boxH, std::string_view inputAppId) {
        const int innerW = boxW - 4;
        TuiEngine::MoveCursor(top + boxH - 2, left + 2);

        std::string prompt = std::format("  {}", TR(MsgKey::L3QuickInputPrompt));
        size_t promptW = TuiEngine::GetDisplayWidth(prompt);

        std::string boxContent = std::format(" [ {:<10} ]", std::string{inputAppId} + "_");
        size_t boxSlotW = 15; // " [ 123456_   ]"

        size_t usedW = promptW + boxSlotW;
        size_t availHint = (static_cast<size_t>(innerW) > usedW + 2) ? (static_cast<size_t>(innerW) - usedW - 2) : 0;

        std::string hint;
        if (availHint >= 16) {
            hint = "  " + TuiEngine::TruncateToWidth(TR(MsgKey::L3QuickInputHint), availHint - 2);
        }
        size_t hintW = TuiEngine::GetDisplayWidth(hint);

        size_t totalDrawn = usedW + hintW;
        size_t padSpaces = (static_cast<size_t>(innerW) > totalDrawn) ? (static_cast<size_t>(innerW) - totalDrawn) : 0;

        TuiEngine::PrintRaw(std::format("\x1b[1;37m{}\x1b[0m\x1b[1;30;47m{}\x1b[0m\x1b[90m{}\x1b[0m{}",
                                        prompt, boxContent, hint, std::string(padSpaces, ' ')));
    }

    void UpdateLevel3Input(std::string_view inputAppId) {
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        const int boxW = std::clamp(w - 4, 70, 120);
        const int maxBoxH = (std::min)(28, h - 2);
        const int boxH = std::clamp(h - 4, 14, maxBoxH);
        const int top = 2 + (h - 2 - boxH) / 2;
        const int left = (std::max)(1, (w - boxW) / 2);

        DrawLevel3QuickInputLine(top, left, boxW, boxH, inputAppId);
    }

    void RenderLevel3Tui(const GameListManager& gameMgr,
                         size_t selectedRow,
                         std::string_view inputAppId,
                         std::string_view accountName,
                         uint64_t steamId,
                         bool fullClear = false) {
        if (!TuiEngine::EnsureMinTerminalSize(76, 18)) {
            return;
        }
        TuiEngine::BeginFrame();
        int w = 80, h = 25;
        TuiEngine::GetScreenSize(w, h);
        if (fullClear) {
            TuiEngine::ClearScreen();
        }

        std::string title = TR_FMT(MsgKey::L3HeaderAccount, accountName, steamId);
        std::string tag = TR_FMT(MsgKey::L3HeaderPage,
                                 gameMgr.CurrentPage() + 1,
                                 gameMgr.TotalPages(),
                                 gameMgr.TotalGames());
        TuiEngine::DrawHeader(title, tag);

        const int boxW = std::clamp(w - 4, 70, 120);
        const int maxBoxH = (std::min)(28, h - 2);
        const int boxH = std::clamp(h - 4, 14, maxBoxH);
        const int top = 2 + (h - 2 - boxH) / 2;
        const int left = (std::max)(1, (w - boxW) / 2);

        TuiEngine::DrawBox(top, left, boxW, boxH, TR(MsgKey::L3BoxTitle).data(), /*clearInterior=*/false);

        const int innerW = boxW - 4;
        constexpr size_t idxColWidth = 4;
        constexpr size_t appIdColWidth = 10;
        constexpr size_t statusColWidth = 8;
        // 1 (space) + 4 (idx) + 3 (" │ ") + 10 (appId) + 3 (" │ ") + nameColWidth + 3 (" │ ") + 8 (status) + 1 (space) = 33 + nameColWidth = innerW
        const size_t nameColWidth = (innerW > 35) ? static_cast<size_t>(innerW - 33) : 18;

        // Table header
        TuiEngine::MoveCursor(top + 2, left + 2);
        std::string idxTitle = TuiEngine::Pad(TuiEngine::TruncateToWidth(std::string{TR(MsgKey::L3ColIndex)}, idxColWidth), idxColWidth);
        std::string appIdTitle = TuiEngine::Pad(TuiEngine::TruncateToWidth(std::string{TR(MsgKey::L3ColAppId)}, appIdColWidth), appIdColWidth);
        std::string nameTitle = TuiEngine::Pad(TuiEngine::TruncateToWidth(std::string{TR(MsgKey::L3ColName)}, nameColWidth), nameColWidth);
        std::string statusHdr = TuiEngine::Pad(TuiEngine::TruncateToWidth(std::string{TR(MsgKey::L3ColLicense)}, statusColWidth), statusColWidth, true);

        std::string hdr = std::format(" {} │ {} │ {} │ {} ", idxTitle, appIdTitle, nameTitle, statusHdr);
        hdr = TuiEngine::Pad(hdr, static_cast<size_t>(innerW));
        TuiEngine::PrintRaw(std::format("\x1b[1;37;44m{}\x1b[0m", hdr));

        TuiEngine::MoveCursor(top + 3, left + 2);
        TuiEngine::PrintRaw(std::format("\x1b[90m{}\x1b[0m", std::string(innerW, '-')));

        // Rows
        const auto pageGames = gameMgr.GetPageItems(gameMgr.CurrentPage());
        const size_t startIndex = gameMgr.CurrentPage() * gameMgr.PageSize();
        const size_t maxVisibleRows = static_cast<size_t>((std::max)(3, boxH - 7));

        for (size_t r = 0; r < maxVisibleRows; ++r) {
            TuiEngine::MoveCursor(top + 4 + static_cast<int>(r), left + 2);
            if (r < pageGames.size()) {
                const auto& g = pageGames[r];
                const size_t globalIdx = startIndex + r + 1;
                bool isShared = g.isShared;
                std::string displayName = g.name;
                for (char& ch : displayName) {
                    if (static_cast<unsigned char>(ch) < 32) ch = ' ';
                }
                std::string truncatedName = TuiEngine::TruncateToWidth(displayName, nameColWidth);
                std::string paddedName = TuiEngine::Pad(truncatedName, nameColWidth);

                std::string rawBadge = std::string{isShared ? TR(MsgKey::L3StatusShared) : TR(MsgKey::L3StatusOwned)};
                std::string statusBadge = TuiEngine::Pad(TuiEngine::TruncateToWidth(rawBadge, statusColWidth), statusColWidth, true);

                std::string idxStr = std::format("{:>4}", (globalIdx <= 9999) ? std::to_string(globalIdx) : "9999");
                std::string appIdStr = std::format("{:<10}", g.appId);

                std::string rowStr = std::format(" {} │ {} │ {} │ {} ",
                                                 idxStr, appIdStr, paddedName, statusBadge);
                rowStr = TuiEngine::Pad(rowStr, static_cast<size_t>(innerW));

                if (r == selectedRow) {
                    rowStr[0] = '>';
                    TuiEngine::PrintRaw(std::format("\x1b[30;107m{}\x1b[0m", rowStr));
                } else if (isShared) {
                    TuiEngine::PrintRaw(std::format("\x1b[96m{}\x1b[0m", rowStr));
                } else {
                    TuiEngine::PrintRaw(std::format("\x1b[37m{}\x1b[0m", rowStr));
                }
            } else {
                TuiEngine::PrintRaw(std::string(innerW, ' '));
            }
        }

        // Direct AppID input box line
        TuiEngine::MoveCursor(top + boxH - 3, left + 2);
        TuiEngine::PrintRaw(std::format("\x1b[90m{}\x1b[0m", std::string(boxW - 4, '-')));

        DrawLevel3QuickInputLine(top, left, boxW, boxH, inputAppId);

        TuiEngine::DrawFooter(TR(MsgKey::L3Footer));
        TuiEngine::EndFrame();
    }
} // namespace

int OnlineSession::RunInteractive() {
    RunAccountSelectionMenu();
    return 0;
}

void OnlineSession::RunAccountSelectionMenu() {
    size_t selected = 0; // Default to new account, preserved across redraws

    while (true) {
        auto accounts = TokenStorage::LoadAccounts();
        const size_t totalItems = 1 + accounts.size() + (accounts.empty() ? 0 : 1);
        if (selected >= totalItems) {
            selected = (totalItems > 0) ? (totalItems - 1) : 0;
        }

        bool menuActive = true;
        bool needFullClear = true;
        bool needRender = true;
        while (menuActive) {
            if (needRender) {
                RenderLevel2Tui(accounts, selected, needFullClear);
                needFullClear = false;
                needRender = false;
            }

            KeyEvent ev = TuiEngine::ReadKey();

            if (ev.code == KeyCode::Resize) {
                needFullClear = true;
                needRender = true;
                continue;
            }

            // Arrow keys
            if (ev.code == KeyCode::Up) {
                selected = (selected > 0) ? (selected - 1) : (totalItems - 1);
                needRender = true;
                continue;
            }
            if (ev.code == KeyCode::Down) {
                selected = (selected + 1 < totalItems) ? (selected + 1) : 0;
                needRender = true;
                continue;
            }

            // ESC or q -> Back to Level 1
            if (ev.code == KeyCode::Escape ||
                (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q'))) {
                TuiEngine::ClearScreen();
                return;
            }

            // 'r' or 'R' -> Edit alias for selected account
            if (ev.code == KeyCode::Char && (ev.ch == 'r' || ev.ch == 'R')) {
                if (TuiEngine::HasInputPending()) continue;
                if (selected >= 1 && selected <= accounts.size()) {
                    const auto& targetAcc = accounts[selected - 1];
                    TuiEngine::ClearScreen();
                    bool hasExistingAlias = !targetAcc.alias.empty();
                    std::string promptStr = hasExistingAlias
                        ? TR_FMT(MsgKey::EditAliasPrompt, targetAcc.accountName)
                        : TR_FMT(MsgKey::SetAliasPrompt, targetAcc.accountName);
                    auto newAlias = TuiEngine::PromptInputModal(
                        hasExistingAlias ? TR(MsgKey::EditAliasTitle) : TR(MsgKey::SetAliasTitle),
                        promptStr,
                        targetAcc.alias);
                    needFullClear = true;
                    needRender = true;
                    if (newAlias) {
                        if (targetAcc.steamId != 0) {
                            TokenStorage::UpdateAccountAlias(targetAcc.steamId, *newAlias);
                        } else {
                            TokenStorage::UpdateAccountAlias(targetAcc.accountName, *newAlias);
                        }
                        break; // Reload accounts & redraw menu with cursor preserved on 'selected'
                    }
                }
                continue;
            }

            // 'd' or Delete -> Delete selected account
            if (ev.code == KeyCode::Delete ||
                (ev.code == KeyCode::Char && (ev.ch == 'd' || ev.ch == 'D'))) {
                if (TuiEngine::HasInputPending()) continue;
                if (selected >= 1 && selected <= accounts.size()) {
                    const auto& targetAcc = accounts[selected - 1];
                    TuiEngine::ClearScreen();
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        TR(MsgKey::DelAccountTitle).data(),
                        TR_FMT(MsgKey::DelAccountMsg, targetAcc.accountName),
                        TR(MsgKey::DelAccountDetail).data(),
                        false);
                    needFullClear = true;
                    needRender = true;
                    if (confirmed) {
                        if (targetAcc.steamId != 0) {
                            TokenStorage::DeleteAccount(targetAcc.steamId);
                        } else {
                            TokenStorage::DeleteAccount(targetAcc.accountName);
                        }
                        if (selected > accounts.size() - 1) {
                            selected = (accounts.size() > 1) ? (accounts.size() - 1) : 0;
                        }
                        break; // Reload accounts & redraw menu
                    }
                }
                continue;
            }

            // Enter key
            if (ev.code == KeyCode::Enter) {
                if (selected == 0) {
                    // Log in to a new account
                    TuiEngine::ClearScreen();
                    auto accName = TuiEngine::PromptInputModal(
                        TR(MsgKey::LoginPromptTitle).data(),
                        TR(MsgKey::LoginPromptAccount).data(),
                        "");
                    if (!accName || accName->empty()) {
                        needFullClear = true;
                        needRender = true;
                        continue;
                    }
                    TuiEngine::ClearScreen();
                    auto pwdStr = TuiEngine::PromptInputModal(
                        TR(MsgKey::LoginPromptTitle).data(),
                        TR(MsgKey::LoginPromptPassword).data(),
                        "",
                        true);
                    if (!pwdStr || pwdStr->empty()) {
                        needFullClear = true;
                        needRender = true;
                        continue;
                    }

                    SecureString secPwd(*pwdStr);
                    SecureWipeString(*pwdStr);

                    TuiEngine::ClearScreen();
                    SteamAuthService authService;
                    auto loginRes = authService.LoginWithCredentials(*accName, secPwd);
                    secPwd.Clear();

                    if (loginRes.success) {
                        TuiEngine::ClearScreen();
                        RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.refreshToken, loginRes.accessToken);
                    } else if (loginRes.cancelled) {
                        needFullClear = true;
                        needRender = true;
                        continue;
                    } else {
                        TuiEngine::ClearScreen();
                        TuiEngine::ShowMessageModal(
                            TR(MsgKey::LoginFailedTitle).data(),
                            loginRes.errorMessage.empty() ? TR(MsgKey::LoginFailedDefaultMsg).data() : loginRes.errorMessage,
                            TR(MsgKey::LoginFailedDetail).data());
                    }
                    needFullClear = true;
                    break;
                } else if (selected >= 1 && selected <= accounts.size()) {
                    // Cached account
                    const auto& acc = accounts[selected - 1];

                    // If credentials are known to be invalid, prompt directly for re-login
                    if (acc.isInvalid) {
                        TuiEngine::ClearScreen();
                        bool relogin = TuiEngine::ShowConfirmModal(
                            TR(MsgKey::TokenExpiredTitle).data(),
                            TR_FMT(MsgKey::TokenExpiredMsg, acc.accountName),
                            TR(MsgKey::TokenExpiredPrompt).data(),
                            true);
                        if (relogin) {
                            TuiEngine::ClearScreen();
                            auto pwdStr = TuiEngine::PromptInputModal(
                                TR(MsgKey::ReloginTitle).data(),
                                TR_FMT(MsgKey::ReloginPrompt, acc.accountName),
                                "",
                                true);
                            if (pwdStr && !pwdStr->empty()) {
                                SecureString secPwd(*pwdStr);
                                SecureWipeString(*pwdStr);
                                TuiEngine::ClearScreen();
                                SteamAuthService authService;
                                auto loginRes = authService.LoginWithCredentials(acc.accountName, secPwd);
                                secPwd.Clear();
                                if (loginRes.success) {
                                    TokenStorage::MarkAccountInvalid(loginRes.steamId, false);
                                    TuiEngine::ClearScreen();
                                    RunInSessionExtraction(loginRes.accountName, loginRes.steamId, loginRes.refreshToken, loginRes.accessToken);
                                } else if (loginRes.cancelled) {
                                    needFullClear = true;
                                    needRender = true;
                                    continue;
                                } else {
                                    TuiEngine::ClearScreen();
                                    TuiEngine::ShowMessageModal(TR(MsgKey::LoginFailedTitle).data(), loginRes.errorMessage);
                                }
                            }
                        }
                        needFullClear = true;
                        break;
                    }

                    TuiEngine::ClearScreen();
                    RunInSessionExtraction(acc.accountName, acc.steamId, acc.refreshToken, acc.accessToken);
                    needFullClear = true;
                    break;
                } else if (!accounts.empty() && selected == accounts.size() + 1) {
                    TuiEngine::ClearScreen();
                    bool confirmed = TuiEngine::ShowConfirmModal(
                        TR(MsgKey::WipeAllTitle).data(),
                        TR(MsgKey::WipeAllMsg).data(),
                        TR(MsgKey::WipeAllDetail).data(),
                        false);
                    needFullClear = true;
                    needRender = true;
                    if (confirmed) {
                        TokenStorage::WipeAll();
                        break;
                    }
                }
            }
        }
    }
}

void OnlineSession::RunInSessionExtraction(
    const std::string& accountName,
    uint64_t steamId,
    const std::string& refreshToken,
    const std::string& accessToken) {

    SteamAuthService authService;
    SteamCmClient cmClient;

    int w = 80, h = 25;
    TuiEngine::GetScreenSize(w, h);
    const int modalW = std::clamp(w - 12, 64, 84);
    const int modalH = 6;
    const int top = (h - modalH) / 2, left = (w - modalW) / 2;
    const size_t innerW = static_cast<size_t>(modalW - 8);

    TuiEngine::ClearScreen();
    TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ConnectingTitle).data());
    TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::ConnectingGatewayMsg), innerW, "\x1b[1;36m");
    std::cout.flush();

    std::string curAccountName = accountName;
    uint64_t curSteamId = steamId != 0 ? steamId : GetJwtSteamId(refreshToken);
    std::string curRefreshToken = refreshToken;
    std::string curAccessToken = accessToken;

    struct TokenWipeGuard {
        std::string& refTok;
        std::string& accTok;
        TokenWipeGuard(std::string& r, std::string& a) noexcept : refTok(r), accTok(a) {}
        ~TokenWipeGuard() noexcept {
            SecureWipeString(refTok);
            SecureWipeString(accTok);
        }
        TokenWipeGuard(const TokenWipeGuard&) = delete;
        TokenWipeGuard& operator=(const TokenWipeGuard&) = delete;
    } tokenGuard{curRefreshToken, curAccessToken};

    if (!cmClient.ConnectAndLogon(curSteamId, curRefreshToken, curAccessToken)) {
        bool logonOk = false;
        int32_t lastE = cmClient.GetLastLogonEResult();
        if (IsCredentialInvalidEResult(lastE) || curRefreshToken.empty()) {
            if (curSteamId != 0) {
                TokenStorage::MarkAccountInvalid(curSteamId, true);
            } else {
                TokenStorage::MarkAccountInvalid(curAccountName, true);
            }
            TuiEngine::ClearScreen();
            bool relogin = TuiEngine::ShowConfirmModal(
                TR(MsgKey::TokenExpiredTitle).data(),
                TR_FMT(MsgKey::TokenExpiredMsg, curAccountName),
                TR(MsgKey::TokenExpiredPrompt).data(),
                true);
            if (!relogin) {
                return;
            }
            TuiEngine::ClearScreen();
            auto pwdStr = TuiEngine::PromptInputModal(
                TR(MsgKey::ReloginTitle).data(),
                TR_FMT(MsgKey::ReloginPrompt, curAccountName),
                "",
                true);
            if (!pwdStr || pwdStr->empty()) {
                return;
            }
            SecureString secPwd(*pwdStr);
            SecureWipeString(*pwdStr);
            TuiEngine::ClearScreen();
            auto loginRes = authService.LoginWithCredentials(curAccountName, secPwd);
            secPwd.Clear();
            if (loginRes.success) {
                TokenStorage::MarkAccountInvalid(loginRes.steamId, false);
                curSteamId = loginRes.steamId;
                SecureWipeString(curRefreshToken);
                SecureWipeString(curAccessToken);
                curRefreshToken = loginRes.refreshToken;
                curAccessToken = loginRes.accessToken;
                TuiEngine::ClearScreen();
                TuiEngine::DrawBox(top, left, modalW, modalH, TR(MsgKey::ConnectingTitle).data());
                TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::ConnectingGatewayMsg), innerW, "\x1b[1;36m");
                std::cout.flush();
                if (cmClient.ConnectAndLogon(curSteamId, curRefreshToken, curAccessToken)) {
                    logonOk = true;
                }
            } else if (loginRes.cancelled) {
                return;
            } else {
                TuiEngine::ClearScreen();
                TuiEngine::ShowMessageModal(
                    TR(MsgKey::LoginFailedTitle).data(),
                    loginRes.errorMessage.empty() ? TR(MsgKey::LoginFailedDefaultMsg).data() : loginRes.errorMessage,
                    TR(MsgKey::LoginFailedDetail).data());
                return;
            }
        }

        if (!logonOk) {
            TuiEngine::ClearScreen();
            TuiEngine::ShowMessageModal(TR(MsgKey::ConnectingFailedTitle).data(),
                                        TR(MsgKey::ConnectingFailedMsg).data(),
                                        TR(MsgKey::ConnectingFailedDetail).data());
            return;
        }
    }

    if (curSteamId == 0 && cmClient.GetSteamId() != 0) {
        curSteamId = cmClient.GetSteamId();
    }

    // Ensure access token (and rolling refresh token) is fresh over authenticated CM session before WebAPI calls
    int64_t exp = GetJwtExpiration(curAccessToken);
    if ((curAccessToken.empty() || (exp > 0 && exp <= std::time(nullptr) + 300)) && !curRefreshToken.empty()) {
        TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::ConnectingRefreshingMsg), innerW, "\x1b[1;33m");
        std::cout.flush();
        std::string newRefreshToken;
        // Dual-Track Token Renewal: Track 1 (Native CM WebSocket RPC, immune to WebAPI 429) -> Track 2 (WebAPI HTTPS fallback)
        auto accessOpt = cmClient.RefreshAccessTokenViaCm(&newRefreshToken);
        if (!accessOpt || accessOpt->empty()) {
            accessOpt = authService.RefreshAccessToken(curSteamId, curRefreshToken, &newRefreshToken);
        }
        if (accessOpt && !accessOpt->empty()) {
            SecureWipeString(curAccessToken);
            curAccessToken = std::move(*accessOpt);
            SecureWipeString(*accessOpt);
            cmClient.SetAccessToken(curAccessToken);
        }
        if (!newRefreshToken.empty()) {
            SecureWipeString(curRefreshToken);
            curRefreshToken = std::move(newRefreshToken);
            SecureWipeString(newRefreshToken);
            cmClient.SetRefreshToken(curRefreshToken);
        }
    }

    {
        CachedAccount updated;
        updated.accountName = curAccountName;
        updated.steamId = curSteamId;
        updated.refreshToken = curRefreshToken;
        updated.accessToken = curAccessToken;
        updated.lastLoginTime = std::time(nullptr);
        updated.isInvalid = false;
        TokenStorage::UpsertAccount(updated);
    }

    TuiEngine::PrintBounded(top + 2, left + 4, TR(MsgKey::SyncingGamesMsg), innerW, "\x1b[1;32m");
    std::cout.flush();

    auto games = authService.FetchOwnedGames(curSteamId, curAccessToken, &cmClient);
    if (games.empty() && !curRefreshToken.empty()) {
        std::string newRefreshToken;
        // Dual-Track Token Renewal: Track 1 (Native CM WebSocket RPC, immune to WebAPI 429) -> Track 2 (WebAPI HTTPS fallback)
        auto accessOpt = cmClient.RefreshAccessTokenViaCm(&newRefreshToken);
        if (!accessOpt || accessOpt->empty()) {
            accessOpt = authService.RefreshAccessToken(curSteamId, curRefreshToken, &newRefreshToken);
        }
        bool refreshRotated = false;
        if (!newRefreshToken.empty()) {
            SecureWipeString(curRefreshToken);
            curRefreshToken = std::move(newRefreshToken);
            SecureWipeString(newRefreshToken);
            cmClient.SetRefreshToken(curRefreshToken);
            refreshRotated = true;
        }
        bool accessRefreshed = false;
        if (accessOpt && !accessOpt->empty()) {
            SecureWipeString(curAccessToken);
            curAccessToken = std::move(*accessOpt);
            SecureWipeString(*accessOpt);
            cmClient.SetAccessToken(curAccessToken);
            accessRefreshed = true;
        }
        if (accessRefreshed || refreshRotated) {
            CachedAccount updated;
            updated.accountName = curAccountName;
            updated.steamId = curSteamId;
            updated.refreshToken = curRefreshToken;
            updated.accessToken = curAccessToken;
            updated.lastLoginTime = std::time(nullptr);
            updated.isInvalid = false;
            TokenStorage::UpsertAccount(updated);
        }
        if (accessRefreshed) {
            games = authService.FetchOwnedGames(curSteamId, curAccessToken, &cmClient);
        }
    }

    if (games.empty()) {
        TuiEngine::ClearScreen();
        TuiEngine::ShowMessageModal(TR(MsgKey::NoGamesFoundTitle).data(),
                                    TR(MsgKey::NoGamesFoundMsg).data(),
                                    TR(MsgKey::NoGamesFoundDetail).data());
        return;
    }

    GameListManager gameMgr(std::move(games), 20);
    size_t selectedRow = 0;
    std::string inputAppId;
    bool needFullClear = true;
    bool needRender = true;

    TuiEngine::FlushInputBuffer();

    while (true) {
        if (needRender) {
            RenderLevel3Tui(gameMgr, selectedRow, inputAppId, curAccountName, curSteamId, needFullClear);
            needFullClear = false;
            needRender = false;
        }

        KeyEvent ev = TuiEngine::ReadKey();

        if (ev.code == KeyCode::Resize) {
            needFullClear = true;
            needRender = true;
            continue;
        }

        // Up / Down
        if (ev.code == KeyCode::Up) {
            const auto pageItems = gameMgr.GetPageItems(gameMgr.CurrentPage());
            if (!pageItems.empty()) {
                selectedRow = (selectedRow > 0) ? (selectedRow - 1) : (pageItems.size() - 1);
                needRender = true;
            }
            continue;
        }
        if (ev.code == KeyCode::Down) {
            const auto pageItems = gameMgr.GetPageItems(gameMgr.CurrentPage());
            if (!pageItems.empty()) {
                selectedRow = (selectedRow + 1 < pageItems.size()) ? (selectedRow + 1) : 0;
                needRender = true;
            }
            continue;
        }

        // Left / 'b' / 'B' -> Previous Page
        if (ev.code == KeyCode::Left ||
            (ev.code == KeyCode::Char && (ev.ch == 'b' || ev.ch == 'B'))) {
            if (gameMgr.PrevPage()) {
                selectedRow = 0;
                needRender = true;
            }
            continue;
        }

        // Right / 'n' / 'N' -> Next Page
        if (ev.code == KeyCode::Right ||
            (ev.code == KeyCode::Char && (ev.ch == 'n' || ev.ch == 'N'))) {
            if (gameMgr.NextPage()) {
                selectedRow = 0;
                needRender = true;
            }
            continue;
        }

        // 'l' / 'L' -> Export CSV
        if (ev.code == KeyCode::Char && (ev.ch == 'l' || ev.ch == 'L')) {
            if (TuiEngine::HasInputPending()) continue;
            TuiEngine::ClearScreen();
            bool confirmed = TuiEngine::ShowConfirmModal(
                TR(MsgKey::CsvExportTitle).data(),
                TR_FMT(MsgKey::CsvExportMsg, gameMgr.TotalGames()),
                TR_FMT(MsgKey::CsvExportDetail, curAccountName),
                false);
            needFullClear = true;
            needRender = true;
            if (confirmed) {
                std::string outPath;
                if (gameMgr.ExportCsv(curAccountName, &outPath)) {
                    TuiEngine::ClearScreen();
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::CsvExportSuccessTitle).data(),
                        TR_FMT(MsgKey::CsvExportSuccessMsg, outPath),
                        TR(MsgKey::CsvExportSuccessDetail).data());
                } else {
                    TuiEngine::ClearScreen();
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::CsvExportFailedTitle).data(),
                        TR_FMT(MsgKey::CsvExportFailedMsg, outPath.empty() ? curAccountName : outPath),
                        TR(MsgKey::CsvExportFailedDetail).data());
                }
            }
            continue;
        }

        // 'a' / 'A' -> Batch extract all
        if (ev.code == KeyCode::Char && (ev.ch == 'a' || ev.ch == 'A')) {
            if (TuiEngine::HasInputPending()) continue;
            TuiEngine::ClearScreen();
            bool confirmed = TuiEngine::ShowConfirmModal(
                TR(MsgKey::BatchConfirmTitle).data(),
                TR_FMT(MsgKey::BatchConfirmMsg, gameMgr.TotalGames()),
                TR(MsgKey::BatchConfirmDetail).data(),
                false);
            needFullClear = true;
            needRender = true;
            if (confirmed) {
                int scrW = 80, scrH = 25;
                TuiEngine::GetScreenSize(scrW, scrH);
                const int bModalW = std::clamp(scrW - 12, 60, 90);
                const int bModalH = 8;
                const int bTop = (scrH - bModalH) / 2;
                const int bLeft = (scrW - bModalW) / 2;

                TuiEngine::ClearScreen();
                TuiEngine::DrawBox(bTop, bLeft, bModalW, bModalH, TR(MsgKey::BatchExtractTitle).data());

                size_t progress = 0;
                size_t succeeded = 0;
                const size_t total = gameMgr.TotalGames();
                bool aborted = false;

                for (const auto& game : gameMgr.Games()) {
                    if (TuiEngine::HasInputPending()) {
                        KeyEvent checkKey = TuiEngine::ReadKey();
                        if (checkKey.code == KeyCode::Escape) {
                            TuiEngine::FlushInputBuffer();
                            aborted = true;
                            break;
                        }
                    }
                    ++progress;
                    TuiEngine::DrawProgressBar(bTop + 3, bLeft + 4, bModalW - 8, progress, total, game.name);

                    auto creds = cmClient.ExtractFullCredentials(game.appId);
                    if (WriteOutputs(game.appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                     creds.depotKeys, creds.dlcs, creds.appTokens, true)) {
                        ++succeeded;
                    }

                    for (int s = 0; s < 4; ++s) {
                        auto keyOpt = TuiEngine::PollKey(50);
                        if (keyOpt && keyOpt->code == KeyCode::Escape) {
                            TuiEngine::FlushInputBuffer();
                            aborted = true;
                            break;
                        }
                    }
                    if (aborted) break;
                }

                TuiEngine::ClearScreen();
                if (aborted) {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::BatchAbortedTitle).data(),
                        TR_FMT(MsgKey::BatchAbortedMsg, succeeded, total),
                        TR(MsgKey::BatchAbortedDetail).data());
                } else {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::BatchSuccessTitle).data(),
                        TR_FMT(MsgKey::BatchSuccessMsg, succeeded, total),
                        TR(MsgKey::BatchSuccessDetail).data());
                }
                needFullClear = true;
                needRender = true;
            }
            continue;
        }

        // Digits 0-9 -> Add to AppID input box
        if (ev.code == KeyCode::Char && ev.ch >= '0' && ev.ch <= '9') {
            if (inputAppId.size() < 10) {
                inputAppId.push_back(ev.ch);
                UpdateLevel3Input(inputAppId);
            }
            continue;
        }

        // Backspace
        if (ev.code == KeyCode::Backspace) {
            if (!inputAppId.empty()) {
                inputAppId.pop_back();
                UpdateLevel3Input(inputAppId);
            }
            continue;
        }

        // Enter -> Extract target AppID (normal mode, auto-detect Denuvo)
        // 'e' / 'E' -> Extract target AppID with forced encrypted ticket (--force-eticket)
        const bool isEnter = (ev.code == KeyCode::Enter);
        const bool isForceE = (ev.code == KeyCode::Char && (ev.ch == 'e' || ev.ch == 'E'));
        if ((isEnter || isForceE) && !TuiEngine::HasInputPending()) {
            const bool forceEticket = isForceE;
            uint32_t targetAppId = 0;
            std::string gameName;

            if (!inputAppId.empty()) {
                auto parsed = ParseAppId(inputAppId);
                if (parsed) {
                    targetAppId = *parsed;
                    gameName = std::format("AppID {}", targetAppId);
                }
                inputAppId.clear();
                UpdateLevel3Input(inputAppId);
            } else {
                const auto pageItems = gameMgr.GetPageItems(gameMgr.CurrentPage());
                if (selectedRow < pageItems.size()) {
                    targetAppId = pageItems[selectedRow].appId;
                    gameName = pageItems[selectedRow].name;
                }
            }

            if (targetAppId > 0) {
                int scrW = 80, scrH = 25;
                TuiEngine::GetScreenSize(scrW, scrH);
                const int eModalW = std::clamp(scrW - 12, 64, 88);
                const int eModalH = 8;
                const int eTop = (scrH - eModalH) / 2, eLeft = (scrW - eModalW) / 2;
                const size_t eInnerW = static_cast<size_t>(eModalW - 8);

                TuiEngine::ClearScreen();
                TuiEngine::DrawBox(eTop, eLeft, eModalW, eModalH, TR(MsgKey::ExtractingOnlineTitle).data());

                std::string lineTarget = TR_FMT(MsgKey::ExtractingOnlineMsg2, gameName, targetAppId);
                TuiEngine::PrintBounded(eTop + 2, eLeft + 4, lineTarget, eInnerW, "\x1b[36m");
                TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingOnlineMsg1), eInnerW, "\x1b[1;33m");
                std::cout.flush();

                auto progressCallback = [&](std::string_view stage, size_t current, size_t total, uint32_t depotId) {
                    if (stage == "tickets") {
                        TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingStageTickets), eInnerW, "\x1b[1;33m");
                        std::cout.flush();
                    } else if (stage == "meta") {
                        TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingStageMeta), eInnerW, "\x1b[1;33m");
                        std::cout.flush();
                    } else if (stage == "keys") {
                        TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingStageKeys), eInnerW, "\x1b[1;33m");
                        std::cout.flush();
                    } else if (stage == "tokens") {
                        TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingStageTokens), eInnerW, "\x1b[1;33m");
                        std::cout.flush();
                    } else if (stage == "manifests_start") {
                        TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingStageManifests), eInnerW, "\x1b[1;33m");
                        std::cout.flush();
                    } else if (stage == "manifest" && total > 0) {
                        std::string progStr = TR_FMT(MsgKey::ExtractingManifestProgress, current, total, depotId);
                        TuiEngine::PrintBounded(eTop + 4, eLeft + 4, progStr, eInnerW, "\x1b[1;33m");
                        TuiEngine::DrawProgressBar(eTop + 5, eLeft + 4, static_cast<int>(eInnerW), current, total, std::format("Depot {}", depotId));
                        std::cout.flush();
                    } else if (stage == "done") {
                        TuiEngine::PrintBounded(eTop + 3, eLeft + 4, TR(MsgKey::ExtractingStageComplete), eInnerW, "\x1b[1;32m");
                        if (total > 0) {
                            TuiEngine::PrintBounded(eTop + 4, eLeft + 4, TR(MsgKey::ExtractingManifestsDone), eInnerW, "\x1b[1;32m");
                            TuiEngine::DrawProgressBar(eTop + 5, eLeft + 4, static_cast<int>(eInnerW), total, total, TR(MsgKey::ProgressDone));
                        }
                        std::cout.flush();
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                };

                auto creds = cmClient.ExtractFullCredentials(targetAppId, forceEticket, progressCallback);
                bool ok = WriteOutputs(targetAppId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                                       creds.depotKeys, creds.dlcs, creds.appTokens, true);
                TuiEngine::ClearScreen();
                if (ok) {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::ExtractSuccessTitle).data(),
                        TR_FMT(MsgKey::ExtractSuccessMsg, targetAppId),
                        TR_FMT(MsgKey::ExtractSuccessDetail, targetAppId));
                } else {
                    TuiEngine::ShowMessageModal(
                        TR(MsgKey::ExtractWarnTitle).data(),
                        TR_FMT(MsgKey::ExtractWarnMsg, targetAppId));
                }
                needFullClear = true;
                needRender = true;
            }
            continue;
        }

        // ESC -> Clear quick extract input if non-empty; otherwise return to Level 2
        if (ev.code == KeyCode::Escape) {
            if (!inputAppId.empty()) {
                inputAppId.clear();
                UpdateLevel3Input(inputAppId);
                continue;
            }
            TuiEngine::ClearScreen();
            cmClient.Disconnect();
            return;
        }

        // 'q' / 'Q' -> Return to Level 2
        if (ev.code == KeyCode::Char && (ev.ch == 'q' || ev.ch == 'Q')) {
            TuiEngine::ClearScreen();
            cmClient.Disconnect();
            return;
        }
    }
}

int OnlineSession::RunSilent(uint32_t appId, const std::string& accountName, bool forceEticket) {
    if (appId == 0) {
        std::cerr << TR(MsgKey::InvalidAppId) << "\n";
        return 1;
    }

    auto accounts = TokenStorage::LoadAccounts();
    if (accounts.empty()) {
        std::cerr << TR(MsgKey::ErrNoSavedAccounts) << "\n";
        std::cerr << TR(MsgKey::ErrNoSavedAccountsTip) << "\n";
        return 1;
    }

    std::vector<const CachedAccount*> candidates;
    if (!accountName.empty()) {
        for (const auto& acc : accounts) {
            if (acc.accountName == accountName || (!acc.alias.empty() && acc.alias == accountName)) {
                candidates.push_back(&acc);
                break;
            }
        }
        if (candidates.empty()) {
            std::cerr << TR_FMT(MsgKey::ErrAccountNotFound, MaskAccount(accountName)) << "\n";
            std::cerr << TR(MsgKey::ErrAccountListTip) << "\n";
            for (const auto& acc : accounts) {
                if (!acc.alias.empty()) {
                    std::cerr << "  - " << MaskAccount(acc.accountName) << " [" << acc.alias << "]\n";
                } else {
                    std::cerr << "  - " << MaskAccount(acc.accountName) << "\n";
                }
            }
            return 1;
        }
    } else {
        for (const auto& acc : accounts) candidates.push_back(&acc);
        // Sort: prioritize valid accounts (!isInvalid), then lastLoginTime descending
        std::sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
            if (a->isInvalid != b->isInvalid) {
                return !a->isInvalid && b->isInvalid;
            }
            return a->lastLoginTime > b->lastLoginTime;
        });
    }

    SteamAuthService authService;
    SteamCmClient cmClient;
    const CachedAccount* activeAcc = nullptr;
    ExtractedAppCredentials creds;
    std::optional<ExtractedAppCredentials> fallbackCreds;
    const CachedAccount* fallbackAcc = nullptr;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto* curAcc = candidates[i];
        if (candidates.size() > 1 && accountName.empty()) {
            std::cout << TR_FMT(MsgKey::CliExtractAttempting,
                                MaskAccount(curAcc->accountName), i + 1, candidates.size()) << "\n";
        } else {
            std::cout << TR_FMT(MsgKey::CliExtractUsing,
                                MaskAccount(curAcc->accountName), appId) << "\n";
        }

        uint64_t curSteamId = curAcc->steamId != 0 ? curAcc->steamId : GetJwtSteamId(curAcc->refreshToken);
        std::string activeRefreshToken = curAcc->refreshToken;
        std::string activeToken = curAcc->accessToken;
        struct SilentTokenWipeGuard {
            std::string& refTok;
            std::string& accTok;
            SilentTokenWipeGuard(std::string& r, std::string& a) noexcept : refTok(r), accTok(a) {}
            ~SilentTokenWipeGuard() noexcept {
                SecureWipeString(refTok);
                SecureWipeString(accTok);
            }
            SilentTokenWipeGuard(const SilentTokenWipeGuard&) = delete;
            SilentTokenWipeGuard& operator=(const SilentTokenWipeGuard&) = delete;
        } silentGuard{activeRefreshToken, activeToken};

        bool logonOk = cmClient.ConnectAndLogon(curSteamId, activeRefreshToken, activeToken);

        if (!logonOk) {
            int32_t lastE = cmClient.GetLastLogonEResult();
            if (IsCredentialInvalidEResult(lastE) || activeRefreshToken.empty()) {
                if (curSteamId != 0) {
                    TokenStorage::MarkAccountInvalid(curSteamId, true);
                } else {
                    TokenStorage::MarkAccountInvalid(curAcc->accountName, true);
                }
            }
        }

        if (logonOk) {
            if (curSteamId == 0 && cmClient.GetSteamId() != 0) {
                curSteamId = cmClient.GetSteamId();
            }
            std::cout << TR(MsgKey::CliExtractLoginSuccess) << "\n";

            int64_t exp = GetJwtExpiration(activeToken);
            if ((activeToken.empty() || (exp > 0 && exp <= std::time(nullptr) + 300)) && !activeRefreshToken.empty()) {
                std::string newRefreshToken;
                // Dual-Track Token Renewal: Track 1 (Native CM WebSocket RPC, immune to WebAPI 429) -> Track 2 (WebAPI HTTPS fallback)
                auto accessOpt = cmClient.RefreshAccessTokenViaCm(&newRefreshToken);
                if (!accessOpt || accessOpt->empty()) {
                    accessOpt = authService.RefreshAccessToken(curSteamId, activeRefreshToken, &newRefreshToken);
                }
                if (accessOpt && !accessOpt->empty()) {
                    SecureWipeString(activeToken);
                    activeToken = std::move(*accessOpt);
                    SecureWipeString(*accessOpt);
                    cmClient.SetAccessToken(activeToken);
                }
                if (!newRefreshToken.empty()) {
                    SecureWipeString(activeRefreshToken);
                    activeRefreshToken = std::move(newRefreshToken);
                    SecureWipeString(newRefreshToken);
                    cmClient.SetRefreshToken(activeRefreshToken);
                }
            }

            {
                CachedAccount updated = *curAcc;
                updated.steamId = curSteamId;
                updated.refreshToken = activeRefreshToken;
                updated.accessToken = activeToken;
                updated.lastLoginTime = std::time(nullptr);
                updated.isInvalid = false;
                TokenStorage::UpsertAccount(updated);
            }

            auto curCreds = cmClient.ExtractFullCredentials(appId, forceEticket);

            const bool hasTicket = curCreds.appOwnershipTicket.has_value() || curCreds.encryptedAppTicket.has_value();
            const bool hasKeys = std::any_of(curCreds.depotKeys.begin(), curCreds.depotKeys.end(),
                                             [](const DepotKeyInfo& k) { return !k.hexKey.empty(); });

            if (hasTicket || hasKeys || !accountName.empty() || candidates.size() == 1) {
                // 当前账号拥有正版授权，或者为用户显式指定/唯一账号
                activeAcc = curAcc;
                creds = std::move(curCreds);
                break;
            }

            // 当前账号未拥有目标游戏正版授权（无票据且无密钥）
            if (!fallbackCreds.has_value()) {
                fallbackCreds = std::move(curCreds);
                fallbackAcc = curAcc;
            }

            if (i + 1 < candidates.size()) {
                std::cout << TR_FMT(MsgKey::CliExtractNoLicenseSwitch,
                                    MaskAccount(curAcc->accountName), appId) << "\n";
                cmClient.Disconnect();
            }
            continue;
        }

        if (accountName.empty() && i + 1 < candidates.size()) {
            std::cout << TR_FMT(MsgKey::CliExtractLoginFailedSwitch, MaskAccount(curAcc->accountName)) << "\n";
        }
    }

    if (!activeAcc) {
        if (fallbackAcc && fallbackCreds.has_value()) {
            activeAcc = fallbackAcc;
            creds = std::move(*fallbackCreds);
            LOG_INFO("OnlineSession", MsgKey::CliExtractFallbackBase, appId);
        } else {
            std::cerr << TR(MsgKey::CliExtractFailedAll) << "\n";
            std::cerr << TR(MsgKey::CliExtractFailedAllTip) << "\n";
            return 1;
        }
    }

    bool ok = WriteOutputs(appId, creds.appOwnershipTicket, creds.encryptedAppTicket,
                           creds.depotKeys, creds.dlcs, creds.appTokens, true);
    if (ok) {
        std::cout << TR_FMT(MsgKey::CliExtractSuccess, appId, appId) << "\n";
        return 0;
    } else {
        std::cerr << TR_FMT(MsgKey::CliExtractWarn, appId) << "\n";
        return 1;
    }
}

} // namespace OST::ExtractTickets
