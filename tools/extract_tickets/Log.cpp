#include "Log.h"
#include "Utils.h"
#if EXTRACT_TICKETS_HAS_LOGGING
#include "TuiEngine.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>

namespace OST::ExtractTickets {

#if EXTRACT_TICKETS_HAS_LOGGING
namespace {
    std::mutex g_logMutex;
    std::ofstream g_logFile;
    bool g_loggingInitialized = false;


    std::string GetTimestampString() {
        const auto now = std::chrono::system_clock::now();
        const auto nowTimeT = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tmNow{};
#if defined(_WIN32)
        localtime_s(&tmNow, &nowTimeT);
#else
        localtime_r(&nowTimeT, &tmNow);
#endif

        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                           tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                           tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
                           static_cast<int>(ms.count()));
    }

    const char* LogLevelToString(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
            default:              return "UNKNOWN";
        }
    }

    std::string RedactMessage(std::string_view msg) {
        if (msg.empty()) return {};
        std::string result{msg};

        // 1. JWT tokens: scan for "eyJ"
        size_t pos = 0;
        while ((pos = result.find("eyJ", pos)) != std::string::npos) {
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(result[pos - 1])) || result[pos - 1] == '_' || result[pos - 1] == '-')) {
                pos += 3;
                continue;
            }
            size_t endPos = pos + 3;
            while (endPos < result.size() &&
                   (std::isalnum(static_cast<unsigned char>(result[endPos])) ||
                    result[endPos] == '_' || result[endPos] == '-' || result[endPos] == '.')) {
                ++endPos;
            }
            size_t len = endPos - pos;
            if (len >= 30) {
                std::string masked = MaskToken(std::string_view(result.data() + pos, len));
                result.replace(pos, len, masked);
                pos += masked.size();
            } else {
                pos += 3;
            }
        }

        // 2. Windows User Path redaction
        if (FindIgnoreCase(result, "users") != std::string_view::npos) {
            result = MaskPath(result);
        }

        return result;
    }
} // namespace
#endif // EXTRACT_TICKETS_HAS_LOGGING

namespace {
    [[nodiscard]] constexpr bool IsUtf8ContinuationByte(char c) noexcept {
        return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
    }

    [[nodiscard]] constexpr size_t CountUtf8Codepoints(std::string_view s) noexcept {
        size_t count = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i == 0 || !IsUtf8ContinuationByte(s[i])) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] constexpr std::string_view Utf8Prefix(std::string_view s, size_t maxCodepoints) noexcept {
        if (maxCodepoints == 0 || s.empty()) return {};
        size_t cp = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i == 0 || !IsUtf8ContinuationByte(s[i])) {
                if (cp == maxCodepoints) {
                    return s.substr(0, i);
                }
                ++cp;
            }
        }
        return s;
    }

    [[nodiscard]] constexpr std::string_view Utf8Suffix(std::string_view s, size_t maxCodepoints) noexcept {
        if (maxCodepoints == 0 || s.empty()) return {};
        size_t cp = 0;
        for (size_t i = s.size(); i > 0; --i) {
            const size_t idx = i - 1;
            if (idx == 0 || !IsUtf8ContinuationByte(s[idx])) {
                ++cp;
                if (cp == maxCodepoints) {
                    return s.substr(idx);
                }
            }
        }
        return s;
    }
} // namespace

std::string MaskAccount(std::string_view account) {
    if (account.empty()) return "***";
    const size_t cpCount = CountUtf8Codepoints(account);
    if (cpCount <= 2) {
        return std::string{Utf8Prefix(account, 1)} + "*";
    }
    if (cpCount == 3) {
        return std::string{Utf8Prefix(account, 1)} + "*" + std::string{Utf8Suffix(account, 1)};
    }
    return std::string{Utf8Prefix(account, 2)} + "***" + std::string{Utf8Suffix(account, 2)};
}

std::string MaskSteamId(uint64_t steamId) {
    if (steamId == 0) return "0";
    std::string s = std::to_string(steamId);
    if (s.size() >= 17) {
        // e.g. 76561198 01234 5678 -> 76561198****5678
        return s.substr(0, 8) + "****" + s.substr(s.size() - 4);
    }
    if (s.size() > 6) {
        return s.substr(0, 3) + "***" + s.substr(s.size() - 2);
    }
    return "***";
}

std::string MaskToken(std::string_view token, size_t prefixChars, size_t suffixChars) {
    if (token.empty()) return "null";
    if (token.size() <= prefixChars + suffixChars) {
        return "***";
    }
    return std::format("{}...[MASKED_len={}]...{}",
                       token.substr(0, prefixChars),
                       token.size(),
                       token.substr(token.size() - suffixChars));
}

std::string MaskTicketHex(std::span<const uint8_t> ticket, size_t prefixBytes, size_t suffixBytes) {
    if (ticket.empty()) return "null";
    if (ticket.size() <= prefixBytes + suffixBytes) {
        return std::format("[TICKET_{}B]", ticket.size());
    }

    auto toHexStr = [](std::span<const uint8_t> bytes) -> std::string {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            std::format_to(std::back_inserter(hex), "{:02X}", b);
        }
        return hex;
    };

    const std::string headHex = toHexStr(ticket.subspan(0, prefixBytes));
    const std::string tailHex = toHexStr(ticket.subspan(ticket.size() - suffixBytes));
    return std::format("{}...[TICKET_{}B]...{}", headHex, ticket.size(), tailHex);
}

std::string MaskKeyHex(std::string_view hexKey) {
    if (hexKey.empty()) return "null";
    if (hexKey.size() >= 8) {
        return std::string{hexKey.substr(0, 4)} + "***" + std::string{hexKey.substr(hexKey.size() - 4)};
    }
    return "***";
}

std::string MaskGroupId(std::string_view groupId) {
    if (groupId.empty()) return "***";
    const size_t len = groupId.size();
    if (len <= 4) {
        return std::string{groupId.substr(0, 1)} + "**" + std::string{groupId.substr(len - 1)};
    }
    return std::string{groupId.substr(0, 3)} + "****" + std::string{groupId.substr(len - 2)};
}

std::string MaskGroupId(uint64_t groupId) {
    return MaskGroupId(std::to_string(groupId));
}

std::string MaskEmail(std::string_view email) {
    if (email.empty()) return "***";
    size_t atPos = email.find('@');
    if (atPos == std::string_view::npos) {
        return MaskAccount(email);
    }
    std::string_view local = email.substr(0, atPos);
    std::string_view domain = email.substr(atPos); // includes '@'
    const size_t cpCount = CountUtf8Codepoints(local);
    if (cpCount <= 1) {
        return "*" + std::string{domain};
    }
    if (cpCount <= 3) {
        return std::string{Utf8Prefix(local, 1)} + "*@" + std::string{email.substr(atPos + 1)};
    }
    return std::string{Utf8Prefix(local, 2)} + "***" + std::string{Utf8Suffix(local, 1)} + std::string{domain};
}

std::string MaskPicsToken(uint64_t picsToken) {
    if (picsToken == 0) return "0";
    std::string s = std::to_string(picsToken);
    if (s.size() >= 8) {
        return s.substr(0, 4) + "****" + s.substr(s.size() - 4);
    }
    return "***";
}

std::string MaskPath(std::string_view path) {
    if (path.empty()) return {};
    std::string s{path};

    size_t pos = 0;
    while ((pos = FindIgnoreCase(s, "users", pos)) != std::string_view::npos) {
        const bool validStart = (pos == 0 || s[pos - 1] == '\\' || s[pos - 1] == '/' ||
                                 s[pos - 1] == '"' || s[pos - 1] == '\'' ||
                                 std::isspace(static_cast<unsigned char>(s[pos - 1])));
        size_t slashPos = pos + 5;
        if (validStart && slashPos < s.size() && (s[slashPos] == '\\' || s[slashPos] == '/')) {
            size_t userStart = slashPos + 1;
            while (userStart < s.size() && (s[userStart] == '\\' || s[userStart] == '/')) {
                ++userStart;
            }
            if (userStart < s.size()) {
                size_t userEnd = s.find_first_of("\\/\"' \t\r\n", userStart);
                if (userEnd == std::string::npos) {
                    userEnd = s.size();
                }
                if (userEnd > userStart) {
                    std::string_view userPart(s.data() + userStart, userEnd - userStart);
                    std::string maskedUser = MaskAccount(userPart);
                    s.replace(userStart, userEnd - userStart, maskedUser);
                    pos = userStart + maskedUser.size();
                    continue;
                }
            }
        }
        pos += 5;
    }
    return s;
}

std::string MaskUrl(std::string_view url) {
    if (url.empty()) return {};
    std::string s{url};
    constexpr std::string_view sensitiveKeys[] = {
        "access_token=", "refresh_token=", "password=", "auth_token="
    };

    for (const auto& key : sensitiveKeys) {
        size_t pos = 0;
        while ((pos = s.find(key, pos)) != std::string::npos) {
            size_t valStart = pos + key.size();
            size_t valEnd = s.find_first_of("&# \r\n", valStart);
            if (valEnd == std::string::npos) {
                valEnd = s.size();
            }
            if (valEnd > valStart) {
                std::string_view val(s.data() + valStart, valEnd - valStart);
                std::string masked = MaskToken(val);
                s.replace(valStart, valEnd - valStart, masked);
                pos = valStart + masked.size();
            } else {
                pos = valStart;
            }
        }
    }
    return s;
}

#if EXTRACT_TICKETS_HAS_LOGGING

void InitLogging(const std::string& logFileName) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_loggingInitialized) {
        g_logFile.open(Utf8Path(logFileName), std::ios::app);
        g_loggingInitialized = true;
        if (g_logFile.is_open()) {
            g_logFile << "\n======================================================================\n"
                      << "  " << TR_FMT(MsgKey::LogSessionStartedHeader, GetTimestampString()) << "\n"
                      << "======================================================================\n";
            g_logFile.flush();
        }
    }
}

void CloseLogging() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile.flush();
        g_logFile.close();
    }
    g_loggingInitialized = false;
}

void LogMessage(LogLevel level, std::string_view tag, std::string_view message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_loggingInitialized) {
        g_logFile.open(Utf8Path("extract_tickets_debug.log"), std::ios::app);
        g_loggingInitialized = true;
    }

    const std::string safeMsg = RedactMessage(message);

    const std::string line = std::format("[{}] [{:<5}] [{}] {}",
                                         GetTimestampString(),
                                         LogLevelToString(level),
                                         tag,
                                         safeMsg);

    if (g_logFile.is_open()) {
        g_logFile << line << "\n";
        g_logFile.flush();
    }
}

#endif // EXTRACT_TICKETS_HAS_LOGGING

} // namespace OST::ExtractTickets

