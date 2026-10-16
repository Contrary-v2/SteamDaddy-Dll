#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

// String & numeric validation
[[nodiscard]] constexpr char AsciiToLower(char c) noexcept {
    const unsigned char uc = static_cast<unsigned char>(c);
    return (uc >= 'A' && uc <= 'Z') ? static_cast<char>(uc + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool EqualIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (AsciiToLower(a[i]) != AsciiToLower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr size_t FindIgnoreCase(std::string_view haystack, std::string_view needle, size_t pos = 0) noexcept {
    if (needle.empty()) return pos <= haystack.size() ? pos : std::string_view::npos;
    if (pos >= haystack.size() || needle.size() > haystack.size() - pos) {
        return std::string_view::npos;
    }
    const size_t limit = haystack.size() - needle.size();
    for (size_t i = pos; i <= limit; ++i) {
        if (EqualIgnoreCase(haystack.substr(i, needle.size()), needle)) {
            return i;
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] constexpr bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    return FindIgnoreCase(haystack, needle) != std::string_view::npos;
}

[[nodiscard]] std::string_view TrimWhitespace(std::string_view str) noexcept;
[[nodiscard]] bool IsDecimal(std::string_view value) noexcept;
[[nodiscard]] bool IsValidManifestId(std::string_view value) noexcept;
[[nodiscard]] bool IsHex64(std::string_view value) noexcept;
[[nodiscard]] std::optional<uint32_t> ParseAppId(std::string_view value) noexcept;
[[nodiscard]] std::string SanitizeComment(std::string_view text);
[[nodiscard]] std::vector<std::string> TokenizeQuoted(std::string_view line);
[[nodiscard]] std::wstring QuoteCmdArg(std::wstring_view arg);

// Hex formatting
[[nodiscard]] std::string ToHexString(std::span<const uint8_t> data);

// File I/O (INV-04 Transactional Atomic Write)
bool WriteBinaryFile(const std::filesystem::path& path, std::span<const uint8_t> data);
bool WriteTextFileAtomic(const std::filesystem::path& path, std::string_view content, bool binaryMode = true);

// Unicode & UTF-8 Path helpers
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide);
[[nodiscard]] inline std::filesystem::path Utf8Path(std::string_view utf8) {
    return std::filesystem::path(Utf8ToWide(utf8));
}
[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.native());
}

// Path & Steam Discovery
[[nodiscard]] std::string JoinPath(std::string_view base, std::string_view name);
[[nodiscard]] std::string NormalizeDir(std::string dir);
[[nodiscard]] std::optional<std::string> FindSteamInstallPath();

} // namespace OST::ExtractTickets
