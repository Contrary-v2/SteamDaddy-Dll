#pragma once
#include "Steam/Types.h"
#include <cstdint>
#include <string_view>

// ─────────────────────────────────────────────────────────────────
//  ManifestClient — HTTP client for depot manifest request codes.
//  Provider table is internal (see kProviders in ManifestClient.cpp);
//  adding a new provider only requires one row there.
//
//  From the configured preferred provider, tries each in order:
//  failed providers are put in cooldown and skipped; the preferred
//  index is never altered by success or cooldown.
//
//  Thread-safe — provider state is guarded by a short-held mutex that
//  never spans I/O; HTTP requests run concurrently.
// ─────────────────────────────────────────────────────────────────
namespace ManifestClient {

    // Per-fetch time budget shared across all fallback attempts (ms).
    constexpr uint32_t kFetchBudgetMs = 15000;

    // How long a failed provider is skipped before being retried (ms).
    constexpr uint32_t kProviderCooldownMs = 60000;

    // Select the active provider by its string name (matches kProviders[i].name).
    // Returns false if no provider matches; the previous selection is kept.
    bool SetProvider(std::string_view name);

    // Name of the currently active provider (for logging / diagnostics).
    const char* ActiveProviderName();

    // Enable or disable ordered failover to subsequent providers on failure.
    // When disabled only the preferred provider is attempted.
    // Corresponds to [manifest] failover in the config file.
    void SetFailoverEnabled(bool enabled);
    bool IsFailoverEnabled();

    // Resolve a manifest GID to its request code. Tries Lua first
    // (fetch_manifest_code_ex, then fetch_manifest_code), then the
    // configured provider (with failover to the rest of the table when
    // enabled). Returns true and sets *outRequestCode on success.
    bool FetchManifestRequestCode(uint64_t manifestGid, uint64_t* outRequestCode,
                                  AppId_t appId = 0, AppId_t depotId = 0);

    // Tear down any cached state (call at unload).
    void Shutdown();
}
