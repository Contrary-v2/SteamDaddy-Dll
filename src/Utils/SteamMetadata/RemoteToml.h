#pragma once
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;
        // True only when all attempted mirrors returned 404 (upstream never published)
        bool        allNotFound = false;
    };

    // Prefer local cache during startup, fallback to remote mirrors
    [[nodiscard]] Result Fetch(const Request& request);

    // Delete the cache file for this request (used on corrupt/stale subset cache)
    bool Invalidate(const Request& request);

} // namespace RemoteToml
