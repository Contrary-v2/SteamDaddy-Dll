#include <span>
#pragma once

#include "Steam/Types.h"

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // Fallback late-hook bootstrapping: Steam checked ownership before hooks were active.
    // Priming next CheckAppOwnership to force a re-query once with captured CUser.
    void RequestRequeryOnce();

    // Queries whether the current Steam account genuinely owns the license for this AppId/DLC.
    // Checks memory cache first, falling back to direct oCheckAppOwnership if CUser is captured.
    [[nodiscard]] bool IsAppTrulyOwned(AppId_t appId);

    // Queries whether the current Steam account has a valid family sharing license for this AppId.
    [[nodiscard]] bool IsSharedLicense(AppId_t appId);

    // Returns true if the account either genuinely owns the license or has a valid family shared license.
    [[nodiscard]] bool HasValidLicense(AppId_t appId);

    // Observer callback: invoked when active packages/depots change in Lua configuration
    using LicenseChangeObserver = void(*)(std::span<const AppId_t> additions, std::span<const AppId_t> removals);
    void SetLicenseChangeObserver(LicenseChangeObserver observer) noexcept;

    // Fast-path lockless check if an AppId is recognized as family-shared from official packages
    [[nodiscard]] bool IsSharedLicenseCached(AppId_t appId) noexcept;
    void RecordFamilySharedApp(AppId_t appId);
}
