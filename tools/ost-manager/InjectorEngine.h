#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OST::Manager {

enum class InjectResult {
    Success,
    AlreadyInjected,
    ProcessNotFound,
    AccessDenied,
    PayloadNotFound,
    IncompatibleArchitecture,
    InjectionFailed
};

class InjectorEngine {
public:
    // Core injection into a target process ID
    [[nodiscard]] static InjectResult InjectPayload(DWORD pid,
                                                    const std::filesystem::path& dllPath,
                                                    std::wstring& outDetail);

    // One-click / silent injection into running Steam instance(s)
    [[nodiscard]] static InjectResult InjectToRunningSteam(const std::filesystem::path& dllPath,
                                                           DWORD& outTargetPid,
                                                           std::wstring& outDetail);

    // Snapshot candidate Steam PIDs sorted by priority
    [[nodiscard]] static std::vector<DWORD> FindSteamPids();

    // Check if module is loaded in process
    [[nodiscard]] static bool IsModuleLoaded(DWORD pid, std::wstring_view moduleName);

    // Watcher mode: background headless loop waiting for Steam to spawn and auto-injecting
    static int RunWatcherLoop(const std::filesystem::path& dllPath);

    // Check if background watcher daemon is already running
    [[nodiscard]] static bool IsWatcherDaemonRunning();

    // Signal currently running background watcher daemon to stop
    [[nodiscard]] static bool StopWatcherDaemon();

    // Start background watcher daemon process detached
    [[nodiscard]] static bool StartWatcherDaemonProcess(const std::filesystem::path& dllPath);

    // Check if auto-start on logon is configured in HKCU Run
    [[nodiscard]] static bool IsAutoStartConfigured();

    // Configure or unregister auto-start on logon in HKCU Run (No UAC required)
    [[nodiscard]] static bool SetAutoStart(bool enable, const std::filesystem::path& dllPath = {});
};

} // namespace OST::Manager
