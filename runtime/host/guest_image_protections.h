#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace recomp::bootstrap {

// Snapshot only: guest read/write/cache metadata, not host mprotect or native
// executable permissions. XEX security descriptors, not PE sections, define it.
// This profile supports 64-KiB title-image pages in 80000000..90000000 only.
class GuestImageProtections final {
public:
    static constexpr std::uint32_t PageBytes = 65536;
    static constexpr std::uint32_t MaximumPages = 4096;

    [[nodiscard]] bool initialize(const std::uint8_t* header, std::uint32_t headerBytes,
                                  std::uint32_t imageBase, std::uint32_t imageBytes) noexcept {
        if (!header || headerBytes < 24 || headerBytes > 16 * 1024 * 1024 ||
                imageBase < 0x80000000 || std::uint64_t{imageBase} + imageBytes > 0x90000000 ||
                !imageBytes || ((imageBase | imageBytes) & (PageBytes - 1))) return false;
        const auto word = [header](std::size_t at) {
            return (std::uint32_t{header[at]} << 24) | (std::uint32_t{header[at + 1]} << 16) |
                   (std::uint32_t{header[at + 2]} << 8) | header[at + 3];
        };
        if (word(0) != 0x58455832 || word(8) != headerBytes) return false;
        const auto security = word(16), optionalCount = word(20);
        if (optionalCount > 4096 || (security & 3) || security < 24 + optionalCount * 8 ||
                std::uint64_t{security} + 0x184 > headerBytes) return false;
        const auto securityBytes = word(security), count = word(security + 0x180);
        if (!count || count > MaximumPages || securityBytes < 0x184 + count * 24 ||
                std::uint64_t{security} + securityBytes > headerBytes ||
                word(security + 4) != imageBytes || word(security + 0x110) != imageBase) return false;
        std::array<std::uint8_t, MaximumPages> permissions{};
        const auto totalPages = imageBytes / PageBytes;
        std::uint32_t page = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            // Low nibble is type; upper 28 bits are count. Hash bytes are not
            // interpreted, copied or exposed by this metadata-only reader.
            const auto descriptor = word(security + 0x184 + i * 24);
            const auto type = descriptor & 15, pages = descriptor >> 4;
            if (type < 1 || type > 3 || !pages || pages > totalPages - page) return false;
            for (std::uint32_t p = 0; p < pages; ++p)
                permissions[page++] = type == 2 ? 4 : 2; // PAGE_READWRITE / PAGE_READONLY.
        }
        if (page != totalPages) return false;
        // Publish only after the entire table and expected image match.
        pages_ = permissions;
        base_ = imageBase;
        size_ = imageBytes;
        return true;
    }

    [[nodiscard]] bool ready() const noexcept { return size_ != 0; }
    [[nodiscard]] std::uint32_t base() const noexcept { return base_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] bool overlaps(std::uint32_t at, std::uint64_t bytes) const noexcept {
        return ready() && bytes && at < std::uint64_t{base_} + size_ && std::uint64_t{at} + bytes > base_;
    }
    [[nodiscard]] std::optional<std::uint32_t> query(std::uint32_t at) const noexcept {
        if (!ready() || at < base_ || std::uint64_t{at} >= std::uint64_t{base_} + size_) return std::nullopt;
        return pages_[(at - base_) / PageBytes];
    }
    [[nodiscard]] bool writable(std::uint32_t at, std::uint32_t bytes) const noexcept {
        if (!bytes || !query(at) || std::uint64_t{at} + bytes > std::uint64_t{base_} + size_) return false;
        for (auto page = (at - base_) / PageBytes; page <= (std::uint64_t{at} + bytes - 1 - base_) / PageBytes; ++page)
            if (pages_[page] != 4) return false;
        return true;
    }

private:
    std::uint32_t base_{0}, size_{0};
    std::array<std::uint8_t, MaximumPages> pages_{};
};

} // namespace recomp::bootstrap
