#include "Pipe/ProcessInspector.h"

#include "OSTPlatform/include/Process.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "OSTPlatform/include/Stopwatch.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace ProcessInspector {
namespace {

    [[nodiscard]] inline std::string_view BaseNameFromPath(std::string_view path) noexcept {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string_view::npos) return path;
        return path.substr(slash + 1);
    }

    [[nodiscard]] std::optional<AppId_t> AppIdFromGameIdString(std::string_view value) noexcept {
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(value);
        if (!parsed) return std::nullopt;

        const AppId_t appId = static_cast<AppId_t>(*parsed & 0xFFFFFFu);
        return appId == k_uAppIdInvalid ? std::nullopt : std::optional<AppId_t>(appId);
    }

    [[nodiscard]] std::optional<AppId_t> AppIdFromAppIdString(std::string_view value) noexcept {
        const auto parsed = OSTPlatform::Numbers::ParseUInt32(value);
        if (!parsed || *parsed == k_uAppIdInvalid) return std::nullopt;
        return *parsed;
    }

    [[nodiscard]] std::optional<std::string> QueryProcessImagePath(PID_t pid) {
        return OSTPlatform::Process::GetImagePath(pid);
    }

    [[nodiscard]] inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
        return std::ranges::equal(a, b, [](char ca, char cb) noexcept {
            return std::tolower(static_cast<unsigned char>(ca)) ==
                   std::tolower(static_cast<unsigned char>(cb));
        });
    }

} // namespace

std::optional<uint64> GetProcessCreationTime(PID_t pid) noexcept {
    return OSTPlatform::Process::GetCreationTime(pid);
}

bool IsSteamProcessName(std::string_view name) noexcept {
    return std::ranges::any_of(kSteamProcessNames, [name](std::string_view steamProc) noexcept {
        return EqualsIgnoreCase(name, steamProc);
    });
}

bool IsToolProcessName(std::string_view name) noexcept {
    return std::ranges::any_of(kToolProcessNames, [name](std::string_view toolProc) noexcept {
        return EqualsIgnoreCase(name, toolProc);
    });
}

ProcessEnvironment ReadSteamEnvironment(PID_t pid) {
    ProcessEnvironment env{};
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"SteamAppId")) {
        env.steamAppId = AppIdFromAppIdString(*value);
    }
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"SteamGameId")) {
        env.steamGameIdAppId = AppIdFromGameIdString(*value);
    }
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"SteamOverlayGameId")) {
        env.steamOverlayGameIdAppId = AppIdFromGameIdString(*value);
    }
    if (auto value = OSTPlatform::Process::GetEnvironmentVariableValue(pid, L"OST_TOOL_EXTRACTION")) {
        env.isOstTool = true;
    }

    LOG_PIPE_DEBUG("ProcessInspector: pid={} steam env {}", pid, env.DebugString());
    return env;
}

ProcessSnapshot InspectProcess(PID_t pid) {
    const OSTPlatform::Stopwatch timer;
    ProcessSnapshot snapshot{};
    snapshot.pid = pid;
    snapshot.creationTime = GetProcessCreationTime(pid).value_or(0);

    if (auto imagePath = QueryProcessImagePath(pid)) {
        snapshot.imagePath = std::move(*imagePath);
        snapshot.imageName = BaseNameFromPath(snapshot.imagePath);
    }
    snapshot.steamClientProcess = IsSteamProcessName(snapshot.imageName);

    // Steam client internal processes never run games or tools; skip expensive PEB reads.
    if (!snapshot.steamClientProcess) {
        snapshot.environment = ReadSteamEnvironment(pid);
        snapshot.isToolProcess = IsToolProcessName(snapshot.imageName) || snapshot.environment.isOstTool;
        snapshot.likelyGameProcess = !snapshot.isToolProcess && snapshot.environment.HasSteamAppEnvironment();
    }

    LOG_PIPE_INFO("ProcessInspector: inspected {} elapsed_ms={:.3f}",
                    snapshot.DebugString(), timer.ElapsedMs());
    return snapshot;
}

} // namespace ProcessInspector
