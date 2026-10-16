#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace OSTPlatform::Encoding {

    [[nodiscard]] std::string WideToUtf8(std::wstring_view value);
    [[nodiscard]] std::wstring Utf8ToWide(std::string_view value);

    [[nodiscard]] inline std::filesystem::path PathFromUtf8(std::string_view u8str) {
#if defined(_WIN32)
        return std::filesystem::path(Utf8ToWide(u8str));
#else
        return std::filesystem::path(u8str);
#endif
    }

    [[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path& p) {
#if defined(_WIN32)
        return WideToUtf8(p.native());
#else
        return p.string();
#endif
    }

} // namespace OSTPlatform::Encoding
