#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::filesystem::path GetSelfDir() {
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    while (len >= buf.size() - 1 && buf.size() < 32768) {
        buf.resize(buf.size() * 2, L'\0');
        len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    }
    return (len > 0) ? std::filesystem::path(buf.data()).parent_path() : std::filesystem::current_path();
}

bool InjectDllIntoProcess(HANDLE hProcess, const std::filesystem::path& dllPath) {
    const std::wstring pathStr = dllPath.wstring();
    const SIZE_T bytes = (pathStr.size() + 1) * sizeof(wchar_t);

    void* remoteMem = ::VirtualAllocEx(hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        return false;
    }

    SIZE_T written = 0;
    if (!::WriteProcessMemory(hProcess, remoteMem, pathStr.c_str(), bytes, &written) || written != bytes) {
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    auto pfnLoadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(hKernel32, "LoadLibraryW"));
    if (!pfnLoadLibraryW) {
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = ::CreateRemoteThread(hProcess, nullptr, 0, pfnLoadLibraryW, remoteMem, 0, nullptr);
    if (!hThread) {
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitRes = ::WaitForSingleObject(hThread, 8000);
    ::CloseHandle(hThread);
    if (waitRes == WAIT_OBJECT_0) {
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    }
    return (waitRes == WAIT_OBJECT_0);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::filesystem::path selfDir = GetSelfDir();

    // Security Invariant 1: Strictly bind target executable to steam.exe.
    // In Root Mode, this is the co-located steam.exe in the same folder.
    // Fallback: If not found in selfDir (e.g. invoked via IFEO forwarding), verify __wargv[1] is steam.exe.
    std::filesystem::path targetExe = selfDir / L"steam.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(targetExe, ec)) {
        if (__argc >= 2) {
            const std::filesystem::path argvExe = __wargv[1];
            if (_wcsicmp(argvExe.filename().c_str(), L"steam.exe") == 0 &&
                std::filesystem::is_regular_file(argvExe, ec)) {
                targetExe = argvExe;
            }
        }
    }

    if (!std::filesystem::is_regular_file(targetExe, ec)) {
        return 1;
    }

    const std::filesystem::path targetDir = targetExe.parent_path();

    // Security Invariant 2: Strictly bind payload DLL to OpenSteamTool.dll.
    // Priority: 1. Co-located DLL in selfDir; 2. Target steamDir; 3. IFEO Payload registry value.
    std::filesystem::path payloadDll = selfDir / L"OpenSteamTool.dll";
    if (!std::filesystem::is_regular_file(payloadDll, ec)) {
        payloadDll = targetDir / L"OpenSteamTool.dll";
    }
    if (!std::filesystem::is_regular_file(payloadDll, ec)) {
        HKEY hKey = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\steam.exe",
                            0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH * 2]{};
            DWORD bytes = sizeof(buf);
            DWORD type = 0;
            if (::RegQueryValueExW(hKey, L"OSTPayloadPath", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes) == ERROR_SUCCESS ||
                ::RegQueryValueExW(hKey, L"Payload", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes) == ERROR_SUCCESS) {
                if (std::filesystem::is_regular_file(buf, ec)) {
                    payloadDll = buf;
                }
            }
            ::RegCloseKey(hKey);
        }
    }

    // Build command line:
    // If invoked via IFEO: argv[0] is ost-loader.exe, argv[1] is steam.exe (intercepted target), argv[2...] are genuine Steam args.
    // If invoked directly: forward all arguments from argv[1...].
    std::wstring cmdLine = L"\"" + targetExe.wstring() + L"\"";
    const int startArg = (__argc >= 2 && _wcsicmp(std::filesystem::path(__wargv[1]).filename().c_str(), L"steam.exe") == 0) ? 2 : 1;

    for (int i = startArg; i < __argc; ++i) {
        cmdLine += L" ";
        const std::wstring_view arg = __wargv[i];
        if (arg.find(L' ') != std::wstring_view::npos && !arg.starts_with(L"\"")) {
            cmdLine += L"\"";
            cmdLine += arg;
            cmdLine += L"\"";
        } else {
            cmdLine += arg;
        }
    }

    // Launch steam.exe with DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED.
    // Specifying DEBUG_ONLY_THIS_PROCESS natively instructs the Windows kernel to bypass IFEO redirection,
    // preventing infinite launch recursion without requiring registry modifications.
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    BOOL launched = ::CreateProcessW(
        targetExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED, nullptr,
        targetDir.c_str(), &si, &pi
    );

    if (!launched) {
        // Fallback: try standard CREATE_SUSPENDED in case debugger privileges are restricted
        launched = ::CreateProcessW(
            targetExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED, nullptr,
            targetDir.c_str(), &si, &pi
        );
        if (!launched) {
            return 1;
        }
    } else {
        // Immediately detach debugger so steam.exe runs autonomously as an independent normal process
        ::DebugSetProcessKillOnExit(FALSE);
        ::DebugActiveProcessStop(pi.dwProcessId);
    }

    // Security Invariant 3: Zero-crash fallback.
    // If OpenSteamTool.dll exists, inject it before entry point.
    // If it does not exist, safely resume Steam so official Steam is never bricked.
    if (std::filesystem::is_regular_file(payloadDll, ec)) {
        InjectDllIntoProcess(pi.hProcess, payloadDll);
    }

    // Awaken target host main thread
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return 0;
}
