#include "IfeoEngine.h"
#include "EmbeddedLoader.h"
#include "EmbeddedVersion.h"
#include "EmbeddedDwmapi.h"
#include "EmbeddedOpenSteamTool.h"

extern "C" {
#include "tinf.h"
}

#include <algorithm>
#include <array>
#include <cwchar>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace OST::Manager {

namespace {

constexpr wchar_t kIfeoKeyPath[]   = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\steam.exe";
constexpr wchar_t kLaunchMutex[]   = L"Global\\OpenSteamTool_IFEO_Launch_Guard";
constexpr wchar_t kValDebugger[]   = L"Debugger";
constexpr wchar_t kValBackup[]     = L"Debugger_Backup";
constexpr wchar_t kValPayload[]    = L"OSTPayloadPath";

// Unique GUID for legacy SDB cleanup
inline constexpr wchar_t kDatabaseGuidStr[] = L"{17910057-0057-4053-5400-535445414D31}";

bool InjectDllIntoProcess(HANDLE hProcess, const std::filesystem::path& dllPath) {
    const std::wstring pathStr = dllPath.wstring();
    const size_t bytes = (pathStr.size() + 1) * sizeof(wchar_t);

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

    ::WaitForSingleObject(hThread, 8000);
    ::CloseHandle(hThread);
    ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return true;
}

} // namespace

std::filesystem::path IfeoEngine::GetSelfExePath() {
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    while (len >= buf.size() - 1 && buf.size() < 32768) {
        buf.resize(buf.size() * 2, L'\0');
        len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    }
    if (len == 0) {
        return std::filesystem::current_path() / "ost-manager.exe";
    }
    return std::filesystem::path(buf.data());
}

std::filesystem::path IfeoEngine::GetSelfDir() {
    return GetSelfExePath().parent_path();
}

std::filesystem::path IfeoEngine::GetDefaultPayloadDllPath() {
    return GetSelfDir() / "OpenSteamTool.dll";
}

std::wstring IfeoEngine::DetectSteamInstallPath() {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t pathBuf[MAX_PATH * 2]{};
        DWORD bytes = sizeof(pathBuf);
        DWORD type = 0;
        if (::RegQueryValueExW(hKey, L"SteamPath", nullptr, &type, reinterpret_cast<LPBYTE>(pathBuf), &bytes) == ERROR_SUCCESS) {
            ::RegCloseKey(hKey);
            std::wstring res(pathBuf);
            std::ranges::replace(res, L'/', L'\\');
            if (std::filesystem::exists(res)) {
                return res;
            }
        }
        ::RegCloseKey(hKey);
    }

    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS ||
        ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t pathBuf[MAX_PATH * 2]{};
        DWORD bytes = sizeof(pathBuf);
        DWORD type = 0;
        if (::RegQueryValueExW(hKey, L"InstallPath", nullptr, &type, reinterpret_cast<LPBYTE>(pathBuf), &bytes) == ERROR_SUCCESS) {
            ::RegCloseKey(hKey);
            std::wstring res(pathBuf);
            std::ranges::replace(res, L'/', L'\\');
            if (std::filesystem::exists(res)) {
                return res;
            }
        }
        ::RegCloseKey(hKey);
    }

    constexpr std::wstring_view kDefaultPath = L"C:\\Program Files (x86)\\Steam";
    if (std::filesystem::exists(kDefaultPath)) {
        return std::wstring(kDefaultPath);
    }
    return L"";
}

static std::wstring ExtractExeFromCommandLine(std::wstring_view cmdLine) {
    while (!cmdLine.empty() && ::iswspace(cmdLine.front())) {
        cmdLine.remove_prefix(1);
    }
    if (cmdLine.empty()) return L"";

    if (cmdLine.front() == L'\"') {
        const size_t endQuote = cmdLine.find(L'\"', 1);
        if (endQuote != std::wstring_view::npos) {
            return std::wstring(cmdLine.substr(1, endQuote - 1));
        }
    }
    const size_t spacePos = cmdLine.find(L' ');
    if (spacePos != std::wstring_view::npos) {
        return std::wstring(cmdLine.substr(0, spacePos));
    }
    return std::wstring(cmdLine);
}

IfeoDetails IfeoEngine::QueryDetails() {
    IfeoDetails details;

    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeoKeyPath, 0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
        details.status = IfeoStatus::NotInstalled;
        return details;
    }

    wchar_t debuggerBuf[MAX_PATH * 2]{};
    DWORD bytes = sizeof(debuggerBuf);
    DWORD type = 0;
    const auto err = ::RegQueryValueExW(hKey, kValDebugger, nullptr, &type, reinterpret_cast<LPBYTE>(debuggerBuf), &bytes);
    if (err != ERROR_SUCCESS || debuggerBuf[0] == L'\0') {
        ::RegCloseKey(hKey);
        details.status = IfeoStatus::NotInstalled;
        return details;
    }

    details.registeredDebuggerCmd = debuggerBuf;
    details.registeredExePath = ExtractExeFromCommandLine(details.registeredDebuggerCmd);

    // Retrieve registered payload path
    wchar_t payloadBuf[MAX_PATH * 2]{};
    bytes = sizeof(payloadBuf);
    if (::RegQueryValueExW(hKey, kValPayload, nullptr, &type, reinterpret_cast<LPBYTE>(payloadBuf), &bytes) == ERROR_SUCCESS) {
        details.registeredPayloadPath = payloadBuf;
    }
    ::RegCloseKey(hKey);

    // Check if the registered debugger refers to ost-manager, ost-loader, ost-shim, or opensteamtool
    std::wstring lowerExe = details.registeredExePath;
    std::ranges::transform(lowerExe, lowerExe.begin(), ::towlower);
    if (lowerExe.find(L"ost-manager") == std::wstring::npos &&
        lowerExe.find(L"ost-loader") == std::wstring::npos &&
        lowerExe.find(L"ost-shim") == std::wstring::npos &&
        lowerExe.find(L"opensteamtool") == std::wstring::npos) {
        details.status = IfeoStatus::NotInstalled;
        return details;
    }

    // 1. Check if registered exe exists on disk
    std::error_code ec;
    details.exeExists = std::filesystem::is_regular_file(details.registeredExePath, ec);
    if (!details.exeExists) {
        details.status = IfeoStatus::BrokenOrphaned;
        return details;
    }

    if (!details.registeredPayloadPath.empty()) {
        details.payloadExists = std::filesystem::is_regular_file(details.registeredPayloadPath, ec);
    }

    // 2. Check if registered in Steam root directory (Root Mode)
    // Root mode strictly executes dedicated loader from the Steam directory
    const std::wstring steamDir = DetectSteamInstallPath();
    bool isRoot = false;
    if (!steamDir.empty()) {
        std::error_code ecRoot;
        if (std::filesystem::equivalent(std::filesystem::path(details.registeredExePath).parent_path(), steamDir, ecRoot)) {
            isRoot = true;
        }
    }

    if (isRoot) {
        details.status = IfeoStatus::InstalledRoot;
        details.isCurrentInstance = true;
        return details;
    }

    // 3. Check if registered exe belongs to this manager's directory (Portable Mode)
    const auto selfExe = GetSelfExePath();
    const auto selfDir = selfExe.parent_path();
    const auto registeredPath = std::filesystem::path(details.registeredExePath);

    if (std::filesystem::equivalent(details.registeredExePath, selfExe, ec) ||
        std::filesystem::equivalent(registeredPath.parent_path(), selfDir, ec)) {
        details.status = IfeoStatus::InstalledPortable;
        details.isCurrentInstance = true;
        return details;
    }

    // Targets another manager instance in a different directory! Collision detected!
    details.status = IfeoStatus::ConflictOtherInstance;
    details.isCurrentInstance = false;
    return details;
}

IfeoStatus IfeoEngine::QueryStatus(std::wstring& outPayloadPath) {
    const auto details = QueryDetails();
    outPayloadPath = details.registeredPayloadPath;
    if (outPayloadPath.empty() && details.status != IfeoStatus::NotInstalled) {
        outPayloadPath = GetDefaultPayloadDllPath().wstring();
    }
    return details.status;
}

static bool RobustDeployFile(const std::filesystem::path& src,
                             const std::filesystem::path& dst,
                             std::wstring& outError);

static bool DeployBufferToFile(const void* data,
                               size_t size,
                               const std::filesystem::path& dst,
                               std::wstring& outError);

bool IfeoEngine::InstallIfeo(const std::filesystem::path& payloadDllPath,
                             std::wstring& outError,
                             bool isRootMode) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(payloadDllPath, ec) || ec) {
        if (!DeployBufferToFile(kEmbeddedOpenSteamToolDll, kEmbeddedOpenSteamToolDllSize, payloadDllPath, outError)) {
            return false;
        }
    }



    std::wstring debuggerCmd;
    if (isRootMode) {
        // Root Mode: strictly bind IFEO to dedicated lightweight loader in Steam root
        const auto rootLoader = payloadDllPath.parent_path() / L"ost-loader.exe";
        if (!std::filesystem::is_regular_file(rootLoader, ec)) {
            outError = L"Dedicated loader not found in Steam root directory: " + rootLoader.wstring();
            return false;
        }
        debuggerCmd = L"\"" + rootLoader.wstring() + L"\"";
    } else {
        // Portable Mode: strictly bind IFEO to ost-manager /launch (never touch ost-loader)
        debuggerCmd = L"\"" + GetSelfExePath().wstring() + L"\" /launch";
        // Clean residual root binaries if root mode was previously installed
        CleanResidualSteamRootBinaries(DetectSteamInstallPath(), payloadDllPath);
    }

    const std::wstring payloadStr = payloadDllPath.wstring();

    HKEY hKey = nullptr;
    DWORD disposition = 0;
    LSTATUS status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIfeoKeyPath, 0, nullptr, 0,
                                       KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                                       nullptr, &hKey, &disposition);
    if (status != ERROR_SUCCESS) {
        outError = L"Failed to create IFEO registry key (err=" + std::to_wstring(status) + L"). Please run as Administrator.";
        return false;
    }

    status = ::RegSetValueExW(hKey, kValDebugger, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(debuggerCmd.c_str()),
                              static_cast<DWORD>((debuggerCmd.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        ::RegCloseKey(hKey);
        outError = L"Failed to write Debugger value (err=" + std::to_wstring(status) + L")";
        return false;
    }

    ::RegSetValueExW(hKey, kValPayload, 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(payloadStr.c_str()),
                     static_cast<DWORD>((payloadStr.size() + 1) * sizeof(wchar_t)));

    // Clean up any stale backup
    ::RegDeleteValueW(hKey, kValBackup);
    ::RegCloseKey(hKey);

    CleanLegacySdbArtifacts();
    return true;
}

bool IfeoEngine::UninstallIfeo(std::wstring& outError) {
    (void)outError;
    HKEY hKey = nullptr;
    LSTATUS status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeoKeyPath, 0,
                                     KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey);
    if (status == ERROR_SUCCESS) {
        ::RegDeleteValueW(hKey, kValDebugger);
        ::RegDeleteValueW(hKey, kValBackup);
        ::RegDeleteValueW(hKey, kValPayload);

        // Check if key is now empty, delete subkey if clean
        DWORD subkeyCount = 0, valueCount = 0;
        if (::RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subkeyCount, nullptr, nullptr,
                              &valueCount, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            ::RegCloseKey(hKey);
            hKey = nullptr;
            if (subkeyCount == 0 && valueCount == 0) {
                ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kIfeoKeyPath, KEY_WOW64_64KEY, 0);
            }
        }
        if (hKey) ::RegCloseKey(hKey);
    }

    // Clean up all deployed binaries from Steam root directory (strictly preserves opensteamtool.toml config)
    const std::wstring steamDir = DetectSteamInstallPath();
    if (!steamDir.empty()) {
        std::error_code ec;
        const auto rootDir = std::filesystem::path(steamDir);
        std::filesystem::remove(rootDir / L"OpenSteamTool.dll", ec);
        std::filesystem::remove(rootDir / L"ost-loader.exe", ec);
        std::filesystem::remove(rootDir / L"ost-shim.exe", ec);
        // NOTE: Strictly preserve opensteamtool.toml user configuration on uninstall!
    }

    CleanLegacySdbArtifacts();
    return true;
}

static bool RobustDeployFile(const std::filesystem::path& src,
                             const std::filesystem::path& dst,
                             std::wstring& outError) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        outError = L"Source file not found: " + src.wstring();
        return false;
    }

    if (std::filesystem::equivalent(src, dst, ec)) {
        return true; // Source and destination are identical; nothing to deploy
    }

    if (std::filesystem::exists(dst, ec)) {
        // Clear read-only/hidden attributes to allow clean overwrite
        ::SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    // 1. Attempt direct overwrite
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) {
        return true;
    }

    // 2. Fallback: try removing old file first, then copy fresh
    std::error_code ecRem;
    std::filesystem::remove(dst, ecRem);
    ec.clear();
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) {
        return true;
    }

    outError = L"Failed to overwrite " + dst.filename().wstring() +
               L" in Steam directory (err=" + std::to_wstring(ec.value()) + L"). "
               L"Please ensure Steam is completely closed and the file is not locked.";
    return false;
}

static bool DeployBufferToFile(const void* data,
                               size_t size,
                               const std::filesystem::path& dst,
                               std::wstring& outError) {
    if (!data || size == 0) {
        outError = L"Internal embedded binary is empty or invalid.";
        return false;
    }

    const unsigned char* pData = static_cast<const unsigned char*>(data);
    std::vector<uint8_t> decompressed;
    const void* writeData = pData;
    size_t writeSize = size;

    if (size >= 10 && pData[0] == 0x1f && pData[1] == 0x8b) {
        // Transparent Gzip decompression for extreme size optimization
        const uint32_t uncompressedSize = static_cast<uint32_t>(pData[size - 4])
                                        | (static_cast<uint32_t>(pData[size - 3]) << 8)
                                        | (static_cast<uint32_t>(pData[size - 2]) << 16)
                                        | (static_cast<uint32_t>(pData[size - 1]) << 24);
        decompressed.resize(uncompressedSize);
        unsigned int dlen = uncompressedSize;
        const int res = tinf_gzip_uncompress(decompressed.data(), &dlen, pData, static_cast<unsigned int>(size));
        if (res != TINF_OK) {
            outError = L"Failed to decompress embedded payload (code " + std::to_wstring(res) + L").";
            return false;
        }
        writeData = decompressed.data();
        writeSize = dlen;
    }

    std::error_code ec;
    if (std::filesystem::exists(dst, ec)) {
        ::SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    HANDLE hFile = ::CreateFileW(
        dst.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        std::error_code ecRem;
        std::filesystem::remove(dst, ecRem);

        hFile = ::CreateFileW(
            dst.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
        );
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD gle = ::GetLastError();
        outError = L"Failed to write " + dst.filename().wstring() +
                   L" into Steam directory (err=" + std::to_wstring(gle) + L"). "
                   L"Please ensure Steam is completely closed and the file is not locked.";
        return false;
    }

    DWORD bytesWritten = 0;
    const BOOL ok = ::WriteFile(hFile, writeData, static_cast<DWORD>(writeSize), &bytesWritten, nullptr);
    const DWORD writeErr = ::GetLastError();
    ::CloseHandle(hFile);

    if (!ok || bytesWritten != writeSize) {
        outError = L"Failed to complete writing " + dst.filename().wstring() +
                   L" (written=" + std::to_wstring(bytesWritten) + L"/" + std::to_wstring(writeSize) +
                   L", err=" + std::to_wstring(writeErr) + L")";
        return false;
    }

    return true;
}

bool IfeoEngine::DeployToSteamRoot(const std::wstring& steamDir,
                                   const std::filesystem::path& sourceDllPath,
                                   std::wstring& outError) {
    std::wstring cleanSteamDir = steamDir;
    while (!cleanSteamDir.empty() && (cleanSteamDir.back() == L'\\' || cleanSteamDir.back() == L'/')) {
        cleanSteamDir.pop_back();
    }

    std::error_code ec;
    if (!std::filesystem::exists(cleanSteamDir, ec)) {
        outError = L"Steam directory does not exist: " + cleanSteamDir;
        return false;
    }

    const auto steamPath = std::filesystem::path(cleanSteamDir);

    // Clean up any legacy loader artifacts if present in Steam root
    std::filesystem::remove(steamPath / L"ost-shim.exe", ec);

    // 1. Copy/Update OpenSteamTool.dll to Steam directory (prefer disk file, fallback to embedded)
    const std::filesystem::path destDll = steamPath / "OpenSteamTool.dll";
    if (std::filesystem::is_regular_file(sourceDllPath, ec)) {
        if (!RobustDeployFile(sourceDllPath, destDll, outError)) {
            return false;
        }
    } else {
        if (!DeployBufferToFile(kEmbeddedOpenSteamToolDll, kEmbeddedOpenSteamToolDllSize, destDll, outError)) {
            return false;
        }
    }

    // 2. Deploy dedicated lightweight loader ost-loader.exe to Steam directory
    // If a custom external ost-loader.exe is placed next to source DLL, honor it;
    // Otherwise, reliably extract the embedded pure ost-loader binary into Steam root.
    const std::filesystem::path destLoader = steamPath / "ost-loader.exe";
    const std::filesystem::path externalLoader = sourceDllPath.parent_path() / L"ost-loader.exe";

    if (std::filesystem::is_regular_file(externalLoader, ec)) {
        if (!RobustDeployFile(externalLoader, destLoader, outError)) {
            return false;
        }
    } else {
        if (!DeployBufferToFile(kEmbeddedOstLoader, kEmbeddedOstLoaderSize, destLoader, outError)) {
            return false;
        }
    }



    return true;
}

void IfeoEngine::CleanResidualSteamRootBinaries(const std::wstring& steamDir,
                                              const std::filesystem::path& currentPayloadPath) noexcept {
    if (steamDir.empty()) {
        return;
    }

    std::error_code ec;
    const auto rootDir = std::filesystem::path(steamDir);
    if (!std::filesystem::exists(rootDir, ec) || !std::filesystem::is_directory(rootDir, ec)) {
        return;
    }

    // 1. Clean root ost-loader.exe
    const auto loaderPath = rootDir / L"ost-loader.exe";
    if (std::filesystem::exists(loaderPath, ec)) {
        ::SetFileAttributesW(loaderPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(loaderPath, ec);
    }

    // 2. Clean root ost-shim.exe
    const auto shimPath = rootDir / L"ost-shim.exe";
    if (std::filesystem::exists(shimPath, ec)) {
        ::SetFileAttributesW(shimPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(shimPath, ec);
    }

    // 3. Clean root OpenSteamTool.dll IF NOT physically equivalent to currentPayloadPath
    const auto rootPayload = rootDir / L"OpenSteamTool.dll";
    if (std::filesystem::exists(rootPayload, ec)) {
        std::error_code ecEq;
        if (currentPayloadPath.empty() || !std::filesystem::equivalent(rootPayload, currentPayloadPath, ecEq)) {
            ::SetFileAttributesW(rootPayload.c_str(), FILE_ATTRIBUTE_NORMAL);
            std::filesystem::remove(rootPayload, ec);
        }
    }
    // NOTE: Strictly preserve opensteamtool.toml!
}

bool IfeoEngine::DeployStandardDll(const std::wstring& steamDir,
                                    const std::filesystem::path& payloadDllPath,
                                    std::wstring& outError) {
    if (steamDir.empty()) {
        outError = L"Steam installation directory could not be detected.";
        return false;
    }

    std::error_code ec;
    const auto rootDir = std::filesystem::path(steamDir);
    if (!std::filesystem::is_directory(rootDir, ec)) {
        outError = L"Steam directory does not exist: " + rootDir.wstring();
        return false;
    }

    DWORD steamPid = 0;
    if (IsSteamRunning(steamPid)) {
        outError = L"Steam is currently running (PID " + std::to_wstring(steamPid) + L"). Please close Steam completely before deploying DLLs.";
        return false;
    }

    // 1. Resolve source OpenSteamTool.dll
    std::filesystem::path srcPayload = payloadDllPath;
    if (!std::filesystem::is_regular_file(srcPayload, ec) || ec) {
        srcPayload = GetSelfDir() / L"OpenSteamTool.dll";
    }

    const auto sourceDir = std::filesystem::is_regular_file(srcPayload, ec) ? srcPayload.parent_path() : GetSelfDir();
    const auto selfDir   = GetSelfDir();

    // 2. Resolve version.dll and dwmapi.dll sources
    std::filesystem::path srcVersion = sourceDir / L"version.dll";
    if (!std::filesystem::is_regular_file(srcVersion, ec) || ec) {
        srcVersion = selfDir / L"version.dll";
    }

    std::filesystem::path srcDwmapi = sourceDir / L"dwmapi.dll";
    if (!std::filesystem::is_regular_file(srcDwmapi, ec) || ec) {
        srcDwmapi = selfDir / L"dwmapi.dll";
    }

    // 3. Deploy OpenSteamTool.dll to Steam root (prefer external file, fallback to embedded)
    const auto dstPayload = rootDir / L"OpenSteamTool.dll";
    if (std::filesystem::is_regular_file(srcPayload, ec)) {
        if (!RobustDeployFile(srcPayload, dstPayload, outError)) {
            return false;
        }
    } else {
        if (!DeployBufferToFile(kEmbeddedOpenSteamToolDll, kEmbeddedOpenSteamToolDllSize, dstPayload, outError)) {
            return false;
        }
    }

    // 4. Deploy proxy DLLs:
    // Deploy version.dll to Steam root (prefer external file, fallback to embedded)
    const auto dstVersion = rootDir / L"version.dll";
    if (std::filesystem::is_regular_file(srcVersion, ec)) {
        if (!RobustDeployFile(srcVersion, dstVersion, outError)) {
            return false;
        }
    } else {
        if (!DeployBufferToFile(kEmbeddedVersionDll, kEmbeddedVersionDllSize, dstVersion, outError)) {
            return false;
        }
    }

    // Deploy dwmapi.dll to Steam\bin\dwmapi.dll (prefer external file, fallback to embedded)
    const auto binDir = rootDir / L"bin";
    std::filesystem::create_directories(binDir, ec);
    const auto dstDwmapi = binDir / L"dwmapi.dll";
    if (std::filesystem::is_regular_file(srcDwmapi, ec)) {
        if (!RobustDeployFile(srcDwmapi, dstDwmapi, outError)) {
            return false;
        }
    } else {
        if (!DeployBufferToFile(kEmbeddedDwmapiDll, kEmbeddedDwmapiDllSize, dstDwmapi, outError)) {
            return false;
        }
    }

    // 5. Clean up any rogue dwmapi.dll misplaced in Steam root
    const auto rogueDwmapi = rootDir / L"dwmapi.dll";
    if (std::filesystem::exists(rogueDwmapi, ec)) {
        ::SetFileAttributesW(rogueDwmapi.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(rogueDwmapi, ec);
    }



    return true;
}

bool IfeoEngine::RemoveStandardDll(const std::wstring& steamDir,
                                   std::wstring& outError) {
    if (steamDir.empty()) {
        outError = L"Steam installation directory could not be detected.";
        return false;
    }

    DWORD steamPid = 0;
    if (IsSteamRunning(steamPid)) {
        outError = L"Steam is currently running (PID " + std::to_wstring(steamPid) + L"). Please close Steam completely before removing DLLs.";
        return false;
    }

    std::error_code ec;
    const auto rootDir = std::filesystem::path(steamDir);
    if (!std::filesystem::is_directory(rootDir, ec)) {
        return true;
    }

    // 1. Remove <Steam>\version.dll
    const auto versionPath = rootDir / L"version.dll";
    if (std::filesystem::exists(versionPath, ec)) {
        ::SetFileAttributesW(versionPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(versionPath, ec);
    }

    // 2. Remove <Steam>\bin\dwmapi.dll
    const auto binDwmapi = rootDir / L"bin" / L"dwmapi.dll";
    if (std::filesystem::exists(binDwmapi, ec)) {
        ::SetFileAttributesW(binDwmapi.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(binDwmapi, ec);
    }

    // 3. Remove rogue <Steam>\dwmapi.dll
    const auto rootDwmapi = rootDir / L"dwmapi.dll";
    if (std::filesystem::exists(rootDwmapi, ec)) {
        ::SetFileAttributesW(rootDwmapi.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(rootDwmapi, ec);
    }

    // 4. Remove <Steam>\OpenSteamTool.dll
    const auto rootPayload = rootDir / L"OpenSteamTool.dll";
    if (std::filesystem::exists(rootPayload, ec)) {
        ::SetFileAttributesW(rootPayload.c_str(), FILE_ATTRIBUTE_NORMAL);
        std::filesystem::remove(rootPayload, ec);
    }

    // Strictly preserve opensteamtool.toml!
    return true;
}

bool IfeoEngine::IsSteamRunning(DWORD& outPid) noexcept {
    outPid = 0;
    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (::Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                outPid = pe.th32ProcessID;
                ::CloseHandle(hSnap);
                return true;
            }
        } while (::Process32NextW(hSnap, &pe));
    }

    ::CloseHandle(hSnap);
    return false;
}

bool IfeoEngine::IsPayloadInjected(DWORD pid) noexcept {
    if (pid == 0) return false;

    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W me{ sizeof(me) };
    if (::Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"OpenSteamTool.dll") == 0) {
                ::CloseHandle(hSnap);
                return true;
            }
        } while (::Module32NextW(hSnap, &me));
    }

    ::CloseHandle(hSnap);
    return false;
}

bool IfeoEngine::LaunchHostAndInject(int argc, wchar_t* argv[]) noexcept {
    if (argc < 3) {
        return false;
    }

    // argv[2] is the target executable provided by Windows IFEO
    const std::wstring targetExe = argv[2];

    // Build command line: targetExe + [argv[3]...]
    std::wstring cmdLine = L"\"" + targetExe + L"\"";
    for (int i = 3; i < argc; ++i) {
        cmdLine += L" ";
        const std::wstring_view arg = argv[i];
        if (arg.find(L' ') != std::wstring_view::npos && !arg.starts_with(L"\"")) {
            cmdLine += L"\"";
            cmdLine += arg;
            cmdLine += L"\"";
        } else {
            cmdLine += arg;
        }
    }

    // Query registered payload DLL (Standard users can read HKLM without elevation)
    std::wstring payloadPathStr;
    HKEY hIfeoKey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeoKeyPath, 0,
                         KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hIfeoKey) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH * 2]{};
        DWORD bytes = sizeof(buf);
        DWORD type = 0;
        if (::RegQueryValueExW(hIfeoKey, kValPayload, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes) == ERROR_SUCCESS) {
            payloadPathStr = buf;
        }
        ::RegCloseKey(hIfeoKey);
    }

    if (payloadPathStr.empty()) {
        payloadPathStr = GetDefaultPayloadDllPath().wstring();
    }

    // Launch target steam.exe in DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED mode.
    // Specifying a debugging flag natively instructs the Windows kernel to bypass IFEO redirection,
    // eliminating any need to write to HKLM or use registry swap mutexes!
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const BOOL launched = ::CreateProcessW(
        targetExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi
    );

    if (!launched) {
        return false;
    }

    // Prevent target process from being terminated when intermediate debugger process exits
    ::DebugSetProcessKillOnExit(FALSE);

    // Immediately detach debugger so steam.exe runs autonomously as an independent normal process
    ::DebugActiveProcessStop(pi.dwProcessId);

    // Inject OpenSteamTool.dll into the suspended process at entry point (0ms)
    std::error_code ecPayload;
    if (std::filesystem::is_regular_file(payloadPathStr, ecPayload)) {
        InjectDllIntoProcess(pi.hProcess, std::filesystem::path(payloadPathStr));
    } else {
        const auto fallbackDll = GetDefaultPayloadDllPath();
        if (std::filesystem::is_regular_file(fallbackDll, ecPayload)) {
            InjectDllIntoProcess(pi.hProcess, fallbackDll);
        }
    }

    // Awaken target host main thread
    ::ResumeThread(pi.hThread);

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

bool IfeoEngine::LaunchSteamWithInjection(const std::filesystem::path& steamDir,
                                          const std::filesystem::path& payloadDll,
                                          DWORD& outPid,
                                          std::wstring& outError) {
    outPid = 0;
    outError.clear();

    const auto steamExe = steamDir / L"steam.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(steamExe, ec)) {
        outError = L"steam.exe not found in " + steamDir.wstring();
        return false;
    }

    if (!std::filesystem::is_regular_file(payloadDll, ec)) {
        outError = L"Payload DLL not found: " + payloadDll.wstring();
        return false;
    }

    std::wstring cmdLine = L"\"" + steamExe.wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const BOOL launched = ::CreateProcessW(
        steamExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED, nullptr,
        steamDir.c_str(), &si, &pi
    );

    if (!launched) {
        const DWORD gle = ::GetLastError();
        outError = L"CreateProcessW failed with error " + std::to_wstring(gle);
        return false;
    }

    outPid = pi.dwProcessId;

    ::DebugSetProcessKillOnExit(FALSE);
    ::DebugActiveProcessStop(pi.dwProcessId);

    if (!InjectDllIntoProcess(pi.hProcess, payloadDll)) {
        outError = L"Failed to inject OpenSteamTool.dll into Steam process.";
        ::ResumeThread(pi.hThread);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return false;
    }

    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

bool IfeoEngine::IsRunningElevated() noexcept {
    HANDLE hToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = ::GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(hToken);
    return ok && elevation.TokenIsElevated != 0;
}

bool IfeoEngine::ElevateAndExecute(HWND hWndParent,
                                    const std::wstring& parameters,
                                    std::wstring& outError) {
    const std::wstring selfExe = GetSelfExePath().wstring();
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hWndParent;
    sei.lpVerb = L"runas";
    sei.lpFile = selfExe.c_str();
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;

    if (!::ShellExecuteExW(&sei)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_CANCELLED) {
            outError = L"Cancelled by user.";
        } else {
            outError = L"Elevation failed (err=" + std::to_wstring(err) + L")";
        }
        return false;
    }

    if (sei.hProcess) {
        // Pump messages while waiting to avoid freezing UI and losing foreground Z-order
        while (true) {
            const DWORD waitRes = ::MsgWaitForMultipleObjectsEx(
                1, &sei.hProcess, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE
            );
            if (waitRes == WAIT_OBJECT_0) {
                break;
            }
            MSG msg;
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }

        DWORD exitCode = 0;
        ::GetExitCodeProcess(sei.hProcess, &exitCode);
        ::CloseHandle(sei.hProcess);

        // Always bring window back to top foreground after elevation finishes!
        if (hWndParent && ::IsWindow(hWndParent)) {
            ::ShowWindow(hWndParent, SW_RESTORE);
            ::SetForegroundWindow(hWndParent);
            ::BringWindowToTop(hWndParent);
            ::SetActiveWindow(hWndParent);
        }

        if (exitCode != 0) {
            // Check if worker recorded a detailed error log in %TEMP%
            std::wstring workerError;
            wchar_t tempPath[MAX_PATH]{};
            if (::GetTempPathW(MAX_PATH, tempPath) > 0) {
                const std::filesystem::path logFile = std::filesystem::path(tempPath) / L"OpenSteamTool_elevated.log";
                FILE* fp = nullptr;
                if (_wfopen_s(&fp, logFile.c_str(), L"r, ccs=UTF-8") == 0 && fp) {
                    wchar_t line[1024]{};
                    if (fgetws(line, 1024, fp)) {
                        workerError = line;
                    }
                    fclose(fp);
                    std::error_code ecLog;
                    std::filesystem::remove(logFile, ecLog);
                }
            }

            if (!workerError.empty()) {
                outError = workerError;
            } else if (exitCode == 2) {
                outError = L"Steam is currently running. Please close Steam completely before installing or uninstalling.";
            } else {
                outError = L"Elevated action failed with exit code " + std::to_wstring(exitCode);
            }
            return false;
        }
    }
    return true;
}

void IfeoEngine::CleanLegacySdbArtifacts() noexcept {
    // 1. Clean registry InstalledSDB
    const std::wstring subKey = std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\InstalledSDB\\") + kDatabaseGuidStr;
    ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), KEY_WOW64_64KEY, 0);
    ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), KEY_WOW64_32KEY, 0);

    // 2. Clean custom sdb file if exists
    wchar_t winDir[MAX_PATH]{};
    if (::GetWindowsDirectoryW(winDir, MAX_PATH) > 0) {
        const std::wstring sdbPath = std::wstring(winDir) + L"\\AppPatch\\Custom\\" + kDatabaseGuidStr + L".sdb";
        std::error_code ec;
        std::filesystem::remove(sdbPath, ec);
    }
}

DllDeployDetails IfeoEngine::QueryStandardDllDetails(const std::wstring& steamDir) noexcept {
    DllDeployDetails details;
    if (steamDir.empty()) {
        return details;
    }

    std::error_code ec;
    const auto rootDir = std::filesystem::path(steamDir);
    if (!std::filesystem::is_directory(rootDir, ec)) {
        return details;
    }

    const auto rootPayload = rootDir / L"OpenSteamTool.dll";
    const auto rootVersion = rootDir / L"version.dll";
    const auto binDwmapi   = rootDir / L"bin" / L"dwmapi.dll";

    details.hasPayload = std::filesystem::is_regular_file(rootPayload, ec);
    details.hasVersion = std::filesystem::is_regular_file(rootVersion, ec);
    details.hasDwmapi  = std::filesystem::is_regular_file(binDwmapi, ec);

    if (!details.hasVersion && !details.hasDwmapi) {
        details.status = DllDeployStatus::NotDeployed;
    } else if (!details.hasPayload) {
        details.status = DllDeployStatus::MissingPayload;
    } else if (details.hasVersion && details.hasDwmapi) {
        details.status = DllDeployStatus::DeployedBoth;
    } else if (details.hasVersion) {
        details.status = DllDeployStatus::DeployedVersion;
    } else {
        details.status = DllDeployStatus::DeployedDwmapi;
    }

    return details;
}

} // namespace OST::Manager
