// dwmapi.dll HiJack Project - Dynamic System Wrapper using ProxyBootstrap
#define _DWMAPI_
#include <windows.h>
#include <dwmapi.h>
#include <atomic>
#include <mutex>
#include "../Common/ProxyBootstrap.hpp"

// ─── 1. Core State & System Loader ──────────────────────────────────────────
static std::once_flag g_dwmapiInitOnce;
static std::atomic<bool> g_dwmapiReady{false};
static HMODULE g_hRealDwmapi = nullptr;

// ─── 2. Typed Pointers for Public Core & Secondary DWM APIs ────────────────
using DllCanUnloadNow_t = HRESULT(WINAPI*)(void);
using DllGetClassObject_t = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

using PFN_DwmFlush = HRESULT(WINAPI*)(void);
using PFN_DwmIsCompositionEnabled = HRESULT(WINAPI*)(BOOL*);
using PFN_DwmGetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
using PFN_DwmSetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
using PFN_DwmExtendFrameIntoClientArea = HRESULT(WINAPI*)(HWND, const MARGINS*);
using PFN_DwmEnableBlurBehindWindow = HRESULT(WINAPI*)(HWND, const DWM_BLURBEHIND*);
using PFN_DwmDefWindowProc = BOOL(WINAPI*)(HWND, UINT, WPARAM, LPARAM, LRESULT*);
using PFN_DwmGetColorizationColor = HRESULT(WINAPI*)(DWORD*, BOOL*);

using PFN_DwmAttachMilContent = HRESULT(WINAPI*)(HWND);
using PFN_DwmDetachMilContent = HRESULT(WINAPI*)(HWND);
using PFN_DwmEnableComposition = HRESULT(WINAPI*)(UINT);
using PFN_DwmEnableMMCSS = HRESULT(WINAPI*)(BOOL);
using PFN_DwmGetCompositionTimingInfo = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
using PFN_DwmGetGraphicsStreamClient = HRESULT(WINAPI*)(UINT, UUID*);
using PFN_DwmGetGraphicsStreamTransformHint = HRESULT(WINAPI*)(UINT, MilMatrix3x2D*);
using PFN_DwmGetTransportAttributes = HRESULT(WINAPI*)(BOOL*, BOOL*, DWORD*);
using PFN_DwmGetUnmetTabRequirements = HRESULT(WINAPI*)(HWND, enum DWM_TAB_WINDOW_REQUIREMENTS*);
using PFN_DwmInvalidateIconicBitmaps = HRESULT(WINAPI*)(HWND);
using PFN_DwmModifyPreviousDxFrameDuration = HRESULT(WINAPI*)(HWND, INT, BOOL);
using PFN_DwmQueryThumbnailSourceSize = HRESULT(WINAPI*)(HTHUMBNAIL, PSIZE);
using PFN_DwmRegisterThumbnail = HRESULT(WINAPI*)(HWND, HWND, PHTHUMBNAIL);
using PFN_DwmRenderGesture = HRESULT(WINAPI*)(enum GESTURE_TYPE, UINT, const DWORD*, const POINT*);
using PFN_DwmSetDxFrameDuration = HRESULT(WINAPI*)(HWND, INT);
using PFN_DwmSetIconicLivePreviewBitmap = HRESULT(WINAPI*)(HWND, HBITMAP, POINT*, DWORD);
using PFN_DwmSetIconicThumbnail = HRESULT(WINAPI*)(HWND, HBITMAP, DWORD);
using PFN_DwmSetPresentParameters = HRESULT(WINAPI*)(HWND, DWM_PRESENT_PARAMETERS*);
using PFN_DwmShowContact = HRESULT(WINAPI*)(DWORD, enum DWM_SHOWCONTACT);
using PFN_DwmTetherContact = HRESULT(WINAPI*)(DWORD, BOOL, POINT);
using PFN_DwmTransitionOwnedWindow = HRESULT(WINAPI*)(HWND, enum DWMTRANSITION_OWNEDWINDOW_TARGET);
using PFN_DwmUnregisterThumbnail = HRESULT(WINAPI*)(HTHUMBNAIL);
using PFN_DwmUpdateThumbnailProperties = HRESULT(WINAPI*)(HTHUMBNAIL, const DWM_THUMBNAIL_PROPERTIES*);

static DllCanUnloadNow_t g_pfnDllCanUnloadNow = nullptr;
static DllGetClassObject_t g_pfnDllGetClassObject = nullptr;

static PFN_DwmFlush g_pfnDwmFlush = nullptr;
static PFN_DwmIsCompositionEnabled g_pfnDwmIsCompositionEnabled = nullptr;
static PFN_DwmGetWindowAttribute g_pfnDwmGetWindowAttribute = nullptr;
static PFN_DwmSetWindowAttribute g_pfnDwmSetWindowAttribute = nullptr;
static PFN_DwmExtendFrameIntoClientArea g_pfnDwmExtendFrameIntoClientArea = nullptr;
static PFN_DwmEnableBlurBehindWindow g_pfnDwmEnableBlurBehindWindow = nullptr;
static PFN_DwmDefWindowProc g_pfnDwmDefWindowProc = nullptr;
static PFN_DwmGetColorizationColor g_pfnDwmGetColorizationColor = nullptr;

static PFN_DwmAttachMilContent g_pfnDwmAttachMilContent = nullptr;
static PFN_DwmDetachMilContent g_pfnDwmDetachMilContent = nullptr;
static PFN_DwmEnableComposition g_pfnDwmEnableComposition = nullptr;
static PFN_DwmEnableMMCSS g_pfnDwmEnableMMCSS = nullptr;
static PFN_DwmGetCompositionTimingInfo g_pfnDwmGetCompositionTimingInfo = nullptr;
static PFN_DwmGetGraphicsStreamClient g_pfnDwmGetGraphicsStreamClient = nullptr;
static PFN_DwmGetGraphicsStreamTransformHint g_pfnDwmGetGraphicsStreamTransformHint = nullptr;
static PFN_DwmGetTransportAttributes g_pfnDwmGetTransportAttributes = nullptr;
static PFN_DwmGetUnmetTabRequirements g_pfnDwmGetUnmetTabRequirements = nullptr;
static PFN_DwmInvalidateIconicBitmaps g_pfnDwmInvalidateIconicBitmaps = nullptr;
static PFN_DwmModifyPreviousDxFrameDuration g_pfnDwmModifyPreviousDxFrameDuration = nullptr;
static PFN_DwmQueryThumbnailSourceSize g_pfnDwmQueryThumbnailSourceSize = nullptr;
static PFN_DwmRegisterThumbnail g_pfnDwmRegisterThumbnail = nullptr;
static PFN_DwmRenderGesture g_pfnDwmRenderGesture = nullptr;
static PFN_DwmSetDxFrameDuration g_pfnDwmSetDxFrameDuration = nullptr;
static PFN_DwmSetIconicLivePreviewBitmap g_pfnDwmSetIconicLivePreviewBitmap = nullptr;
static PFN_DwmSetIconicThumbnail g_pfnDwmSetIconicThumbnail = nullptr;
static PFN_DwmSetPresentParameters g_pfnDwmSetPresentParameters = nullptr;
static PFN_DwmShowContact g_pfnDwmShowContact = nullptr;
static PFN_DwmTetherContact g_pfnDwmTetherContact = nullptr;
static PFN_DwmTransitionOwnedWindow g_pfnDwmTransitionOwnedWindow = nullptr;
static PFN_DwmUnregisterThumbnail g_pfnDwmUnregisterThumbnail = nullptr;
static PFN_DwmUpdateThumbnailProperties g_pfnDwmUpdateThumbnailProperties = nullptr;

// ─── 3. Pointers for Undocumented Named APIs ─────────────────────────────────
#define DECLARE_UNDOCUMENTED_POINTER(name) static FARPROC g_pfn_##name = nullptr;
DECLARE_UNDOCUMENTED_POINTER(DwmTetherTextContact)
DECLARE_UNDOCUMENTED_POINTER(DwmpAllocateSecurityDescriptor)
DECLARE_UNDOCUMENTED_POINTER(DwmpDxGetWindowSharedSurface)
DECLARE_UNDOCUMENTED_POINTER(DwmpDxUpdateWindowSharedSurface)
DECLARE_UNDOCUMENTED_POINTER(DwmpDxgiIsThreadDesktopComposited)
DECLARE_UNDOCUMENTED_POINTER(DwmpEnableDDASupport)
DECLARE_UNDOCUMENTED_POINTER(DwmpFreeSecurityDescriptor)
DECLARE_UNDOCUMENTED_POINTER(DwmpGetColorizationParameters)
DECLARE_UNDOCUMENTED_POINTER(DwmpRenderFlick)
DECLARE_UNDOCUMENTED_POINTER(DwmpSetColorizationParameters)
DECLARE_UNDOCUMENTED_POINTER(DwmpUpdateProxyWindowForCapture)
#undef DECLARE_UNDOCUMENTED_POINTER

// ─── 4. Flat Dispatch Table for Undocumented Ordinals ────────────────────────
static FARPROC g_pfnOrdinals[256] = {};

// ─── 5. Pre-Binding on Initialization (Zero-Lock Dispatch) ──────────────────
static void LoadRealDwmapi() noexcept {
    std::call_once(g_dwmapiInitOnce, []() noexcept {
        g_hRealDwmapi = OST::Proxy::SystemDllLoader::Load(L"dwmapi.dll");
        if (g_hRealDwmapi) {
            g_pfnDllCanUnloadNow = reinterpret_cast<DllCanUnloadNow_t>(::GetProcAddress(g_hRealDwmapi, "DllCanUnloadNow"));
            g_pfnDllGetClassObject = reinterpret_cast<DllGetClassObject_t>(::GetProcAddress(g_hRealDwmapi, "DllGetClassObject"));

            #define BIND_CORE(name) g_pfn##name = reinterpret_cast<PFN_##name>(::GetProcAddress(g_hRealDwmapi, #name))
            BIND_CORE(DwmFlush);
            BIND_CORE(DwmIsCompositionEnabled);
            BIND_CORE(DwmGetWindowAttribute);
            BIND_CORE(DwmSetWindowAttribute);
            BIND_CORE(DwmExtendFrameIntoClientArea);
            BIND_CORE(DwmEnableBlurBehindWindow);
            BIND_CORE(DwmDefWindowProc);
            BIND_CORE(DwmGetColorizationColor);
            BIND_CORE(DwmAttachMilContent);
            BIND_CORE(DwmDetachMilContent);
            BIND_CORE(DwmEnableComposition);
            BIND_CORE(DwmEnableMMCSS);
            BIND_CORE(DwmGetCompositionTimingInfo);
            BIND_CORE(DwmGetGraphicsStreamClient);
            BIND_CORE(DwmGetGraphicsStreamTransformHint);
            BIND_CORE(DwmGetTransportAttributes);
            BIND_CORE(DwmGetUnmetTabRequirements);
            BIND_CORE(DwmInvalidateIconicBitmaps);
            BIND_CORE(DwmModifyPreviousDxFrameDuration);
            BIND_CORE(DwmQueryThumbnailSourceSize);
            BIND_CORE(DwmRegisterThumbnail);
            BIND_CORE(DwmRenderGesture);
            BIND_CORE(DwmSetDxFrameDuration);
            BIND_CORE(DwmSetIconicLivePreviewBitmap);
            BIND_CORE(DwmSetIconicThumbnail);
            BIND_CORE(DwmSetPresentParameters);
            BIND_CORE(DwmShowContact);
            BIND_CORE(DwmTetherContact);
            BIND_CORE(DwmTransitionOwnedWindow);
            BIND_CORE(DwmUnregisterThumbnail);
            BIND_CORE(DwmUpdateThumbnailProperties);
            #undef BIND_CORE

            #define BIND_UNDOCUMENTED(name) g_pfn_##name = ::GetProcAddress(g_hRealDwmapi, #name)
            BIND_UNDOCUMENTED(DwmTetherTextContact);
            BIND_UNDOCUMENTED(DwmpAllocateSecurityDescriptor);
            BIND_UNDOCUMENTED(DwmpDxGetWindowSharedSurface);
            BIND_UNDOCUMENTED(DwmpDxUpdateWindowSharedSurface);
            BIND_UNDOCUMENTED(DwmpDxgiIsThreadDesktopComposited);
            BIND_UNDOCUMENTED(DwmpEnableDDASupport);
            BIND_UNDOCUMENTED(DwmpFreeSecurityDescriptor);
            BIND_UNDOCUMENTED(DwmpGetColorizationParameters);
            BIND_UNDOCUMENTED(DwmpRenderFlick);
            BIND_UNDOCUMENTED(DwmpSetColorizationParameters);
            BIND_UNDOCUMENTED(DwmpUpdateProxyWindowForCapture);
            #undef BIND_UNDOCUMENTED

            constexpr int kKnownOrdinals[] = {
                103, 104, 105, 106, 107, 108, 109, 110,
                112, 113, 114,
                124,
                132,
                138, 139, 140, 141, 142,
                144, 145,
                147, 148,
                150, 151, 152, 153, 154, 155,
                157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172,
                173, 174, 175, 176, 177, 178, 179, 180, 181, 182,
                184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
                199, 200, 201, 202, 203, 204
            };
            for (int ord : kKnownOrdinals) {
                if (ord < 256) {
                    g_pfnOrdinals[ord] = ::GetProcAddress(g_hRealDwmapi, MAKEINTRESOURCEA(ord));
                }
            }
        }
        g_dwmapiReady.store(true, std::memory_order_release);
    });
}

inline void EnsureRealDwmapiLoaded() noexcept {
    if (!g_dwmapiReady.load(std::memory_order_acquire)) {
        LoadRealDwmapi();
    }
}

// ─── 6. Export Implementations ──────────────────────────────────────────────
extern "C" {

HRESULT WINAPI DllCanUnloadNow(void) {
    EnsureRealDwmapiLoaded();
    return g_pfnDllCanUnloadNow ? g_pfnDllCanUnloadNow() : S_OK;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    EnsureRealDwmapiLoaded();
    return g_pfnDllGetClassObject ? g_pfnDllGetClassObject(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

// Strongly-Typed SDK Functions
DWMAPI DwmFlush(void) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmFlush ? g_pfnDwmFlush() : S_OK;
}

DWMAPI DwmIsCompositionEnabled(BOOL* pfEnabled) {
    EnsureRealDwmapiLoaded();
    if (!g_pfnDwmIsCompositionEnabled) {
        if (pfEnabled) *pfEnabled = FALSE;
        return S_OK;
    }
    return g_pfnDwmIsCompositionEnabled(pfEnabled);
}

DWMAPI DwmGetWindowAttribute(HWND hwnd, DWORD dwAttribute, PVOID pvAttribute, DWORD cbAttribute) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetWindowAttribute ? g_pfnDwmGetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute) : E_NOTIMPL;
}

DWMAPI DwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmSetWindowAttribute ? g_pfnDwmSetWindowAttribute(hwnd, dwAttribute, pvAttribute, cbAttribute) : S_OK;
}

DWMAPI DwmExtendFrameIntoClientArea(HWND hWnd, const MARGINS* pMarInset) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmExtendFrameIntoClientArea ? g_pfnDwmExtendFrameIntoClientArea(hWnd, pMarInset) : S_OK;
}

DWMAPI DwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND* pBlurBehind) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmEnableBlurBehindWindow ? g_pfnDwmEnableBlurBehindWindow(hWnd, pBlurBehind) : S_OK;
}

DWMAPI_(BOOL) DwmDefWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* plResult) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmDefWindowProc ? g_pfnDwmDefWindowProc(hWnd, msg, wParam, lParam, plResult) : FALSE;
}

DWMAPI DwmGetColorizationColor(DWORD* pcrColorization, BOOL* pfOpaqueBlend) {
    EnsureRealDwmapiLoaded();
    if (!g_pfnDwmGetColorizationColor) {
        if (pcrColorization) *pcrColorization = 0;
        if (pfOpaqueBlend) *pfOpaqueBlend = FALSE;
        return S_OK;
    }
    return g_pfnDwmGetColorizationColor(pcrColorization, pfOpaqueBlend);
}

DWMAPI DwmAttachMilContent(HWND hwnd) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmAttachMilContent ? g_pfnDwmAttachMilContent(hwnd) : E_NOTIMPL;
}

DWMAPI DwmDetachMilContent(HWND hwnd) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmDetachMilContent ? g_pfnDwmDetachMilContent(hwnd) : E_NOTIMPL;
}

DWMAPI DwmEnableComposition(UINT uCompositionAction) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmEnableComposition ? g_pfnDwmEnableComposition(uCompositionAction) : S_OK;
}

DWMAPI DwmEnableMMCSS(BOOL fEnableMMCSS) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmEnableMMCSS ? g_pfnDwmEnableMMCSS(fEnableMMCSS) : S_OK;
}

DWMAPI DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO* pTimingInfo) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetCompositionTimingInfo ? g_pfnDwmGetCompositionTimingInfo(hwnd, pTimingInfo) : E_NOTIMPL;
}

DWMAPI DwmGetGraphicsStreamClient(UINT uIndex, UUID* pClientUuid) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetGraphicsStreamClient ? g_pfnDwmGetGraphicsStreamClient(uIndex, pClientUuid) : E_NOTIMPL;
}

DWMAPI DwmGetGraphicsStreamTransformHint(UINT uIndex, MilMatrix3x2D* pTransform) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetGraphicsStreamTransformHint ? g_pfnDwmGetGraphicsStreamTransformHint(uIndex, pTransform) : E_NOTIMPL;
}

DWMAPI DwmGetTransportAttributes(BOOL* pfIsRemoting, BOOL* pfIsConnected, DWORD* pDwGeneration) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetTransportAttributes ? g_pfnDwmGetTransportAttributes(pfIsRemoting, pfIsConnected, pDwGeneration) : E_NOTIMPL;
}

DWMAPI DwmGetUnmetTabRequirements(HWND appWindow, enum DWM_TAB_WINDOW_REQUIREMENTS* value) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmGetUnmetTabRequirements ? g_pfnDwmGetUnmetTabRequirements(appWindow, value) : E_NOTIMPL;
}

DWMAPI DwmInvalidateIconicBitmaps(HWND hwnd) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmInvalidateIconicBitmaps ? g_pfnDwmInvalidateIconicBitmaps(hwnd) : S_OK;
}

DWMAPI DwmModifyPreviousDxFrameDuration(HWND hwnd, INT cRefreshes, BOOL fRelative) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmModifyPreviousDxFrameDuration ? g_pfnDwmModifyPreviousDxFrameDuration(hwnd, cRefreshes, fRelative) : S_OK;
}

DWMAPI DwmQueryThumbnailSourceSize(HTHUMBNAIL hThumbnail, PSIZE pSize) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmQueryThumbnailSourceSize ? g_pfnDwmQueryThumbnailSourceSize(hThumbnail, pSize) : E_NOTIMPL;
}

DWMAPI DwmRegisterThumbnail(HWND hwndDestination, HWND hwndSource, PHTHUMBNAIL phThumbnailId) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmRegisterThumbnail ? g_pfnDwmRegisterThumbnail(hwndDestination, hwndSource, phThumbnailId) : E_NOTIMPL;
}

DWMAPI DwmRenderGesture(enum GESTURE_TYPE gt, UINT cContacts, const DWORD* pdwPointerID, const POINT* pPoints) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmRenderGesture ? g_pfnDwmRenderGesture(gt, cContacts, pdwPointerID, pPoints) : S_OK;
}

DWMAPI DwmSetDxFrameDuration(HWND hwnd, INT cRefreshes) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmSetDxFrameDuration ? g_pfnDwmSetDxFrameDuration(hwnd, cRefreshes) : S_OK;
}

DWMAPI DwmSetIconicLivePreviewBitmap(HWND hwnd, HBITMAP hbmp, POINT* pptClient, DWORD dwSITFlags) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmSetIconicLivePreviewBitmap ? g_pfnDwmSetIconicLivePreviewBitmap(hwnd, hbmp, pptClient, dwSITFlags) : S_OK;
}

DWMAPI DwmSetIconicThumbnail(HWND hwnd, HBITMAP hbmp, DWORD dwSITFlags) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmSetIconicThumbnail ? g_pfnDwmSetIconicThumbnail(hwnd, hbmp, dwSITFlags) : S_OK;
}

DWMAPI DwmSetPresentParameters(HWND hwnd, DWM_PRESENT_PARAMETERS* pPresentParams) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmSetPresentParameters ? g_pfnDwmSetPresentParameters(hwnd, pPresentParams) : S_OK;
}

DWMAPI DwmShowContact(DWORD dwPointerID, enum DWM_SHOWCONTACT eShowContact) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmShowContact ? g_pfnDwmShowContact(dwPointerID, eShowContact) : S_OK;
}

DWMAPI DwmTetherContact(DWORD dwPointerID, BOOL fEnable, POINT ptTether) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmTetherContact ? g_pfnDwmTetherContact(dwPointerID, fEnable, ptTether) : S_OK;
}

DWMAPI DwmTransitionOwnedWindow(HWND hwnd, enum DWMTRANSITION_OWNEDWINDOW_TARGET target) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmTransitionOwnedWindow ? g_pfnDwmTransitionOwnedWindow(hwnd, target) : S_OK;
}

DWMAPI DwmUnregisterThumbnail(HTHUMBNAIL hThumbnailId) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmUnregisterThumbnail ? g_pfnDwmUnregisterThumbnail(hThumbnailId) : S_OK;
}

DWMAPI DwmUpdateThumbnailProperties(HTHUMBNAIL hThumbnailId, const DWM_THUMBNAIL_PROPERTIES* ptnProperties) {
    EnsureRealDwmapiLoaded();
    return g_pfnDwmUpdateThumbnailProperties ? g_pfnDwmUpdateThumbnailProperties(hThumbnailId, ptnProperties) : S_OK;
}

// ─── 7. Defensive 4-Register Forwarding for Undocumented & Ordinals ────────
// Only populates RCX, RDX, R8, R9. Zero stack parameter reads or pollution.
// Returns E_NOTIMPL if target ordinal or unexported function is missing on host OS.
#define FORWARD_UNDOCUMENTED_NAMED(name) \
    HRESULT WINAPI name(void* a1, void* a2, void* a3, void* a4) { \
        EnsureRealDwmapiLoaded(); \
        using Pfn = HRESULT(WINAPI*)(void*, void*, void*, void*); \
        const auto pfn = reinterpret_cast<Pfn>(g_pfn_##name); \
        return pfn ? pfn(a1, a2, a3, a4) : E_NOTIMPL; \
    }

#define FORWARD_ORDINAL(stub, ord) \
    HRESULT WINAPI stub(void* a1, void* a2, void* a3, void* a4) { \
        EnsureRealDwmapiLoaded(); \
        using Pfn = HRESULT(WINAPI*)(void*, void*, void*, void*); \
        const auto pfn = reinterpret_cast<Pfn>(g_pfnOrdinals[ord]); \
        return pfn ? pfn(a1, a2, a3, a4) : E_NOTIMPL; \
    }

FORWARD_UNDOCUMENTED_NAMED(DwmTetherTextContact)
FORWARD_UNDOCUMENTED_NAMED(DwmpAllocateSecurityDescriptor)
FORWARD_UNDOCUMENTED_NAMED(DwmpDxGetWindowSharedSurface)
FORWARD_UNDOCUMENTED_NAMED(DwmpDxUpdateWindowSharedSurface)
FORWARD_UNDOCUMENTED_NAMED(DwmpDxgiIsThreadDesktopComposited)
FORWARD_UNDOCUMENTED_NAMED(DwmpEnableDDASupport)
FORWARD_UNDOCUMENTED_NAMED(DwmpFreeSecurityDescriptor)
FORWARD_UNDOCUMENTED_NAMED(DwmpGetColorizationParameters)
FORWARD_UNDOCUMENTED_NAMED(DwmpRenderFlick)
FORWARD_UNDOCUMENTED_NAMED(DwmpSetColorizationParameters)
FORWARD_UNDOCUMENTED_NAMED(DwmpUpdateProxyWindowForCapture)

// Ordinals (74 Noname Functions)
FORWARD_ORDINAL(DwmpOrdinal103, 103)
FORWARD_ORDINAL(DwmpOrdinal104, 104)
FORWARD_ORDINAL(DwmpOrdinal105, 105)
FORWARD_ORDINAL(DwmpOrdinal106, 106)
FORWARD_ORDINAL(DwmpOrdinal107, 107)
FORWARD_ORDINAL(DwmpOrdinal108, 108)
FORWARD_ORDINAL(DwmpOrdinal109, 109)
FORWARD_ORDINAL(DwmpOrdinal110, 110)
FORWARD_ORDINAL(DwmpOrdinal112, 112)
FORWARD_ORDINAL(DwmpOrdinal113, 113)
FORWARD_ORDINAL(DwmpOrdinal114, 114)
FORWARD_ORDINAL(DwmpOrdinal124, 124)
FORWARD_ORDINAL(DwmpOrdinal132, 132)
FORWARD_ORDINAL(DwmpOrdinal138, 138)
FORWARD_ORDINAL(DwmpOrdinal139, 139)
FORWARD_ORDINAL(DwmpOrdinal140, 140)
FORWARD_ORDINAL(DwmpOrdinal141, 141)
FORWARD_ORDINAL(DwmpOrdinal142, 142)
FORWARD_ORDINAL(DwmpOrdinal144, 144)
FORWARD_ORDINAL(DwmpOrdinal145, 145)
FORWARD_ORDINAL(DwmpOrdinal146, 146)
FORWARD_ORDINAL(DwmpOrdinal147, 147)
FORWARD_ORDINAL(DwmpOrdinal148, 148)
FORWARD_ORDINAL(DwmpOrdinal150, 150)
FORWARD_ORDINAL(DwmpOrdinal151, 151)
FORWARD_ORDINAL(DwmpOrdinal152, 152)
FORWARD_ORDINAL(DwmpOrdinal153, 153)
FORWARD_ORDINAL(DwmpOrdinal154, 154)
FORWARD_ORDINAL(DwmpOrdinal155, 155)
FORWARD_ORDINAL(DwmpOrdinal157, 157)
FORWARD_ORDINAL(DwmpOrdinal158, 158)
FORWARD_ORDINAL(DwmpOrdinal159, 159)
FORWARD_ORDINAL(DwmpOrdinal160, 160)
FORWARD_ORDINAL(DwmpOrdinal161, 161)
FORWARD_ORDINAL(DwmpOrdinal162, 162)
FORWARD_ORDINAL(DwmpOrdinal163, 163)
FORWARD_ORDINAL(DwmpOrdinal164, 164)
FORWARD_ORDINAL(DwmpOrdinal165, 165)
FORWARD_ORDINAL(DwmpOrdinal166, 166)
FORWARD_ORDINAL(DwmpOrdinal167, 167)
FORWARD_ORDINAL(DwmpOrdinal168, 168)
FORWARD_ORDINAL(DwmpOrdinal169, 169)
FORWARD_ORDINAL(DwmpOrdinal170, 170)
FORWARD_ORDINAL(DwmpOrdinal171, 171)
FORWARD_ORDINAL(DwmpOrdinal172, 172)
FORWARD_ORDINAL(DwmpOrdinal173, 173)
FORWARD_ORDINAL(DwmpOrdinal174, 174)
FORWARD_ORDINAL(DwmpOrdinal175, 175)
FORWARD_ORDINAL(DwmpOrdinal176, 176)
FORWARD_ORDINAL(DwmpOrdinal177, 177)
FORWARD_ORDINAL(DwmpOrdinal178, 178)
FORWARD_ORDINAL(DwmpOrdinal179, 179)
FORWARD_ORDINAL(DwmpOrdinal180, 180)
FORWARD_ORDINAL(DwmpOrdinal181, 181)
FORWARD_ORDINAL(DwmpOrdinal182, 182)
FORWARD_ORDINAL(DwmpOrdinal184, 184)
FORWARD_ORDINAL(DwmpOrdinal185, 185)
FORWARD_ORDINAL(DwmpOrdinal186, 186)
FORWARD_ORDINAL(DwmpOrdinal187, 187)
FORWARD_ORDINAL(DwmpOrdinal188, 188)
FORWARD_ORDINAL(DwmpOrdinal189, 189)
FORWARD_ORDINAL(DwmpOrdinal190, 190)
FORWARD_ORDINAL(DwmpOrdinal191, 191)
FORWARD_ORDINAL(DwmpOrdinal192, 192)
FORWARD_ORDINAL(DwmpOrdinal193, 193)
FORWARD_ORDINAL(DwmpOrdinal194, 194)
FORWARD_ORDINAL(DwmpOrdinal195, 195)
FORWARD_ORDINAL(DwmpOrdinal196, 196)
FORWARD_ORDINAL(DwmpOrdinal197, 197)
FORWARD_ORDINAL(DwmpOrdinal199, 199)
FORWARD_ORDINAL(DwmpOrdinal200, 200)
FORWARD_ORDINAL(DwmpOrdinal201, 201)
FORWARD_ORDINAL(DwmpOrdinal202, 202)
FORWARD_ORDINAL(DwmpOrdinal203, 203)
FORWARD_ORDINAL(DwmpOrdinal204, 204)

} // extern "C"

#undef FORWARD_UNDOCUMENTED_NAMED
#undef FORWARD_ORDINAL

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        LoadRealDwmapi();
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
