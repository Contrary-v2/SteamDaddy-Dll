#pragma once

#include <cstdint>
#include <filesystem>

namespace OSTPlatform::RemoteProcess {

    enum class Architecture {
        Unknown,
        X86,
        X64,
    };

    enum class InjectStatus {
        Ok,
        OpenProcessFailed,
        UnknownArchitecture,
        AllocFailed,
        WriteFailed,
        ResolveLoadLibraryFailed,
        CreateThreadFailed,
        WaitFailed,
        RemoteLoadFailed,
    };

    [[nodiscard]] const char* ToString(Architecture architecture) noexcept;
    [[nodiscard]] const char* ToString(InjectStatus status) noexcept;
    [[nodiscard]] Architecture GetArchitecture(uint32_t pid);
    [[nodiscard]] InjectStatus InjectLibrary(uint32_t pid, const std::filesystem::path& libraryPath);

} // namespace OSTPlatform::RemoteProcess
