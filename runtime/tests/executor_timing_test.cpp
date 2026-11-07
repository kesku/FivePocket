// SPDX-License-Identifier: GPL-3.0-or-later
#include "diag_timeline.h"
#include "guest_cooperative_executor.h"
#include "guest_parallel_wake.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace recomp::bootstrap {
bool runNativeExclusiveSpan(GuestCooperativeExecutor&, const GuestExecutionGuard::Entry&);
bool guestExclusiveScopeActive() noexcept;
void diagnosticDumpImports() {}
}

// Replace only the diagnostic and renderer dependencies. Scheduling, waits,
// cancellation and scope unwinding use the production executor and guard.
extern "C" {
int __android_log_print(int, const char*, const char*, ...) { return 0; }
int __android_log_vprint(int, const char*, const char*, va_list) { return 0; }
std::atomic<bool> recomp_v240_baseline{false}, recomp_v241_baseline{false};
#if defined(__APPLE__)
std::atomic<int> recomp_ios_render_sixteenths{16};
#endif
}

namespace {
using namespace recomp::bootstrap;
namespace timeline = recomp::diag::timeline;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::uint32_t MainToken = 0x1000;
constexpr std::uint32_t WorkerToken = 0x2000;

struct Event {
    timeline::Kind kind;
    std::uint16_t argument;
    std::uint32_t token;
};

struct Capture {
    std::mutex mutex;
    std::array<Event, 4096> events{};
    std::size_t count = 0;
    bool overflow = false;
    std::atomic<bool> parkBegan{false}, parkEnded{false}, exclusiveBegan{false};

    void reset() {
        std::lock_guard lock(mutex);
        count = 0;
        overflow = false;
        parkBegan = parkEnded = exclusiveBegan = false;
    }

    void record(timeline::Kind kind, std::uint16_t argument, std::uint32_t token) {
        std::lock_guard lock(mutex);
        if (count < events.size()) events[count++] = {kind, argument, token};
        else overflow = true;
        if (token == MainToken) {
            if (kind == timeline::ParkBegin) parkBegan.store(true, std::memory_order_release);
            if (kind == timeline::ParkEnd) parkEnded.store(true, std::memory_order_release);
            if (kind == timeline::ExclusiveBegin) exclusiveBegan.store(true, std::memory_order_release);
        }
    }

    std::vector<Event> select(timeline::Kind first, timeline::Kind second) {
        std::lock_guard lock(mutex);
        if (overflow) throw std::runtime_error("timing capture overflowed");
        std::vector<Event> selected;
        for (std::size_t i = 0; i < count; ++i)
            if (events[i].kind == first || events[i].kind == second) selected.push_back(events[i]);
        return selected;
    }
} capture;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool await(const std::atomic<bool>& ready) {
    const auto deadline = Clock::now() + 2s;
    while (!ready.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

template<class Entry>
GuestCooperativeResult run(GuestCooperativeExecutor& executor, Entry&& entry) {
    capture.reset();
    setGuestThreadDomainPolicy([](std::uint32_t token, std::uint32_t, std::uint32_t) {
        return token == WorkerToken ? 1U : 0U;
    });
    GuestCooperativeLimits limits;
    limits.functionEntries = 10000;
    return executor.run(MainToken, std::forward<Entry>(entry), limits, [](std::uint32_t) {});
}

void requirePair(unsigned kind) {
    const auto events = capture.select(timeline::ExclusiveBegin, timeline::ExclusiveEnd);
    require(events.size() == 2, "outermost ownership must have exactly one begin/end pair");
    require(events[0].kind == timeline::ExclusiveBegin && events[0].argument == kind,
            "exclusive begin has the wrong kind or order");
    require(events[1].kind == timeline::ExclusiveEnd, "exclusive end is missing or out of order");
    require(events[0].token == MainToken && events[1].token == MainToken,
            "exclusive ownership must end on the owning task");
    require(!guestExclusiveScopeActive(), "exclusive ownership survived its scope");
}

void requirePark(unsigned reason) {
    const auto events = capture.select(timeline::ParkBegin, timeline::ParkEnd);
    require(events.size() == 2, "wait must have exactly one park begin/end pair");
    require(events[0].kind == timeline::ParkBegin && events[1].kind == timeline::ParkEnd,
            "park events are out of order");
    require(events[1].argument == reason, "park end has the wrong notified/poll/deadline reason");
    require(events[0].token == MainToken && events[1].token == MainToken,
            "park events must name the waiting task");
}

void gateReturn() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] {
        require(executor.runNativeNonYielding([] {}, 256), "gate was refused");
        require(!guestExclusiveScopeActive(), "returned gate still owns exclusivity");
    });
    require(result.execution.completed(), "gate execution failed");
    requirePair(1);
}

void nestedSpanReturn() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] {
        require(runNativeExclusiveSpan(executor, [&] {
            require(runNativeExclusiveSpan(executor, [&] {
                require(executor.runNativeNonYielding([] {}, 256), "nested gate was refused");
            }), "nested span was refused");
            require(guestExclusiveScopeActive(), "nested return ended outer ownership early");
        }), "outer span was refused");
        require(!guestExclusiveScopeActive(), "returned span still owns exclusivity");
    });
    require(result.execution.completed(), "nested span execution failed");
    requirePair(4);
}

void spanUnwind() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] {
        (void)runNativeExclusiveSpan(executor, [&] {
            (void)runNativeExclusiveSpan(executor, [&] {
                (void)executor.runNativeNonYielding([] { throw std::runtime_error("entry failure"); }, 256);
            });
        });
    });
    require(result.execution.reason == GuestExecutionStopReason::HostException, "entry exception was not reported");
    requirePair(4);
}

void spanCancellation() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] {
        (void)runNativeExclusiveSpan(executor, [&] {
            (void)executor.runNativeNonYielding([] {
                GuestExecutionGuard::checkpoint("first");
                GuestExecutionGuard::checkpoint("over-budget");
            }, 1);
        });
    });
    require(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded,
            "scope cancellation lost its budget failure");
    requirePair(4);
}

void beginCancellation() {
    GuestCooperativeExecutor executor;
    std::atomic<bool> workerEntered{false};
    bool entryRan = false;
    const auto result = run(executor, [&] {
        require(executor.enqueue(WorkerToken, [&] {
            workerEntered.store(true, std::memory_order_release);
            wakeGuestWaiters(MainToken);
            require(await(capture.exclusiveBegan), "exclusive begin never arrived");
            GuestExecutionGuard::stopForGuestTrap();
        }), "worker was refused");
        require(executor.waitUntil([&] { return workerEntered.load(std::memory_order_acquire); }, Clock::now() + 2s),
                "worker did not start");
        (void)runNativeExclusiveSpan(executor, [&] { entryRan = true; });
    });
    require(result.execution.reason == GuestExecutionStopReason::GuestTrap, "worker cancellation lost its trap");
    require(!entryRan, "cancelled begin ran its entry");
    requirePair(4);
}

void handoffReturn() {
    GuestCooperativeExecutor executor;
    bool workerRan = false;
    const auto result = run(executor, [&] {
        require(executor.enqueue(WorkerToken, [&] { workerRan = true; }), "handoff worker was refused");
        require(executor.runNativeWorkerNext(WorkerToken, 256), "handoff failed");
    });
    require(result.execution.completed() && workerRan, "handoff did not finish its worker");
    requirePair(3);
}

void handoffCancellation() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] {
        (void)runNativeExclusiveSpan(executor, [&] {
            require(executor.enqueue(WorkerToken, [] { GuestExecutionGuard::stopForGuestTrap(); }),
                    "handoff worker was refused");
            (void)executor.runNativeWorkerNext(WorkerToken, 256);
        });
    });
    require(result.execution.reason == GuestExecutionStopReason::GuestTrap, "handoff cancellation lost its trap");
    requirePair(4);
}

void immediateWait() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] { require(executor.waitUntil([] { return true; }), "ready wait failed"); });
    require(result.execution.completed(), "ready wait execution failed");
    require(capture.select(timeline::ParkBegin, timeline::ParkEnd).empty(), "ready wait should not record a park");
}

void pollWait() {
    GuestCooperativeExecutor executor;
    const auto result = run(executor, [&] {
        require(executor.waitUntil([&] { return capture.parkEnded.load(std::memory_order_acquire); }, Clock::now() + 1h),
                "poll wait failed");
    });
    require(result.execution.completed(), "poll wait execution failed");
    requirePark(2);
    const auto stats = guestParallelStats();
    require(stats.wakePoll == 1 && stats.wakeDeadline == 0 && stats.wakeNotified == 0,
            "poll classification disagrees with executor counters");
}

void deadlineWait() {
    GuestCooperativeExecutor executor;
    unsigned checks = 0;
    const auto result = run(executor, [&] {
        const auto deadline = Clock::now() - 1ms;
        // Readiness changes after the initial check, with the deadline already
        // elapsed. This exercises the boundary without a wall-clock race.
        require(executor.waitUntil([&] { return ++checks > 1; }, deadline), "deadline wait failed");
    });
    require(result.execution.completed(), "deadline wait execution failed");
    requirePark(3);
    const auto stats = guestParallelStats();
    require(stats.wakeDeadline == 1 && stats.wakePoll == 0 && stats.wakeNotified == 0,
            "deadline classification disagrees with executor counters");
}

void notifiedWait() {
    GuestCooperativeExecutor executor;
    std::atomic<bool> ready{false};
    const auto result = run(executor, [&] {
        std::jthread producer([&](std::stop_token stop) {
            const auto deadline = Clock::now() + 2s;
            while (!capture.parkBegan.load(std::memory_order_acquire) && !stop.stop_requested()) {
                if (Clock::now() >= deadline) return;
                std::this_thread::yield();
            }
            if (stop.stop_requested()) return;
            while (!guestParallelStats().wakeNotified && !stop.stop_requested()) {
                if (Clock::now() >= deadline) return;
                wakeGuestWaiters(MainToken);
                std::this_thread::yield();
            }
            ready.store(true, std::memory_order_release);
            wakeGuestWaiters(MainToken);
        });
        require(executor.waitUntil([&] { return ready.load(std::memory_order_acquire); }, Clock::now() + 2s),
                "notified wait failed");
    });
    require(result.execution.completed(), "notified wait execution failed");
    const auto events = capture.select(timeline::ParkBegin, timeline::ParkEnd);
    const auto stats = guestParallelStats();
    std::uint64_t notified = 0, polled = 0;
    require(!events.empty() && events.size() % 2 == 0, "notified wait has unpaired park events");
    for (std::size_t i = 0; i < events.size(); i += 2) {
        require(events[i].kind == timeline::ParkBegin && events[i + 1].kind == timeline::ParkEnd,
                "notified park events are out of order");
        require(events[i].token == MainToken && events[i + 1].token == MainToken,
                "notified events name the wrong task");
        if (events[i + 1].argument == 1) ++notified;
        else if (events[i + 1].argument == 2) ++polled;
        else throw std::runtime_error("notification fixture reached its deadline");
    }
    require(notified && notified == stats.wakeNotified && polled == stats.wakePoll && !stats.wakeDeadline,
            "notified events disagree with executor counters");
}
}

namespace recomp::diag::timeline {
std::atomic<bool> armed{true};
thread_local std::uint32_t waitObject = 0;
thread_local std::uint32_t token = 0;
void setThreadToken(std::uint32_t value) noexcept { token = value; }
void record(Kind kind, std::uint16_t argument, std::uint32_t) noexcept { capture.record(kind, argument, token); }
void recordAt(std::uint64_t, Kind kind, std::uint16_t argument, std::uint32_t extra) noexcept {
    record(kind, argument, extra);
}
void tick() noexcept {}
}

int main(int argc, char** argv) {
    const std::array tests{
        std::pair{"gate-return", gateReturn}, std::pair{"nested-span-return", nestedSpanReturn},
        std::pair{"span-unwind", spanUnwind}, std::pair{"span-cancellation", spanCancellation},
        std::pair{"begin-cancellation", beginCancellation}, std::pair{"handoff-return", handoffReturn},
        std::pair{"handoff-cancellation", handoffCancellation}, std::pair{"immediate-wait", immediateWait},
        std::pair{"poll-wait", pollWait}, std::pair{"deadline-wait", deadlineWait},
        std::pair{"notified-wait", notifiedWait},
    };
    unsigned ran = 0, failed = 0;
    for (const auto& [name, test] : tests) {
        if (argc > 1 && std::string_view(argv[1]) != name) continue;
        ++ran;
        try { test(); std::printf("PASS %s\n", name); }
        catch (const std::exception& error) { ++failed; std::fprintf(stderr, "FAIL %s: %s\n", name, error.what()); }
    }
    if (!ran) { std::fprintf(stderr, "Unknown timing test\n"); return 2; }
    std::printf("%u timing tests, %u failures\n", ran, failed);
    return failed ? 1 : 0;
}
