#include "Utils.h"
#include "I18n.h"
#include "Log.h"
#include "TuiEngine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if __has_include(<OSTPlatform/SteamLocator.hpp>)
#include <OSTPlatform/SteamLocator.hpp>
#else
#include "../../src/OSTPlatform/include/OSTPlatform/SteamLocator.hpp"
#endif

#include "RaiiGuards.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace OST::ExtractTickets {

std::string_view TrimWhitespace(std::string_view str) noexcept {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    return str;
}

bool IsDecimal(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool IsValidManifestId(std::string_view value) noexcept {
    if (value.empty()) return false;
    bool hasNonZero = false;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        if (ch != '0') hasNonZero = true;
    }
    return hasNonZero;
}

bool IsHex64(std::string_view value) noexcept {
    if (value.size() != 64) return false;
    for (char ch : value) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::optional<uint32_t> ParseAppId(std::string_view value) noexcept {
    value = TrimWhitespace(value);
    if (value.empty()) return std::nullopt;
    uint32_t parsed{0};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

std::string SanitizeComment(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return out;
}

std::vector<std::string> TokenizeQuoted(std::string_view line) {
    std::vector<std::string> tokens;
    tokens.reserve(4);

    size_t pos = 0;
    while (pos < line.size()) {
        pos = line.find('"', pos);
        if (pos == std::string_view::npos) break;

        std::string token;
        token.reserve(32);
        size_t i = pos + 1;
        bool closed = false;

        while (i < line.size()) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                if (line[i + 1] == '"') {
                    token.push_back('"');
                    i += 2;
                    continue;
                }
                if (line[i + 1] == '\\') {
                    token.push_back('\\');
                    i += 2;
                    continue;
                }
            }
            if (line[i] == '"') {
                closed = true;
                pos = i + 1;
                break;
            }
            token.push_back(line[i]);
            ++i;
        }

        if (!closed) break;
        tokens.push_back(std::move(token));
    }

    return tokens;
}

std::string ToHexString(std::span<const uint8_t> data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return out;
}

bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> data) {
    const auto tempPath = std::filesystem::path(
        std::format(L"{}.{}.tmp", path.native(), GetCurrentProcessId()));

    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    bool writeOk = false;
    {
        ScopedHandle hTemp{CreateFileW(
            tempPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)};
        if (hTemp.IsValid()) {
            DWORD bytesWritten = 0;
            const auto* ptr = reinterpret_cast<const char*>(data.data());
            size_t remaining = data.size();
            bool hasError = false;
            while (remaining > 0) {
                DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(MAXDWORD)));
                if (!WriteFile(hTemp, ptr, chunk, &bytesWritten, nullptr) || bytesWritten == 0) {
                    hasError = true;
                    break;
                }
                ptr += bytesWritten;
                remaining -= bytesWritten;
            }
            if (!hasError && FlushFileBuffers(hTemp)) {
                writeOk = true;
            }
        }
    }

    if (!writeOk) {
        std::filesystem::remove(tempPath, ec);
        const std::string utf8Path = PathToUtf8(path);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutWriteFileFailed, utf8Path) << "\n";
        }
        LOG_ERROR("Utils", MsgKey::LogUtilsWriteFileFailed, MaskPath(utf8Path));
        return false;
    }

    bool replaced = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (ReplaceFileW(path.c_str(), tempPath.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            replaced = true;
            break;
        }
        DWORD lastErr = GetLastError();
        if (lastErr == ERROR_FILE_NOT_FOUND) {
            if (MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                replaced = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!replaced) {
        std::filesystem::remove(tempPath, ec);
        const std::string utf8Path = PathToUtf8(path);
        if (!TuiEngine::IsActive()) {
            std::cerr << TR_FMT(MsgKey::OutWriteFileFailed, utf8Path) << "\n";
        }
        LOG_ERROR("Utils", MsgKey::LogUtilsWriteFileFailed, MaskPath(utf8Path));
        return false;
    }

    return true;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, std::string_view content, [[maybe_unused]] bool binaryMode) {
    return WriteBinaryFile(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(content.data()), content.size()));
}

std::string JoinPath(std::string_view base, std::string_view name) {
    std::string result;
    result.reserve(base.size() + 1 + name.size());
    result.append(base);
    std::replace(result.begin(), result.end(), '/', '\\');
    if (!result.empty() && result.back() != '\\') {
        result.push_back('\\');
    }
    const size_t nameStart = result.size();
    result.append(name);
    std::replace(result.begin() + nameStart, result.end(), '/', '\\');
    return result;
}

std::string NormalizeDir(std::string dir) {
    std::replace(dir.begin(), dir.end(), '/', '\\');
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return dir;
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), len);
    return w;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), s.data(), len, nullptr, nullptr);
    return s;
}

std::optional<std::string> FindSteamInstallPath() {
    static const auto s_cachedPath = []() -> std::optional<std::string> {
        if (auto resolved = OSTPlatform::SteamLocator::ResolvePath()) {
            std::string norm = NormalizeDir(PathToUtf8(*resolved));
            LOG_DEBUG("SteamPath", MsgKey::LogUtilsFoundSteamPath, MaskPath(norm));
            return norm;
        }
        return std::nullopt;
    }();

    return s_cachedPath;
}

std::wstring QuoteCmdArg(std::wstring_view arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out;
    out.reserve(arg.size() + 8);
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

} // namespace OST::ExtractTickets
