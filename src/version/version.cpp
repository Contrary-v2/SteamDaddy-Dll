// version.dll HiJack Project - Dynamic System Wrapper using ProxyBootstrap
#include <windows.h>
#include <winver.h>
#include <atomic>
#include <mutex>
#include "../Common/ProxyBootstrap.hpp"

// ─── 1. Core State & System Loader ──────────────────────────────────────────
static std::once_flag g_versionInitOnce;
static std::atomic<bool> g_versionReady{false};
static HMODULE g_hRealVersion = nullptr;

// ─── 2. Typed Function Pointers matching winver.h ───────────────────────────
using PFN_GetFileVersionInfoA = BOOL(APIENTRY*)(LPCSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoW = BOOL(APIENTRY*)(LPCWSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoSizeA = DWORD(APIENTRY*)(LPCSTR, LPDWORD);
using PFN_GetFileVersionInfoSizeW = DWORD(APIENTRY*)(LPCWSTR, LPDWORD);
using PFN_GetFileVersionInfoExA = BOOL(APIENTRY*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoExW = BOOL(APIENTRY*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
using PFN_GetFileVersionInfoSizeExA = DWORD(APIENTRY*)(DWORD, LPCSTR, LPDWORD);
using PFN_GetFileVersionInfoSizeExW = DWORD(APIENTRY*)(DWORD, LPCWSTR, LPDWORD);
using PFN_GetFileVersionInfoByHandle = BOOL(APIENTRY*)(int, HANDLE, LPVOID*, PUINT);
using PFN_VerFindFileA = DWORD(APIENTRY*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
using PFN_VerFindFileW = DWORD(APIENTRY*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
using PFN_VerInstallFileA = DWORD(APIENTRY*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
using PFN_VerInstallFileW = DWORD(APIENTRY*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
using PFN_VerLanguageNameA = DWORD(APIENTRY*)(DWORD, LPSTR, DWORD);
using PFN_VerLanguageNameW = DWORD(APIENTRY*)(DWORD, LPWSTR, DWORD);
using PFN_VerQueryValueA = BOOL(APIENTRY*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
using PFN_VerQueryValueW = BOOL(APIENTRY*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

static PFN_GetFileVersionInfoA        g_pfnGetFileVersionInfoA = nullptr;
static PFN_GetFileVersionInfoW        g_pfnGetFileVersionInfoW = nullptr;
static PFN_GetFileVersionInfoSizeA    g_pfnGetFileVersionInfoSizeA = nullptr;
static PFN_GetFileVersionInfoSizeW    g_pfnGetFileVersionInfoSizeW = nullptr;
static PFN_GetFileVersionInfoExA      g_pfnGetFileVersionInfoExA = nullptr;
static PFN_GetFileVersionInfoExW      g_pfnGetFileVersionInfoExW = nullptr;
static PFN_GetFileVersionInfoSizeExA  g_pfnGetFileVersionInfoSizeExA = nullptr;
static PFN_GetFileVersionInfoSizeExW  g_pfnGetFileVersionInfoSizeExW = nullptr;
static PFN_GetFileVersionInfoByHandle g_pfnGetFileVersionInfoByHandle = nullptr;
static PFN_VerFindFileA               g_pfnVerFindFileA = nullptr;
static PFN_VerFindFileW               g_pfnVerFindFileW = nullptr;
static PFN_VerInstallFileA            g_pfnVerInstallFileA = nullptr;
static PFN_VerInstallFileW            g_pfnVerInstallFileW = nullptr;
static PFN_VerLanguageNameA           g_pfnVerLanguageNameA = nullptr;
static PFN_VerLanguageNameW           g_pfnVerLanguageNameW = nullptr;
static PFN_VerQueryValueA             g_pfnVerQueryValueA = nullptr;
static PFN_VerQueryValueW             g_pfnVerQueryValueW = nullptr;

// ─── 3. Pre-Binding on Initialization (Zero-Lock Dispatch) ──────────────────
static void LoadRealVersion() noexcept {
    std::call_once(g_versionInitOnce, []() noexcept {
        g_hRealVersion = OST::Proxy::SystemDllLoader::Load(L"version.dll");
        if (g_hRealVersion) {
            #define BIND_API(name) g_pfn##name = reinterpret_cast<PFN_##name>(::GetProcAddress(g_hRealVersion, #name))
            BIND_API(GetFileVersionInfoA);
            BIND_API(GetFileVersionInfoW);
            BIND_API(GetFileVersionInfoSizeA);
            BIND_API(GetFileVersionInfoSizeW);
            BIND_API(GetFileVersionInfoExA);
            BIND_API(GetFileVersionInfoExW);
            BIND_API(GetFileVersionInfoSizeExA);
            BIND_API(GetFileVersionInfoSizeExW);
            BIND_API(GetFileVersionInfoByHandle);
            BIND_API(VerFindFileA);
            BIND_API(VerFindFileW);
            BIND_API(VerInstallFileA);
            BIND_API(VerInstallFileW);
            BIND_API(VerLanguageNameA);
            BIND_API(VerLanguageNameW);
            BIND_API(VerQueryValueA);
            BIND_API(VerQueryValueW);
            #undef BIND_API
        }
        g_versionReady.store(true, std::memory_order_release);
    });
}

inline void EnsureRealVersionLoaded() noexcept {
    if (!g_versionReady.load(std::memory_order_acquire)) {
        LoadRealVersion();
    }
}

// ─── 4. Export Implementations matching winver.h ────────────────────────────
extern "C" {

BOOL APIENTRY GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoA ? g_pfnGetFileVersionInfoA(lptstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

BOOL APIENTRY GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoW ? g_pfnGetFileVersionInfoW(lptstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

DWORD APIENTRY GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoSizeA ? g_pfnGetFileVersionInfoSizeA(lptstrFilename, lpdwHandle) : 0;
}

DWORD APIENTRY GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoSizeW ? g_pfnGetFileVersionInfoSizeW(lptstrFilename, lpdwHandle) : 0;
}

BOOL APIENTRY GetFileVersionInfoExA(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoExA ? g_pfnGetFileVersionInfoExA(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

BOOL APIENTRY GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoExW ? g_pfnGetFileVersionInfoExW(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData) : FALSE;
}

DWORD APIENTRY GetFileVersionInfoSizeExA(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoSizeExA ? g_pfnGetFileVersionInfoSizeExA(dwFlags, lpwstrFilename, lpdwHandle) : 0;
}

DWORD APIENTRY GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoSizeExW ? g_pfnGetFileVersionInfoSizeExW(dwFlags, lpwstrFilename, lpdwHandle) : 0;
}

BOOL APIENTRY GetFileVersionInfoByHandle(int flags, HANDLE hFile, LPVOID* ppData, PUINT puLen) {
    EnsureRealVersionLoaded();
    return g_pfnGetFileVersionInfoByHandle ? g_pfnGetFileVersionInfoByHandle(flags, hFile, ppData, puLen) : FALSE;
}

DWORD APIENTRY VerFindFileA(DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir, LPSTR szCurDir, PUINT lpuCurDirLen, LPSTR szDestDir, PUINT lpuDestDirLen) {
    EnsureRealVersionLoaded();
    return g_pfnVerFindFileA ? g_pfnVerFindFileA(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen) : 0;
}

DWORD APIENTRY VerFindFileW(DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir, LPWSTR szCurDir, PUINT lpuCurDirLen, LPWSTR szDestDir, PUINT lpuDestDirLen) {
    EnsureRealVersionLoaded();
    return g_pfnVerFindFileW ? g_pfnVerFindFileW(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen) : 0;
}

DWORD APIENTRY VerInstallFileA(DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir, LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT lpuTmpFileLen) {
    EnsureRealVersionLoaded();
    return g_pfnVerInstallFileA ? g_pfnVerInstallFileA(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen) : 0;
}

DWORD APIENTRY VerInstallFileW(DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir, LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT lpuTmpFileLen) {
    EnsureRealVersionLoaded();
    return g_pfnVerInstallFileW ? g_pfnVerInstallFileW(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen) : 0;
}

DWORD APIENTRY VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang) {
    EnsureRealVersionLoaded();
    return g_pfnVerLanguageNameA ? g_pfnVerLanguageNameA(wLang, szLang, cchLang) : 0;
}

DWORD APIENTRY VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang) {
    EnsureRealVersionLoaded();
    return g_pfnVerLanguageNameW ? g_pfnVerLanguageNameW(wLang, szLang, cchLang) : 0;
}

BOOL APIENTRY VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID * lplpBuffer, PUINT puLen) {
    EnsureRealVersionLoaded();
    return g_pfnVerQueryValueA ? g_pfnVerQueryValueA(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE;
}

BOOL APIENTRY VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID * lplpBuffer, PUINT puLen) {
    EnsureRealVersionLoaded();
    return g_pfnVerQueryValueW ? g_pfnVerQueryValueW(pBlock, lpSubBlock, lplpBuffer, puLen) : FALSE;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
