#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OSTPlatform::PE {

// Owning byte buffer for bulk file reads. Unlike std::vector<uint8_t>, sizing
// it does NOT initialize the storage — it uses std::make_unique_for_overwrite,
// which is a single uninitialized array allocation (one operator new[]). So a
// buffer about to be fully overwritten by a file read pays neither a zero-fill
// nor a per-element construct loop. Move-only; converts to a const span for
// scanning/parsing.
class ByteBuffer {
public:
    ByteBuffer() = default;
    explicit ByteBuffer(size_t size)
        : data_(size ? std::make_unique_for_overwrite<uint8_t[]>(size) : nullptr),
          size_(size) {}

    [[nodiscard]] uint8_t* data() noexcept { return data_.get(); }
    [[nodiscard]] const uint8_t* data() const noexcept { return data_.get(); }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] std::span<uint8_t> span() noexcept { return {data_.get(), size_}; }
    [[nodiscard]] std::span<const uint8_t> span() const noexcept { return {data_.get(), size_}; }
    [[nodiscard]] std::span<const uint8_t> const_span() const noexcept { return {data_.get(), size_}; }

    operator std::span<const uint8_t>() const noexcept { return {data_.get(), size_}; }
    operator std::span<uint8_t>() noexcept { return {data_.get(), size_}; }

private:
    std::unique_ptr<uint8_t[]> data_;
    size_t size_ = 0;
};

struct Section {
    std::string name;
    uint32_t virtualAddress = 0;
    uint32_t virtualSize = 0;
    uint32_t rawOffset = 0;
    uint32_t rawSize = 0;
    uint32_t characteristics = 0;  // IMAGE_SECTION_HEADER::Characteristics

    [[nodiscard]] bool ContainsRva(uint32_t rva) const noexcept;
    // IMAGE_SCN_MEM_EXECUTE / IMAGE_SCN_MEM_WRITE — a section that is both is a
    // W^X violation (self-modifying code), the hallmark of a runtime-decrypting
    // protector.
    [[nodiscard]] bool IsExecutable() const noexcept { return (characteristics & 0x20000000u) != 0; }
    [[nodiscard]] bool IsWritable() const noexcept { return (characteristics & 0x80000000u) != 0; }
};

struct Export {
    uint32_t rva = 0;
    std::string forwarder;

    [[nodiscard]] bool IsForwarder() const noexcept { return !forwarder.empty(); }
};

class Image {
public:
    explicit Image(const std::filesystem::path& path);

    [[nodiscard]] explicit operator bool() const noexcept { return valid_; }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }
    [[nodiscard]] uint64_t FileSize() const noexcept { return fileSize_; }
    [[nodiscard]] uint32_t EntryPointRva() const noexcept { return entryPointRva_; }
    [[nodiscard]] std::span<const Section> Sections() const noexcept { return sections_; }

    [[nodiscard]] bool HasSection(std::string_view name) const;
    [[nodiscard]] const Section* SectionContainingRva(uint32_t rva) const;
    [[nodiscard]] std::optional<size_t> RvaToOffset(uint32_t rva) const;
    [[nodiscard]] std::optional<uint32_t> RawOffsetToRva(size_t rawOffset) const;
    [[nodiscard]] ByteBuffer ReadRawBytes(size_t offset, size_t size) const;
    [[nodiscard]] ByteBuffer ReadAllBytes() const;
    [[nodiscard]] std::optional<Export> FindExport(std::string_view symbolName) const;

private:
    std::filesystem::path path_;
    ByteBuffer headerBytes_;
    std::vector<Section> sections_;
    uint64_t fileSize_ = 0;
    uint32_t entryPointRva_ = 0;
    uint32_t exportDirectoryRva_ = 0;
    uint32_t exportDirectorySize_ = 0;
    bool valid_ = false;
};

} // namespace OSTPlatform::PE
