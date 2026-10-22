// xinput1_4.dll HiJack Project - Dynamic System Wrapper using ProxyBootstrap
#include <windows.h>
#include <atomic>
#include <mutex>
#include "../Common/ProxyBootstrap.hpp"

// ─── 1. Core State & System Loader ──────────────────────────────────────────
static std::once_flag g_xinputInitOnce;
static std::atomic<bool> g_xinputReady{false};
static HMODULE g_hRealXInput = nullptr;

// ─── 2. Typed Function Pointers matching Windows XInput APIs ────────────────
// Standard APIs
using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, void*);
using PFN_XInputSetState = DWORD(WINAPI*)(DWORD, void*);
using PFN_XInputGetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, void*);
using PFN_XInputEnable = void(WINAPI*)(BOOL);
using PFN_XInputGetAudioDeviceIds = DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*);
using PFN_XInputGetBatteryInformation = DWORD(WINAPI*)(DWORD, BYTE, void*);
using PFN_XInputGetKeystroke = DWORD(WINAPI*)(DWORD, DWORD, void*);

static PFN_XInputGetState              g_pfnXInputGetState = nullptr;
static PFN_XInputSetState              g_pfnXInputSetState = nullptr;
static PFN_XInputGetCapabilities       g_pfnXInputGetCapabilities = nullptr;
static PFN_XInputEnable                g_pfnXInputEnable = nullptr;
static PFN_XInputGetAudioDeviceIds     g_pfnXInputGetAudioDeviceIds = nullptr;
static PFN_XInputGetBatteryInformation g_pfnXInputGetBatteryInformation = nullptr;
static PFN_XInputGetKeystroke          g_pfnXInputGetKeystroke = nullptr;

// Undocumented Ordinals (Required for Steam Big Picture / Guide Button navigation)
static FARPROC g_pfnOrdinal100 = nullptr; // XInputGetStateEx
static FARPROC g_pfnOrdinal101 = nullptr; // XInputWaitForGuideButton
static FARPROC g_pfnOrdinal102 = nullptr; // XInputCancelGuideButtonWait
static FARPROC g_pfnOrdinal103 = nullptr; // XInputPowerOffController
static FARPROC g_pfnOrdinal104 = nullptr; // XInputGetBaseBusInformation
static FARPROC g_pfnOrdinal108 = nullptr; // XInputGetAudioDeviceIdsEx

// ─── 3. Pre-Binding on Initialization (Zero-Lock Dispatch) ──────────────────
static void LoadRealXInput() noexcept {
    std::call_once(g_xinputInitOnce, []() noexcept {
        g_hRealXInput = OST::Proxy::SystemDllLoader::Load(L"xinput1_4.dll");
        if (g_hRealXInput) {
            #define BIND_API(name) g_pfn##name = reinterpret_cast<PFN_##name>(::GetProcAddress(g_hRealXInput, #name))
            BIND_API(XInputGetState);
            BIND_API(XInputSetState);
            BIND_API(XInputGetCapabilities);
            BIND_API(XInputEnable);
            BIND_API(XInputGetAudioDeviceIds);
            BIND_API(XInputGetBatteryInformation);
            BIND_API(XInputGetKeystroke);
            #undef BIND_API

            #define BIND_ORD(num) g_pfnOrdinal##num = ::GetProcAddress(g_hRealXInput, reinterpret_cast<LPCSTR>(num))
            BIND_ORD(100);
            BIND_ORD(101);
            BIND_ORD(102);
            BIND_ORD(103);
            BIND_ORD(104);
            BIND_ORD(108);
            #undef BIND_ORD
        }
        g_xinputReady.store(true, std::memory_order_release);
    });
}

static inline void EnsureRealXInputLoaded() noexcept {
    if (!g_xinputReady.load(std::memory_order_acquire)) {
        LoadRealXInput();
    }
}

// ─── 4. Exported Wrapper Forwarders ─────────────────────────────────────────
extern "C" {

DWORD WINAPI XInputGetState(DWORD dwUserIndex, void* pState) {
    EnsureRealXInputLoaded();
    return g_pfnXInputGetState ? g_pfnXInputGetState(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD dwUserIndex, void* pVibration) {
    EnsureRealXInputLoaded();
    return g_pfnXInputSetState ? g_pfnXInputSetState(dwUserIndex, pVibration) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, void* pCapabilities) {
    EnsureRealXInputLoaded();
    return g_pfnXInputGetCapabilities ? g_pfnXInputGetCapabilities(dwUserIndex, dwFlags, pCapabilities) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL enable) {
    EnsureRealXInputLoaded();
    if (g_pfnXInputEnable) {
        g_pfnXInputEnable(enable);
    }
}

DWORD WINAPI XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId, UINT* pRenderCount, LPWSTR pCaptureDeviceId, UINT* pCaptureCount) {
    EnsureRealXInputLoaded();
    return g_pfnXInputGetAudioDeviceIds
        ? g_pfnXInputGetAudioDeviceIds(dwUserIndex, pRenderDeviceId, pRenderCount, pCaptureDeviceId, pCaptureCount)
        : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType, void* pBatteryInformation) {
    EnsureRealXInputLoaded();
    return g_pfnXInputGetBatteryInformation
        ? g_pfnXInputGetBatteryInformation(dwUserIndex, devType, pBatteryInformation)
        : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved, void* pKeystroke) {
    EnsureRealXInputLoaded();
    return g_pfnXInputGetKeystroke
        ? g_pfnXInputGetKeystroke(dwUserIndex, dwReserved, pKeystroke)
        : ERROR_DEVICE_NOT_CONNECTED;
}

// Undocumented Ordinal Wrappers
DWORD WINAPI XInputOrdinal100(DWORD a1, void* a2) {
    EnsureRealXInputLoaded();
    return g_pfnOrdinal100 ? reinterpret_cast<DWORD(WINAPI*)(DWORD, void*)>(g_pfnOrdinal100)(a1, a2) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputOrdinal101(DWORD a1, DWORD a2, void* a3) {
    EnsureRealXInputLoaded();
    return g_pfnOrdinal101 ? reinterpret_cast<DWORD(WINAPI*)(DWORD, DWORD, void*)>(g_pfnOrdinal101)(a1, a2, a3) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputOrdinal102(DWORD a1) {
    EnsureRealXInputLoaded();
    return g_pfnOrdinal102 ? reinterpret_cast<DWORD(WINAPI*)(DWORD)>(g_pfnOrdinal102)(a1) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputOrdinal103(DWORD a1) {
    EnsureRealXInputLoaded();
    return g_pfnOrdinal103 ? reinterpret_cast<DWORD(WINAPI*)(DWORD)>(g_pfnOrdinal103)(a1) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputOrdinal104(DWORD a1, void* a2) {
    EnsureRealXInputLoaded();
    return g_pfnOrdinal104 ? reinterpret_cast<DWORD(WINAPI*)(DWORD, void*)>(g_pfnOrdinal104)(a1, a2) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputOrdinal108(DWORD a1, void* a2, void* a3, void* a4, void* a5) {
    EnsureRealXInputLoaded();
    return g_pfnOrdinal108 ? reinterpret_cast<DWORD(WINAPI*)(DWORD, void*, void*, void*, void*)>(g_pfnOrdinal108)(a1, a2, a3, a4, a5) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        LoadRealXInput();
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
