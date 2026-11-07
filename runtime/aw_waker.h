#pragma once
// async-wake (agents/async-wake, 2026-10-03): main's host-level wake-ups off main's thread (runtime switches diag-aw-*).
//
// wakeGuestWaiters(token) keeps every guest-visible step where it is today: the registry's state change (semaphore count, event
// state, critical-section owner, the reserved waiters) under R on the signalling thread, then - for each notified token, in the
// registry's order - the task's wakeSeq bump and its cvWaiting test under the scheduler lock, synchronously on the caller.
// Only the host notify that follows the unlock (condition_variable::notify_all -> pthread_cond_broadcast -> FUTEX_WAKE: the
// kernel's try_to_wake_up of the parked host thread, ~10 us of main's CPU per real wake on the Thor, estimate) may move:
//  - dedupe (diag-aw-dedupe, and implied by the waker modes): a notify for a task that the scheduler already notified inside
//    the SAME parallelWait park wait (cvGen) is skipped. That wait's predicate (wakeSeq != seq) became true at the first
//    bump and stays true, and the first notify already took the task off the futex queue: the task leaves the wait at its next
//    predicate check either way. Waits whose predicates do not read wakeSeq (permit, delay, probe, start) keep every notify,
//    and so does another thread's repeat of a first notify that came from main (that one may still be in the waker's queue).
//  - waker (diag-aw-all: spin window 500 us, diag-aw-all2k: 2000 us): main's notifies go through one FIFO ring to one waker
//    thread (little role: cpu0-2 on the Thor, nice -20). The waker spins for the window after its last notify, then parks. Main notifies
//    inline (as today) only when no handed notify is outstanding and the waker is not awake, and then restarts it. Notify
//    order: one FIFO; while a handed notify is outstanding main only appends, so notifies are issued in main's order. If the
//    waker made no progress for StallNs while notifies are outstanding (it was preempted or is parking), main takes the queued
//    ones back and issues them itself, then this call's inline (the waker's single in-flight notify, if any, may then complete
//    after main's: host wake order only). Switching the waker off drains in order (outstanding notifies keep the FIFO path).
// No guest-visible state is written by the waker: it only calls notify on tasks whose wake was already published under the
// scheduler lock; a late or spurious notify is harmless (every wait re-checks its predicate under the lock). No wake is lost:
// an item stays in the ring until a consumer completes it; the waker parks only after re-checking the ring (Dekker pairing
// with main's fence + state check after its append, which restarts a parking waker); main's stall check takes back items a
// slow or parked waker holds. Each woken task keeps its own park-poll timer (250 us / 2 ms first step) as before.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include "cpu_topology.h" // topology lane: the waker's little role
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// The switch state, set by the renderer's window poll (generic_renderer.cpp awWindow); defined in guest_cooperative_executor.cpp.
extern "C" std::atomic<unsigned> recomp_aw_mode;    // bit 0 dedupe, bit 1 waker (the renderer sets both for diag-aw-all*)
extern "C" std::atomic<unsigned> recomp_aw_spin_us; // the waker's spin window (us)
extern "C" std::atomic<int> recomp_aw_waker_tid;    // the waker's host tid (0 = none): trace_runner's ranking leaves it alone

namespace recomp_aw {
enum : unsigned { ModeDedupe = 1u, ModeWaker = 2u };
inline unsigned mode() noexcept { return recomp_aw_mode.load(std::memory_order_relaxed); }
constexpr std::uint64_t StallNs = 100'000; // no waker progress for this long while notifies are outstanding: main takes them back

struct Stats {
    std::atomic<unsigned long long> deduped{0}, handed{0}, inlined{0}, restarts{0}, parks{0}, rescues{0}, rescued{0},
        full{0}, issueNs{0}, awakeNs{0}, lagNs{0}, lagMaxNs{0}, backlogMax{0}, notifies{0};
};
inline Stats& stats() noexcept { static Stats s; return s; }
inline void raiseMax(std::atomic<unsigned long long>& m, unsigned long long v) noexcept {
    auto cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
inline std::uint64_t nowNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// Bounded MPMC ring (Vyukov): one producer (main) appends; the waker and main's take-back claim cells by CAS on head.
// done counts completed notifies: outstanding = appended - done (producer side).
template <class Item, unsigned N>
class Ring {
    static_assert((N & (N - 1)) == 0, "power of two");
    struct alignas(64) Cell { std::atomic<std::uint64_t> seq{0}; Item item{}; std::uint64_t at{0}; };
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> done_{0};
    Cell cells_[N];
public:
    Ring() noexcept { for (unsigned i = 0; i < N; ++i) cells_[i].seq.store(i, std::memory_order_relaxed); }
    bool push(Item&& item, std::uint64_t at) noexcept { // producer only
        const auto pos = tail_.load(std::memory_order_relaxed);
        Cell& c = cells_[pos & (N - 1)];
        if (c.seq.load(std::memory_order_acquire) != pos) return false; // full
        c.item = std::move(item);
        c.at = at;
        c.seq.store(pos + 1, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_release);
        return true;
    }
    bool pop(Item& out, std::uint64_t& at) noexcept { // any consumer
        auto pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & (N - 1)];
            const auto seq = c.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<std::int64_t>(seq - (pos + 1));
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
#if defined(RECOMP_AW_MUTANT_LIFO) // mutant: a consumer takes the NEWEST queued notify (reordered release); must fail the order checks
                    const auto last = tail_.load(std::memory_order_acquire) - 1;
                    if (last != pos) {
                        Cell& l = cells_[last & (N - 1)];
                        if (l.seq.load(std::memory_order_acquire) == last + 1) { std::swap(c.item, l.item); std::swap(c.at, l.at); }
                    }
#endif
                    out = std::move(c.item);
                    at = c.at;
                    c.seq.store(pos + N, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }
    bool ready() const noexcept { // a cell is claimable (consumer-side emptiness test)
        const auto pos = head_.load(std::memory_order_relaxed);
        return cells_[pos & (N - 1)].seq.load(std::memory_order_acquire) == pos + 1;
    }
    void complete() noexcept { done_.fetch_add(1, std::memory_order_release); }
    std::uint64_t outstanding() const noexcept { return tail_.load(std::memory_order_relaxed) - done_.load(std::memory_order_acquire); }
};

// Notify: callable(Item&) noexcept; the item is reset (released) by the consumer right after it.
template <class Item, class Notify, unsigned N = 512>
class Waker {
public:
    explicit Waker(Notify notify) : notify_(std::move(notify)) {}
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    // Producer (main's thread only). items[0..k): this call's notifies in order. wakerOn: the switch wants the waker (off:
    // notifies stay inline, except behind outstanding handed ones, which keep the FIFO). preferInline: no reason to hand them
    // while nothing is outstanding (the render thread's kick). Every item is notified exactly once, by main now or a consumer.
    void deliver(Item* items, unsigned k, bool wakerOn, bool preferInline, unsigned spinUs) noexcept {
        if (!k) return;
        if (wakerOn && !started()) (void)start();
        if (!started()) { inlineAll(items, k); return; }
        auto& st = stats();
        spinUs_.store(spinUs, std::memory_order_relaxed);
        const auto now = nowNs();
        if (ring_.outstanding()) {
#if !defined(RECOMP_AW_MUTANT_INLINE_PENDING)
            if (now - heartbeat_.load(std::memory_order_relaxed) > StallNs) { // waker stalled or parked: take the queue back
                takeBack();
#else // mutant: main notifies inline while handed notifies are still queued (reorders releases); must fail the order checks
            if (true) {
#endif
                inlineAll(items, k);
                if (wakerOn && state_.load(std::memory_order_relaxed) == Parked) restart();
                return;
            }
        } else {
            if (!wakerOn || preferInline || state_.load(std::memory_order_acquire) != Awake) {
                inlineAll(items, k);
                if (wakerOn && !preferInline && state_.load(std::memory_order_relaxed) == Parked) restart();
                return;
            }
        }
        unsigned i = 0;
        for (; i < k; ++i)
            if (!ring_.push(std::move(items[i]), now)) break;
        st.handed.fetch_add(i, std::memory_order_relaxed);
        raiseMax(st.backlogMax, ring_.outstanding());
        if (i < k) { // full: take every queued notify back (order), then this call's rest inline
            st.full.fetch_add(1, std::memory_order_relaxed);
            takeBack();
            inlineAll(items + i, k - i);
        }
        std::atomic_thread_fence(std::memory_order_seq_cst); // pairs with the waker's fence before its last ring check
#if !defined(RECOMP_AW_MUTANT_NO_RECHECK) // mutant: no state re-check after the append (a wake lost while the waker parks)
        if (state_.load(std::memory_order_relaxed) == Parked) restart();
#endif
    }
    // Starts the waker thread once (producer); false if it could not be started (then every delivery is inline).
    bool start() noexcept {
        if (startState_.load(std::memory_order_acquire) == 0) {
            startState_.store(1, std::memory_order_relaxed);
            try {
                std::thread([this] { run(); }).detach();
                startState_.store(2, std::memory_order_release);
            } catch (...) { startState_.store(3, std::memory_order_release); }
        }
        return started();
    }
    bool started() const noexcept { return startState_.load(std::memory_order_acquire) == 2; }
    std::uint64_t outstanding() const noexcept { return ring_.outstanding(); }
    bool parked() const noexcept { return state_.load(std::memory_order_acquire) == Parked; }

private:
    enum : unsigned { Awake = 1, Parked = 2 };
    void inlineAll(Item* items, unsigned k) noexcept {
        for (unsigned i = 0; i < k; ++i) { notify_(items[i]); items[i] = Item{}; }
        stats().inlined.fetch_add(k, std::memory_order_relaxed);
        stats().notifies.fetch_add(k, std::memory_order_relaxed);
    }
    void takeBack() noexcept { // the waker pauses its pops meanwhile (at most the one it already committed to overlaps)
        Item item{};
        std::uint64_t at = 0;
        unsigned n = 0;
        takingBack_.store(true, std::memory_order_seq_cst);
        while (ring_.pop(item, at)) { notify_(item); item = Item{}; ring_.complete(); ++n; }
        takingBack_.store(false, std::memory_order_release);
        stats().rescues.fetch_add(1, std::memory_order_relaxed);
        stats().rescued.fetch_add(n, std::memory_order_relaxed);
        stats().notifies.fetch_add(n, std::memory_order_relaxed);
    }
    void restart() noexcept { // one restart per park: a pending one is not repeated
        if (restartPending_.exchange(true, std::memory_order_acq_rel)) return;
        try {
            { std::lock_guard<std::mutex> g(m_); kick_ = true; }
            cv_.notify_one();
            stats().restarts.fetch_add(1, std::memory_order_relaxed);
        } catch (...) { restartPending_.store(false, std::memory_order_release); }
    }
    void place() noexcept {
#if defined(__ANDROID__)
        cpu_set_t little;
        CPU_ZERO(&little);
        RECOMP_TOPOLOGY_ADD_CPUS(little, recomp_topology::roles().little); // topology lane: the little role (Thor: the A510 cores 0-2)
        (void)sched_setaffinity(0, sizeof little, &little);
#endif
    }
    void run() noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        recomp_aw_waker_tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_relaxed);
        (void)pthread_setname_np(pthread_self(), "recomp-aw-waker");
        (void)setpriority(PRIO_PROCESS, 0, -20); // (the app's RLIMIT_NICE allows it, as trace_runner's v336)
#elif defined(__APPLE__)
        // iOS (Claude 2026-10-17): the waker spins for its window after each notify; at utility QoS it runs on an efficiency
        // core (the CPU and GPU share one power budget, c105/c129). Named for the sampler; tid = pthread_threadid_np's.
        (void)pthread_setname_np("recomp-aw-waker");
        (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        { std::uint64_t id = 0; (void)pthread_threadid_np(nullptr, &id); recomp_aw_waker_tid.store(static_cast<int>(id), std::memory_order_relaxed); }
#endif
        place();
        auto& st = stats();
        std::uint64_t now = nowNs(), last = now, awakeSince = now, placed = now;
        Item item{};
        for (;;) {
            heartbeat_.store(now, std::memory_order_relaxed);
            std::uint64_t at = 0;
            if (takingBack_.load(std::memory_order_acquire)) { relax(); now = last = nowNs(); continue; } // main is taking the queue back
            if (ring_.pop(item, at)) {
                notify_(item);
                item = Item{};
                ring_.complete();
                const auto t = nowNs();
                st.issueNs.fetch_add(t - now, std::memory_order_relaxed);
                st.lagNs.fetch_add(t - at, std::memory_order_relaxed);
                raiseMax(st.lagMaxNs, t - at);
                st.notifies.fetch_add(1, std::memory_order_relaxed);
                now = last = t;
                continue;
            }
            const std::uint64_t window = std::uint64_t(spinUs_.load(std::memory_order_relaxed)) * 1000u;
            if (now - last >= window) {
                std::unique_lock<std::mutex> g(m_);
                state_.store(Parked, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst); // pairs with deliver's fence after the append
                if (ring_.ready()) { state_.store(Awake, std::memory_order_relaxed); g.unlock(); now = last = nowNs(); continue; }
                st.parks.fetch_add(1, std::memory_order_relaxed);
                st.awakeNs.fetch_add(now - awakeSince, std::memory_order_relaxed);
                cv_.wait(g, [&] { return kick_; });
                kick_ = false;
                restartPending_.store(false, std::memory_order_release);
                state_.store(Awake, std::memory_order_seq_cst);
                g.unlock();
                now = last = awakeSince = nowNs();
                if (now - placed > 1'000'000'000ull) { place(); placed = now; } // re-applied after a park, at most once a second
                continue;
            }
            relax();
            now = nowNs();
        }
    }

    Notify notify_;
    Ring<Item, N> ring_;
    alignas(64) std::atomic<std::uint64_t> heartbeat_{0};
    alignas(64) std::atomic<unsigned> state_{Awake};
    std::atomic<bool> restartPending_{false};
    std::atomic<bool> takingBack_{false};
    std::atomic<unsigned> spinUs_{500};
    std::atomic<unsigned> startState_{0};
    std::mutex m_;
    std::condition_variable cv_;
    bool kick_{false};
};
} // namespace recomp_aw
