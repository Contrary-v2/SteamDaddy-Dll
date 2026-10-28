#pragma once

namespace HookManager {
    /// Installs early UI hooks (LoadModuleWithPath) to gate steamclient loading cooperatively.
    void InstallEarlyUIHooks() noexcept;
    void InstallUIHooks() noexcept;
    void UninstallUIHooks() noexcept;

    void InstallClientHooks() noexcept;
    void UninstallClientHooks() noexcept;

    void DetachWorkerThreads() noexcept;

    [[nodiscard]] bool AreEarlyUIHooksInstalled() noexcept;
    [[nodiscard]] bool AreUIHooksInstalled() noexcept;
    [[nodiscard]] bool AreClientHooksInstalled() noexcept;
}
