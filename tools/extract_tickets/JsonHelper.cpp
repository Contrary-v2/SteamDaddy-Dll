#include "JsonHelper.h"

#include <cctype>
#include <charconv>
#include <limits>

namespace OST::ExtractTickets {

namespace {
    std::string_view SkipWhitespace(std::string_view sv) noexcept {
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
            sv.remove_prefix(1);
        }
        return sv;
    }

    // State-aware JSON key search: only matches `"key"` tokens when not inside a string value
    size_t FindKeyPosition(std::string_view json, std::string_view key) noexcept {
        bool inStr = false;
        bool escaped = false;
        size_t strStart = 0;

        for (size_t i = 0; i < json.size(); ++i) {
            const char c = json[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\' && inStr) {
                escaped = true;
                continue;
            }
            if (c == '"') {
                if (!inStr) {
                    inStr = true;
                    strStart = i + 1;
                } else {
                    inStr = false;
                    const std::string_view token = json.substr(strStart, i - strStart);
                    size_t afterQuote = json.find_first_not_of(" \t\r\n", i + 1);
                    if (afterQuote != std::string_view::npos && json[afterQuote] == ':' && token == key) {
                        return afterQuote + 1; // Position immediately after ':'
                    }
                }
            }
        }
        return std::string_view::npos;
    }

    std::string UnescapeJsonString(std::string_view input) {
        std::string out;
        out.reserve(input.size());

        for (size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '\\' && i + 1 < input.size()) {
                char next = input[++i];
                switch (next) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        // 4-hex digit unicode escape
                        if (i + 4 < input.size()) {
                            unsigned int codepoint = 0;
                            auto hexChunk = input.substr(i + 1, 4);
                            auto [ptr, ec] = std::from_chars(hexChunk.data(), hexChunk.data() + 4, codepoint, 16);
                            if (ec == std::errc() && ptr == hexChunk.data() + 4) {
                                i += 4;
                                // Handle UTF-16 surrogate pairs (RFC 8259)
                                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                                    if (i + 6 < input.size() && input[i + 1] == '\\' && input[i + 2] == 'u') {
                                        unsigned int lowSurrogate = 0;
                                        auto lowChunk = input.substr(i + 3, 4);
                                        auto [ptr2, ec2] = std::from_chars(lowChunk.data(), lowChunk.data() + 4, lowSurrogate, 16);
                                        if (ec2 == std::errc() && ptr2 == lowChunk.data() + 4 &&
                                            lowSurrogate >= 0xDC00 && lowSurrogate <= 0xDFFF) {
                                            i += 6;
                                            codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (lowSurrogate - 0xDC00));
                                        } else {
                                            out.push_back('?');
                                            break;
                                        }
                                    } else {
                                        out.push_back('?');
                                        break;
                                    }
                                } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                                    // Unpaired low surrogate is invalid Unicode
                                    out.push_back('?');
                                    break;
                                }
                                if (codepoint <= 0x7F) {
                                    out.push_back(static_cast<char>(codepoint));
                                } else if (codepoint <= 0x7FF) {
                                    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
                                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                                } else if (codepoint <= 0xFFFF) {
                                    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
                                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                                } else if (codepoint <= 0x10FFFF) {
                                    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
                                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                                } else {
                                    out.push_back('?');
                                }
                                break;
                            }
                        }
                        out.push_back('?');
                        break;
                    }
                    default:
                        out.push_back(next);
                        break;
                }
            } else {
                out.push_back(input[i]);
            }
        }
        return out;
    }

    size_t FindMatchingBracket(std::string_view sv, size_t openPos, char openChar, char closeChar,
                               size_t limitPos = std::string_view::npos) noexcept {
        const size_t end = (limitPos == std::string_view::npos || limitPos > sv.size()) ? sv.size() : limitPos;
        if (openPos >= end || sv[openPos] != openChar) return std::string_view::npos;

        int depth = 0;
        bool inStr = false;
        bool escaped = false;
        for (size_t i = openPos; i < end; ++i) {
            const char c = sv[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\' && inStr) {
                escaped = true;
                continue;
            }
            if (c == '"') {
                inStr = !inStr;
                continue;
            }
            if (inStr) continue;

            if (c == openChar) {
                ++depth;
            } else if (c == closeChar) {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string_view::npos;
    }

    size_t FindArrayStartForKey(std::string_view json, std::string_view key) noexcept {
        const size_t valPos = FindKeyPosition(json, key);
        if (valPos == std::string_view::npos) return std::string_view::npos;
        const size_t arrayStart = json.find_first_not_of(" \t\r\n", valPos);
        if (arrayStart == std::string_view::npos || json[arrayStart] != '[') {
            return std::string_view::npos;
        }
        return arrayStart;
    }
} // namespace

std::optional<std::string> JsonHelper::GetString(std::string_view json, std::string_view key) {
    size_t valPos = FindKeyPosition(json, key);
    if (valPos == std::string_view::npos) return std::nullopt;

    std::string_view sv = SkipWhitespace(json.substr(valPos));
    if (sv.empty() || sv.front() != '"') return std::nullopt;

    sv.remove_prefix(1); // skip opening quote
    bool escaped = false;
    for (size_t i = 0; i < sv.size(); ++i) {
        if (escaped) {
            escaped = false;
        } else if (sv[i] == '\\') {
            escaped = true;
        } else if (sv[i] == '"') {
            return UnescapeJsonString(sv.substr(0, i));
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> JsonHelper::GetUInt64(std::string_view json, std::string_view key) {
    size_t valPos = FindKeyPosition(json, key);
    if (valPos == std::string_view::npos) return std::nullopt;

    std::string_view sv = SkipWhitespace(json.substr(valPos));
    if (sv.empty()) return std::nullopt;

    if (sv.front() == '"') {
        sv.remove_prefix(1);
        size_t endQuote = sv.find('"');
        if (endQuote == std::string_view::npos) return std::nullopt;
        sv = sv.substr(0, endQuote);
    } else {
        size_t endNum = sv.find_first_of(" \t\r\n,}]");
        if (endNum != std::string_view::npos) {
            sv = sv.substr(0, endNum);
        }
    }

    if (sv.empty()) return std::nullopt;
    uint64_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc() && ptr == sv.data() + sv.size()) {
        return val;
    }
    return std::nullopt;
}

std::optional<uint32_t> JsonHelper::GetUInt32(std::string_view json, std::string_view key) {
    auto val64 = GetUInt64(json, key);
    if (val64.has_value() && *val64 <= (std::numeric_limits<uint32_t>::max)()) {
        return static_cast<uint32_t>(*val64);
    }
    return std::nullopt;
}

std::vector<AllowedConfirmation> JsonHelper::GetConfirmations(std::string_view json) {
    std::vector<AllowedConfirmation> list;
    size_t arrayStart = FindArrayStartForKey(json, "allowed_confirmations");
    if (arrayStart == std::string_view::npos) return list;

    size_t arrayEnd = FindMatchingBracket(json, arrayStart, '[', ']');
    if (arrayEnd == std::string_view::npos) return list;

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos || objStart >= arrayEnd) break;

        size_t objEnd = FindMatchingBracket(json, objStart, '{', '}', arrayEnd);
        if (objEnd == std::string_view::npos || objEnd >= arrayEnd) break;

        std::string_view itemJson = json.substr(objStart, objEnd - objStart + 1);
        auto cTypeOpt = GetUInt32(itemJson, "confirmation_type");
        if (cTypeOpt && *cTypeOpt <= static_cast<uint32_t>((std::numeric_limits<int>::max)())) {
            AllowedConfirmation conf;
            conf.type = static_cast<int>(*cTypeOpt);
            conf.associatedMessage = GetString(itemJson, "associated_message").value_or("");
            list.push_back(std::move(conf));
        }

        pos = objEnd + 1;
    }
    return list;
}

std::vector<std::string> JsonHelper::GetStringArray(std::string_view json, std::string_view key) {
    std::vector<std::string> items;
    size_t arrayStart = FindArrayStartForKey(json, key);
    if (arrayStart == std::string_view::npos) return items;

    size_t arrayEnd = FindMatchingBracket(json, arrayStart, '[', ']');
    if (arrayEnd == std::string_view::npos) return items;

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t strStart = json.find('"', pos);
        if (strStart == std::string_view::npos || strStart >= arrayEnd) break;

        size_t strEnd = std::string_view::npos;
        bool escaped = false;
        for (size_t i = strStart + 1; i < arrayEnd; ++i) {
            if (escaped) {
                escaped = false;
            } else if (json[i] == '\\') {
                escaped = true;
            } else if (json[i] == '"') {
                strEnd = i;
                break;
            }
        }
        if (strEnd == std::string_view::npos) break;

        items.push_back(UnescapeJsonString(json.substr(strStart + 1, strEnd - strStart - 1)));
        pos = strEnd + 1;
    }
    return items;
}

std::vector<OwnedGameInfo> JsonHelper::ParseOwnedGames(std::string_view json) {
    std::vector<OwnedGameInfo> games;

    size_t arrayStart = FindArrayStartForKey(json, "games");
    if (arrayStart == std::string_view::npos) return games;

    size_t arrayEnd = FindMatchingBracket(json, arrayStart, '[', ']');
    if (arrayEnd == std::string_view::npos) {
        arrayEnd = json.size();
    }

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos || objStart >= arrayEnd) break;

        size_t objEnd = FindMatchingBracket(json, objStart, '{', '}', arrayEnd);
        if (objEnd == std::string_view::npos || objEnd >= arrayEnd) break;

        std::string_view itemJson = json.substr(objStart, objEnd - objStart + 1);

        auto appIdOpt = GetUInt32(itemJson, "appid");
        auto nameOpt = GetString(itemJson, "name");

        if (appIdOpt && *appIdOpt > 0) {
            OwnedGameInfo info;
            info.appId = *appIdOpt;
            info.name = nameOpt.value_or("App " + std::to_string(info.appId));
            games.push_back(std::move(info));
        }

        pos = objEnd + 1;
    }

    return games;
}

std::vector<OwnedGameInfo> JsonHelper::ParseSharedLibraryApps(std::string_view json) {
    std::vector<OwnedGameInfo> games;

    size_t arrayStart = FindArrayStartForKey(json, "apps");
    if (arrayStart == std::string_view::npos) return games;

    size_t arrayEnd = FindMatchingBracket(json, arrayStart, '[', ']');
    if (arrayEnd == std::string_view::npos) {
        arrayEnd = json.size();
    }

    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string_view::npos || objStart >= arrayEnd) break;

        size_t objEnd = FindMatchingBracket(json, objStart, '{', '}', arrayEnd);
        if (objEnd == std::string_view::npos || objEnd >= arrayEnd) break;

        std::string_view itemJson = json.substr(objStart, objEnd - objStart + 1);

        auto appIdOpt = GetUInt32(itemJson, "appid");
        auto nameOpt = GetString(itemJson, "name");
        auto excludeOpt = GetUInt32(itemJson, "exclude_reason");

        if (appIdOpt && *appIdOpt > 0) {
            // exclude_reason != 0 indicates developer or borrower restriction
            if (!excludeOpt.has_value() || *excludeOpt == 0) {
                OwnedGameInfo info;
                info.appId = *appIdOpt;
                info.name = nameOpt.value_or("");
                games.push_back(std::move(info));
            }
        }

        pos = objEnd + 1;
    }

    return games;
}

} // namespace OST::ExtractTickets
