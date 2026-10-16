#include "InjectorEngine.h"
#include "IfeoEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <tlhelp32.h>

namespace OST::Manager {

namespace {

constexpr wchar_t kWatcherMutexName[] = L"Local\\OpenSteamTool_AutoInject_Watcher";
constexpr wchar_t kWatcherStopEventName[] = L"Local\\OpenSteamTool_Watcher_Stop_Event";
constexpr wchar_t kAutoStartSubKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoStartValueName[] = L"ost_watcher";
constexpr wchar_t kLegacyAutoStartValueName[] = L"OpenSteamTool_AutoInject";

struct ScopedHKey {
    HKEY h = nullptr;
    ScopedHKey() = default;
    explicit ScopedHKey(HKEY handle) noexcept : h(handle) {}
    ~ScopedHKey() noexcept { if (h) ::RegCloseKey(h); }
    ScopedHKey(const ScopedHKey&) = delete;
    ScopedHKey& operator=(const ScopedHKey&) = delete;
    ScopedHKey(ScopedHKey&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedHKey& operator=(ScopedHKey&& o) noexcept {
        if (this != &o) {
            if (h) ::RegCloseKey(h);
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    [[nodiscard]] HKEY Get() const noexcept { return h; }
    [[nodiscard]] HKEY* AddressOf() noexcept { return &h; }
    explicit operator bool() const noexcept { return h != nullptr; }
};

struct ScopedHandle {
    HANDLE h = nullptr;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : h((handle == INVALID_HANDLE_VALUE) ? nullptr : handle) {}
    ~ScopedHandle() noexcept { if (h) ::CloseHandle(h); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& o) noexcept {
        if (this != &o) {
            if (h) ::CloseHandle(h);
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    operator HANDLE() const noexcept { return h; }
    explicit operator bool() const noexcept { return h != nullptr; }
    [[nodiscard]] HANDLE Get() const noexcept { return h; }
    void Reset(HANDLE handle = nullptr) noexcept {
        if (h) ::CloseHandle(h);
        h = (handle == INVALID_HANDLE_VALUE) ? nullptr : handle;
    }
};

template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F&& func) : func_(std::forward<F>(func)) {}
    ~ScopeGuard() { if (active_) func_(); }
    void Dismiss() noexcept { active_ = false; }
private:
    F func_;
    bool active_ = true;
};

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                  b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::atomic<bool> g_stopRequested{ false };
std::atomic<HANDLE> g_stopEventHandle{ nullptr };

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) noexcept {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true, std::memory_order_release);
            if (HANDLE hEvent = g_stopEventHandle.load(std::memory_order_acquire)) {
                ::SetEvent(hEvent);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

} // namespace

bool InjectorEngine::IsModuleLoaded(DWORD pid, std::wstring_view moduleName) {
    if (pid == 0 || moduleName.empty()) return false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        ScopedHandle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snap) {
            const DWORD gle = ::GetLastError();
            if ((gle == ERROR_BAD_LENGTH || gle == ERROR_PARTIAL_COPY) && attempt + 1 < 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            return false;
        }

        MODULEENTRY32W me{ sizeof(me) };
        if (::Module32FirstW(snap.Get(), &me)) {
            do {
                if (EqualsIgnoreCase(moduleName, me.szModule)) {
                    return true;
                }
            } while (::Module32NextW(snap.Get(), &me));
        }
        break;
    }
    return false;
}

std::vector<DWORD> InjectorEngine::FindSteamPids() {
    std::vector<DWORD> pids;
    ScopedHandle snap(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return pids;

    DWORD currentSessionId = 0;
    const bool hasSession = ::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSessionId);

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (::Process32FirstW(snap.Get(), &pe)) {
        do {
            if (EqualsIgnoreCase(L"steam.exe", pe.szExeFile)) {
                if (hasSession) {
                    DWORD targetSessionId = 0;
                    if (::ProcessIdToSessionId(pe.th32ProcessID, &targetSessionId) &&
                        targetSessionId != currentSessionId) {
                        continue; // Skip processes from other terminal sessions
                    }
                }
                pids.push_back(pe.th32ProcessID);
            }
        } while (::Process32NextW(snap.Get(), &pe));
    }

    if (pids.size() <= 1) {
        return pids;
    }

    // Prioritize candidate PIDs: 64-bit native, essential modules loaded, newest creation time
    struct ScoredPid {
        DWORD pid = 0;
        int score = 0;
        ULONGLONG creationTime = 0;
    };

    std::vector<ScoredPid> scored;
    scored.reserve(pids.size());

    for (DWORD pid : pids) {
        int score = 0;
        ULONGLONG createTime = 0;

        ScopedHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (h) {
            DWORD exitCode = 0;
            if (!::GetExitCodeProcess(h.Get(), &exitCode) || exitCode != STILL_ACTIVE) {
                score = -100;
            } else {
                BOOL isWow64 = FALSE;
                if (::IsWow64Process(h.Get(), &isWow64) && !isWow64) {
                    score += 10;
                }

                FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
                if (::GetProcessTimes(h.Get(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULARGE_INTEGER uli{};
                    uli.LowPart = ftCreate.dwLowDateTime;
                    uli.HighPart = ftCreate.dwHighDateTime;
                    createTime = uli.QuadPart;
                }
            }
        }

        if (score >= 0) {
            if (IsModuleLoaded(pid, L"steamclient64.dll")) score += 100;
            if (IsModuleLoaded(pid, L"steamui.dll"))       score += 50;
            if (IsModuleLoaded(pid, L"tier0_s64.dll"))     score += 20;
        }

        scored.push_back({ pid, score, createTime });
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredPid& a, const ScoredPid& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.creationTime > b.creationTime;
    });

    for (size_t i = 0; i < pids.size(); ++i) {
        pids[i] = scored[i].pid;
    }
    return pids;
}

InjectResult InjectorEngine::InjectPayload(DWORD pid,
                                          const std::filesystem::path& dllPath,
                                          std::wstring& outDetail) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dllPath, ec) || ec) {
        outDetail = L"Payload DLL file not found: " + dllPath.wstring();
        return InjectResult::PayloadNotFound;
    }

    const std::wstring payloadName = dllPath.filename().wstring();
    if (IsModuleLoaded(pid, payloadName)) {
        outDetail = L"OpenSteamTool is already injected in process (PID " + std::to_wstring(pid) + L").";
        return InjectResult::AlreadyInjected;
    }

    constexpr DWORD kAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

    ScopedHandle hProcess(::OpenProcess(kAccess, FALSE, pid));
    if (!hProcess) {
        const DWORD gle = ::GetLastError();
        outDetail = L"OpenProcess failed (PID=" + std::to_wstring(pid) + L", Error=" + std::to_wstring(gle) + L")";
        return InjectResult::AccessDenied;
    }

    BOOL isWow64 = FALSE;
    if (::IsWow64Process(hProcess.Get(), &isWow64) && isWow64) {
        outDetail = L"Target process is 32-bit WOW64, but OpenSteamTool payload is 64-bit.";
        return InjectResult::IncompatibleArchitecture;
    }

    const std::wstring nativeDllPath = dllPath.wstring();
    const SIZE_T byteCount = (nativeDllPath.size() + 1) * sizeof(wchar_t);

    void* remoteMem = ::VirtualAllocEx(hProcess.Get(), nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        const DWORD gle = ::GetLastError();
        outDetail = L"VirtualAllocEx failed (Error=" + std::to_wstring(gle) + L")";
        return InjectResult::InjectionFailed;
    }

    ScopeGuard freeRemoteGuard([&] {
        ::VirtualFreeEx(hProcess.Get(), remoteMem, 0, MEM_RELEASE);
    });

    SIZE_T bytesWritten = 0;
    if (!::WriteProcessMemory(hProcess.Get(), remoteMem, nativeDllPath.c_str(), byteCount, &bytesWritten) ||
        bytesWritten != byteCount) {
        const DWORD gle = ::GetLastError();
        outDetail = L"WriteProcessMemory failed (Error=" + std::to_wstring(gle) + L")";
        return InjectResult::InjectionFailed;
    }

    HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    FARPROC pfnLoadLibraryW = hKernel32 ? ::GetProcAddress(hKernel32, "LoadLibraryW") : nullptr;
    if (!pfnLoadLibraryW) {
        outDetail = L"Failed to resolve LoadLibraryW from kernel32.dll";
        return InjectResult::InjectionFailed;
    }

    ScopedHandle hThread(::CreateRemoteThread(hProcess.Get(), nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pfnLoadLibraryW), remoteMem, 0, nullptr));
    if (!hThread) {
        const DWORD gle = ::GetLastError();
        outDetail = L"CreateRemoteThread failed (Error=" + std::to_wstring(gle) + L")";
        return InjectResult::InjectionFailed;
    }

    const DWORD waitRes = ::WaitForSingleObject(hThread.Get(), 15000);
    if (waitRes != WAIT_OBJECT_0) {
        // Do NOT free remote memory on timeout to avoid UAF
        freeRemoteGuard.Dismiss();
        outDetail = L"Remote thread execution timed out.";
        return InjectResult::InjectionFailed;
    }

    // Verify whether target process survived and module loaded
    DWORD exitCode = 0;
    if (::GetExitCodeProcess(hProcess.Get(), &exitCode) && exitCode != STILL_ACTIVE) {
        outDetail = L"Target process terminated during injection.";
        return InjectResult::InjectionFailed;
    }

    bool loaded = IsModuleLoaded(pid, payloadName);
    for (int retry = 0; !loaded && retry < 3; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        loaded = IsModuleLoaded(pid, payloadName);
    }

    if (!loaded) {
        outDetail = L"LoadLibraryW returned but module was not detected in target PEB.";
        return InjectResult::InjectionFailed;
    }

    outDetail = L"Successfully injected OpenSteamTool into Steam (PID: " + std::to_wstring(pid) + L").";
    return InjectResult::Success;
}

InjectResult InjectorEngine::InjectToRunningSteam(const std::filesystem::path& dllPath,
                                                 DWORD& outTargetPid,
                                                 std::wstring& outDetail) {
    outTargetPid = 0;
    const auto pids = FindSteamPids();
    if (pids.empty()) {
        outDetail = L"Steam process is not currently running.";
        return InjectResult::ProcessNotFound;
    }

    InjectResult lastResult = InjectResult::InjectionFailed;
    for (DWORD pid : pids) {
        lastResult = InjectPayload(pid, dllPath, outDetail);
        if (lastResult == InjectResult::Success || lastResult == InjectResult::AlreadyInjected) {
            outTargetPid = pid;
            return lastResult;
        }
    }

    return lastResult;
}

bool InjectorEngine::IsWatcherDaemonRunning() {
    ScopedHandle h(::OpenMutexW(SYNCHRONIZE, FALSE, kWatcherMutexName));
    return static_cast<bool>(h);
}

bool InjectorEngine::StopWatcherDaemon() {
    ScopedHandle hEvent(::OpenEventW(EVENT_MODIFY_STATE, FALSE, kWatcherStopEventName));
    if (hEvent) {
        ::SetEvent(hEvent.Get());
        return true;
    }
    return false;
}

bool InjectorEngine::StartWatcherDaemonProcess(const std::filesystem::path& dllPath) {
    if (IsWatcherDaemonRunning()) {
        return true;
    }

    const std::wstring selfExe = IfeoEngine::GetSelfExePath().wstring();
    std::wstring cmdLine = L"\"" + selfExe + L"\" /watch";
    if (!dllPath.empty()) {
        cmdLine += L" \"" + dllPath.wstring() + L"\"";
    }

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(
        selfExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi
    );

    if (ok) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return true;
    }
    return false;
}

int InjectorEngine::RunWatcherLoop(const std::filesystem::path& dllPath) {
    // 1. Single instance guard
    ScopedHandle hMutex(::CreateMutexW(nullptr, FALSE, kWatcherMutexName));
    if (!hMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // Daemon is already running
    }

    // 2. Stop event
    ScopedHandle hStopEvent(::CreateEventW(nullptr, TRUE, FALSE, kWatcherStopEventName));
    g_stopEventHandle.store(hStopEvent.Get(), std::memory_order_release);
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    const std::filesystem::path effectiveDll = dllPath.empty()
        ? IfeoEngine::GetDefaultPayloadDllPath()
        : dllPath;

    DWORD lastInjectedPid = 0;

    while (!g_stopRequested.load(std::memory_order_acquire)) {
        const auto pids = FindSteamPids();
        if (!pids.empty()) {
            const DWORD candidatePid = pids.front();
            if (candidatePid != lastInjectedPid) {
                const std::wstring payloadName = effectiveDll.filename().wstring();
                if (!IsModuleLoaded(candidatePid, payloadName)) {
                    std::wstring detail;
                    const auto res = InjectPayload(candidatePid, effectiveDll, detail);
                    if (res == InjectResult::Success || res == InjectResult::AlreadyInjected) {
                        lastInjectedPid = candidatePid;
                    }
                } else {
                    lastInjectedPid = candidatePid;
                }
            }
        } else {
            lastInjectedPid = 0;
        }

        // Interruptible wait 250ms
        const DWORD waitRes = ::WaitForSingleObject(hStopEvent.Get(), 250);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        }
    }

    return 0;
}

bool InjectorEngine::IsAutoStartConfigured() {
    HKEY hKey = nullptr;
    const LSTATUS status = ::RegOpenKeyExW(
        HKEY_CURRENT_USER, kAutoStartSubKey, 0, KEY_QUERY_VALUE, &hKey
    );
    if (status != ERROR_SUCCESS) {
        return false;
    }
    ScopedHKey scoped(hKey);

    DWORD type = 0;
    if (::RegQueryValueExW(scoped.Get(), kAutoStartValueName, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS) {
        return true;
    }
    // Also check legacy value name for backward compatibility
    return ::RegQueryValueExW(scoped.Get(), kLegacyAutoStartValueName, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS;
}

bool InjectorEngine::SetAutoStart(bool enable, const std::filesystem::path& dllPath) {
    if (enable) {
        HKEY hKey = nullptr;
        const LSTATUS status = ::RegCreateKeyExW(
            HKEY_CURRENT_USER, kAutoStartSubKey, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr
        );
        if (status != ERROR_SUCCESS) {
            return false;
        }
        ScopedHKey scoped(hKey);

        const std::wstring selfExe = IfeoEngine::GetSelfExePath().wstring();
        std::wstring cmdLine = L"\"" + selfExe + L"\" /watch";
        if (!dllPath.empty() && dllPath != IfeoEngine::GetDefaultPayloadDllPath()) {
            cmdLine += L" \"" + dllPath.wstring() + L"\"";
        }

        const DWORD byteCount = static_cast<DWORD>((cmdLine.size() + 1) * sizeof(wchar_t));
        const LSTATUS setStatus = ::RegSetValueExW(
            scoped.Get(), kAutoStartValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmdLine.c_str()), byteCount
        );

        // Seamlessly purge legacy value name if present
        ::RegDeleteValueW(scoped.Get(), kLegacyAutoStartValueName);

        return (setStatus == ERROR_SUCCESS);
    } else {
        HKEY hKey = nullptr;
        const LSTATUS status = ::RegOpenKeyExW(
            HKEY_CURRENT_USER, kAutoStartSubKey, 0, KEY_SET_VALUE, &hKey
        );
        if (status != ERROR_SUCCESS) {
            return true; // Key already does not exist or not accessible
        }
        ScopedHKey scoped(hKey);
        const LSTATUS delStatus = ::RegDeleteValueW(scoped.Get(), kAutoStartValueName);
        ::RegDeleteValueW(scoped.Get(), kLegacyAutoStartValueName);
        return (delStatus == ERROR_SUCCESS || delStatus == ERROR_FILE_NOT_FOUND);
    }
}

} // namespace OST::Manager
