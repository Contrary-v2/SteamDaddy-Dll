#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <format>
#include <string>
#include <vector>
#include <filesystem>

#include "I18n.h"
#include "IfeoEngine.h"
#include "InjectorEngine.h"
#include "OpenSteamToolBuildInfo.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace OST::Manager;

namespace {

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
};

constexpr int kWindowWidth  = 580;
constexpr int kWindowHeight = 620;

// Control IDs
enum ControlId : int {
    IdVersionText = 99,
    IdLangCombo = 100,
    IdGroupStatus,
    IdStatusText,
    IdGroupDllStatus,
    IdDllStatusText,
    IdGroupProbe,
    IdProbeText,
    IdGroupPaths,
    IdSteamLabel,
    IdSteamEdit,
    IdPayloadLabel,
    IdPayloadEdit,
    IdGroupActions,
    IdBtnInstallPortable,
    IdBtnInstallRoot,
    IdBtnUninstall,
    IdBtnHotInject,
    IdBtnToggleWatcher,
    IdBtnDeployDll,
    IdBtnRemoveDll,
    IdBtnRefresh,
    IdBtnOpenLogs,
    IdTipText,
    IdStatusBar,
    IdTimerStatus = 200
};

HWND g_hWnd                 = nullptr;
HWND g_hVersionText         = nullptr;
HWND g_hLangLabel           = nullptr;
HWND g_hLangCombo           = nullptr;
HWND g_hGroupStatus         = nullptr;
HWND g_hStatusText          = nullptr;
HWND g_hGroupDllStatus      = nullptr;
HWND g_hDllStatusText       = nullptr;
HWND g_hGroupProbe          = nullptr;
HWND g_hProbeText           = nullptr;
HWND g_hGroupPaths          = nullptr;
HWND g_hSteamLabel          = nullptr;
HWND g_hSteamEdit           = nullptr;
HWND g_hPayloadLabel        = nullptr;
HWND g_hPayloadEdit         = nullptr;
HWND g_hGroupActions        = nullptr;
HWND g_hBtnInstallPortable  = nullptr;
HWND g_hBtnInstallRoot      = nullptr;
HWND g_hBtnUninstall        = nullptr;
HWND g_hBtnHotInject        = nullptr;
HWND g_hBtnToggleWatcher    = nullptr;
HWND g_hBtnDeployDll        = nullptr;
HWND g_hBtnRemoveDll        = nullptr;
HWND g_hBtnRefresh          = nullptr;
HWND g_hBtnOpenLogs         = nullptr;
HWND g_hTipText             = nullptr;
HWND g_hStatusBar           = nullptr;
HFONT g_hFontUi             = nullptr;
HFONT g_hFontBold           = nullptr;

std::wstring g_detectedSteamPath;
std::wstring g_payloadDllPath;

void RefreshUiTexts() {
    // Window Title with Version
    const std::wstring title = std::format(L"{} [{}]", I18n::Get(StringId::AppTitle), L"" OPENSTEAMTOOL_VERSION);
    ::SetWindowTextW(g_hWnd, title.c_str());
    ::SetWindowTextW(g_hVersionText, L"" OPENSTEAMTOOL_VERSION);

    // Groups & Labels
    ::SetWindowTextW(g_hGroupStatus, I18n::Get(StringId::StatusGroup).data());
    ::SetWindowTextW(g_hGroupProbe, I18n::Get(StringId::SteamProcGroup).data());
    ::SetWindowTextW(g_hGroupPaths, I18n::Get(StringId::PathsGroup).data());
    ::SetWindowTextW(g_hSteamLabel, I18n::Get(StringId::SteamDirLabel).data());
    ::SetWindowTextW(g_hPayloadLabel, I18n::Get(StringId::PayloadLabel).data());
    ::SetWindowTextW(g_hGroupActions, I18n::Get(StringId::ActionsGroup).data());

    // Buttons
    ::SetWindowTextW(g_hBtnInstallPortable, I18n::Get(StringId::BtnInstallPortable).data());
    ::SetWindowTextW(g_hBtnInstallRoot, I18n::Get(StringId::BtnInstallRoot).data());
    ::SetWindowTextW(g_hBtnUninstall, I18n::Get(StringId::BtnUninstall).data());
    ::SetWindowTextW(g_hBtnHotInject, I18n::Get(StringId::BtnInjectNow).data());

    const bool watcherRunning = InjectorEngine::IsWatcherDaemonRunning();
    ::SetWindowTextW(g_hBtnToggleWatcher,
                     watcherRunning ? I18n::Get(StringId::BtnStopWatcher).data()
                                    : I18n::Get(StringId::BtnStartWatcher).data());

    ::SetWindowTextW(g_hBtnDeployDll, I18n::Get(StringId::BtnDeployDll).data());
    ::SetWindowTextW(g_hBtnRemoveDll, I18n::Get(StringId::BtnRemoveDll).data());
    ::SetWindowTextW(g_hBtnRefresh, I18n::Get(StringId::BtnRefresh).data());
    ::SetWindowTextW(g_hBtnOpenLogs, I18n::Get(StringId::BtnOpenLogs).data());

    // Refresh IFEO Status with Collision / Broken Detection
    const auto ifeoDetails = IfeoEngine::QueryDetails();
    if (ifeoDetails.status == IfeoStatus::InstalledRoot &&
        !ifeoDetails.registeredPayloadPath.empty() && std::filesystem::exists(ifeoDetails.registeredPayloadPath)) {
        g_payloadDllPath = ifeoDetails.registeredPayloadPath;
    } else {
        g_payloadDllPath = IfeoEngine::GetDefaultPayloadDllPath().wstring();
    }

    switch (ifeoDetails.status) {
        case IfeoStatus::InstalledPortable:
            ::SetWindowTextW(g_hStatusText, I18n::Get(StringId::StatusInstalledPortable).data());
            break;
        case IfeoStatus::InstalledRoot:
            ::SetWindowTextW(g_hStatusText, I18n::Get(StringId::StatusInstalledRoot).data());
            break;
        case IfeoStatus::ConflictOtherInstance:
            ::SetWindowTextW(g_hStatusText, I18n::Get(StringId::StatusConflictOtherInstance).data());
            break;
        case IfeoStatus::BrokenOrphaned:
            ::SetWindowTextW(g_hStatusText, I18n::Get(StringId::StatusBrokenOrphaned).data());
            break;
        default:
            ::SetWindowTextW(g_hStatusText, I18n::Get(StringId::StatusNotInstalled).data());
            break;
    }

    // Refresh Standard DLL Deployment Status
    ::SetWindowTextW(g_hGroupDllStatus, I18n::Get(StringId::DllStatusGroup).data());
    const auto dllDetails = IfeoEngine::QueryStandardDllDetails(g_detectedSteamPath);
    switch (dllDetails.status) {
        case DllDeployStatus::DeployedBoth:
            ::SetWindowTextW(g_hDllStatusText, I18n::Get(StringId::DllStatusDeployedBoth).data());
            break;
        case DllDeployStatus::DeployedVersion:
            ::SetWindowTextW(g_hDllStatusText, I18n::Get(StringId::DllStatusDeployedVersion).data());
            break;
        case DllDeployStatus::DeployedDwmapi:
            ::SetWindowTextW(g_hDllStatusText, I18n::Get(StringId::DllStatusDeployedDwmapi).data());
            break;
        case DllDeployStatus::MissingPayload:
            ::SetWindowTextW(g_hDllStatusText, I18n::Get(StringId::DllStatusMissingPayload).data());
            break;
        default:
            ::SetWindowTextW(g_hDllStatusText, I18n::Get(StringId::DllStatusNotDeployed).data());
            break;
    }

    // Refresh Steam Client Probe & Safety Lock
    DWORD steamPid = 0;
    const bool isSteamRunning = IfeoEngine::IsSteamRunning(steamPid);
    if (isSteamRunning) {
        if (IfeoEngine::IsPayloadInjected(steamPid)) {
            std::wstring probeMsg = I18n::Format(StringId::SteamRunningInjected, steamPid);
            ::SetWindowTextW(g_hProbeText, probeMsg.c_str());
        } else {
            std::wstring probeMsg = I18n::Format(StringId::SteamRunningNotInjected, steamPid);
            ::SetWindowTextW(g_hProbeText, probeMsg.c_str());
        }
        ::SetWindowTextW(g_hTipText, I18n::Get(StringId::TipSteamRunningLocked).data());
        ::EnableWindow(g_hBtnInstallPortable, FALSE);
        ::EnableWindow(g_hBtnInstallRoot, FALSE);
        ::EnableWindow(g_hBtnUninstall, FALSE);
        ::EnableWindow(g_hBtnHotInject, FALSE);
        ::EnableWindow(g_hBtnToggleWatcher, FALSE);
        ::EnableWindow(g_hBtnDeployDll, FALSE);
        ::EnableWindow(g_hBtnRemoveDll, FALSE);
    } else {
        ::SetWindowTextW(g_hProbeText, I18n::Get(StringId::SteamNotRunning).data());
        ::SetWindowTextW(g_hTipText, I18n::Get(StringId::TipPortable).data());
        ::EnableWindow(g_hBtnInstallPortable, TRUE);
        ::EnableWindow(g_hBtnInstallRoot, TRUE);
        ::EnableWindow(g_hBtnUninstall, TRUE);
        ::EnableWindow(g_hBtnHotInject, TRUE);
        ::EnableWindow(g_hBtnToggleWatcher, TRUE);
        ::EnableWindow(g_hBtnDeployDll, TRUE);
        ::EnableWindow(g_hBtnRemoveDll, TRUE);
    }

    // Refresh Path Edits
    if (g_detectedSteamPath.empty()) {
        ::SetWindowTextW(g_hSteamEdit, I18n::Get(StringId::SteamDirNotFound).data());
    } else {
        ::SetWindowTextW(g_hSteamEdit, g_detectedSteamPath.c_str());
    }

    if (g_payloadDllPath.empty() || !std::filesystem::exists(g_payloadDllPath)) {
        ::SetWindowTextW(g_hPayloadEdit, I18n::Get(StringId::PayloadNotFound).data());
    } else {
        ::SetWindowTextW(g_hPayloadEdit, g_payloadDllPath.c_str());
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusReady).data());
}

bool EnsureSteamNotRunning() {
    DWORD steamPid = 0;
    if (IfeoEngine::IsSteamRunning(steamPid)) {
        std::wstring msg = I18n::Format(StringId::MsgSteamRunningBlocked, steamPid);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

bool CheckConflictBeforeInstall() {
    const auto details = IfeoEngine::QueryDetails();
    if (details.status == IfeoStatus::ConflictOtherInstance) {
        std::wstring prompt = I18n::Format(
            StringId::MsgConflictConfirmPrompt,
            details.registeredExePath,
            IfeoEngine::GetSelfExePath().wstring()
        );
        const int choice = ::MessageBoxW(
            g_hWnd, prompt.c_str(),
            I18n::Get(StringId::MsgConflictConfirmTitle).data(),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2
        );
        return (choice == IDYES);
    }
    return true;
}

void BringManagerToFront() {
    if (g_hWnd && ::IsWindow(g_hWnd)) {
        ::ShowWindow(g_hWnd, SW_RESTORE);
        ::SetForegroundWindow(g_hWnd);
        ::BringWindowToTop(g_hWnd);
        ::SetActiveWindow(g_hWnd);
    }
}

void OnInstallPortable() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    if (!CheckConflictBeforeInstall()) {
        return;
    }

    // Portable mode strictly binds to manager's directory, decoupling from UI display state
    const auto portablePayload = IfeoEngine::GetDefaultPayloadDllPath();
    g_payloadDllPath = portablePayload.wstring();

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    std::wstring err;
    bool ok = false;
    if (IfeoEngine::IsRunningElevated()) {
        ok = IfeoEngine::InstallIfeo(portablePayload, err, false);
    } else {
        const std::wstring params = L"/install-portable \"" + portablePayload.wstring() + L"\"";
        ok = IfeoEngine::ElevateAndExecute(g_hWnd, params, err);
    }

    BringManagerToFront();
    RefreshUiTexts();
    if (ok) {
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgInstallPortableSuccess).data(),
                      I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    } else if (err != L"Cancelled by user.") {
        std::wstring msg = I18n::Format(StringId::MsgInstallFailed, err);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }
}

void OnInstallRoot() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    if (!CheckConflictBeforeInstall()) {
        return;
    }

    if (g_detectedSteamPath.empty()) {
        BringManagerToFront();
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgSteamDirMissing).data(),
                      I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    const auto localPayload = IfeoEngine::GetDefaultPayloadDllPath();
    std::wstring err;
    bool ok = false;
    if (IfeoEngine::IsRunningElevated()) {
        if (!IfeoEngine::DeployToSteamRoot(g_detectedSteamPath, localPayload, err)) {
            BringManagerToFront();
            RefreshUiTexts();
            std::wstring msg = I18n::Format(StringId::MsgDeployFailed, err);
            ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
            return;
        }
        const std::filesystem::path rootDll = std::filesystem::path(g_detectedSteamPath) / L"OpenSteamTool.dll";
        ok = IfeoEngine::InstallIfeo(rootDll, err, true);
    } else {
        std::wstring cleanSteamDir = g_detectedSteamPath;
        while (!cleanSteamDir.empty() && (cleanSteamDir.back() == L'\\' || cleanSteamDir.back() == L'/')) {
            cleanSteamDir.pop_back();
        }
        const std::wstring params = L"/install-root \"" + cleanSteamDir + L"\" \"" + localPayload.wstring() + L"\"";
        ok = IfeoEngine::ElevateAndExecute(g_hWnd, params, err);
    }

    BringManagerToFront();
    RefreshUiTexts();
    if (ok) {
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgInstallRootSuccess).data(),
                      I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    } else if (err != L"Cancelled by user.") {
        std::wstring msg = I18n::Format(StringId::MsgInstallFailed, err);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }
}

void OnUninstall() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    const auto details = IfeoEngine::QueryDetails();
    if (details.status == IfeoStatus::ConflictOtherInstance) {
        BringManagerToFront();
        std::wstring prompt = I18n::Format(
            StringId::MsgUninstallOtherConfirm,
            details.registeredExePath
        );
        const int choice = ::MessageBoxW(
            g_hWnd, prompt.c_str(),
            I18n::Get(StringId::MsgConflictConfirmTitle).data(),
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST
        );
        if (choice != IDYES) {
            return;
        }
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    std::wstring err;
    bool ok = false;
    if (IfeoEngine::IsRunningElevated()) {
        ok = IfeoEngine::UninstallIfeo(err);
    } else {
        ok = IfeoEngine::ElevateAndExecute(g_hWnd, L"/uninstall", err);
    }

    BringManagerToFront();
    RefreshUiTexts();
    if (ok) {
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgUninstallSuccess).data(),
                      I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    } else if (err != L"Cancelled by user.") {
        std::wstring msg = I18n::Format(StringId::MsgUninstallFailed, err);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    }
}

void OnSingleInject() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    if (!std::filesystem::exists(g_payloadDllPath)) {
        std::wstring msg = I18n::Format(StringId::MsgDllMissing, g_payloadDllPath);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
        return;
    }

    if (g_detectedSteamPath.empty()) {
        g_detectedSteamPath = IfeoEngine::DetectSteamInstallPath();
    }

    if (g_detectedSteamPath.empty()) {
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgSteamDirMissing).data(),
                      I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
        return;
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    DWORD targetPid = 0;
    std::wstring err;
    if (IfeoEngine::LaunchSteamWithInjection(g_detectedSteamPath, g_payloadDllPath, targetPid, err)) {
        RefreshUiTexts();
        std::wstring msg = I18n::Format(StringId::MsgInjectSuccess, targetPid);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION);
    } else {
        RefreshUiTexts();
        std::wstring msg = I18n::Format(StringId::MsgInjectFailed, err);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
    }
}

void OnToggleWatcher() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    if (InjectorEngine::IsWatcherDaemonRunning()) {
        (void)InjectorEngine::StopWatcherDaemon();
        (void)InjectorEngine::SetAutoStart(false);
        ::Sleep(250);
        RefreshUiTexts();
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgWatcherStopped).data(),
                      I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION);
    } else {
        if (!std::filesystem::exists(g_payloadDllPath)) {
            std::wstring msg = I18n::Format(StringId::MsgDllMissing, g_payloadDllPath);
            ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
            return;
        }
        if (InjectorEngine::StartWatcherDaemonProcess(g_payloadDllPath)) {
            (void)InjectorEngine::SetAutoStart(true, g_payloadDllPath);
            ::Sleep(250);
            RefreshUiTexts();
            ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgWatcherStarted).data(),
                          I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION);
        } else {
            ::MessageBoxW(g_hWnd, L"Failed to start background watcher daemon process.",
                          I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
        }
    }
}

void OnOpenLogs() {
    std::filesystem::path logDir;

    const auto ifeoDetails = IfeoEngine::QueryDetails();
    const auto dllDetails  = IfeoEngine::QueryStandardDllDetails(g_detectedSteamPath);

    // If Standard DLL is deployed OR IFEO is installed in Steam root mode, open Steam's root log directory
    const bool isSteamRootActive = (ifeoDetails.status == IfeoStatus::InstalledRoot) ||
                                   (dllDetails.status != DllDeployStatus::NotDeployed);

    auto resolveLogDir = [](const std::filesystem::path& base) -> std::filesystem::path {
        auto d1 = base / "steamdaddy";
        if (std::filesystem::exists(d1)) return d1;
        auto d2 = base / "opensteamtool";
        if (std::filesystem::exists(d2)) return d2;
        return d1;
    };

    if (isSteamRootActive && !g_detectedSteamPath.empty()) {
        logDir = resolveLogDir(std::filesystem::path(g_detectedSteamPath));
    } else {
        logDir = resolveLogDir(IfeoEngine::GetSelfDir());
        // Defensive fallback: if selfDir/steamdaddy doesn't exist but Steam log dir exists, open Steam's
        if (!std::filesystem::exists(logDir) && !g_detectedSteamPath.empty()) {
            auto steamLogDir = resolveLogDir(std::filesystem::path(g_detectedSteamPath));
            if (std::filesystem::exists(steamLogDir)) {
                logDir = steamLogDir;
            }
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    ::ShellExecuteW(nullptr, L"open", logDir.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
}

void OnDeployDll() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    if (g_detectedSteamPath.empty()) {
        g_detectedSteamPath = IfeoEngine::DetectSteamInstallPath();
    }
    if (g_detectedSteamPath.empty()) {
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgSteamDirMissing).data(),
                      I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
        return;
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    std::wstring err;
    if (IfeoEngine::DeployStandardDll(g_detectedSteamPath, g_payloadDllPath, err)) {
        RefreshUiTexts();
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgDeployDllSuccess).data(),
                      I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION);
    } else {
        RefreshUiTexts();
        std::wstring msg = I18n::Format(StringId::MsgDeployDllFailed, err);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
    }
}

void OnRemoveDll() {
    if (!EnsureSteamNotRunning()) {
        return;
    }

    if (g_detectedSteamPath.empty()) {
        g_detectedSteamPath = IfeoEngine::DetectSteamInstallPath();
    }
    if (g_detectedSteamPath.empty()) {
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgSteamDirMissing).data(),
                      I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
        return;
    }

    ::SetWindowTextW(g_hStatusBar, I18n::Get(StringId::StatusProcessing).data());
    ::UpdateWindow(g_hStatusBar);

    std::wstring err;
    if (IfeoEngine::RemoveStandardDll(g_detectedSteamPath, err)) {
        RefreshUiTexts();
        ::MessageBoxW(g_hWnd, I18n::Get(StringId::MsgRemoveDllSuccess).data(),
                      I18n::Get(StringId::MsgSuccessTitle).data(), MB_OK | MB_ICONINFORMATION);
    } else {
        RefreshUiTexts();
        std::wstring msg = I18n::Format(StringId::MsgRemoveDllFailed, err);
        ::MessageBoxW(g_hWnd, msg.c_str(), I18n::Get(StringId::MsgErrorTitle).data(), MB_OK | MB_ICONERROR);
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_hWnd = hWnd;
            // Apply font
            NONCLIENTMETRICSW ncm{ sizeof(ncm) };
            ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            g_hFontUi = ::CreateFontIndirectW(&ncm.lfMessageFont);

            LOGFONTW lfBold = ncm.lfMessageFont;
            lfBold.lfWeight = FW_BOLD;
            g_hFontBold = ::CreateFontIndirectW(&lfBold);

            // 0. Version label (Upper left)
            g_hVersionText = CreateWindowW(L"STATIC", L"" OPENSTEAMTOOL_VERSION,
                                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           20, 14, 180, 20, hWnd, reinterpret_cast<HMENU>(IdVersionText), nullptr, nullptr);

            // 1. Language switcher (Upper right)
            g_hLangLabel = CreateWindowW(L"STATIC", L"Language / 语言 / Idioma:",
                                         WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                         220, 16, 190, 22, hWnd, nullptr, nullptr, nullptr);
            g_hLangCombo = CreateWindowW(L"COMBOBOX", nullptr,
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                         420, 12, 120, 120, hWnd, reinterpret_cast<HMENU>(IdLangCombo), nullptr, nullptr);
            ::SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
            ::SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"简体中文"));
            ::SendMessageW(g_hLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Español"));
            ::SendMessageW(g_hLangCombo, CB_SETCURSEL, static_cast<WPARAM>(I18n::GetLanguage()), 0);

            // 2. Status Group Boxes (Left: IFEO Status, Right: Standard DLL Status)
            g_hGroupStatus = CreateWindowW(L"BUTTON", L"System IFEO Status",
                                           WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                           20, 42, 252, 55, hWnd, reinterpret_cast<HMENU>(IdGroupStatus), nullptr, nullptr);
            g_hStatusText = CreateWindowW(L"STATIC", L"...",
                                          WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                          32, 65, 232, 22, hWnd, reinterpret_cast<HMENU>(IdStatusText), nullptr, nullptr);

            g_hGroupDllStatus = CreateWindowW(L"BUTTON", L"Standard DLL Status",
                                              WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                              280, 42, 260, 55, hWnd, reinterpret_cast<HMENU>(IdGroupDllStatus), nullptr, nullptr);
            g_hDllStatusText = CreateWindowW(L"STATIC", L"...",
                                             WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                             292, 65, 240, 22, hWnd, reinterpret_cast<HMENU>(IdDllStatusText), nullptr, nullptr);

            // 3. Probe Group Box
            g_hGroupProbe = CreateWindowW(L"BUTTON", L"Steam Client Probe",
                                          WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                          20, 102, 520, 55, hWnd, reinterpret_cast<HMENU>(IdGroupProbe), nullptr, nullptr);
            g_hProbeText = CreateWindowW(L"STATIC", L"...",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         35, 125, 490, 22, hWnd, reinterpret_cast<HMENU>(IdProbeText), nullptr, nullptr);

            // 4. Paths Group Box
            g_hGroupPaths = CreateWindowW(L"BUTTON", L"Target & Paths",
                                          WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                          20, 162, 520, 115, hWnd, reinterpret_cast<HMENU>(IdGroupPaths), nullptr, nullptr);

            g_hSteamLabel = CreateWindowW(L"STATIC", L"Steam Path:",
                                          WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          35, 187, 100, 20, hWnd, reinterpret_cast<HMENU>(IdSteamLabel), nullptr, nullptr);
            g_hSteamEdit  = CreateWindowW(L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | WS_BORDER,
                                          140, 185, 380, 22, hWnd, reinterpret_cast<HMENU>(IdSteamEdit), nullptr, nullptr);

            g_hPayloadLabel = CreateWindowW(L"STATIC", L"OST Payload:",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            35, 222, 100, 20, hWnd, reinterpret_cast<HMENU>(IdPayloadLabel), nullptr, nullptr);
            g_hPayloadEdit  = CreateWindowW(L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL | WS_BORDER,
                                            140, 220, 380, 22, hWnd, reinterpret_cast<HMENU>(IdPayloadEdit), nullptr, nullptr);

            // 5. Actions Group Box
            g_hGroupActions = CreateWindowW(L"BUTTON", L"Operations",
                                            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                            20, 287, 520, 220, hWnd, reinterpret_cast<HMENU>(IdGroupActions), nullptr, nullptr);

            g_hBtnInstallPortable = CreateWindowW(L"BUTTON", L"Install IFEO (Portable Mode)",
                                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                                  35, 310, 310, 32, hWnd, reinterpret_cast<HMENU>(IdBtnInstallPortable), nullptr, nullptr);
            g_hBtnDeployDll = CreateWindowW(L"BUTTON", L"Deploy Standard DLL",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                            365, 310, 160, 41, hWnd, reinterpret_cast<HMENU>(IdBtnDeployDll), nullptr, nullptr);

            g_hBtnInstallRoot = CreateWindowW(L"BUTTON", L"Install IFEO (Steam Root Mode)",
                                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                              35, 348, 310, 32, hWnd, reinterpret_cast<HMENU>(IdBtnInstallRoot), nullptr, nullptr);
            g_hBtnRemoveDll = CreateWindowW(L"BUTTON", L"Remove DLL",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                            365, 358, 160, 41, hWnd, reinterpret_cast<HMENU>(IdBtnRemoveDll), nullptr, nullptr);

            g_hBtnUninstall = CreateWindowW(L"BUTTON", L"Uninstall IFEO",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                            35, 386, 310, 32, hWnd, reinterpret_cast<HMENU>(IdBtnUninstall), nullptr, nullptr);

            g_hBtnHotInject = CreateWindowW(L"BUTTON", L"Launch & Inject Steam",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                            35, 426, 310, 32, hWnd, reinterpret_cast<HMENU>(IdBtnHotInject), nullptr, nullptr);
            g_hBtnRefresh = CreateWindowW(L"BUTTON", L"Refresh Status",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                          365, 407, 160, 41, hWnd, reinterpret_cast<HMENU>(IdBtnRefresh), nullptr, nullptr);

            g_hBtnToggleWatcher = CreateWindowW(L"BUTTON", L"Start Background Watcher",
                                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                                35, 464, 310, 32, hWnd, reinterpret_cast<HMENU>(IdBtnToggleWatcher), nullptr, nullptr);
            g_hBtnOpenLogs = CreateWindowW(L"BUTTON", L"Open Logs",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                           365, 455, 160, 41, hWnd, reinterpret_cast<HMENU>(IdBtnOpenLogs), nullptr, nullptr);

            g_hTipText = CreateWindowW(L"STATIC", L"",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       20, 520, 520, 35, hWnd, reinterpret_cast<HMENU>(IdTipText), nullptr, nullptr);

            // Set font to all controls
            const HWND kControls[] = {
                g_hVersionText, g_hLangLabel, g_hLangCombo, g_hGroupStatus, g_hGroupDllStatus, g_hGroupProbe, g_hGroupPaths, g_hSteamLabel,
                g_hSteamEdit, g_hPayloadLabel, g_hPayloadEdit, g_hGroupActions,
                g_hBtnInstallPortable, g_hBtnInstallRoot, g_hBtnUninstall,
                g_hBtnHotInject, g_hBtnToggleWatcher,
                g_hBtnDeployDll, g_hBtnRemoveDll,
                g_hBtnRefresh, g_hBtnOpenLogs, g_hTipText
            };
            for (HWND hCtrl : kControls) {
                if (hCtrl) ::SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontUi), TRUE);
            }
            if (g_hStatusText) {
                ::SendMessageW(g_hStatusText, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
            }
            if (g_hDllStatusText) {
                ::SendMessageW(g_hDllStatusText, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
            }
            if (g_hProbeText) {
                ::SendMessageW(g_hProbeText, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
            }

            // Show UAC shield on actions if running as standard user
            #ifndef BCM_SETSHIELD
            #define BCM_SETSHIELD 0x160C
            #endif
            if (!IfeoEngine::IsRunningElevated()) {
                ::SendMessageW(g_hBtnInstallPortable, BCM_SETSHIELD, 0, TRUE);
                ::SendMessageW(g_hBtnInstallRoot, BCM_SETSHIELD, 0, TRUE);
                ::SendMessageW(g_hBtnUninstall, BCM_SETSHIELD, 0, TRUE);
            }

            // Detect environment
            g_detectedSteamPath = IfeoEngine::DetectSteamInstallPath();
            g_payloadDllPath    = IfeoEngine::GetDefaultPayloadDllPath().wstring();

            RefreshUiTexts();

            // Heartbeat timer to keep button state and process probes dynamically in sync
            ::SetTimer(hWnd, IdTimerStatus, 1000, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == IdTimerStatus) {
                // Keep watcher button state dynamically in sync with process liveness
                const bool watcherRunning = InjectorEngine::IsWatcherDaemonRunning();
                const std::wstring_view expectedText = watcherRunning
                    ? I18n::Get(StringId::BtnStopWatcher)
                    : I18n::Get(StringId::BtnStartWatcher);
                wchar_t currentText[128]{};
                ::GetWindowTextW(g_hBtnToggleWatcher, currentText, 128);
                if (expectedText != currentText) {
                    ::SetWindowTextW(g_hBtnToggleWatcher, expectedText.data());
                }

                // Also keep probe text and safety lock dynamically in sync with steam status
                DWORD steamPid = 0;
                const bool isSteamRunning = IfeoEngine::IsSteamRunning(steamPid);
                if (isSteamRunning) {
                    if (IfeoEngine::IsPayloadInjected(steamPid)) {
                        std::wstring probeMsg = I18n::Format(StringId::SteamRunningInjected, steamPid);
                        ::SetWindowTextW(g_hProbeText, probeMsg.c_str());
                    } else {
                        std::wstring probeMsg = I18n::Format(StringId::SteamRunningNotInjected, steamPid);
                        ::SetWindowTextW(g_hProbeText, probeMsg.c_str());
                    }

                    if (::IsWindowEnabled(g_hBtnInstallPortable)) {
                        ::SetWindowTextW(g_hTipText, I18n::Get(StringId::TipSteamRunningLocked).data());
                        ::EnableWindow(g_hBtnInstallPortable, FALSE);
                        ::EnableWindow(g_hBtnInstallRoot, FALSE);
                        ::EnableWindow(g_hBtnUninstall, FALSE);
                        ::EnableWindow(g_hBtnHotInject, FALSE);
                        ::EnableWindow(g_hBtnToggleWatcher, FALSE);
                        ::EnableWindow(g_hBtnDeployDll, FALSE);
                        ::EnableWindow(g_hBtnRemoveDll, FALSE);
                    }
                } else {
                    ::SetWindowTextW(g_hProbeText, I18n::Get(StringId::SteamNotRunning).data());

                    if (!::IsWindowEnabled(g_hBtnInstallPortable)) {
                        ::SetWindowTextW(g_hTipText, I18n::Get(StringId::TipPortable).data());
                        ::EnableWindow(g_hBtnInstallPortable, TRUE);
                        ::EnableWindow(g_hBtnInstallRoot, TRUE);
                        ::EnableWindow(g_hBtnUninstall, TRUE);
                        ::EnableWindow(g_hBtnHotInject, TRUE);
                        ::EnableWindow(g_hBtnToggleWatcher, TRUE);
                        ::EnableWindow(g_hBtnDeployDll, TRUE);
                        ::EnableWindow(g_hBtnRemoveDll, TRUE);
                    }
                }
            }
            return 0;
        }

        case WM_COMMAND: {
            const int wmId = LOWORD(wParam);
            const int wmEvent = HIWORD(wParam);

            if (wmId == IdLangCombo && wmEvent == CBN_SELCHANGE) {
                const auto sel = static_cast<int>(::SendMessageW(g_hLangCombo, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(kLanguageCount)) {
                    I18n::SetLanguage(static_cast<Language>(sel));
                    RefreshUiTexts();
                }
                return 0;
            }

            switch (wmId) {
                case IdBtnInstallPortable:
                    OnInstallPortable();
                    break;
                case IdBtnInstallRoot:
                    OnInstallRoot();
                    break;
                case IdBtnUninstall:
                    OnUninstall();
                    break;
                case IdBtnHotInject:
                    OnSingleInject();
                    break;
                case IdBtnToggleWatcher:
                    OnToggleWatcher();
                    break;
                case IdBtnDeployDll:
                    OnDeployDll();
                    break;
                case IdBtnRemoveDll:
                    OnRemoveDll();
                    break;
                case IdBtnRefresh:
                    g_detectedSteamPath = IfeoEngine::DetectSteamInstallPath();
                    RefreshUiTexts();
                    break;
                case IdBtnOpenLogs:
                    OnOpenLogs();
                    break;
                default:
                    break;
            }
            return 0;
        }

        case WM_DESTROY: {
            ::KillTimer(hWnd, IdTimerStatus);
            if (g_hFontUi)   ::DeleteObject(g_hFontUi);
            if (g_hFontBold) ::DeleteObject(g_hFontBold);
            ::PostQuitMessage(0);
            return 0;
        }

        default:
            return ::DefWindowProcW(hWnd, message, wParam, lParam);
    }
}

void RecordElevatedError(const std::wstring& err) {
    wchar_t tempPath[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, tempPath) > 0) {
        const std::filesystem::path logFile = std::filesystem::path(tempPath) / L"OpenSteamTool_elevated.log";
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, logFile.c_str(), L"w, ccs=UTF-8") == 0 && fp) {
            fputws(err.c_str(), fp);
            fclose(fp);
        }
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // 1. Check if invoked as IFEO launch interceptor (Runs as standard user, zero UAC popups)
    if (__argc >= 2 && (_wcsicmp(__wargv[1], L"/launch") == 0 || _wcsicmp(__wargv[1], L"-launch") == 0)) {
        // Fast, zero-window background launch & injection (bypasses IFEO natively)
        return IfeoEngine::LaunchHostAndInject(__argc, __wargv) ? 0 : 1;
    }

    // 2. Headless Background Watcher Daemon Mode
    if (__argc >= 2 && (_wcsicmp(__wargv[1], L"/watch") == 0 || _wcsicmp(__wargv[1], L"-w") == 0 || _wcsicmp(__wargv[1], L"/daemon") == 0)) {
        const std::filesystem::path dllPath = (__argc >= 3) ? std::filesystem::path(__wargv[2]) : IfeoEngine::GetDefaultPayloadDllPath();
        return InjectorEngine::RunWatcherLoop(dllPath);
    }

    // 3. Headless One-Shot Injection Mode
    if (__argc >= 2 && (_wcsicmp(__wargv[1], L"/inject") == 0 || _wcsicmp(__wargv[1], L"-i") == 0 || _wcsicmp(__wargv[1], L"/silent") == 0)) {
        const std::filesystem::path dllPath = (__argc >= 3) ? std::filesystem::path(__wargv[2]) : IfeoEngine::GetDefaultPayloadDllPath();
        DWORD targetPid = 0;
        if (IfeoEngine::IsSteamRunning(targetPid)) {
            std::wstring detail;
            const auto res = InjectorEngine::InjectToRunningSteam(dllPath, targetPid, detail);
            return (res == InjectResult::Success || res == InjectResult::AlreadyInjected) ? 0 : 1;
        } else {
            const auto steamDir = IfeoEngine::DetectSteamInstallPath();
            std::wstring err;
            return IfeoEngine::LaunchSteamWithInjection(steamDir, dllPath, targetPid, err) ? 0 : 1;
        }
    }

    // 4. Elevated Worker Actions (Only run when explicitly elevated by GUI)
    if (__argc >= 2) {
        if (_wcsicmp(__wargv[1], L"/install-portable") == 0 ||
            _wcsicmp(__wargv[1], L"/install-root") == 0 ||
            _wcsicmp(__wargv[1], L"/uninstall") == 0 ||
            _wcsicmp(__wargv[1], L"/deploy-dll") == 0 ||
            _wcsicmp(__wargv[1], L"/remove-dll") == 0) {
            DWORD dummyPid = 0;
            if (IfeoEngine::IsSteamRunning(dummyPid)) {
                RecordElevatedError(L"Steam is currently running (PID " + std::to_wstring(dummyPid) + L"). Please close Steam completely before proceeding.");
                return 2; // Blocked: Steam active
            }
        }

        if (_wcsicmp(__wargv[1], L"/install-portable") == 0 && __argc >= 3) {
            std::wstring err;
            if (!IfeoEngine::InstallIfeo(__wargv[2], err)) {
                RecordElevatedError(err);
                return 1;
            }
            return 0;
        }
        if (_wcsicmp(__wargv[1], L"/install-root") == 0 && __argc >= 4) {
            std::wstring err;
            if (!IfeoEngine::DeployToSteamRoot(__wargv[2], __wargv[3], err)) {
                RecordElevatedError(err);
                return 1;
            }
            const std::filesystem::path rootDll = std::filesystem::path(__wargv[2]) / L"OpenSteamTool.dll";
            if (!IfeoEngine::InstallIfeo(rootDll, err, true)) {
                RecordElevatedError(err);
                return 1;
            }
            return 0;
        }
        if (_wcsicmp(__wargv[1], L"/uninstall") == 0) {
            std::wstring err;
            if (!IfeoEngine::UninstallIfeo(err)) {
                RecordElevatedError(err);
                return 1;
            }
            return 0;
        }
        if (_wcsicmp(__wargv[1], L"/deploy-dll") == 0 && __argc >= 4) {
            std::wstring err;
            if (!IfeoEngine::DeployStandardDll(__wargv[2], __wargv[3], err)) {
                RecordElevatedError(err);
                return 1;
            }
            return 0;
        }
        if (_wcsicmp(__wargv[1], L"/remove-dll") == 0 && __argc >= 3) {
            std::wstring err;
            if (!IfeoEngine::RemoveStandardDll(__wargv[2], err)) {
                RecordElevatedError(err);
                return 1;
            }
            return 0;
        }

        // 5. Standalone Auto-Start CLI Actions (Runs as standard user, zero UAC needed)
        if (_wcsicmp(__wargv[1], L"/install-autostart") == 0 || _wcsicmp(__wargv[1], L"/enable-autostart") == 0 || _wcsicmp(__wargv[1], L"/autostart-enable") == 0) {
            const std::filesystem::path dllPath = (__argc >= 3) ? std::filesystem::path(__wargv[2]) : IfeoEngine::GetDefaultPayloadDllPath();
            return InjectorEngine::SetAutoStart(true, dllPath) ? 0 : 1;
        }
        if (_wcsicmp(__wargv[1], L"/uninstall-autostart") == 0 || _wcsicmp(__wargv[1], L"/disable-autostart") == 0 || _wcsicmp(__wargv[1], L"/autostart-disable") == 0) {
            return InjectorEngine::SetAutoStart(false) ? 0 : 1;
        }
    }

    // 6. GUI Manager Mode (Runs as standard user, zero-elevation start)
    // Defensive guard: prevent multiple manager GUI windows from running concurrently across folders
    constexpr wchar_t kManagerGuiMutex[] = L"Local\\OpenSteamTool_Manager_GUI_Mutex";
    constexpr wchar_t kClassName[] = L"OpenSteamTool_Manager_WndClass";

    ScopedHandle hGuiMutex(::CreateMutexW(nullptr, FALSE, kManagerGuiMutex));
    if (!hGuiMutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // Bring existing running manager window to foreground and exit cleanly
        HWND hExisting = ::FindWindowW(kClassName, nullptr);
        if (hExisting) {
            ::ShowWindow(hExisting, SW_RESTORE);
            ::SetForegroundWindow(hExisting);
        }
        return 0;
    }

    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icex{ sizeof(icex) };
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    ::InitCommonControlsEx(&icex);

    // Auto-detect UI language
    I18n::SetLanguage(I18n::DetectSystemLanguage());

    WNDCLASSEXW wcex{ sizeof(wcex) };
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wcex.hCursor        = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wcex.hbrBackground  = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wcex.lpszMenuName   = nullptr;
    wcex.lpszClassName  = kClassName;
    wcex.hIconSm        = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));

    if (!::RegisterClassExW(&wcex)) {
        return 1;
    }

    const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
    const int screenH = ::GetSystemMetrics(SM_CYSCREEN);
    const int posX = (screenW - kWindowWidth) / 2;
    const int posY = (screenH - kWindowHeight) / 2;

    const std::wstring initialTitle = std::format(L"{} [{}]", I18n::Get(StringId::AppTitle), L"" OPENSTEAMTOOL_VERSION);
    g_hWnd = ::CreateWindowExW(
        0, kClassName, initialTitle.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, kWindowWidth, kWindowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hWnd) {
        return 1;
    }

    ::ShowWindow(g_hWnd, nCmdShow);
    ::UpdateWindow(g_hWnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        if (!::IsDialogMessageW(g_hWnd, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}
