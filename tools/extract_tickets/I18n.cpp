#include "I18n.h"

#include <array>
#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace OST::ExtractTickets {

namespace {

#define OST_I18N_TABLE_DEF(key, en, zh, es) std::array<std::string_view, kLanguageCount>{ en, zh, es },

constexpr std::array<std::string_view, kLanguageCount> kTranslations[] = {
    OST_I18N_MESSAGES(OST_I18N_TABLE_DEF)
    OST_I18N_LOG_MESSAGES(OST_I18N_TABLE_DEF)
};

#undef OST_I18N_TABLE_DEF

static_assert(std::size(kTranslations) == kMsgKeyCount, "kTranslations size must match kMsgKeyCount exactly!");

std::atomic<Language> s_currentLang{Language::English};

bool TryParseLanguage(std::string_view val, Language& outLang) noexcept {
    if (val.starts_with("zh") || val.starts_with("ZH") || val == "chinese" || val == "schinese" || val == "cn" || val == "CN") {
        outLang = Language::Chinese;
        return true;
    }
    if (val.starts_with("es") || val.starts_with("ES") || val == "spanish" || val == "latam") {
        outLang = Language::Spanish;
        return true;
    }
    if (val.starts_with("en") || val.starts_with("EN") || val == "english") {
        outLang = Language::English;
        return true;
    }
    return false;
}

} // namespace

Language I18n::DetectSystemLanguage() noexcept {
    char envBuf[64]{0};
#if defined(_WIN32)
    DWORD envLen = GetEnvironmentVariableA("OST_LANG", envBuf, sizeof(envBuf));
    if (envLen > 0 && envLen < sizeof(envBuf)) {
        Language parsed{};
        if (TryParseLanguage(std::string_view(envBuf, envLen), parsed)) {
            return parsed;
        }
    }

    LANGID langId = GetUserDefaultUILanguage();
    WORD primaryLang = PRIMARYLANGID(langId);
    if (primaryLang == LANG_CHINESE) {
        return Language::Chinese;
    }
    if (primaryLang == LANG_SPANISH) {
        return Language::Spanish;
    }
#endif
    return Language::English;
}

Language I18n::GetCurrentLanguage() noexcept {
    return s_currentLang.load(std::memory_order_relaxed);
}

void I18n::SetLanguage(Language lang) noexcept {
    s_currentLang.store(lang, std::memory_order_relaxed);
#if defined(_WIN32)
    SetEnvironmentVariableA("OST_LANG", GetLanguageCode(lang).data());
#endif
}

bool I18n::SetLanguageFromString(std::string_view langStr) noexcept {
    Language parsed{};
    if (TryParseLanguage(langStr, parsed)) {
        SetLanguage(parsed);
        return true;
    }
    return false;
}

void I18n::InitLanguage() noexcept {
    SetLanguage(DetectSystemLanguage());
}

std::string_view I18n::GetLanguageCode(Language lang) noexcept {
    switch (lang) {
        case Language::Chinese: return "zh";
        case Language::Spanish: return "es";
        case Language::English:
        default:                return "en";
    }
}

const char* I18n::GetSteamLanguageCode(Language lang) noexcept {
    switch (lang) {
        case Language::Chinese: return "schinese";
        case Language::Spanish: return "spanish";
        case Language::English:
        default:                return "english";
    }
}

std::string_view I18n::Get(MsgKey key) noexcept {
    const size_t kIdx = static_cast<size_t>(key);
    if (kIdx >= kMsgKeyCount) return "";
    const size_t lIdx = static_cast<size_t>(s_currentLang.load(std::memory_order_relaxed));
    return kTranslations[kIdx][lIdx < kLanguageCount ? lIdx : 0];
}

} // namespace OST::ExtractTickets
