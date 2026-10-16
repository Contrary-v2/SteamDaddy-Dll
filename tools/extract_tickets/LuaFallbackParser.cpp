#include "LuaFallbackParser.h"
#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace OST::ExtractTickets {

namespace {

std::string_view StripUnquotedComment(std::string_view line, std::string_view commentMarker, std::string* outComment = nullptr) {
    char quoteChar = '\0';
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && quoteChar == '"') {
            escaped = true;
            continue;
        }
        if (quoteChar != '\0') {
            if (c == quoteChar) {
                quoteChar = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quoteChar = c;
            continue;
        }
        if (line.substr(i).starts_with(commentMarker)) {
            if (outComment) {
                *outComment = SanitizeComment(TrimWhitespace(line.substr(i + commentMarker.size())));
            }
            return TrimWhitespace(line.substr(0, i));
        }
    }
    return line;
}

size_t FindClosingParen(std::string_view sv, size_t openPos) noexcept {
    if (openPos >= sv.size() || sv[openPos] != '(') return std::string_view::npos;
    int depth = 0;
    char quoteChar = '\0';
    bool escaped = false;
    for (size_t i = openPos; i < sv.size(); ++i) {
        const char c = sv[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && quoteChar != '\0') {
            escaped = true;
            continue;
        }
        if (quoteChar != '\0') {
            if (c == quoteChar) quoteChar = '\0';
            continue;
        }
        if (c == '"' || c == '\'') {
            quoteChar = c;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

void ParseTomlLuaPaths(const std::string& tomlFilePath, std::vector<std::string>& outPaths) {
    std::filesystem::path tomlPath = Utf8Path(tomlFilePath);
    std::ifstream file(tomlPath);
    if (!file) return;

    std::filesystem::path baseDir = tomlPath.parent_path();

    std::string line;
    bool inLuaSection = false;
    bool inPathsArray = false;
    bool firstLine = true;

    while (std::getline(file, line)) {
        std::string_view sv(line);
        if (firstLine) {
            firstLine = false;
            if (sv.starts_with("\xEF\xBB\xBF")) {
                sv.remove_prefix(3);
            }
        }
        sv = TrimWhitespace(StripUnquotedComment(sv, "#"));
        if (sv.empty()) continue;

        if (sv.starts_with('[') && sv.ends_with(']') && !inPathsArray) {
            std::string_view sec = sv.substr(1, sv.size() - 2);
            sec = TrimWhitespace(sec);
            inLuaSection = EqualIgnoreCase(sec, "lua");
            inPathsArray = false;
            continue;
        }

        if (!inLuaSection) continue;

        if (!inPathsArray) {
            size_t eqPos = sv.find('=');
            if (eqPos != std::string_view::npos) {
                std::string_view keyPart = TrimWhitespace(sv.substr(0, eqPos));
                if (EqualIgnoreCase(keyPart, "paths")) {
                    size_t bracket = sv.find('[', eqPos + 1);
                    if (bracket != std::string_view::npos) {
                        inPathsArray = true;
                        sv = sv.substr(bracket + 1);
                    }
                }
            }
        }

        if (inPathsArray) {
            size_t pos = 0;
            while (pos < sv.size()) {
                if (sv[pos] == ']') {
                    inPathsArray = false;
                    break;
                }
                if (sv[pos] == '"' || sv[pos] == '\'') {
                    const char qChar = sv[pos];
                    std::string pStr;
                    size_t i = pos + 1;
                    bool closed = false;
                    while (i < sv.size()) {
                        if (qChar == '"' && sv[i] == '\\' && i + 1 < sv.size()) {
                            if (sv[i + 1] == '"' || sv[i + 1] == '\\') {
                                pStr.push_back(sv[i + 1]);
                                i += 2;
                                continue;
                            }
                        }
                        if (sv[i] == qChar) {
                            closed = true;
                            pos = i + 1;
                            break;
                        }
                        pStr.push_back(sv[i]);
                        ++i;
                    }
                    if (closed) {
                        std::filesystem::path p = Utf8Path(pStr);
                        if (p.is_relative()) {
                            outPaths.push_back(PathToUtf8((baseDir / p).lexically_normal()));
                        } else {
                            outPaths.push_back(PathToUtf8(p.lexically_normal()));
                        }
                        continue;
                    }
                    break;
                }
                ++pos;
            }
        }
    }
}

std::vector<std::string_view> SplitArgs(std::string_view argsStr) {
    std::vector<std::string_view> args;
    size_t start = 0;
    char quoteChar = '\0';
    bool escaped = false;
    for (size_t i = 0; i < argsStr.size(); ++i) {
        const char c = argsStr[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && quoteChar != '\0') {
            escaped = true;
            continue;
        }
        if (quoteChar != '\0') {
            if (c == quoteChar) quoteChar = '\0';
            continue;
        }
        if (c == '"' || c == '\'') {
            quoteChar = c;
        } else if (c == ',') {
            args.push_back(TrimWhitespace(argsStr.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start <= argsStr.size()) {
        args.push_back(TrimWhitespace(argsStr.substr(start)));
    }
    return args;
}

std::string StripQuotes(std::string_view str) {
    std::string_view trimmed = TrimWhitespace(str);
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '"' && trimmed.back() == '"') ||
         (trimmed.front() == '\'' && trimmed.back() == '\''))) {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    return std::string(trimmed);
}

bool LuaContentRefersToAppId(std::string_view content, uint32_t targetAppId) {
    size_t searchPos = 0;
    while ((searchPos = FindIgnoreCase(content, "addappid", searchPos)) != std::string_view::npos) {
        size_t openP = content.find('(', searchPos + 8);
        if (openP != std::string_view::npos) {
            size_t closeP = FindClosingParen(content, openP);
            if (closeP != std::string_view::npos && closeP > openP) {
                auto args = SplitArgs(content.substr(openP + 1, closeP - openP - 1));
                if (!args.empty()) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0])); parsedId && *parsedId == targetAppId) {
                        return true;
                    }
                }
            }
        }
        searchPos += 8;
    }
    return false;
}

void ParseLuaContent(std::string_view content, uint32_t targetAppId, LuaFallbackData& out) {
    size_t pos = 0;
    uint32_t currentDlcId = 0;

    while (pos < content.size()) {
        size_t nextPos = content.find('\n', pos);
        std::string_view rawLine = (nextPos != std::string_view::npos)
            ? content.substr(pos, nextPos - pos)
            : content.substr(pos);
        pos = (nextPos != std::string_view::npos) ? nextPos + 1 : content.size();

        std::string_view line = TrimWhitespace(rawLine);
        if (line.empty() || line.starts_with("--")) {
            continue;
        }

        std::string comment;
        std::string_view code = StripUnquotedComment(line, "--", &comment);

        // 1. addappid(...)
        if (size_t fPos = FindIgnoreCase(code, "addappid"); fPos != std::string_view::npos) {
            size_t openP = code.find('(', fPos + 8);
            size_t closeP = (openP != std::string_view::npos) ? FindClosingParen(code, openP) : std::string_view::npos;
            if (openP != std::string_view::npos && closeP != std::string_view::npos && closeP > openP) {
                std::string_view argsStr = code.substr(openP + 1, closeP - openP - 1);
                auto args = SplitArgs(argsStr);
                if (!args.empty()) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0]))) {
                        uint32_t dId = *parsedId;
                        if (args.size() >= 3) {
                            std::string key = StripQuotes(args[2]);
                            if (key.size() == 64 && IsHex64(key)) {
                                out.depotKeys[dId] = key;
                            }
                        }

                        if (dId != targetAppId) {
                            out.dlcIds.insert(dId);
                            if (!comment.empty()) {
                                out.dlcNames.try_emplace(dId, comment);
                                currentDlcId = dId;
                            } else if (currentDlcId != 0 && dId > currentDlcId && dId <= currentDlcId + 20) {
                                out.depotToDlc.try_emplace(dId, currentDlcId);
                            }
                        }
                    }
                }
            }
        }
        // 2. setmanifestid(...)
        else if (size_t fPos = FindIgnoreCase(code, "setmanifestid"); fPos != std::string_view::npos) {
            size_t openP = code.find('(', fPos + 13);
            size_t closeP = (openP != std::string_view::npos) ? FindClosingParen(code, openP) : std::string_view::npos;
            if (openP != std::string_view::npos && closeP != std::string_view::npos && closeP > openP) {
                std::string_view argsStr = code.substr(openP + 1, closeP - openP - 1);
                auto args = SplitArgs(argsStr);
                if (args.size() >= 2) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0]))) {
                        uint32_t dId = *parsedId;
                        std::string manId = StripQuotes(args[1]);
                        if (IsValidManifestId(manId)) {
                            out.depotManifests[dId] = manId;
                        }
                    }
                }
            }
        }
        // 3. addtoken(...)
        else if (size_t fPos = FindIgnoreCase(code, "addtoken"); fPos != std::string_view::npos) {
            size_t openP = code.find('(', fPos + 8);
            size_t closeP = (openP != std::string_view::npos) ? FindClosingParen(code, openP) : std::string_view::npos;
            if (openP != std::string_view::npos && closeP != std::string_view::npos && closeP > openP) {
                std::string_view argsStr = code.substr(openP + 1, closeP - openP - 1);
                auto args = SplitArgs(argsStr);
                if (args.size() >= 2) {
                    if (auto parsedId = ParseAppId(StripQuotes(args[0]))) {
                        uint32_t tAppId = *parsedId;
                        std::string tokStr = StripQuotes(args[1]);
                        uint64_t tokenVal = 0;
                        auto [ptr, ec] = std::from_chars(tokStr.data(), tokStr.data() + tokStr.size(), tokenVal);
                        if (ec == std::errc{} && ptr == tokStr.data() + tokStr.size() && tokenVal != 0) {
                            out.appTokens[tAppId] = tokenVal;
                        }
                    }
                }
            }
        }
    }
}

void ScanManifestFilesInDir(const std::string& dir, LuaFallbackData& out) {
    std::error_code ec;
    std::filesystem::path dirP = Utf8Path(dir);
    if (!std::filesystem::exists(dirP, ec) || !std::filesystem::is_directory(dirP, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(dirP, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            std::string fname = PathToUtf8(entry.path().filename());
            if (fname.size() > 9 && EqualIgnoreCase(std::string_view(fname).substr(fname.size() - 9), ".manifest")) {
                size_t under = fname.find('_');
                size_t dot = fname.rfind('.');
                if (under != std::string_view::npos && dot != std::string_view::npos && dot > under + 1) {
                    std::string_view depotStr = std::string_view(fname).substr(0, under);
                    std::string_view candidateMan = std::string_view(fname).substr(under + 1, dot - under - 1);
                    if (auto dId = ParseAppId(depotStr)) {
                        if (IsValidManifestId(candidateMan)) {
                            out.manifestFiles[*dId] = PathToUtf8(entry.path());
                            out.depotManifests.try_emplace(*dId, std::string(candidateMan));
                        }
                    }
                }
            }
        }
    }
}

} // namespace

std::vector<std::string> GetOstLuaSearchDirectories(const std::string& steamPath) {
    std::vector<std::string> dirs;

    auto addDir = [&](const std::string& path) {
        if (path.empty()) return;
        std::string norm = NormalizeDir(path);
        if (norm.empty()) return;

        std::error_code ec;
        if (std::filesystem::is_directory(Utf8Path(norm), ec)) {
            if (std::none_of(dirs.begin(), dirs.end(), [&](const std::string& existing) {
                return EqualIgnoreCase(existing, norm);
            })) {
                dirs.push_back(std::move(norm));
            }
        }
    };

    // 1. Steam official config directories
    if (!steamPath.empty()) {
        addDir(JoinPath(steamPath, "config\\lua"));
        addDir(JoinPath(steamPath, "config\\st_config"));
    }

    // 2. Executable directory
    wchar_t exeBuf[MAX_PATH * 2]{};
    std::string exeDir;
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf)));
    if (exeLen > 0 && exeLen < std::size(exeBuf)) {
        std::filesystem::path p(exeBuf);
        exeDir = PathToUtf8(p.parent_path());
        if (!exeDir.empty()) {
            addDir(JoinPath(exeDir, "config\\lua"));
            addDir(JoinPath(exeDir, "lua"));
            addDir(JoinPath(exeDir, "st_config"));
            addDir(exeDir);
        }
    }

    // 3. Current working directory
    std::error_code cwdEc;
    std::filesystem::path cwdPath = std::filesystem::current_path(cwdEc);
    std::string cwd;
    if (!cwdEc) {
        cwd = PathToUtf8(cwdPath);
        addDir(JoinPath(cwd, "config\\lua"));
        addDir(JoinPath(cwd, "lua"));
        addDir(JoinPath(cwd, "st_config"));
        addDir(cwd);
    }

    // 4. opensteamtool.toml configured lua paths
    std::vector<std::string> tomlLocations;
    if (!exeDir.empty()) tomlLocations.push_back(JoinPath(exeDir, "opensteamtool.toml"));
    if (!cwd.empty() && !EqualIgnoreCase(cwd, exeDir)) {
        tomlLocations.push_back(JoinPath(cwd, "opensteamtool.toml"));
    }
    if (!steamPath.empty()) {
        tomlLocations.push_back(JoinPath(steamPath, "opensteamtool.toml"));
    }

    std::vector<std::string> tomlLuaPaths;
    for (const auto& tomlFile : tomlLocations) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(Utf8Path(tomlFile), ec)) {
            ParseTomlLuaPaths(tomlFile, tomlLuaPaths);
        }
    }

    for (const auto& p : tomlLuaPaths) {
        addDir(p);
    }

    return dirs;
}

LuaFallbackData ParseLuaFallbackData(const std::string& steamPath, uint32_t targetAppId) {
    LuaFallbackData result;
    if (targetAppId == 0) return result;

    const std::vector<std::string> searchDirs = GetOstLuaSearchDirectories(steamPath);
    const std::string targetLuaName = std::to_string(targetAppId) + ".lua";

    std::vector<std::string> matchedFiles;
    std::vector<std::string> manifestDirs;

    // First, look specifically for <targetAppId>.lua and dedicated <targetAppId>/ folder
    for (const auto& dir : searchDirs) {
        std::string directFile = JoinPath(dir, targetLuaName);
        std::error_code ec;
        if (std::filesystem::is_regular_file(Utf8Path(directFile), ec)) {
            matchedFiles.push_back(directFile);
            manifestDirs.push_back(dir);
        }

        std::string subDir = JoinPath(dir, std::to_string(targetAppId));
        if (std::filesystem::is_directory(Utf8Path(subDir), ec)) {
            std::string subFile = JoinPath(subDir, targetLuaName);
            if (std::filesystem::is_regular_file(Utf8Path(subFile), ec)) {
                matchedFiles.push_back(subFile);
            }
            manifestDirs.push_back(subDir);
        }
    }

    // If no direct <targetAppId>.lua was found, scan all .lua files in the search directories
    if (matchedFiles.empty()) {
        for (const auto& dir : searchDirs) {
            std::error_code dirEc;
            std::filesystem::path dirP = Utf8Path(dir);
            if (!std::filesystem::exists(dirP, dirEc) || !std::filesystem::is_directory(dirP, dirEc)) continue;

            for (const auto& entry : std::filesystem::directory_iterator(dirP, dirEc)) {
                if (dirEc) break;
                if (entry.is_regular_file(dirEc)) {
                    std::string fname = PathToUtf8(entry.path().filename());
                    if (fname.size() > 4 && EqualIgnoreCase(std::string_view(fname).substr(fname.size() - 4), ".lua")) {
                        matchedFiles.push_back(PathToUtf8(entry.path()));
                        manifestDirs.push_back(dir);
                    }
                }
            }
        }
    }

    for (const auto& filePath : matchedFiles) {
        std::ifstream file(Utf8Path(filePath), std::ios::binary);
        if (!file) continue;

        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        if (size <= 0 || size > 16 * 1024 * 1024) continue;

        file.seekg(0, std::ios::beg);
        std::string content(static_cast<size_t>(size), '\0');
        file.read(content.data(), size);
        content.resize(static_cast<size_t>(file.gcount()));

        // If scanning a generic Lua file, ensure it refers to targetAppId specifically
        const std::string fileName = PathToUtf8(Utf8Path(filePath).filename());
        if (!EqualIgnoreCase(fileName, targetLuaName)) {
            if (!LuaContentRefersToAppId(content, targetAppId)) {
                continue;
            }
        }

        ParseLuaContent(content, targetAppId, result);
    }

    // Scan for co-located .manifest files in the matched directories
    for (const auto& mDir : manifestDirs) {
        ScanManifestFilesInDir(mDir, result);
    }

    return result;
}

} // namespace OST::ExtractTickets
