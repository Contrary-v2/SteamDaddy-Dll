#pragma once

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>

namespace PatternLoader {

    // Load metadata before installing hooks for a module.
    bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component);

    // Resolve by RVA first, then fall back to signature scanning.
    [[nodiscard]] void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName);

    // If a cached TOML lacked a requested function entry, refresh that module's
    // exact-SHA cache after initialization.
    void RefreshMissingMetadata();

    // Report unresolved functions after all hooks have been installed.
    void ReportMissingFunctions();

} // namespace PatternLoader
