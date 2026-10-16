#include "include/DynamicLibrary.h"

#include "include/Encoding.h"
#include "include/Log.h"

#include <windows.h>

#include <array>
#include <vector>

namespace OSTPlatform::DynamicLibrary {
namespace {

constexpr DWORD kMaxWin32PathBuffer = 32768;

std::string WidePathToUtf8(std::wstring_view path, const char* operation) {
    std::string result = Encoding::WideToUtf8(path);
    if (result.empty() && !path.empty()) {
        OSTP_LOG_DEBUG("{}: failed to convert path to UTF-8", operation);
    }
    return result;
}

std::filesystem::path ModuleFilePath(HMODULE module, const char* operation) {
    wchar_t stackBuf[MAX_PATH];
    ::SetLastError(ERROR_SUCCESS);
    DWORD len = ::GetModuleFileNameW(module, stackBuf, static_cast<DWORD>(std::size(stackBuf)));
    if (len == 0) {
        OSTP_LOG_WARN("{}: GetModuleFileNameW failed (error={})", operation, ::GetLastError());
        return {};
    }
    if (len < static_cast<DWORD>(std::size(stackBuf))) {
        return std::filesystem::path(std::wstring_view(stackBuf, len));
    }

    for (DWORD capacity = MAX_PATH * 2; capacity <= kMaxWin32PathBuffer; capacity *= 2) {
        std::vector<wchar_t> buffer(capacity);
        ::SetLastError(ERROR_SUCCESS);
        len = ::GetModuleFileNameW(module, buffer.data(), capacity);
        if (len == 0) {
            OSTP_LOG_WARN("{}: GetModuleFileNameW failed (error={})", operation, ::GetLastError());
            return {};
        }
        if (len < capacity) {
            return std::filesystem::path(std::wstring_view(buffer.data(), len));
        }
    }

    OSTP_LOG_WARN("{}: GetModuleFileNameW path exceeds {} wchars", operation, kMaxWin32PathBuffer);
    return {};
}

std::string DirectoryFromQuery(DWORD (*query)(DWORD, wchar_t*), const char* operation) {
    wchar_t stackBuf[MAX_PATH];
    DWORD len = query(static_cast<DWORD>(std::size(stackBuf)), stackBuf);
    if (len == 0) {
        OSTP_LOG_WARN("{} failed (error={})", operation, ::GetLastError());
        return {};
    }
    if (len < static_cast<DWORD>(std::size(stackBuf))) {
        return WidePathToUtf8(std::wstring_view(stackBuf, len), operation);
    }

    DWORD capacity = (len > MAX_PATH) ? len + 1 : MAX_PATH * 2;
    for (; capacity <= kMaxWin32PathBuffer; capacity *= 2) {
        std::vector<wchar_t> buffer(capacity);
        len = query(capacity, buffer.data());
        if (len == 0) {
            OSTP_LOG_WARN("{} failed (error={})", operation, ::GetLastError());
            return {};
        }
        if (len < capacity) {
            return WidePathToUtf8(std::wstring_view(buffer.data(), len), operation);
        }
        if (len > capacity) {
            capacity = len;
        }
    }

    OSTP_LOG_WARN("{} path exceeds {} wchars", operation, kMaxWin32PathBuffer);
    return {};
}

DWORD GetCurrentDirectoryThunk(DWORD capacity, wchar_t* buffer) {
    return ::GetCurrentDirectoryW(capacity, buffer);
}

DWORD GetSystemDirectoryThunk(DWORD capacity, wchar_t* buffer) {
    return ::GetSystemDirectoryW(buffer, capacity);
}

} // namespace

ModuleHandle Load(const std::filesystem::path& path) {
    ModuleHandle module = reinterpret_cast<ModuleHandle>(::LoadLibraryW(path.c_str()));
    if (!module) {
        const DWORD err = ::GetLastError();
        OSTP_LOG_WARN("LoadLibraryW('{}') failed (error={})", Encoding::PathToUtf8(path), err);
        ::SetLastError(err);
    }
    return module;
}

bool Unload(ModuleHandle module) {
    if (!module) return false;
    const BOOL ok = ::FreeLibrary(reinterpret_cast<HMODULE>(module));
    if (!ok) {
        const DWORD err = ::GetLastError();
        OSTP_LOG_WARN("FreeLibrary({:p}) failed (error={})", module, err);
        ::SetLastError(err);
        return false;
    }
    return true;
}

ModuleHandle GetLoaded(std::string_view moduleName) {
    const std::wstring wideName = Encoding::Utf8ToWide(moduleName);
    if (wideName.empty() && !moduleName.empty()) {
        OSTP_LOG_DEBUG("GetLoaded('{}'): module name conversion failed", moduleName);
        return nullptr;
    }
    return reinterpret_cast<ModuleHandle>(::GetModuleHandleW(wideName.empty() ? nullptr : wideName.c_str()));
}

void* GetSymbol(ModuleHandle module, const char* symbolName) {
    if (!module || !symbolName) return nullptr;
    void* symbol = reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(module), symbolName));
    if (!symbol) {
        const DWORD err = ::GetLastError();
        OSTP_LOG_DEBUG("GetProcAddress('{}') failed (error={})", symbolName, err);
        ::SetLastError(err);
    }
    return symbol;
}

void* GetSymbol(ModuleHandle module, uint16_t ordinal) {
    if (!module) return nullptr;
    void* symbol = reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(module), MAKEINTRESOURCEA(ordinal)));
    if (!symbol) {
        const DWORD err = ::GetLastError();
        OSTP_LOG_DEBUG("GetProcAddress(ordinal={}) failed (error={})", ordinal, err);
        ::SetLastError(err);
    }
    return symbol;
}

uint32_t GetLastErrorCode() noexcept {
    return ::GetLastError();
}

std::string GetCurrentDirectoryPath() {
    return DirectoryFromQuery(GetCurrentDirectoryThunk, "GetCurrentDirectoryW");
}

std::string GetSystemDirectoryPath() {
    return DirectoryFromQuery(GetSystemDirectoryThunk, "GetSystemDirectoryW");
}

std::filesystem::path GetModuleDirectory(ModuleHandle module) {
    return ModuleFilePath(reinterpret_cast<HMODULE>(module), "GetModuleDirectory").parent_path();
}

std::filesystem::path GetMainExecutablePath() {
    return ModuleFilePath(nullptr, "GetMainExecutablePath");
}

} // namespace OSTPlatform::DynamicLibrary
