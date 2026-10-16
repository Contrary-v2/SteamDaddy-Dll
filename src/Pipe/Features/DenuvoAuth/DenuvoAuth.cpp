#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/LuaConfig.h"
#include "Pipe/ProcessInspector.h"
#include "OSTPlatform/include/Process.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace PipeManager::DenuvoAuth {
namespace {

    constexpr std::chrono::milliseconds kStartupGraceDuration{2500};
    constexpr std::chrono::milliseconds kTicketLeaseDuration{3000};

    // Scheme 1 (Default): Targeted micro-pulse durations
    // Startup micro-pulse (300ms): satisfies initial Handshake offline token verification (avoiding 88500012)
    // while expiring well before the game engine initializes save directories (preventing save drift).
    constexpr std::chrono::milliseconds kScheme1StartupPulseDuration{300};

    // Ownership ticket micro-pulse (300ms): triggered specifically upon GetAppOwnershipTicketExtendedData,
    // allowing Denuvo's immediate memcmp cross-check (takes ~3ms in logs) to succeed (avoiding Error 54).
    // Automatically expires in 300ms, cleanly restoring authentic SteamID for gameplay and saves.
    constexpr std::chrono::milliseconds kScheme1TicketPulseDuration{300};

    // Encrypted ticket delivery pulse (2500ms): triggered specifically upon GetEncryptedAppTicket,
    // when ticket bytes are delivered to Denuvo for activation/re-activation.
    // 2500ms comfortably absorbs Denuvo's ~1.4s internal VM crypto hashing and subsequent
    // secondary-pipe verification burst (~1.8s total), cleanly completing activation
    // before game overlay or engine save systems initialize.
    constexpr std::chrono::milliseconds kScheme1DeliveryPulseDuration{2500};

    enum class Stage {
        None,
        Authorizing,
        EndAuthorization,
    };

    const char* ToString(Stage stage) {
        switch (stage) {
        case Stage::None:             return "None";
        case Stage::Authorizing:      return "Authorizing";
        case Stage::EndAuthorization: return "EndAuthorization";
        }
        return "?";
    }

    struct ProcessAuth {
        bool scanned = false;
        bool denuvo = false;
        bool isDAuth2 = false;
        bool startupArmed = false;
        bool scanning = false;
        uint32 pid = 0;

        // Scheme 1 (Default): Multi-pipe aware Micro-Pulse State Machine
        // Preserves authentic user save paths (userdata/<RealSteamID>/) while eliminating Error 54.
        std::chrono::steady_clock::time_point scheme1Deadline{};

        // Scheme 2 (-dauth2 / dauth2(appid)): Adaptive time-window lease
        std::chrono::steady_clock::time_point authDeadline{};

        AppId_t authorizedAppId = k_uAppIdInvalid;
        uint32 handshakeCount = 0;

        bool operator==(const ProcessAuth&) const = default;
        auto operator<=>(const ProcessAuth&) const = default;

        [[nodiscard]] Stage CurrentStage(std::chrono::steady_clock::time_point now) const noexcept {
            if (!denuvo) return Stage::None;
            const auto deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            if (deadline == std::chrono::steady_clock::time_point{}) return Stage::None;
            return now <= deadline ? Stage::Authorizing : Stage::EndAuthorization;
        }

        std::string DebugString() const {
            const auto now = std::chrono::steady_clock::now();
            const auto deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto stage = CurrentStage(now);
            return std::format("mode={} denuvo={} stage={} active={} remaining_ms={} handshakeCount={} auth_appid={} pid={}",
                               isDAuth2 ? "dauth2" : "default", denuvo, ToString(stage), now <= deadline,
                               remainingMs > 0 ? remainingMs : 0, handshakeCount, authorizedAppId, pid);
        }

        void OnHandshake(const PipeContext& ctx) {
            ++handshakeCount;

            if (ctx.appId != k_uAppIdInvalid) {
                authorizedAppId = ctx.appId;
            }
            pid = ctx.process.pid;

            if (!denuvo) {
                return;
            }

            if (isDAuth2) {
                // Startup grace period: arms 2500ms upon initial connection so Denuvo has ample
                // time to verify local offline tokens or launch handshake without leaking real SteamID (avoiding 88500012).
                if (!startupArmed) {
                    startupArmed = true;
                    const auto now = std::chrono::steady_clock::now();
                    const auto startupDeadline = now + kStartupGraceDuration;
                    if (startupDeadline > authDeadline) {
                        authDeadline = startupDeadline;
                    }
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 2 - dauth2] startup grace window armed for pid={} (+{}ms) {}",
                                  pid, kStartupGraceDuration.count(), this->DebugString());
                } else {
                    LOG_PIPE_DEBUG("DenuvoAuth: [Scheme 2 - dauth2] additional pipe connected handshakeCount={} {}",
                                   handshakeCount, this->DebugString());
                }
            } else {
                // Scheme 1 (Default): Multi-pipe aware startup micro-pulse (+300ms).
                // Arms on first handshake across any pipe of this process, providing enough time
                // for Denuvo to verify offline tokens without risking save directory drift.
                if (!startupArmed) {
                    startupArmed = true;
                    const auto now = std::chrono::steady_clock::now();
                    const auto startupDeadline = now + kScheme1StartupPulseDuration;
                    if (startupDeadline > scheme1Deadline) {
                        scheme1Deadline = startupDeadline;
                    }
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 1 - Default] startup micro-pulse armed for pid={} (+{}ms) {}",
                                  pid, kScheme1StartupPulseDuration.count(), this->DebugString());
                } else {
                    LOG_PIPE_DEBUG("DenuvoAuth: [Scheme 1 - Default] additional pipe connected handshakeCount={} {}",
                                   handshakeCount, this->DebugString());
                }
            }
        }

        void OnOwnershipTicketRequested() {
            if (!denuvo) return;
            const auto now = std::chrono::steady_clock::now();

            if (isDAuth2) {
                // When a ticket is fetched/requested, extend the lease by 3000ms.
                // Guarantees Denuvo's memcmp(Ticket->SteamID, GetSteamID()) verification easily passes (avoiding Error 54).
                const auto newDeadline = now + kTicketLeaseDuration;
                if (newDeadline > authDeadline) {
                    authDeadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 2 - dauth2] ticket lease extended for pid={} (+{}ms) {}",
                                  pid, kTicketLeaseDuration.count(), this->DebugString());
                }
            } else {
                // Scheme 1 (Default): Arm ownership ticket micro-pulse (+300ms).
                // Denuvo requests GetAppOwnershipTicketExtendedData and immediately performs
                // memcmp(Ticket->SteamID, GetSteamID()) cross-check (takes ~3ms in telemetry).
                // A 300ms micro-pulse provides high tolerance for Denuvo verification across all pipes,
                // while cleanly expiring before game engines access userdata/<RealSteamID>/ saves.
                const auto newDeadline = now + kScheme1TicketPulseDuration;
                if (newDeadline > scheme1Deadline) {
                    scheme1Deadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 1 - Default] ownership micro-pulse armed for pid={} (+{}ms) {}",
                                  pid, kScheme1TicketPulseDuration.count(), this->DebugString());
                }
            }
        }

        void OnEncryptedTicketRequested() {
            if (!denuvo) return;
            LOG_PIPE_DEBUG("DenuvoAuth: [Async Reservation] encrypted ticket requested (0ms pollution) for pid={} {}",
                           pid, this->DebugString());
        }

        void OnEncryptedTicketDelivered() {
            if (!denuvo) return;
            const auto now = std::chrono::steady_clock::now();

            if (isDAuth2) {
                const auto newDeadline = now + kTicketLeaseDuration;
                if (newDeadline > authDeadline) {
                    authDeadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 2 - dauth2] encrypted ticket delivered lease extended for pid={} (+{}ms) {}",
                                  pid, kTicketLeaseDuration.count(), this->DebugString());
                }
            } else {
                // Precision Delivery Pulse (2500ms): starts timing exactly when the ticket bytes
                // are delivered to Denuvo. Accommodates VM crypto computation (~1.4s) and the
                // secondary-pipe 5-hit verification burst (~1.8s) during initial activation or token refresh.
                const auto newDeadline = now + kScheme1DeliveryPulseDuration;
                if (newDeadline > scheme1Deadline) {
                    scheme1Deadline = newDeadline;
                    LOG_PIPE_INFO("DenuvoAuth: [Scheme 1 - Default] encrypted ticket delivered -> activation delivery pulse armed for pid={} (+{}ms) {}",
                                  pid, kScheme1DeliveryPulseDuration.count(), this->DebugString());
                }
            }
        }

        [[nodiscard]] bool CanUseAuthorizedIdentity() const noexcept {
            if (!denuvo) return false;
            const auto deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            if (deadline == std::chrono::steady_clock::time_point{}) return false;
            const auto now = std::chrono::steady_clock::now();
            return now <= deadline;
        }
    };

    std::mutex g_authMutex;
    std::condition_variable g_scanCv;
    std::unordered_map<ProcessKey, ProcessAuth, ProcessKeyHash> g_processAuth;
    std::unordered_map<PipeKey, ProcessKey, PipeKeyHash> g_pipeProcess;

    class ScanningGuard {
    public:
        explicit ScanningGuard(ProcessKey key) : key_(key), active_(true) {}
        ~ScanningGuard() {
            if (active_) {
                Dismiss();
            }
        }
        void Dismiss() noexcept {
            if (active_) {
                {
                    std::lock_guard lk(g_authMutex);
                    auto it = g_processAuth.find(key_);
                    if (it != g_processAuth.end()) {
                        it->second.scanning = false;
                    }
                }
                g_scanCv.notify_all();
                active_ = false;
            }
        }
        void DismissLocked(ProcessAuth& auth) noexcept {
            if (active_) {
                auth.scanning = false;
                g_scanCv.notify_all();
                active_ = false;
            }
        }
        ScanningGuard(const ScanningGuard&) = delete;
        ScanningGuard& operator=(const ScanningGuard&) = delete;
    private:
        ProcessKey key_;
        bool active_;
    };

    ProcessAuth* FindAuthForPipe(const PipeKey& pipeKey) {
        const auto pipeIt = g_pipeProcess.find(pipeKey);
        if (pipeIt != g_pipeProcess.end()) {
            const auto authIt = g_processAuth.find(pipeIt->second);
            if (authIt != g_processAuth.end()) return &authIt->second;
            g_pipeProcess.erase(pipeIt);
        }

        // Resilient fallback: match by active process if the specific pipe handle
        // was not handshaked yet or was evicted from g_pipeProcess.
        // Syscall optimization: pre-check in memory if pipeKey.pid exists in g_processAuth
        // before calling Win32 GetProcessCreationTime syscall.
        if (pipeKey.pid != 0) {
            bool hasPidCandidate = false;
            for (const auto& [procKey, _] : g_processAuth) {
                if (procKey.pid == pipeKey.pid) {
                    hasPidCandidate = true;
                    break;
                }
            }
            if (hasPidCandidate) {
                if (const auto currentCreation = ProcessInspector::GetProcessCreationTime(pipeKey.pid)) {
                    const ProcessKey activeKey{pipeKey.pid, *currentCreation};
                    const auto authIt = g_processAuth.find(activeKey);
                    if (authIt != g_processAuth.end()) {
                        g_pipeProcess[pipeKey] = activeKey;
                        return &authIt->second;
                    }
                }
            }
        }

        return nullptr;
    }

} // namespace

void Apply(const PipeContext& ctx) {
    if (!ctx.gameProcess || !ctx.trackedApp) return;

    const PipeKey pipeKey = MakePipeKey(ctx.pipe);
    if (!pipeKey.IsValid()) return;

    bool needsScan = false;
    std::optional<ScanningGuard> scanGuard;
    {
        std::unique_lock lock(g_authMutex);
        auto it = g_processAuth.find(ctx.process);
        if (it != g_processAuth.end() && it->second.scanning) {
            g_scanCv.wait_for(lock, std::chrono::seconds(5), [&]() {
                auto sit = g_processAuth.find(ctx.process);
                return sit == g_processAuth.end() || !sit->second.scanning;
            });
            it = g_processAuth.find(ctx.process);
        }

        if (it == g_processAuth.end() || !it->second.scanned) {
            needsScan = true;
            g_processAuth[ctx.process].scanning = true;
            scanGuard.emplace(ctx.process);
        }
    }

    bool denuvo = false;
    bool isDAuth2 = LuaConfig::IsDAuth2(ctx.appId);

    if (needsScan) {
        bool cmdLineQueried = false;
        std::optional<std::string> cmdLine;
        auto GetCmdLine = [&]() -> const std::string* {
            if (!cmdLineQueried && ctx.process.pid != 0) {
                cmdLineQueried = true;
                cmdLine = OSTPlatform::Process::GetProcessCommandLine(ctx.process.pid);
            }
            return cmdLine ? &*cmdLine : nullptr;
        };

        if (!isDAuth2) {
            if (const auto* cmd = GetCmdLine()) {
                if (HasDAuth2Arg(cmd->c_str())) {
                    isDAuth2 = true;
                    LuaConfig::SetCmdLineDAuth2(ctx.appId, true);
                    LOG_PIPE_INFO("DenuvoAuth: detected -dauth2 in process command line for appid={}", ctx.appId);
                }
            }
        }

        bool isNoDenuvo = LuaConfig::IsNoDenuvo(ctx.appId);
        bool isForcedDenuvo = LuaConfig::IsForcedDenuvo(ctx.appId);

        if (const auto* cmd = GetCmdLine()) {
            if (!isNoDenuvo && HasNoDenuvoArg(cmd->c_str())) {
                isNoDenuvo = true;
                LuaConfig::SetCmdLineNoDenuvo(ctx.appId, true);
                LOG_PIPE_INFO("DenuvoAuth: detected -nodenuvo in process command line for appid={}", ctx.appId);
            }
            if (!isForcedDenuvo && HasForcedDenuvoArg(cmd->c_str())) {
                isForcedDenuvo = true;
                LuaConfig::SetCmdLineForcedDenuvo(ctx.appId, true);
                LOG_PIPE_INFO("DenuvoAuth: detected -forcedenuvo in process command line for appid={}", ctx.appId);
            }
        }

        if (isNoDenuvo) {
            denuvo = false;
            LOG_PIPE_INFO("DenuvoAuth: nodenuvo appid={} — skipping ProtectionScan and forcing non-Denuvo", ctx.appId);
        } else if (isForcedDenuvo) {
            denuvo = true;
            LOG_PIPE_INFO("DenuvoAuth: forcedenuvo appid={} — skipping ProtectionScan and forcing Denuvo", ctx.appId);
        } else {
            denuvo = ScanProtection(ctx.process.pid).denuvoDetected;
        }
    }

    std::lock_guard lock(g_authMutex);
    if (g_processAuth.size() >= 256) {
        std::erase_if(g_processAuth, [&](const auto& pair) {
            if (pair.first == ctx.process) return false;
            auto currentCreation = ProcessInspector::GetProcessCreationTime(pair.first.pid);
            return !currentCreation || *currentCreation != pair.first.creationTime;
        });
        std::erase_if(g_pipeProcess, [&](const auto& pair) {
            return !g_processAuth.contains(pair.second);
        });
    }
    if (g_pipeProcess.size() >= 512) {
        std::erase_if(g_pipeProcess, [&](const auto& pair) {
            if (pair.second == ctx.process) return false;
            return !g_processAuth.contains(pair.second);
        });
    }

    ProcessAuth& auth = g_processAuth[ctx.process];
    g_pipeProcess[pipeKey] = ctx.process;

    if (needsScan && !auth.scanned) {
        auth.scanned = true;
        auth.denuvo = denuvo;
        auth.isDAuth2 = isDAuth2;
    } else {
        if (isDAuth2) {
            auth.isDAuth2 = true;
        }
        LOG_PIPE_TRACE("DenuvoAuth: reusing cached protection result {} denuvo={} isDAuth2={}",
                       ctx.process.DebugString(), auth.denuvo, auth.isDAuth2);
    }

    if (scanGuard) {
        scanGuard->DismissLocked(auth);
    }

    auth.OnHandshake(ctx);
}

namespace {

    template <typename Fn>
    void DispatchAuthForPipe(const CPipeClient* pipe, AppId_t appId, Fn&& fn) {
        std::lock_guard lock(g_authMutex);
        if (pipe) {
            const PipeKey pipeKey = MakePipeKey(pipe);
            ProcessAuth* auth = FindAuthForPipe(pipeKey);
            if (auth) {
                if (auth->denuvo) {
                    if (auth->authorizedAppId == k_uAppIdInvalid && appId != k_uAppIdInvalid) {
                        auth->authorizedAppId = appId;
                    }
                    fn(*auth);
                }
                return;
            }
        }
        if (appId == k_uAppIdInvalid) return;

        // Fallback: match by AppId when pipe is null or untracked.
        // Prioritize exact match for appId first to avoid ambiguous cross-dispatch.
        // Defend against dead processes: verify creation time matches procKey.
        for (auto& [procKey, auth] : g_processAuth) {
            if (auth.denuvo && auth.authorizedAppId == appId) {
                const auto creation = ProcessInspector::GetProcessCreationTime(procKey.pid);
                if (creation && *creation == procKey.creationTime) {
                    fn(auth);
                    return;
                }
            }
        }

        // Defend against cross-app corruption: verify InspectProcess resolves same AppId before binding.
        for (auto& [procKey, auth] : g_processAuth) {
            if (auth.denuvo && auth.authorizedAppId == k_uAppIdInvalid) {
                const auto creation = ProcessInspector::GetProcessCreationTime(procKey.pid);
                if (creation && *creation == procKey.creationTime) {
                    const auto inspect = ProcessInspector::InspectProcess(procKey.pid);
                    const auto resolved = inspect.ResolveAppId();
                    if (resolved == appId) {
                        auth.authorizedAppId = appId;
                        fn(auth);
                        return;
                    }
                }
            }
        }
    }

} // namespace

void OnTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    OnOwnershipTicketRequested(pipe, appId);
}

void OnOwnershipTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    DispatchAuthForPipe(pipe, appId, [](ProcessAuth& auth) {
        auth.OnOwnershipTicketRequested();
    });
}

void OnEncryptedTicketRequested(const CPipeClient* pipe, AppId_t appId) {
    DispatchAuthForPipe(pipe, appId, [](ProcessAuth& auth) {
        auth.OnEncryptedTicketRequested();
    });
}

void OnEncryptedTicketDelivered(const CPipeClient* pipe, AppId_t appId) {
    DispatchAuthForPipe(pipe, appId, [](ProcessAuth& auth) {
        auth.OnEncryptedTicketDelivered();
    });
}


bool IsAuthorizedPipe(const CPipeClient* pipe) {
    if (!pipe) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window: null pipe");
        return false;
    }

    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    const ProcessAuth* auth = FindAuthForPipe(pipeKey);
    if (!auth) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not tracked by DenuvoAuth {}", pipeKey.DebugString());
        return false;
    }

    if (!auth->CanUseAuthorizedIdentity()) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe outside authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
        return false;
    }
    LOG_PIPE_DEBUG("DenuvoAuth: pipe in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
    return true;
}

bool IsDenuvoPipe(const CPipeClient* pipe) {
    if (!pipe) return false;
    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    const ProcessAuth* auth = FindAuthForPipe(pipeKey);
    return auth && auth->denuvo;
}

AppId_t GetAuthorizedAppId(const CPipeClient* pipe) {
    if (!pipe) return k_uAppIdInvalid;
    const PipeKey pipeKey = MakePipeKey(pipe);
    std::lock_guard lock(g_authMutex);
    const ProcessAuth* auth = FindAuthForPipe(pipeKey);
    return auth ? auth->authorizedAppId : k_uAppIdInvalid;
}

} // namespace PipeManager::DenuvoAuth
