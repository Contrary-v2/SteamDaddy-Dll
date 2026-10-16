#pragma once

// Denuvo-specific module scanner. ProcessInspector stays generic; this file
// owns only the current "is this process protected" policy.

#include "Steam/Types.h"

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>

namespace PipeManager::DenuvoAuth {

    enum class DetectionMethod {
        None,
        LegacySectionString,
        OepPattern,
        ProtectedBlobSection,
    };

    [[nodiscard]] constexpr const char* ToString(DetectionMethod method) noexcept {
        switch (method) {
        case DetectionMethod::None:                 return "None";
        case DetectionMethod::LegacySectionString:  return "LegacySectionString";
        case DetectionMethod::OepPattern:           return "OepPattern";
        case DetectionMethod::ProtectedBlobSection: return "ProtectedBlobSection";
        }
        return "Unknown";
    }

    struct ProtectionScanReport {
        bool denuvoDetected = false;
        DetectionMethod method = DetectionMethod::None;
        std::string modulePath;
        std::string sectionName;
        uint32 pid = 0;
        uint32 moduleSize = 0;
        uint32 entryPointRva = 0;
        uint32 matchRva = 0;
        size_t matchRawOffset = 0;
        size_t scannedModules = 0;
        double elapsedMs = 0.0;

        bool operator==(const ProtectionScanReport&) const = default;

        [[nodiscard]] std::string DebugString() const {
            return std::format(
                "[pid={}] denuvoDetected={} method={} module={} section={} moduleSize={} entryPointRva=0x{:X} matchRva=0x{:X} matchRaw=0x{:X} scannedModules={} elapsedMs={:.3f}",
                pid,
                denuvoDetected,
                ToString(method),
                modulePath.empty() ? "-" : modulePath,
                sectionName.empty() ? "-" : sectionName,
                moduleSize,
                entryPointRva,
                matchRva,
                matchRawOffset,
                scannedModules,
                elapsedMs);
        }
    };

    [[nodiscard]] ProtectionScanReport ScanProtection(PID_t pid);
    [[nodiscard]] bool IsDenuvoPath(const std::filesystem::path& exePath);

} // namespace PipeManager::DenuvoAuth
