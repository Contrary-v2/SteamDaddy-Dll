#include "Pipe/Features/DenuvoAuth/DenuvoSync.h"
#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"
#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Tickets/AppTicket.h"
#include "OSTPlatform/include/Encoding.h"
#include "Steam/Structs.h"
#include "Steam/Enums.h"
#include "Utils/Config/Config.h"
#include "Hook/Hooks_Package.h"
#include "dllmain.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace PipeManager::DenuvoAuth {

    bool HasCmdLineArg(std::string_view cmdLine, std::string_view arg) noexcept {
        if (cmdLine.empty() || arg.empty()) return false;
        const size_t argLen = arg.size();
        if (cmdLine.size() < argLen) return false;

        for (size_t i = 0; i <= cmdLine.size() - argLen; ++i) {
            // Fast token boundary check: token must begin at string start or after whitespace/quotes
            if (i == 0 || std::isspace(static_cast<unsigned char>(cmdLine[i - 1])) || cmdLine[i - 1] == '"' || cmdLine[i - 1] == '\'') {
                if (_strnicmp(cmdLine.data() + i, arg.data(), argLen) == 0) {
                    if (i + argLen == cmdLine.size()) {
                        return true;
                    }
                    const char next = cmdLine[i + argLen];
                    if (std::isspace(static_cast<unsigned char>(next)) || next == '"' || next == '\'' || next == '=' || next == ':') {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool HasCmdLineArg(const char* cmdLine, const char* arg) {
        if (!cmdLine || !arg) return false;
        return HasCmdLineArg(std::string_view(cmdLine), std::string_view(arg));
    }

    bool HasNoDenuvoArg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-nodenuvo") ||
               HasCmdLineArg(sv, "-no-denuvo") ||
               HasCmdLineArg(sv, "-no_denuvo") ||
               HasCmdLineArg(sv, "/nodenuvo") ||
               HasCmdLineArg(sv, "/no-denuvo") ||
               HasCmdLineArg(sv, "/no_denuvo");
    }

    bool HasForcedDenuvoArg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-forcedenuvo") ||
               HasCmdLineArg(sv, "-force-denuvo") ||
               HasCmdLineArg(sv, "-force_denuvo") ||
               HasCmdLineArg(sv, "/forcedenuvo") ||
               HasCmdLineArg(sv, "/force-denuvo") ||
               HasCmdLineArg(sv, "/force_denuvo");
    }

    bool HasDAuth2Arg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-dauth2") ||
               HasCmdLineArg(sv, "--dauth2") ||
               HasCmdLineArg(sv, "-dauth-2") ||
               HasCmdLineArg(sv, "-dauth_2") ||
               HasCmdLineArg(sv, "/dauth2") ||
               HasCmdLineArg(sv, "/dauth-2") ||
               HasCmdLineArg(sv, "/dauth_2");
    }

    bool HasLuaArg(const char* cmdLine) {
        if (!cmdLine) return false;
        const std::string_view sv(cmdLine);
        return HasCmdLineArg(sv, "-lua") ||
               HasCmdLineArg(sv, "--lua") ||
               HasCmdLineArg(sv, "-d+") ||
               HasCmdLineArg(sv, "/lua") ||
               HasCmdLineArg(sv, "/d+");
    }

namespace {

    std::mutex g_dPlusMutex;
    std::unordered_set<AppId_t> g_dPlusLaunches;
    std::mutex g_syncFileMutex;

    std::string_view TrimWhitespace(std::string_view str) noexcept {
        while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
            str.remove_prefix(1);
        }
        while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
            str.remove_suffix(1);
        }
        return str;
    }

    std::vector<std::string> TokenizeQuoted(std::string_view line) {
        std::vector<std::string> tokens;
        tokens.reserve(4);

        size_t pos = 0;
        while (pos < line.size()) {
            pos = line.find('"', pos);
            if (pos == std::string_view::npos) break;

            std::string token;
            token.reserve(32);
            size_t i = pos + 1;
            bool closed = false;

            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    if (line[i + 1] == '"') {
                        token.push_back('"');
                        i += 2;
                        continue;
                    }
                    if (line[i + 1] == '\\') {
                        token.push_back('\\');
                        i += 2;
                        continue;
                    }
                }
                if (line[i] == '"') {
                    closed = true;
                    pos = i + 1;
                    break;
                }
                token.push_back(line[i]);
                ++i;
            }

            if (!closed) break;
            tokens.push_back(std::move(token));
        }

        return tokens;
    }

    std::filesystem::path FindAcfPath(AppId_t appId, const std::string& exePath) {
        std::error_code ec;

        // Clean exePath from enclosing quotes and whitespace
        std::string_view cleanExe = exePath;
        cleanExe = TrimWhitespace(cleanExe);
        while (!cleanExe.empty() && (cleanExe.front() == '"' || cleanExe.front() == '\'')) cleanExe.remove_prefix(1);
        while (!cleanExe.empty() && (cleanExe.back() == '"' || cleanExe.back() == '\'')) cleanExe.remove_suffix(1);
        cleanExe = TrimWhitespace(cleanExe);

        // Strategy 1: Walk up from exePath directly to find steamapps folder
        if (!cleanExe.empty()) {
            for (auto p = OSTPlatform::Encoding::PathFromUtf8(cleanExe).parent_path();
                 !p.empty() && p != p.parent_path();
                 p = p.parent_path()) {
                const auto& fn = p.filename().native();
                if (_wcsicmp(fn.c_str(), L"steamapps") == 0) {
                    auto candidate = p / ("appmanifest_" + std::to_string(appId) + ".acf");
                    if (std::filesystem::exists(candidate, ec) && !ec) {
                        LOG_INFO("DenuvoSync: located ACF from exePath: {}", OSTPlatform::Encoding::PathToUtf8(candidate));
                        return candidate;
                    }
                }
            }
        }

        // Strategy 2: Check primary Steam install path
        std::string steamPath = SteamInstallPath;
        if (!steamPath.empty()) {
            auto primaryAcf = OSTPlatform::Encoding::PathFromUtf8(steamPath) / "steamapps" / ("appmanifest_" + std::to_string(appId) + ".acf");
            if (std::filesystem::exists(primaryAcf, ec) && !ec) {
                LOG_INFO("DenuvoSync: located ACF from primary Steam path: {}", OSTPlatform::Encoding::PathToUtf8(primaryAcf));
                return primaryAcf;
            }

            // Strategy 3: Parse libraryfolders.vdf
            auto libVdf = OSTPlatform::Encoding::PathFromUtf8(steamPath) / "steamapps" / "libraryfolders.vdf";
            std::ifstream file(libVdf);
            if (file.is_open()) {
                std::string line;
                while (std::getline(file, line)) {
                    std::string_view trimmed = TrimWhitespace(line);
                    if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) continue;

                    auto tokens = TokenizeQuoted(trimmed);
                    if (tokens.size() >= 2) {
                        std::string key = tokens[0];
                        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (key == "path") {
                            auto libAcf = OSTPlatform::Encoding::PathFromUtf8(tokens[1]) / "steamapps" / ("appmanifest_" + std::to_string(appId) + ".acf");
                            if (std::filesystem::exists(libAcf, ec) && !ec) {
                                LOG_INFO("DenuvoSync: located ACF from libraryfolders.vdf: {}", OSTPlatform::Encoding::PathToUtf8(libAcf));
                                return libAcf;
                            }
                        }
                    }
                }
            }
        }

        LOG_WARN("DenuvoSync: failed to find appmanifest_{}.acf across all library paths", appId);
        return {};
    }

    struct AcfData {
        std::map<uint32_t, std::string> installedDepots; // depotId -> manifestGid
        std::map<uint32_t, uint64_t> depotSizes;
    };

    AcfData ParseAcf(const std::filesystem::path& acfPath) {
        AcfData data;
        std::ifstream file(acfPath);
        if (!file.is_open()) return data;

        std::string line;
        std::vector<std::string> stack;
        std::string pendingSection;

        while (std::getline(file, line)) {
            std::string_view trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) continue;

            auto tokens = TokenizeQuoted(trimmed);

            if (tokens.size() == 1) {
                std::string s = tokens[0];
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                pendingSection = std::move(s);
            }

            for (char ch : trimmed) {
                if (ch == '{') {
                    if (!pendingSection.empty()) {
                        stack.push_back(std::move(pendingSection));
                        pendingSection.clear();
                    } else {
                        stack.push_back("");
                    }
                }
            }

            if (tokens.size() >= 2) {
                std::string key = tokens[0];
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const std::string& val = tokens[1];

                // Case A: stack = [..., "installeddepots", "<depotId>"]
                if (stack.size() >= 2 && stack[stack.size() - 2] == "installeddepots") {
                    const std::string& depotSec = stack.back();
                    uint32_t dId = 0;
                    auto [p, ec] = std::from_chars(depotSec.data(), depotSec.data() + depotSec.size(), dId);
                    if (ec == std::errc{} && dId != 0) {
                        if (key == "manifest") {
                            data.installedDepots[dId] = val;
                        } else if (key == "size") {
                            uint64_t sz = 0;
                            std::from_chars(val.data(), val.data() + val.size(), sz);
                            data.depotSizes[dId] = sz;
                        }
                    }
                }
                // Case B: stack = [..., "mounteddepots"] -> "<depotId>" "<manifestGid>"
                else if (!stack.empty() && stack.back() == "mounteddepots") {
                    uint32_t dId = 0;
                    auto [p, ec] = std::from_chars(tokens[0].data(), tokens[0].data() + tokens[0].size(), dId);
                    if (ec == std::errc{} && dId != 0 && !val.empty()) {
                        data.installedDepots.try_emplace(dId, val);
                    }
                }
            }

            for (char ch : trimmed) {
                if (ch == '}') {
                    if (!stack.empty()) {
                        stack.pop_back();
                    }
                }
            }
        }

        return data;
    }

    std::map<uint32_t, std::string> ParseConfigVdfKeys(const std::string& steamPath) {
        std::map<uint32_t, std::string> keys;
        if (steamPath.empty()) return keys;

        auto configVdf = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(steamPath)) / "config" / "config.vdf";
        std::ifstream file(configVdf);
        if (!file.is_open()) return keys;

        std::string line;
        std::vector<std::string> stack;
        std::string pendingSection;

        while (std::getline(file, line)) {
            std::string_view trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.starts_with("//") || trimmed.starts_with("#")) continue;

            auto tokens = TokenizeQuoted(trimmed);

            if (tokens.size() == 1) {
                std::string s = tokens[0];
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                pendingSection = std::move(s);
            }

            for (char ch : trimmed) {
                if (ch == '{') {
                    if (!pendingSection.empty()) {
                        stack.push_back(std::move(pendingSection));
                        pendingSection.clear();
                    } else {
                        stack.push_back("");
                    }
                }
            }

            if (tokens.size() >= 2) {
                std::string key = tokens[0];
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (key == "decryptionkey" && tokens[1].size() == 64 && stack.size() >= 2) {
                    if (stack[stack.size() - 2] == "depots") {
                        const std::string& depotSec = stack.back();
                        uint32_t dId = 0;
                        auto [p, ec] = std::from_chars(depotSec.data(), depotSec.data() + depotSec.size(), dId);
                        if (ec == std::errc{} && dId != 0) {
                            keys[dId] = tokens[1];
                        }
                    }
                }
            }

            for (char ch : trimmed) {
                if (ch == '}') {
                    if (!stack.empty()) {
                        stack.pop_back();
                    }
                }
            }
        }

        return keys;
    }

    bool AtomicWriteLines(const std::filesystem::path& targetPath, const std::vector<std::string>& lines) {
        std::error_code ec;
        auto parent = targetPath.parent_path();
        std::filesystem::create_directories(parent, ec);

        static std::atomic<uint64_t> s_seq{0};
        const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
        const auto tid = GetCurrentThreadId();
        const std::filesystem::path tempPath = targetPath.wstring() + L".tmp." + std::to_wstring(tid) + L"." + std::to_wstring(seq);

        {
            std::ofstream out(tempPath, std::ios::trunc);
            if (!out.is_open()) {
                LOG_ERROR("DenuvoSync: failed to open temp file for writing: {}", OSTPlatform::Encoding::PathToUtf8(tempPath));
                return false;
            }
            for (const auto& line : lines) {
                out << line << "\n";
            }
            out.flush();
            if (!out.good()) {
                out.close();
                std::filesystem::remove(tempPath, ec);
                return false;
            }
        }

        bool moved = false;
        DWORD lastError = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            SetFileAttributesW(targetPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                moved = true;
                break;
            }
            lastError = GetLastError();
            Sleep(10);
        }

        if (!moved) {
            LOG_ERROR("DenuvoSync: MoveFileExW failed to replace {} (error={})",
                      OSTPlatform::Encoding::PathToUtf8(targetPath), lastError);
            std::filesystem::remove(tempPath, ec);
            return false;
        }

        return true;
    }

    void CopyManifestMirrors(AppId_t appId, const std::filesystem::path& targetDir, const AcfData& acfData, const std::filesystem::path& acfPath) {
        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);

        std::vector<std::filesystem::path> searchDirs;
        auto addSearchDir = [&](const std::filesystem::path& dir) {
            if (dir.empty()) return;
            for (const auto& existing : searchDirs) {
                if (std::filesystem::equivalent(dir, existing, ec) || (!ec && dir == existing)) return;
                ec.clear();
            }
            if (std::filesystem::is_directory(dir, ec) && !ec) {
                searchDirs.push_back(dir);
            }
        };

        if (!acfPath.empty()) {
            addSearchDir(acfPath.parent_path() / "depotcache");
            if (acfPath.parent_path() != acfPath.parent_path().parent_path()) {
                addSearchDir(acfPath.parent_path().parent_path() / "depotcache");
            }
        }

        if (SteamInstallPath[0] != '\0') {
            addSearchDir(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / "depotcache");
        }

        for (const auto& [depotId, gid] : acfData.installedDepots) {
            if (gid.empty() || gid == "0") continue;
            std::string manifestFileName = std::to_string(depotId) + "_" + gid + ".manifest";

            for (const auto& sDir : searchDirs) {
                auto srcFile = sDir / manifestFileName;
                if (std::filesystem::exists(srcFile, ec) && !ec) {
                    auto destFile = targetDir / manifestFileName;
                    if (!std::filesystem::exists(destFile, ec)) {
                        if (CopyFileW(srcFile.c_str(), destFile.c_str(), FALSE)) {
                            LOG_INFO("DenuvoSync: mirrored manifest {} -> {}",
                                     OSTPlatform::Encoding::PathToUtf8(srcFile),
                                     OSTPlatform::Encoding::PathToUtf8(destFile));
                        } else {
                            LOG_WARN("DenuvoSync: failed to mirror manifest {} -> {} (error={})",
                                     OSTPlatform::Encoding::PathToUtf8(srcFile),
                                     OSTPlatform::Encoding::PathToUtf8(destFile), GetLastError());
                        }
                    }
                    break;
                }
            }
        }
    }

    void CleanObsoleteManifestMirrors(const std::filesystem::path& targetDir, const std::map<uint32_t, std::string>& installedDepots) {
        std::error_code ec;
        if (!std::filesystem::exists(targetDir, ec) || !std::filesystem::is_directory(targetDir, ec)) {
            return;
        }

        // Safety Defense 1: Reject any path that contains "depotcache"
        std::string dirLower = OSTPlatform::Encoding::PathToUtf8(targetDir);
        std::transform(dirLower.begin(), dirLower.end(), dirLower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        if (dirLower.find("depotcache") != std::string::npos) {
            LOG_ERROR("DenuvoSync: CleanObsoleteManifestMirrors REFUSING directory containing depotcache: {}", dirLower);
            return;
        }

        // Safety Defense 2: targetDir must not be the root lua directory itself
        std::filesystem::path luaBaseDir;
        if (LuaDir[0] != '\0') {
            luaBaseDir = OSTPlatform::Encoding::PathFromUtf8(LuaDir);
        } else if (SteamInstallPath[0] != '\0') {
            luaBaseDir = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath)) / "config" / "lua";
        }
        if (!luaBaseDir.empty() && std::filesystem::equivalent(targetDir, luaBaseDir, ec)) {
            LOG_ERROR("DenuvoSync: CleanObsoleteManifestMirrors REFUSING root lua directory: {}", dirLower);
            return;
        }
        ec.clear();

        // Safety Defense 3: Only delete manifests within targetDir whose depotId matches an installed depot
        // but whose GID does NOT match the latest GID from ACF.
        try {
            for (const auto& entry : std::filesystem::directory_iterator(targetDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;

                auto fn = OSTPlatform::Encoding::PathToUtf8(entry.path().filename());
                // Expected format: <depotId>_<gid>.manifest
                if (!fn.ends_with(".manifest")) continue;

                size_t underscore = fn.find('_');
                size_t dot = fn.rfind(".manifest");
                if (underscore == std::string::npos || dot == std::string::npos || dot <= underscore) continue;

                std::string_view depotStr = std::string_view(fn).substr(0, underscore);
                std::string_view gidStr = std::string_view(fn).substr(underscore + 1, dot - underscore - 1);

                uint32_t depotId = 0;
                auto [p, parseEc] = std::from_chars(depotStr.data(), depotStr.data() + depotStr.size(), depotId);
                if (parseEc != std::errc{} || depotId == 0) continue;

                auto it = installedDepots.find(depotId);
                if (it != installedDepots.end()) {
                    // If depot is installed, and the file's GID does NOT match the latest active GID
                    if (!it->second.empty() && it->second != "0" && gidStr != it->second) {
                        const std::string entryPathUtf8 = OSTPlatform::Encoding::PathToUtf8(entry.path());
                        LOG_INFO("DenuvoSync: removing obsolete manifest mirror in lua dir: {}", entryPathUtf8);
                        std::error_code removeEc;
                        if (!std::filesystem::remove(entry.path(), removeEc)) {
                            LOG_WARN("DenuvoSync: failed to remove obsolete manifest '{}' ({})",
                                     entryPathUtf8, removeEc.message());
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_WARN("DenuvoSync: exception during CleanObsoleteManifestMirrors: {}", ex.what());
        }
    }

    std::string ToHex(const uint8_t* data, size_t size) {
        std::string hex;
        hex.reserve(size * 2);
        static constexpr char kDigits[] = "0123456789abcdef";
        for (size_t i = 0; i < size; ++i) {
            hex.push_back(kDigits[(data[i] >> 4) & 0x0F]);
            hex.push_back(kDigits[data[i] & 0x0F]);
        }
        return hex;
    }

    std::filesystem::path ResolveAppLuaPath(AppId_t appId) {
        // 1. Check if LuaConfig already tracked the source Lua file for this app
        std::string tracked = LuaConfig::FindLuaFileForAppId(appId);
        if (!tracked.empty()) {
            std::error_code ec;
            auto p = OSTPlatform::Encoding::PathFromUtf8(tracked);
            if (std::filesystem::exists(p, ec) && !ec) {
                return p;
            }
        }

        // 2. Search configured Lua directory: <LuaDir>/<appId>/<appId>.lua and <LuaDir>/<appId>.lua
        std::filesystem::path luaBaseDir;
        if (LuaDir[0] != '\0') {
            luaBaseDir = OSTPlatform::Encoding::PathFromUtf8(LuaDir);
        } else if (SteamInstallPath[0] != '\0') {
            luaBaseDir = std::filesystem::path(OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath)) / "config" / "lua";
        }

        if (!luaBaseDir.empty()) {
            auto p1 = luaBaseDir / std::to_string(appId) / (std::to_string(appId) + ".lua");
            std::error_code ec;
            if (std::filesystem::exists(p1, ec) && !ec) return p1;

            auto p2 = luaBaseDir / (std::to_string(appId) + ".lua");
            if (std::filesystem::exists(p2, ec) && !ec) return p2;
        }

        // 3. Fallback to <storageBase>/config/st/<appId>.lua
        const auto storageBase = OSTPlatform::Encoding::PathFromUtf8(GetStorageDirectory());
        auto stPath = storageBase / "config" / "st" / (std::to_string(appId) + ".lua");
        std::error_code ec;
        if (std::filesystem::exists(stPath, ec) && !ec) return stPath;

        // Default target for new creation if baseDir is known: <LuaDir>/<appId>/<appId>.lua
        if (!luaBaseDir.empty()) {
            return luaBaseDir / std::to_string(appId) / (std::to_string(appId) + ".lua");
        }
        return {};
    }

} // namespace

void OnSpawnProcess(AppId_t appId, const char* pExePath, const char* cmdLine) {
    if (appId == 0 || appId == k_uAppIdInvalid) return;

    const bool hasLuaArg = HasLuaArg(cmdLine);
    const bool hasNoDenuvo = HasNoDenuvoArg(cmdLine);
    const bool hasForcedDenuvo = HasForcedDenuvoArg(cmdLine);
    const bool hasDAuth2 = HasDAuth2Arg(cmdLine);

    LuaConfig::SetCmdLineNoDenuvo(appId, hasNoDenuvo);
    LuaConfig::SetCmdLineForcedDenuvo(appId, hasForcedDenuvo);
    LuaConfig::SetCmdLineDAuth2(appId, hasDAuth2);

    if (hasNoDenuvo) {
        LOG_INFO("DenuvoSync: -nodenuvo active for appId={} — skipping Denuvo sync and manifest locking", appId);
        ClearDPlusLaunch(appId);
        return;
    }

    if (hasForcedDenuvo) {
        LOG_INFO("DenuvoSync: -forcedenuvo active for appId={}", appId);
    }

    if (hasDAuth2) {
        LOG_INFO("DenuvoSync: -dauth2 active for appId={} — Scheme 2 (adaptive ticket lease) enabled", appId);
    }

    std::filesystem::path existingLuaPath = ResolveAppLuaPath(appId);
    std::error_code ec;
    const bool hasLua = LuaConfig::HasDepot(appId, false) ||
                        (!existingLuaPath.empty() && std::filesystem::exists(existingLuaPath, ec) && !ec);

    // 🛑 ABSOLUTE ZERO-OPERATION INVARIANT:
    // If there is no Lua file configured and neither -lua/-d+ nor -forcedenuvo is passed,
    // OST executes ABSOLUTELY ZERO operations. Pure vanilla pass-through!
    if (!hasLua && !hasLuaArg && !hasForcedDenuvo) {
        ClearDPlusLaunch(appId);
        return;
    }

    // 🛑 LICENSE ENTITLEMENT RESTRICTION:
    // -lua can ONLY extract/generate on accounts that have a valid license (genuinely owned or family shared).
    // If the account does not have a valid license, strictly refuse generation or updating.
    const bool hasValidLicense = Hooks_Package::HasValidLicense(appId);
    if (!hasValidLicense) {
        if (hasLuaArg) {
            LOG_WARN("DenuvoSync: -lua specified for appId={}, but app does not have a valid license on current account — skipping generation", appId);
        }
        ClearDPlusLaunch(appId);
        return;
    }

    if (hasLuaArg) {
        std::lock_guard lock(g_dPlusMutex);
        g_dPlusLaunches.insert(appId);
        LOG_INFO("DenuvoSync: recorded -lua launch option for appId={}", appId);
    } else {
        ClearDPlusLaunch(appId);
    }

    // Valid license owner (genuine or family shared) launching with existing Lua OR -lua / -forcedenuvo:
    std::string_view exeSv = pExePath ? pExePath : "";
    exeSv = TrimWhitespace(exeSv);
    while (!exeSv.empty() && (exeSv.front() == '"' || exeSv.front() == '\'')) {
        exeSv.remove_prefix(1);
    }
    while (!exeSv.empty() && (exeSv.back() == '"' || exeSv.back() == '\'')) {
        exeSv.remove_suffix(1);
    }
    exeSv = TrimWhitespace(exeSv);
    try {
        SyncOrGenerate(appId, std::string(exeSv), hasLuaArg);
    } catch (const std::exception& ex) {
        LOG_ERROR("DenuvoSync: exception in SyncOrGenerate for appId={}: {}", appId, ex.what());
    } catch (...) {
        LOG_ERROR("DenuvoSync: unknown exception in SyncOrGenerate for appId={}", appId);
    }
}

bool IsDPlusLaunch(AppId_t appId) {
    if (appId == 0 || appId == k_uAppIdInvalid) return false;
    std::lock_guard lock(g_dPlusMutex);
    return g_dPlusLaunches.contains(appId);
}

void ClearDPlusLaunch(AppId_t appId) {
    if (appId == 0 || appId == k_uAppIdInvalid) return;
    std::lock_guard lock(g_dPlusMutex);
    g_dPlusLaunches.erase(appId);
}

bool SyncOrGenerate(AppId_t appId, const std::string& exePath, bool allowGenerate) {
    if (appId == 0 || appId == k_uAppIdInvalid) return false;

    // Gatekeeper: if nodenuvo is configured, completely skip
    if (LuaConfig::IsNoDenuvo(appId)) {
        LOG_INFO("DenuvoSync: nodenuvo set for appId={} — skipping Denuvo sync", appId);
        return false;
    }

    // Determine Lua paths
    std::filesystem::path luaBaseDir;
    if (LuaDir[0] != '\0') {
        luaBaseDir = OSTPlatform::Encoding::PathFromUtf8(LuaDir);
    } else if (SteamInstallPath[0] != '\0') {
        luaBaseDir = OSTPlatform::Encoding::PathFromUtf8(SteamInstallPath) / "config" / "lua";
    }
    if (luaBaseDir.empty()) {
        LOG_WARN("DenuvoSync: Lua directory undetermined");
        return false;
    }

    const auto appLuaDir = luaBaseDir / std::to_string(appId);
    std::filesystem::path luaFilePath = ResolveAppLuaPath(appId);
    std::error_code ec;
    const bool luaFileExists = !luaFilePath.empty() && std::filesystem::exists(luaFilePath, ec) && !ec;

    if (!luaFileExists) {
        if (!allowGenerate) {
            return false;
        }
        luaFilePath = appLuaDir / (std::to_string(appId) + ".lua");
    }

    // Locate ACF file
    auto acfPath = FindAcfPath(appId, exePath);
    if (acfPath.empty()) {
        LOG_WARN("DenuvoSync: cannot find ACF for appId={}", appId);
        return false;
    }

    AcfData acfData = ParseAcf(acfPath);
    LOG_INFO("DenuvoSync: parsed ACF depots: count={}", acfData.installedDepots.size());
    if (acfData.installedDepots.empty()) {
        LOG_WARN("DenuvoSync: no installed depots found in {}", OSTPlatform::Encoding::PathToUtf8(acfPath));
        return false;
    }

    auto depotKeys = ParseConfigVdfKeys(SteamInstallPath);
    LOG_INFO("DenuvoSync: parsed config.vdf keys: count={}", depotKeys.size());

    bool ticketAlreadyWritten = false;
    {
        std::lock_guard fileLock(g_syncFileMutex);
        const bool currentFileExists = std::filesystem::exists(luaFilePath, ec) && !ec;
        if (!currentFileExists) {
            // ── Case 1: First-time generation (-lua / -d+) ────────────────────────
            LOG_INFO("DenuvoSync: generating new <AppId>.lua for appId={}", appId);
            std::vector<std::string> lines;
            lines.reserve(32);
            lines.push_back("-- Auto-generated by OpenSteamTool (-d+) for AppID: " + std::to_string(appId));
            lines.push_back("");
            lines.push_back("-- Base Game");
            lines.push_back("addappid(" + std::to_string(appId) + ")");

            for (const auto& [depotId, gid] : acfData.installedDepots) {
                if (depotId != appId) {
                    auto itKey = depotKeys.find(depotId);
                    if (itKey != depotKeys.end() && !itKey->second.empty()) {
                        lines.push_back("addappid(" + std::to_string(depotId) + ", 1, \"" + itKey->second + "\")");
                    } else {
                        lines.push_back("addappid(" + std::to_string(depotId) + ")");
                    }
                }
            }

            lines.push_back("");
            lines.push_back("-- Locked Manifests (Prevent Auto-Update)");
            for (const auto& [depotId, gid] : acfData.installedDepots) {
                if (!gid.empty() && gid != "0") {
                    lines.push_back("setManifestid(" + std::to_string(depotId) + ", \"" + gid + "\")");
                }
            }

            std::vector<uint8_t> cachedTicket = AppTicket::GetSteamConfigStoreTicket(appId);
            if (cachedTicket.empty()) {
                cachedTicket = AppTicket::GetCachedAppOwnershipTicket(appId);
            }
            if (!cachedTicket.empty()) {
                lines.push_back("");
                lines.push_back("-- App Ownership Ticket (AppTicket)");
                lines.push_back("setAppTicket(" + std::to_string(appId) + ", \"" + ToHex(cachedTicket.data(), cachedTicket.size()) + "\")");
                ticketAlreadyWritten = true;
            }

            if (!AtomicWriteLines(luaFilePath, lines)) {
                LOG_ERROR("DenuvoSync: failed to write {}", OSTPlatform::Encoding::PathToUtf8(luaFilePath));
                return false;
            }
            LuaConfig::ParseFile(OSTPlatform::Encoding::PathToUtf8(luaFilePath));
        } else {
            // ── Case 2: In-place update / re-locking for existing Lua ────────────
            LOG_INFO("DenuvoSync: checking and updating existing <AppId>.lua for appId={}", appId);
            std::ifstream in(luaFilePath);
            if (!in.is_open()) {
                LOG_ERROR("DenuvoSync: failed to read {}", OSTPlatform::Encoding::PathToUtf8(luaFilePath));
                return false;
            }

            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line)) {
                lines.push_back(line);
            }
            in.close();

            std::unordered_set<uint32_t> handledDepots;
            int lastManifestLineIdx = -1;
            bool manifestModified = false;

            for (size_t i = 0; i < lines.size(); ++i) {
                std::string_view sv = TrimWhitespace(lines[i]);
                if (sv.empty()) continue;

                std::string lower(sv);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                // Check for setManifestid line
                size_t mPos = lower.find("setmanifestid");
                if (mPos != std::string::npos) {
                    lastManifestLineIdx = static_cast<int>(i);
                    // Extract depotId: setManifestid( <depotId> ,
                    size_t openParen = lower.find('(', mPos);
                    size_t comma = lower.find(',', openParen);
                    if (openParen != std::string::npos && comma != std::string::npos && comma > openParen) {
                        std::string_view dStr = TrimWhitespace(std::string_view(lower).substr(openParen + 1, comma - openParen - 1));
                        uint32_t dId = 0;
                        auto [p, ec2] = std::from_chars(dStr.data(), dStr.data() + dStr.size(), dId);
                        if (ec2 == std::errc{} && dId != 0) {
                            auto itDepot = acfData.installedDepots.find(dId);
                            if (itDepot != acfData.installedDepots.end() && !itDepot->second.empty() && itDepot->second != "0") {
                                std::string newLine = "setManifestid(" + std::to_string(dId) + ", \"" + itDepot->second + "\")";
                                if (lines[i] != newLine) {
                                    lines[i] = std::move(newLine);
                                    manifestModified = true;
                                }
                                handledDepots.insert(dId);
                            }
                        }
                    }
                }
            }

            // Enforce full coverage: insert any missing installed depot manifests
            std::vector<std::string> missingManifestLines;
            for (const auto& [depotId, gid] : acfData.installedDepots) {
                if (!gid.empty() && gid != "0" && !handledDepots.contains(depotId)) {
                    missingManifestLines.push_back("setManifestid(" + std::to_string(depotId) + ", \"" + gid + "\")");
                    LOG_INFO("DenuvoSync: re-inserting missing manifest depot={} gid={}",
                             depotId, gid);
                }
            }

            if (!missingManifestLines.empty()) {
                size_t insertPos = lastManifestLineIdx >= 0 ? static_cast<size_t>(lastManifestLineIdx + 1) : lines.size();
                lines.insert(lines.begin() + insertPos, missingManifestLines.begin(), missingManifestLines.end());
                manifestModified = true;
            }

            if (manifestModified) {
                if (!AtomicWriteLines(luaFilePath, lines)) {
                    LOG_ERROR("DenuvoSync: failed to update {}", OSTPlatform::Encoding::PathToUtf8(luaFilePath));
                    return false;
                }
                LuaConfig::ParseFile(OSTPlatform::Encoding::PathToUtf8(luaFilePath));
            } else {
                LOG_DEBUG("DenuvoSync: manifests in {} are already up-to-date", OSTPlatform::Encoding::PathToUtf8(luaFilePath));
            }
        }
    }

    // Mirror .manifest files into <AppId>/ folder, clean obsolete mirrors, and sync to Steam depotcache
    CopyManifestMirrors(appId, appLuaDir, acfData, acfPath);
    CleanObsoleteManifestMirrors(appLuaDir, acfData.installedDepots);
    LuaConfig::SyncManifests(OSTPlatform::Encoding::PathToUtf8(appLuaDir));

    // If an AppTicket is already cached, ensure it is synchronized into Lua (skip if freshly written during generation)
    if (!ticketAlreadyWritten) {
        std::vector<uint8_t> cachedTicket = AppTicket::GetSteamConfigStoreTicket(appId);
        if (cachedTicket.empty()) {
            cachedTicket = AppTicket::GetCachedAppOwnershipTicket(appId);
        }
        if (!cachedTicket.empty()) {
            SyncAppTicketToLua(appId, cachedTicket.data(), cachedTicket.size());
        }
    }

    LOG_INFO("DenuvoSync: SyncOrGenerate completed successfully for appId={}", appId);
    return true;
}

bool SyncAppTicketToLua(AppId_t appId, const uint8_t* pTicketData, size_t ticketSize) {
    if (appId == 0 || appId == k_uAppIdInvalid || !pTicketData || ticketSize < AppTicket::kSteamIdTicketMinimumSize) {
        return false;
    }

    const uint64_t steamId = AppTicket::ExtractSteamIdFromTicketBytes(pTicketData, ticketSize);
    if (steamId == 0) {
        LOG_WARN("DenuvoSync: cannot extract SteamID from ticket for appId={}", appId);
        return false;
    }

    // 🛑 LICENSE ENTITLEMENT RESTRICTION:
    // Only genuine owners or accounts with a valid family sharing license are allowed to persist AppTickets to disk.
    // If the current account does not hold a valid license, keep in memory only.
    if (!Hooks_Package::HasValidLicense(appId)) {
        LOG_DEBUG("DenuvoSync: appId={} does not have a valid license on current account — skipping disk ticket sync", appId);
        AppTicket::WriteAppOwnershipTicket(appId, std::vector<uint8_t>(pTicketData, pTicketData + ticketSize));
        return false;
    }

    const std::string newHex = ToHex(pTicketData, ticketSize);

    const auto luaPath = ResolveAppLuaPath(appId);
    if (luaPath.empty()) {
        LOG_WARN("DenuvoSync: unable to resolve Lua file path for appId={}", appId);
        return false;
    }

    std::error_code ec;
    bool luaExists = std::filesystem::exists(luaPath, ec) && !ec;

    // If Lua does not exist yet, check if this app was launched with -lua
    if (!luaExists) {
        if (IsDPlusLaunch(appId)) {
            LOG_INFO("DenuvoSync: -lua active and Lua missing for appId={}, generating via SyncOrGenerate", appId);
            SyncOrGenerate(appId, "", true);
            luaExists = std::filesystem::exists(luaPath, ec) && !ec;
        }
    }

    if (!luaExists) {
        LOG_DEBUG("DenuvoSync: no Lua configuration exists for appId={} — skipping ticket sync to disk", appId);
        // Keep in memory so this session has it
        AppTicket::WriteAppOwnershipTicket(appId, std::vector<uint8_t>(pTicketData, pTicketData + ticketSize));
        return false;
    }

    std::lock_guard fileLock(g_syncFileMutex);
    std::ifstream in(luaPath);
    if (!in.is_open()) {
        LOG_ERROR("DenuvoSync: failed to open Lua file {} for ticket update", OSTPlatform::Encoding::PathToUtf8(luaPath));
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    in.close();

    bool found = false;
    bool modified = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view sv = TrimWhitespace(lines[i]);
        if (sv.empty()) continue;

        std::string lower(sv);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        size_t pos = lower.find("setappticket");
        if (pos == std::string::npos) continue;

        size_t openParen = lower.find('(', pos);
        if (openParen == std::string::npos) continue;

        std::string_view prefix = TrimWhitespace(sv.substr(0, pos));
        bool isComment = false;
        if (!prefix.empty()) {
            if (prefix.starts_with("--")) {
                isComment = true;
            } else {
                continue;
            }
        }

        size_t comma = lower.find(',', openParen);
        if (comma == std::string::npos || comma <= openParen) continue;

        std::string_view arg1 = TrimWhitespace(sv.substr(openParen + 1, comma - openParen - 1));
        uint32_t parsedAppId = 0;
        auto [p, ec2] = std::from_chars(arg1.data(), arg1.data() + arg1.size(), parsedAppId);
        if (ec2 != std::errc{} || parsedAppId != appId) {
            continue;
        }

        found = true;

        size_t firstQuote = sv.find_first_of("\"'", comma);
        size_t secondQuote = std::string_view::npos;
        if (firstQuote != std::string_view::npos) {
            const char quoteChar = sv[firstQuote];
            secondQuote = sv.find(quoteChar, firstQuote + 1);
        }
        std::string_view existingHex;
        if (firstQuote != std::string_view::npos && secondQuote != std::string_view::npos && secondQuote > firstQuote) {
            existingHex = sv.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        }

        if (isComment || existingHex != newHex) {
            lines[i] = std::format("setAppTicket({}, \"{}\")", appId, newHex);
            modified = true;
            LOG_INFO("DenuvoSync: {} setAppTicket for appId={} in {}",
                     isComment ? "uncommented & updated" : "updated", appId, OSTPlatform::Encoding::PathToUtf8(luaPath));

            if (i > 0) {
                std::string prevLower = lines[i - 1];
                std::transform(prevLower.begin(), prevLower.end(), prevLower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (prevLower.find("commented out because eticket") != std::string::npos) {
                    lines.erase(lines.begin() + (i - 1));
                    --i;
                }
            }
        } else {
            LOG_DEBUG("DenuvoSync: setAppTicket for appId={} is already active and up-to-date", appId);
        }
        break;
    }

    if (!found) {
        lines.push_back("");
        lines.push_back("-- App Ownership Ticket (AppTicket)");
        lines.push_back(std::format("setAppTicket({}, \"{}\")", appId, newHex));
        modified = true;
        LOG_INFO("DenuvoSync: appended setAppTicket for appId={} in {}", appId, OSTPlatform::Encoding::PathToUtf8(luaPath));
    }

    if (modified) {
        if (!AtomicWriteLines(luaPath, lines)) {
            LOG_ERROR("DenuvoSync: failed to write updated Lua file {}", OSTPlatform::Encoding::PathToUtf8(luaPath));
            return false;
        }
        // Reload Lua config only when the file content was actually modified
        LuaConfig::ParseFile(OSTPlatform::Encoding::PathToUtf8(luaPath));
    }

    // Always store ticket in memory
    AppTicket::WriteAppOwnershipTicket(appId, std::vector<uint8_t>(pTicketData, pTicketData + ticketSize));
    return true;
}

} // namespace PipeManager::DenuvoAuth
