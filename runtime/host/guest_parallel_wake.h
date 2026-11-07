#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

// Parallel guest-thread scheduling hooks (multicore plan, local-analysis/parallel-design/plan.md).
// Implemented by the executor translation unit (guest_cooperative_executor.cpp). The mode is a
// compile-time switch of that translation unit only: RECOMP_GUEST_THREADS=0 cooperative (default),
// 1 gil (one execution permit), 2 lanes (permit domains chosen by the policy below). No class
// layout seen by other translation units depends on the mode.
//
// In cooperative mode the wake calls are no-ops: the cooperative scheduler re-evaluates every
// parked task's predicate itself.

// ---- Per-function checkpoint fast path (called by PPC_RUNTIME_CHECKPOINT in trace_prelude.h) ----
//
// Nonzero forces the slow path on every guest function entry. It is always nonzero in cooperative
// builds and outside a parallel run. During a parallel run the executor sets it for a stop, for an
// exclusive scope (runNativeNonYielding / runNativeWorkerNext, so their entry budgets stay exact),
// and while a newly enqueued task waits for its creator's next checkpoint. Read with a relaxed
// atomic load; written only by the executor.
extern "C" __attribute__((visibility("hidden"))) std::uint32_t recomp_guest_checkpoint_attention;
// Slow path: budgets, stops, safepoints, quantum rotation and the (sampled) trace sink. It re-arms
// *countdown. countdown may be null (a context without a slot); the call then counts one entry.
// In cooperative builds it is exactly the old per-entry chain: GuestExecutionGuard::checkpoint
// with the label bounded to 128 bytes, and the countdown is re-armed to take the next entry too.
extern "C" void recomp_guest_execution_checkpoint_slow(std::int32_t* countdown, const char* label,
                                                       std::size_t length);

namespace recomp::bootstrap {

typedef std::int32_t __attribute__((__may_alias__)) GuestCheckpointCountdown;
// As an unsigned value, any countdown at or above this limit means "take the slow path now". An
// uninitialised or copied-back context can therefore delay a slow path by at most this many
// entries; stops, exclusive scopes and task starts use the attention word instead.
inline constexpr std::uint32_t GuestCheckpointCountdownLimit = 4096;

// The countdown lives in PPCContext's tail padding (the class is alignas(0x40); the slot is the
// first four bytes after v127). Neither the layout nor TLS is involved, and every context works:
// padding is not guaranteed to be zeroed, but any initial value is bounded by the limit above
// (a value-initialised or reused context usually starts at 0 or -1 and takes the slow path on its
// first entry). A context type without v127 (the synthetic test context) has no slot and takes the
// slow path on every entry. C++17-compatible (the prelude is also used by C++17 translation units).
template<class Context, class = void>
struct GuestHasCheckpointCountdown : std::false_type {};
template<class Context>
struct GuestHasCheckpointCountdown<Context, std::void_t<decltype(std::declval<Context&>().v127)>> : std::true_type {};

template<class Context>
[[gnu::always_inline]] inline GuestCheckpointCountdown* guestCheckpointCountdown(Context& ctx) noexcept {
    if constexpr (GuestHasCheckpointCountdown<Context>::value) {
        constexpr std::size_t end = offsetof(Context, v127) + sizeof(std::declval<Context&>().v127);
        constexpr std::size_t slot = (end + alignof(std::int32_t) - 1) & ~(alignof(std::int32_t) - 1);
        static_assert(slot + sizeof(std::int32_t) <= sizeof(Context),
                      "PPCContext has no tail padding for the checkpoint countdown");
        return reinterpret_cast<GuestCheckpointCountdown*>(reinterpret_cast<unsigned char*>(&ctx) + slot);
    } else {
        (void)ctx;
        return nullptr;
    }
}

// The complete per-entry fast path: one decrement, one relaxed load of the attention word, one
// branch. `label` must have static storage (the generated prologue passes __func__).
template<class Context>
[[gnu::always_inline]] inline void guestCheckpoint(Context& ctx, const char* label, std::size_t length) {
    if constexpr (GuestHasCheckpointCountdown<Context>::value) {
        auto* countdown = guestCheckpointCountdown(ctx);
        const auto next = static_cast<std::uint32_t>(*countdown) - 1U;
        *countdown = static_cast<std::int32_t>(next);
        if (__builtin_expect((next >= GuestCheckpointCountdownLimit) |
                (__atomic_load_n(&recomp_guest_checkpoint_attention, __ATOMIC_RELAXED) != 0U), 0)) {
            recomp_guest_execution_checkpoint_slow(reinterpret_cast<std::int32_t*>(countdown), label, length);
        }
    } else {
        (void)ctx;
        recomp_guest_execution_checkpoint_slow(nullptr, label, length);
    }
}

// ---- Wake notifications (the registry calls these after releasing its mutex) ----

// Ask the task running as `token` to re-evaluate its wait predicate. Safe from any thread, with or
// without the registry mutex held: the scheduler's own lock is a leaf and never calls back out.
void wakeGuestWaiters(std::uint32_t token) noexcept;

// Unknown waiter set (broadcast signals): every parked task re-evaluates its predicate.
void wakeAllGuestWaiters() noexcept;

// The guest thread token bound to the calling host thread, or 0 outside executor tasks (for
// example setup code before run()). Cooperative mode: the task whose code runs on this thread.
std::uint32_t currentGuestThreadToken() noexcept;

// Permit-domain policy for `lanes` mode: maps a new task to a domain (0 = shared lane, 1..7 extra
// lanes; out-of-range results map to 0). Must be set before guest workers start (immutable
// afterwards: safe publication by thread start). It is called while the registry mutex may be
// held, so it must be a pure function. Arguments: guest thread token, guest start address, start
// context (the values passed to noteGuestThreadStart; 0 when the task was not described).
using GuestThreadDomainPolicy = std::function<unsigned(std::uint32_t token, std::uint32_t entry, std::uint32_t context)>;
void setGuestThreadDomainPolicy(GuestThreadDomainPolicy policy);

// Describe the guest start identity of the task that the calling host thread is about to enqueue
// (or run as the main task). Consumed by the next enqueue/run of the same token on this thread, in
// every mode. It feeds RENDER_DIAG_TASK_CREATED (token, entry, context, xapi, priority, domain and,
// when guestBase is non-null and context is an aligned pointer in 0x10000..0xFFFF0000, the first
// two big-endian words at context) and the lanes policy (token, entry, context). Pass the prepared
// thread's startRoutine, startContext and xapiStartup, and the guest address-space base.
void noteGuestThreadStart(std::uint32_t token, std::uint32_t entry, std::uint32_t context,
                          std::uint32_t xapiStartup, const std::uint8_t* guestBase) noexcept;

// Optional name of the blocking import for the next wait on this host thread; attributes late
// wakeups (poll-fallback completions) to it. The pointer must stay valid for the whole run (string
// literal). Without it, the last checkpoint label of the waiting task is used.
void noteGuestWaitSite(const char* site) noexcept;

// Scheduler evidence for tests and diagnostics: the current or most recent parallel run.
struct GuestParallelLateSite {
    const char* site{nullptr};
    std::uint64_t count{0};
};
struct GuestParallelStats {
    unsigned mode{0};                     // RECOMP_GUEST_THREADS of the executor build
    std::uint64_t slowCheckpoints{0};     // slow-path entries (a subset of all function entries)
    std::uint64_t permitAcquired{0}, permitContended{0}, permitWaitMaxNs{0}, permitHoldMaxNs{0};
    std::uint64_t maxConcurrentPermits{0};
    std::uint64_t rotations{0}, parks{0}, wakeNotified{0}, wakePoll{0}, wakeDeadline{0}, lateWakeups{0};
    std::uint64_t wakeCalls{0}, wakeAllCalls{0}, safepointParks{0}, safepointMaxNs{0};
    std::uint64_t exclusiveScopes{0}, handoffs{0}, probes{0}, probesConfirmed{0};
    std::uint64_t boostsParked{0}, boostsPending{0}, delays{0};
    std::array<GuestParallelLateSite, 8> lateSites{};
};
GuestParallelStats guestParallelStats() noexcept;

}  // namespace recomp::bootstrap
