#ifndef DLLMAIN_H
#define DLLMAIN_H

#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "Steam/Types.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Steam/Callback.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/Config.h"

#include <atomic>
#include <cstddef>

inline std::atomic<OSTPlatform::DynamicLibrary::ModuleHandle> client_hModule{nullptr};
inline std::atomic<OSTPlatform::DynamicLibrary::ModuleHandle> ui_hModule{nullptr};

inline std::atomic<bool> g_HooksInstalled{false};
inline std::atomic<bool> g_IsDiversionActive{false};
// Steam whether diversion shadow copy was adopted by SteamUI LoadModuleWithPath dispatch.
inline std::atomic<bool> g_DiversionAdopted{false};
inline std::atomic<bool> g_steamClientMappedAtAttach{false};

inline constexpr size_t kRuntimePathCapacity = 1024;

inline char SteamInstallPath[kRuntimePathCapacity] = {};
inline char SteamclientPath[kRuntimePathCapacity]  = {};
inline char SteamUIPath[kRuntimePathCapacity]      = {};
inline char DiversionPath[kRuntimePathCapacity]    = {};
inline char LuaDir[kRuntimePathCapacity]           = {};
inline char ConfigPath[kRuntimePathCapacity]       = {};
inline char DllDir[kRuntimePathCapacity]           = {};

inline std::atomic<bool> g_IsPortableMode{false};

inline bool IsPortableMode() noexcept {
    return g_IsPortableMode.load(std::memory_order_acquire);
}

inline const char* GetStorageDirectory() noexcept {
    if (IsPortableMode() && DllDir[0] != '\0') {
        return DllDir;
    }
    if (SteamInstallPath[0] != '\0') {
        return SteamInstallPath;
    }
    if (DllDir[0] != '\0') {
        return DllDir;
    }
    return "";
}

// The fake AppId used by -onlinefix (SpaceWar).
constexpr AppId_t kOnlineFixAppId = 480;

#endif // DLLMAIN_H
