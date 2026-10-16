#include "include/Numbers.h"

#include <charconv>
#include <system_error>

namespace OSTPlatform::Numbers {
namespace {

std::optional<std::string_view> ApplySlice(std::string_view value, TextSlice slice) noexcept {
    if (slice.offset > value.size()) return std::nullopt;
    return value.substr(slice.offset, slice.count);
}

template <typename T>
std::optional<T> ParseStrict(std::string_view value, int base) noexcept {
    if (value.empty()) return std::nullopt;

    T result = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, result, base);
    if (ec != std::errc{} || ptr != last) return std::nullopt;
    return result;
}

template <typename T>
std::optional<T> ParseStrict(std::wstring_view value, int base) noexcept {
    if (value.empty() || value.size() > 64) return std::nullopt;
    char buf[64];
    for (size_t i = 0; i < value.size(); ++i) {
        const wchar_t c = value[i];
        if (c > 127) return std::nullopt;
        buf[i] = static_cast<char>(c);
    }
    return ParseStrict<T>(std::string_view(buf, value.size()), base);
}

template <typename T>
std::optional<T> ParseStrict(const char* value, int base) noexcept {
    if (!value) return std::nullopt;
    return ParseStrict<T>(std::string_view(value), base);
}

template <typename T>
std::optional<T> ParseStrict(std::string_view value, TextSlice slice, int base) noexcept {
    const auto sliced = ApplySlice(value, slice);
    if (!sliced) return std::nullopt;
    return ParseStrict<T>(*sliced, base);
}

std::string_view StripHexPrefix(std::string_view value) noexcept {
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
    }
    return value;
}

std::wstring_view StripHexPrefix(std::wstring_view value) noexcept {
    if (value.size() >= 2 && value[0] == L'0' && (value[1] == L'x' || value[1] == L'X')) {
        value.remove_prefix(2);
    }
    return value;
}

template <typename T>
std::optional<T> ParseHexInteger(std::string_view value) noexcept {
    return ParseStrict<T>(StripHexPrefix(value), 16);
}

template <typename T>
std::optional<T> ParseHexInteger(std::wstring_view value) noexcept {
    return ParseStrict<T>(StripHexPrefix(value), 16);
}

template <typename T>
std::optional<T> ParseHexInteger(const char* value) noexcept {
    if (!value) return std::nullopt;
    return ParseHexInteger<T>(std::string_view(value));
}

template <typename T>
std::optional<T> ParseHexInteger(std::string_view value, TextSlice slice) noexcept {
    const auto sliced = ApplySlice(value, slice);
    if (!sliced) return std::nullopt;
    return ParseHexInteger<T>(*sliced);
}

} // namespace

#define DEFINE_DECIMAL_PARSER(name, type) \
    std::optional<type> name(std::string_view value) noexcept { return ParseStrict<type>(value, 10); } \
    std::optional<type> name(std::wstring_view value) noexcept { return ParseStrict<type>(value, 10); } \
    std::optional<type> name(const char* value) noexcept { return ParseStrict<type>(value, 10); } \
    std::optional<type> name(std::string_view value, TextSlice slice) noexcept { return ParseStrict<type>(value, slice, 10); }

#define DEFINE_HEX_PARSER(name, type) \
    std::optional<type> name(std::string_view value) noexcept { return ParseHexInteger<type>(value); } \
    std::optional<type> name(std::wstring_view value) noexcept { return ParseHexInteger<type>(value); } \
    std::optional<type> name(const char* value) noexcept { return ParseHexInteger<type>(value); } \
    std::optional<type> name(std::string_view value, TextSlice slice) noexcept { return ParseHexInteger<type>(value, slice); }

DEFINE_DECIMAL_PARSER(ParseUInt64, uint64_t)
DEFINE_DECIMAL_PARSER(ParseUInt32, uint32_t)
DEFINE_DECIMAL_PARSER(ParseInt64, int64_t)
DEFINE_DECIMAL_PARSER(ParseInt32, int32_t)

DEFINE_HEX_PARSER(ParseHexUInt64, uint64_t)
DEFINE_HEX_PARSER(ParseHexUInt32, uint32_t)
DEFINE_HEX_PARSER(ParseHexUInt8, uint8_t)

#undef DEFINE_DECIMAL_PARSER
#undef DEFINE_HEX_PARSER

} // namespace OSTPlatform::Numbers
