#pragma once

#include <chrono>
#include <optional>

namespace recomp::bootstrap {

// Native monotonic scheduling state. Signal/reset behavior belongs to the
// enclosing dispatcher object; missed periodic ticks coalesce, never accumulate.
struct GuestWaitableTimer {
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> due;
    Clock::duration period{};

    bool refresh(Clock::time_point now) noexcept {
        if (!due || now < *due) { return false; }
        if (period == Clock::duration::zero()) {
            due.reset();
        } else {
            const auto remaining = period - ((now - *due) % period);
            if (remaining > Clock::time_point::max() - now) {
                due.reset(); // No representable future expiration.
            } else {
                due = now + remaining;
            }
        }
        return true;
    }
};

} // namespace recomp::bootstrap
