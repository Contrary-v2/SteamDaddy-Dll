#include "TuiEngine.h"
#include "Crypto.h"
#include "I18n.h"
#include "Utils.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>

namespace OST::ExtractTickets {

#pragma comment(lib, "user32.lib")

namespace {
    bool g_tuiActive = false;
    bool s_inFrame = false;
    std::string s_frameBuffer;
    bool s_pasteAllowed = false;

    void OutputToTerminal(std::string_view sv) {
        if (s_inFrame) {
            s_frameBuffer.append(sv);
        } else {
            std::cout.write(sv.data(), sv.size());
        }
    }
}

bool TuiEngine::IsActive() noexcept {
    return g_tuiActive;
}

void TuiEngine::SetActive(bool active) noexcept {
    g_tuiActive = active;
}

void TuiEngine::BeginFrame() {
    s_inFrame = true;
    s_frameBuffer.clear();
    if (s_frameBuffer.capacity() < 8192) {
        s_frameBuffer.reserve(8192);
    }
    s_frameBuffer.append("\x1b[H");
}

void TuiEngine::EndFrame() {
    if (!s_inFrame) return;
    s_inFrame = false;
    if (!s_frameBuffer.empty()) {
        std::cout.write(s_frameBuffer.data(), s_frameBuffer.size());
        std::cout.flush();
        s_frameBuffer.clear();
    }
}

void TuiEngine::PrintRaw(std::string_view str) {
    OutputToTerminal(str);
}

void TuiEngine::SetPasteAllowed(bool allowed) noexcept {
    s_pasteAllowed = allowed;
}

bool TuiEngine::IsPasteAllowed() noexcept {
    return s_pasteAllowed;
}

#if defined(_WIN32)
namespace {
    BOOL WINAPI ConsoleCtrlHandler(DWORD fdwCtrlType) {
        switch (fdwCtrlType) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
            case CTRL_CLOSE_EVENT:
                if (g_tuiActive) {
                    TuiEngine::ShowCursor(true);
                    TuiEngine::ExitAlternateScreen();
                }
                return FALSE;
            default:
                return FALSE;
        }
    }
} // namespace
#endif

void TuiEngine::EnableVirtualTerminal() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
        SetConsoleOutputCP(CP_UTF8);
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
        DWORD dwInMode = 0;
        if (GetConsoleMode(hIn, &dwInMode)) {
            // Enable ENABLE_MOUSE_INPUT with ENABLE_WINDOW_INPUT and ENABLE_EXTENDED_FLAGS.
            // Keep ENABLE_VIRTUAL_TERMINAL_INPUT disabled so ReadConsoleInputW retains
            // native Win32 VK_UP/VK_DOWN/VK_RETURN and Unicode IME input.
            dwInMode = (dwInMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE))
                     | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
            SetConsoleMode(hIn, dwInMode);
        }
        SetConsoleCP(CP_UTF8);
    }

#if defined(_WIN32)
    static bool handlerInstalled = false;
    if (!handlerInstalled) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        handlerInstalled = true;
    }
    SetConsoleTitleW(L"extract_tickets");
#endif
}

void TuiEngine::EnterAlternateScreen() {
    // \x1b[?7l: Disable DECAWM Auto-Wrap Mode to prevent hardware edge line wrapping & auto-scrolling
    // \x1b[?1000h\x1b[?1006h: Enable mouse button reporting in SGR mode to tell terminal emulators (Windows Terminal, etc.)
    // that the application captures mouse clicks, completely suppressing terminal GUI default right-click paste!
    // \x1b[?1049h: Switch to alternate screen buffer
    // \x1b[?2004h: Bracketed paste mode inside alternate screen
    // \x1b[2J\x1b[H: Clear screen and home cursor
    std::cout << "\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[?1049h\x1b[?2004h\x1b[2J\x1b[H";
    std::cout.flush();
}

void TuiEngine::ExitAlternateScreen() {
    // Restore DECAWM Auto-Wrap Mode (\x1b[?7h), disable bracketed paste (\x1b[?2004l),
    // disable mouse tracking (\x1b[?1000l\x1b[?1006l), and exit alternate screen (\x1b[?1049l)
    std::cout << "\x1b[?7h\x1b[?2004l\x1b[?1000l\x1b[?1006l\x1b[?1049l";
    std::cout.flush();
}

void TuiEngine::ShowCursor(bool show) {
    OutputToTerminal(show ? "\x1b[?25h" : "\x1b[?25l");
    if (!s_inFrame) std::cout.flush();
}

void TuiEngine::MoveCursor(int row, int col) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    if (len > 0) {
        OutputToTerminal(std::string_view(buf, static_cast<size_t>(len)));
    }
}

void TuiEngine::ClearScreen() {
    OutputToTerminal("\x1b[2J\x1b[H");
    if (!s_inFrame) std::cout.flush();
}

void TuiEngine::RepositionCursor() {
    // Reposition cursor to top-left without wiping buffer to black, preventing flicker during typing
    OutputToTerminal("\x1b[H");
    if (!s_inFrame) std::cout.flush();
}

void TuiEngine::GetScreenSize(int& outWidth, int& outHeight) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hOut, &csbi)) {
        outWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        outHeight = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        outWidth = 80;
        outHeight = 25;
    }
}

bool TuiEngine::EnsureMinTerminalSize(int minW, int minH) {
    int w = 80, h = 25;
    GetScreenSize(w, h);
    if (w < minW || h < minH) {
        ClearScreen();
        int midY = (std::max)(1, h / 2 - 1);
        MoveCursor(midY, 1);
        std::string warn1 = TR_FMT(MsgKey::TermTooSmallWarn1, w, h, minW, minH);
        std::string warn2{TR(MsgKey::TermTooSmallWarn2)};
        const size_t safeW = static_cast<size_t>(w > 1 ? w - 1 : 1);
        std::cout << "\x1b[1;33m" << Pad(warn1, safeW, true) << "\x1b[0m";
        MoveCursor(midY + 1, 1);
        std::cout << "\x1b[90m" << Pad(warn2, safeW, true) << "\x1b[0m";
        std::cout.flush();
        return false;
    }
    return true;
}

namespace {
    struct ClipboardScopeGuard {
        bool open{false};
        explicit ClipboardScopeGuard(HWND hwnd = nullptr) noexcept : open(OpenClipboard(hwnd) != FALSE) {}
        ~ClipboardScopeGuard() noexcept {
            if (open) {
                CloseClipboard();
            }
        }
        [[nodiscard]] explicit operator bool() const noexcept { return open; }
    };

    struct GlobalLockScopeGuard {
        HGLOBAL hMem{nullptr};
        const wchar_t* ptr{nullptr};
        explicit GlobalLockScopeGuard(HGLOBAL h) noexcept
            : hMem(h), ptr(h ? static_cast<const wchar_t*>(GlobalLock(h)) : nullptr) {}
        ~GlobalLockScopeGuard() noexcept {
            if (hMem && ptr) {
                GlobalUnlock(hMem);
            }
        }
        [[nodiscard]] const wchar_t* Get() const noexcept { return ptr; }
        [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }
    };

    std::string GetClipboardTextUtf8() {
        ClipboardScopeGuard clipGuard;
        if (!clipGuard) return "";
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) return "";
        GlobalLockScopeGuard lockGuard(hData);
        if (!lockGuard) return "";
        const wchar_t* wstr = lockGuard.Get();
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        std::string result;
        if (len > 1) {
            result.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
        }
        std::string clean;
        clean.reserve(result.size());
        for (char c : result) {
            if (c != '\r' && c != '\n') {
                clean.push_back(c);
            }
        }
        return clean;
    }

    bool TryReadAnsiSequence(HANDLE hIn, std::string& outSeq) {
        outSeq.clear();
        if (WaitForSingleObject(hIn, 15) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(hIn, &pending) || pending == 0) {
            return false;
        }
        INPUT_RECORD ir;
        DWORD read = 0;
        if (!PeekConsoleInputW(hIn, &ir, 1, &read) || read == 0) {
            return false;
        }
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) {
            return false;
        }
        WCHAR lead = ir.Event.KeyEvent.uChar.UnicodeChar;
        if (lead != L'[' && lead != L'O') {
            return false;
        }
        ReadConsoleInputW(hIn, &ir, 1, &read);

        while (outSeq.size() < 32) {
            if (WaitForSingleObject(hIn, 25) != WAIT_OBJECT_0) {
                break;
            }
            if (!ReadConsoleInputW(hIn, &ir, 1, &read) || read == 0) {
                break;
            }
            if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) {
                continue;
            }
            WCHAR c = ir.Event.KeyEvent.uChar.UnicodeChar;
            if (c >= 32 && c <= 126) {
                outSeq.push_back(static_cast<char>(c));
                if ((c >= 0x40 && c <= 0x7E) || c == '~') {
                    return true;
                }
            }
        }
        return !outSeq.empty();
    }

    std::string ReadBracketedPastePayload(HANDLE hIn, bool keepPayload) {
        std::string result;
        while (true) {
            if (WaitForSingleObject(hIn, 300) != WAIT_OBJECT_0) {
                break;
            }
            INPUT_RECORD rec;
            DWORD read = 0;
            if (!ReadConsoleInputW(hIn, &rec, 1, &read) || read == 0) {
                break;
            }
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto& ke = rec.Event.KeyEvent;
            WCHAR wc = ke.uChar.UnicodeChar;
            WORD vk = ke.wVirtualKeyCode;

            if (vk == VK_ESCAPE || wc == 27) {
                std::string seq;
                if (TryReadAnsiSequence(hIn, seq) && seq == "201~") {
                    break;
                }
                continue;
            }

            if (keepPayload && wc != 0) {
                if (wc == L'\r' || wc == L'\n') {
                    continue;
                }
                if (IS_HIGH_SURROGATE(wc)) {
                    INPUT_RECORD nextRec;
                    DWORD nextRead = 0;
                    if (ReadConsoleInputW(hIn, &nextRec, 1, &nextRead) && nextRead == 1 &&
                        nextRec.EventType == KEY_EVENT && IS_LOW_SURROGATE(nextRec.Event.KeyEvent.uChar.UnicodeChar)) {
                        WCHAR pair[2] = { wc, nextRec.Event.KeyEvent.uChar.UnicodeChar };
                        char utf8Buf[8]{0};
                        int b = WideCharToMultiByte(CP_UTF8, 0, pair, 2, utf8Buf, sizeof(utf8Buf) - 1, nullptr, nullptr);
                        if (b > 0 && result.size() + b <= 256) {
                            result.append(utf8Buf, b);
                        }
                    }
                    continue;
                }
                char utf8Buf[8]{0};
                int b = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8Buf, sizeof(utf8Buf) - 1, nullptr, nullptr);
                if (b > 0 && result.size() + b <= 256) {
                    result.append(utf8Buf, b);
                }
            }
        }
        return result;
    }

    bool IsWideCodePoint(uint32_t cp) {
        if (cp < 0x1100) return false;
        return (cp >= 0x1100 && cp <= 0x115F) ||
               (cp >= 0x2329 && cp <= 0x232A) ||
               (cp >= 0x2E80 && cp <= 0x303E) ||
               (cp >= 0x3040 && cp <= 0xA4CF) ||
               (cp >= 0xAC00 && cp <= 0xD7A3) ||
               (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFE10 && cp <= 0xFE19) ||
               (cp >= 0xFE30 && cp <= 0xFE6F) ||
               (cp >= 0xFF00 && cp <= 0xFF60) ||
               (cp >= 0xFFE0 && cp <= 0xFFE6) ||
               (cp >= 0x1F300 && cp <= 0x1F9FF) || // Emojis
               (cp >= 0x20000 && cp <= 0x3FFFD);
    }

    size_t NextCodePoint(std::string_view s, size_t pos, uint32_t& outCp) {
        if (pos >= s.size()) return 0;
        unsigned char c0 = static_cast<unsigned char>(s[pos]);
        if (c0 < 0x80) {
            outCp = c0;
            return 1;
        } else if ((c0 & 0xE0) == 0xC0 && pos + 1 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
            outCp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
            return 2;
        } else if ((c0 & 0xF0) == 0xE0 && pos + 2 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
            outCp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            return 3;
        } else if ((c0 & 0xF8) == 0xF0 && pos + 3 < s.size()) {
            unsigned char c1 = static_cast<unsigned char>(s[pos + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[pos + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[pos + 3]);
            outCp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            return 4;
        }
        outCp = c0;
        return 1;
    }
} // namespace

size_t TuiEngine::GetDisplayWidth(std::string_view utf8Str) {
    size_t width = 0;
    size_t pos = 0;
    uint32_t cp = 0;
    while (pos < utf8Str.size()) {
        size_t len = NextCodePoint(utf8Str, pos, cp);
        if (len == 0) break;
        pos += len;
        width += IsWideCodePoint(cp) ? 2 : 1;
    }
    return width;
}

std::string TuiEngine::TruncateToWidth(std::string_view utf8Str, size_t maxWidth) {
    size_t width = 0;
    size_t pos = 0;
    uint32_t cp = 0;
    while (pos < utf8Str.size()) {
        size_t len = NextCodePoint(utf8Str, pos, cp);
        if (len == 0) break;
        size_t charWidth = IsWideCodePoint(cp) ? 2 : 1;
        if (width + charWidth > maxWidth) {
            break;
        }
        width += charWidth;
        pos += len;
    }
    return std::string{utf8Str.substr(0, pos)};
}

std::string TuiEngine::TruncateHeadToWidth(std::string_view utf8Str, size_t maxWidth) {
    size_t totalWidth = GetDisplayWidth(utf8Str);
    if (totalWidth <= maxWidth) {
        return std::string{utf8Str};
    }
    size_t pos = 0;
    uint32_t cp = 0;
    size_t curWidth = 0;
    size_t excess = totalWidth - maxWidth;
    while (pos < utf8Str.size() && curWidth < excess) {
        size_t len = NextCodePoint(utf8Str, pos, cp);
        if (len == 0) break;
        curWidth += IsWideCodePoint(cp) ? 2 : 1;
        pos += len;
    }
    return std::string{utf8Str.substr(pos)};
}

void TuiEngine::PopBackUtf8(std::string& s) noexcept {
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) {
            break;
        }
    }
}

std::string TuiEngine::Pad(std::string_view utf8Str, size_t targetWidth, bool center) {
    size_t curWidth = GetDisplayWidth(utf8Str);
    if (curWidth >= targetWidth) {
        return TruncateToWidth(utf8Str, targetWidth);
    }
    size_t padTotal = targetWidth - curWidth;
    if (center) {
        size_t leftPad = padTotal / 2;
        size_t rightPad = padTotal - leftPad;
        return std::string(leftPad, ' ') + std::string{utf8Str} + std::string(rightPad, ' ');
    } else {
        return std::string{utf8Str} + std::string(padTotal, ' ');
    }
}

void TuiEngine::PrintBounded(int row, int col, std::string_view text, size_t maxWidth, std::string_view ansiStyle) {
    if (maxWidth == 0) return;
    MoveCursor(row, col);
    std::string truncated = TruncateToWidth(text, maxWidth);
    std::string padded = Pad(truncated, maxWidth);
    if (!ansiStyle.empty()) {
        OutputToTerminal(ansiStyle);
        OutputToTerminal(padded);
        OutputToTerminal("\x1b[0m");
    } else {
        OutputToTerminal(padded);
    }
}

void TuiEngine::DrawHeader(std::string_view title, std::string_view statusTag) {
    int w = 80, h = 25;
    GetScreenSize(w, h);

    MoveCursor(1, 1);
    std::string tagStr = statusTag.empty() ? "" : std::format("[{}]", statusTag);
    size_t tagWidth = GetDisplayWidth(tagStr);
    const size_t maxHeaderW = static_cast<size_t>(w > 1 ? w - 1 : 1);
    size_t availTitle = (maxHeaderW > tagWidth + 4) ? (maxHeaderW - tagWidth - 4) : 20;

    std::string truncatedTitle = TruncateToWidth(title, availTitle);
    size_t titleWidth = GetDisplayWidth(truncatedTitle);

    size_t spaceBetween = 2;
    if (maxHeaderW > titleWidth + tagWidth + 2) {
        spaceBetween = maxHeaderW - titleWidth - tagWidth - 2;
    }

    std::string headerStr = std::format("\x1b[1;37;44m {}{}{} \x1b[0m",
                                        truncatedTitle,
                                        std::string(spaceBetween, ' '),
                                        tagStr);
    OutputToTerminal(headerStr);
}

void TuiEngine::DrawFooter(std::string_view shortcuts, std::string_view ansiStyle) {
    int w = 80, h = 25;
    GetScreenSize(w, h);

    MoveCursor(h, 1);
    // Crucial: Pad to w - 1 to guarantee we never write to the bottom-right corner (h, w),
    // which triggers hardware DECAWM automatic line-feed/vertical scrolling on Windows Console/VT100!
    const size_t maxFooterW = static_cast<size_t>(w > 1 ? w - 1 : 1);
    std::string padded = Pad(std::format(" {}", shortcuts), maxFooterW);
    OutputToTerminal(ansiStyle);
    OutputToTerminal(padded);
    OutputToTerminal("\x1b[0m");
    if (!s_inFrame) std::cout.flush();
}

void TuiEngine::DrawFooter(std::string_view shortcuts) {
    DrawFooter(shortcuts, "\x1b[1;30;47m");
}

void TuiEngine::DrawBox(int top, int left, int width, int height, std::string_view title, bool clearInterior) {
    if (width < 4 || height < 3) return;
    if (top < 1) top = 1;
    if (left < 1) left = 1;

    // Top border
    MoveCursor(top, left);
    OutputToTerminal("\x1b[36m╭");
    if (title.empty()) {
        for (int i = 0; i < width - 2; ++i) OutputToTerminal("─");
    } else {
        std::string titleFormatted = std::format(" [ {} ] ", title);
        size_t titleW = GetDisplayWidth(titleFormatted);
        if (titleW + 4 <= static_cast<size_t>(width)) {
            OutputToTerminal("─\x1b[1;37m");
            OutputToTerminal(titleFormatted);
            OutputToTerminal("\x1b[0;36m");
            for (size_t i = 0; i < width - 2 - 1 - titleW; ++i) OutputToTerminal("─");
        } else {
            for (int i = 0; i < width - 2; ++i) OutputToTerminal("─");
        }
    }
    OutputToTerminal("╮\x1b[0m");

    // Side borders
    if (clearInterior) {
        std::string interiorSpaces(static_cast<size_t>(width - 2), ' ');
        for (int r = 1; r < height - 1; ++r) {
            MoveCursor(top + r, left);
            OutputToTerminal("\x1b[36m│\x1b[0m");
            OutputToTerminal(interiorSpaces);
            OutputToTerminal("\x1b[36m│\x1b[0m");
        }
    } else {
        for (int r = 1; r < height - 1; ++r) {
            MoveCursor(top + r, left);
            OutputToTerminal("\x1b[36m│\x1b[0m");
            MoveCursor(top + r, left + width - 1);
            OutputToTerminal("\x1b[36m│\x1b[0m");
        }
    }

    // Bottom border
    MoveCursor(top + height - 1, left);
    OutputToTerminal("\x1b[36m╰");
    for (int i = 0; i < width - 2; ++i) OutputToTerminal("─");
    OutputToTerminal("╯\x1b[0m");
}

bool TuiEngine::ShowConfirmModal(std::string_view title,
                                 std::string_view question,
                                 std::string_view detail,
                                 bool defaultYes) {
    if (!g_tuiActive) {
        std::cout << "\n[" << title << "] " << question;
        if (!detail.empty()) std::cout << "\n" << detail;
        std::cout << "\n" << TR_FMT(MsgKey::CliConfirmPrompt, defaultYes ? "Y/n" : "y/N");
        std::cout.flush();
        std::string s;
        if (std::getline(std::cin, s)) {
            s = std::string(TrimWhitespace(s));
            if (s.empty()) return defaultYes;
            return (s[0] == 'y' || s[0] == 'Y');
        }
        return defaultYes;
    }

    int w = 80, h = 25;
    GetScreenSize(w, h);

    size_t maxTextW = std::max(GetDisplayWidth(question), GetDisplayWidth(detail));
    maxTextW = std::max(maxTextW, GetDisplayWidth(title) + 8);
    const int modalW = std::clamp(static_cast<int>(maxTextW) + 12, 60, w - 4);
    const int modalH = detail.empty() ? 7 : 9;
    const int top = (std::max)(2, (h - modalH) / 2);
    const int left = (w - modalW) / 2;

    // Clear modal area
    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        std::cout << std::string(modalW, ' ');
    }

    // Draw box
    DrawBox(top, left, modalW, modalH, title);

    // Question line
    MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;37m" << Pad(TruncateToWidth(question, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";

    // Detail line
    if (!detail.empty()) {
        MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << Pad(TruncateToWidth(detail, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";
    }

    bool selectedYes = defaultYes;

    auto renderButtons = [&]() {
        MoveCursor(top + modalH - 2, left + 4);
        std::string_view btnYes = TR(MsgKey::BtnYes);
        std::string_view btnNo = TR(MsgKey::BtnNo);
        if (selectedYes) {
            std::cout << "\x1b[1;97;42m" << btnYes << "\x1b[0m    \x1b[90m" << btnNo << "\x1b[0m";
        } else {
            std::cout << "\x1b[90m" << btnYes << "\x1b[0m    \x1b[1;97;41m" << btnNo << "\x1b[0m";
        }
        size_t buttonsW = GetDisplayWidth(btnYes) + GetDisplayWidth(btnNo) + 4;
        if (static_cast<size_t>(modalW - 8) > buttonsW) {
            std::cout << std::string(static_cast<size_t>(modalW - 8) - buttonsW, ' ');
        }
        std::cout.flush();
    };

    renderButtons();

    FlushInputBuffer();

    while (true) {
        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Left || ev.code == KeyCode::Right || ev.code == KeyCode::Tab) {
            selectedYes = !selectedYes;
            renderButtons();
            continue;
        }
        if (ev.code == KeyCode::Char) {
            if ((ev.ch == 'y' || ev.ch == 'Y') && !HasInputPending()) {
                FlushInputBuffer();
                return true;
            }
            if ((ev.ch == 'n' || ev.ch == 'N') && !HasInputPending()) {
                FlushInputBuffer();
                return false;
            }
        } else if (ev.code == KeyCode::Enter && !HasInputPending()) {
            FlushInputBuffer();
            return selectedYes;
        } else if (ev.code == KeyCode::Escape) {
            FlushInputBuffer();
            return false;
        }
    }
}

void TuiEngine::FlushInputBuffer() noexcept {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
        FlushConsoleInputBuffer(hIn);
    }
}

bool TuiEngine::ShowMessageModal(std::string_view title,
                                 std::string_view message,
                                 std::string_view detail) {
    if (!g_tuiActive) {
        std::cout << "\n[" << title << "] " << message;
        if (!detail.empty()) std::cout << "\n" << detail;
        std::cout << "\n" << TR(MsgKey::CliPressEnterContinue);
        std::cout.flush();
        std::string s;
        std::getline(std::cin, s);
        return true;
    }

    int w = 80, h = 25;
    GetScreenSize(w, h);

    size_t maxTextW = std::max(GetDisplayWidth(message), GetDisplayWidth(detail));
    maxTextW = std::max(maxTextW, GetDisplayWidth(title) + 8);
    const int modalW = std::clamp(static_cast<int>(maxTextW) + 12, 56, w - 4);
    const int modalH = detail.empty() ? 7 : 9;
    const int top = (std::max)(2, (h - modalH) / 2);
    const int left = (w - modalW) / 2;

    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        std::cout << std::string(modalW, ' ');
    }

    DrawBox(top, left, modalW, modalH, title);

    MoveCursor(top + 2, left + 4);
    std::cout << "\x1b[1;37m" << Pad(TruncateToWidth(message, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";

    if (!detail.empty()) {
        MoveCursor(top + 3, left + 4);
        std::cout << "\x1b[90m" << Pad(TruncateToWidth(detail, static_cast<size_t>(modalW - 8)), static_cast<size_t>(modalW - 8)) << "\x1b[0m";
    }

    MoveCursor(top + modalH - 2, left + 4);
    std::string_view btnClose = TR(MsgKey::BtnClose);
    std::cout << "\x1b[1;97;44m" << btnClose << "\x1b[0m";
    size_t btnW = GetDisplayWidth(btnClose);
    if (static_cast<size_t>(modalW - 8) > btnW) {
        std::cout << std::string(static_cast<size_t>(modalW - 8) - btnW, ' ');
    }
    std::cout.flush();

    FlushInputBuffer(); // Discard any residual VK_RETURN injected by mouse right-click before entering modal

    while (true) {
        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Enter && !HasInputPending()) {
            FlushInputBuffer();
            return true;
        }
        if (ev.code == KeyCode::Escape) {
            FlushInputBuffer();
            return false;
        }
    }
}

std::optional<std::string> TuiEngine::PromptInputModal(std::string_view title,
                                                       std::string_view prompt,
                                                       std::string_view defaultValue,
                                                       bool isPassword) {
    if (!g_tuiActive) {
        if (isPassword) {
            std::string promptStr = std::string(prompt);
            SecureString pwd = ReadPasswordFromConsole(promptStr.c_str());
            if (pwd.Empty()) return std::nullopt;
            std::string result(pwd.Data());
            pwd.Clear();
            return result;
        }
        std::cout << "\n" << prompt;
        if (!defaultValue.empty()) {
            std::cout << " [" << defaultValue << "]";
        }
        std::cout << ": ";
        std::cout.flush();
        std::string s;
        if (std::getline(std::cin, s)) {
            if (s.empty() && !defaultValue.empty()) s = defaultValue;
            return s;
        }
        return std::nullopt;
    }

    struct PasteScopeGuard {
        PasteScopeGuard() { TuiEngine::SetPasteAllowed(true); }
        ~PasteScopeGuard() { TuiEngine::SetPasteAllowed(false); }
    } pasteGuard;

    int w = 80, h = 25;
    GetScreenSize(w, h);

    size_t maxTextW = std::max(GetDisplayWidth(prompt), GetDisplayWidth(title) + 8);
    const int modalW = std::clamp(static_cast<int>(maxTextW) + 16, 58, w - 4);
    const int modalH = 8;
    const int top = (std::max)(2, (h - modalH) / 2);
    const int left = (w - modalW) / 2;

    for (int r = 0; r < modalH; ++r) {
        MoveCursor(top + r, left);
        OutputToTerminal(std::string(modalW, ' '));
    }

    DrawBox(top, left, modalW, modalH, title);

    MoveCursor(top + 2, left + 4);
    OutputToTerminal(std::format("\x1b[1;37m{}\x1b[0m", TruncateToWidth(prompt, modalW - 8)));

    MoveCursor(top + modalH - 2, left + 4);
    OutputToTerminal(std::format("\x1b[90m{}\x1b[0m", TR(MsgKey::HintInputEnterEsc)));

    std::string value{defaultValue};
    struct PasswordWiper {
        std::string& s;
        bool isPwd;
        ~PasswordWiper() noexcept {
            if (isPwd) {
                SecureWipeString(s);
            }
        }
    } wiper{value, isPassword};

    const int inputTop = top + 4;
    const int inputLeft = left + 4;
    const int inputWidth = modalW - 8;

    while (true) {
        MoveCursor(inputTop, inputLeft);
        std::string displayVal;
        if (isPassword) {
            displayVal = std::string(value.size(), '*');
        } else {
            displayVal = value;
        }

        const size_t visibleWidth = static_cast<size_t>((std::max)(4, inputWidth - 4));
        std::string visibleText = TruncateHeadToWidth(displayVal, visibleWidth);
        std::string boxContent = std::format("[ {} ]", Pad(visibleText, visibleWidth));
        OutputToTerminal(std::format("\x1b[30;107m{}\x1b[0m", boxContent));
        if (!s_inFrame) std::cout.flush();

        KeyEvent ev = ReadKey();
        if (ev.code == KeyCode::Escape) {
            if (isPassword) {
                SecureWipeString(value);
            }
            FlushInputBuffer();
            return std::nullopt;
        }
        if (ev.code == KeyCode::Enter && !HasInputPending()) {
            std::string result = value;
            if (isPassword) {
                SecureWipeString(value);
            }
            FlushInputBuffer();
            return result;
        }
        if (ev.code == KeyCode::Backspace) {
            if (!value.empty()) {
                PopBackUtf8(value);
            }
        } else if (ev.code == KeyCode::Char) {
            std::string chunk;
            if (!ev.text.empty()) {
                chunk = ev.text;
            } else if (static_cast<unsigned char>(ev.ch) >= 32 && ev.ch != 127) {
                chunk.push_back(ev.ch);
            }

            // Drain burst of input (e.g. paste of long string) to prevent screen flicker
            while (HasInputPending()) {
                KeyEvent nextEv = ReadKey();
                if (nextEv.code == KeyCode::Char) {
                    if (!nextEv.text.empty()) {
                        chunk += nextEv.text;
                    } else if (static_cast<unsigned char>(nextEv.ch) >= 32 && nextEv.ch != 127) {
                        chunk.push_back(nextEv.ch);
                    }
                } else if (nextEv.code == KeyCode::Backspace) {
                    if (!chunk.empty()) {
                        PopBackUtf8(chunk);
                    } else if (!value.empty()) {
                        PopBackUtf8(value);
                    }
                } else if (nextEv.code == KeyCode::Enter) {
                    if (!chunk.empty() && value.size() + chunk.size() <= 256) {
                        value += chunk;
                    }
                    std::string result = value;
                    if (isPassword) {
                        SecureWipeString(value);
                    }
                    FlushInputBuffer();
                    return result;
                } else if (nextEv.code == KeyCode::Escape) {
                    if (isPassword) {
                        SecureWipeString(value);
                    }
                    FlushInputBuffer();
                    return std::nullopt;
                } else {
                    break;
                }
            }

            if (!chunk.empty() && value.size() + chunk.size() <= 256) {
                value += chunk;
            }
        }
    }
}

void TuiEngine::DrawProgressBar(int row, int col, int width,
                                size_t current, size_t total,
                                std::string_view label) {
    if (width < 20) return;
    const double pct = (total == 0) ? 1.0 : std::clamp(static_cast<double>(current) / static_cast<double>(total), 0.0, 1.0);
    const int percentInt = static_cast<int>(pct * 100.0);

    const int barWidth = std::clamp(width - 25, 10, 32);
    const int filled = static_cast<int>(pct * barWidth);

    std::string barStr = "[" + std::string(filled, '=') + (filled < barWidth ? ">" : "") + std::string(barWidth - filled - (filled < barWidth ? 1 : 0), ' ') + "]";
    std::string statsStr = std::format(" {:>3}% ({}/{}) ", percentInt, current, total);
    size_t prefixW = static_cast<size_t>(barWidth) + GetDisplayWidth(statsStr);

    size_t maxLabelW = (static_cast<size_t>(width) > prefixW) ? (static_cast<size_t>(width) - prefixW) : 0;
    std::string truncatedLabel = TruncateToWidth(label, maxLabelW);
    std::string paddedLabel = Pad(truncatedLabel, maxLabelW);

    MoveCursor(row, col);
    OutputToTerminal(std::format("\x1b[1;32m{}\x1b[0m\x1b[1;37m{}\x1b[0m\x1b[90m{}\x1b[0m",
                                 barStr, statsStr, paddedLabel));
    if (!s_inFrame) std::cout.flush();
}

KeyEvent TuiEngine::ReadKey() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) {
        return { KeyCode::None, 0, "" };
    }

    DWORD dwMode = 0;
    if (!GetConsoleMode(hIn, &dwMode)) {
        int c = std::cin.get();
        if (c == EOF) return { KeyCode::Escape, 0, "" };
        if (c == '\r' || c == '\n') return { KeyCode::Enter, 0, "" };
        return { KeyCode::Char, static_cast<char>(c), std::string(1, static_cast<char>(c)) };
    }

    static int s_lastW = 0, s_lastH = 0;
    if (s_lastW == 0 && s_lastH == 0) {
        GetScreenSize(s_lastW, s_lastH);
    }

    INPUT_RECORD ir;
    DWORD numRead = 0;

    while (true) {
        int curW = 0, curH = 0;
        GetScreenSize(curW, curH);
        if (curW != s_lastW || curH != s_lastH) {
            s_lastW = curW;
            s_lastH = curH;
            return { KeyCode::Resize, 0, "" };
        }

        if (!ReadConsoleInputW(hIn, &ir, 1, &numRead) || numRead == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (ir.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            GetScreenSize(s_lastW, s_lastH);
            return { KeyCode::Resize, 0, "" };
        }

        if (ir.EventType == MOUSE_EVENT) {
            const auto& me = ir.Event.MouseEvent;
            // Handle right-click press (RIGHTMOST_BUTTON_PRESSED) on button click (dwEventFlags == 0):
            if (me.dwEventFlags == 0 && (me.dwButtonState & RIGHTMOST_BUTTON_PRESSED)) {
                if (s_pasteAllowed) {
                    std::string clip = GetClipboardTextUtf8();
                    if (!clip.empty()) {
                        return { KeyCode::Char, 0, std::move(clip) };
                    }
                }
            }
            // Discard all other mouse interactions (movement, left-click, wheel, right-click in menus)
            continue;
        }

        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            const auto& ke = ir.Event.KeyEvent;
            WORD vk = ke.wVirtualKeyCode;
            WCHAR wc = ke.uChar.UnicodeChar;

            if (vk == VK_UP) return { KeyCode::Up, 0, "" };
            if (vk == VK_DOWN) return { KeyCode::Down, 0, "" };
            if (vk == VK_LEFT) return { KeyCode::Left, 0, "" };
            if (vk == VK_RIGHT) return { KeyCode::Right, 0, "" };
            if (vk == VK_DELETE) return { KeyCode::Delete, 0, "" };
            if (vk == VK_RETURN) return { KeyCode::Enter, 0, "" };
            if (vk == VK_BACK) return { KeyCode::Backspace, 0, "" };
            if (vk == VK_TAB) return { KeyCode::Tab, 0, "" };

            // Intercept Ctrl+V
            if ((vk == 'V' && (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) || wc == 22) {
                if (s_pasteAllowed) {
                    std::string clip = GetClipboardTextUtf8();
                    if (!clip.empty()) {
                        return { KeyCode::Char, 0, std::move(clip) };
                    }
                }
                return { KeyCode::None, 0, "" };
            }

            if (vk == VK_ESCAPE || wc == 27) {
                std::string seq;
                if (TryReadAnsiSequence(hIn, seq)) {
                    if (!seq.empty() && seq[0] == '<') {
                        // SGR mouse sequence: <button;col;row(M|m)
                        // If right button pressed (<2;...M) and paste is allowed:
                        if (s_pasteAllowed && seq.starts_with("<2;") && seq.ends_with('M')) {
                            std::string clip = GetClipboardTextUtf8();
                            if (!clip.empty()) {
                                return { KeyCode::Char, 0, std::move(clip) };
                            }
                        }
                        return { KeyCode::None, 0, "" };
                    }
                    if (seq == "A") return { KeyCode::Up, 0, "" };
                    if (seq == "B") return { KeyCode::Down, 0, "" };
                    if (seq == "C") return { KeyCode::Right, 0, "" };
                    if (seq == "D") return { KeyCode::Left, 0, "" };
                    if (seq == "3~") return { KeyCode::Delete, 0, "" };
                    if (seq == "200~") {
                        std::string payload = ReadBracketedPastePayload(hIn, s_pasteAllowed);
                        if (s_pasteAllowed && !payload.empty()) {
                            return { KeyCode::Char, 0, std::move(payload) };
                        }
                        return { KeyCode::None, 0, "" };
                    }
                    if (seq == "201~") {
                        return { KeyCode::None, 0, "" };
                    }
                    return { KeyCode::None, 0, "" };
                }
                return { KeyCode::Escape, 0, "" };
            }

            // Ignore pure modifier keys (Shift, Ctrl, Alt, CapsLock, NumLock, etc.)
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) {
                continue;
            }

            // Printable character or IME committed character
            if (wc != 0) {
                if (IS_HIGH_SURROGATE(wc)) {
                    INPUT_RECORD nextIr;
                    DWORD nextRead = 0;
                    if (ReadConsoleInputW(hIn, &nextIr, 1, &nextRead) && nextRead == 1 &&
                        nextIr.EventType == KEY_EVENT && IS_LOW_SURROGATE(nextIr.Event.KeyEvent.uChar.UnicodeChar)) {
                        WCHAR pair[2] = { wc, nextIr.Event.KeyEvent.uChar.UnicodeChar };
                        char utf8Buf[8]{0};
                        int bytes = WideCharToMultiByte(CP_UTF8, 0, pair, 2, utf8Buf, sizeof(utf8Buf) - 1, nullptr, nullptr);
                        if (bytes > 0) {
                            return { KeyCode::Char, 0, std::string(utf8Buf, bytes) };
                        }
                    }
                    continue;
                }

                char utf8Buf[8]{0};
                int bytes = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8Buf, sizeof(utf8Buf) - 1, nullptr, nullptr);
                if (bytes > 0) {
                    char asciiCh = (bytes == 1) ? utf8Buf[0] : 0;
                    return { KeyCode::Char, asciiCh, std::string(utf8Buf, bytes) };
                }
            }
        }
    }
}

bool TuiEngine::HasInputPending() noexcept {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) {
        return false;
    }
    DWORD dwMode = 0;
    if (!GetConsoleMode(hIn, &dwMode)) {
        return false;
    }
    DWORD numEvents = 0;
    if (!GetNumberOfConsoleInputEvents(hIn, &numEvents) || numEvents == 0) {
        return false;
    }
    std::vector<INPUT_RECORD> peekRecs(numEvents);
    DWORD peekRead = 0;
    if (!PeekConsoleInputW(hIn, peekRecs.data(), numEvents, &peekRead) || peekRead == 0) {
        return false;
    }
    for (DWORD i = 0; i < peekRead; ++i) {
        if (peekRecs[i].EventType == KEY_EVENT && peekRecs[i].Event.KeyEvent.bKeyDown) {
            WORD vk = peekRecs[i].Event.KeyEvent.wVirtualKeyCode;
            if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                vk != VK_CAPITAL && vk != VK_NUMLOCK && vk != VK_SCROLL) {
                return true;
            }
        }
        if (peekRecs[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
            return true;
        }
    }
    return false;
}

std::optional<KeyEvent> TuiEngine::PollKey(int timeoutMs) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hIn == nullptr) {
        return std::nullopt;
    }

    DWORD dwMode = 0;
    if (!GetConsoleMode(hIn, &dwMode)) {
        return ReadKey();
    }

    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        int remainingMs = timeoutMs;
        if (timeoutMs > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= timeoutMs) {
                return std::nullopt;
            }
            remainingMs = timeoutMs - static_cast<int>(elapsed);
        }

        DWORD waitRes = WaitForSingleObject(hIn, remainingMs > 0 ? remainingMs : 0);
        if (waitRes != WAIT_OBJECT_0) {
            return std::nullopt;
        }

        DWORD numEvents = 0;
        if (!GetNumberOfConsoleInputEvents(hIn, &numEvents) || numEvents == 0) {
            return std::nullopt;
        }

        std::vector<INPUT_RECORD> peekRecs(numEvents);
        DWORD peekRead = 0;
        if (!PeekConsoleInputW(hIn, peekRecs.data(), numEvents, &peekRead) || peekRead == 0) {
            return std::nullopt;
        }

        bool hasActionable = false;
        for (DWORD i = 0; i < peekRead; ++i) {
            if (peekRecs[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                hasActionable = true;
                break;
            }
            if (peekRecs[i].EventType == KEY_EVENT && peekRecs[i].Event.KeyEvent.bKeyDown) {
                WORD vk = peekRecs[i].Event.KeyEvent.wVirtualKeyCode;
                if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                    vk != VK_CAPITAL && vk != VK_NUMLOCK && vk != VK_SCROLL) {
                    hasActionable = true;
                    break;
                }
            }
        }

        if (hasActionable) {
            return ReadKey();
        }

        // Only non-actionable events (mouse, key-up, pure modifier) -> drain one record to advance
        INPUT_RECORD drainIr;
        DWORD drainRead = 0;
        ReadConsoleInputW(hIn, &drainIr, 1, &drainRead);
    }
}

} // namespace OST::ExtractTickets
