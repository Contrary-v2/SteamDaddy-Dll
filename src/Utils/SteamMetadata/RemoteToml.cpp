#include "RemoteToml.h"
#include "dllmain.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>
#include <toml++/toml.hpp>

namespace RemoteToml {

namespace {
    // Upstream pattern repository: hazre/steam-monitor (stable + beta tracks)
    constexpr const char* kGithubTemplate =
        "https://raw.githubusercontent.com/hazre/steam-monitor/"
        "main/{channel}/{component}/{sha256}.toml";
    constexpr const char* kGhProxyTemplate =
        "https://gh-proxy.com/https://raw.githubusercontent.com/hazre/steam-monitor/"
        "main/{channel}/{component}/{sha256}.toml";

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    static std::vector<std::string> BuildUrlTemplates()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (!remoteUrlTemplate.empty()) {
            if (!IsValidTemplate(remoteUrlTemplate)) {
                LOG_WARN("RemoteToml: remote.url_template must contain "
                         "{channel}, {component}, and {sha256}; remote fetch disabled");
                return {};
            }
            return { remoteUrlTemplate };
        }

        if (Config::GetRemoteOrder() == "github-first")
            return { kGithubTemplate, kGhProxyTemplate };
        return { kGhProxyTemplate, kGithubTemplate };
    }

    static bool ValidateTomlContent(std::string_view body, std::string* outError = nullptr)
    {
        try {
            const auto tbl = toml::parse(body);
            return !tbl.empty();
        } catch (const toml::parse_error& e) {
            if (outError) *outError = e.description();
            return false;
        }
    }

    std::filesystem::path GetCacheFilePath(const Request& request, const std::string& sha256)
    {
        using OSTPlatform::Encoding::PathFromUtf8;
        namespace fs = std::filesystem;
        fs::path steamRoot = PathFromUtf8(request.dllPath).parent_path();
        fs::path baseDir = PathFromUtf8(GetStorageDirectory());
        if (baseDir.empty()) {
            baseDir = steamRoot;
        }
        return baseDir / "steamdaddy" / request.channel / request.component / (sha256 + ".toml");
    }
} // namespace

Result Fetch(const Request& request)
{
    namespace fs = std::filesystem;
    using OSTPlatform::Encoding::PathFromUtf8;
    Result out;

    // 1. Compute SHA-256 of DLL on disk
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256Of failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Resolve cache path & ensure cache directory exists
    fs::path cachePath = GetCacheFilePath(request, out.sha256);
    fs::path cacheDir = cachePath.parent_path();
    std::error_code ec;
    fs::create_directories(cacheDir, ec);

    // Check local storage directory first, then fallback to steam install dir if in portable mode
    fs::path localPath = cachePath;
    bool found = fs::exists(localPath, ec) && !ec;
    if (!found && IsPortableMode()) {
        fs::path steamRoot = PathFromUtf8(request.dllPath).parent_path();
        fs::path steamCachePath = steamRoot / "steamdaddy" / request.channel / request.component / (out.sha256 + ".toml");
        if (fs::exists(steamCachePath, ec) && !ec) {
            localPath = steamCachePath;
            found = true;
        }
    }

    if (found) {
        ec.clear();
        const auto sz = fs::file_size(localPath, ec);
        if (!ec && sz > 0) {
            std::ifstream ifs(localPath, std::ios::binary);
            if (ifs) {
                std::string buf((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
                if (!buf.empty()) {
                    LOG_INFO("RemoteToml({}/{}): loaded from cache {}",
                             request.channel, request.component, localPath.string());
                    out.body = std::move(buf);
                    out.ok = true;
                    out.fromCache = true;
                    return out;
                }
            }
            LOG_WARN("RemoteToml({}/{}): cache file exists but empty: {}",
                     request.channel, request.component, localPath.string());
        }
    }

    // 3. Cache miss: poll mirrors in order
    const std::vector<std::string> urlTemplates = BuildUrlTemplates();
    OSTPlatform::Http::Result http;
    std::string lastUrl;
    size_t notFoundCount = 0;

    for (size_t i = 0; i < urlTemplates.size(); ++i) {
        lastUrl = ExpandTemplate(urlTemplates[i], request, out.sha256);
        LOG_INFO("RemoteToml({}/{}): downloading {}",
                 request.channel, request.component, lastUrl);

        http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(), nullptr, 0, nullptr);

        const bool is404 = http.ok && http.status == 404;
        const bool gotBody = http.ok && http.status == 200 && !http.body.empty();
        if (!gotBody && !is404) {
            LOG_INFO("RemoteToml({}/{}): transport failed ({} ok={} HTTP={}), retrying once",
                     request.channel, request.component, lastUrl, http.ok, http.status);
            http = OSTPlatform::Http::Execute(L"GET", lastUrl.c_str(), nullptr, 0, nullptr);
        }

        if (http.ok && http.status == 200 && !http.body.empty()) {
            std::string tomlErr;
            if (ValidateTomlContent(http.body, &tomlErr)) {
                break;
            } else {
                LOG_WARN("RemoteToml({}/{}): mirror returned invalid TOML: {} ({})",
                         request.channel, request.component, lastUrl, tomlErr);
            }
        }

        const bool hasNext = (i + 1 < urlTemplates.size());
        if (http.ok && http.status == 404) {
            ++notFoundCount;
            LOG_WARN("RemoteToml({}/{}): mirror 404: {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (hasNext) {
            LOG_WARN("RemoteToml({}/{}): mirror failed (ok={} HTTP={}), falling back",
                     request.channel, request.component, http.ok, http.status);
        }
    }

    // 4. Remote OK -> write cache atomically via .tmp, return body
    if (http.ok && http.status == 200 && !http.body.empty()) {
        fs::path tmpPath = cachePath;
        tmpPath += ".tmp";
        std::error_code writeEc;
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(http.body.data(), static_cast<std::streamsize>(http.body.size()));
                ofs.close();
            }
            if (!ofs) writeEc = std::make_error_code(std::errc::io_error);
        }
        if (!writeEc) {
            fs::remove(cachePath, writeEc);
            if (!writeEc) fs::rename(tmpPath, cachePath, writeEc);
        }
        if (writeEc) {
            LOG_WARN("RemoteToml({}/{}): could not write cache {} ({})",
                     request.channel, request.component, cachePath.string(), writeEc.message());
            std::error_code cleanupEc;
            fs::remove(tmpPath, cleanupEc);
        } else {
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, cachePath.string());
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Failure
    out.allNotFound = !urlTemplates.empty() && notFoundCount == urlTemplates.size();
    if (out.allNotFound) {
        LOG_WARN("RemoteToml({}/{}): upstream has not published (all {} mirrors 404, last URL: {})",
                 request.channel, request.component, urlTemplates.size(),
                 lastUrl.empty() ? "<none>" : lastUrl);
    } else {
        LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
                 request.channel, request.component,
                 lastUrl.empty() ? "<none>" : lastUrl, http.status);
    }
    return out;
}

bool Invalidate(const Request& request)
{
    namespace fs = std::filesystem;
    using OSTPlatform::Encoding::PathFromUtf8;
    const std::string sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    if (sha256.empty()) return false;

    fs::path cachePath = GetCacheFilePath(request, sha256);
    std::error_code ec;
    bool removed = fs::remove(cachePath, ec);
    if (IsPortableMode()) {
        fs::path steamRoot = PathFromUtf8(request.dllPath).parent_path();
        fs::path steamCachePath = steamRoot / "steamdaddy" / request.channel / request.component / (sha256 + ".toml");
        std::error_code ec2;
        if (fs::remove(steamCachePath, ec2)) removed = true;
    }
    if (removed) {
        LOG_WARN("RemoteToml({}/{}): removed stale cache {}",
                 request.channel, request.component, cachePath.string());
    }
    return removed;
}

} // namespace RemoteToml
