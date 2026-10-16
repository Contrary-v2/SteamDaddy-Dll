#include "include/Hash.h"

#include "Handles.h"
#include "include/Encoding.h"
#include "include/Log.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <vector>

namespace OSTPlatform::Hash {

namespace {

uint32_t StatusCode(NTSTATUS status) {
    return static_cast<uint32_t>(status);
}

class Sha256Context {
public:
    bool Init() {
        NTSTATUS status = BCryptOpenAlgorithmProvider(alg_.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(status)) {
            OSTP_LOG_WARN("Sha256Context: BCryptOpenAlgorithmProvider failed (status=0x{:08X})",
                          StatusCode(status));
            return false;
        }

        DWORD cbData = 0;
        DWORD hashObjSize = 0;
        status = BCryptGetProperty(alg_.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjSize),
                                   sizeof(DWORD), &cbData, 0);
        if (!BCRYPT_SUCCESS(status) || hashObjSize == 0) {
            OSTP_LOG_WARN("Sha256Context: BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed (status=0x{:08X}, size={})",
                          StatusCode(status), hashObjSize);
            return false;
        }
        hashObj_.resize(hashObjSize);

        status = BCryptGetProperty(alg_.get(), BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize_),
                                   sizeof(DWORD), &cbData, 0);
        if (!BCRYPT_SUCCESS(status) || hashSize_ == 0) {
            OSTP_LOG_WARN("Sha256Context: BCryptGetProperty(BCRYPT_HASH_LENGTH) failed (status=0x{:08X}, size={})",
                          StatusCode(status), hashSize_);
            return false;
        }

        status = BCryptCreateHash(alg_.get(), hash_.put(), hashObj_.data(), hashObjSize, nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(status)) {
            OSTP_LOG_WARN("Sha256Context: BCryptCreateHash failed (status=0x{:08X})", StatusCode(status));
            return false;
        }

        return true;
    }

    bool Update(std::span<const uint8_t> chunk) {
        if (!hash_) return false;
        if (chunk.empty()) return true;
        const NTSTATUS status = BCryptHashData(
            hash_.get(),
            const_cast<PUCHAR>(chunk.data()),
            static_cast<ULONG>(chunk.size()),
            0);
        if (!BCRYPT_SUCCESS(status)) {
            OSTP_LOG_WARN("Sha256Context: BCryptHashData failed (status=0x{:08X})", StatusCode(status));
            return false;
        }
        return true;
    }

    std::string Finalize() {
        if (!hash_ || hashSize_ == 0) return {};
        std::vector<uint8_t> hashBuf(hashSize_);
        const NTSTATUS status = BCryptFinishHash(hash_.get(), hashBuf.data(), hashSize_, 0);
        if (!BCRYPT_SUCCESS(status)) {
            OSTP_LOG_WARN("Sha256Context: BCryptFinishHash failed (status=0x{:08X})", StatusCode(status));
            return {};
        }

        static constexpr char kHex[] = "0123456789abcdef";
        std::string result;
        result.reserve(hashSize_ * 2);
        for (uint8_t b : hashBuf) {
            result += kHex[b >> 4];
            result += kHex[b & 0xF];
        }
        return result;
    }

private:
    Windows::UniqueBCryptAlgHandle alg_;
    Windows::UniqueBCryptHashHandle hash_;
    std::vector<uint8_t> hashObj_;
    DWORD hashSize_ = 0;
};

} // namespace

std::string Sha256OfFile(const std::filesystem::path& path) {
    Windows::UniqueFileHandle hFile(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!hFile) {
        OSTP_LOG_WARN("Sha256OfFile: CreateFileW failed for '{}' (error={})",
                      Encoding::PathToUtf8(path), GetLastError());
        return {};
    }

    Sha256Context ctx;
    if (!ctx.Init()) {
        return {};
    }

    constexpr DWORD kChunk = 65536;
    std::vector<uint8_t> buf(kChunk);
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(hFile.get(), buf.data(), kChunk, &bytesRead, nullptr)) {
            OSTP_LOG_WARN("Sha256OfFile: ReadFile failed for '{}' (error={})",
                          Encoding::PathToUtf8(path), GetLastError());
            return {};
        }
        if (bytesRead == 0) break;
        if (!ctx.Update(std::span<const uint8_t>(buf.data(), bytesRead))) {
            return {};
        }
    }

    return ctx.Finalize();
}

std::string Sha256(std::span<const uint8_t> data) {
    Sha256Context ctx;
    if (!ctx.Init()) {
        return {};
    }
    if (!ctx.Update(data)) {
        return {};
    }
    return ctx.Finalize();
}

} // namespace OSTPlatform::Hash
