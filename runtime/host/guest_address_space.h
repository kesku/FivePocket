#pragma once

#include <cstddef>
#include <cstdint>

namespace recomp::bootstrap {

// XenonRecomp addresses guest memory as base + uint32_t guest_address. The
// runtime therefore reserves the complete 32-bit range as one contiguous host
// mapping. Anonymous pages remain physically uncommitted until touched.
class GuestAddressSpace final {
public:
    static constexpr std::uint64_t Size = std::uint64_t{1} << 32U;

    GuestAddressSpace() = default;
    ~GuestAddressSpace();

    GuestAddressSpace(const GuestAddressSpace&) = delete;
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;

    bool reserve() noexcept;
    // Fresh-space opt-in: the A0000000 and C0000000 physical views share the
    // same512MiB backing. Needed by original823BDC30's uncached shader copy.
    // No E-view/4KiB-bias or complete Xbox memory/protection emulation claim.
    // Refuses an already reserved space; never remaps populated guest data.
    bool reserveWithPhysicalAcAlias() noexcept;
    void release() noexcept;

    [[nodiscard]] bool isReserved() const noexcept;
    [[nodiscard]] std::uint8_t* base() const noexcept;
    [[nodiscard]] std::uint8_t* translate(std::uint32_t guestAddress) const noexcept;

private:
    std::uint8_t* base_{nullptr};
};

} // namespace recomp::bootstrap
