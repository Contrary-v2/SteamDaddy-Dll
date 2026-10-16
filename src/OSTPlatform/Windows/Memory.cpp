#include "include/Memory.h"

#include "include/Log.h"
#include "Windows/Handles.h"

#include <windows.h>
#include <psapi.h>

#include <cstring>

namespace OSTPlatform::Memory {

std::optional<ModuleImage> GetModuleImage(DynamicLibrary::ModuleHandle module) {
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(module), &info, sizeof(info))) {
        OSTP_LOG_DEBUG("GetModuleImage(module={}) failed (error={})", module, GetLastError());
        return std::nullopt;
    }

    if (!info.lpBaseOfDll || info.SizeOfImage == 0) {
        return std::nullopt;
    }

    return ModuleImage{
        static_cast<uint8_t*>(info.lpBaseOfDll),
        static_cast<size_t>(info.SizeOfImage),
    };
}

namespace {

bool TryFindModuleSection(
    uint8_t* base,
    const char* targetName,
    size_t targetLen,
    uint32_t* outRva,
    uint32_t* outSize)
{
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        const WORD numSections = nt->FileHeader.NumberOfSections;

        for (WORD i = 0; i < numSections; ++i, ++section) {
            char name[9] = {};
            std::memcpy(name, section->Name, 8);
            if (std::strncmp(name, targetName, targetLen) == 0 && name[targetLen] == '\0') {
                *outRva = section->VirtualAddress;
                *outSize = section->Misc.VirtualSize;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return false;
}

} // namespace

std::optional<ModuleSection> GetModuleSection(DynamicLibrary::ModuleHandle module, std::string_view sectionName) {
    if (!module || sectionName.empty() || sectionName.size() > 8) return std::nullopt;

    auto* base = reinterpret_cast<uint8_t*>(module);
    uint32_t rva = 0;
    uint32_t size = 0;
    if (!TryFindModuleSection(base, sectionName.data(), sectionName.size(), &rva, &size)) {
        OSTP_LOG_DEBUG("GetModuleSection: section '{}' not found or inaccessible (module={:p})",
                       sectionName, module);
        return std::nullopt;
    }

    return ModuleSection{
        base + rva,
        static_cast<size_t>(size),
    };
}

bool WriteExecutableBytes(void* target, std::span<const uint8_t> bytes) {
    if (!target || bytes.empty()) {
        if (!target) OSTP_LOG_WARN("WriteExecutableBytes: target is null");
        return false;
    }

    Windows::ScopedVirtualProtect guard(target, bytes.size(), PAGE_EXECUTE_READWRITE);
    if (!guard.IsSuccess()) {
        OSTP_LOG_WARN("WriteExecutableBytes(target={:p}, size={}) VirtualProtect(RWX) failed (error={})",
                      target, bytes.size(), GetLastError());
        return false;
    }

    std::memcpy(target, bytes.data(), bytes.size());
    return true;
}

bool WriteExecutableByte(void* target, uint8_t value) {
    return WriteExecutableBytes(target, {&value, 1});
}

} // namespace OSTPlatform::Memory
