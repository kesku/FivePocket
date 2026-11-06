#pragma once

#include "guest_execution_guard.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace recomp::bootstrap {

struct GuestCooperativeLimits {
    std::uint64_t functionEntries{10000};
    std::uint32_t quantum{32};
    std::size_t tasks{65}; // Initial task plus at most 64 workers.
    bool nativeHighPriorityTiers{false}; // Explicit native +3/+4 capped at8x, not console timing.
};

struct GuestCooperativeResult {
    GuestExecutionResult execution;
    std::size_t workersCreated{0};
    std::size_t workersEntered{0};
    std::uint64_t contextSwitches{0};
    std::uint64_t waitOperations{0};
};

// Stackful, serialized diagnostic execution. Each worker owns a host thread,
// but one mutex is held throughout guest execution and released only while a
// checkpoint waits for its next turn. Registry rebinding runs under that gate.
//
// This is NOT a full Xbox scheduler: there is no preemption inside functions,
// absolute/alertable wait/APC support or instruction budget. Relative waits use
// monotonic deadlines. Normal/saturation priority tiers weight checkpoint slices,
// not host OS priority or strict Xbox dispatch ordering. Optional event wake
// boosts last one scheduling boundary; they do not change guest base priority.
// Infinite waits use explicit readiness callbacks; other host blocking is not
// permitted. The outer process supervisor is still required. Entry callbacks
// must reach checkpoints or park through waitUntil().
// Keep the executor and its callbacks alive for run(); never destroy it while
// a run is in progress. No task or entry callback survives a completed run.
class GuestCooperativeExecutor final {
public:
    static constexpr std::size_t MaximumTasks = 65;
    using Entry = GuestExecutionGuard::Entry;
    using BindThread = std::function<void(std::uint32_t)>;
    using Ready = std::function<bool()>;
    // Injectable for fault tests. Must return an owned joinable thread or throw
    // before launching; it must not detach, retain, or invoke the body inline.
    using ThreadFactory = std::function<std::thread(Entry)>;

    explicit GuestCooperativeExecutor(ThreadFactory factory = {});
    GuestCooperativeExecutor(const GuestCooperativeExecutor&) = delete;
    GuestCooperativeExecutor& operator=(const GuestCooperativeExecutor&) = delete;

    [[nodiscard]] GuestCooperativeResult run(
            std::uint32_t mainToken,
            const Entry& entry,
            const GuestCooperativeLimits& limits,
            const BindThread& bindThread,
            const GuestExecutionGuard::FunctionTraceSink& traceSink = {});
    // Only callable from the active gated guest task. Enqueueing never switches
    // tasks inline; the next checkpoint gives the new worker a turn. This lets a
    // kernel adapter publish resume state before any worker touches that state.
    [[nodiscard]] bool enqueue(std::uint32_t token, const Entry& entry, std::int32_t priority = 0);
    // Gated metadata update only; never switches inline. Weights are 1/4, 1/2,
    // 1, 2, 4 for increments -2..2 (minimum slice one checkpoint). Runnable
    // tasks rotate after their slice. Event wake boosts use the separate opt-in
    // batch method below; no strict Xbox priority dispatch is claimed.
    [[nodiscard]] bool setPriority(std::uint32_t token, std::int32_t priority);
    // Explicit native wake policy: one extra weighted slice tier (capped at
    // shift3), decaying after the resumed task's next slice or next wait.
    // Atomic batch: every distinct token must name a live parked task; failure
    // changes none. No inline execution, base-priority change or OS scheduling.
    [[nodiscard]] bool boostEventWaiters(const std::uint32_t* tokens,std::size_t count);
    [[nodiscard]] bool running() const noexcept;
    // Read-only scheduling evidence, only under the current executor gate.
    // parkedTask never invokes/consumes a readiness predicate. A producer flag
    // alone cannot establish that its original consumer has actually parked.
    [[nodiscard]] std::uint32_t currentTaskToken() const noexcept;
    [[nodiscard]] bool parkedTask(std::uint32_t token) const noexcept;
    // Current gated task only. commitExit must publish real thread completion
    // without scheduling/reentry. Success unwinds this task and NEVER returns;
    // invalid context or a rejected commit returns false without ending a task.
    [[nodiscard]] bool terminateCurrentThread(std::uint32_t token,const Ready& commitExit);
    // Runs readiness checks under the execution gate (possibly on a different
    // host task), without changing guest identity. A true check may consume its
    // signal. False return means invalid context, wait-budget exhaustion, or
    // no runnable producer. No callback survives this method's return/unwind.
    // Timed readiness must become true on signal OR at its monotonic deadline.
    [[nodiscard]] bool waitUntil(const Ready& ready,
            std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
    // Even an elapsed/zero delay relinquishes the current slice once. The
    // caller resumes only at/after the captured native monotonic deadline.
    [[nodiscard]] bool delayUntil(std::chrono::steady_clock::time_point deadline);
    // Native interrupt primitive: run on the CURRENT gated guest task without
    // checkpoint task rotation. Keeps global and additional1..4096entry budgets
    // active; waits/delays and nested scopes reject. Entry exceptions/guest stops
    // propagate and always clear the scope. Does not set CPU/PCR/IRQL, authorize
    // a guest callback, or create a worker. No host-blocking work is permitted.
    [[nodiscard]] bool runNativeNonYielding(const Entry& entry,std::uint32_t maximumEntries);
    // Immediate, exclusive handoff to one newly enqueued worker, then back to
    // this caller. No peer readiness predicates or guest jobs run in between.
    // Bounds ALL worker checkpoints (1..4096), including outside a nested
    // runNativeNonYielding scope. Worker waits/delays and nested handoffs reject.
    // This schedules an existing worker; it cannot invent a guest identity.
    [[nodiscard]] bool runNativeWorkerNext(std::uint32_t token,std::uint32_t maximumEntries);

private:
    [[nodiscard]] bool waitUntilImpl(const Ready& ready,
            std::optional<std::chrono::steady_clock::time_point> deadline, bool yieldCurrentSlice);
    struct Job {
        std::uint32_t token{0};
        Entry entry;
        std::thread host;
        std::condition_variable selected;
        bool finished{false};
        bool entered{false};
        std::int32_t priority{0};
        std::uint64_t sliceEntries{0};
        bool eventWakeBoost{false};
        Ready ready;
        std::optional<std::chrono::steady_clock::time_point> deadline;
    };
    struct Current {
        GuestCooperativeExecutor* executor;
        std::size_t index;
        std::unique_lock<std::mutex>* lock;
    };
    static thread_local Current* current_;

    void worker(std::size_t index);
    // The selected task alone can execute. Wake every task only for terminal
    // cancellation; broadcasting each slice causes a host-thread stampede.
    void notifySchedulingChange();
    void execute(std::size_t index, std::unique_lock<std::mutex>& lock);
    void checkpoint(std::string_view label);
    void bind(std::size_t index);
    void finish(std::size_t index, GuestExecutionResult result, std::unique_lock<std::mutex>& lock);
    [[nodiscard]] std::size_t nextTask(std::size_t after);
    [[nodiscard]] std::size_t selectTask(std::size_t after, std::unique_lock<std::mutex>& lock);

    ThreadFactory factory_;
    std::mutex gate_;
    std::condition_variable changed_;
    std::atomic_bool running_{false};
    std::array<std::unique_ptr<Job>, MaximumTasks> jobs_{};
    std::size_t jobCount_{0};
    std::size_t selected_{0};
    std::size_t workersEntered_{0};
    std::uint64_t entries_{0};
    std::uint64_t switches_{0};
    std::uint64_t waits_{0};
    std::uint32_t boundToken_{0};
    bool reschedule_{false};
    struct NonYielding { std::size_t index;std::uint64_t start;std::uint32_t budget; };
    std::optional<NonYielding> nativeNonYielding_;
    struct NativeHandoff { std::size_t parent,worker;std::uint64_t start;std::uint32_t budget; };
    std::optional<NativeHandoff> nativeHandoff_;
    GuestCooperativeLimits limits_{};
    BindThread bindThread_;
    GuestExecutionGuard::FunctionTraceSink traceSink_;
    std::optional<GuestExecutionResult> terminal_;
};

} // namespace recomp::bootstrap
