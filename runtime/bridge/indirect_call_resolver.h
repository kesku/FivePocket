#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>

struct PPCContext;

namespace recomp::bootstrap {

constexpr std::size_t MaximumGuestFunctionMappings = 1'000'000;

// Validates the private generated mapping once before guest execution. The
// expected count comes from the separately validated source and excludes the
// terminal {0, nullptr} sentinel.
[[nodiscard]] bool initializeGuestIndirectCallResolver(
        std::size_t expectedMappingCount,
        std::size_t& validatedMappingCount) noexcept;
void resetGuestIndirectCallResolver() noexcept;
[[nodiscard]] bool hasExactGuestFunction(std::uint32_t guestTarget) noexcept;
struct InvalidGuestIndirectCall {
    std::uint32_t target, linkRegister, stackPointer;
    // r3,r4,r5,r6,r7,r11,r26,r31 at the rejected transfer, not after unwind.
    std::array<std::uint32_t,8> registers{};
};
// First rejected transfer since initialize/reset. No memory payload is read here.
// LR is a return location, not necessarily the branch instruction PC.
[[nodiscard]] std::optional<InvalidGuestIndirectCall> stoppedGuestIndirectCall();

} // namespace recomp::bootstrap

extern "C" void recomp_host_guest_call_indirect(
        std::uint32_t guestTarget,
        PPCContext& context,
        std::uint8_t* base);
