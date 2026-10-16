#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

enum class KeyCode {
    None,
    Char,
    Up,
    Down,
    Left,
    Right,
    Enter,
    Escape,
    Backspace,
    Delete,
    Tab,
    Resize,
};

struct KeyEvent {
    KeyCode code{KeyCode::None};
    char ch{0};
    std::string text{};
};

class TuiEngine {
public:
    // Terminal setup and RAII alternate buffer
    static void EnableVirtualTerminal();
    static void EnterAlternateScreen();
    static void ExitAlternateScreen();
    static void ShowCursor(bool show);
    static void MoveCursor(int row, int col);
    static void ClearScreen();
    static void RepositionCursor();

    // Screen dimensions
    static void GetScreenSize(int& outWidth, int& outHeight);
    [[nodiscard]] static bool EnsureMinTerminalSize(int minW = 76, int minH = 20);

    // Text formatting and East Asian width alignment
    static size_t GetDisplayWidth(std::string_view utf8Str);
    static std::string TruncateToWidth(std::string_view utf8Str, size_t maxWidth);
    static std::string TruncateHeadToWidth(std::string_view utf8Str, size_t maxWidth);
    static void PopBackUtf8(std::string& s) noexcept;
    static std::string Pad(std::string_view utf8Str, size_t targetWidth, bool center = false);
    static void PrintBounded(int row, int col, std::string_view text, size_t maxWidth, std::string_view ansiStyle = "");

    // Frame buffer batch rendering
    static void BeginFrame();
    static void EndFrame();
    static void PrintRaw(std::string_view str);

    // Box and UI drawing
    static void DrawHeader(std::string_view title, std::string_view statusTag);
    static void DrawFooter(std::string_view shortcuts, std::string_view ansiStyle);
    static void DrawFooter(std::string_view shortcuts);
    static void DrawBox(int top, int left, int width, int height, std::string_view title = "", bool clearInterior = true);
    
    // Modal confirmation dialog (returns true for Yes, false for No)
    static bool ShowConfirmModal(std::string_view title,
                                std::string_view question,
                                std::string_view detail = "",
                                bool defaultYes = false);

    // Modal message alert dialog (returns true for Enter, false for ESC)
    static bool ShowMessageModal(std::string_view title,
                                 std::string_view message,
                                 std::string_view detail = "");

    static void FlushInputBuffer() noexcept;

    // Paste control for modals and terminal bracketed paste
    static void SetPasteAllowed(bool allowed) noexcept;
    [[nodiscard]] static bool IsPasteAllowed() noexcept;

    // Modal text/password input dialog (returns std::nullopt if cancelled via ESC)
    static std::optional<std::string> PromptInputModal(std::string_view title,
                                                       std::string_view prompt,
                                                       std::string_view defaultValue = "",
                                                       bool isPassword = false);

    // Progress bar inside an existing box or standalone
    static void DrawProgressBar(int row, int col, int width,
                                size_t current, size_t total,
                                std::string_view label = "");

    static bool IsActive() noexcept;
    static void SetActive(bool active) noexcept;

    // Input reading
    static KeyEvent ReadKey();
    [[nodiscard]] static std::optional<KeyEvent> PollKey(int timeoutMs);
    [[nodiscard]] static bool HasInputPending() noexcept;
};

class TuiSessionGuard {
public:
    explicit TuiSessionGuard(bool active = true) : m_active(active) {
        if (m_active) {
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
                if (GetConsoleMode(hIn, &m_origInMode)) {
                    m_hasOrigInMode = true;
                    // Enable ENABLE_MOUSE_INPUT with ENABLE_WINDOW_INPUT and ENABLE_EXTENDED_FLAGS.
                    // Keep ENABLE_VIRTUAL_TERMINAL_INPUT disabled so ReadConsoleInputW retains
                    // native Win32 VK_UP/VK_DOWN/VK_RETURN and Unicode IME input.
                    DWORD newMode = (m_origInMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE))
                                  | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
                    SetConsoleMode(hIn, newMode);
                }
            }
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
                if (GetConsoleMode(hOut, &m_origOutMode)) {
                    m_hasOrigOutMode = true;
                }
            }
            m_origOutputCP = GetConsoleOutputCP();
            TuiEngine::SetActive(true);
            TuiEngine::EnableVirtualTerminal();
            TuiEngine::EnterAlternateScreen();
            TuiEngine::ShowCursor(false);
            TuiEngine::ClearScreen();
        }
    }

    ~TuiSessionGuard() {
        if (m_active) {
            TuiEngine::ShowCursor(true);
            TuiEngine::ExitAlternateScreen();
            TuiEngine::SetActive(false);
            if (m_hasOrigInMode) {
                HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
                if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
                    SetConsoleMode(hIn, m_origInMode | ENABLE_EXTENDED_FLAGS);
                }
            }
            if (m_hasOrigOutMode) {
                HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
                    SetConsoleMode(hOut, m_origOutMode);
                }
            }
            if (m_origOutputCP != 0) {
                SetConsoleOutputCP(m_origOutputCP);
            }
        }
    }

    TuiSessionGuard(const TuiSessionGuard&) = delete;
    TuiSessionGuard& operator=(const TuiSessionGuard&) = delete;

private:
    bool m_active{true};
    bool m_hasOrigInMode{false};
    DWORD m_origInMode{0};
    bool m_hasOrigOutMode{false};
    DWORD m_origOutMode{0};
    UINT m_origOutputCP{0};
};

} // namespace OST::ExtractTickets
