#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Compile-time and run-time 32-bit FNV-1a hash algorithm.
// Standard 32-bit FNV-1a parameters:
//   Offset basis: 0x811c9dc5 (2166136261)
//   Prime:        0x01000193 (16777619)
//
// Invariants:
//   Output MUST be bit-exact with the original Steam client IPC hashing
//   expectations, NetPacket job routing, and remote pattern matching signatures.

namespace Detail {
    inline constexpr uint32_t kFnv1a32OffsetBasis = 0x811c9dc5u;
    inline constexpr uint32_t kFnv1a32Prime       = 0x01000193u;
}

// 1. std::span<const uint8_t> overload for raw byte sequences
[[nodiscard]] constexpr uint32_t Fnv1aHash(std::span<const uint8_t> bytes) noexcept {
    uint32_t hash = Detail::kFnv1a32OffsetBasis;
    for (uint8_t byte : bytes) {
        hash ^= static_cast<uint32_t>(byte);
        hash *= Detail::kFnv1a32Prime;
    }
    return hash;
}

// 2. std::span<const std::byte> overload for std::byte sequences
[[nodiscard]] constexpr uint32_t Fnv1aHash(std::span<const std::byte> bytes) noexcept {
    uint32_t hash = Detail::kFnv1a32OffsetBasis;
    for (std::byte b : bytes) {
        hash ^= static_cast<uint32_t>(static_cast<uint8_t>(b));
        hash *= Detail::kFnv1a32Prime;
    }
    return hash;
}

// 3. std::string_view overload for text, std::string and string slices
[[nodiscard]] constexpr uint32_t Fnv1aHash(std::string_view str) noexcept {
    uint32_t hash = Detail::kFnv1a32OffsetBasis;
    for (char c : str) {
        hash ^= static_cast<uint32_t>(static_cast<uint8_t>(c));
        hash *= Detail::kFnv1a32Prime;
    }
    return hash;
}

// 4. const char* overload: defensive against nullptr (returns 0u on nullptr)
[[nodiscard]] constexpr uint32_t Fnv1aHash(const char* str) noexcept {
    if (str == nullptr) {
        return 0u;
    }
    uint32_t hash = Detail::kFnv1a32OffsetBasis;
    while (*str != '\0') {
        hash ^= static_cast<uint32_t>(static_cast<uint8_t>(*str++));
        hash *= Detail::kFnv1a32Prime;
    }
    return hash;
}

// Compile-time formal verification assertions (zero runtime cost)
static_assert(Fnv1aHash("") == Detail::kFnv1a32OffsetBasis, "FNV-1a empty string must equal offset basis");
static_assert(Fnv1aHash(static_cast<const char*>(nullptr)) == 0u, "FNV-1a nullptr defensive guard failed");
static_assert(Fnv1aHash("BBuildAndAsyncSendFrame") == 0x82428E37u, "FNV-1a signature test 1 failed");
static_assert(Fnv1aHash("BuildDepotDependency") == 0xC37F2D8Eu, "FNV-1a signature test 2 failed");
static_assert(Fnv1aHash(std::string_view("FamilyGroupsClient.NotifyRunningApps#1")) == 0xd6b937d4u, "FNV-1a netpacket test failed");
