#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <optional>

namespace OST::Manager {

enum class IfeoStatus {
    NotInstalled,
    InstalledPortable,
    InstalledRoot,
    ConflictOtherInstance,  // IFEO is registered to another manager path
    BrokenOrphaned          // IFEO targets a nonexistent or moved executable
};

struct IfeoDetails {
    IfeoStatus status = IfeoStatus::NotInstalled;
    std::wstring registeredDebuggerCmd;
    std::wstring registeredExePath;
    std::wstring registeredPayloadPath;
    bool isCurrentInstance = false;
    bool exeExists = false;
    bool payloadExists = false;
};

enum class DllDeployStatus {
    NotDeployed,
    DeployedVersion,
    DeployedDwmapi,
    DeployedBoth,
    MissingPayload
};

struct DllDeployDetails {
    DllDeployStatus status = DllDeployStatus::NotDeployed;
    bool hasPayload = false;
    bool hasVersion = false;
    bool hasDwmapi = false;
};

class IfeoEngine {
public:
    [[nodiscard]] static std::filesystem::path GetSelfExePath();
    [[nodiscard]] static std::filesystem::path GetSelfDir();
    [[nodiscard]] static std::filesystem::path GetDefaultPayloadDllPath();
    [[nodiscard]] static std::wstring DetectSteamInstallPath();

    [[nodiscard]] static IfeoDetails QueryDetails();
    [[nodiscard]] static IfeoStatus QueryStatus(std::wstring& outPayloadPath);
    [[nodiscard]] static DllDeployDetails QueryStandardDllDetails(const std::wstring& steamDir) noexcept;

    [[nodiscard]] static bool InstallIfeo(const std::filesystem::path& payloadDllPath,
                                          std::wstring& outError,
                                          bool isRootMode = false);

    [[nodiscard]] static bool UninstallIfeo(std::wstring& outError);

    [[nodiscard]] static bool DeployToSteamRoot(const std::wstring& steamDir,
                                                const std::filesystem::path& sourceDllPath,
                                                std::wstring& outError);

    [[nodiscard]] static bool DeployStandardDll(const std::wstring& steamDir,
                                                const std::filesystem::path& payloadDllPath,
                                                std::wstring& outError);

    [[nodiscard]] static bool RemoveStandardDll(const std::wstring& steamDir,
                                                std::wstring& outError);

    static void CleanResidualSteamRootBinaries(const std::wstring& steamDir,
                                              const std::filesystem::path& currentPayloadPath) noexcept;

    [[nodiscard]] static bool IsSteamRunning(DWORD& outPid) noexcept;
    [[nodiscard]] static bool IsPayloadInjected(DWORD pid) noexcept;

    [[nodiscard]] static bool LaunchHostAndInject(int argc, wchar_t* argv[]) noexcept;
    [[nodiscard]] static bool LaunchSteamWithInjection(const std::filesystem::path& steamDir,
                                                        const std::filesystem::path& payloadDll,
                                                        DWORD& outPid,
                                                        std::wstring& outError);

    [[nodiscard]] static bool IsRunningElevated() noexcept;
    [[nodiscard]] static bool ElevateAndExecute(HWND hWndParent,
                                                const std::wstring& parameters,
                                                std::wstring& outError);

    static void CleanLegacySdbArtifacts() noexcept;
};

} // namespace OST::Manager
