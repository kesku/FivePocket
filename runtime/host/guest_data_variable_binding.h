#pragma once
#include "guest_address_space.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::bootstrap {
// Reversible relocation of one parser-confirmed kernel DATA export. Payload
// semantics belong to the explicit host service/profile, not this memory helper.
// Bind before workers start; reset after they join. Space must outlive binding.
class GuestDataVariableBinding final {
public:
    struct Range { std::uint32_t base; std::uint64_t size; };
    ~GuestDataVariableBinding() { reset(); }
    GuestDataVariableBinding() = default;
    GuestDataVariableBinding(const GuestDataVariableBinding&) = delete;
    GuestDataVariableBinding& operator=(const GuestDataVariableBinding&) = delete;
    bool initialize(GuestAddressSpace& space, Range image, std::uint32_t slot,
            std::uint32_t ordinal, std::uint32_t storage, const std::vector<std::uint8_t>& payload,
            const std::vector<Range>& excluded) {
        if (space_ || !space.isReserved() || !ordinal || ordinal > 0xFFFF || !slot || !storage ||
            ((slot | storage) & 3) || payload.empty() || payload.size() > 64 || !fits(image)) return false;
        const Range destination{storage, payload.size()};
        if (!fits(destination) || overlaps(image, destination) || slot < image.base ||
            std::uint64_t{slot} + 4 > std::uint64_t{image.base} + image.size) return false;
        for (auto range : excluded) if (!fits(range) || overlaps(range, destination)) return false;
        const auto* bytes = space.translate(slot);
        const auto word = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
            (std::uint32_t{bytes[2]} << 8) | bytes[3];
        const auto swapped = (word >> 24) | ((word >> 8) & 0xFF00) | ((word << 8) & 0xFF0000) | (word << 24);
        if ((word & 0xFF00FFFF) != ordinal && (swapped & 0xFF00FFFF) != ordinal) return false;
        for (std::size_t i = 0; i < payload.size(); ++i) if (space.translate(storage)[i]) return false;
        // Nothing is published until all checks pass.
        original_ = word; slot_ = slot; storage_ = storage; size_ = payload.size();
        std::memcpy(space.translate(storage), payload.data(), size_);
        write(space, slot, storage); space_ = &space;
        return true;
    }
    void reset() noexcept {
        if (!space_) return;
        write(*space_, slot_, original_);
        std::memset(space_->translate(storage_), 0, size_); space_ = nullptr;
    }
private:
    static bool fits(Range range) noexcept {
        return range.size && range.size <= GuestAddressSpace::Size &&
            std::uint64_t{range.base} <= GuestAddressSpace::Size - range.size;
    }
    static bool overlaps(Range a, Range b) noexcept {
        return a.base < std::uint64_t{b.base} + b.size && b.base < std::uint64_t{a.base} + a.size;
    }
    static void write(GuestAddressSpace& space, std::uint32_t address, std::uint32_t value) noexcept {
        auto* b = space.translate(address);
        for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
    }
    GuestAddressSpace* space_{nullptr};
    std::uint32_t slot_{0}, storage_{0}, original_{0};
    std::size_t size_{0};
};
}
