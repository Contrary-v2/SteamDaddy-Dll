#pragma once

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdint>
#include <utility>

// Private move-only RAII wrappers for Win32 resources owned by Windows/*.cpp.
namespace OSTPlatform::Windows {

struct NullHandlePolicy {
    static bool IsValid(HANDLE h) noexcept { return h != nullptr; }
    static void Close(HANDLE h) noexcept { ::CloseHandle(h); }
};

struct InvalidHandlePolicy {
    static bool IsValid(HANDLE h) noexcept { return h != nullptr && h != INVALID_HANDLE_VALUE; }
    static void Close(HANDLE h) noexcept { ::CloseHandle(h); }
};

template <typename Policy>
class UniqueHandleT {
public:
    UniqueHandleT() noexcept = default;
    explicit UniqueHandleT(HANDLE handle) noexcept : handle_(handle) {}

    UniqueHandleT(UniqueHandleT&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueHandleT& operator=(UniqueHandleT&& other) noexcept {
        if (this != &other) {
            Reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    UniqueHandleT(const UniqueHandleT&) = delete;
    UniqueHandleT& operator=(const UniqueHandleT&) = delete;

    ~UniqueHandleT() { Reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return Policy::IsValid(handle_); }

    HANDLE* put() noexcept {
        Reset();
        return &handle_;
    }

    void Reset(HANDLE handle = nullptr) noexcept {
        if (handle_ == handle) return;
        if (Policy::IsValid(handle_)) Policy::Close(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

using UniqueHandle = UniqueHandleT<NullHandlePolicy>;
using UniqueFileHandle = UniqueHandleT<InvalidHandlePolicy>;
using UniqueSnapshotHandle = UniqueFileHandle;

class UniqueWinHttpHandle {
public:
    UniqueWinHttpHandle() noexcept = default;
    explicit UniqueWinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}

    UniqueWinHttpHandle(UniqueWinHttpHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueWinHttpHandle& operator=(UniqueWinHttpHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    UniqueWinHttpHandle(const UniqueWinHttpHandle&) = delete;
    UniqueWinHttpHandle& operator=(const UniqueWinHttpHandle&) = delete;

    ~UniqueWinHttpHandle() { Reset(); }

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    HINTERNET* put() noexcept {
        Reset();
        return &handle_;
    }

    void Reset(HINTERNET handle = nullptr) noexcept {
        if (handle_ == handle) return;
        if (handle_) ::WinHttpCloseHandle(handle_);
        handle_ = handle;
    }

private:
    HINTERNET handle_ = nullptr;
};

class UniqueBCryptAlgHandle {
public:
    UniqueBCryptAlgHandle() noexcept = default;
    explicit UniqueBCryptAlgHandle(BCRYPT_ALG_HANDLE handle) noexcept : handle_(handle) {}

    UniqueBCryptAlgHandle(UniqueBCryptAlgHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueBCryptAlgHandle& operator=(UniqueBCryptAlgHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    UniqueBCryptAlgHandle(const UniqueBCryptAlgHandle&) = delete;
    UniqueBCryptAlgHandle& operator=(const UniqueBCryptAlgHandle&) = delete;

    ~UniqueBCryptAlgHandle() { Reset(); }

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    BCRYPT_ALG_HANDLE* put() noexcept {
        Reset();
        return &handle_;
    }

    void Reset(BCRYPT_ALG_HANDLE handle = nullptr) noexcept {
        if (handle_ == handle) return;
        if (handle_) ::BCryptCloseAlgorithmProvider(handle_, 0);
        handle_ = handle;
    }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class UniqueBCryptHashHandle {
public:
    UniqueBCryptHashHandle() noexcept = default;
    explicit UniqueBCryptHashHandle(BCRYPT_HASH_HANDLE handle) noexcept : handle_(handle) {}

    UniqueBCryptHashHandle(UniqueBCryptHashHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueBCryptHashHandle& operator=(UniqueBCryptHashHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.handle_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    UniqueBCryptHashHandle(const UniqueBCryptHashHandle&) = delete;
    UniqueBCryptHashHandle& operator=(const UniqueBCryptHashHandle&) = delete;

    ~UniqueBCryptHashHandle() { Reset(); }

    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    BCRYPT_HASH_HANDLE* put() noexcept {
        Reset();
        return &handle_;
    }

    void Reset(BCRYPT_HASH_HANDLE handle = nullptr) noexcept {
        if (handle_ == handle) return;
        if (handle_) ::BCryptDestroyHash(handle_);
        handle_ = handle;
    }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

class UniqueRegKey {
public:
    UniqueRegKey() noexcept = default;

    UniqueRegKey(UniqueRegKey&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }
    UniqueRegKey& operator=(UniqueRegKey&& other) noexcept {
        if (this != &other) {
            Reset(other.key_);
            other.key_ = nullptr;
        }
        return *this;
    }

    UniqueRegKey(const UniqueRegKey&) = delete;
    UniqueRegKey& operator=(const UniqueRegKey&) = delete;

    ~UniqueRegKey() { Reset(); }

    [[nodiscard]] HKEY get() const noexcept { return key_; }
    [[nodiscard]] explicit operator bool() const noexcept { return key_ != nullptr; }

    HKEY* put() noexcept {
        Reset();
        return &key_;
    }

    void Reset(HKEY key = nullptr) noexcept {
        if (key_ == key) return;
        if (key_) ::RegCloseKey(key_);
        key_ = key;
    }

private:
    HKEY key_ = nullptr;
};

class UniqueMapView {
public:
    UniqueMapView() noexcept = default;
    explicit UniqueMapView(const uint8_t* base) noexcept : base_(base) {}

    UniqueMapView(UniqueMapView&& other) noexcept : base_(other.base_) { other.base_ = nullptr; }
    UniqueMapView& operator=(UniqueMapView&& other) noexcept {
        if (this != &other) {
            Reset(other.base_);
            other.base_ = nullptr;
        }
        return *this;
    }

    UniqueMapView(const UniqueMapView&) = delete;
    UniqueMapView& operator=(const UniqueMapView&) = delete;

    ~UniqueMapView() { Reset(); }

    [[nodiscard]] const uint8_t* get() const noexcept { return base_; }
    [[nodiscard]] explicit operator bool() const noexcept { return base_ != nullptr; }

    void Reset(const uint8_t* base = nullptr) noexcept {
        if (base_ == base) return;
        if (base_) ::UnmapViewOfFile(base_);
        base_ = base;
    }

private:
    const uint8_t* base_ = nullptr;
};

class ScopedVirtualProtect {
public:
    ScopedVirtualProtect(void* address, size_t size, DWORD newProtect)
        : address_(address), size_(size) {
        if (address_ && size_ > 0) {
            ok_ = ::VirtualProtect(address_, size_, newProtect, &oldProtect_) != FALSE;
        }
    }

    ~ScopedVirtualProtect() {
        if (ok_) {
            DWORD ignored = 0;
            ::VirtualProtect(address_, size_, oldProtect_, &ignored);
            ::FlushInstructionCache(::GetCurrentProcess(), address_, size_);
        }
    }

    ScopedVirtualProtect(const ScopedVirtualProtect&) = delete;
    ScopedVirtualProtect& operator=(const ScopedVirtualProtect&) = delete;
    ScopedVirtualProtect(ScopedVirtualProtect&&) = delete;
    ScopedVirtualProtect& operator=(ScopedVirtualProtect&&) = delete;

    [[nodiscard]] bool IsSuccess() const noexcept { return ok_; }
    [[nodiscard]] DWORD OldProtect() const noexcept { return oldProtect_; }

private:
    void* address_ = nullptr;
    size_t size_ = 0;
    DWORD oldProtect_ = 0;
    bool ok_ = false;
};

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() {
        if (active_) fn_();
    }

    void Dismiss() noexcept { active_ = false; }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit(Fn) -> ScopeExit<Fn>;

} // namespace OSTPlatform::Windows
