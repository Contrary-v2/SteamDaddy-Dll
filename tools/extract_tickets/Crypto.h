#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

// ============================================================================
// RAII Secure String (Wiped on destruction with SecureZeroMemory)
// ============================================================================
class SecureString {
public:
    SecureString();
    explicit SecureString(std::string_view str);
    ~SecureString() noexcept;

    SecureString(const SecureString& other);
    SecureString& operator=(const SecureString& other);
    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    void Assign(std::string_view str);
    void Append(char c);
    void PopBack();
    void Clear() noexcept;

    [[nodiscard]] const std::string& Data() const noexcept { return m_data; }
    [[nodiscard]] const char* c_str() const noexcept { return m_data.c_str(); }
    [[nodiscard]] size_t Size() const noexcept { return m_data.size(); }
    [[nodiscard]] bool Empty() const noexcept { return m_data.empty(); }

private:
    std::string m_data;
};

// ============================================================================
// Windows CNG (BCrypt) & Crypto Utilities
// ============================================================================

// Reads password from console using _getch() with '*' mask or invisible
[[nodiscard]] SecureString ReadPasswordFromConsole(const char* prompt = nullptr);

// Converts hex string to byte buffer
[[nodiscard]] std::vector<uint8_t> HexToBytes(std::string_view hex);

// Encodes binary data to standard Base64 string (no newlines)
[[nodiscard]] std::string Base64Encode(std::span<const uint8_t> data);

// Decodes standard Base64 string to binary data
[[nodiscard]] std::vector<uint8_t> Base64Decode(std::string_view base64);

// Securely zeroes full allocated capacity of a std::string and clears its length
inline void SecureWipeString(std::string& s) noexcept {
    if (s.capacity() > 0 && s.data() != nullptr) {
        volatile char* p = s.data();
        const size_t cap = s.capacity();
        for (size_t i = 0; i < cap; ++i) {
            p[i] = '\0';
        }
        s.clear();
    }
}

// Securely zeroes full allocated capacity of a std::vector<uint8_t> and clears its length
inline void SecureWipeBuffer(std::vector<uint8_t>& buf) noexcept {
    if (buf.capacity() > 0 && buf.data() != nullptr) {
        volatile uint8_t* p = buf.data();
        const size_t cap = buf.capacity();
        for (size_t i = 0; i < cap; ++i) {
            p[i] = 0;
        }
        buf.clear();
    }
}

struct ScopedBufferWiper {
    std::vector<uint8_t>& buf;
    explicit ScopedBufferWiper(std::vector<uint8_t>& b) noexcept : buf(b) {}
    ~ScopedBufferWiper() noexcept { SecureWipeBuffer(buf); }
    ScopedBufferWiper(const ScopedBufferWiper&) = delete;
    ScopedBufferWiper& operator=(const ScopedBufferWiper&) = delete;
};

// Parses expiration unix timestamp from Steam OAuth JWT access token (0 if missing/invalid)
[[nodiscard]] int64_t GetJwtExpiration(std::string_view jwt);

// Parses 64-bit SteamID ("sub" claim) from Steam OAuth JWT token (0 if missing/invalid)
[[nodiscard]] uint64_t GetJwtSteamId(std::string_view jwt);

// Encrypts password using Steam's RSA public key (modulus & exponent)
// Returns Base64-encoded encrypted password ready for BeginAuthSessionViaCredentials
[[nodiscard]] std::string EncryptPasswordWithRSA(
    const SecureString& password,
    std::string_view modHex,
    std::string_view expHex);

} // namespace OST::ExtractTickets
