#pragma once

#include "guest_address_space.h"

#include <cstdint>
#include <string>

namespace recomp::bootstrap {

enum class GuestThreadEnvironmentState {
    Idle,
    Ready,
    InvalidRequest,
    AddressSpaceUnavailable,
    AddressRangeInvalid,
    AddressOverlap,
};

struct GuestThreadEnvironmentLayout {
    std::uint32_t pcrAddress{0};
    std::uint32_t threadAddress{0};
    std::uint32_t tlsAddress{0};
    std::uint32_t imageBase{0};
    std::uint32_t imageSize{0};
    std::uint32_t stackBase{0}; // Low address; the upper bound is exclusive.
    std::uint32_t stackSize{0};
    std::uint32_t tlsRawDataAddress{0};
    std::uint32_t tlsRawDataSize{0};
    std::uint32_t tlsDataSize{0};
    std::uint32_t tlsSlotCount{0};
    std::uint32_t threadId{1};
};

struct GuestThreadEnvironmentResult {
    GuestThreadEnvironmentState state{GuestThreadEnvironmentState::Idle};
    GuestThreadEnvironmentLayout layout{};
    std::string diagnostic{"No guest thread environment requested"};

    [[nodiscard]] bool succeeded() const noexcept {
        return state == GuestThreadEnvironmentState::Ready;
    }
};

// Initializes only the PCR, the known thread identity/stack/TLS fields, and TLS
// storage in caller-selected scratch ranges. This does not create a scheduler
// object or a host thread. The caller validates metadata against its decoded
// image and excludes other runtime ranges (heaps, headers, title structures).
// The address space must outlive this object, or reset must be called first.
class GuestThreadEnvironment final {
public:
    static constexpr std::uint32_t PcrSize = 0x2D8U;
    static constexpr std::uint32_t ThreadSize = 0x170U;
    static constexpr std::uint32_t MaxStaticTlsSize = 1024U * 1024U;
    static constexpr std::uint32_t MaxTlsSlots = 2048U;

    GuestThreadEnvironment() = default;
    GuestThreadEnvironment(const GuestThreadEnvironment&) = delete;
    GuestThreadEnvironment& operator=(const GuestThreadEnvironment&) = delete;
    GuestThreadEnvironment(GuestThreadEnvironment&&) = delete;
    GuestThreadEnvironment& operator=(GuestThreadEnvironment&&) = delete;

    // A failed request does not write to guest memory. Reinitialization while
    // owned storage remains bound returns false and preserves the ready result;
    // call reset explicitly before replacing that environment.
    [[nodiscard]] bool initialize(
            GuestAddressSpace& addressSpace,
            const GuestThreadEnvironmentLayout& layout);
    void reset();

    [[nodiscard]] const GuestThreadEnvironmentResult& result() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    // Zero when unavailable or when the layout specifies no dynamic slots.
    [[nodiscard]] std::uint32_t dynamicTlsAddress() const noexcept;
    [[nodiscard]] std::uint32_t tlsStorageSize() const noexcept;

private:
    GuestAddressSpace* addressSpace_{nullptr};
    std::uint32_t tlsStorageSize_{0};
    GuestThreadEnvironmentResult result_{};
};

[[nodiscard]] const char* guestThreadEnvironmentStateName(
        GuestThreadEnvironmentState state) noexcept;

} // namespace recomp::bootstrap
