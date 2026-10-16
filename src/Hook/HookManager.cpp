#include "HookManager.h"
#include "Hooks_Decryption.h"
#include "Hooks_IPC.h"
#include "Hooks_Manifest.h"
#include "Hooks_Misc.h"
#include "Hooks_NetPacket.h"
#include "Hooks_Package.h"
#include "Hooks_SteamUI.h"
#include "PendingAPICalls.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <mutex>

namespace HookManager {

namespace {
    std::mutex s_lifecycleMutex;
    std::atomic<bool> s_uiHooksInstalled{false};
    std::atomic<bool> s_clientHooksInstalled{false};
}

void InstallUIHooks() noexcept {
    std::scoped_lock lock(s_lifecycleMutex);
    if (s_uiHooksInstalled.load(std::memory_order_relaxed)) {
        return;
    }
    LOG_INFO("HookManager: Installing UI hooks...");
    Hooks_SteamUI::Install();
    s_uiHooksInstalled.store(true, std::memory_order_release);
}

void UninstallUIHooks() noexcept {
    std::scoped_lock lock(s_lifecycleMutex);
    if (!s_uiHooksInstalled.load(std::memory_order_relaxed)) {
        return;
    }
    LOG_INFO("HookManager: Uninstalling UI hooks...");
    Hooks_SteamUI::Uninstall();
    s_uiHooksInstalled.store(false, std::memory_order_release);
    LOG_INFO("HookManager: UI hooks uninstalled cleanly");
}

void InstallClientHooks() noexcept {
    std::scoped_lock lock(s_lifecycleMutex);
    if (s_clientHooksInstalled.load(std::memory_order_relaxed)) {
        return;
    }
    LOG_INFO("HookManager: Installing client hooks (6 modules)...");
    Hooks_Decryption::Install();
    Hooks_IPC::Install();
    Hooks_Manifest::Install();
    Hooks_Misc::Install();
    Hooks_NetPacket::Install();
    Hooks_Package::Install();
    s_clientHooksInstalled.store(true, std::memory_order_release);
    LOG_INFO("HookManager: Client hooks installed successfully");
}

void UninstallClientHooks() noexcept {
    std::scoped_lock lock(s_lifecycleMutex);
    if (!s_clientHooksInstalled.load(std::memory_order_relaxed)) {
        return;
    }
    LOG_INFO("HookManager: Uninstalling client hooks in strict LIFO order...");
    // Strict LIFO rollback: reverse of Decryption -> IPC -> Manifest -> Misc -> NetPacket -> Package
    Hooks_Package::Uninstall();
    Hooks_NetPacket::Uninstall();
    Hooks_Misc::Uninstall();
    Hooks_Manifest::Uninstall();
    Hooks_IPC::Uninstall();
    Hooks_Decryption::Uninstall();

    // After all Detours hooks are safely unhooked, disarm all VEH breakpoints and unregister handler
    VehCommon::DisarmAll();
    VehCommon::RemoveHandler();

    // Reset pending API call context cache
    PendingAPICalls::Clear();
    s_clientHooksInstalled.store(false, std::memory_order_release);
    LOG_INFO("HookManager: Client hooks uninstalled cleanly");
}

void DetachWorkerThreads() noexcept {
    // Lock-free invocation: OS terminates secondary threads during ExitProcess (pvReserved != nullptr).
    // Never acquire mutexes held by dead threads.
    LOG_INFO("HookManager: Detaching worker threads for process termination...");
    Hooks_SteamUI::DetachWorkerThreads();
}

bool AreUIHooksInstalled() noexcept {
    return s_uiHooksInstalled.load(std::memory_order_acquire);
}

bool AreClientHooksInstalled() noexcept {
    return s_clientHooksInstalled.load(std::memory_order_acquire);
}

} // namespace HookManager

