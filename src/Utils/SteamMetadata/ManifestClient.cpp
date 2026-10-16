#include "ManifestClient.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

namespace ManifestClient {

    // ── parsers ────────────────────────────────────────────────────
    using Parser = bool (*)(std::string_view body, uint64_t* out);

    // [PROVIDER-FIX] 1.5.3.6 version: does NOT strip whitespace before
    // from_chars; relies on the HTTP layer delivering a clean body.
    // The 1.4.8.16 version added trimming for robustness — kept here
    // because it is strictly safer (HTML error pages, extra newlines).
    static bool ParsePlainUint(std::string_view body, uint64_t* out) noexcept {
        if (!out) return false;
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front())))
            body.remove_prefix(1);
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())))
            body.remove_suffix(1);
        if (body.empty()) return false;
        uint64_t code = 0;
        auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), code);
        // Require full consumption so HTML/partial text is rejected.
        if (ec != std::errc{} || ptr != body.data() + body.size()) return false;
        *out = code;
        return true;
    }

    // [PROVIDER-FIX] Dual-mode JSON parser: handles both
    //   "content": "1234567890"   (quoted string — used by steamrun)
    //   "content": 1234567890     (raw integer — future-proof)
    // The 1.5.3.6 version only handles the quoted form; kept the
    // 1.4.8.16 dual-mode implementation as it is a strict superset.
    static bool ParseSteamRunJson(std::string_view body, uint64_t* out) noexcept {
        if (!out) return false;
        const size_t key = body.find("\"content\"");
        if (key == std::string_view::npos) return false;

        const size_t colon = body.find(':', key + 9);
        if (colon == std::string_view::npos) return false;

        std::string_view rest = body.substr(colon + 1);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            rest.remove_prefix(1);
        if (rest.empty()) return false;

        if (rest.front() == '"') {
            rest.remove_prefix(1);
            const size_t closeQuote = rest.find('"');
            if (closeQuote == std::string_view::npos) return false;
            return ParsePlainUint(rest.substr(0, closeQuote), out);
        }

        size_t endIdx = 0;
        while (endIdx < rest.size() && std::isdigit(static_cast<unsigned char>(rest[endIdx])))
            ++endIdx;
        if (endIdx == 0) return false;
        return ParsePlainUint(rest.substr(0, endIdx), out);
    }

    // ── provider table ────────────────────────────────────────────
    //
    // Table order = fallback priority order; first row = default preferred source.
    // Adding a provider: add one Make() row here, nothing else changes.
    //
    // needsDepot=true  → urlTemplate has TWO %llu: (depotId, gid).
    //                    The provider is skipped when depotId==0.
    // needsDepot=false → urlTemplate has ONE %llu: (gid).

    struct Provider {
        std::string_view name;
        const char*      urlTemplate;   // snprintf format: one or two %llu
        Parser           parse;
        bool             needsDepot;    // requires two-arg URL substitution
        const wchar_t*   headers;       // extra WinHTTP request headers, or nullptr
    };

    consteval Provider Make(std::string_view name, const char* url, Parser parse,
                            bool needsDepot = false, const wchar_t* headers = nullptr) {
        return {name, url, parse, needsDepot, headers};
    }

    // [PROVIDER-FIX] Added 20770407 and SDM (two-arg providers); reordered
    // to match the intended priority from the audit spec.
    static constexpr Provider kProviders[] = {
        Make("20770407",     "https://20770407.xyz/manifest/%llu/%llu",          ParsePlainUint, true),
        Make("SDM",          "https://steamapi.993499094.xyz/manifest/%llu/%llu",ParsePlainUint, true),
        Make("manifestdex",  "https://manifest.manifestdex.com/%llu",            ParsePlainUint, false,
             L"User-Agent: ManifestDeX/1.0\r\n"),
        Make("wudrm",        "http://gmrc.wudrm.com/manifest/%llu",              ParsePlainUint),
        Make("steamrun",     "https://manifest.steam.run/api/manifest/%llu",     ParseSteamRunJson),
        Make("opensteamtool","https://manifest.opensteamtool.com/%llu",          ParsePlainUint),
    };

    static constexpr size_t kProviderCount = std::size(kProviders);

    // ── provider state ────────────────────────────────────────────
    //
    // g_stateMutex guards ONLY in-memory index/cooldown fields.
    // It is never held across HTTP I/O so a slow upstream cannot
    // block concurrent depot fetches.

    static std::mutex g_stateMutex;

    // [PROVIDER-FIX] Replaced atomic<Provider*> with index + mutex.
    // This enables per-provider cooldown tracking and failover.
    static size_t g_active = 0;                         // preferred provider index

    // Cooldown endpoints: provider i is skipped until g_deadUntil[i] passes.
    static std::chrono::steady_clock::time_point g_deadUntil[kProviderCount];

    // Success sequence numbers: used to discard stale concurrent failures
    // that arrive after a later success has already cleared a provider.
    static uint64_t g_okSeq[kProviderCount] = {};

    // Failover master switch (corresponds to [manifest] failover in config).
    static bool g_failoverEnabled = true;

    static size_t ActiveIndex() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_active;
    }

    static bool FailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failoverEnabled;
    }

    static bool IsCoolingDown(size_t i, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_deadUntil[i] > now;
    }

    static uint64_t OkSeq(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_okSeq[i];
    }

    static void MarkFailed(size_t i, std::chrono::steady_clock::time_point now, uint64_t seq) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        // If a success arrived between the pre-request sample and this failure,
        // the failure is stale — do not punish a provider that just proved itself alive.
        if (g_okSeq[i] != seq) {
            LOG_MANIFEST_DEBUG("Manifest {} failure ignored (superseded by a success)",
                               kProviders[i].name);
            return;
        }
        g_deadUntil[i] = now + std::chrono::milliseconds(kProviderCooldownMs);
    }

    static void RecordSuccess(size_t i) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ++g_okSeq[i];
        g_deadUntil[i] = {};
    }

    // ── public state API ──────────────────────────────────────────

    bool SetProvider(std::string_view name) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        for (size_t i = 0; i < kProviderCount; ++i) {
            if (kProviders[i].name == name) {
                // Re-selecting the same provider is a no-op: a config reload
                // triggers full re-application and we must not reset a known-bad
                // provider's cooldown just because the config was touched.
                if (i == g_active) return true;
                g_active = i;
                // Clear cooldown only for the newly selected provider so that
                // in-flight failures from the old selection don't carry over.
                g_deadUntil[i] = {};
                ++g_okSeq[i];
                return true;
            }
        }
        return false;
    }

    // [PROVIDER-FIX] New API: wired by Config.cpp to [manifest] failover.
    void SetFailoverEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_failoverEnabled == enabled) return;
        g_failoverEnabled = enabled;
        // When the switch is toggled, stale cooldown conclusions are no longer
        // trustworthy — reset to a clean baseline.
        for (auto& t : g_deadUntil) t = {};
    }

    bool IsFailoverEnabled() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return g_failoverEnabled;
    }

    const char* ActiveProviderName() {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        return kProviders[g_active].name.data();
    }

    void Shutdown() {
        // No persistent connection to tear down; state is reset at next SetProvider.
    }

    // ── fetch ─────────────────────────────────────────────────────

    // Issue one HTTP request to provider p. Caller supplies clamped timeouts.
    // [PROVIDER-FIX] needsDepot providers get two-arg URL (depotId, gid).
    static bool FetchFrom(const Provider& p, uint64_t gid, AppId_t depotId, uint64_t* outCode,
                          uint32_t resolveMs, uint32_t connectMs, uint32_t sendMs, uint32_t recvMs) {
        char url[512];
        if (p.needsDepot) {
            if (!depotId) return false;   // cannot form URL without depot
            std::snprintf(url, sizeof(url), p.urlTemplate,
                          static_cast<unsigned long long>(depotId),
                          static_cast<unsigned long long>(gid));
        } else {
            std::snprintf(url, sizeof(url), p.urlTemplate,
                          static_cast<unsigned long long>(gid));
        }

        auto r = OSTPlatform::Http::Execute(
            L"GET",
            url,
            nullptr,
            0,
            p.headers,
            resolveMs,
            connectMs,
            sendMs,
            recvMs);

        LOG_MANIFEST_INFO("Manifest {} status={} depot={} gid={}", p.name, r.status, depotId, gid);

        if (!r.ok || r.status != 200) return false;
        return p.parse(r.body, outCode);
    }

    // Failover disabled: try only the configured preferred provider.
    static bool FetchActive(uint64_t gid, uint64_t* outCode, AppId_t depotId = 0) {
        const Provider& p = kProviders[ActiveIndex()];
        const Config::ManifestTimeouts t = Config::GetManifestTimeouts();
        return FetchFrom(p, gid, depotId, outCode, t.resolve, t.connect, t.send, t.recv);
    }

    // [PROVIDER-FIX] Failover enabled: walk the provider table starting from
    // the preferred index. Skip providers that are cooling down or require a
    // depotId we don't have. Clamp each attempt's timeout to the remaining
    // budget so the total wall time stays bounded.
    static bool FetchWithFailover(uint64_t gid, uint64_t* outCode, AppId_t depotId = 0) {
        const Config::ManifestTimeouts t = Config::GetManifestTimeouts();
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(kFetchBudgetMs);

        // Snapshot start index once so a concurrent SetProvider call mid-fetch
        // does not scramble this iteration's ordering.
        const size_t start = ActiveIndex();
        size_t tried = 0;

        for (size_t k = 0; k < kProviderCount; ++k) {
            const size_t i = (start + k) % kProviderCount;
            const Provider& p = kProviders[i];

            const auto now = std::chrono::steady_clock::now();

            if (IsCoolingDown(i, now)) {
                LOG_MANIFEST_DEBUG("Manifest {} skipped (cooldown)", p.name);
                continue;
            }
            if (p.needsDepot && !depotId) {
                // Not a failure: we simply cannot form a valid URL.
                LOG_MANIFEST_DEBUG("Manifest {} skipped (no depot id)", p.name);
                continue;
            }

            // Sample success counter before the request so we can detect
            // whether a concurrent success arrived while we were in-flight.
            const uint64_t seq = OkSeq(i);

            // Clamp per-attempt timeouts to remaining budget.
            // WinHTTP treats timeout=0 as infinite; floor at 1ms.
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (left <= 0) {
                LOG_MANIFEST_WARN("Manifest gid={}: {}ms budget exhausted after {} attempt(s)",
                                  gid, kFetchBudgetMs, tried);
                break;
            }
            const auto cap = [left](uint32_t ms) -> uint32_t {
                return static_cast<uint32_t>(
                    std::max<int64_t>(1, std::min<int64_t>(ms, left)));
            };

            const bool ok = FetchFrom(p, gid, depotId, outCode,
                                      cap(t.resolve), cap(t.connect),
                                      cap(t.send), cap(t.recv));
            ++tried;

            if (ok) {
                RecordSuccess(i);
                return true;
            }

            MarkFailed(i, std::chrono::steady_clock::now(), seq);
        }

        if (tried == 0)
            LOG_MANIFEST_WARN("Manifest gid={}: every provider is in cooldown or requires depot id", gid);
        return false;
    }

    // ── public entry point ────────────────────────────────────────

    // [PROVIDER-FIX] Lua calls are now serialised under g_luaMutex because
    // LuaConfig exposes a single shared lua_State without internal locking.
    // The mutex is released before any HTTP I/O begins.
    static std::mutex g_luaMutex;

    static bool TryLua(uint64_t manifestGid, uint64_t* outRequestCode,
                       AppId_t appId, AppId_t depotId)
    {
        std::lock_guard<std::mutex> lock(g_luaMutex);

        if (appId && depotId && LuaConfig::HasManifestCodeFuncEx()) {
            if (LuaConfig::CallManifestFetchCodeEx(appId, depotId, manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via fetch_manifest_code_ex", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} fetch_manifest_code_ex returned nil, trying fetch_manifest_code", manifestGid);
        }

        if (LuaConfig::HasManifestCodeFunc()) {
            if (LuaConfig::CallManifestFetchCode(manifestGid, outRequestCode)) {
                LOG_MANIFEST_INFO("Manifest gid={} resolved via manifest.lua", manifestGid);
                return true;
            }
            LOG_MANIFEST_WARN("Manifest gid={} lua returned nil, falling back to config", manifestGid);
        }

        return false;
    }

    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId, AppId_t depotId)
    {
        if (!outRequestCode) return false;

        if (TryLua(manifestGid, outRequestCode, appId, depotId))
            return true;

        if (FailoverEnabled())
            return FetchWithFailover(manifestGid, outRequestCode, depotId);

        return FetchActive(manifestGid, outRequestCode, depotId);
    }

} // namespace ManifestClient
