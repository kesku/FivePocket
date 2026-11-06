#pragma once
#include <cstdint>

namespace recomp::bootstrap {
// Xbox 360 TB rate; independent of host CPU cycles and GPU-clock hypotheses.
inline constexpr std::uint32_t GuestTimebaseFrequency = 50000000;
constexpr std::uint64_t guestTimebaseFromNanoseconds(std::uint64_t nanoseconds) noexcept {
    return nanoseconds / 20; // Exact integer scale, no multiplication overflow.
}
// One process-wide monotonic epoch, shared by every guest thread. The trace
// process is a single session. No reset, wall-clock adjustment or CPU affinity.
std::uint64_t readGuestTimebase() noexcept;
std::uint64_t guestTimebaseReadCount() noexcept;
}
