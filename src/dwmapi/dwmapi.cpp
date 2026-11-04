// dwmapi.dll Proxy Project using ProxyBootstrap
#include <windows.h>
#include "../Common/ProxyBootstrap.hpp"

static HMODULE g_hRealDwmapi = nullptr;

static FARPROC p_DwmAttachMilContent = nullptr;
static FARPROC p_DwmDefWindowProc = nullptr;
static FARPROC p_DwmDetachMilContent = nullptr;
static FARPROC p_DwmEnableBlurBehindWindow = nullptr;
static FARPROC p_DwmEnableComposition = nullptr;
static FARPROC p_DwmEnableMMCSS = nullptr;
static FARPROC p_DwmExtendFrameIntoClientArea = nullptr;
static FARPROC p_DwmFlush = nullptr;
static FARPROC p_DwmGetColorizationColor = nullptr;
static FARPROC p_DwmGetCompositionTimingInfo = nullptr;
static FARPROC p_DwmGetGraphicsStreamClient = nullptr;
static FARPROC p_DwmGetGraphicsStreamTransformHint = nullptr;
static FARPROC p_DwmGetTransportAttributes = nullptr;
static FARPROC p_DwmGetUnmetTabRequirements = nullptr;
static FARPROC p_DwmGetWindowAttribute = nullptr;
static FARPROC p_DwmInvalidateIconicBitmaps = nullptr;
static FARPROC p_DwmIsCompositionEnabled = nullptr;
static FARPROC p_DwmModifyPreviousDxFrameDuration = nullptr;
static FARPROC p_DwmQueryThumbnailSourceSize = nullptr;
static FARPROC p_DwmRegisterThumbnail = nullptr;
static FARPROC p_DwmRenderGesture = nullptr;
static FARPROC p_DwmSetDxFrameDuration = nullptr;
static FARPROC p_DwmSetIconicLivePreviewBitmap = nullptr;
static FARPROC p_DwmSetIconicThumbnail = nullptr;
static FARPROC p_DwmSetPresentParameters = nullptr;
static FARPROC p_DwmSetWindowAttribute = nullptr;
static FARPROC p_DwmShowContact = nullptr;
static FARPROC p_DwmTetherContact = nullptr;
static FARPROC p_DwmTransitionOwnedWindow = nullptr;
static FARPROC p_DwmUnregisterThumbnail = nullptr;
static FARPROC p_DwmUpdateThumbnailProperties = nullptr;

static void EnsureRealDwmapi()
{
    static const bool ready = [] {
        g_hRealDwmapi = OST::Proxy::SystemDllLoader::Load(L"dwmapi.dll");
        if (!g_hRealDwmapi)
            return false;
        const struct { const char* Name; FARPROC* Slot; } entries[] = {
            { "DwmAttachMilContent", &p_DwmAttachMilContent },
            { "DwmDefWindowProc", &p_DwmDefWindowProc },
            { "DwmDetachMilContent", &p_DwmDetachMilContent },
            { "DwmEnableBlurBehindWindow", &p_DwmEnableBlurBehindWindow },
            { "DwmEnableComposition", &p_DwmEnableComposition },
            { "DwmEnableMMCSS", &p_DwmEnableMMCSS },
            { "DwmExtendFrameIntoClientArea", &p_DwmExtendFrameIntoClientArea },
            { "DwmFlush", &p_DwmFlush },
            { "DwmGetColorizationColor", &p_DwmGetColorizationColor },
            { "DwmGetCompositionTimingInfo", &p_DwmGetCompositionTimingInfo },
            { "DwmGetGraphicsStreamClient", &p_DwmGetGraphicsStreamClient },
            { "DwmGetGraphicsStreamTransformHint", &p_DwmGetGraphicsStreamTransformHint },
            { "DwmGetTransportAttributes", &p_DwmGetTransportAttributes },
            { "DwmGetUnmetTabRequirements", &p_DwmGetUnmetTabRequirements },
            { "DwmGetWindowAttribute", &p_DwmGetWindowAttribute },
            { "DwmInvalidateIconicBitmaps", &p_DwmInvalidateIconicBitmaps },
            { "DwmIsCompositionEnabled", &p_DwmIsCompositionEnabled },
            { "DwmModifyPreviousDxFrameDuration", &p_DwmModifyPreviousDxFrameDuration },
            { "DwmQueryThumbnailSourceSize", &p_DwmQueryThumbnailSourceSize },
            { "DwmRegisterThumbnail", &p_DwmRegisterThumbnail },
            { "DwmRenderGesture", &p_DwmRenderGesture },
            { "DwmSetDxFrameDuration", &p_DwmSetDxFrameDuration },
            { "DwmSetIconicLivePreviewBitmap", &p_DwmSetIconicLivePreviewBitmap },
            { "DwmSetIconicThumbnail", &p_DwmSetIconicThumbnail },
            { "DwmSetPresentParameters", &p_DwmSetPresentParameters },
            { "DwmSetWindowAttribute", &p_DwmSetWindowAttribute },
            { "DwmShowContact", &p_DwmShowContact },
            { "DwmTetherContact", &p_DwmTetherContact },
            { "DwmTransitionOwnedWindow", &p_DwmTransitionOwnedWindow },
            { "DwmUnregisterThumbnail", &p_DwmUnregisterThumbnail },
            { "DwmUpdateThumbnailProperties", &p_DwmUpdateThumbnailProperties },
        };
        for (const auto& e : entries)
            *e.Slot = ::GetProcAddress(g_hRealDwmapi, e.Name);
        return true;
    }();
    (void)ready;
}

#define OST_FORWARD_DWM(Ret, Name, Params, Args, FailValue)               \
    extern "C" Ret WINAPI Name Params                                     \
    {                                                                     \
        EnsureRealDwmapi();                                               \
        if (!p_##Name) return FailValue;                                  \
        return reinterpret_cast<Ret(WINAPI*)Params>(p_##Name) Args;       \
    }

OST_FORWARD_DWM(HRESULT, DwmAttachMilContent, (HWND hwnd), (hwnd), E_FAIL)
OST_FORWARD_DWM(BOOL, DwmDefWindowProc,
                (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* plResult),
                (hwnd, msg, wParam, lParam, plResult), FALSE)
OST_FORWARD_DWM(HRESULT, DwmDetachMilContent, (HWND hwnd), (hwnd), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmEnableBlurBehindWindow, (HWND hwnd, const void* pBlurBehind),
                (hwnd, pBlurBehind), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmEnableComposition, (UINT uCompositionAction), (uCompositionAction), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmEnableMMCSS, (BOOL fEnableMMCSS), (fEnableMMCSS), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmExtendFrameIntoClientArea, (HWND hwnd, const void* pMarInset),
                (hwnd, pMarInset), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmFlush, (), (), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetColorizationColor, (DWORD* pcrColorization, BOOL* pfOpaqueBlend),
                (pcrColorization, pfOpaqueBlend), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetCompositionTimingInfo, (HWND hwnd, void* pTimingInfo),
                (hwnd, pTimingInfo), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetGraphicsStreamClient, (UINT uIndex, void* pClientUuid),
                (uIndex, pClientUuid), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetGraphicsStreamTransformHint, (UINT uIndex, void* pTransform),
                (uIndex, pTransform), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetTransportAttributes,
                (BOOL* pfIsRemoting, BOOL* pfIsConnected, DWORD* pDwGeneration),
                (pfIsRemoting, pfIsConnected, pDwGeneration), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetUnmetTabRequirements, (HWND appWindow, void* value),
                (appWindow, value), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmGetWindowAttribute,
                (HWND hwnd, DWORD dwAttribute, void* pvAttribute, DWORD cbAttribute),
                (hwnd, dwAttribute, pvAttribute, cbAttribute), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmInvalidateIconicBitmaps, (HWND hwnd), (hwnd), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmIsCompositionEnabled, (BOOL* pfEnabled), (pfEnabled), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmModifyPreviousDxFrameDuration,
                (HWND hwnd, INT cRefreshes, BOOL fRelative), (hwnd, cRefreshes, fRelative), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmQueryThumbnailSourceSize, (HANDLE hThumbnail, void* pSize),
                (hThumbnail, pSize), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmRegisterThumbnail,
                (HWND hwndDestination, HWND hwndSource, HANDLE* phThumbnailId),
                (hwndDestination, hwndSource, phThumbnailId), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmRenderGesture,
                (int gt, UINT cContacts, const DWORD* pdwPointerID, const POINT* pPoints),
                (gt, cContacts, pdwPointerID, pPoints), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetDxFrameDuration, (HWND hwnd, INT cRefreshes), (hwnd, cRefreshes), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetIconicLivePreviewBitmap,
                (HWND hwnd, HBITMAP hbmp, POINT ptClient, DWORD dwSITFlags),
                (hwnd, hbmp, ptClient, dwSITFlags), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetIconicThumbnail, (HWND hwnd, HBITMAP hbmp, DWORD dwSITFlags),
                (hwnd, hbmp, dwSITFlags), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetPresentParameters, (HWND hwnd, void* pPresentParams),
                (hwnd, pPresentParams), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmSetWindowAttribute,
                (HWND hwnd, DWORD dwAttribute, const void* pvAttribute, DWORD cbAttribute),
                (hwnd, dwAttribute, pvAttribute, cbAttribute), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmShowContact, (DWORD dwPointerID, DWORD eShowContact),
                (dwPointerID, eShowContact), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmTetherContact, (DWORD dwPointerID, BOOL fEnable, POINT ptTether),
                (dwPointerID, fEnable, ptTether), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmTransitionOwnedWindow, (HWND hwnd, int target), (hwnd, target), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmUnregisterThumbnail, (HANDLE hThumbnailId), (hThumbnailId), E_FAIL)
OST_FORWARD_DWM(HRESULT, DwmUpdateThumbnailProperties, (HANDLE hThumbnailId, const void* ptnProperties),
                (hThumbnailId, ptnProperties), E_FAIL)

#undef OST_FORWARD_DWM

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID /*pvReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        EnsureRealDwmapi();
        return OST::Proxy::Bootstrap::OnAttach(hModule);
    }
    return TRUE;
}
