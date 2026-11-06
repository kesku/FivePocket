#pragma once
#include "guest_address_space.h"
#include <array>
#include <cstdint>
#include <vector>

namespace recomp::bootstrap {
// Two exported pointer VARIABLES, not device objects or renderer readiness.
// Caller supplies parser-confirmed xboxkrnl 0x1BE/0x1BF data-import slots and
// dedicated zero scratch storage. Space must outlive this reversible binding.
class GuestVideoDeviceGlobals final {
public:
    struct Range { std::uint32_t base; std::uint64_t size; };
    struct Layout { std::uint32_t imageBase, imageSize, deviceSlot, xamDeviceSlot, storage; };
    ~GuestVideoDeviceGlobals() { reset(); }
    GuestVideoDeviceGlobals() = default;
    GuestVideoDeviceGlobals(const GuestVideoDeviceGlobals&) = delete;
    GuestVideoDeviceGlobals& operator=(const GuestVideoDeviceGlobals&) = delete;
    bool initialize(GuestAddressSpace& space, Layout layout, const std::vector<Range>& excluded) {
        if (space_ || !space.isReserved() || !layout.imageSize || !layout.storage ||
            ((layout.deviceSlot | layout.xamDeviceSlot | layout.storage) & 3) ||
            layout.deviceSlot == layout.xamDeviceSlot) return false;
        const Range image{layout.imageBase, layout.imageSize}, storage{layout.storage, 8};
        if (!fits(image) || !fits(storage) || overlaps(image, storage)) return false;
        for (auto range : excluded) if (!fits(range) || overlaps(range, storage)) return false;
        const std::array slots{layout.deviceSlot, layout.xamDeviceSlot};
        const std::array ordinals{0x1BEu, 0x1BFu};
        for (unsigned i = 0; i < 2; ++i) {
            if (slots[i] < image.base || std::uint64_t{slots[i]} + 4 > std::uint64_t{image.base} + image.size) return false;
            const auto word = read(space, slots[i]);
            const auto swapped = (word >> 24) | ((word >> 8) & 0xFF00) |
                ((word << 8) & 0xFF0000) | (word << 24);
            if ((word & 0xFF00FFFF) != ordinals[i] && (swapped & 0xFF00FFFF) != ordinals[i]) return false;
        }
        if (read(space, layout.storage) || read(space, layout.storage + 4)) return false;
        for (unsigned i = 0; i < 2; ++i) originals_[i] = read(space, slots[i]);
        // All validation precedes the first mutation.
        for (unsigned i = 0; i < 2; ++i) write(space, slots[i], layout.storage + i * 4);
        layout_ = layout; space_ = &space;
        return true;
    }
    void reset() noexcept {
        if (!space_) return;
        write(*space_, layout_.deviceSlot, originals_[0]);
        write(*space_, layout_.xamDeviceSlot, originals_[1]);
        write(*space_, layout_.storage, 0); write(*space_, layout_.storage + 4, 0);
        space_ = nullptr;
    }
    std::uint32_t device() const noexcept { return space_ ? read(*space_, layout_.storage) : 0; }
    bool matchesDevice(std::uint32_t expected) const noexcept {
        return space_ && expected && read(*space_,layout_.deviceSlot)==layout_.storage &&
            read(*space_,layout_.xamDeviceSlot)==layout_.storage+4 && read(*space_,layout_.storage)==expected;
    }
    std::uint32_t xamDevice() const noexcept { return space_ ? read(*space_, layout_.storage + 4) : 0; }
private:
    static bool fits(Range range) noexcept {
        return range.size && range.size <= GuestAddressSpace::Size &&
            std::uint64_t{range.base} <= GuestAddressSpace::Size - range.size;
    }
    static bool overlaps(Range a, Range b) noexcept {
        return a.base < std::uint64_t{b.base} + b.size && b.base < std::uint64_t{a.base} + a.size;
    }
    static std::uint32_t read(const GuestAddressSpace& space, std::uint32_t address) noexcept {
        const auto* b = space.translate(address);
        return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
    }
    static void write(GuestAddressSpace& space, std::uint32_t address, std::uint32_t value) noexcept {
        auto* b = space.translate(address);
        for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
    }
    GuestAddressSpace* space_{nullptr};
    Layout layout_{};
    std::array<std::uint32_t, 2> originals_{};
};
}
