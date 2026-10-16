#include "SteamDiagnostics.h"
#include "SteamDaddyBuildInfo.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace SteamDiagnostics {

namespace {

    struct Snapshot {
        std::string steamDaddyVersion = STEAMDADDY_VERSION;
        std::string buildID = "(unavailable)";
        std::string steamclientPath;
        std::string steamclientSha256 = "(unavailable)";
        std::string steamUIPath;
        std::string steamUISha256 = "(unavailable)";
    };

    Snapshot g_snapshot;
    std::shared_mutex g_snapshotMutex;

    using GetBootstrapperVersion_t = int64_t (*)();

    // Isolated low-level SEH function with no C++ object unwinding (avoids MSVC C2712).
    static int64_t SafeInvokeBootstrapper(GetBootstrapperVersion_t fn)
    {
        __try {
            return fn ? fn() : -1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    static std::string DetectSteamBuildID()
    {
        const auto steam = OSTPlatform::DynamicLibrary::GetLoaded("steam.exe");
        if (!steam) {
            LOG_WARN("SteamDiagnostics: steam.exe module not loaded; build id unavailable");
            return "(unavailable)";
        }

        const auto getBootstrapperVersion =
            reinterpret_cast<GetBootstrapperVersion_t>(
                OSTPlatform::DynamicLibrary::GetSymbol(steam, "GetBootstrapperVersion"));
        if (!getBootstrapperVersion) {
            LOG_WARN("SteamDiagnostics: steam.exe!GetBootstrapperVersion not exported");
            return "(unavailable)";
        }

        const int64_t ver = SafeInvokeBootstrapper(getBootstrapperVersion);
        if (ver < 0) {
            LOG_WARN("SteamDiagnostics: GetBootstrapperVersion triggered hardware exception or returned invalid");
            return "(unavailable)";
        }

        return std::to_string(ver);
    }

    static std::string HashOrUnavailable(const std::string& path)
    {
        std::string sha256 = OSTPlatform::Hash::Sha256OfFile(OSTPlatform::Encoding::PathFromUtf8(path));
        return sha256.empty() ? "(unavailable)" : std::move(sha256);
    }

    static bool IsSamePathNormalized(const std::string& a, const std::string& b)
    {
        if (a.empty() || b.empty()) return false;
        if (a == b) return true;
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto pa = OSTPlatform::Encoding::PathFromUtf8(a);
        const auto pb = OSTPlatform::Encoding::PathFromUtf8(b);
        if (pa.filename() != pb.filename()) return false;
        return fs::equivalent(pa, pb, ec) && !ec;
    }

    static std::string AppendSnapshot(std::string message)
    {
        std::shared_lock<std::shared_mutex> lock(g_snapshotMutex);
        message +=
            "\n\nSteam diagnostics:\n"
            "  SteamDaddy version: " + g_snapshot.steamDaddyVersion + "\n"
            "  Build ID:              " + g_snapshot.buildID + "\n"
            "  steamclient64.dll SHA: " + g_snapshot.steamclientSha256 + "\n"
            "  steamui.dll SHA:       " + g_snapshot.steamUISha256;
        return message;
    }

    // ── Diagnostic alert aggregator & single modal dialog lifecycle ──
    struct WarningItem {
        std::string title;
        std::string message;
    };

    std::mutex g_diagMutex;
    std::vector<WarningItem> g_pendingWarnings;
    bool g_dialogWorkerRunning = false;

    static void ProcessWarningQueue()
    {
        while (true) {
            std::string displayTitle;
            std::string displayMessage;
            {
                std::lock_guard<std::mutex> lock(g_diagMutex);
                if (g_pendingWarnings.empty()) {
                    g_dialogWorkerRunning = false;
                    break;
                }

                if (g_pendingWarnings.size() == 1) {
                    displayTitle = std::move(g_pendingWarnings[0].title);
                    displayMessage = AppendSnapshot(std::move(g_pendingWarnings[0].message));
                    g_pendingWarnings.clear();
                } else {
                    displayTitle = "SteamDaddy - Multiple Startup Warnings";
                    displayMessage = "SteamDaddy detected multiple issues during startup:\n\n";
                    for (size_t i = 0; i < g_pendingWarnings.size(); ++i) {
                        displayMessage += "[" + std::to_string(i + 1) + "] " + g_pendingWarnings[i].title + "\n";
                        displayMessage += g_pendingWarnings[i].message + "\n\n";
                    }
                    displayMessage = AppendSnapshot(std::move(displayMessage));
                    g_pendingWarnings.clear();
                }
            }

            OSTPlatform::Dialog::ShowWarning(displayTitle, displayMessage);
        }
    }

} // namespace

void Initialize(const std::string& steamclientPath,
                const std::string& steamUIPath)
{
    const std::string buildID = DetectSteamBuildID();
    const std::string clientSha = HashOrUnavailable(steamclientPath);
    const std::string uiSha = HashOrUnavailable(steamUIPath);

    {
        std::unique_lock<std::shared_mutex> lock(g_snapshotMutex);
        g_snapshot.buildID = buildID;
        g_snapshot.steamclientPath = steamclientPath;
        g_snapshot.steamclientSha256 = clientSha;
        g_snapshot.steamUIPath = steamUIPath;
        g_snapshot.steamUISha256 = uiSha;
    }

    LOG_INFO("SteamDiagnostics: ost.version={} build={} steamclient64.sha256={} steamui.sha256={}",
             STEAMDADDY_VERSION,
             buildID,
             clientSha,
             uiSha);
}

std::string Sha256Of(const std::string& path)
{
    {
        std::shared_lock<std::shared_mutex> lock(g_snapshotMutex);
        if (IsSamePathNormalized(path, g_snapshot.steamclientPath)) {
            return g_snapshot.steamclientSha256 == "(unavailable)"
                ? std::string{}
                : g_snapshot.steamclientSha256;
        }

        if (IsSamePathNormalized(path, g_snapshot.steamUIPath)) {
            return g_snapshot.steamUISha256 == "(unavailable)"
                ? std::string{}
                : g_snapshot.steamUISha256;
        }
    }

    return OSTPlatform::Hash::Sha256OfFile(OSTPlatform::Encoding::PathFromUtf8(path));
}

void ShowWarning(std::string title, std::string message)
{
    std::lock_guard<std::mutex> lock(g_diagMutex);

    // Deduplicate identical pending messages to prevent noise.
    for (const auto& item : g_pendingWarnings) {
        if (item.title == title && item.message == message) {
            LOG_DEBUG("SteamDiagnostics: suppressed duplicate pending warning: {}", title);
            return;
        }
    }

    g_pendingWarnings.push_back({std::move(title), std::move(message)});

    if (!g_dialogWorkerRunning) {
        g_dialogWorkerRunning = true;
        OSTPlatform::Thread::StartDetached([]() -> uint32_t {
            ProcessWarningQueue();
            return 0;
        });
    }
}

} // namespace SteamDiagnostics
