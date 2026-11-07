#include "guest_timebase.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include "recomp_app_dir.h" // data-paths: the app's files directory.
namespace recomp::bootstrap {
// v112 (declared here: the shared header stays unchanged)
std::uint64_t guestTimebaseFromCounter(std::uint64_t ticks, std::uint64_t frequency) noexcept;
bool guestTimebaseUsesCounter() noexcept;
namespace {
// v112: the read count stays exact (run_trace.py checks it) but is spread over padded shards chosen by the
// counter's low bits, so the 1M+ reads per second from several guest threads stop bouncing one cache line.
struct alignas(64) Shard { std::atomic<std::uint64_t> reads{0}; };
std::array<Shard, 16> shards;

std::uint64_t steadyNanoseconds() noexcept {
    using Clock = std::chrono::steady_clock;
    static_assert(Clock::is_steady);
    static const auto epoch = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - epoch).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

#if defined(__aarch64__)
// v112: the ARM generic timer, the counter the vDSO's clock_gettime(CLOCK_MONOTONIC) itself reads, synchronized
// across cores and readable at EL0, read directly (no vDSO call). Scaled to the same 50 MHz guest rate from the
// same process epoch. files/private-trace/diag-timebase-steady-clock (read once) restores the steady_clock path.
inline std::uint64_t counter() noexcept { std::uint64_t v; asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
inline std::uint64_t counterFrequency() noexcept { std::uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
struct CounterClock {
    bool enabled = false; std::uint64_t epoch = 0, frequency = 0;
    CounterClock() noexcept {
        frequency = counterFrequency();
        const bool off = ::access(RECOMP_APP_PATH("/private-trace/diag-timebase-steady-clock"), F_OK) == 0 ||
            std::getenv("RECOMP_TIMEBASE_STEADY_CLOCK") != nullptr;
        enabled = !off && frequency >= 1000000 && frequency <= 1000000000;
        epoch = counter();
        (void)steadyNanoseconds(); // both epochs at the first read
    }
};
const CounterClock& counterClock() noexcept { static const CounterClock c; return c; }
#endif
}

std::uint64_t guestTimebaseFromCounter(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    // ticks * 50 MHz / frequency without overflow for ~10^4 years of ticks at 19.2 MHz.
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(ticks) * GuestTimebaseFrequency) / frequency);
}

std::uint64_t readGuestTimebase() noexcept {
#if defined(__aarch64__)
    const auto& c = counterClock();
    if (c.enabled) {
        const std::uint64_t now = counter();
        shards[now & 15].reads.fetch_add(1, std::memory_order_relaxed);
        return guestTimebaseFromCounter(now - c.epoch, c.frequency);
    }
#endif
    const auto ns = steadyNanoseconds();
    shards[ns & 15].reads.fetch_add(1, std::memory_order_relaxed);
    // Signed nanoseconds cover over 292 years, far beyond a bounded trace.
    return guestTimebaseFromNanoseconds(ns);
}
std::uint64_t guestTimebaseReadCount() noexcept {
    std::uint64_t total = 0;
    for (const auto& s : shards) total += s.reads.load(std::memory_order_relaxed);
    return total;
}
bool guestTimebaseUsesCounter() noexcept {
#if defined(__aarch64__)
    return counterClock().enabled;
#else
    return false;
#endif
}
}
