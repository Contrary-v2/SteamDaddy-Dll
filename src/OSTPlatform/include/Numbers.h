#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace OSTPlatform::Numbers {

    struct TextSlice {
        size_t offset = 0;
        size_t count = std::string_view::npos;
    };

    // Strict decimal parsing: full input must be consumed and fit the target type.
    [[nodiscard]] std::optional<uint64_t> ParseUInt64(std::string_view value) noexcept;
    [[nodiscard]] std::optional<uint64_t> ParseUInt64(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<uint64_t> ParseUInt64(const char* value) noexcept;
    [[nodiscard]] std::optional<uint64_t> ParseUInt64(std::string_view value, TextSlice slice) noexcept;

    [[nodiscard]] std::optional<uint32_t> ParseUInt32(std::string_view value) noexcept;
    [[nodiscard]] std::optional<uint32_t> ParseUInt32(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<uint32_t> ParseUInt32(const char* value) noexcept;
    [[nodiscard]] std::optional<uint32_t> ParseUInt32(std::string_view value, TextSlice slice) noexcept;

    [[nodiscard]] std::optional<int64_t> ParseInt64(std::string_view value) noexcept;
    [[nodiscard]] std::optional<int64_t> ParseInt64(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<int64_t> ParseInt64(const char* value) noexcept;
    [[nodiscard]] std::optional<int64_t> ParseInt64(std::string_view value, TextSlice slice) noexcept;

    [[nodiscard]] std::optional<int32_t> ParseInt32(std::string_view value) noexcept;
    [[nodiscard]] std::optional<int32_t> ParseInt32(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<int32_t> ParseInt32(const char* value) noexcept;
    [[nodiscard]] std::optional<int32_t> ParseInt32(std::string_view value, TextSlice slice) noexcept;

    // Strict hexadecimal parsing; optional 0x/0X prefix is accepted.
    [[nodiscard]] std::optional<uint64_t> ParseHexUInt64(std::string_view value) noexcept;
    [[nodiscard]] std::optional<uint64_t> ParseHexUInt64(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<uint64_t> ParseHexUInt64(const char* value) noexcept;
    [[nodiscard]] std::optional<uint64_t> ParseHexUInt64(std::string_view value, TextSlice slice) noexcept;

    [[nodiscard]] std::optional<uint32_t> ParseHexUInt32(std::string_view value) noexcept;
    [[nodiscard]] std::optional<uint32_t> ParseHexUInt32(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<uint32_t> ParseHexUInt32(const char* value) noexcept;
    [[nodiscard]] std::optional<uint32_t> ParseHexUInt32(std::string_view value, TextSlice slice) noexcept;

    [[nodiscard]] std::optional<uint8_t> ParseHexUInt8(std::string_view value) noexcept;
    [[nodiscard]] std::optional<uint8_t> ParseHexUInt8(std::wstring_view value) noexcept;
    [[nodiscard]] std::optional<uint8_t> ParseHexUInt8(const char* value) noexcept;
    [[nodiscard]] std::optional<uint8_t> ParseHexUInt8(std::string_view value, TextSlice slice) noexcept;

} // namespace OSTPlatform::Numbers
