#pragma once

namespace HookManager {
    void InstallUIHooks() noexcept;
    void UninstallUIHooks() noexcept;

    void InstallClientHooks() noexcept;
    void UninstallClientHooks() noexcept;

    void DetachWorkerThreads() noexcept;

    [[nodiscard]] bool AreUIHooksInstalled() noexcept;
    [[nodiscard]] bool AreClientHooksInstalled() noexcept;
}
