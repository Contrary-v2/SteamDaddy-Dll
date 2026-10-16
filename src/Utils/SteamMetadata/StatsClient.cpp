#include "StatsClient.h"

#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace StatsClient {
namespace {

    using namespace std::chrono_literals;

    std::shared_mutex g_mutex;
    std::atomic<bool> g_isShuttingDown{false};
    std::atomic<uint32_t> g_activeWorkers{0};

    std::unordered_map<AppId_t, uint64_t> g_cache;
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_failedLookups;
    std::unordered_set<AppId_t> g_inFlight;

    constexpr auto kFailureCooldown = 60s;
    constexpr size_t kMaxCacheSize = 2048;
    constexpr size_t kMaxFailedSize = 512;

    constexpr uint32_t kResolveMs = 1500;
    constexpr uint32_t kConnectMs = 1500;
    constexpr uint32_t kSendMs    = 2000;
    constexpr uint32_t kRecvMs    = 3000;

    static bool ParseSteamId(std::string_view body, uint64_t* outSteamId) noexcept {
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(body);
        if (!parsed || *parsed == 0) return false;
        *outSteamId = *parsed;
        return true;
    }

    static void EvictExpiredFailures(const std::chrono::steady_clock::time_point& now)
    {
        if (g_failedLookups.size() < kMaxFailedSize) return;

        // Evict expired entries first to prevent full-table purge avalanche
        for (auto it = g_failedLookups.begin(); it != g_failedLookups.end(); ) {
            if (now - it->second >= kFailureCooldown) {
                it = g_failedLookups.erase(it);
            } else {
                ++it;
            }
        }

        // If still over limit, drop oldest entries
        if (g_failedLookups.size() >= kMaxFailedSize) {
            size_t countToRemove = g_failedLookups.size() / 2;
            auto it = g_failedLookups.begin();
            while (countToRemove > 0 && it != g_failedLookups.end()) {
                it = g_failedLookups.erase(it);
                --countToRemove;
            }
        }
    }

    bool ExecuteFetch(AppId_t appId, uint64_t* outSteamId) {
        if (g_isShuttingDown.load(std::memory_order_acquire)) {
            return false;
        }

        char url[128];
        std::snprintf(url, sizeof(url), "https://stats.opensteamtool.com/%u", appId);

        auto r = OSTPlatform::Http::Execute(
            L"GET", url, nullptr, 0, nullptr,
            kResolveMs, kConnectMs, kSendMs, kRecvMs);
        LOG_ACHIEVEMENT_INFO("Stats SteamID API status={} appid={}", r.status, appId);

        const auto now = std::chrono::steady_clock::now();
        if (!r.ok || r.status != 200) {
            LOG_ACHIEVEMENT_WARN("Stats SteamID API failed appid={} status={} ok={}", appId, r.status, r.ok);
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_inFlight.erase(appId);
            EvictExpiredFailures(now);
            g_failedLookups[appId] = now;
            return false;
        }

        uint64_t steamId = 0;
        if (!ParseSteamId(r.body, &steamId)) {
            LOG_ACHIEVEMENT_WARN("Stats SteamID API returned invalid body appid={} bytes={}", appId, r.body.size());
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_inFlight.erase(appId);
            EvictExpiredFailures(now);
            g_failedLookups[appId] = now;
            return false;
        }

        {
            std::unique_lock<std::shared_mutex> lock(g_mutex);
            g_inFlight.erase(appId);
            g_failedLookups.erase(appId);

            if (g_cache.size() >= kMaxCacheSize) {
                // Drop a fraction of cache rather than clearing everything to avoid avalanche
                size_t countToRemove = g_cache.size() / 4;
                auto it = g_cache.begin();
                while (countToRemove > 0 && it != g_cache.end()) {
                    it = g_cache.erase(it);
                    --countToRemove;
                }
            }
            g_cache[appId] = steamId;
        }

        if (outSteamId) {
            *outSteamId = steamId;
        }
        LOG_ACHIEVEMENT_INFO("Stats SteamID API resolved appid={} steamid={}", appId, steamId);
        return true;
    }

} // namespace

bool TryGetCachedStatSteamId(AppId_t appId, uint64_t* outSteamId) {
    if (!outSteamId || appId == k_uAppIdInvalid) return false;

    // Fast read path under shared lock
    {
        std::shared_lock<std::shared_mutex> readLock(g_mutex);
        auto it = g_cache.find(appId);
        if (it != g_cache.end()) {
            *outSteamId = it->second;
            return true;
        }
    }

    // Cache miss: trigger async pre-warm (non-blocking)
    PrewarmStatSteamId(appId);
    return false;
}

void PrewarmStatSteamId(AppId_t appId) {
    if (appId == k_uAppIdInvalid) return;

    if (!Config::GetStatsEnableApi() || g_isShuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    bool shouldStart = false;

    // Single-lock critical section: check cache, in-flight, cooldown, and mark in-flight
    {
        std::unique_lock<std::shared_mutex> writeLock(g_mutex);
        if (g_cache.contains(appId) || g_inFlight.contains(appId)) {
            return;
        }

        auto failIt = g_failedLookups.find(appId);
        if (failIt != g_failedLookups.end()) {
            if (now - failIt->second < kFailureCooldown) {
                LOG_ACHIEVEMENT_TRACE("Stats SteamID API in cooldown for appid={}", appId);
                return;
            }
            g_failedLookups.erase(failIt);
        }

        g_inFlight.insert(appId);
        shouldStart = true;
    }

    if (shouldStart) {
        LOG_ACHIEVEMENT_DEBUG("Starting background prewarm for stat steamid appid={}", appId);
        g_activeWorkers.fetch_add(1, std::memory_order_relaxed);

        const bool started = OSTPlatform::Thread::StartDetached([appId]() -> uint32_t {
            struct WorkerGuard {
                ~WorkerGuard() {
                    g_activeWorkers.fetch_sub(1, std::memory_order_relaxed);
                }
            } guard;

            if (g_isShuttingDown.load(std::memory_order_acquire)) {
                return 0;
            }
            ExecuteFetch(appId, nullptr);
            return 0;
        });

        if (!started) {
            LOG_ACHIEVEMENT_WARN("Failed to start detached prewarm thread for appid={}", appId);
            g_activeWorkers.fetch_sub(1, std::memory_order_relaxed);
            std::unique_lock<std::shared_mutex> writeLock(g_mutex);
            g_inFlight.erase(appId);
        }
    }
}

bool FetchStatSteamId(AppId_t appId, uint64_t* outSteamId) {
    if (!outSteamId || appId == k_uAppIdInvalid) return false;

    if (!Config::GetStatsEnableApi() || g_isShuttingDown.load(std::memory_order_acquire)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::shared_mutex> writeLock(g_mutex);
        auto it = g_cache.find(appId);
        if (it != g_cache.end()) {
            *outSteamId = it->second;
            LOG_ACHIEVEMENT_DEBUG("Stats SteamID API cache hit appid={} steamid={}", appId, *outSteamId);
            return true;
        }

        auto failIt = g_failedLookups.find(appId);
        if (failIt != g_failedLookups.end()) {
            if (now - failIt->second < kFailureCooldown) {
                LOG_ACHIEVEMENT_TRACE("Stats SteamID API in cooldown for appid={}", appId);
                return false;
            }
            g_failedLookups.erase(failIt);
        }

        g_inFlight.insert(appId);
    }

    return ExecuteFetch(appId, outSteamId);
}

void Shutdown() noexcept {
    g_isShuttingDown.store(true, std::memory_order_release);
    std::unique_lock<std::shared_mutex> lock(g_mutex);
    g_cache.clear();
    g_inFlight.clear();
    g_failedLookups.clear();
}

} // namespace StatsClient
