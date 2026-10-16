#include "Hooks_Diversion.h"
#include "OSTPlatform/include/Detour.h"
#include "OSTPlatform/include/Encoding.h"
#include "Utils/Logging/Log.h"
#include "dllmain.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <windows.h>

namespace Hooks_Diversion {

namespace {

using LoadLibraryExW_t     = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetModuleHandleW_t   = HMODULE(WINAPI*)(LPCWSTR);
using GetModuleHandleA_t   = HMODULE(WINAPI*)(LPCSTR);
using GetModuleHandleExW_t = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using GetModuleHandleExA_t = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);

LoadLibraryExW_t     s_origLoadLibraryExW     = nullptr;
GetModuleHandleW_t   s_origGetModuleHandleW   = nullptr;
GetModuleHandleA_t   s_origGetModuleHandleA   = nullptr;
GetModuleHandleExW_t s_origGetModuleHandleExW = nullptr;
GetModuleHandleExA_t s_origGetModuleHandleExA = nullptr;

std::atomic<bool> s_isInstalled{false};
std::wstring s_wideDiversionPath;

[[nodiscard]] inline bool IsSteamClientModuleW(std::wstring_view path) noexcept {
    if (path.empty()) return false;
    const auto pos = path.find_last_of(L"\\/");
    const std::wstring_view fn = (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);

    switch (fn.size()) {
        case 17: return _wcsnicmp(fn.data(), L"steamclient64.dll", 17) == 0;
        case 15: return _wcsnicmp(fn.data(), L"steamclient.dll",   15) == 0;
        case 13: return _wcsnicmp(fn.data(), L"steamclient64",     13) == 0;
        case 11: return _wcsnicmp(fn.data(), L"steamclient",       11) == 0;
        default: return false;
    }
}

[[nodiscard]] inline bool IsSteamClientModuleA(std::string_view path) noexcept {
    if (path.empty()) return false;
    const auto pos = path.find_last_of("\\/");
    const std::string_view fn = (pos == std::string_view::npos) ? path : path.substr(pos + 1);

    switch (fn.size()) {
        case 17: return _strnicmp(fn.data(), "steamclient64.dll", 17) == 0;
        case 15: return _strnicmp(fn.data(), "steamclient.dll",   15) == 0;
        case 13: return _strnicmp(fn.data(), "steamclient64",     13) == 0;
        case 11: return _strnicmp(fn.data(), "steamclient",       11) == 0;
        default: return false;
    }
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName && IsSteamClientModuleW(lpLibFileName)) {
        if (!s_wideDiversionPath.empty()) {
            HMODULE hDiv = s_origLoadLibraryExW(s_wideDiversionPath.c_str(), hFile, dwFlags);
            if (hDiv) {
                auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
                client_hModule.compare_exchange_strong(expected, reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hDiv));
                g_IsDiversionActive.store(true, std::memory_order_release);

                // Startup Race Barrier: Wait for client hooks to be installed before
                // steam.exe invokes exports on the returned module handle.
                if (!g_HooksInstalled.load(std::memory_order_acquire) && g_hClientHooksReadyEvent) {
                    ::WaitForSingleObject(g_hClientHooksReadyEvent, 2000);
                }

                LOG_INFO("Hooks_Diversion: Redirected LoadLibraryExW {} -> {}",
                         OSTPlatform::Encoding::WideToUtf8(lpLibFileName),
                         DiversionPath);
                return hDiv;
            }
            LOG_ERROR("Hooks_Diversion: Failed to load diversion module from {}, falling back to original {}",
                      DiversionPath, OSTPlatform::Encoding::WideToUtf8(lpLibFileName));
        }

        HMODULE hOrig = s_origLoadLibraryExW(lpLibFileName, hFile, dwFlags);
        if (hOrig) {
            auto expected = static_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(nullptr);
            client_hModule.compare_exchange_strong(expected, reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hOrig));
            if (!g_HooksInstalled.load(std::memory_order_acquire) && g_hClientHooksReadyEvent) {
                ::WaitForSingleObject(g_hClientHooksReadyEvent, 2000);
            }
        }
        return hOrig;
    }
    return s_origLoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

HMODULE WINAPI Hooked_GetModuleHandleW(LPCWSTR lpModuleName) {
    if (lpModuleName && IsSteamClientModuleW(lpModuleName)) {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) {
            return reinterpret_cast<HMODULE>(h);
        }
    }
    return s_origGetModuleHandleW(lpModuleName);
}

HMODULE WINAPI Hooked_GetModuleHandleA(LPCSTR lpModuleName) {
    if (lpModuleName && IsSteamClientModuleA(lpModuleName)) {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) {
            return reinterpret_cast<HMODULE>(h);
        }
    }
    return s_origGetModuleHandleA(lpModuleName);
}

BOOL WINAPI Hooked_GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
        IsSteamClientModuleW(lpModuleName))
    {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) {
            if (phModule) {
                *phModule = reinterpret_cast<HMODULE>(h);
                if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) && !s_wideDiversionPath.empty()) {
                    HMODULE dummy = nullptr;
                    s_origGetModuleHandleExW(dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN,
                                             s_wideDiversionPath.c_str(), &dummy);
                }
                return TRUE;
            }
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    return s_origGetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI Hooked_GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (lpModuleName && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
        IsSteamClientModuleA(lpModuleName))
    {
        auto h = client_hModule.load(std::memory_order_acquire);
        if (h) {
            if (phModule) {
                *phModule = reinterpret_cast<HMODULE>(h);
                if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) && DiversionPath[0] != '\0') {
                    HMODULE dummy = nullptr;
                    s_origGetModuleHandleExA(dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN,
                                             DiversionPath, &dummy);
                }
                return TRUE;
            }
            ::SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    return s_origGetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

} // namespace

bool EnsureDiversionFileReady(const std::wstring& originalPath, const std::wstring& diversionPath) noexcept {
    if (originalPath.empty() || diversionPath.empty()) {
        return false;
    }
    s_wideDiversionPath = diversionPath;

    WIN32_FILE_ATTRIBUTE_DATA origAttr{}, divAttr{};
    const bool origExists = ::GetFileAttributesExW(originalPath.c_str(), GetFileExInfoStandard, &origAttr) != 0 &&
                            !(origAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    const bool divExists  = ::GetFileAttributesExW(diversionPath.c_str(), GetFileExInfoStandard, &divAttr) != 0 &&
                            !(divAttr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);

    if (!origExists) {
        LOG_WARN("Hooks_Diversion: Original steamclient64.dll not found at {}",
                 OSTPlatform::Encoding::WideToUtf8(originalPath));
        return false;
    }

    bool isUpToDate = false;
    if (origExists && divExists) {
        if ((origAttr.nFileSizeHigh != 0 || origAttr.nFileSizeLow != 0) &&
            origAttr.nFileSizeHigh == divAttr.nFileSizeHigh &&
            origAttr.nFileSizeLow  == divAttr.nFileSizeLow &&
            origAttr.ftLastWriteTime.dwLowDateTime  == divAttr.ftLastWriteTime.dwLowDateTime &&
            origAttr.ftLastWriteTime.dwHighDateTime == divAttr.ftLastWriteTime.dwHighDateTime)
        {
            isUpToDate = true;
        }
    }

    if (isUpToDate) {
        LOG_DEBUG("Hooks_Diversion: Diversion module is already up to date ({})", DiversionPath);
        g_IsDiversionActive.store(true, std::memory_order_release);
        return true;
    }

    try {
        std::filesystem::path divFs(diversionPath);
        std::error_code ec;
        std::filesystem::create_directories(divFs.parent_path(), ec);
        if (divExists && (divAttr.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
            ::SetFileAttributesW(diversionPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        }

        if (::CopyFileW(originalPath.c_str(), diversionPath.c_str(), FALSE)) {
            LOG_INFO("Hooks_Diversion: Cloned steamclient64.dll -> diversion64.dll");
            g_IsDiversionActive.store(true, std::memory_order_release);
            return true;
        }
        const DWORD gle = ::GetLastError();
        LOG_ERROR("Hooks_Diversion: CopyFileW failed with error {}", gle);
    } catch (...) {
        LOG_ERROR("Hooks_Diversion: Exception during file clone");
    }

    return false;
}

void Install() noexcept {
    if (s_isInstalled.load(std::memory_order_acquire)) {
        return;
    }

    if (!g_hClientHooksReadyEvent) {
        g_hClientHooksReadyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    HMODULE hModule = ::GetModuleHandleW(L"kernelbase.dll");
    if (!hModule) {
        hModule = ::GetModuleHandleW(L"kernel32.dll");
    }
    if (!hModule) {
        LOG_ERROR("Hooks_Diversion: Failed to locate kernelbase.dll or kernel32.dll");
        return;
    }

    auto pfnLoadLibraryExW = reinterpret_cast<LoadLibraryExW_t>(::GetProcAddress(hModule, "LoadLibraryExW"));
    auto pfnGetModuleHandleW = reinterpret_cast<GetModuleHandleW_t>(::GetProcAddress(hModule, "GetModuleHandleW"));
    auto pfnGetModuleHandleA = reinterpret_cast<GetModuleHandleA_t>(::GetProcAddress(hModule, "GetModuleHandleA"));
    auto pfnGetModuleHandleExW = reinterpret_cast<GetModuleHandleExW_t>(::GetProcAddress(hModule, "GetModuleHandleExW"));
    auto pfnGetModuleHandleExA = reinterpret_cast<GetModuleHandleExA_t>(::GetProcAddress(hModule, "GetModuleHandleExA"));

    if (!pfnLoadLibraryExW || !pfnGetModuleHandleW || !pfnGetModuleHandleA || !pfnGetModuleHandleExW || !pfnGetModuleHandleExA) {
        LOG_ERROR("Hooks_Diversion: Failed to locate module management entrypoints");
        return;
    }

    s_origLoadLibraryExW     = pfnLoadLibraryExW;
    s_origGetModuleHandleW   = pfnGetModuleHandleW;
    s_origGetModuleHandleA   = pfnGetModuleHandleA;
    s_origGetModuleHandleExW = pfnGetModuleHandleExW;
    s_origGetModuleHandleExA = pfnGetModuleHandleExA;

    OSTPlatform::Detour::TransactionGuard guard;
    if (!guard) {
        LOG_ERROR("Hooks_Diversion: Failed to begin detour transaction");
        return;
    }

    bool attachedAll =
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&s_origLoadLibraryExW), reinterpret_cast<void*>(Hooked_LoadLibraryExW)) &&
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&s_origGetModuleHandleW), reinterpret_cast<void*>(Hooked_GetModuleHandleW)) &&
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&s_origGetModuleHandleA), reinterpret_cast<void*>(Hooked_GetModuleHandleA)) &&
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&s_origGetModuleHandleExW), reinterpret_cast<void*>(Hooked_GetModuleHandleExW)) &&
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&s_origGetModuleHandleExA), reinterpret_cast<void*>(Hooked_GetModuleHandleExA));

    if (!attachedAll) {
        LOG_ERROR("Hooks_Diversion: Failed to attach module management detours");
        return;
    }

    if (!guard.Commit()) {
        LOG_ERROR("Hooks_Diversion: Failed to commit detour transaction");
        return;
    }

    s_isInstalled.store(true, std::memory_order_release);
    LOG_INFO("Hooks_Diversion: Chokepoint detours installed successfully (LoadLibraryExW + GetModuleHandle family)");
}

void Uninstall() noexcept {
    if (!s_isInstalled.load(std::memory_order_acquire)) {
        return;
    }

    OSTPlatform::Detour::TransactionGuard guard;
    if (!guard) {
        return;
    }

    if (s_origLoadLibraryExW)     static_cast<void>(OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origLoadLibraryExW), reinterpret_cast<void*>(Hooked_LoadLibraryExW)));
    if (s_origGetModuleHandleW)   static_cast<void>(OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleW), reinterpret_cast<void*>(Hooked_GetModuleHandleW)));
    if (s_origGetModuleHandleA)   static_cast<void>(OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleA), reinterpret_cast<void*>(Hooked_GetModuleHandleA)));
    if (s_origGetModuleHandleExW) static_cast<void>(OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleExW), reinterpret_cast<void*>(Hooked_GetModuleHandleExW)));
    if (s_origGetModuleHandleExA) static_cast<void>(OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&s_origGetModuleHandleExA), reinterpret_cast<void*>(Hooked_GetModuleHandleExA)));

    static_cast<void>(guard.Commit());
    s_isInstalled.store(false, std::memory_order_release);

    if (g_hClientHooksReadyEvent) {
        ::CloseHandle(g_hClientHooksReadyEvent);
        g_hClientHooksReadyEvent = nullptr;
    }

    LOG_INFO("Hooks_Diversion: Chokepoint detours unhooked cleanly");
}

bool IsInstalled() noexcept {
    return s_isInstalled.load(std::memory_order_acquire);
}

} // namespace Hooks_Diversion
