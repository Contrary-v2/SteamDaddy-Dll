#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <system_error>

namespace OSTPlatform {

/**
 * @brief Unified Steam installation directory locator with a 5-tier fast short-circuit pipeline.
 *
 * Designed to address TD-04:
 * Tier 0: Explicit caller override or OST_STEAM_PATH environment variable.
 * Tier 1: Real-time running process detection via EnumProcesses (<2ms, memory-only).
 * Tier 2: Portable / local directory detection (adjacent or parent to current executable).
 * Tier 3: Complete registry fallback (HKCU SteamExe/SteamPath, HKLM WOW6432Node/native).
 * Tier 4: Fast fixed-drive common installation probe (strictly non-recursive).
 */
class SteamLocator {
public:
    [[nodiscard]] static std::optional<std::filesystem::path> ResolvePath(
        std::optional<std::filesystem::path> explicitOverride = std::nullopt) noexcept {

        // -----------------------------------------------------------------
        // Tier 0: Explicit Override & Environment Variable
        // -----------------------------------------------------------------
        if (explicitOverride.has_value() && !explicitOverride->empty()) {
            if (auto dir = NormalizeAndVerifyDir(*explicitOverride)) {
                return dir;
            }
        }

        wchar_t envBuf[MAX_PATH];
        DWORD envLen = ::GetEnvironmentVariableW(L"OST_STEAM_PATH", envBuf, static_cast<DWORD>(std::size(envBuf)));
        if (envLen > 0) {
            if (envLen < static_cast<DWORD>(std::size(envBuf))) {
                if (auto dir = NormalizeAndVerifyDir(std::filesystem::path(std::wstring_view(envBuf, envLen)))) {
                    return dir;
                }
            } else {
                std::wstring dynBuf(envLen, L'\0');
                envLen = ::GetEnvironmentVariableW(L"OST_STEAM_PATH", dynBuf.data(), envLen);
                if (envLen > 0 && envLen <= dynBuf.size()) {
                    dynBuf.resize(envLen);
                    if (auto dir = NormalizeAndVerifyDir(std::filesystem::path(dynBuf))) {
                        return dir;
                    }
                }
            }
        }

        // -----------------------------------------------------------------
        // Tier 1: Running Process Inspection (EnumProcesses + QueryFullProcessImageNameW)
        // Memory-level inspection, zero disk searching, ultra-fast (<2ms)
        // -----------------------------------------------------------------
        if (auto dir = ProbeRunningSteamProcess()) {
            return dir;
        }

        // -----------------------------------------------------------------
        // Tier 2: Portable / Local Directory Probe
        // Detects steam.exe and steamclient64.dll adjacent or in parent folder
        // -----------------------------------------------------------------
        if (auto dir = ProbePortableDirectory()) {
            return dir;
        }

        // -----------------------------------------------------------------
        // Tier 3: Comprehensive Windows Registry Fallback Chain
        // -----------------------------------------------------------------
        if (auto dir = ProbeRegistryPaths()) {
            return dir;
        }

        // -----------------------------------------------------------------
        // Tier 4: Common Fixed Drive Paths (Strictly Non-Recursive)
        // -----------------------------------------------------------------
        if (auto dir = ProbeCommonFixedDrivePaths()) {
            return dir;
        }

        return std::nullopt;
    }

private:
    struct ScopedHandle {
        HANDLE handle = nullptr;
        explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle(h) {}
        ~ScopedHandle() noexcept {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                ::CloseHandle(handle);
            }
        }
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) {
            other.handle = nullptr;
        }
        ScopedHandle& operator=(ScopedHandle&& other) noexcept {
            if (this != &other) {
                if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(handle);
                }
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }
        [[nodiscard]] bool IsValid() const noexcept {
            return handle != nullptr && handle != INVALID_HANDLE_VALUE;
        }
    };

    [[nodiscard]] static std::optional<std::filesystem::path> NormalizeAndVerifyDir(
        const std::filesystem::path& candidate) noexcept {
        std::error_code ec;
        if (candidate.empty()) return std::nullopt;

        // If candidate directly points to steam.exe or another file
        if (std::filesystem::is_regular_file(candidate, ec)) {
            const auto& filename = candidate.filename().native();
            if (_wcsicmp(filename.c_str(), L"steam.exe") == 0) {
                const auto parent = candidate.parent_path();
                if (std::filesystem::is_regular_file(parent / L"steam.exe", ec)) {
                    return parent.lexically_normal();
                }
            }
            return std::nullopt;
        }

        // If candidate is a directory
        if (std::filesystem::is_directory(candidate, ec)) {
            if (std::filesystem::is_regular_file(candidate / L"steam.exe", ec)) {
                return candidate.lexically_normal();
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] static std::optional<std::filesystem::path> ProbeRunningSteamProcess() noexcept {
        DWORD processIds[2048];
        DWORD bytesReturned = 0;
        if (!::EnumProcesses(processIds, sizeof(processIds), &bytesReturned)) {
            return std::nullopt;
        }

        const DWORD processCount = bytesReturned / sizeof(DWORD);
        wchar_t imagePath[MAX_PATH];

        for (DWORD i = 0; i < processCount; ++i) {
            const DWORD pid = processIds[i];
            if (pid == 0) continue;

            ScopedHandle hProcess(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            if (!hProcess.IsValid()) continue;

            DWORD size = static_cast<DWORD>(std::size(imagePath));
            if (::QueryFullProcessImageNameW(hProcess.handle, 0, imagePath, &size) && size > 0) {
                const std::wstring_view pathView(imagePath, size);
                const auto lastSlash = pathView.find_last_of(L"\\/");
                const std::wstring_view exeName = (lastSlash != std::wstring_view::npos)
                    ? pathView.substr(lastSlash + 1)
                    : pathView;

                if (exeName.size() == 9 && _wcsnicmp(exeName.data(), L"steam.exe", 9) == 0) {
                    const std::filesystem::path fullPath(pathView);
                    const auto dir = fullPath.parent_path();
                    if (auto verified = NormalizeAndVerifyDir(dir)) {
                        return verified;
                    }
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<std::filesystem::path> ProbePortableDirectory() noexcept {
        wchar_t exePath[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (len == 0 || len >= static_cast<DWORD>(std::size(exePath))) {
            return std::nullopt;
        }

        const std::filesystem::path selfPath(std::wstring_view(exePath, len));
        const auto selfDir = selfPath.parent_path();
        std::error_code ec;

        // 1. Same directory probe
        if (std::filesystem::is_regular_file(selfDir / L"steam.exe", ec) &&
            std::filesystem::is_regular_file(selfDir / L"steamclient64.dll", ec)) {
            return selfDir.lexically_normal();
        }

        // 2. Parent directory probe
        const auto parentDir = selfDir.parent_path();
        if (!parentDir.empty() &&
            std::filesystem::is_regular_file(parentDir / L"steam.exe", ec) &&
            std::filesystem::is_regular_file(parentDir / L"steamclient64.dll", ec)) {
            return parentDir.lexically_normal();
        }

        return std::nullopt;
    }

    [[nodiscard]] static std::optional<std::wstring> ReadRegistryString(
        HKEY rootKey, const wchar_t* subKey, const wchar_t* valueName) noexcept {
        wchar_t buffer[1024];
        DWORD byteSize = sizeof(buffer);
        const LSTATUS status = ::RegGetValueW(
            rootKey,
            subKey,
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            buffer,
            &byteSize
        );
        if (status == ERROR_SUCCESS && byteSize >= sizeof(wchar_t)) {
            DWORD charLen = (byteSize / sizeof(wchar_t));
            while (charLen > 0 && (buffer[charLen - 1] == L'\0' || buffer[charLen - 1] == L' ')) {
                --charLen;
            }
            if (charLen > 0) {
                return std::wstring(buffer, charLen);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<std::filesystem::path> ProbeRegistryPaths() noexcept {
        constexpr const wchar_t* kSteamSubKey = L"SOFTWARE\\Valve\\Steam";
        constexpr const wchar_t* kSteamWowSubKey = L"SOFTWARE\\WOW6432Node\\Valve\\Steam";

        // 1. HKCU -> SteamExe
        if (const auto val = ReadRegistryString(HKEY_CURRENT_USER, kSteamSubKey, L"SteamExe")) {
            if (auto dir = NormalizeAndVerifyDir(std::filesystem::path(*val))) {
                return dir;
            }
        }

        // 2. HKCU -> SteamPath
        if (const auto val = ReadRegistryString(HKEY_CURRENT_USER, kSteamSubKey, L"SteamPath")) {
            if (auto dir = NormalizeAndVerifyDir(std::filesystem::path(*val))) {
                return dir;
            }
        }

        // 3. HKLM WOW6432Node -> InstallPath
        if (const auto val = ReadRegistryString(HKEY_LOCAL_MACHINE, kSteamWowSubKey, L"InstallPath")) {
            if (auto dir = NormalizeAndVerifyDir(std::filesystem::path(*val))) {
                return dir;
            }
        }

        // 4. HKLM Native -> InstallPath
        if (const auto val = ReadRegistryString(HKEY_LOCAL_MACHINE, kSteamSubKey, L"InstallPath")) {
            if (auto dir = NormalizeAndVerifyDir(std::filesystem::path(*val))) {
                return dir;
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] static std::optional<std::filesystem::path> ProbeCommonFixedDrivePaths() noexcept {
        constexpr const wchar_t* kCommonSuffixes[] = {
            L"Program Files (x86)\\Steam",
            L"Program Files\\Steam",
            L"Steam"
        };

        const auto checkDrive = [&](const wchar_t* driveRootStr) -> std::optional<std::filesystem::path> {
            const std::filesystem::path driveRoot(driveRootStr);
            for (const auto* suffix : kCommonSuffixes) {
                const std::filesystem::path candidate = driveRoot / suffix;
                if (auto dir = NormalizeAndVerifyDir(candidate)) {
                    return dir;
                }
            }
            return std::nullopt;
        };

        wchar_t driveStrings[512] = { 0 };
        const DWORD len = ::GetLogicalDriveStringsW(static_cast<DWORD>(std::size(driveStrings) - 1), driveStrings);

        if (len > 0 && len < static_cast<DWORD>(std::size(driveStrings))) {
            const wchar_t* cur = driveStrings;
            while (*cur) {
                if (::GetDriveTypeW(cur) == DRIVE_FIXED) {
                    if (auto dir = checkDrive(cur)) {
                        return dir;
                    }
                }
                cur += wcslen(cur) + 1;
            }
        } else {
            for (const auto* fallbackDrive : { L"C:\\", L"D:\\", L"E:\\", L"F:\\" }) {
                if (auto dir = checkDrive(fallbackDrive)) {
                    return dir;
                }
            }
        }

        return std::nullopt;
    }
};

} // namespace OSTPlatform
