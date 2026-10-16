#pragma once

#include <filesystem>
#include <span>
#include <string>

namespace OSTPlatform::Hash {

    [[nodiscard]] std::string Sha256OfFile(const std::filesystem::path& path);
    [[nodiscard]] std::string Sha256(std::span<const uint8_t> data);

} // namespace OSTPlatform::Hash
