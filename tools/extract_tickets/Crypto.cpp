#include "Crypto.h"
#include "I18n.h"
#include "JsonHelper.h"
#include "Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <conio.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <limits>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace OST::ExtractTickets {

// ============================================================================
// SecureString Implementation
// ============================================================================

SecureString::SecureString() {
    m_data.reserve(128);
}

SecureString::SecureString(std::string_view str) {
    m_data.reserve((std::max)(static_cast<size_t>(128), str.size()));
    m_data.assign(str);
}

SecureString::~SecureString() noexcept {
    Clear();
}

SecureString::SecureString(const SecureString& other) {
    m_data.reserve((std::max)(static_cast<size_t>(128), other.m_data.size()));
    m_data.assign(other.m_data);
}

SecureString& SecureString::operator=(const SecureString& other) {
    if (this != &other) {
        Clear();
        m_data.reserve((std::max)(static_cast<size_t>(128), other.m_data.size()));
        m_data.assign(other.m_data);
    }
    return *this;
}

SecureString::SecureString(SecureString&& other) noexcept : m_data(std::move(other.m_data)) {
    other.Clear();
}

SecureString& SecureString::operator=(SecureString&& other) noexcept {
    if (this != &other) {
        Clear();
        m_data = std::move(other.m_data);
        other.Clear();
    }
    return *this;
}

void SecureString::Assign(std::string_view str) {
    Clear();
    m_data.reserve((std::max)(static_cast<size_t>(128), str.size()));
    m_data.assign(str);
}

void SecureString::Append(char c) {
    if (m_data.size() >= m_data.capacity()) {
        std::string next;
        next.reserve((std::max)(static_cast<size_t>(128), m_data.capacity() * 2));
        next.assign(m_data);
        Clear();
        m_data = std::move(next);
    }
    m_data.push_back(c);
}

void SecureString::PopBack() {
    if (!m_data.empty()) {
        volatile char* p = &m_data.back();
        *p = 0;
        m_data.pop_back();
    }
}

void SecureString::Clear() noexcept {
    if (m_data.capacity() > 0) {
        SecureZeroMemory(m_data.data(), m_data.capacity());
        m_data.clear();
    }
}

// ============================================================================
// Password Console Input with Mask
// ============================================================================

SecureString ReadPasswordFromConsole(const char* prompt) {
    std::string_view effectivePrompt = (prompt != nullptr) ? std::string_view(prompt) : TR(MsgKey::CliDefaultPasswordPrompt);
    if (!effectivePrompt.empty()) {
        std::cout << effectivePrompt;
        std::cout.flush();
    }

    SecureString pwd;
    while (true) {
        int ch = _getch();
        if (ch == EOF) {
            std::cout << "\n";
            break;
        }
        if (ch == '\r' || ch == '\n') {
            std::cout << "\n";
            break;
        } else if (ch == '\b' || ch == 127) { // Backspace
            if (!pwd.Empty()) {
                pwd.PopBack();
                std::cout << "\b \b";
                std::cout.flush();
            }
        } else if (ch == 0 || ch == 0xE0) {
            // Extended keys (arrows, function keys) -> consume trailing byte
            if (_getch() == EOF) {
                std::cout << "\n";
                break;
            }
        } else if (ch == 27) { // ESC
            pwd.Clear();
            std::cout << "\n";
            return pwd;
        } else if (ch == 3) { // Ctrl+C
            pwd.Clear();
            std::cout << "\n" << TR(MsgKey::CliInputInterrupted) << "\n";
            return pwd;
        } else if (ch >= 32 && ch <= 126) { // Printable characters
            pwd.Append(static_cast<char>(ch));
            std::cout << '*';
            std::cout.flush();
        }
    }
    return pwd;
}

// ============================================================================
// Hex & Base64 Helpers
// ============================================================================

std::vector<uint8_t> HexToBytes(std::string_view hex) {
    if (hex.empty() || (hex.size() % 2 != 0)) {
        return {};
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto hexCharVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int high = hexCharVal(hex[i]);
        int low = hexCharVal(hex[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        bytes.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return bytes;
}

std::string Base64Encode(std::span<const uint8_t> data) {
    if (data.empty()) return {};

    DWORD charsNeeded = 0;
    if (!CryptBinaryToStringA(
            data.data(),
            static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &charsNeeded) || charsNeeded == 0) {
        return {};
    }

    std::string result(charsNeeded, '\0');
    if (!CryptBinaryToStringA(
            data.data(),
            static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            result.data(),
            &charsNeeded)) {
        return {};
    }

    // CryptBinaryToStringA includes null-terminator in charsNeeded
    while (!result.empty() && (result.back() == '\0' || result.back() == '\r' || result.back() == '\n')) {
        result.pop_back();
    }
    return result;
}

std::vector<uint8_t> Base64Decode(std::string_view base64) {
    if (base64.empty()) return {};

    DWORD bytesNeeded = 0;
    if (!CryptStringToBinaryA(
            base64.data(),
            static_cast<DWORD>(base64.size()),
            CRYPT_STRING_BASE64,
            nullptr,
            &bytesNeeded,
            nullptr,
            nullptr) || bytesNeeded == 0) {
        return {};
    }

    std::vector<uint8_t> result(bytesNeeded);
    if (!CryptStringToBinaryA(
            base64.data(),
            static_cast<DWORD>(base64.size()),
            CRYPT_STRING_BASE64,
            result.data(),
            &bytesNeeded,
            nullptr,
            nullptr)) {
        return {};
    }
    result.resize(bytesNeeded);
    return result;
}

namespace {
    std::vector<uint8_t> DecodeJwtPayload(std::string_view jwt) {
        constexpr size_t kMaxJwtLen = 16384;
        if (jwt.empty() || jwt.size() > kMaxJwtLen) return {};

        auto firstDot = jwt.find('.');
        if (firstDot == std::string_view::npos) return {};
        auto secondDot = jwt.find('.', firstDot + 1);
        if (secondDot == std::string_view::npos || secondDot <= firstDot + 1) return {};

        std::string_view payloadB64 = jwt.substr(firstDot + 1, secondDot - firstDot - 1);
        std::string b64{payloadB64};
        for (char& c : b64) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        while (b64.size() % 4 != 0) {
            b64.push_back('=');
        }

        auto decoded = Base64Decode(b64);
        SecureWipeString(b64);
        return decoded;
    }
} // namespace

int64_t GetJwtExpiration(std::string_view jwt) {
    auto decoded = DecodeJwtPayload(jwt);
    if (decoded.empty()) return 0;

    std::string_view jsonStr(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    auto expOpt = JsonHelper::GetUInt64(jsonStr, "exp");
    SecureZeroMemory(decoded.data(), decoded.size());
    if (!expOpt.has_value() || *expOpt > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
        return 0;
    }
    return static_cast<int64_t>(*expOpt);
}

uint64_t GetJwtSteamId(std::string_view jwt) {
    auto decoded = DecodeJwtPayload(jwt);
    if (decoded.empty()) return 0;

    std::string_view jsonStr(reinterpret_cast<const char*>(decoded.data()), decoded.size());
    auto subOpt = JsonHelper::GetUInt64(jsonStr, "sub");
    SecureZeroMemory(decoded.data(), decoded.size());
    return subOpt.value_or(0);
}

// ============================================================================
// Steam RSA Encryption via Windows CNG
// ============================================================================

std::string EncryptPasswordWithRSA(
    const SecureString& password,
    std::string_view modHex,
    std::string_view expHex) {

    if (password.Empty() || modHex.empty() || expHex.empty()) {
        LOG_ERROR("Crypto", MsgKey::LogCryptoRsaEmptyParam);
        return {};
    }

    std::vector<uint8_t> modBytes = HexToBytes(modHex);
    std::vector<uint8_t> expBytes = HexToBytes(expHex);

    if (modBytes.empty() || expBytes.empty()) {
        LOG_ERROR("Crypto", MsgKey::LogCryptoRsaInvalidModExp);
        return {};
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0);
    if (status != 0 || !hAlg) {
        LOG_ERROR("Crypto", MsgKey::LogCryptoOpenAlgFailed, static_cast<uint32_t>(status));
        return {};
    }

    struct AlgGuard {
        BCRYPT_ALG_HANDLE handle{nullptr};
        explicit AlgGuard(BCRYPT_ALG_HANDLE h) noexcept : handle(h) {}
        ~AlgGuard() noexcept { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
        AlgGuard(const AlgGuard&) = delete;
        AlgGuard& operator=(const AlgGuard&) = delete;
    } algGuard{hAlg};

    // Construct BCRYPT_RSAKEY_BLOB:
    // [BCRYPT_RSAKEY_BLOB header][PublicExp][Modulus]
    const size_t blobSize = sizeof(BCRYPT_RSAKEY_BLOB) + expBytes.size() + modBytes.size();
    std::vector<uint8_t> keyBlob(blobSize);

    auto* pHeader = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(keyBlob.data());
    pHeader->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    pHeader->BitLength = static_cast<ULONG>(modBytes.size() * 8);
    pHeader->cbPublicExp = static_cast<ULONG>(expBytes.size());
    pHeader->cbModulus = static_cast<ULONG>(modBytes.size());
    pHeader->cbPrime1 = 0;
    pHeader->cbPrime2 = 0;

    uint8_t* pDest = keyBlob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    std::memcpy(pDest, expBytes.data(), expBytes.size());
    pDest += expBytes.size();
    std::memcpy(pDest, modBytes.data(), modBytes.size());

    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptImportKeyPair(
        hAlg,
        nullptr,
        BCRYPT_RSAPUBLIC_BLOB,
        &hKey,
        keyBlob.data(),
        static_cast<ULONG>(keyBlob.size()),
        0);

    if (status != 0 || !hKey) {
        LOG_ERROR("Crypto", MsgKey::LogCryptoImportKeyFailed, static_cast<uint32_t>(status));
        return {};
    }

    struct KeyGuard {
        BCRYPT_KEY_HANDLE handle{nullptr};
        explicit KeyGuard(BCRYPT_KEY_HANDLE h) noexcept : handle(h) {}
        ~KeyGuard() noexcept { if (handle) BCryptDestroyKey(handle); }
        KeyGuard(const KeyGuard&) = delete;
        KeyGuard& operator=(const KeyGuard&) = delete;
    } keyGuard{hKey};

    // Encrypt password using PKCS#1 v1.5 padding (standard for Steam web authentication)
    ULONG encryptedSize = 0;
    status = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.c_str())),
        static_cast<ULONG>(password.Size()),
        nullptr,
        nullptr,
        0,
        nullptr,
        0,
        &encryptedSize,
        BCRYPT_PAD_PKCS1);

    if (status != 0 || encryptedSize == 0) {
        LOG_ERROR("Crypto", MsgKey::LogCryptoCalcSizeFailed, static_cast<uint32_t>(status));
        return {};
    }

    std::vector<uint8_t> encryptedBytes(encryptedSize);
    ULONG actualWritten = 0;
    status = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.c_str())),
        static_cast<ULONG>(password.Size()),
        nullptr,
        nullptr,
        0,
        encryptedBytes.data(),
        encryptedSize,
        &actualWritten,
        BCRYPT_PAD_PKCS1);

    if (status != 0) {
        LOG_ERROR("Crypto", MsgKey::LogCryptoEncryptFailed, static_cast<uint32_t>(status));
        return {};
    }

    encryptedBytes.resize(actualWritten);
    LOG_DEBUG("Crypto", MsgKey::LogCryptoRsaSuccess, actualWritten);

    return Base64Encode(encryptedBytes);
}

} // namespace OST::ExtractTickets
