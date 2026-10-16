#include "dllmain.h"
#include "Hooks_IPC.h"
#include "IPCMessages.gen.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Pipe/PipeManager.h"
#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"
#include "Utils/SteamMetadata/IPCLoader.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace {

    template <typename T>
    class NoDestructor final {
        alignas(T) std::byte m_storage[sizeof(T)];

    public:
        template <typename... Args>
        explicit NoDestructor(Args&&... args) {
            std::construct_at(reinterpret_cast<T*>(m_storage), std::forward<Args>(args)...);
        }

        ~NoDestructor() = default;

        NoDestructor(const NoDestructor&) = delete;
        NoDestructor& operator=(const NoDestructor&) = delete;
        NoDestructor(NoDestructor&&) = delete;
        NoDestructor& operator=(NoDestructor&&) = delete;

        [[nodiscard]] T* get() noexcept { return std::launder(reinterpret_cast<T*>(m_storage)); }
        [[nodiscard]] const T* get() const noexcept { return std::launder(reinterpret_cast<const T*>(m_storage)); }
        [[nodiscard]] T& operator*() noexcept { return *get(); }
        [[nodiscard]] const T& operator*() const noexcept { return *get(); }
        [[nodiscard]] T* operator->() noexcept { return get(); }
        [[nodiscard]] const T* operator->() const noexcept { return get(); }
    };

    RESOLVE_FUNC(GetPipeClient, CPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    //  Handler dispatch table
    struct ResolvedHandler {
        uint64_t     key;           // (static_cast<uint64_t>(interfaceID) << 32) | funcHash
        const char*  interfaceName;
        const char*  methodName;
        IPCHandlerFn pre;
        IPCHandlerFn post;

        ResolvedHandler(const IPCHandlerEntry& entry, const IPCLoader::Method& method) noexcept
            : key((static_cast<uint64_t>(method.interfaceID) << 32) | method.funcHash),
              interfaceName(entry.interfaceName),
              methodName(entry.methodName),
              pre(entry.pre),
              post(entry.post) {}

        std::string DebugString() const {
            return std::format("{}::{} -> key=0x{:016X}",
                interfaceName ? interfaceName : "",
                methodName ? methodName : "",
                key);
        }
    };
    NoDestructor<std::vector<ResolvedHandler>> g_Handlers;

    static const ResolvedHandler* FindHandler(EIPCInterface iface, uint32 funcHash) {
        const uint64_t targetKey = (static_cast<uint64_t>(iface) << 32) | funcHash;
        for (const auto& e : *g_Handlers) {
            if (e.key == targetKey) return &e;
        }
        return nullptr;
    }

    static void HandleHandshake(void* pServer, HSteamPipe hSteamPipe, std::span<uint8> body)
    {
        IPCMessages::IPCHandshakeReq handshake{body};
        if (!handshake.ok()) return;

        CPipeClient* pipe = GetPipe(pServer, hSteamPipe);
        if (!pipe) return;
        // set client PID 
        pipe->m_clientPID = handshake.pid();

        LOG_IPC_DEBUG("Received handshake from {},{}", pipe->DebugString(), handshake.DebugString());
        PipeManager::OnHandshake(pipe);
    }

    HOOK_FUNC(IPCProcessMessage, bool, void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        // Parse request header once for this buffer
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok())
            return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        const EIPCCommand cmd = request.command();

        // 1. Handshake messages
        if (cmd == EIPCCommand::Handshake) {
            HandleHandshake(pServer, hSteamPipe, request.body());
            return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
        }

        // 2. InterfaceCall messages
        if (cmd == EIPCCommand::InterfaceCall) {
            IPCMessages::IPCInterfaceCall call{request.body()};
            if (!call.ok())
                return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

            // Detect the first SteamNetworkingSockets call (interface 46) so state is tracked.
            // Skipped once already seen or when OnlineFix is not active.
            if (Hooks_Misc::IsOnlineFixActive() && !Hooks_Misc::IsNetworkingSocketsActive()) {
                if (call.interfaceID() == EIPCInterface::IClientNetworkingSocketsSerialized)
                    Hooks_Misc::NotifyNetworkingSocketsUsed();
            }

            // For OnlineFix games, intercept IClientRemoteStorage (interface 13):
            // If the method passes an AppID as its first parameter (e.g. *ForApp calls),
            // rewrite 480 to realAppId before steamclient processes it.
            // NOTE: We do NOT blindly scan the entire payload to avoid corrupting file
            // data in FileWrite calls that might contain 480 (0x000001E0) as content.
            if (Hooks_Misc::IsOnlineFixActive() && call.interfaceID() == EIPCInterface::IClientRemoteStorage) {
                if (Hooks_Misc::GetAppIDForCurrentPipeWrap() == Hooks_Misc::kOnlineFixAppId) {
                    const AppId_t realAppId = Hooks_Misc::ResolvePipeAppId();
                    if (realAppId != 0 && call.body().size() >= sizeof(uint32_t)) {
                        uint32_t firstArg = 0;
                        memcpy(&firstArg, call.body().data(), sizeof(uint32_t));
                        if (firstArg == Hooks_Misc::kOnlineFixAppId) {
                            memcpy(const_cast<uint8_t*>(call.body().data()), &realAppId, sizeof(uint32_t));
                            LOG_IPC_DEBUG("IClientRemoteStorage IPC: mapped AppID {} -> {} in request header arg",
                                          Hooks_Misc::kOnlineFixAppId, realAppId);
                        }
                    }
                }
            }

            // Lookup handler by interface ID + method hash.
            // If matched and app is configured in Lua or OnlineFix is active, resolve pipe and invoke handlers.
            if (const auto* handler = FindHandler(call.interfaceID(), call.funcHash())) {
                if (CPipeClient* pipe = GetPipe(pServer, hSteamPipe)) {
                    AppId_t currentAppId = Hooks_Misc::ResolvePipeAppId();
                    if (currentAppId == 0 && !pipe->m_bInProcess) {
                        currentAppId = PipeManager::DenuvoAuth::GetAuthorizedAppId(pipe);
                    }
                    if ((currentAppId != 0 && LuaConfig::HasDepot(currentAppId, false)) || Hooks_Misc::IsOnlineFixActive()) {
                        LOG_IPC_TRACE("Resolved IPC handler: {} {}", pipe->DebugString(), handler->DebugString());
                        if (handler->pre)
                            handler->pre(pipe, pRead, pWrite);

                        bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

                        if (result && handler->post)
                            handler->post(pipe, pRead, pWrite);

                        return result;
                    }
                }
            }
        }

        // All other IPC messages pass through to the original function.
        return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
    }

} // namespace


namespace Hooks_IPC {

    void Install() {
        RESOLVE_C(GetPipeClient);

        // Each module registers a static array. Hash lookup against the
        // IPCLoader metadata happens inside RegisterHandlers.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        LOG_IPC_INFO("Hooks_IPC: {} handlers registered", g_Handlers->size());

        HOOK_BEGIN();
        INSTALL_HOOK_C(IPCProcessMessage);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(IPCProcessMessage);
        UNHOOK_END();
    }

    void RegisterHandlers(std::span<const IPCHandlerEntry> entries) {
        for (const auto& e : entries) {
            const auto* m = IPCLoader::Find(e.interfaceName, e.methodName);
            if (!m) {
                LOG_IPC_WARN("[Handler Disabled] no IPC spec for {}", e.DebugString());
                continue;
            }
            const uint64_t key = (static_cast<uint64_t>(m->interfaceID) << 32) | m->funcHash;
            auto it = std::ranges::find_if(*g_Handlers, [key](const ResolvedHandler& h) { return h.key == key; });
            if (it != g_Handlers->end()) {
                *it = ResolvedHandler(e, *m);
                LOG_IPC_DEBUG("Hooks_IPC: updated {}", it->DebugString());
            } else {
                auto& handler = g_Handlers->emplace_back(e, *m);
                LOG_IPC_DEBUG("Hooks_IPC: resolved {}", handler.DebugString());
            }
        }
    }

}
