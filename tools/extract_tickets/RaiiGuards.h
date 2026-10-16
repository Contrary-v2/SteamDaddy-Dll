#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

#include "steam.h"

namespace OST::ExtractTickets {

// RAII wrapper for Windows HANDLE
struct ScopedHandle {
    HANDLE handle{INVALID_HANDLE_VALUE};

    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) noexcept : handle(h) {}

    ~ScopedHandle() noexcept {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                CloseHandle(handle);
            }
            handle = other.handle;
            other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    operator HANDLE() const noexcept { return handle; }
    [[nodiscard]] bool IsValid() const noexcept { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
};

// RAII wrapper for Windows Job Object with KILL_ON_JOB_CLOSE
struct ScopedJobObject {
    HANDLE handle{nullptr};

    ScopedJobObject() noexcept {
        handle = CreateJobObjectW(nullptr, nullptr);
        if (handle) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
    }

    ~ScopedJobObject() noexcept {
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }

    ScopedJobObject(const ScopedJobObject&) = delete;
    ScopedJobObject& operator=(const ScopedJobObject&) = delete;

    ScopedJobObject(ScopedJobObject&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }

    ScopedJobObject& operator=(ScopedJobObject&& other) noexcept {
        if (this != &other) {
            if (handle) {
                CloseHandle(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    bool Assign(HANDLE hProcess) noexcept {
        if (!handle || !hProcess || hProcess == INVALID_HANDLE_VALUE) return false;
        return AssignProcessToJobObject(handle, hProcess) != FALSE;
    }

    [[nodiscard]] bool IsValid() const noexcept { return handle != nullptr; }
    [[nodiscard]] HANDLE Get() const noexcept { return handle; }
};

// RAII guard for spoofed Steam environment variables
struct ScopedSteamEnvGuard {
    ~ScopedSteamEnvGuard() noexcept {
        SetEnvironmentVariableA("SteamAppId", nullptr);
        SetEnvironmentVariableA("SteamGameId", nullptr);
        SetEnvironmentVariableA("SteamOverlayGameId", nullptr);
        SetEnvironmentVariableA("OST_TOOL_EXTRACTION", nullptr);
    }
};

// RAII wrapper for MapViewOfFile memory pointer
struct ScopedFileMappingView {
    const void* address{nullptr};

    ScopedFileMappingView() = default;
    explicit ScopedFileMappingView(const void* addr) noexcept : address(addr) {}

    ~ScopedFileMappingView() noexcept {
        if (address) {
            UnmapViewOfFile(address);
        }
    }

    ScopedFileMappingView(const ScopedFileMappingView&) = delete;
    ScopedFileMappingView& operator=(const ScopedFileMappingView&) = delete;

    ScopedFileMappingView(ScopedFileMappingView&& other) noexcept : address(other.address) {
        other.address = nullptr;
    }

    ScopedFileMappingView& operator=(ScopedFileMappingView&& other) noexcept {
        if (this != &other) {
            if (address) {
                UnmapViewOfFile(address);
            }
            address = other.address;
            other.address = nullptr;
        }
        return *this;
    }

    template <typename T>
    [[nodiscard]] const T* As() const noexcept { return static_cast<const T*>(address); }

    explicit operator bool() const noexcept { return address != nullptr; }
};

// RAII guard for Steam client session, loaded module, and process environment
struct SteamSessionGuard {
    ISteamClient* client{nullptr};
    HSteamPipe pipe{0};
    HSteamUser user{0};
    HMODULE module{nullptr};
    bool ownsEnv{true};

    SteamSessionGuard() = default;
    SteamSessionGuard(ISteamClient* c, HSteamPipe p, HSteamUser u, HMODULE m) noexcept
        : client(c), pipe(p), user(u), module(m), ownsEnv(true) {}

    ~SteamSessionGuard() noexcept {
        Reset();
    }

    SteamSessionGuard(const SteamSessionGuard&) = delete;
    SteamSessionGuard& operator=(const SteamSessionGuard&) = delete;

    SteamSessionGuard(SteamSessionGuard&& other) noexcept
        : client(other.client), pipe(other.pipe), user(other.user), module(other.module), ownsEnv(other.ownsEnv) {
        other.client = nullptr;
        other.pipe = 0;
        other.user = 0;
        other.module = nullptr;
        other.ownsEnv = false;
    }

    SteamSessionGuard& operator=(SteamSessionGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            client = other.client;
            pipe = other.pipe;
            user = other.user;
            module = other.module;
            ownsEnv = other.ownsEnv;
            other.client = nullptr;
            other.pipe = 0;
            other.user = 0;
            other.module = nullptr;
            other.ownsEnv = false;
        }
        return *this;
    }

    void Reset() noexcept {
        if (client) {
            if (pipe && user) {
                client->ReleaseUser(pipe, user);
                user = 0;
            }
            if (pipe) {
                client->BReleaseSteamPipe(pipe);
                pipe = 0;
            }
            client = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
            SetDllDirectoryW(nullptr);
        }
        if (ownsEnv) {
            // Clear spoofed Steam environment variables
            SetEnvironmentVariableA("SteamAppId", nullptr);
            SetEnvironmentVariableA("SteamGameId", nullptr);
            SetEnvironmentVariableA("SteamOverlayGameId", nullptr);
            SetEnvironmentVariableA("OST_TOOL_EXTRACTION", nullptr);
            ownsEnv = false;
        }
    }
};

// RAII guard for Windows console code page restoration
struct ConsoleCodePageGuard {
    UINT oldOutputCP{0};
    UINT oldCP{0};

    ConsoleCodePageGuard() noexcept {
        oldOutputCP = GetConsoleOutputCP();
        oldCP = GetConsoleCP();
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    ~ConsoleCodePageGuard() noexcept {
        if (oldOutputCP != 0) {
            SetConsoleOutputCP(oldOutputCP);
        }
        if (oldCP != 0) {
            SetConsoleCP(oldCP);
        }
    }

    ConsoleCodePageGuard(const ConsoleCodePageGuard&) = delete;
    ConsoleCodePageGuard& operator=(const ConsoleCodePageGuard&) = delete;
};

} // namespace OST::ExtractTickets
