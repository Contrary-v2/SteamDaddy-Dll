#include "IPCLoader.h"
#include "dllmain.h"
#include "IPCMessages.gen.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

namespace IPCLoader {

namespace {

    struct Registry {
        std::vector<Interface> interfaces;
        std::unordered_map<EIPCInterface, size_t> byID;
        std::unordered_map<std::string, size_t> byName;
        std::unordered_map<uint64_t, const Method*> byMethodHash;

        static uint64_t MakeMethodKey(EIPCInterface interfaceID, uint32_t funcHash) noexcept
        {
            return (static_cast<uint64_t>(interfaceID) << 32) | static_cast<uint64_t>(funcHash);
        }

        void Clear() noexcept
        {
            interfaces.clear();
            byID.clear();
            byName.clear();
            byMethodHash.clear();
        }

        void Add(Interface iface)
        {
            const size_t index = interfaces.size();
            byID[iface.id] = index;
            byName[iface.name] = index;
            interfaces.push_back(std::move(iface));
        }

        void BuildIndex()
        {
            byMethodHash.clear();
            for (const auto& iface : interfaces) {
                for (const auto& method : iface.methods) {
                    byMethodHash[MakeMethodKey(method.interfaceID, method.funcHash)] = &method;
                }
            }
        }

        const Method* Find(EIPCInterface interfaceID, uint32_t funcHash) const noexcept
        {
            const auto it = byMethodHash.find(MakeMethodKey(interfaceID, funcHash));
            if (it != byMethodHash.end()) return it->second;

            // Fallback if index has not been built
            const auto ifaceIt = byID.find(interfaceID);
            if (ifaceIt == byID.end()) return nullptr;

            for (const auto& method : interfaces[ifaceIt->second].methods) {
                if (method.funcHash == funcHash) return &method;
            }
            return nullptr;
        }

        const Method* Find(std::string_view interfaceName,
                           std::string_view methodName) const noexcept
        {
            const auto it = byName.find(std::string(interfaceName));
            if (it == byName.end()) return nullptr;

            for (const auto& method : interfaces[it->second].methods) {
                if (method.name == methodName) return &method;
            }
            return nullptr;
        }

        size_t MethodCount() const noexcept
        {
            size_t count = 0;
            for (const auto& iface : interfaces)
                count += iface.methods.size();
            return count;
        }
    };

    Registry g_registry;
    std::shared_mutex g_ipcMutex;

    // ---- TOML helpers ----

    static bool ParseHexU32(std::string_view s, uint32_t& out) noexcept
    {
        const auto parsed = OSTPlatform::Numbers::ParseHexUInt32(s);
        if (!parsed) return false;
        out = *parsed;
        return true;
    }

    static void ValidateVtableRva(std::string_view ifaceName, Interface& iface)
    {
        if (iface.vtableRva == 0) return;

        // Alignment check (must be pointer-aligned on 64-bit architecture)
        if ((iface.vtableRva % sizeof(void*)) != 0) {
            LOG_WARN("IPCLoader: [{}] vtable_rva 0x{:X} is not 8-byte aligned, resetting to 0",
                     ifaceName, iface.vtableRva);
            iface.vtableRva = 0;
            return;
        }

        // PE module bounds check if steamclient64.dll is loaded
        const auto clientMod = OSTPlatform::DynamicLibrary::GetLoaded("steamclient64.dll");
        if (clientMod) {
            if (const auto img = OSTPlatform::Memory::GetModuleImage(clientMod)) {
                if (static_cast<size_t>(iface.vtableRva) + sizeof(void*) > img->size) {
                    LOG_WARN("IPCLoader: [{}] vtable_rva 0x{:X} exceeds steamclient64.dll image size 0x{:X}, resetting to 0",
                             ifaceName, iface.vtableRva, img->size);
                    iface.vtableRva = 0;
                    return;
                }
            }
        } else {
            // General safety limit in case of offline/test runner environments
            if (iface.vtableRva >= 0x20000000) {
                LOG_WARN("IPCLoader: [{}] vtable_rva 0x{:X} exceeds reasonable 512MB limit, resetting to 0",
                         ifaceName, iface.vtableRva);
                iface.vtableRva = 0;
                return;
            }
        }
    }

    static bool ParseInterfaceTable(std::string_view name,
                                    const toml::table& tbl,
                                    Interface& out)
    {
        out.name = std::string(name);

        const auto expected = EIPCInterfaceFromName(name);
        if (!expected) {
            LOG_WARN("IPCLoader: [{}] is not declared in EIPCInterface", name);
            return false;
        }
        out.id = *expected;

        // Backward-compatible metadata: EIPCInterface is authoritative.
        if (auto v = tbl["interface_id"].value<int64_t>()) {
            if (*v < 0 || *v > 0xFF) {
                LOG_WARN("IPCLoader: [{}] interface_id out of range ({})", name, *v);
                return false;
            }
            if (static_cast<EIPCInterface>(*v) != out.id) {
                LOG_WARN("IPCLoader: [{}] interface_id {} disagrees with generated EIPCInterface value {}",
                         name, *v, static_cast<uint8_t>(out.id));
                return false;
            }
        }

        // Dual-mode compatibility for string hex or native integer vtable_rva
        if (auto v = tbl["vtable_rva"].value<std::string>()) {
            ParseHexU32(*v, out.vtableRva);
        } else if (auto v = tbl["vtable_rva"].value<int64_t>()) {
            if (*v > 0) out.vtableRva = static_cast<uint32_t>(*v);
        }

        ValidateVtableRva(name, out);

        // Walk dotted sub-tables — each is a method.
        for (auto& [methodKey, methodVal] : tbl) {
            if (!methodVal.is_table()) continue;
            const auto& mtbl = *methodVal.as_table();

            Method m;
            m.interfaceID = out.id;
            m.name = std::string(methodKey.str());

            // Dual-mode compatibility for funcHash
            if (auto v = mtbl["funcHash"].value<std::string>()) {
                if (!ParseHexU32(*v, m.funcHash)) {
                    LOG_WARN("IPCLoader: [{}.{}] bad funcHash '{}'",
                             name, m.name, *v);
                    continue;
                }
            } else if (auto v = mtbl["funcHash"].value<int64_t>()) {
                m.funcHash = static_cast<uint32_t>(*v);
            } else {
                LOG_WARN("IPCLoader: [{}.{}] missing funcHash", name, m.name);
                continue;
            }

            // Dual-mode compatibility for fencepost
            if (auto v = mtbl["fencepost"].value<std::string>()) {
                ParseHexU32(*v, m.fencepost);
            } else if (auto v = mtbl["fencepost"].value<int64_t>()) {
                m.fencepost = static_cast<uint32_t>(*v);
            }

            // Dual-mode compatibility for argc
            if (auto v = mtbl["argc"].value<int64_t>()) {
                m.argc = static_cast<uint32_t>(*v);
            } else if (auto v = mtbl["argc"].value<std::string>()) {
                uint32_t argcVal = 0;
                if (ParseHexU32(*v, argcVal)) m.argc = argcVal;
            }

            out.methods.push_back(std::move(m));
        }
        return true;
    }

    static void ShowMissingPopup(const std::string& sha256)
    {
        const std::string rootLabel = IsPortableMode() ? "<PortableDir>" : "<Steam>";
        SteamDiagnostics::ShowWarning(
            "SteamDaddy - IPC spec missing",
            "SteamDaddy: IPC spec file not found.\n\n"
            "IPC interception is disabled for this session; pattern-based "
            "hooks are unaffected.\n\n"
            "You can:\n"
            "  1. Wait for the next tracker update, then restart Steam.\n"
            "  2. Drop a matching TOML at:\n"
            "       " + rootLabel + "\\steamdaddy\\ipc\\steamclient\\" + sha256 + ".toml\n"
            "  3. Check tracker repository:\n"
            "       https://github.com/mmxlyo/steam-monitor/tree/ipc/steamclient\n"
            "  4. Report the diagnostics below:\n"
            "       https://github.com/Contrary7/SteamDaddy-Backup/issues");
    }

} // namespace

constexpr const char* kIPCChannel = "ipc";

bool Load(const std::string& steamclientPath)
{
    RemoteToml::Result r = RemoteToml::Fetch({
        kIPCChannel,
        "steamclient",
        steamclientPath,
    });

    if (!r.ok) {
        ShowMissingPopup(r.sha256.empty() ? "(hash failed)" : r.sha256);
        return false;
    }

    Registry localRegistry;

    try {
        toml::table root = toml::parse(r.body);
        for (auto& [key, val] : root) {
            if (!val.is_table()) continue;
            Interface iface;
            if (!ParseInterfaceTable(key.str(), *val.as_table(), iface)) continue;

            localRegistry.Add(std::move(iface));
        }
    } catch (const toml::parse_error& e) {
        LOG_WARN("IPCLoader: TOML parse error: {}", e.description());
        ShowMissingPopup(r.sha256);
        return false;
    }

    localRegistry.BuildIndex();

    // Atomic commit under exclusive lock (Strong Exception Safety)
    {
        std::unique_lock<std::shared_mutex> lock(g_ipcMutex);
        g_registry = std::move(localRegistry);
    }

    LOG_INFO("IPCLoader: loaded {} methods across {} interfaces ({})",
             MethodCount(), InterfaceCount(),
             r.fromCache ? "cache" : "remote");
    return true;
}

const Method* Find(EIPCInterface interfaceID, uint32_t funcHash)
{
    std::shared_lock<std::shared_mutex> lock(g_ipcMutex);
    return g_registry.Find(interfaceID, funcHash);
}

const Method* Find(std::string_view ifaceName, std::string_view methodName)
{
    std::shared_lock<std::shared_mutex> lock(g_ipcMutex);
    return g_registry.Find(ifaceName, methodName);
}

size_t InterfaceCount() noexcept
{
    std::shared_lock<std::shared_mutex> lock(g_ipcMutex);
    return g_registry.interfaces.size();
}

size_t MethodCount() noexcept
{
    std::shared_lock<std::shared_mutex> lock(g_ipcMutex);
    return g_registry.MethodCount();
}

} // namespace IPCLoader
