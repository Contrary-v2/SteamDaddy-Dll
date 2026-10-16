#include "Pipe/Features/Injection/Injection.h"

#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Process.h"
#include "OSTPlatform/include/RemoteProcess.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Pipe/ProcessInspector.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <compare>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace PipeManager::Injection {
namespace {

    // Keyed on (process, path) so each DLL injects at most once per process
    // while several [[inject]] entries can still target the same game.
    struct InjectedKey {
        ProcessKey  process;
        std::string path;

        bool operator==(const InjectedKey&) const = default;
        auto operator<=>(const InjectedKey&) const = default;
    };

    struct InjectedKeyHash {
        std::size_t operator()(const InjectedKey& key) const noexcept {
            uint64_t h1 = ProcessKeyHash{}(key.process);
            uint64_t h2 = std::hash<std::string>{}(key.path);
            uint64_t x = h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return static_cast<std::size_t>(x);
        }
    };

    std::mutex g_mutex;
    std::unordered_set<InjectedKey, InjectedKeyHash> g_injected;

    bool ClaimInjection(const InjectedKey& key) {
        std::scoped_lock lock(g_mutex);
        if (g_injected.size() >= 256) {
            std::erase_if(g_injected, [&](const InjectedKey& k) {
                if (k.process == key.process) return false;
                auto currentCreation = ProcessInspector::GetProcessCreationTime(k.process.pid);
                return !currentCreation || *currentCreation != k.process.creationTime;
            });
        }
        return g_injected.insert(key).second;
    }

    void UnclaimInjection(const InjectedKey& key) {
        std::scoped_lock lock(g_mutex);
        g_injected.erase(key);
    }

    // RAII guard ensuring automatic UnclaimInjection rollback if injection fails or throws.
    class InjectionClaimGuard {
    public:
        InjectionClaimGuard(ProcessKey process, std::string path)
            : key_{std::move(process), std::move(path)}, claimed_(ClaimInjection(key_)) {}

        ~InjectionClaimGuard() {
            if (claimed_ && !dismissed_) {
                UnclaimInjection(key_);
            }
        }

        [[nodiscard]] bool IsClaimed() const noexcept { return claimed_; }
        void Dismiss() noexcept { dismissed_ = true; }

        InjectionClaimGuard(const InjectionClaimGuard&) = delete;
        InjectionClaimGuard& operator=(const InjectionClaimGuard&) = delete;
        InjectionClaimGuard(InjectionClaimGuard&&) = delete;
        InjectionClaimGuard& operator=(InjectionClaimGuard&&) = delete;

    private:
        InjectedKey key_;
        bool claimed_ = false;
        bool dismissed_ = false;
    };

    [[nodiscard]] bool MatchesCmdLine(const std::string& cmdLine, const std::string& pattern) {
        if (pattern.empty()) return true;
        if (cmdLine.empty()) return false;

        // If the pattern starts with a command-line switch character, use token boundary matching
        if ((pattern.front() == '-' || pattern.front() == '/') &&
            DenuvoAuth::HasCmdLineArg(cmdLine.c_str(), pattern.c_str())) {
            return true;
        }

        // Case-insensitive substring fallback
        auto it = std::search(
            cmdLine.begin(), cmdLine.end(),
            pattern.begin(), pattern.end(),
            [](char ch1, char ch2) noexcept {
                return std::tolower(static_cast<unsigned char>(ch1)) ==
                       std::tolower(static_cast<unsigned char>(ch2));
            });
        return it != cmdLine.end();
    }

    [[nodiscard]] bool Matches(const Config::InjectDll& dll, const PipeContext& ctx,
                               const std::optional<std::string>& cmdLine) {
        const bool whitelisted = dll.whenAppids.contains(ctx.appId);
        if (!dll.allGames && !ctx.trackedApp && !whitelisted) return false;

        // Exclusion list strictly protects game processes from DLL injection unless explicitly whitelisted;
        // game unlocking and DLC simulation (addappid, manifest, etc.) remain 100% active.
        // Whitelist priority: if explicitly specified in when_appids, the game is never excluded.
        if (ctx.gameProcess && !whitelisted && dll.excludeAppids.contains(ctx.appId)) {
            LOG_INJECT_WARN("inject skipped: game appid={} is in exclude_appids", ctx.appId);
            return false;
        }

        if (!dll.whenAppids.empty() && !whitelisted) return false;
        if (!dll.whenCmdline.empty()) {
            if (!cmdLine || !MatchesCmdLine(*cmdLine, dll.whenCmdline)) {
                return false;
            }
        }
        return true;
    }

} // namespace

void Apply(const PipeContext& ctx) {
    const auto injectDlls = Config::GetInjectDlls();
    if (injectDlls.empty()) return;
    if (!ctx.gameProcess) return;

    // Read the command line lazily: only if an injection entry uses it.
    std::optional<std::string> cmdLine;
    bool cmdLineResolved = false;
    auto commandLine = [&]() -> const std::optional<std::string>& {
        if (!cmdLineResolved) {
            cmdLine = OSTPlatform::Process::GetProcessCommandLine(ctx.process.pid);
            cmdLineResolved = true;
        }
        return cmdLine;
    };

    for (const auto& dll : injectDlls) {
        if (dll.path.empty()) continue;

        const std::optional<std::string>& cmd = dll.whenCmdline.empty() ? cmdLine : commandLine();
        if (!Matches(dll, ctx, cmd)) continue;

        InjectionClaimGuard claimGuard(ctx.process, dll.path);
        if (!claimGuard.IsClaimed()) continue;

        const std::filesystem::path path = OSTPlatform::Encoding::PathFromUtf8(dll.path);
        const auto status = OSTPlatform::RemoteProcess::InjectLibrary(ctx.process.pid, path);
        const std::string filenameUtf8 = OSTPlatform::Encoding::PathToUtf8(path.filename());
        if (status == OSTPlatform::RemoteProcess::InjectStatus::Ok) {
            claimGuard.Dismiss();
            LOG_INJECT_INFO("injected pid={} appid={} dll=\"{}\"",
                            ctx.process.pid, ctx.appId, filenameUtf8);
        } else {
            LOG_INJECT_WARN("inject failed pid={} appid={} status={} dll=\"{}\"",
                            ctx.process.pid, ctx.appId,
                            OSTPlatform::RemoteProcess::ToString(status),
                            filenameUtf8);
            // claimGuard destructor will automatically call UnclaimInjection!
        }
    }
}

} // namespace PipeManager::Injection
