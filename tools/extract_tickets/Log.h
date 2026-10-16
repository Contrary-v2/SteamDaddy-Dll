#pragma once

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#if defined(_DEBUG) || defined(EXTRACT_TICKETS_LOGGING_ENABLED)
    #define EXTRACT_TICKETS_HAS_LOGGING 1
    #include "I18n.h"
#else
    #define EXTRACT_TICKETS_HAS_LOGGING 0
#endif

namespace OST::ExtractTickets {

// ============================================================================
// Security Masking Functions (Triple Masking Standard)
// ============================================================================

[[nodiscard]] std::string MaskAccount(std::string_view account);
[[nodiscard]] std::string MaskSteamId(uint64_t steamId);
[[nodiscard]] std::string MaskToken(std::string_view token, size_t prefixChars = 6, size_t suffixChars = 4);
[[nodiscard]] std::string MaskTicketHex(std::span<const uint8_t> ticket, size_t prefixBytes = 4, size_t suffixBytes = 4);
[[nodiscard]] std::string MaskKeyHex(std::string_view hexKey);
[[nodiscard]] std::string MaskGroupId(std::string_view groupId);
[[nodiscard]] std::string MaskGroupId(uint64_t groupId);
[[nodiscard]] std::string MaskEmail(std::string_view email);
[[nodiscard]] std::string MaskPicsToken(uint64_t picsToken);
[[nodiscard]] std::string MaskPath(std::string_view path);
[[nodiscard]] std::string MaskUrl(std::string_view url);

// ============================================================================
// Diagnostics Logging System
// ============================================================================

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error
};

#if EXTRACT_TICKETS_HAS_LOGGING

void LogMessage(LogLevel level, std::string_view tag, std::string_view message);
void InitLogging(const std::string& logFileName = "extract_tickets_debug.log");
void CloseLogging();

template <typename... Args>
inline void LogTrace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
#if defined(_DEBUG) || !defined(NDEBUG)
    LogMessage(LogLevel::Trace, tag, std::format(fmt, std::forward<Args>(args)...));
#else
    (void)tag;
#endif
}

template <typename... Args>
inline void LogTrace(std::string_view tag, MsgKey key, Args&&... args) {
#if defined(_DEBUG) || !defined(NDEBUG)
    if constexpr (sizeof...(Args) == 0) {
        LogMessage(LogLevel::Trace, tag, TR(key));
    } else {
        LogMessage(LogLevel::Trace, tag, TR_FMT(key, std::forward<Args>(args)...));
    }
#else
    (void)tag;
    (void)key;
#endif
}

template <typename... Args>
inline void LogDebug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    LogMessage(LogLevel::Debug, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void LogDebug(std::string_view tag, MsgKey key, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        LogMessage(LogLevel::Debug, tag, TR(key));
    } else {
        LogMessage(LogLevel::Debug, tag, TR_FMT(key, std::forward<Args>(args)...));
    }
}

template <typename... Args>
inline void LogInfo(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    LogMessage(LogLevel::Info, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void LogInfo(std::string_view tag, MsgKey key, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        LogMessage(LogLevel::Info, tag, TR(key));
    } else {
        LogMessage(LogLevel::Info, tag, TR_FMT(key, std::forward<Args>(args)...));
    }
}

template <typename... Args>
inline void LogWarn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    LogMessage(LogLevel::Warn, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void LogWarn(std::string_view tag, MsgKey key, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        LogMessage(LogLevel::Warn, tag, TR(key));
    } else {
        LogMessage(LogLevel::Warn, tag, TR_FMT(key, std::forward<Args>(args)...));
    }
}

template <typename... Args>
inline void LogError(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    LogMessage(LogLevel::Error, tag, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void LogError(std::string_view tag, MsgKey key, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
        LogMessage(LogLevel::Error, tag, TR(key));
    } else {
        LogMessage(LogLevel::Error, tag, TR_FMT(key, std::forward<Args>(args)...));
    }
}

#define LOG_TRACE(...) ::OST::ExtractTickets::LogTrace(__VA_ARGS__)
#define LOG_DEBUG(...) ::OST::ExtractTickets::LogDebug(__VA_ARGS__)
#define LOG_INFO(...)  ::OST::ExtractTickets::LogInfo(__VA_ARGS__)
#define LOG_WARN(...)  ::OST::ExtractTickets::LogWarn(__VA_ARGS__)
#define LOG_ERROR(...) ::OST::ExtractTickets::LogError(__VA_ARGS__)

#else

inline void LogMessage(LogLevel, std::string_view, std::string_view) noexcept {}
inline void InitLogging(const std::string& = "") noexcept {}
inline void CloseLogging() noexcept {}

#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...)  ((void)0)
#define LOG_WARN(...)  ((void)0)
#define LOG_ERROR(...) ((void)0)

#endif

} // namespace OST::ExtractTickets
