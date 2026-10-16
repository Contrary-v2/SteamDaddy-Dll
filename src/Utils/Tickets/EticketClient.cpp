#include "EticketClient.h"

#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace EticketClient {
namespace {

    // On-demand mint endpoint. The backend is POSTed
    // {app_id, nonce(hex), existing_steam_id} and returns
    // {eticket, appticket, steam_id}. Any failure falls back to the static
    // credential-store ticket.
    //
    // Resolved in two steps so a build can be self-contained without putting
    // any one deployment's backend into public source:
    //   1. seteticketurl() in the Lua config, if called (runtime override).
    //   2. OST_ETICKET_URL, baked in at compile time via
    //        cmake -DOST_ETICKET_URL="https://your-host/eticket"
    //
    // Empty when neither is set, which disables the feature outright: the DLL
    // never makes a network request and behaves exactly like stock OST.
#ifndef OST_ETICKET_URL
#define OST_ETICKET_URL ""
#endif

    std::string EticketUrl() {
        const std::string& configured = LuaConfig::GetEticketUrl();
        if (!configured.empty()) return configured;
        return std::string(OST_ETICKET_URL);
    }

    // Short connect timeouts so a down/unreachable backend fails fast and the
    // caller falls back; generous recv because the backend mints via a live
    // Steam CM round-trip (~1-5s).
    using namespace std::chrono_literals;
    constexpr auto kResolveTimeout = 1000ms;
    constexpr auto kConnectTimeout = 1500ms;
    constexpr auto kSendTimeout    = 2000ms;
    constexpr auto kRecvTimeout    = 8000ms;

    constexpr uint32_t kResolveMs = static_cast<uint32_t>(kResolveTimeout.count());
    constexpr uint32_t kConnectMs = static_cast<uint32_t>(kConnectTimeout.count());
    constexpr uint32_t kSendMs    = static_cast<uint32_t>(kSendTimeout.count());
    constexpr uint32_t kRecvMs    = static_cast<uint32_t>(kRecvTimeout.count());

    struct CachedTickets {
        std::vector<uint8_t> eticket;
        std::vector<uint8_t> ownership;
        // The pool account these tickets were minted under. If the registry's
        // current account later differs (user re-activated onto a different pool
        // account mid-session), the cache is evicted and re-minted so the served
        // ticket never disagrees with the account Denuvo now sees.
        uint64_t steamId = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<AppId_t, CachedTickets> g_cache;  // only successful fetches are cached
    // Apps the backend has told us it has no owning pool account for. There's no
    // point hammering the backend (or logging) on every retry within a launch, so
    // we skip on-demand for the rest of the session once we learn this.
    std::unordered_set<AppId_t> g_noOwnerApps;

    [[nodiscard]] std::string ToHex(std::span<const uint8_t> bytes) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            out.push_back(digits[b >> 4]);
            out.push_back(digits[b & 0x0F]);
        }
        return out;
    }

    [[nodiscard]] constexpr int HexNibble(char c) noexcept {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    [[nodiscard]] bool FromHex(std::string_view hex, std::vector<uint8_t>& out) noexcept {
        if (hex.empty() || (hex.size() % 2) != 0) {
            out.clear();
            return false;
        }
        out.clear();
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            int hi = HexNibble(hex[i]);
            int lo = HexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0) {
                out.clear();
                return false;
            }
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return true;
    }

    // Fast zero-allocation JSON scanner for top-level keys.
    // Handles escape sequences, whitespace, and distinguishes strings from bare values (number/null).
    [[nodiscard]] bool FindJsonValue(std::string_view json, std::string_view key, std::string_view& val, bool& isQuoted) noexcept {
        val = {};
        isQuoted = false;
        size_t i = 0;
        const size_t n = json.size();

        while (i < n) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const size_t keyStart = i + 1;
            size_t keyEnd = keyStart;
            bool keyEscaped = false;
            while (keyEnd < n) {
                if (json[keyEnd] == '\\') {
                    if (keyEnd + 1 >= n) return false;
                    keyEscaped = true;
                    keyEnd += 2;
                    continue;
                }
                if (json[keyEnd] == '"') {
                    break;
                }
                ++keyEnd;
            }

            if (keyEnd >= n) return false;

            const std::string_view currentKey = json.substr(keyStart, keyEnd - keyStart);
            i = keyEnd + 1;

            while (i < n && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
                ++i;
            }
            if (i >= n || json[i] != ':') {
                continue;
            }
            ++i;

            while (i < n && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
                ++i;
            }
            if (i >= n) return false;

            const bool match = (!keyEscaped && currentKey == key);

            if (json[i] == '"') {
                const size_t valStart = i + 1;
                size_t valEnd = valStart;
                while (valEnd < n) {
                    if (json[valEnd] == '\\') {
                        if (valEnd + 1 >= n) return false;
                        valEnd += 2;
                        continue;
                    }
                    if (json[valEnd] == '"') {
                        break;
                    }
                    ++valEnd;
                }
                if (valEnd >= n) return false;

                if (match) {
                    val = json.substr(valStart, valEnd - valStart);
                    isQuoted = true;
                    return true;
                }
                i = valEnd + 1;
            } else {
                const size_t valStart = i;
                size_t valEnd = valStart;
                while (valEnd < n &&
                       json[valEnd] != ',' && json[valEnd] != '}' && json[valEnd] != ']' &&
                       json[valEnd] != ' ' && json[valEnd] != '\t' && json[valEnd] != '\r' && json[valEnd] != '\n') {
                    ++valEnd;
                }

                if (match) {
                    val = json.substr(valStart, valEnd - valStart);
                    isQuoted = false;
                    if (val == "null") return false;
                    return true;
                }
                i = valEnd;
            }
        }

        return false;
    }

    [[nodiscard]] bool ExtractStringField(std::string_view body, std::string_view key, std::string_view& out) noexcept {
        bool isQuoted = false;
        if (!FindJsonValue(body, key, out, isQuoted)) return false;
        return isQuoted && !out.empty();
    }

    [[nodiscard]] bool ExtractStringField(std::string_view body, std::string_view key, std::string& out) {
        std::string_view sv;
        if (!ExtractStringField(body, key, sv)) return false;
        out.assign(sv);
        return true;
    }

    [[nodiscard]] bool ExtractUInt64Field(std::string_view body, std::string_view key, uint64_t& out) noexcept {
        std::string_view val;
        bool isQuoted = false;
        if (!FindJsonValue(body, key, val, isQuoted)) return false;
        if (val.empty()) return false;
        const auto parsed = OSTPlatform::Numbers::ParseUInt64(val);
        if (parsed) {
            out = *parsed;
            return true;
        }
        return false;
    }

    // Single backend mint → both tickets. Cached per app on success; failures are
    // not cached so the next call (the game retries ownership/eticket) re-attempts
    // — except a "no owning account" verdict, which is sticky for the session.
    // nonce and existingSteamId are only used on the first fetch for an app; once
    // an entry is cached, subsequent calls (ownership vs eticket, any order) share
    // it so both layers always align to the same account.
    bool EnsureFetched(AppId_t appId, std::span<const uint8_t> nonce, uint64_t existingSteamId, CachedTickets& out) {
        if (appId == 0) return false;

        // No seteticketurl() in the config: feature off, never touch the network.
        // Callers fall back to the static credential-store ticket, i.e. stock OST.
        const std::string endpointUrl = EticketUrl();
        if (endpointUrl.empty()) return false;

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_noOwnerApps.count(appId)) return false;  // already known: no pool owner
            auto it = g_cache.find(appId);
            if (it != g_cache.end()) {
                // Evict if the registry's current account differs from the one we
                // cached — a re-activation onto a different pool account must not
                // be served the previous account's ticket.
                const uint64_t cachedId = it->second.steamId;
                if (existingSteamId != 0 && cachedId != 0 && cachedId != existingSteamId) {
                    LOG_IPC_DEBUG("EticketClient: appid={} registry SteamID={} differs from cached SteamID={} — evicting stale cache entry and re-minting",
                                  appId, existingSteamId, cachedId);
                    g_cache.erase(it);
                } else {
                    out = it->second;
                    return true;
                }
            }
        }

        const std::string nonceHex = ToHex(nonce);
        std::string reqBody =
            "{\"app_id\":\"" + std::to_string(appId) + "\",\"nonce\":\"" + nonceHex + "\"";
        // Only send existing_steam_id when we actually have one — an empty/zero
        // value would make the backend refuse (it's read as "a ticket exists but
        // for account 0", i.e. foreign) instead of picking an owner itself.
        if (existingSteamId != 0) {
            reqBody += ",\"existing_steam_id\":\"" + std::to_string(existingSteamId) + "\"";
        }
        reqBody += "}";

        auto r = OSTPlatform::Http::Execute(
            L"POST", endpointUrl.c_str(),
            reqBody.data(), static_cast<uint32_t>(reqBody.size()),
            L"Content-Type: application/json\r\n",
            kResolveMs, kConnectMs, kSendMs, kRecvMs);

        if (!r.ok) {
            LOG_IPC_WARN("EticketClient: on-demand fetch failed appid={} status={} ok={} (fallback to credential store)",
                         appId, r.status, r.ok);
            return false;
        }

        // 409 = the backend deliberately refused. Two distinct reasons:
        //   - no owning account: nothing in the pool owns this app → skip for the
        //     rest of the session (sticky) so we stop retrying/logging.
        //   - foreign_account: the static ticket already in the registry belongs
        //     to an account the backend doesn't operate → it (correctly) won't
        //     mint a DIFFERENT account's ticket. Fall back to the static ticket,
        //     but DON'T make it sticky — a later re-activation could change it.
        if (r.status == 409) {
            if (r.body.find("\"foreign_account\":true") != std::string::npos) {
                LOG_IPC_DEBUG("EticketClient: appid={} existing ticket belongs to an account outside our pool — skipping on-demand override for this launch",
                              appId);
            } else {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (g_noOwnerApps.size() >= 256) {
                        g_noOwnerApps.clear();
                    }
                    g_noOwnerApps.insert(appId);
                }
                LOG_IPC_DEBUG("EticketClient: appid={} no owning account in pool — skipping on-demand for this session",
                              appId);
            }
            return false;
        }

        if (r.status != 200) {
            LOG_IPC_WARN("EticketClient: on-demand fetch failed appid={} status={} ok={} (fallback to credential store)",
                         appId, r.status, r.ok);
            return false;
        }

        CachedTickets fetched;
        std::string_view hexView;
        if (ExtractStringField(r.body, "eticket", hexView)) {
            if (!FromHex(hexView, fetched.eticket)) fetched.eticket.clear();
        }
        if (ExtractStringField(r.body, "appticket", hexView)) {
            if (!FromHex(hexView, fetched.ownership)) fetched.ownership.clear();
        }

        if (fetched.eticket.empty() && fetched.ownership.empty()) {
            LOG_IPC_WARN("EticketClient: backend returned no usable tickets appid={} bytes={}", appId, r.body.size());
            return false;
        }

        // Remember which pool account the backend minted under, so a later call
        // whose registry account differs triggers the eviction above.
        if (!ExtractUInt64Field(r.body, "steam_id", fetched.steamId)) {
            LOG_IPC_TRACE("EticketClient: steam_id absent or non-numeric in response for appid={}", appId);
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_cache.size() >= 256) {
                g_cache.clear();
            }
            g_cache[appId] = fetched;
            out = fetched;
        }
        LOG_IPC_INFO("EticketClient: minted appid={} eticket_bytes={} ownership_bytes={} nonce_bytes={}",
                     appId, fetched.eticket.size(), fetched.ownership.size(), nonce.size());
        return true;
    }

} // namespace

std::optional<std::vector<uint8_t>> FetchFreshEticket(AppId_t appId, std::span<const uint8_t> nonce, uint64_t existingSteamId) {
    CachedTickets t;
    if (!EnsureFetched(appId, nonce, existingSteamId, t) || t.eticket.empty()) return std::nullopt;
    return t.eticket;
}

std::optional<std::vector<uint8_t>> FetchOwnershipTicket(AppId_t appId, std::span<const uint8_t> nonce, uint64_t existingSteamId) {
    CachedTickets t;
    if (!EnsureFetched(appId, nonce, existingSteamId, t) || t.ownership.empty()) return std::nullopt;
    return t.ownership;
}

} // namespace EticketClient
