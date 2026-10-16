#pragma once

#include "include/DynamicLibrary.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace OSTPlatform::Memory {

    struct ModuleImage {
        uint8_t* base = nullptr;
        size_t size = 0;
    };

    struct ModuleSection {
        uint8_t* base = nullptr;
        size_t size = 0;
    };

    [[nodiscard]] std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module);
    [[nodiscard]] std::optional<ModuleSection> GetModuleSection(DynamicLibrary::ModuleHandle module, std::string_view sectionName);
    bool WriteExecutableByte(void* target, uint8_t value);
    [[nodiscard]] bool WriteExecutableBytes(void* target, std::span<const uint8_t> bytes);

} // namespace OSTPlatform::Memory
