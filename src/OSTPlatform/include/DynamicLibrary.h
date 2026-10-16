#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace OSTPlatform::DynamicLibrary {

    using ModuleHandle = void*;

    [[nodiscard]] ModuleHandle Load(const std::filesystem::path& path);
    bool Unload(ModuleHandle module);
    [[nodiscard]] ModuleHandle GetLoaded(std::string_view moduleName);
    [[nodiscard]] void* GetSymbol(ModuleHandle module, const char* symbolName);
    [[nodiscard]] void* GetSymbol(ModuleHandle module, uint16_t ordinal);
    [[nodiscard]] uint32_t GetLastErrorCode() noexcept;

    [[nodiscard]] std::string GetCurrentDirectoryPath();
    [[nodiscard]] std::string GetSystemDirectoryPath();
    [[nodiscard]] std::filesystem::path GetModuleDirectory(ModuleHandle module);
    [[nodiscard]] std::filesystem::path GetMainExecutablePath();

} // namespace OSTPlatform::DynamicLibrary
