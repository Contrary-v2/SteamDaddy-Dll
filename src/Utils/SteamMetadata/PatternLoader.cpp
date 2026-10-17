#include "PatternLoader.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Support/FnvHash.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

// ---- compile-time sanity checks for FNV-1a table keys ----
static_assert(Fnv1aHash("BBuildAndAsyncSendFrame") == 0x82428E37u,
              "FNV-1a mismatch for BBuildAndAsyncSendFrame");
static_assert(Fnv1aHash("BuildDepotDependency") == 0xC37F2D8Eu,
              "FNV-1a mismatch for BuildDepotDependency");

namespace {

// ---- per-function pattern record ----
struct PatternEntry {
    std::string name;
    uintptr_t   rva = 0;   // 0 = not present in file
    std::string sig;        // empty = not present in file
};

// key = Fnv1aHash(funcName)
using PatternMap = std::unordered_map<uint32_t, PatternEntry>;

// Global state synchronization
static std::shared_mutex g_patternMutex;

// module → its pattern map
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, PatternMap> g_moduleMaps;

// Requests recorded at Load() for inline self-healing on missing function entries
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, RemoteToml::Request> g_moduleReqs;

// Track modules that already had one refresh attempt in this session
static std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle> g_refreshedModules;

// Modules whose Load() call failed (popup already shown). FindPattern silently returns nullptr
static std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle> g_failedModules;

// functions whose names were not found during FindPattern
static std::unordered_set<std::string> g_missingFunctions;

// ---- byte-pattern scanner ----
static bool ParseSig(const std::string& str,
                     std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& mask)
{
    bytes.clear();
    mask.clear();
    for (const char* p = str.c_str(); *p; ) {
        if (*p == ' ' || *p == '\t' || *p == ',') { ++p; continue; }
        if (p[0] == '?' && p[1] == '?') {
            bytes.push_back(0); mask.push_back(0); p += 2; continue;
        }
        char hi = p[0], lo = p[1];
        if (!hi || !lo) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return false;
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
        mask.push_back(1);
        p += 2;
    }
    return !bytes.empty();
}

static void* ScanModule(OSTPlatform::DynamicLibrary::ModuleHandle module,
                        const std::vector<uint8_t>& bytes,
                        const std::vector<uint8_t>& mask)
{
    const auto image = OSTPlatform::Memory::GetModuleImage(module);
    if (!image) return nullptr;

    auto* base = image->base;
    size_t size = image->size;
    size_t patLen = bytes.size();
    if (size < patLen) return nullptr;

    for (size_t i = 0; i <= size - patLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] && base[i + j] != bytes[j]) { found = false; break; }
        }
        if (found) return base + i;
    }
    return nullptr;
}

// ---- TOML pattern parser ----
static PatternMap TableToPatternMap(const toml::table& tbl)
{
    PatternMap map;
    map.reserve(tbl.size());
    for (auto& [rawKey, val] : tbl) {
        if (!val.is_table()) continue;
        auto& sub = *val.as_table();

        const auto parsedKey = OSTPlatform::Numbers::ParseHexUInt32(rawKey.str());
        if (!parsedKey) continue;
        const uint32_t hashKey = *parsedKey;

        PatternEntry entry;
        if (auto v = sub["name"].value<std::string>()) entry.name = *v;

        // Dual-mode compatibility for string hex or native integer RVA
        if (auto v = sub["rva"].value<std::string>()) {
            if (const auto rva = OSTPlatform::Numbers::ParseHexUInt64(*v)) {
                entry.rva = static_cast<uintptr_t>(*rva);
            }
        } else if (auto v = sub["rva"].value<int64_t>()) {
            if (*v > 0) {
                entry.rva = static_cast<uintptr_t>(*v);
            }
        }

        if (auto v = sub["sig"].value<std::string>()) entry.sig = *v;

        map[hashKey] = std::move(entry);
    }
    return map;
}

static PatternMap ParsePatternString(std::string_view body,
                                     std::string* outError = nullptr)
{
    try {
        return TableToPatternMap(toml::parse(body));
    } catch (const toml::parse_error& e) {
        if (outError) *outError = e.description();
        return {};
    }
}

// ---- popup helpers ----
static void ShowDownloadFailedPopup(const std::string& dllName,
                                    const std::string& sha256,
                                    const std::string& component)
{
    const std::string rootLabel = IsPortableMode() ? "<PortableDir>" : "<Steam>";
    SteamDiagnostics::ShowWarning(
        "SteamDaddy - Unsupported Steam Version",
        "SteamDaddy: signature file not found for " + dllName + ".\n\n"
        "Hooks that depend on " + dllName + " are disabled for this session; "
        "other modules are unaffected.\n\n"
        "You can:\n"
        "  1. Wait for upstream signature update, then restart Steam.\n"
        "  2. Drop a matching TOML at:\n"
        "       " + rootLabel + "\\steamdaddy\\pattern\\" + component + "\\" + sha256 + ".toml\n"
        "  3. Check upstream repository:\n"
        "       https://github.com/hazre/steam-monitor/tree/main/pattern/" + component + "\n"
        "  4. Report diagnostics at:\n"
        "       https://github.com/Contrary7/SteamDaddy-Backup/issues");
}

} // namespace

// ---- public API ----

namespace PatternLoader {

    constexpr const char* kPatternChannel = "pattern";

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component)
{
    if (!module) return false;

    namespace fs = std::filesystem;
    const RemoteToml::Request req{ kPatternChannel, component, dllPath };
    const std::string dllName = fs::path(dllPath).filename().string();

    auto tryParse = [&](const RemoteToml::Result& res, PatternMap& out, std::string& err) -> bool {
        if (!res.ok) return false;
        out = ParsePatternString(res.body, &err);
        return !out.empty();
    };

    PatternMap map;
    std::string parseErr;
    RemoteToml::Result r = RemoteToml::Fetch(req);
    if (tryParse(r, map, parseErr)) {
        LOG_INFO("PatternLoader: loaded {} patterns for {} ({})",
                 map.size(), component, r.fromCache ? "cache" : "remote");
        std::unique_lock lock(g_patternMutex);
        g_moduleMaps[module] = std::move(map);
        g_moduleReqs[module] = req;
        return true;
    }

    // Self-heal: stale or empty cache
    if (r.ok && r.fromCache) {
        LOG_WARN("PatternLoader: parsed empty spec for {} ({}), dropping stale cache and refetching once",
                 component, parseErr.empty() ? "no entries" : parseErr);
        RemoteToml::Invalidate(req);
        r = RemoteToml::Fetch(req);
        parseErr.clear();
        if (tryParse(r, map, parseErr)) {
            LOG_INFO("PatternLoader: self-heal ok, loaded {} patterns for {} ({})",
                     map.size(), component, r.fromCache ? "cache" : "remote");
            std::unique_lock lock(g_patternMutex);
            g_moduleMaps[module] = std::move(map);
            g_moduleReqs[module] = req;
            return true;
        }
    } else if (!r.ok) {
        LOG_WARN("PatternLoader: download failed for {} (see RemoteToml log above)", component);
    } else {
        LOG_WARN("PatternLoader: TOML for {} parsed empty ({})",
                 component, parseErr.empty() ? "no entries" : parseErr);
    }

    std::string sha = r.sha256.empty() ? "(hash failed)" : r.sha256;
    ShowDownloadFailedPopup(dllName, sha, component);
    std::unique_lock lock(g_patternMutex);
    g_failedModules.insert(module);
    return false;
}

void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    {
        std::shared_lock lock(g_patternMutex);
        if (g_failedModules.count(module)) {
            return nullptr;
        }
    }

    uint32_t key = Fnv1aHash(funcName);

    {
        std::shared_lock lock(g_patternMutex);
        auto mapIt = g_moduleMaps.find(module);
        if (mapIt == g_moduleMaps.end()) {
            LOG_WARN("PatternLoader: FindPattern called for module that was never loaded ('{}')", funcName);
            lock.unlock();
            std::unique_lock wlock(g_patternMutex);
            g_missingFunctions.insert(funcName);
            return nullptr;
        }

        const auto& map = mapIt->second;
        auto entryIt = map.find(key);
        if (entryIt != map.end()) {
            const PatternEntry& entry = entryIt->second;
            if (entry.rva != 0) {
                void* addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(module) + entry.rva);
                LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:X}", funcName, entry.rva);
                return addr;
            }
            if (!entry.sig.empty()) {
                std::vector<uint8_t> bytes, mask;
                if (ParseSig(entry.sig, bytes, mask)) {
                    void* addr = ScanModule(module, bytes, mask);
                    if (addr) {
                        uintptr_t rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(module);
                        LOG_DEBUG("PatternLoader: {} resolved via sig @ RVA 0x{:X}", funcName, rva);
                        return addr;
                    }
                    LOG_WARN("PatternLoader: sig scan miss for '{}'", funcName);
                }
            }
        }
    }

    // Subset refresh: if function was absent from cache, invalidate and refresh once
    bool canRefresh = false;
    RemoteToml::Request reqToRefresh;
    {
        std::unique_lock lock(g_patternMutex);
        if (!g_refreshedModules.count(module)) {
            g_refreshedModules.insert(module);
            auto reqIt = g_moduleReqs.find(module);
            if (reqIt != g_moduleReqs.end()) {
                reqToRefresh = reqIt->second;
                canRefresh = true;
            }
        }
    }

    if (canRefresh) {
        LOG_WARN("PatternLoader: '{}' not in cached spec, dropping possibly-stale subset and refetching once", funcName);
        RemoteToml::Invalidate(reqToRefresh);
        RemoteToml::Result r = RemoteToml::Fetch(reqToRefresh);
        if (r.ok) {
            std::string refreshErr;
            PatternMap fresh = ParsePatternString(r.body, &refreshErr);
            if (!fresh.empty()) {
                std::unique_lock lock(g_patternMutex);
                auto& map = g_moduleMaps[module];
                LOG_INFO("PatternLoader: subset refresh ok, {} -> {} entries ({})",
                         map.size(), fresh.size(), r.fromCache ? "cache" : "remote");
                map = std::move(fresh);
                auto entryIt = map.find(key);
                if (entryIt != map.end()) {
                    const PatternEntry& entry = entryIt->second;
                    if (entry.rva != 0) {
                        void* addr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(module) + entry.rva);
                        LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:X} after refresh", funcName, entry.rva);
                        return addr;
                    }
                    if (!entry.sig.empty()) {
                        std::vector<uint8_t> bytes, mask;
                        if (ParseSig(entry.sig, bytes, mask)) {
                            void* addr = ScanModule(module, bytes, mask);
                            if (addr) {
                                LOG_DEBUG("PatternLoader: {} resolved via sig after refresh", funcName);
                                return addr;
                            }
                        }
                    }
                }
            }
        }
    }

    LOG_WARN("PatternLoader: no entry for '{}' (key=0x{:08X})", funcName, key);
    std::unique_lock lock(g_patternMutex);
    g_missingFunctions.insert(funcName);
    return nullptr;
}

void RefreshMissingMetadata()
{
    // Compatible no-op: subset refresh runs inline during FindPattern
}

void ReportMissingFunctions()
{
    std::vector<std::string> missing;
    {
        std::unique_lock lock(g_patternMutex);
        if (g_missingFunctions.empty()) return;
        missing.assign(g_missingFunctions.begin(), g_missingFunctions.end());
        g_missingFunctions.clear();
    }
    std::sort(missing.begin(), missing.end());

    std::string list;
    for (const auto& name : missing) {
        list += "  - " + name + "\n";
    }

    SteamDiagnostics::ShowWarning(
        "SteamDaddy - Missing Signatures",
        "SteamDaddy: some functions could not be located in this Steam build.\n\n"
        "Missing functions:\n" +
        list +
        "\nHooks for these functions are disabled for this session.\n"
        "Please report to: https://github.com/Contrary7/SteamDaddy-Backup/issues");
}

} // namespace PatternLoader
