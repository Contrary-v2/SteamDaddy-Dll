#include "TokenStorage.h"
#include "Crypto.h"
#include "Log.h"
#include "RaiiGuards.h"
#include "Utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>

#pragma comment(lib, "crypt32.lib")

namespace OST::ExtractTickets {

namespace {
    constexpr uint32_t kHeaderMagic = 0x4B54534F; // "OSTK"
    constexpr uint16_t kHeaderVersion = 1;

    // App-specific entropy for DPAPI to prevent generic DPAPI dumping tools
    constexpr uint8_t kAppEntropy[] = {
        0x4F, 0x53, 0x54, 0x2D, 0x53, 0x65, 0x63, 0x75,
        0x72, 0x65, 0x2D, 0x54, 0x6F, 0x6B, 0x65, 0x6E,
        0x2D, 0x45, 0x6E, 0x74, 0x72, 0x6F, 0x70, 0x79,
        0x2D, 0x4B, 0x65, 0x79, 0x32, 0x30, 0x32, 0x36
    };

    std::recursive_mutex g_storageMutex;

    void WipeString(std::string& s) noexcept {
        if (s.capacity() > 0) {
            SecureZeroMemory(s.data(), s.capacity());
            s.clear();
        }
    }

    void WipeAccountTokens(CachedAccount& acc) noexcept {
        WipeString(acc.refreshToken);
        WipeString(acc.accessToken);
    }

    struct ScopedAccountsWiper {
        std::vector<CachedAccount>& accounts;
        explicit ScopedAccountsWiper(std::vector<CachedAccount>& accs) noexcept : accounts(accs) {}
        ~ScopedAccountsWiper() noexcept {
            for (auto& acc : accounts) {
                WipeAccountTokens(acc);
            }
        }
        ScopedAccountsWiper(const ScopedAccountsWiper&) = delete;
        ScopedAccountsWiper& operator=(const ScopedAccountsWiper&) = delete;
    };

    struct ScopedDataBlob {
        DATA_BLOB blob{0, nullptr};
        bool wipeOnFree{false};

        explicit ScopedDataBlob(bool wipe = false) noexcept : wipeOnFree(wipe) {}
        ~ScopedDataBlob() noexcept {
            if (blob.pbData) {
                if (wipeOnFree && blob.cbData > 0) {
                    SecureZeroMemory(blob.pbData, blob.cbData);
                }
                LocalFree(blob.pbData);
                blob.pbData = nullptr;
                blob.cbData = 0;
            }
        }
        ScopedDataBlob(const ScopedDataBlob&) = delete;
        ScopedDataBlob& operator=(const ScopedDataBlob&) = delete;
    };

    struct QuarantineStore {
        std::vector<std::string> lines;

        ~QuarantineStore() noexcept {
            Clear();
        }

        void Clear() noexcept {
            for (auto& line : lines) {
                WipeString(line);
            }
            lines.clear();
        }
    };

    QuarantineStore& GetQuarantineStore() {
        static QuarantineStore s_quarantine;
        return s_quarantine;
    }

    void AppendSanitizedField(std::string& out, std::string_view sv) {
        for (char c : sv) {
            if (c != '\t' && c != '\r' && c != '\n') {
                out.push_back(c);
            }
        }
    }

    std::string SerializeAccounts(const std::vector<CachedAccount>& accounts) {
        size_t totalBytes = 64;
        for (const auto& acc : accounts) {
            totalBytes += acc.accountName.size() + acc.refreshToken.size() +
                          acc.accessToken.size() + acc.alias.size() + 64;
        }
        for (const auto& qLine : GetQuarantineStore().lines) {
            totalBytes += qLine.size() + 2;
        }

        std::string result;
        result.reserve(totalBytes);

        char numBuf[32];
        for (const auto& acc : accounts) {
            AppendSanitizedField(result, acc.accountName);
            result.push_back('\t');

            auto [p1, ec1] = std::to_chars(numBuf, numBuf + sizeof(numBuf), acc.steamId);
            if (ec1 == std::errc{}) {
                result.append(numBuf, static_cast<size_t>(p1 - numBuf));
            } else {
                result.push_back('0');
            }
            result.push_back('\t');

            auto [p2, ec2] = std::to_chars(numBuf, numBuf + sizeof(numBuf), acc.lastLoginTime);
            if (ec2 == std::errc{}) {
                result.append(numBuf, static_cast<size_t>(p2 - numBuf));
            } else {
                result.push_back('0');
            }
            result.push_back('\t');

            AppendSanitizedField(result, acc.refreshToken);
            result.push_back('\t');
            AppendSanitizedField(result, acc.accessToken);
            result.push_back('\t');
            result.push_back(acc.isInvalid ? '1' : '0');
            result.push_back('\t');
            result.append(TokenStorage::SanitizeAlias(acc.alias));
            result.push_back('\n');
        }
        for (const auto& qLine : GetQuarantineStore().lines) {
            if (!qLine.empty()) {
                result.append(qLine);
                result.push_back('\n');
            }
        }
        return result;
    }

    std::vector<CachedAccount> DeserializeAccounts(std::string_view plaintext) {
        std::vector<CachedAccount> result;
        GetQuarantineStore().Clear();

        size_t lineStart = 0;
        while (lineStart < plaintext.size()) {
            size_t lineEnd = plaintext.find('\n', lineStart);
            if (lineEnd == std::string_view::npos) {
                lineEnd = plaintext.size();
            }
            std::string_view line = plaintext.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;

            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (line.empty()) continue;

            std::array<std::string_view, 8> fields{};
            size_t fieldCount = 0;
            size_t colStart = 0;
            while (fieldCount < fields.size()) {
                size_t tabPos = line.find('\t', colStart);
                if (tabPos == std::string_view::npos) {
                    fields[fieldCount++] = line.substr(colStart);
                    break;
                }
                fields[fieldCount++] = line.substr(colStart, tabPos - colStart);
                colStart = tabPos + 1;
            }

            if (fieldCount >= 4) {
                const std::string_view accountNameSv = fields[0];
                const std::string_view steamIdStr = fields[1];
                const std::string_view timeStr = fields[2];
                const std::string_view refreshTokenSv = fields[3];

                uint64_t sId = 0;
                const auto [p1, ec1] = std::from_chars(steamIdStr.data(), steamIdStr.data() + steamIdStr.size(), sId);
                int64_t loginTime = 0;
                const auto [p2, ec2] = std::from_chars(timeStr.data(), timeStr.data() + timeStr.size(), loginTime);

                if (ec1 == std::errc{} && p1 == steamIdStr.data() + steamIdStr.size() &&
                    ec2 == std::errc{} && p2 == timeStr.data() + timeStr.size() &&
                    !accountNameSv.empty() && !refreshTokenSv.empty()) {
                    if (sId == 0) {
                        sId = GetJwtSteamId(refreshTokenSv);
                    }
                    CachedAccount acc;
                    acc.accountName.assign(accountNameSv);
                    acc.steamId = sId;
                    acc.lastLoginTime = loginTime;
                    acc.refreshToken.assign(refreshTokenSv);

                    if (fieldCount >= 5) {
                        acc.accessToken.assign(fields[4]);
                    }
                    if (fieldCount >= 6) {
                        acc.isInvalid = (fields[5] == "1" || EqualIgnoreCase(fields[5], "true"));
                    }
                    if (fieldCount >= 7) {
                        acc.alias = TokenStorage::SanitizeAlias(fields[6]);
                    }
                    result.push_back(std::move(acc));
                } else {
                    LOG_WARN("TokenStorage", MsgKey::LogStoreQuarantineAbnormal);
                    GetQuarantineStore().lines.emplace_back(line);
                }
            } else {
                LOG_WARN("TokenStorage", MsgKey::LogStoreSkipMalformed, fieldCount);
                GetQuarantineStore().lines.emplace_back(line);
            }
        }
        return result;
    }

    bool WriteAllBytes(HANDLE hFile, const void* data, DWORD bytesToWrite) noexcept {
        const auto* ptr = static_cast<const uint8_t*>(data);
        DWORD totalWritten = 0;
        while (totalWritten < bytesToWrite) {
            DWORD written = 0;
            if (!WriteFile(hFile, ptr + totalWritten, bytesToWrite - totalWritten, &written, nullptr) || written == 0) {
                return false;
            }
            totalWritten += written;
        }
        return true;
    }

    void SecureWipeFile(const std::filesystem::path& p) {
        std::error_code ec;
        auto fileSize = std::filesystem::file_size(p, ec);
        if (!ec && fileSize > 0) {
            ScopedHandle hFile(CreateFileW(
                p.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (hFile.IsValid()) {
                constexpr std::array<uint8_t, 1024> zeros{};
                uint64_t remaining = fileSize;
                while (remaining > 0) {
                    const DWORD toWrite = static_cast<DWORD>((std::min)(remaining, static_cast<uint64_t>(zeros.size())));
                    if (!WriteAllBytes(hFile, zeros.data(), toWrite)) {
                        break;
                    }
                    remaining -= toWrite;
                }
                FlushFileBuffers(hFile);
            }
        }
        std::filesystem::remove(p, ec);
    }
} // namespace

std::filesystem::path TokenStorage::GetStorageFilePath() {
    wchar_t localAppData[MAX_PATH * 2]{0};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (len == 0 || len >= std::size(localAppData)) {
        PWSTR pPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pPath)) && pPath) {
            struct CoTaskMemGuard {
                PWSTR ptr{nullptr};
                explicit CoTaskMemGuard(PWSTR p) noexcept : ptr(p) {}
                ~CoTaskMemGuard() noexcept { if (ptr) CoTaskMemFree(ptr); }
                CoTaskMemGuard(const CoTaskMemGuard&) = delete;
                CoTaskMemGuard& operator=(const CoTaskMemGuard&) = delete;
            } guard{pPath};
            std::filesystem::path dir = std::filesystem::path(pPath) / L"OpenSteamTool" / L"credentials";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir / L"accounts.enc";
        }
        return L"accounts.enc";
    }

    std::filesystem::path dir = std::filesystem::path(localAppData) / L"OpenSteamTool" / L"credentials";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"accounts.enc";
}

std::vector<CachedAccount> TokenStorage::LoadAccounts() {
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    const auto filePath = GetStorageFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        return {};
    }

    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        LOG_WARN("TokenStorage", MsgKey::LogStoreOpenFileFailed, MaskPath(PathToUtf8(filePath)));
        return {};
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t reserved = 0;
    uint32_t payloadSize = 0;

    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    ifs.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
    ifs.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));

    if (!ifs || magic != kHeaderMagic || version != kHeaderVersion || payloadSize == 0 || payloadSize > 10 * 1024 * 1024) {
        LOG_WARN("TokenStorage", MsgKey::LogStoreHeaderCorrupt);
        return {};
    }

    std::vector<uint8_t> encryptedBlob(payloadSize);
    ifs.read(reinterpret_cast<char*>(encryptedBlob.data()), payloadSize);
    if (ifs.gcount() != static_cast<std::streamsize>(payloadSize)) {
        LOG_WARN("TokenStorage", MsgKey::LogStoreIncompleteRead);
        return {};
    }

    DATA_BLOB inBlob;
    inBlob.pbData = encryptedBlob.data();
    inBlob.cbData = static_cast<DWORD>(encryptedBlob.size());

    DATA_BLOB entropyBlob;
    entropyBlob.pbData = const_cast<BYTE*>(kAppEntropy);
    entropyBlob.cbData = sizeof(kAppEntropy);

    ScopedDataBlob outGuard(true);
    if (!CryptUnprotectData(&inBlob, nullptr, &entropyBlob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outGuard.blob)) {
        LOG_ERROR("TokenStorage", MsgKey::LogStoreDpapiDecryptFailed, GetLastError());
        return {};
    }

    const std::string_view plaintextView(
        reinterpret_cast<const char*>(outGuard.blob.pbData),
        outGuard.blob.cbData);
    auto accounts = DeserializeAccounts(plaintextView);

    LOG_DEBUG("TokenStorage", MsgKey::LogStoreLoadedAccounts, accounts.size());
    return accounts;
}

bool TokenStorage::SaveAccounts(const std::vector<CachedAccount>& accounts) {
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    const auto filePath = GetStorageFilePath();

    if (accounts.empty() && GetQuarantineStore().lines.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec)) {
            SecureWipeFile(filePath);
        }
        return true;
    }

    std::string plaintext = SerializeAccounts(accounts);
    struct PlaintextWiper {
        std::string& str;
        explicit PlaintextWiper(std::string& s) noexcept : str(s) {}
        ~PlaintextWiper() noexcept { WipeString(str); }
        PlaintextWiper(const PlaintextWiper&) = delete;
        PlaintextWiper& operator=(const PlaintextWiper&) = delete;
    } plaintextWiper{plaintext};

    DATA_BLOB inBlob;
    inBlob.pbData = reinterpret_cast<BYTE*>(plaintext.data());
    inBlob.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB entropyBlob;
    entropyBlob.pbData = const_cast<BYTE*>(kAppEntropy);
    entropyBlob.cbData = sizeof(kAppEntropy);

    ScopedDataBlob outGuard(false);
    if (!CryptProtectData(&inBlob, L"OST_Steam_Tokens", &entropyBlob, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outGuard.blob)) {
        LOG_ERROR("TokenStorage", MsgKey::LogStoreDpapiEncryptFailed, GetLastError());
        return false;
    }

    WipeString(plaintext);

    const auto tempPath = std::filesystem::path(
        std::format(L"{}.{}.tmp", filePath.native(), GetCurrentProcessId()));

    {
        ScopedHandle hTemp(CreateFileW(
            tempPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!hTemp.IsValid()) {
            LOG_ERROR("TokenStorage", MsgKey::LogStoreCreateTempFailed, MaskPath(PathToUtf8(tempPath)));
            return false;
        }

        const uint32_t magic = kHeaderMagic;
        const uint16_t version = kHeaderVersion;
        const uint16_t reserved = 0;
        const uint32_t payloadSize = outGuard.blob.cbData;

        uint8_t header[12]{};
        std::memcpy(header + 0, &magic, sizeof(magic));
        std::memcpy(header + 4, &version, sizeof(version));
        std::memcpy(header + 6, &reserved, sizeof(reserved));
        std::memcpy(header + 8, &payloadSize, sizeof(payloadSize));

        if (!WriteAllBytes(hTemp, header, sizeof(header)) ||
            !WriteAllBytes(hTemp, outGuard.blob.pbData, payloadSize) ||
            !FlushFileBuffers(hTemp)) {
            const DWORD writeErr = GetLastError();
            LOG_ERROR("TokenStorage", MsgKey::LogStoreWriteTempFailed,
                      writeErr, MaskPath(PathToUtf8(tempPath)));
            CloseHandle(hTemp.handle);
            hTemp.handle = INVALID_HANDLE_VALUE;
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    bool replaced = false;
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (ReplaceFileW(filePath.c_str(), tempPath.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            replaced = true;
            break;
        }
        lastErr = GetLastError();
        if (lastErr == ERROR_FILE_NOT_FOUND) {
            if (MoveFileExW(tempPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                replaced = true;
            } else {
                lastErr = GetLastError();
                LOG_ERROR("TokenStorage", MsgKey::LogStoreInitMoveFailed, lastErr);
            }
            break;
        }
        if (lastErr == ERROR_SHARING_VIOLATION ||
            lastErr == ERROR_LOCK_VIOLATION ||
            lastErr == ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
            Sleep(static_cast<DWORD>(15U << attempt));
            continue;
        }
        break;
    }

    if (!replaced) {
        if (lastErr != ERROR_FILE_NOT_FOUND) {
            LOG_ERROR("TokenStorage", MsgKey::LogStoreAtomicReplaceFailed, lastErr);
        }
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    LOG_DEBUG("TokenStorage", MsgKey::LogStorePersistedAccounts, accounts.size());
    return true;
}

bool TokenStorage::UpsertAccount(const CachedAccount& account) {
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return (account.steamId != 0 && a.steamId == account.steamId) ||
               (!account.accountName.empty() && EqualIgnoreCase(a.accountName, account.accountName));
    });

    if (it != accounts.end()) {
        if (!account.accountName.empty()) {
            it->accountName = account.accountName;
        }
        if (account.steamId != 0) it->steamId = account.steamId;
        if (!account.refreshToken.empty()) {
            WipeString(it->refreshToken);
            it->refreshToken = account.refreshToken;
        }
        if (!account.accessToken.empty()) {
            WipeString(it->accessToken);
            it->accessToken = account.accessToken;
        }
        if (account.lastLoginTime > 0) it->lastLoginTime = account.lastLoginTime;
        // Merge policy for alias:
        // Ordinary logins pass empty alias, so NEVER overwrite an existing user-configured alias!
        if (!account.alias.empty()) {
            it->alias = SanitizeAlias(account.alias);
        }
        it->isInvalid = account.isInvalid;
    } else {
        CachedAccount sanitized = account;
        sanitized.alias = SanitizeAlias(account.alias);
        accounts.push_back(std::move(sanitized));
    }

    return SaveAccounts(accounts);
}

bool TokenStorage::UpdateAccountAlias(uint64_t steamId, std::string_view newAlias) {
    if (steamId == 0) return false;
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.steamId == steamId;
    });

    if (it == accounts.end()) {
        return false;
    }

    it->alias = SanitizeAlias(newAlias);
    return SaveAccounts(accounts);
}

bool TokenStorage::UpdateAccountAlias(std::string_view accountName, std::string_view newAlias) {
    if (accountName.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return EqualIgnoreCase(a.accountName, accountName);
    });

    if (it == accounts.end()) {
        return false;
    }

    it->alias = SanitizeAlias(newAlias);
    return SaveAccounts(accounts);
}

bool TokenStorage::MarkAccountInvalid(uint64_t steamId, bool isInvalid) {
    if (steamId == 0) return false;
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.steamId == steamId;
    });

    if (it == accounts.end()) {
        return false;
    }

    if (it->isInvalid == isInvalid) {
        return true;
    }

    it->isInvalid = isInvalid;
    return SaveAccounts(accounts);
}

bool TokenStorage::MarkAccountInvalid(std::string_view accountName, bool isInvalid) {
    if (accountName.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return EqualIgnoreCase(a.accountName, accountName);
    });

    if (it == accounts.end()) {
        return false;
    }

    if (it->isInvalid == isInvalid) {
        return true;
    }

    it->isInvalid = isInvalid;
    return SaveAccounts(accounts);
}

std::string TokenStorage::SanitizeAlias(std::string_view raw) {
    if (raw.empty()) return {};

    std::string clean;
    clean.reserve(raw.size());
    for (char c : raw) {
        if (static_cast<unsigned char>(c) < 32 || c == 127 || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        clean.push_back(c);
    }

    size_t start = 0;
    while (start < clean.size() && clean[start] == ' ') {
        ++start;
    }
    size_t end = clean.size();
    while (end > start && clean[end - 1] == ' ') {
        --end;
    }
    if (start >= end) {
        return {};
    }

    std::string trimmed = clean.substr(start, end - start);

    if (trimmed.size() > 64) {
        trimmed.resize(64);
        while (!trimmed.empty()) {
            unsigned char last = static_cast<unsigned char>(trimmed.back());
            if ((last & 0x80) == 0) {
                break;
            }
            if ((last & 0xC0) == 0xC0) {
                trimmed.pop_back();
                break;
            }
            size_t seqLen = 0;
            size_t idx = trimmed.size() - 1;
            while (idx > 0 && (static_cast<unsigned char>(trimmed[idx]) & 0xC0) == 0x80) {
                --idx;
                ++seqLen;
            }
            unsigned char lead = static_cast<unsigned char>(trimmed[idx]);
            size_t needed = 0;
            if ((lead & 0xE0) == 0xC0) needed = 1;
            else if ((lead & 0xF0) == 0xE0) needed = 2;
            else if ((lead & 0xF8) == 0xF0) needed = 3;
            if (needed > 0 && seqLen >= needed) {
                break;
            } else {
                trimmed.erase(idx);
                break;
            }
        }
        while (!trimmed.empty() && trimmed.back() == ' ') {
            trimmed.pop_back();
        }
    }

    return trimmed;
}

bool TokenStorage::DeleteAccount(uint64_t steamId) {
    if (steamId == 0) return false;
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return a.steamId == steamId;
    });

    if (it == accounts.end()) {
        return false;
    }

    WipeAccountTokens(*it);
    accounts.erase(it);

    LOG_INFO("TokenStorage", MsgKey::LogStoreDeletedSteamId, MaskSteamId(steamId));
    return SaveAccounts(accounts);
}

bool TokenStorage::DeleteAccount(std::string_view accountName) {
    if (accountName.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    auto accounts = LoadAccounts();
    ScopedAccountsWiper wiper{accounts};

    auto it = std::find_if(accounts.begin(), accounts.end(), [&](const CachedAccount& a) {
        return EqualIgnoreCase(a.accountName, accountName);
    });

    if (it == accounts.end()) {
        return false;
    }

    WipeAccountTokens(*it);
    accounts.erase(it);

    LOG_INFO("TokenStorage", MsgKey::LogStoreDeletedAccount, MaskAccount(accountName));
    return SaveAccounts(accounts);
}

bool TokenStorage::WipeAll() {
    std::lock_guard<std::recursive_mutex> lock(g_storageMutex);

    const auto filePath = GetStorageFilePath();
    std::error_code ec;
    if (std::filesystem::exists(filePath, ec)) {
        SecureWipeFile(filePath);
        LOG_INFO("TokenStorage", MsgKey::LogStoreWipedAll);
    }
    const auto legacyTempPath = std::filesystem::path(filePath.wstring() + L".tmp");
    if (std::filesystem::exists(legacyTempPath, ec)) {
        SecureWipeFile(legacyTempPath);
    }
    const auto pidTempPath = std::filesystem::path(
        std::format(L"{}.{}.tmp", filePath.native(), GetCurrentProcessId()));
    if (std::filesystem::exists(pidTempPath, ec)) {
        SecureWipeFile(pidTempPath);
    }
    GetQuarantineStore().Clear();
    return true;
}

} // namespace OST::ExtractTickets
