#pragma once

#include "Steam/Types.h"
#include "OSTPlatform/include/Process.h"

#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace ProcessInspector {

    inline constexpr std::array<std::string_view, 6> kSteamProcessNames = {
        "steam.exe",
        "steamwebhelper.exe",
        "steamservice.exe",
        "steamerrorreporter.exe",
        "gameoverlayui.exe",
        "gameoverlayui64.exe",
    };

    inline constexpr std::array<std::string_view, 3> kToolProcessNames = {
        "extract_tickets.exe",
        "ost-injector.exe",
        "injector.exe",
    };

    struct ProcessEnvironment {
        std::optional<AppId_t> steamOverlayGameIdAppId;
        std::optional<AppId_t> steamGameIdAppId;
        std::optional<AppId_t> steamAppId;
        bool isOstTool = false;

        bool operator==(const ProcessEnvironment&) const = default;

        [[nodiscard]] constexpr AppId_t ResolveAppId() const noexcept {
            if (steamOverlayGameIdAppId) return *steamOverlayGameIdAppId;
            if (steamGameIdAppId) return *steamGameIdAppId;
            return steamAppId.value_or(k_uAppIdInvalid);
        }

        [[nodiscard]] constexpr bool HasSteamAppEnvironment() const noexcept {
            return steamAppId || steamGameIdAppId || steamOverlayGameIdAppId;
        }

        [[nodiscard]] std::string DebugString() const {
            return std::format(
                "SteamAppId={} SteamGameId={} SteamOverlayGameId={} isOstTool={} resolvedAppId={}",
                steamAppId.value_or(k_uAppIdInvalid),
                steamGameIdAppId.value_or(k_uAppIdInvalid),
                steamOverlayGameIdAppId.value_or(k_uAppIdInvalid),
                isOstTool,
                ResolveAppId());
        }
    };

    struct ProcessSnapshot {
        PID_t pid = 0;
        uint64 creationTime = 0;
        std::string imagePath;
        std::string imageName;
        bool steamClientProcess = false;
        bool isToolProcess = false;
        bool likelyGameProcess = false;
        ProcessEnvironment environment;

        bool operator==(const ProcessSnapshot&) const = default;

        [[nodiscard]] constexpr AppId_t ResolveAppId() const noexcept {
            return environment.ResolveAppId();
        }

        [[nodiscard]] std::string DebugString() const {
            return std::format(
                "pid={} creation={} image={} steamClientProcess={} isToolProcess={} likelyGameProcess={} env=[{}]",
                pid,
                std::format("{} filetime={}", OSTPlatform::Process::FormatCreationTime(creationTime), creationTime),
                imageName.empty() ? "-" : imageName,
                steamClientProcess,
                isToolProcess,
                likelyGameProcess,
                environment.DebugString());
        }
    };

    [[nodiscard]] std::optional<uint64> GetProcessCreationTime(PID_t pid) noexcept;
    [[nodiscard]] bool IsSteamProcessName(std::string_view name) noexcept;
    [[nodiscard]] bool IsToolProcessName(std::string_view name) noexcept;
    [[nodiscard]] ProcessEnvironment ReadSteamEnvironment(PID_t pid);
    [[nodiscard]] ProcessSnapshot InspectProcess(PID_t pid);

} // namespace ProcessInspector
