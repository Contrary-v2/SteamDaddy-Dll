#pragma once

// Shared identities passed between Pipe features. Keep feature-owned state in
// each feature module so this layer stays only about "who is calling".

#include "Steam/Structs.h"
#include "Steam/Types.h"
#include "OSTPlatform/include/Process.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace PipeManager {

    // PID plus creation time lets caches survive PID reuse safely.
    struct ProcessKey {
        PID_t pid = 0;
        uint64 creationTime = 0;

        bool operator==(const ProcessKey&) const = default;
        auto operator<=>(const ProcessKey&) const = default;

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return pid != 0 && creationTime != 0;
        }

        [[nodiscard]] std::string DebugString() const {
            return std::format("pid={} creation={} filetime={}",
                               pid,
                               OSTPlatform::Process::FormatCreationTime(creationTime),
                               creationTime);
        }
    };

    // Container companion for unordered caches; ProcessKey owns the meaning.
    // Uses SplitMix64 avalanche permutation to eliminate linear collisions and sign extension bias.
    struct ProcessKeyHash {
        std::size_t operator()(const ProcessKey& key) const noexcept {
            uint64_t x = (static_cast<uint64_t>(key.pid) << 32) ^ key.creationTime;
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return static_cast<std::size_t>(x);
        }
    };

    // A process may open several Steam IPC pipes.
    struct PipeKey {
        PID_t pid = 0;
        HSteamPipe pipe = 0;

        bool operator==(const PipeKey&) const = default;
        auto operator<=>(const PipeKey&) const = default;

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return pid != 0 && pipe != 0;
        }

        [[nodiscard]] std::string DebugString() const {
            return std::format("pid={} pipe=0x{:08X}", pid, pipe);
        }
    };

    // Container companion for unordered pipe maps; PipeKey owns the meaning.
    // Packs (pid, pipe) uniquely and applies SplitMix64 avalanche mixer.
    struct PipeKeyHash {
        std::size_t operator()(const PipeKey& key) const noexcept {
            uint64_t x = (static_cast<uint64_t>(key.pid) << 32) | static_cast<uint32_t>(key.pipe);
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return static_cast<std::size_t>(x);
        }
    };

    [[nodiscard]] inline PipeKey MakePipeKey(const CPipeClient* pipe) noexcept {
        if (!pipe) return {};
        return PipeKey{pipe->m_clientPID, pipe->m_hSteamPipe};
    }

    struct PipeContext {
        CPipeClient* pipe = nullptr;
        ProcessKey process;
        AppId_t appId = k_uAppIdInvalid;
        // These flags are resolved once in PipeManager and consumed by features.
        bool gameProcess = false;
        bool trackedApp = false;

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return pipe != nullptr && process.IsValid();
        }
    };

} // namespace PipeManager
