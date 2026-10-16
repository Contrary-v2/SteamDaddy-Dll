#include "PendingAPICalls.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace PendingAPICalls {

namespace {

    constexpr size_t kMaxPendingCapacity = 256;
    constexpr auto kTicketExpiryDuration = std::chrono::minutes(2);

    struct TicketEntry {
        AppId_t appId = k_uAppIdInvalid;
        std::chrono::steady_clock::time_point insertedAt{};
    };

    std::mutex g_mutex;
    std::unordered_map<SteamAPICall_t, TicketEntry> g_encryptedTickets;

} // namespace

void RecordEncryptedTicket(SteamAPICall_t call, AppId_t appID) noexcept
{
    // Pre-lock validation: zero-cost fast rejection of invalid inputs
    if (call == k_uAPICallInvalid || appID == k_uAppIdInvalid) return;

    try {
        std::scoped_lock lock(g_mutex);
        const auto now = std::chrono::steady_clock::now();

        // In-place overwrite priority: avoid triggering capacity eviction on retried calls
        const auto it = g_encryptedTickets.find(call);
        if (it != g_encryptedTickets.end()) {
            it->second = TicketEntry{appID, now};
            return;
        }

        // Capacity overflow pruning
        if (g_encryptedTickets.size() >= kMaxPendingCapacity) {
            std::erase_if(g_encryptedTickets, [now](const auto& pair) noexcept {
                return (now - pair.second.insertedAt) > kTicketExpiryDuration;
            });
            if (g_encryptedTickets.size() >= kMaxPendingCapacity) {
                auto oldestIt = std::min_element(
                    g_encryptedTickets.begin(), g_encryptedTickets.end(),
                    [](const auto& a, const auto& b) noexcept {
                        return a.second.insertedAt < b.second.insertedAt;
                    });
                if (oldestIt != g_encryptedTickets.end()) {
                    g_encryptedTickets.erase(oldestIt);
                }
            }
        }
        g_encryptedTickets.emplace(call, TicketEntry{appID, now});
    } catch (...) {
        // Complete exception safety boundary: protect host thread from escaping exceptions
    }
}

std::optional<AppId_t> TakeEncryptedTicket(SteamAPICall_t call) noexcept
{
    // Pre-lock fast path: zero contention on invalid handle
    if (call == k_uAPICallInvalid) return std::nullopt;

    try {
        std::scoped_lock lock(g_mutex);
        const auto it = g_encryptedTickets.find(call);
        if (it == g_encryptedTickets.end()) return std::nullopt;

        const AppId_t appID = it->second.appId;
        g_encryptedTickets.erase(it);
        return appID;
    } catch (...) {
        return std::nullopt;
    }
}

void Clear() noexcept
{
    try {
        std::scoped_lock lock(g_mutex);
        g_encryptedTickets.clear();
    } catch (...) {
    }
}

size_t GetPendingCount() noexcept
{
    try {
        std::scoped_lock lock(g_mutex);
        return g_encryptedTickets.size();
    } catch (...) {
        return 0;
    }
}

} // namespace PendingAPICalls

