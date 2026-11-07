#include "diag_log_sample.h" // v218
#include "diag_timeline.h"
#include <unordered_map>
#include "guest_cooperative_executor.h"
#include "guest_parallel_wake.h"
#include "guest_thread_priority.h"
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h> // v110: access() for the diag- switches (host builds too)
#include <sys/resource.h> // v240: getpriority/setpriority
#include <sched.h> // v240
#include <cerrno> // v240
#include <cstring> // v362: std::memcpy
#include "fast_tls_v362.h" // v362
// v362 (guest_execution_guard.cpp): the calling thread's activeExecution variable (its address; read in place by the claimed thread)
extern "C" const void* recomp_v362_guard_active_slot() noexcept;
#include <functional> // v240
#if defined(__ANDROID__)
#include <android/log.h>
#include "diag_timeline.inc" // v159: the timeline probe (defined here, used by other TUs through diag_timeline.h)
#include "diag_native_stacks.h" // v199: native stacks at a stall (stallWatchdogMain)
#include <unistd.h>
#endif

// Guest-thread execution mode of THIS translation unit (multicore plan WP1):
//   0 cooperative (default): today's serialized executor, unchanged scheduling.
//   1 gil:   one execution permit; waits evaluated by the waiter with wake sequences.
//   2 lanes: permit domains chosen by setGuestThreadDomainPolicy (default: all tasks in domain 0).
// The class layout in guest_cooperative_executor.h is identical in every mode: parallel state lives
// in a .cpp-private singleton, so other translation units need no define.
#ifndef RECOMP_GUEST_THREADS
#define RECOMP_GUEST_THREADS 0
#endif
#if RECOMP_GUEST_THREADS < 0 || RECOMP_GUEST_THREADS > 2
#error "RECOMP_GUEST_THREADS must be 0 (cooperative), 1 (gil) or 2 (lanes)"
#endif

// Nonzero forces the per-function checkpoint slow path (guest_parallel_wake.h). Permanently 1 in
// cooperative builds; parallel runs clear the idle bit for their duration.
extern "C" __attribute__((visibility("hidden"))) std::uint32_t recomp_guest_checkpoint_attention = 1;

#if defined(__ANDROID__)
extern "C" void recomp_diag_task_host_started(std::uint32_t token, int tid) __attribute__((weak));
#endif
namespace recomp::bootstrap { void diagnosticDumpImports(); } // v49 (guest_import_registry.cpp)
#include "pooled_host.h" // v240
#include "prime_swap_v245.h" // v245
extern "C" std::atomic<unsigned> recomp_v346_main_spin_us; // v346/v347 (defined with the gate counters below)
#include "poll_sleep_v266.h" // v266
#include "park_poll_jp.h" // job-path: diag-jp-park-poll (park poll schedule of non-critical tasks)
// job-path: the in-run switch (files/private-trace/diag-jp-park-poll, set by the renderer's switch poll; read once per wait).
extern "C" { std::atomic<bool> recomp_jp_park_poll{false}; }
#include "aw_waker.h" // async-wake: diag-aw-* (dedupe of repeat notifies; main's notifies through a waker thread)
#if defined(__APPLE__)
// ios-qos-main-wait (Claude, 2026-10-18): the pad-poll hook in Main.mm sets the main loop's QoS only while main polls the pad, and
// main stops polling before gameplay (c195: its last change came before the renderer's A/B windows began). Main waits here
// every frame in gameplay (c193 sampler: sub_823C7A68 < sub_822DE960), so the same rule is applied again on main's own
// thread from its waits: ios-qos-main = user-interactive; ios-qos-main-low = utility; ios-qos-main-keep = default; none = utility
// at 100 % render scale and above, default below 100 % (ADOPTED 2026-10-18, see below).
#define RECOMP_IOS_SWITCHES_NO_MACRO
#include "ios_switches.h"
#include <pthread/qos.h>
namespace recomp::bootstrap { std::uint32_t currentGuestThreadToken() noexcept; }
extern "C" std::atomic<int> recomp_ios_render_sixteenths; // (generic_renderer.cpp: the applied render scale, 16 = 100 %)
#include <mach/mach_time.h>
// ios-qos-recheck (Claude 2026-10-20): the QoS hooks below re-read their switch files at most every 250 ms per thread, no longer
// every 16th wait. A check is one to four access() syscalls (a file-system lookup under the sandbox), and guest threads,
// main on the frame's serial chain among them, wait hundreds of times per second. The same files, same decisions, later by
// at most 250 ms. The first wait of a thread checks at once.
inline bool iosQosRecheckDue(std::uint64_t& next) noexcept {
    const std::uint64_t now = mach_absolute_time();
    if (now < next) return false;
    static const std::uint64_t step = [] {mach_timebase_info_data_t tb{}; mach_timebase_info(&tb);
        return tb.numer ? std::uint64_t(250'000'000ull) * tb.denom / tb.numer : std::uint64_t(6'000'000ull);}();
    next = now + step; return true;
}
namespace {
// ios-qos-guest-low (Claude 2026-10-20; scheduling only, A/B aid): every guest thread except main and the game's render thread at
// utility QoS while files/private-trace/ios-qos-guest-low is present, so the two performance cores stay with the frame's serial
// chain (main -> render thread -> worker); without the file nothing is changed. The render thread's token comes from the present
// hook (generic_renderer.cpp).
extern "C" { std::atomic<std::uint32_t> recomp_ios_render_guest_token{0}; std::atomic<unsigned> recomp_ios_guest_low_applied{0}; }
void iosGuestQosFromWait(std::uint32_t token) noexcept {
    if (!token || token == recomp_ios_render_guest_token.load(std::memory_order_relaxed)) return;
    static thread_local std::uint64_t next = 0; static thread_local int applied = -1;
    if (!iosQosRecheckDue(next)) return; // (ios-qos-recheck)
    static const std::string home = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/Library/r/private-trace/";
    static const std::string low = home + "ios-qos-guest-low";
    const int want = recomp_ios_switches_access(low.c_str(), F_OK) == 0 ? 2 : 0;
    if (want == applied || (applied == -1 && want == 0)) return;
    const int r = pthread_set_qos_class_self_np(want == 2 ? QOS_CLASS_UTILITY : QOS_CLASS_DEFAULT, 0);
    if (want == 2) recomp_ios_guest_low_applied.fetch_add(1, std::memory_order_relaxed);
    else recomp_ios_guest_low_applied.fetch_sub(1, std::memory_order_relaxed);
    if (applied == -1) std::fprintf(stderr, "APPLE_IOS_QOS thread=guest token=%08x want=%d result=%d\n", token, want, r);
    applied = want;
}
void iosMainQosFromWait() noexcept {
    const std::uint32_t token = recomp::bootstrap::currentGuestThreadToken();
    if (token != 0x63001000u) { iosGuestQosFromWait(token); return; }
    static thread_local std::uint64_t next = 0; static thread_local int applied = -1;
    if (!iosQosRecheckDue(next)) return; // (ios-qos-recheck)
    static const std::string home = std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/Library/r/private-trace/";
    static const std::string ui = home + "ios-qos-main", low = home + "ios-qos-main-low", keep = home + "ios-qos-main-keep",
        dflt = home + "ios-qos-main-default"; // (Claude 2026-10-20: default even with -low on: the reversed order of a -low A/B)
    const int want = recomp_ios_switches_access(ui.c_str(), F_OK) == 0 ? 1 : recomp_ios_switches_access(dflt.c_str(), F_OK) == 0 ? 0 :
        recomp_ios_switches_access(low.c_str(), F_OK) == 0 ? 2 :
        recomp_ios_switches_access(keep.c_str(), F_OK) == 0 ? 0 :
        // no switch: utility while the frame is GPU-bound (100 % and above; c199/c200: utility -2.0 / -4.4 ms in both phase
        // orders), default below 100 %, where main itself limits (c197/c198: default -5.2 ms pooled, 79 vs 54 % in band)
        recomp_ios_render_sixteenths.load(std::memory_order_relaxed) < 16 ? 0 : 2;
    if (want == applied) return;
    const int r = pthread_set_qos_class_self_np(want == 1 ? QOS_CLASS_USER_INTERACTIVE : want == 2 ? QOS_CLASS_UTILITY : QOS_CLASS_DEFAULT, 0);
    std::fprintf(stderr, "APPLE_IOS_QOS thread=main-wait token=63001000 want=%d result=%d\n", want, r);
    applied = want;
}
}
#define RECOMP_IOS_MAIN_QOS_FROM_WAIT() iosMainQosFromWait()
#else
#define RECOMP_IOS_MAIN_QOS_FROM_WAIT() ((void)0)
#endif
// async-wake: the switch state (set by the renderer's window poll, awWindow) and the waker's tid (trace_runner's ranking).
extern "C" { std::atomic<unsigned> recomp_aw_mode{0}; std::atomic<unsigned> recomp_aw_spin_us{500}; std::atomic<int> recomp_aw_waker_tid{0}; }
// async-wake: notifies handed to the waker and not yet completed (the renderer keeps the FIFO path on until they drain).
extern "C" unsigned long long recomp_aw_outstanding() noexcept;
// async-wake: cumulative counters for the renderer's RENDER_DIAG_AW line (out[0..13], see aw_waker.h Stats).
extern "C" void recomp_aw_stats(unsigned long long* out) noexcept {
    const auto& s = recomp_aw::stats();
    const std::atomic<unsigned long long>* v[] = {&s.deduped, &s.handed, &s.inlined, &s.restarts, &s.parks, &s.rescues, &s.rescued,
        &s.full, &s.issueNs, &s.awakeNs, &s.lagNs, &s.lagMaxNs, &s.backlogMax, &s.notifies};
    for (unsigned i = 0; i < 14; ++i) out[i] = v[i]->load(std::memory_order_relaxed);
}
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v240_baseline; // v240 A/B (generic_renderer.cpp)
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v241_baseline; // v241 A/B (generic_renderer.cpp)
// v241: host threads that never stop for a world stop (the renderer thread, the presenter) register here; during every
// outermost exclusive scope they run at nice +10 (restored to 0 when it ends), so the guest threads the stop waits for get
// the big cores first. v240 outdoor timelines: time-to-safepoint + enqueue ~450 us of a ~1 ms stop, 2.14 stops/frame, with
// cpu3-6 ~90% busy. A/B: diag-v241-baseline (no yield).
namespace { std::array<std::atomic<int>, 4> stopYieldTids{}; std::atomic<unsigned long long> stopYields{0}; bool stopYieldApplied = false; }
extern "C" void recomp_register_stop_yield_thread(int tid) {
    for (auto& t : stopYieldTids) { int expected = 0; if (t.compare_exchange_strong(expected, tid)) return; if (expected == tid) return; }
}
namespace recomp::bootstrap {
namespace { thread_local bool poolNextEnqueue = false; std::atomic<unsigned long long> pooledTasks{0}, pooledCreated{0}, pooledFallbacks{0}; }
void enqueueNextOnPooledHost() noexcept { poolNextEnqueue = true; }
PooledHostStats pooledHostStats() noexcept { return {pooledTasks.load(), pooledCreated.load(), pooledFallbacks.load(), stopYields.load()}; }
}
// v110: runtime switches (files/private-trace/diag-...); host tests point this at a scratch directory.
#if !defined(RECOMP_DIAG_SWITCH_DIR)
#define RECOMP_DIAG_SWITCH(name) RECOMP_APP_PATH("/private-trace/" name) // data-paths (host tests: recomp_app_dir.h)
#endif
namespace recomp::bootstrap {
// v110 (declared by callers: trace_runner.cpp, host tests): names the exclusive scopes this host thread
// opens next (RENDER_DIAG_EXCL_SITE) and returns the previous name; nullptr clears. The string must live
// for the whole run.
const char* noteGuestExclusiveSite(const char* site) noexcept;
// v110: true while any task owns an exclusive scope (the world is stopped); vulkan_engine.cpp's fence timing.
bool guestExclusiveScopeActive() noexcept;
// v110: runs entry as ONE exclusive scope of the calling task (a span with no budget or scopeActive of its
// own), so the non-yielding scopes and the worker handoff inside it nest instead of each stopping and
// restarting the world. With files/private-trace/diag-irq-split-scopes (v109), entry runs unwrapped.
// false: refused (no live task, stop observed, inside a scope or handoff); entry did not run.
bool runNativeExclusiveSpan(GuestCooperativeExecutor& executor, const GuestExecutionGuard::Entry& entry);
bool exclusiveSpanEnabled() noexcept;
namespace {
thread_local const char* exclusiveSiteContext = nullptr;
bool diagSwitchPresent(const char* path) noexcept { return ::access(path, F_OK) == 0; }
} // namespace
const char* noteGuestExclusiveSite(const char* site) noexcept {
    const char* previous = exclusiveSiteContext;
    exclusiveSiteContext = site;
    return previous;
}
bool exclusiveSpanEnabled() noexcept {
    static const bool enabled = !diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-irq-split-scopes"));
    return enabled;
}
// v117: files/private-trace/diag-exec-slow-zero-yield restores v115's delay path for zero-length delays.
bool zeroYieldFastEnabled() noexcept {
    static const bool enabled = !diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-slow-zero-yield"));
    return enabled;
}
// v184: files/private-trace/diag-exec-always-notify restores v183's unconditional notify in wakeGuestWaiters.
bool alwaysNotifyWakes() noexcept {
    static const bool enabled = diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-always-notify"));
    return enabled;
}
// v191: files/private-trace/diag-exec-irq-scoped: the tile-IRQ exclusive spans ("retire-tile-irq", "retire-tile-tail")
// stop only permit domains 2, 6 and 7 (the D3D render thread, the CPU2 tile worker and 641c8000, in shared or split
// mode) plus the span owner's and the IRQ worker's; every other domain keeps running (on the console the CPU2
// interrupt preempts only CPU2). Off: every exclusive scope stops every domain (v110-v190).
bool irqScopedEnabled() noexcept {
    static const bool enabled = diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-irq-scoped"));
    return enabled;
}
// v185: files/private-trace/diag-exec-notify-under-lock restores v184 (the notify inside the scheduler lock).
bool notifyUnderLock() noexcept {
    static const bool enabled = diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-notify-under-lock"));
    return enabled;
}
} // namespace recomp::bootstrap
namespace recomp::bootstrap {
namespace {
struct Cancelled final {};
struct ThreadExited final {};

constexpr std::size_t MaximumCheckpointLabelBytes = 128; // recomp_guest_execution_checkpoint_named's bound.

constexpr const char* guestThreadModeName() noexcept {
    return RECOMP_GUEST_THREADS == 1 ? "gil" : RECOMP_GUEST_THREADS == 2 ? "lanes" : "cooperative";
}

__attribute__((format(printf, 1, 2))) void executorLog(const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_INFO, "RecompRenderer", format, arguments);
#else
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
#endif
    va_end(arguments);
}
// quiet-log (diag_log_sample.h): executorLog for the high-rate line types nothing reads; not logged while
// files/private-trace/diag-quiet-log exists. Out of line: a call site keeps its CFG (and IR-PGO record).
__attribute__((format(printf, 1, 2), noinline)) void executorLogQuiet(const char* format, ...) noexcept {
    if (recomp::diag::quietSkip()) return;
    va_list arguments;
    va_start(arguments, format);
    recomp::diag::quietLogEmit(4 /* ANDROID_LOG_INFO */, "RecompRenderer", format, arguments);
    va_end(arguments);
}

std::string_view boundedCheckpointLabel(const char* label, std::size_t length) noexcept {
    return label ? std::string_view(label, std::min(length, MaximumCheckpointLabelBytes)) : std::string_view{};
}

// Start identity described by the creator right before enqueue/run (noteGuestThreadStart).
struct StartNote {
    std::uint32_t token{0}, entry{0}, context{0}, xapi{0};
    const std::uint8_t* base{nullptr};
};
thread_local StartNote pendingStartNote{};
thread_local const char* pendingWaitSite = nullptr;

StartNote takeStartNote(std::uint32_t token) noexcept {
    StartNote note{};
    if (token != 0 && pendingStartNote.token == token) note = pendingStartNote;
    pendingStartNote = StartNote{};
    note.token = token;
    return note;
}

std::uint32_t peekGuestWord(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | __atomic_load_n(bytes + i, __ATOMIC_RELAXED);
    return value;
}

// v218: the tile-IRQ workers are created ~46/s under a handful of recycled tokens: a token's first 2 lines of each
// kind, then every 64th (diag_log_sample.h; diag-v218-baseline logs every task again).
bool taskLineSampled(std::uint32_t token, unsigned kind) noexcept {
    if (recomp::diag::logAll.load(std::memory_order_relaxed)) return true;
    static std::mutex m; static std::unordered_map<std::uint64_t, std::uint64_t> seen;
    try {
        std::lock_guard l(m);
        const auto n = ++seen[(std::uint64_t{token} << 1) | kind];
        return n <= 2 || n % 64 == 0;
    } catch (...) { return true; }
}
// One line per created task, every mode (the integrator derives the lanes policy from these). v218: sampled per token.
void logTaskCreated(const StartNote& note, std::int32_t priority, unsigned domain) noexcept {
    if (!taskLineSampled(note.token, 0)) return;
    const auto context = note.context;
    if (note.base && (context & 3U) == 0 && context >= 0x10000U && context <= 0xFFFF0000U) {
        executorLog("RENDER_DIAG_TASK_CREATED token=%08x entry=%08x context=%08x xapi=%08x priority=%d "
                    "domain=%u mode=%s ctx_words=%08x,%08x",
                    note.token, note.entry, context, note.xapi, priority, domain, guestThreadModeName(),
                    peekGuestWord(note.base + context), peekGuestWord(note.base + context + 4));
    } else {
        executorLog("RENDER_DIAG_TASK_CREATED token=%08x entry=%08x context=%08x xapi=%08x priority=%d "
                    "domain=%u mode=%s ctx_words=none",
                    note.token, note.entry, context, note.xapi, priority, domain, guestThreadModeName());
    }
}

std::mutex domainPolicyMutex;
GuestThreadDomainPolicy domainPolicy;

[[maybe_unused]] unsigned evaluateDomainPolicy(const StartNote& note, unsigned domains) noexcept {
    GuestThreadDomainPolicy policy;
    try {
        std::lock_guard lock(domainPolicyMutex);
        policy = domainPolicy;
    } catch (...) {
        return 0;
    }
    if (!policy) return 0;
    try {
        const auto domain = policy(note.token, note.entry, note.context);
        return domain < domains ? domain : 0U;
    } catch (...) {
        return 0;
    }
}
} // namespace

void setGuestThreadDomainPolicy(GuestThreadDomainPolicy policy) {
    std::lock_guard lock(domainPolicyMutex);
    domainPolicy = std::move(policy);
}

// ios-wait-slots (Claude 2026-10-21; DIAGNOSTIC, two relaxed stores per parallel wait): c412 hung with every guest thread in
// waitUntil. Each host thread that waits takes one slot (token, the object the registry set in timeline::waitObject, the wait
// start). recomp_ios_wait_slots() lists the waiting ones with the first 16 bytes of the object when it is a guest address; the
// stall watcher (ios_sampler.h) prints them after each capture (APPLE_IOS_STALL_WAIT).
namespace iosWaits {
struct Slot { std::atomic<std::uint32_t> token{0}, object{0}; std::atomic<std::uint64_t> sinceNs{0}; };
inline Slot slots[160]; inline std::atomic<unsigned> used{0}; inline std::atomic<const std::uint8_t*> guestBase{nullptr};
inline std::uint64_t nowNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Scope {
    Slot* slot=nullptr;
    explicit Scope(std::uint32_t token) noexcept {
        static thread_local int mine=-1;
        if(mine<0){const unsigned i=used.fetch_add(1,std::memory_order_relaxed);mine=i<160?int(i):160;}
        if(mine>=160)return;
        slot=&slots[mine];slot->token.store(token,std::memory_order_relaxed);
        slot->object.store(recomp::diag::timeline::waitObject,std::memory_order_relaxed);
        slot->sinceNs.store(nowNs(),std::memory_order_release);
    }
    ~Scope(){if(slot)slot->sinceNs.store(0,std::memory_order_release);}
};
}
extern "C" unsigned recomp_ios_wait_slots(std::uint32_t* tokens,std::uint32_t* objects,std::uint64_t* waitedNs,std::uint8_t (*head)[16],unsigned max) noexcept {
    const auto now=iosWaits::nowNs();unsigned n=0;
    const auto* base=iosWaits::guestBase.load(std::memory_order_relaxed);
    const unsigned used=std::min(iosWaits::used.load(std::memory_order_relaxed),160u);
    for(unsigned i=0;i<used && n<max;++i) {
        const auto since=iosWaits::slots[i].sinceNs.load(std::memory_order_acquire);if(!since)continue;
        tokens[n]=iosWaits::slots[i].token.load(std::memory_order_relaxed);objects[n]=iosWaits::slots[i].object.load(std::memory_order_relaxed);
        waitedNs[n]=now>since?now-since:0;std::memset(head[n],0,16);
        if(base && objects[n]>=0x10000u && objects[n]<0xFFFF0000u)std::memcpy(head[n],base+objects[n],16);
        ++n;
    }
    return n;
}

// ios-stall-mem (Claude 2026-10-21; DIAGNOSTIC): raw guest bytes for the stall watcher (RECOMP_IOS_STALL_MEM), e.g. the game's
// job-completion queue main waited on in the c322/c412 hangs (0x83CB64F8: outstanding counter, ring, waiting flag; event at +4112).
extern "C" unsigned recomp_ios_guest_read(std::uint32_t address,std::uint8_t* out,unsigned size) noexcept {
    const auto* base=iosWaits::guestBase.load(std::memory_order_relaxed);
    if(!base || !size || size>256 || address<0x10000u || address>0xFFFF0000u-size)return 0;
    std::memcpy(out,base+address,size);return size;
}
void noteGuestThreadStart(std::uint32_t token, std::uint32_t entry, std::uint32_t context,
                          std::uint32_t xapiStartup, const std::uint8_t* guestBase) noexcept {
    pendingStartNote = StartNote{token, entry, context, xapiStartup, guestBase};
    if(guestBase)iosWaits::guestBase.store(guestBase,std::memory_order_relaxed); // (ios-wait-slots)
}

void noteGuestWaitSite(const char* site) noexcept { pendingWaitSite = site; }

#if RECOMP_GUEST_THREADS == 0
// ============================================================================================
// Cooperative mode: the serialized executor exactly as before. WP1 adds only the task-creation
// log, the start note and the thread-local identity; scheduling is untouched.
// ============================================================================================
namespace {
thread_local std::uint32_t cooperativeToken = 0; // Task whose code runs on this host thread.
}

void wakeGuestWaiters(std::uint32_t) noexcept {}
} // namespace recomp::bootstrap
extern "C" unsigned long long recomp_aw_outstanding() noexcept { return 0; } // async-wake (no waker in cooperative builds)
namespace recomp::bootstrap {
void wakeAllGuestWaiters() noexcept {}
std::uint32_t currentGuestThreadToken() noexcept { return cooperativeToken; }
const char* exchangeGuestHostActivity(const char*) noexcept { return nullptr; } // v226 (parallel builds only)
GuestParallelStats guestParallelStats() noexcept { return {}; }

thread_local GuestCooperativeExecutor::Current* GuestCooperativeExecutor::current_ = nullptr;

GuestCooperativeExecutor::GuestCooperativeExecutor(ThreadFactory factory)
    : factory_(factory ? std::move(factory) : ThreadFactory([](Entry entry) {
          return std::thread(std::move(entry));
      })) {}

bool GuestCooperativeExecutor::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

bool GuestCooperativeExecutor::terminateCurrentThread(std::uint32_t token,const Ready& commitExit) {
    if (!current_ || current_->executor!=this || !current_->lock->owns_lock() ||
        !GuestExecutionGuard::isActive() || terminal_ || nativeNonYielding_ ||
        nativeHandoff_ || !commitExit || jobs_[current_->index]->token!=token ||
        jobs_[current_->index]->finished) return false;
    if (!commitExit()) return false;
    throw ThreadExited{};
}

GuestCooperativeResult GuestCooperativeExecutor::run(
        std::uint32_t mainToken,
        const Entry& entry,
        const GuestCooperativeLimits& limits,
        const BindThread& bindThread,
        const GuestExecutionGuard::FunctionTraceSink& traceSink) {
    if (mainToken == 0 || !entry || !bindThread) {
        return {{GuestExecutionStopReason::InvalidEntry, 0, "Executor entry or binding is empty"}};
    }
    if (limits.functionEntries == 0 || limits.quantum == 0 || limits.quantum > 1000000
            || limits.tasks == 0 || limits.tasks > MaximumTasks) {
        return {{GuestExecutionStopReason::InvalidBudget, 0, "Executor limits are outside their bounds"}};
    }
    if (current_ != nullptr || GuestExecutionGuard::isActive()) {
        return {{GuestExecutionStopReason::NestedRun, 0, "Nested cooperative execution is not allowed"}};
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return {{GuestExecutionStopReason::NestedRun, 0, "Executor already has an active run"}};
    }
    std::unique_lock lock(gate_);
    entries_ = switches_ = waits_ = 0;
    workersEntered_ = 0;
    boundToken_ = 0;
    jobCount_ = selected_ = 0;
    reschedule_ = false;
    nativeNonYielding_.reset();
    nativeHandoff_.reset();
    terminal_.reset();
    limits_ = limits;
    try {
        bindThread_ = bindThread;
        traceSink_ = traceSink;
        auto main = std::make_unique<Job>();
        main->token = mainToken;
        main->entry = entry;
        jobs_[0] = std::move(main);
        jobCount_ = 1;
        logTaskCreated(takeStartNote(mainToken), 0, 0); // WP1: creation evidence, no scheduling effect.
        execute(0, lock);
        changed_.wait(lock, [&] { return terminal_.has_value(); });
    } catch (...) {
        // Setup/coordination failures are distinct from a deliberate guest stop.
        if (!terminal_) {
            terminal_.emplace(GuestExecutionResult{
                    GuestExecutionStopReason::HostException, entries_, "Executor setup failed"});
        }
        notifySchedulingChange();
    }
    // Waiting tasks need this gate to unwind. Never join while holding it.
    lock.unlock();
    for (std::size_t i = 1; i < jobCount_; ++i) {
        if (jobs_[i]->host.joinable()) { jobs_[i]->host.join(); }
    }
    lock.lock();
    auto result = GuestCooperativeResult{std::move(*terminal_),
            jobCount_ == 0 ? 0 : jobCount_ - 1, workersEntered_, switches_, waits_};
    result.execution.completedFunctionEntries = entries_;
    for (auto& job : jobs_) { job.reset(); }
    jobCount_ = 0;
    bindThread_ = {};
    traceSink_ = {};
    terminal_.reset();
    running_.store(false, std::memory_order_release);
    return result;
}

std::uint32_t GuestCooperativeExecutor::currentTaskToken() const noexcept {
    if(!current_ || current_->executor!=this || !current_->lock->owns_lock() ||
       !GuestExecutionGuard::isActive() || terminal_ || jobs_[current_->index]->finished)return 0;
    return jobs_[current_->index]->token;
}
bool GuestCooperativeExecutor::parkedTask(std::uint32_t token) const noexcept {
    if(!currentTaskToken() || !token)return false;
    for(std::size_t i=0;i<jobCount_;++i)
        if(jobs_[i]->token==token)return jobs_[i]->entered &&
            !jobs_[i]->finished && bool(jobs_[i]->ready);
    return false;
}

bool GuestCooperativeExecutor::enqueue(std::uint32_t token, const Entry& entry, std::int32_t priority) {
    const auto note = takeStartNote(token); // WP1: consumed on every call, logged only on success.
    if (current_ == nullptr || current_->executor != this || !current_->lock->owns_lock()
            || !GuestExecutionGuard::isActive() || terminal_ || token == 0 || !entry
            || !validPortableSchedulerPriority(priority,limits_.nativeHighPriorityTiers)) {
        return false;
    }
    // DIAGNOSTIC v690: recycle a FINISHED job slot (same token first, else any
    // finished non-main slot at capacity). Its host already released the gate.
    std::size_t reuse = MaximumTasks;
    for (std::size_t i = 0; i < jobCount_; ++i) {
        if (jobs_[i]->token == token) { if (i == 0 || !jobs_[i]->finished) { return false; } reuse = i; }
    }
    if (reuse == MaximumTasks && jobCount_ >= limits_.tasks)
        for (std::size_t i = 1; i < jobCount_; ++i) if (jobs_[i]->finished) { reuse = i; break; }
    if (reuse == MaximumTasks && jobCount_ >= limits_.tasks) { return false; }
    const auto index = reuse == MaximumTasks ? jobCount_ : reuse;
    std::unique_ptr<Job> old;
    if (reuse != MaximumTasks) {
        if (jobs_[index]->host.joinable()) { jobs_[index]->host.join(); }
        old = std::move(jobs_[index]);
    }
    try {
        auto job = std::make_unique<Job>();
        job->token = token;
        job->entry = entry;
        job->priority = priority;
        jobs_[index] = std::move(job);
        auto host = factory_([this, index] { worker(index); });
        if (!host.joinable()) {
            jobs_[index] = std::move(old);
            return false;
        }
        jobs_[index]->host = std::move(host);
        if (reuse == MaximumTasks) { ++jobCount_; }
        reschedule_ = true;
        logTaskCreated(note, priority, 0); // WP1: creation evidence, no scheduling effect.
        return true;
    } catch (const std::bad_alloc&) {
        jobs_[index].reset();
        return false;
    } catch (const std::system_error&) {
        jobs_[index].reset();
        return false;
    }
}

bool GuestCooperativeExecutor::boostEventWaiters(const std::uint32_t* tokens,std::size_t count) {
    if (!current_ || current_->executor!=this || !current_->lock->owns_lock() ||
        !GuestExecutionGuard::isActive() || terminal_ || !tokens || !count || count>MaximumTasks) return false;
    std::array<Job*,MaximumTasks> selected{};
    for(std::size_t i=0;i<count;++i) {
        for(std::size_t j=0;j<i;++j) if(tokens[i]==tokens[j]) return false;
        for(std::size_t j=0;j<jobCount_;++j) if(jobs_[j]->token==tokens[i]) {
            auto& job=*jobs_[j];
            if(j==current_->index || job.finished || !job.ready) return false;
            selected[i]=&job;break;
        }
        if(!selected[i]) return false;
    }
    for(std::size_t i=0;i<count;++i) selected[i]->eventWakeBoost=true;
    return true;
}

bool GuestCooperativeExecutor::setPriority(std::uint32_t token, std::int32_t priority) {
    if (current_ == nullptr || current_->executor != this || !current_->lock->owns_lock()
            || !GuestExecutionGuard::isActive() || terminal_ || !validPortableSchedulerPriority(priority,limits_.nativeHighPriorityTiers)) {
        return false;
    }
    for (std::size_t i = 0; i < jobCount_; ++i) {
        if (jobs_[i]->token == token && !jobs_[i]->finished) {
            jobs_[i]->priority = priority;
            // Do not reset the consumed slice: repeated priority writes must
            // not let the current task evade rotation indefinitely.
            return true;
        }
    }
    return false;
}

void GuestCooperativeExecutor::worker(std::size_t index) {
    std::unique_lock lock(gate_);
    jobs_[index]->selected.wait(lock, [&] { return terminal_ || selected_ == index; });
    if (!terminal_) { execute(index, lock); }
    else { jobs_[index]->finished = true; }
}

void GuestCooperativeExecutor::notifySchedulingChange() {
    if (terminal_) {
        changed_.notify_all(); // Coordinator/deadline waiter.
        for (std::size_t i = 0; i < jobCount_; ++i) jobs_[i]->selected.notify_one();
    } else if (selected_ < jobCount_) {
        jobs_[selected_]->selected.notify_one();
    }
}

void GuestCooperativeExecutor::execute(std::size_t index, std::unique_lock<std::mutex>& lock) {
    Current context{this, index, &lock};
    auto* previous = current_;
    current_ = &context;
    GuestExecutionGuard guard;
    auto result = guard.run(limits_.functionEntries, [&] {
        bind(index);
        jobs_[index]->entered=true;
        if (index != 0) { ++workersEntered_; }
        try { jobs_[index]->entry(); }
        catch (const ThreadExited&) {
            // Only terminateCurrentThread can construct this private signal.
            // The registry has already published completion under this gate;
            // finish() now retires the task and selects its real waiters/peers.
        }
    }, [&](std::string_view label) { checkpoint(label); });
    current_ = previous;
    cooperativeToken = 0; // WP1: identity ends with the task's guest code.
    finish(index, std::move(result), lock);
}

void GuestCooperativeExecutor::bind(std::size_t index) {
    const auto token = jobs_[index]->token;
    cooperativeToken = token; // WP1: bind always runs on the task's own host thread.
    bindThread_(token);
    if (boundToken_ != 0 && token != boundToken_) { ++switches_; }
    boundToken_ = token;
}

std::size_t GuestCooperativeExecutor::nextTask(std::size_t after) {
    try {
        if (nativeHandoff_) {
            const auto& handoff=*nativeHandoff_;
            return jobs_[handoff.worker]->finished?handoff.parent:handoff.worker;
        }
        for (std::size_t offset = 1; offset <= jobCount_; ++offset) {
            const auto candidate = (after + offset) % jobCount_;
            auto& job = *jobs_[candidate];
            if (job.finished) { continue; }
            if (job.ready) {
                const auto ready = job.ready; // Keep the invoked callback alive while clearing its slot.
                if (!ready()) {
                    if (job.deadline && std::chrono::steady_clock::now() >= *job.deadline) {
                        // Poll once more at/after expiry if time advanced during the first check.
                        if (!ready()) { throw std::logic_error("Expired wait readiness did not complete"); }
                    } else { continue; }
                }
                job.ready = {};
                job.deadline.reset();
            }
            return candidate;
        }
    } catch (...) {
        if (!terminal_) {
            terminal_.emplace(GuestExecutionResult{GuestExecutionStopReason::HostException,
                    entries_, "Cooperative readiness check failed"});
        }
        notifySchedulingChange();
    }
    return MaximumTasks;
}

std::size_t GuestCooperativeExecutor::selectTask(std::size_t after, std::unique_lock<std::mutex>& lock) {
    try {
        for (;;) {
            const auto next = nextTask(after);
            if (terminal_ || next != MaximumTasks) { return next; }
            std::optional<std::chrono::steady_clock::time_point> earliest;
            for (std::size_t i = 0; i < jobCount_; ++i) {
                const auto& job = *jobs_[i];
                if (!job.finished && job.ready && job.deadline && (!earliest || *job.deadline < *earliest)) {
                    earliest = job.deadline;
                }
            }
            if (!earliest) { return MaximumTasks; }
            // No guest can run. Sleep until the nearest deadline without spinning
            // or holding the gate; the outer process watchdog still bounds this.
            changed_.wait_until(lock, *earliest);
        }
    } catch (...) {
        if (!terminal_) { terminal_.emplace(GuestExecutionResult{GuestExecutionStopReason::HostException,
                entries_, "Cooperative deadline coordination failed"}); }
        notifySchedulingChange();
        return MaximumTasks;
    }
}

bool GuestCooperativeExecutor::waitUntil(const Ready& ready,
        std::optional<std::chrono::steady_clock::time_point> deadline) {
    RECOMP_IOS_MAIN_QOS_FROM_WAIT();
    return waitUntilImpl(ready, deadline, false);
}

bool GuestCooperativeExecutor::delayUntil(std::chrono::steady_clock::time_point deadline) {
    return waitUntilImpl([deadline] { return std::chrono::steady_clock::now() >= deadline; }, deadline, true);
}

bool GuestCooperativeExecutor::runNativeNonYielding(const Entry& entry,std::uint32_t maximumEntries) {
    if (!current_ || current_->executor!=this || !current_->lock->owns_lock() ||
        !GuestExecutionGuard::isActive() || terminal_ || nativeNonYielding_ ||
        !entry || !maximumEntries || maximumEntries>4096) return false;
    nativeNonYielding_=NonYielding{current_->index,entries_,maximumEntries};
    struct Clear { std::optional<NonYielding>& scope;~Clear() { scope.reset(); } } clear{nativeNonYielding_};
    entry();
    return true;
}
// v29: the cooperative gate already serializes every task, so the lock gate is the budget-1 scope.
bool runNativeLockedGate(GuestCooperativeExecutor& executor,const GuestExecutionGuard::Entry& entry) {
    return executor.runNativeNonYielding(entry,1);
}
// v110: the cooperative gate already serializes every task: a span is the plain call.
bool runNativeExclusiveSpan(GuestCooperativeExecutor&,const GuestExecutionGuard::Entry& entry) {
    if (!entry) return false;
    entry();
    return true;
}
bool guestExclusiveScopeActive() noexcept { return false; }
// v43 (declared in trace_runner.cpp): unknown here; callers take the gate itself.
int nativeLockedGateAdmits(GuestCooperativeExecutor&) {return -1;}
bool GuestCooperativeExecutor::runNativeWorkerNext(std::uint32_t token,std::uint32_t maximumEntries) {
    if (!current_ || current_->executor!=this || !current_->lock->owns_lock() ||
        !GuestExecutionGuard::isActive() || terminal_ || nativeNonYielding_ || nativeHandoff_ ||
        !token || !maximumEntries || maximumEntries>4096) return false;
    const auto parent=current_->index;
    std::size_t worker=MaximumTasks;
    for (std::size_t i=1;i<jobCount_;++i) if (jobs_[i]->token==token) {worker=i;break;}
    if (worker==MaximumTasks || worker==parent || jobs_[worker]->entered || jobs_[worker]->finished || jobs_[worker]->ready)
        return false;
    nativeHandoff_=NativeHandoff{parent,worker,entries_,maximumEntries};
    struct Clear {std::optional<NativeHandoff>& scope;~Clear(){scope.reset();}} clear{nativeHandoff_};
    selected_=worker;notifySchedulingChange();
    jobs_[parent]->selected.wait(*current_->lock,[&] {return terminal_ || selected_==parent;});
    if (terminal_) throw Cancelled{};
    bind(parent);
    return jobs_[worker]->finished;
}

bool GuestCooperativeExecutor::waitUntilImpl(const Ready& ready,
        std::optional<std::chrono::steady_clock::time_point> deadline, bool yieldCurrentSlice) {
    if (current_ == nullptr || current_->executor != this || !current_->lock->owns_lock()
            || !GuestExecutionGuard::isActive() || terminal_ || nativeNonYielding_ || nativeHandoff_ || !ready
            || waits_ >= limits_.functionEntries) { return false; }
    ++waits_;
    if (!yieldCurrentSlice && ready()) { return true; }
    const auto index = current_->index;
    jobs_[index]->sliceEntries = 0;
    jobs_[index]->eventWakeBoost = false;
    jobs_[index]->ready = ready;
    jobs_[index]->deadline = deadline;
    try {
        const auto next = selectTask(index, *current_->lock);
        if (terminal_) { throw Cancelled{}; }
        if (next == index) { return true; } // A readiness check completed immediately.
        if (next == MaximumTasks) {
            jobs_[index]->ready = {};
            jobs_[index]->deadline.reset();
            return false;
        }
        selected_ = next;
        notifySchedulingChange();
        jobs_[index]->selected.wait(*current_->lock, [&] { return terminal_ || selected_ == index; });
        if (terminal_) { throw Cancelled{}; }
        bind(index);
        return true;
    } catch (...) {
        jobs_[index]->ready = {};
        jobs_[index]->deadline.reset();
        throw;
    }
}

void GuestCooperativeExecutor::checkpoint(std::string_view label) {
    if (terminal_) { throw Cancelled{}; }
    if (nativeHandoff_ && (current_->index!=nativeHandoff_->worker || entries_-nativeHandoff_->start>=nativeHandoff_->budget)) {
        terminal_.emplace(GuestExecutionResult{GuestExecutionStopReason::FunctionEntryBudgetExceeded,
                entries_,"Exclusive native worker handoff budget or ownership boundary"});
        notifySchedulingChange();throw Cancelled{};
    }
    if (nativeNonYielding_ && current_->index!=nativeNonYielding_->index)
        throw std::logic_error("Native non-yielding scope lost its owning task");
    if (nativeNonYielding_ && entries_-nativeNonYielding_->start>=nativeNonYielding_->budget) {
        terminal_.emplace(GuestExecutionResult{GuestExecutionStopReason::FunctionEntryBudgetExceeded,
                entries_,"Native non-yielding function-entry budget exhausted"});
        notifySchedulingChange();throw Cancelled{};
    }
    if (entries_ >= limits_.functionEntries) {
        terminal_.emplace(GuestExecutionResult{GuestExecutionStopReason::FunctionEntryBudgetExceeded,
                entries_, "Global cooperative function-entry budget exhausted"});
        notifySchedulingChange();
        throw Cancelled{};
    }
    ++entries_;
    if (traceSink_) { traceSink_(label); }
    const auto index = current_->index;
    auto& job = *jobs_[index];
    ++job.sliceEntries;
    if (nativeNonYielding_) return; // Accounting/stop checks above still apply.
    const auto shift = std::min(3,portablePrioritySliceShift(job.priority)+(job.eventWakeBoost?1:0));
    const auto quantum = shift >= 0
            ? std::uint64_t{limits_.quantum} << shift
            : std::max(std::uint64_t{1}, std::uint64_t{limits_.quantum} >> -shift);
    if (!reschedule_ && job.sliceEntries < quantum) { return; }
    reschedule_ = false;
    job.sliceEntries = 0;
    job.eventWakeBoost = false;
    const auto next = nextTask(index);
    if (terminal_) { throw Cancelled{}; }
    if (next == index || next == MaximumTasks) { return; }
    selected_ = next;
    notifySchedulingChange();
    jobs_[index]->selected.wait(*current_->lock, [&] { return terminal_ || selected_ == index; });
    if (terminal_) { throw Cancelled{}; }
    bind(index);
}

void GuestCooperativeExecutor::finish(std::size_t index, GuestExecutionResult result, std::unique_lock<std::mutex>& lock) {
    jobs_[index]->finished = true;
    jobs_[index]->ready = {};
    jobs_[index]->deadline.reset();
    if (!terminal_ && !result.completed()) {
        result.completedFunctionEntries = entries_;
        terminal_.emplace(std::move(result));
    }
    if (!terminal_) {
        selected_ = selectTask(index, lock);
        if (!terminal_ && selected_ == MaximumTasks) {
            bool allFinished = true;
            for (std::size_t i = 0; i < jobCount_; ++i) {
                allFinished = allFinished && jobs_[i]->finished;
            }
            terminal_.emplace(GuestExecutionResult{
                    allFinished ? GuestExecutionStopReason::Completed : GuestExecutionStopReason::NoRunnableTask,
                    entries_, allFinished ? "" : "All remaining tasks wait without a runnable signal producer"});
        }
    }
    notifySchedulingChange();
}

} // namespace recomp::bootstrap

extern "C" void recomp_guest_execution_checkpoint_slow(std::int32_t* countdown, const char* label, std::size_t length) {
    // Cooperative: every entry reaches this path (the attention word is permanently nonzero), so the
    // scheduler sees exactly the old per-entry chain. Re-arm to take the next entry too.
    if (countdown) *reinterpret_cast<recomp::bootstrap::GuestCheckpointCountdown*>(countdown) = 0;
    recomp::bootstrap::GuestExecutionGuard::checkpoint(recomp::bootstrap::boundedCheckpointLabel(label, length));
}

#else
// ============================================================================================
// Parallel modes (gil / lanes). Design: local-analysis/parallel-design/executor.md section 5.
//  - A task needs its domain's permit to run guest code or evaluate a wait predicate.
//  - Waits are evaluated only by the waiter: wake sequence + global epoch (no lost wakeups),
//    bounded poll fallback (0.25 ms doubling to 8 ms) whose completions count as late wakeups.
//  - runNativeNonYielding / runNativeWorkerNext are exclusive scopes: every other task parks at
//    its next checkpoint (safepoint) or stays parked in its wait; no predicate runs meanwhile.
//  - Stops propagate through every blocking point; joins happen only with no lock held.
//  - Lock order: registry R > sched (leaf) > lateMutex / sinkMutex. Nothing is called while
//    sched is held except condition-variable waits.
// ============================================================================================
namespace {
using Clock = std::chrono::steady_clock;
using Entry = GuestExecutionGuard::Entry;
using Ready = GuestCooperativeExecutor::Ready;

// v171 (goal step 3): 64 permit domains (was 8), so diag-domain-each can give every non-renderer guest thread its own.
constexpr unsigned PermitDomains = RECOMP_GUEST_THREADS == 1 ? 1U : 64U;
constexpr std::uint64_t CountdownBatch = 1024;          // Slow path at least this often per task.
constexpr Clock::duration PollInitial = std::chrono::microseconds(250);
constexpr Clock::duration PollMaximum = std::chrono::milliseconds(8);
// job-path: the schedule's off state is exactly the original one.
static_assert(Clock::duration(recomp_jp::ShortInitial) == PollInitial && Clock::duration(recomp_jp::Maximum) == PollMaximum);
constexpr Clock::duration LogInterval = std::chrono::seconds(10);
constexpr std::uint32_t AttentionIdle = 1, AttentionStop = 2, AttentionExclusive = 4, AttentionArm = 8;
// Lanes audit L3: an exclusive scope waits for every other permit holder to reach a safepoint. A
// holder that runs entry-free guest code (a spin loop on another domain's progress) never does, and
// the requester then waits silently forever. Name the holders after this delay, then periodically.
constexpr Clock::duration SafepointReportAfter = std::chrono::milliseconds(250);
constexpr Clock::duration SafepointReportEvery = std::chrono::seconds(2);
constexpr unsigned ExclusiveHandoff = 3;                // counters: gate<=1, <=256, <=4096, handoff
constexpr unsigned ExclusiveSpan = 4;                   // v110 site kind: runNativeExclusiveSpan
// v110: why a task waited for its domain permit (RENDER_DIAG_EXEC_PERMIT).
enum PermitReason : unsigned { PermitRotation, PermitSafepoint, PermitWaitReturn, PermitDelayReturn, PermitStart, PermitReasons };

void raiseAttention(std::uint32_t bits) noexcept {
    __atomic_fetch_or(&recomp_guest_checkpoint_attention, bits, __ATOMIC_SEQ_CST);
}
void lowerAttention(std::uint32_t bits) noexcept {
    __atomic_fetch_and(&recomp_guest_checkpoint_attention, ~bits, __ATOMIC_SEQ_CST);
}
std::uint32_t attentionBits() noexcept {
    return __atomic_load_n(&recomp_guest_checkpoint_attention, __ATOMIC_ACQUIRE);
}
std::uint64_t elapsedNs(Clock::time_point since) noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count();
    return ns > 0 ? static_cast<std::uint64_t>(ns) : 0;
}

struct MaxCounter {
    std::atomic<std::uint64_t> value{0};
    void update(std::uint64_t candidate) noexcept {
        auto current = value.load(std::memory_order_relaxed);
        while (candidate > current && !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {}
    }
};

struct Counters {
    std::atomic<std::uint64_t> slowCheckpoints{0}, permitAcquired{0}, permitContended{0}, permitWaitNs{0}, permitHoldNs{0};
    MaxCounter permitWaitMaxNs, permitHoldMaxNs, maxConcurrent;
    std::atomic<std::uint64_t> rotations{0}, parks{0}, wakeNotified{0}, wakePoll{0}, wakeDeadline{0}, lateWakeups{0};
    std::atomic<std::uint64_t> wakeCalls{0}, wakeAllCalls{0}, safepointParks{0}, safepointNs{0};
    // v184: targeted wakes that notified / skipped the notify (task not blocked in cv.wait); timed parks that ended at
    // their poll timeout with a wake already recorded (a skipped notify that was needed, or a notify that arrived late).
    std::atomic<std::uint64_t> wakeNotifySent{0}, wakeNotifySkipped{0}, wakeAtTimeout{0}, skippedAtTimeout{0};
    std::atomic<std::uint64_t> v346SpinHits{0}, v346SpinMisses{0}; // v346: main's pre-park spins that saw a wake / did not
    MaxCounter safepointMaxNs, exclusiveMaxNs;
    std::array<std::atomic<std::uint64_t>, 4> exclusiveCount{}, exclusiveNs{};
    std::atomic<std::uint64_t> probes{0}, probesConfirmed{0}, boostsParked{0}, boostsPending{0}, delays{0};
    std::atomic<std::uint64_t> fastYields{0}; // v117: zero-length delays that kept the permit (no scheduler lock)
    // v29 lock gate (runNativeLockedGate): count, time inside, time waiting for the lock.
    std::atomic<std::uint64_t> lockGates{0}, lockGateNs{0}, lockGateWaitNs{0};
    MaxCounter lockGateMaxNs;
};

enum class TaskState : std::uint8_t {
    Created, AwaitingPermit, Running, ParkedWait, ParkedDelay, ParkedSafepoint, InHandoff, Finished
};

struct Task {
    std::uint32_t token{0};
    std::size_t index{0};
    unsigned gateTimingTick{0}; // v330c: runNativeLockedGate's 1-in-256 timing tick (own thread only; was a thread_local)
    unsigned domain{0};
    Entry entry;
    std::thread host;
    std::condition_variable cv; // Only this task's own host thread waits on it.
    bool cvWaiting=false; // v184: inside a cv.wait (set and cleared under the scheduler lock; see wakeGuestWaiters)
    std::uint64_t skippedSeq=0; // v185: wakeSeq after the last wake whose notify was skipped
    // Guarded by the scheduler lock.
    TaskState state{TaskState::Created};
    bool armed{false}, entered{false}, finished{false}, inWait{false}, timedPark{false};
    bool observesParking{false}, probeFlag{false};
    std::uint64_t wakeSeq{0};
    std::uint64_t parkSeq{0}, parkEpoch{0}; // Values this task parked on (pending-wake detection).
    Clock::time_point permitSince{};
    // Written by other tasks, read by the owner.
    std::atomic<std::int32_t> priority{0};
    std::atomic<bool> boostPending{false};
    // Written by the handoff parent before the worker's grant (published by the lock).
    bool handoffWorker{false};
    std::uint32_t handoffBudget{0};
    // Confined to the task's own host thread (children: mutated only by the owner, under the lock).
    std::vector<std::shared_ptr<Task>> children;
    std::uint64_t entries{0}, unflushed{0}, sliceStart{0}, handoffEntries{0}, scopeEntries{0};
    GuestCheckpointCountdown* armedCountdown{nullptr};
    std::int32_t armedValue{0};
    std::uint32_t scopeBudget{0};
    bool scopeActive{false}, sliceBoost{false}, rescheduleHint{false}, evaluating{false};
    bool unwinding{false}; // Holds ParallelState::unwindMutex while cancellation unwinds this task.
    // Written by the owner at each slow checkpoint; read by another task's safepoint watchdog (L3).
    std::atomic<const char*> lastLabel{nullptr};
    std::atomic<const char*> hostActivity{nullptr}; // v226 DIAGNOSTIC: the host region this task's thread is in (null: guest code)
    // v44: cumulative permit accounting for RENDER_DIAG_EXEC_TASKS (guarded by the scheduler lock):
    // how long this task held its domain permit, waited for it, and how often it parked at a safepoint.
    std::uint64_t permitHeldNs{0}, permitWaitedNs{0}, safepointParkCount{0};
    // v110 (scheduler lock): permit waits by reason (count, ns), waits/delays entered, and when a handoff
    // granted this worker its permit (worker start latency).
    std::array<std::uint64_t, PermitReasons> reasonWaits{}, reasonWaitNs{};
    std::uint64_t waitCalls{0}, delayCalls{0};
    Clock::time_point handoffGrantedAt{};
    // v151 (scheduler lock): contended acquisitions that were granted while this task waited, the time from the grant
    // (permitSince) to this thread running again, and the bounded pre-park spin (hits: granted while spinning).
    std::uint64_t grantCount{0}, grantLatencyNs{0}, spinHits{0}, spinMisses{0}, spinNs{0};
    Clock::time_point queuedAt{}; // v154 (scheduler lock): when this task last queued for its domain permit
    std::uint64_t rotationsDeferred{0}; // v154: slice ends of this task that kept the permit for a young waiter
    // async-wake (scheduler lock): parallelWait's park waits are numbered (cvGen; seqWait while inside one: its predicate
    // reads wakeSeq); notifiedGen = the park wait wakeGuestWaiters last notified (diag-aw-*: a repeat notify there is skipped).
    std::uint64_t cvGen{0}, notifiedGen{0};
    bool seqWait{false}, notifiedByMain{false}; // (notifiedByMain: that first notify came from main, possibly via the waker)
};

// v110: exclusive-scope accounting per (site context, kind); outermost scopes only (guarded by sched).
struct ExclusiveSite {
    const char* context{nullptr};
    unsigned kind{0};
    std::uint64_t count{0}, totalNs{0}, maxNs{0}, ttsNs{0}, maxTtsNs{0}, parkedNs{0}, parks{0};
    std::uint64_t workerStarts{0}, workerStartNs{0}, maxWorkerStartNs{0};
    // Permit waits (other than safepoint parks) that began while this site owned exclusivity: tasks
    // woken or queued during the stop, kept off their permit by it.
    std::uint64_t blockedWaits{0}, blockedNs{0};
};

struct Domain {
    Task* owner{nullptr};
    std::deque<Task*> queue;
    // v117: queue.size() mirrored after every change under S.sched (syncQueued), read relaxed by the zero-yield fast
    // path. A stale zero only delays a rotation to the task's next checkpoint slice, the v115 behaviour at any other entry.
    std::atomic<std::uint32_t> queued{0};
    // v151: owner mirrored after every change under S.sched (setOwner), read by a permit waiter spinning without the
    // lock. Only a hint: the waiter re-checks `owner` under the lock before it proceeds.
    std::atomic<Task*> ownerView{nullptr};
};
inline void syncQueued(Domain& d) noexcept {
    d.queued.store(static_cast<std::uint32_t>(d.queue.size()), std::memory_order_relaxed);
    // v266: a sleeping Sleep(0) poller may own this domain's permit (its fast path keeps it): wake it so its next Sleep(0)
    // takes the rotating path (poll_sleep_v266.h; nothing when no poller sleeps).
    if (!d.queue.empty()) recomp_v266::beforePark();
}
inline void setOwner(Domain& d, Task* owner) noexcept { d.owner = owner; d.ownerView.store(owner, std::memory_order_release); }

struct ParallelState {
    std::mutex sched; // Leaf lock (see the lock order above).
    std::condition_variable coordinator, exclusiveCv, supervisorCv;
    std::atomic<bool> active{false};
    const GuestCooperativeExecutor* executor{nullptr};
    GuestCooperativeLimits limits{};
    GuestCooperativeExecutor::BindThread bindThread;
    GuestExecutionGuard::FunctionTraceSink traceSink;
    const GuestCooperativeExecutor::ThreadFactory* factory{nullptr};
    std::vector<std::shared_ptr<Task>> tasks;
    // v185: tasks[i]->token for every published slot (dense, so wakeGuestWaiters scans 4-byte tokens instead of
    // dereferencing every Task). Written with tasks[i], under the scheduler lock; a slot past the array is scanned
    // through its Task (taskTokenOf).
    std::array<std::uint32_t, GuestCooperativeExecutor::MaximumTasks> taskTokens{};
    std::array<Domain, PermitDomains> domains{};
    unsigned running{0};
    Task* exclusiveOwner{nullptr};
    std::atomic<Task*> exclusiveOwnerView{nullptr};
    // v191: the permit domains the current exclusive scope stops (bit per domain; all by default). A scoped tile-IRQ
    // span (diag-exec-irq-scoped) stops only the renderer domains and its own; the others keep running.
    std::uint64_t exclusiveDomains{~0ull};
    std::atomic<std::uint64_t> exclusiveDomainsView{~0ull};
    unsigned exclusiveDepth{0};
    std::atomic<bool> handoffActive{false};
    Task* handoffParent{nullptr};
    Task* handoffTarget{nullptr};
    std::uint64_t epoch{0}, activity{0};
    unsigned pendingArm{0};
    std::optional<GuestExecutionResult> terminal;
    std::vector<GuestExecutionResult> secondaryStops;
    std::atomic<std::uint64_t> globalEntries{0}, waits{0}, switches{0};
    std::atomic<std::size_t> workersCreated{0}, workersEntered{0};
    std::atomic<std::uint32_t> lastBound{0};
    bool probeActive{false}, probeDone{false};
    Task* probeInitiator{nullptr};
    unsigned probeRemaining{0};
    std::uint64_t probeActivity{0};
    bool supervisorStop{false};
    std::vector<std::thread> zombies;
    std::thread supervisor;
    Clock::time_point startedAt{}, nextLog{};
    std::mutex sinkMutex; // lanes: serialises the trace sink, which need not be thread-safe.
    // v29: the renderer's host-only inlineGate bodies (budget 1). Outer lock, below the permit and
    // above R (design lock order: permit -> L_gpu -> R -> ... -> sched).
    // v292: on its own cache line (the render thread takes it for every D3D hook body; the sink/unwind/late mutexes and the
    // late sites beside it are other threads' lines).
    alignas(64) std::mutex nativeLockGate;
    // Cancelled tasks unwind one at a time (as under the cooperative gate), so host destructors
    // in guest-facing code never run concurrently after a stop. Taken with no other lock held.
    alignas(64) std::mutex unwindMutex; // (v292: starts the line after nativeLockGate's)
    std::mutex lateMutex;
    std::array<GuestParallelLateSite, 32> lateSites{};
    std::uint64_t lateOverflow{0};
    Counters counters;
    // v110 (sched): per-site exclusive accounting; the site of the scope that owns exclusivity now.
    std::array<ExclusiveSite, 32> exclusiveSites{};
    std::uint64_t exclusiveSiteOverflow{0};
    std::array<std::uint64_t, 5> nestedExclusive{};
    ExclusiveSite* activeSite{nullptr};
};

ParallelState& parallelState() noexcept {
    static ParallelState* const state = new ParallelState(); // Never destroyed: late wake calls stay safe.
    return *state;
}

thread_local Task* currentTask = nullptr;
// v362 (exact, fast_tls_v362.h): the claimed (render) thread reads currentTask and the guard's activeExecution through their
// own addresses, cached by that thread, instead of a TLS resolver call each (liveTask runs for every lock gate and every
// ownsNativeRendering admission check: ~10 times per draw, two resolver calls each in v349). Same variables, same values.
// (The cached addresses live in fastThread.slots: Slot::CurrentTask = &currentTask, Slot::GuardActive = &activeExecution of the
// claimed thread, each written once by that thread.)
// currentTask of the calling thread; `fast` = v362::onFastThread() (only the claimed thread passes true).
[[gnu::always_inline]] inline Task* currentTaskOf(bool fast) noexcept {
#if defined(RECOMP_V362_MUTANT_TASK_ANY_THREAD) // test mutant: the cached address answers on every thread while the switch is on
    fast = recomp::v362::trimsOn();
#endif
    if (!fast) return currentTask;
    void* address = recomp::v362::slotAddress(recomp::v362::Slot::CurrentTask);
    if (!address) [[unlikely]] { address = &currentTask; recomp::v362::setSlotAddress(recomp::v362::Slot::CurrentTask, address); }
    return *static_cast<Task**>(address);
}
// GuestExecutionGuard::isActive() (activeExecution != nullptr) of the calling thread; `fast` = v362::onFastThread().
[[gnu::always_inline]] inline bool guardActive(bool fast) noexcept {
    if (!fast) return GuestExecutionGuard::isActive();
    const void* address = recomp::v362::slotAddress(recomp::v362::Slot::GuardActive);
    if (!address) [[unlikely]] {
        address = recomp_v362_guard_active_slot();
        recomp::v362::setSlotAddress(recomp::v362::Slot::GuardActive, const_cast<void*>(address));
    }
    const void* active; std::memcpy(&active, address, sizeof active); // (the pointer variable's own bytes)
    return active != nullptr;
}

// ---- v110: the scheduler lock's acquisitions, per call site (function:line), by waiting role.
// A contended acquisition spins (try_lock with yields, bounded to ~20 us like the registry's) before the
// futex unless files/private-trace/diag-sched-no-spin exists (v109: straight to the futex). The lock and
// its order are unchanged; only how a waiter waits. RENDER_DIAG_EXEC_SCHED every 10 s (logCounters).
enum SchedRole : unsigned { SchedRoleMain, SchedRoleRender, SchedRoleTile, SchedRoleAudio, SchedRoleOther, SchedRoleHost, SchedRoles };
struct SchedLockSite {
    const char* function;
    int line;
    std::atomic<std::uint64_t> acquisitions{0}, contended{0}, spun{0}, waitNs{0};
    std::array<std::atomic<std::uint64_t>, SchedRoles> roleWaitNs{};
    MaxCounter maxWaitNs;
    SchedLockSite* next{nullptr};
    SchedLockSite(const char* f, int l) noexcept;
};
std::atomic<SchedLockSite*> schedSites{nullptr};
SchedLockSite::SchedLockSite(const char* f, int l) noexcept : function(f), line(l) {
    next = schedSites.load(std::memory_order_relaxed); // static locals: listed on first use
    while (!schedSites.compare_exchange_weak(next, this, std::memory_order_release, std::memory_order_relaxed)) {}
}
bool schedSpinEnabled() noexcept {
    static const bool enabled = !diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-sched-no-spin"));
    return enabled;
}
unsigned schedRole() noexcept {
    const Task* t = currentTask;
    if (!t) return SchedRoleHost;
    return t->token == 0x63001000u ? SchedRoleMain : t->token == 0x6408e000u ? SchedRoleRender :
           t->token == 0x640b1000u ? SchedRoleTile : t->token == 0x64141000u ? SchedRoleAudio : SchedRoleOther;
}
void schedLockContended(std::unique_lock<std::mutex>& lock, SchedLockSite& site) {
    const auto started = Clock::now();
    bool spun = false;
    if (schedSpinEnabled()) {
        for (unsigned attempt = 0; attempt < 256 && !spun; ++attempt) {
            for (unsigned k = 0; k < 16; ++k) {
#if defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
            }
            spun = lock.try_lock();
        }
    }
    if (!spun) lock.lock();
    const auto ns = elapsedNs(started);
    site.contended.fetch_add(1, std::memory_order_relaxed);
    if (spun) site.spun.fetch_add(1, std::memory_order_relaxed);
    site.waitNs.fetch_add(ns, std::memory_order_relaxed);
    site.roleWaitNs[schedRole()].fetch_add(ns, std::memory_order_relaxed);
    site.maxWaitNs.update(ns);
}
std::unique_lock<std::mutex> lockSched(ParallelState& S, SchedLockSite& site) {
    std::unique_lock<std::mutex> lock(S.sched, std::try_to_lock);
    site.acquisitions.fetch_add(1, std::memory_order_relaxed);
    if (!lock.owns_lock()) schedLockContended(lock, site);
    return lock;
}
void relockSched(std::unique_lock<std::mutex>& lock, SchedLockSite& site) {
    site.acquisitions.fetch_add(1, std::memory_order_relaxed);
    if (!lock.try_lock()) schedLockContended(lock, site);
}
#define RECOMP_SCHED_CAT2(a, b) a##b
#define RECOMP_SCHED_CAT(a, b) RECOMP_SCHED_CAT2(a, b)
#define RECOMP_SCHED_SITE RECOMP_SCHED_CAT(schedSite_, __LINE__)
// Declares `name`, a std::unique_lock<std::mutex> owning S.sched (the former std::unique_lock/lock_guard).
#define RECOMP_SCHED_LOCK(name) \
    static SchedLockSite RECOMP_SCHED_SITE{__func__, __LINE__}; \
    auto name = lockSched(S, RECOMP_SCHED_SITE)
#define RECOMP_SCHED_RELOCK(name) \
    do { static SchedLockSite RECOMP_SCHED_SITE{__func__, __LINE__}; relockSched(name, RECOMP_SCHED_SITE); } while (false)

// Every cancellation of a task goes through here: serialise its unwinding, then throw.
[[noreturn]] void throwCancelled() {
    Task* t = currentTask;
    if (t && !t->unwinding) {
        parallelState().unwindMutex.lock(); // Released by runTaskBody after finishTask.
        t->unwinding = true;
    }
    throw Cancelled{};
}

[[noreturn]] void cancelRun(std::unique_lock<std::mutex>& lock) {
    if (lock.owns_lock()) lock.unlock();
    throwCancelled();
}

void releaseUnwind(ParallelState& S, Task& t) noexcept {
    if (!t.unwinding) return;
    t.unwinding = false;
    S.unwindMutex.unlock();
}

GuestExecutionResult stopResult(GuestExecutionStopReason reason, const char* detail, std::uint64_t entries) {
    return GuestExecutionResult{reason, entries, detail};
}

void setTerminalLocked(ParallelState& S, GuestExecutionResult result) {
    if (S.terminal) {
        if (S.secondaryStops.size() < 8) S.secondaryStops.push_back(std::move(result));
        return;
    }
    S.terminal = std::move(result);
    { // v192 DIAGNOSTIC: the first terminal cause, its thread and the exclusive state at that moment
        const Task* self = currentTask;
        executorLog("RENDER_DIAG_EXEC_FIRST_STOP reason=%u detail=%s token=%08x domain=%u exclusive_owner=%08x exclusive_domains=%016llx",
                    unsigned(S.terminal->reason), S.terminal->detail.empty() ? "-" : S.terminal->detail.c_str(), self ? self->token : 0u,
                    self ? self->domain : 0u, S.exclusiveOwner ? S.exclusiveOwner->token : 0u,
                    static_cast<unsigned long long>(S.exclusiveDomains));
    }
    raiseAttention(AttentionStop);
    for (auto& task : S.tasks) if (task) task->cv.notify_all();
    S.exclusiveCv.notify_all();
    S.coordinator.notify_all();
}

std::uint64_t flushEntries(ParallelState& S, Task& t) noexcept {
    if (t.unflushed) {
        S.globalEntries.fetch_add(t.unflushed, std::memory_order_relaxed);
        t.unflushed = 0;
    }
    return S.globalEntries.load(std::memory_order_relaxed);
}

void grantLocked(ParallelState& S, Task& t) {
    setOwner(S.domains[t.domain], &t);
    ++S.running;
    S.counters.maxConcurrent.update(S.running);
    t.state = TaskState::Running;
    t.permitSince = Clock::now();
    S.counters.permitAcquired.fetch_add(1, std::memory_order_relaxed);
}

// v191: whether the current exclusive scope stops domain `index` (always true outside a scoped IRQ span).
inline bool domainStopped(const ParallelState& S, unsigned index) noexcept { return (S.exclusiveDomains >> (index & 63)) & 1; }
inline bool domainStoppedView(const ParallelState& S, unsigned index) noexcept {
    return (S.exclusiveDomainsView.load(std::memory_order_acquire) >> (index & 63)) & 1;
}
std::uint64_t scopedExclusiveMask(const Task& t) noexcept {
    if (!irqScopedEnabled() || !exclusiveSiteContext || PermitDomains < 8) return ~0ull;
    if (std::strcmp(exclusiveSiteContext, "retire-tile-irq") && std::strcmp(exclusiveSiteContext, "retire-tile-tail")) return ~0ull;
    return (1ull << 2) | (1ull << 6) | (1ull << 7) | (1ull << (t.domain & 63));
}
void setExclusiveDomainsLocked(ParallelState& S, std::uint64_t mask) noexcept {
    S.exclusiveDomains = mask;
    S.exclusiveDomainsView.store(mask, std::memory_order_release);
}

void grantNextLocked(ParallelState& S, Domain& d) {
    if (d.owner || d.queue.empty()) return;
    Task* head = d.queue.front();
    // Exclusive scope: no ordinary grants in the domains it stops (v191: a scoped IRQ span leaves the others running).
    if (S.exclusiveOwner && S.exclusiveOwner != head && domainStopped(S, unsigned(&d - S.domains.data()))) return;
    d.queue.pop_front();
    syncQueued(d);
    grantLocked(S, *head);
    head->cv.notify_all();
}

// A new task becomes eligible only at its creator's next scheduling boundary (next checkpoint,
// park, rotation or finish), like the cooperative "enqueue never switches inline" contract.
void armChildrenLocked(ParallelState& S, Task& t) {
    if (t.children.empty()) return;
    for (auto& child : t.children) {
        if (child->finished || child->armed || child->state != TaskState::Created) continue;
        child->armed = true;
        child->state = TaskState::AwaitingPermit;
        auto& d = S.domains[child->domain];
        d.queue.push_back(child.get());
        syncQueued(d);
        ++S.activity;
        grantNextLocked(S, d);
    }
    t.children.clear();
    if (S.pendingArm && !--S.pendingArm) lowerAttention(AttentionArm);
}

// v224 DIAGNOSTIC (scheduler lock): which task completes each world stop (its release makes the stopping owner's
// predicate true), after how long, with the last slow-path label it passed; and the owner's wake after that release.
struct LastStopper { std::uint32_t token{0}; const char* label{nullptr}; std::uint64_t count{0}, ttsNs{0}, maxNs{0}; };
struct StopAudit {
    bool active{false}; Task* owner{nullptr}; Clock::time_point started{}, doneAt{}; bool done{false};
    // v226: each permit holder's host activity when the stop began (the last stopper's is what it was finishing)
    std::array<std::pair<const Task*, const char*>, 64> atStart{}; unsigned atStartCount{0};
    std::array<LastStopper, 16> last{}; std::uint64_t stops{0}, ownerWakeNs{0}, ownerWakeMaxNs{0}, noneNeeded{0};
} stopAudit;
void noteStopReleaseLocked(ParallelState& S, Task& t) {
    auto& a = stopAudit;
    if (!a.active || a.done || &t == a.owner || S.running > 1) return;
    a.done = true; a.doneAt = Clock::now();
    const auto ns = elapsedNs(a.started);
    const char* label = "guest"; // v226: keyed by the host activity at the stop's start (was: the last slow-path label)
    for (unsigned i = 0; i < a.atStartCount; ++i) if (a.atStart[i].first == &t) { if (a.atStart[i].second) label = a.atStart[i].second; break; }
    LastStopper* slot = nullptr;
    for (auto& e : a.last) if (e.token == t.token && e.label == label) { slot = &e; break; }
    if (!slot) for (auto& e : a.last) if (!e.count) { slot = &e; slot->token = t.token; slot->label = label; break; }
    if (!slot) slot = &a.last.back(); // (overflow bucket keeps its first identity)
    ++slot->count; slot->ttsNs += ns; slot->maxNs = std::max(slot->maxNs, ns);
}
void releasePermitLocked(ParallelState& S, Task& t, TaskState next) {
    armChildrenLocked(S, t);
    auto& d = S.domains[t.domain];
    t.state = next;
    if (d.owner != &t) return;
    setOwner(d, nullptr);
    --S.running;
    noteStopReleaseLocked(S, t); // v224
    const auto held = elapsedNs(t.permitSince);
    S.counters.permitHoldNs.fetch_add(held, std::memory_order_relaxed);
    S.counters.permitHoldMaxNs.update(held);
    t.permitHeldNs += held;
    grantNextLocked(S, d);
    if (S.exclusiveOwner) S.exclusiveCv.notify_all();
}

// v151: how long the next-in-line permit waiter spins before it parks (Lea: spin briefly, then block). v148 device
// profile (prologue): the render thread rotated ~220/s and waited ~1.5 ms each time while its domain-2 peers held the
// permit ~0.5 ms per turn; the render thread's only core is the X3, which idles (and can be paused by core_ctl) as soon
// as the thread sleeps, so each handoff may pay a deep-idle exit. Spinning keeps the waiter's core awake for short peer
// turns. Only the queue head spins (one spinner per domain); safepoint parks (exclusive spans, ~3 ms) never spin.
// v153: OFF by default. The v152 device A/B falsified the latency hypothesis: grant -> running was ~140 us parked or
// spinning, rotation waits are the peers' real turns, frame 52.8 vs 53.2 ms. Opt-in: diag-exec-permit-spin (500 us),
// -spin-short (100 us), -spin-long (2 ms); diag-exec-permit-no-spin still forces 0.
std::uint64_t permitSpinNs() noexcept {
    static const std::uint64_t ns = diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-permit-no-spin")) ? 0
        : diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-permit-spin-long")) ? 2000000
        : diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-permit-spin-short")) ? 100000
        : diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-permit-spin")) ? 500000 : 0;
    return ns;
}

void acquirePermitLocked(ParallelState& S, Task& t, std::unique_lock<std::mutex>& lock, bool front = false,
        PermitReason reason = PermitStart) {
    if (S.terminal) cancelRun(lock);
    auto& d = S.domains[t.domain];
    // v191: outside the domains a scoped span stops, a task may take its free permit (a task's first grant still waits
    // for every exclusive scope: defence in depth, as a new task is armed only at its creator's next checkpoint or by
    // the IRQ handoff).
    if (!d.owner && (S.exclusiveOwner == &t ||
            ((!S.exclusiveOwner || (reason != PermitStart && !domainStopped(S, t.domain))) && d.queue.empty()))) {
        grantLocked(S, t);
        return;
    }
    S.counters.permitContended.fetch_add(1, std::memory_order_relaxed);
    recomp::diag::timeline::mark(recomp::diag::timeline::PermitWaitBegin, std::uint16_t(reason)); // v159
    const auto started = Clock::now();
    auto* const stoppedBy = reason != PermitSafepoint && S.exclusiveOwner && S.exclusiveOwner != &t && domainStopped(S, t.domain)
        ? S.activeSite : nullptr;
    t.state = TaskState::AwaitingPermit;
    t.queuedAt = started; // v154
    if (front) d.queue.push_front(&t); else d.queue.push_back(&t);
    syncQueued(d);
    grantNextLocked(S, d);
    bool spun = false;
    if (d.owner != &t && reason != PermitSafepoint && !d.queue.empty() && d.queue.front() == &t) {
        if (const auto budget = permitSpinNs()) { // v151: the lock is released while spinning (leaf rule kept)
            spun = true;
            const auto spinStart = Clock::now();
            const auto until = spinStart + std::chrono::nanoseconds(budget);
            lock.unlock();
            for (unsigned i = 1; d.ownerView.load(std::memory_order_acquire) != &t; ++i) {
#if defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
                if ((i & 15) == 0 && Clock::now() >= until) break;
            }
            RECOMP_SCHED_RELOCK(lock);
            t.spinNs += elapsedNs(spinStart);
            if (d.owner == &t) ++t.spinHits; else ++t.spinMisses;
        }
    }
    t.cvWaiting = true; // v184
    t.cv.wait(lock, [&] { return d.owner == &t || S.terminal.has_value(); });
    t.cvWaiting = false;
    if (d.owner == &t) { // v151: grant -> running again (wake-up latency when parked, detection when spinning)
        ++t.grantCount;
        t.grantLatencyNs += elapsedNs(t.permitSince);
    }
    (void)spun;
    if (d.owner != &t) {
        d.queue.erase(std::remove(d.queue.begin(), d.queue.end(), &t), d.queue.end());
        syncQueued(d);
        cancelRun(lock);
    }
    const auto waited = elapsedNs(started);
    recomp::diag::timeline::mark(recomp::diag::timeline::PermitWaitEnd, std::uint16_t(reason)); // v159
    S.counters.permitWaitNs.fetch_add(waited, std::memory_order_relaxed);
    S.counters.permitWaitMaxNs.update(waited);
    t.permitWaitedNs += waited;
    ++t.reasonWaits[reason]; // v110
    t.reasonWaitNs[reason] += waited;
    if (stoppedBy) {
        ++stoppedBy->blockedWaits;
        stoppedBy->blockedNs += waited;
    }
    if (S.terminal) {
        releasePermitLocked(S, t, TaskState::AwaitingPermit);
        cancelRun(lock);
    }
}

std::uint64_t quantumFor(const ParallelState& S, const Task& t) noexcept {
    const auto shift = std::min(3, portablePrioritySliceShift(t.priority.load(std::memory_order_relaxed))
                                   + (t.sliceBoost ? 1 : 0));
    const std::uint64_t quantum = S.limits.quantum;
    return shift >= 0 ? quantum << shift : std::max<std::uint64_t>(1, quantum >> -shift);
}

// Arm the context's countdown so the K-th following entry takes the slow path. K never passes the
// end of the current slice, the global budget share or CountdownBatch.
void armCountdown(ParallelState& S, Task& t, GuestCheckpointCountdown* countdown) noexcept {
    if (!countdown) {
        t.armedCountdown = nullptr;
        return;
    }
    const auto slice = t.entries - t.sliceStart;
    const auto quantum = quantumFor(S, t);
    std::uint64_t budget = std::min<std::uint64_t>(CountdownBatch, quantum > slice ? quantum - slice : 1);
    const auto total = S.globalEntries.load(std::memory_order_relaxed) + t.unflushed;
    const auto remaining = S.limits.functionEntries > total ? S.limits.functionEntries - total : 1;
    budget = std::min<std::uint64_t>(budget, std::max<std::uint64_t>(1, remaining / PermitDomains));
    budget = std::min<std::uint64_t>(budget, GuestCheckpointCountdownLimit - 1);
    const auto value = static_cast<std::int32_t>(budget - 1);
    *countdown = value;
    t.armedCountdown = countdown;
    t.armedValue = value;
}

// Entries executed on this context since the last arm, including the current one. A different or
// externally rewritten context (native callback context, restored copy) counts only this entry.
std::uint64_t consumedEntries(const Task& t, const GuestCheckpointCountdown* countdown) noexcept {
    if (!countdown || countdown != t.armedCountdown) return 1;
    const std::int64_t consumed = std::int64_t{t.armedValue} - std::int64_t{*countdown};
    if (consumed < 1 || consumed > std::int64_t{t.armedValue} + 1) return 1;
    return static_cast<std::uint64_t>(consumed);
}

void resumeTask(ParallelState& S, Task& t) {
    S.bindThread(t.token); // As the cooperative bind(): the registry names the running task.
    const auto previous = S.lastBound.exchange(t.token, std::memory_order_acq_rel);
    if (previous != 0 && previous != t.token) S.switches.fetch_add(1, std::memory_order_relaxed);
}

// v110: the park's wall time is charged to the exclusive site that caused it.
void chargeParkLocked(ExclusiveSite* site, Clock::time_point parkedAt) noexcept {
    if (!site) return;
    site->parkedNs += elapsedNs(parkedAt);
    ++site->parks;
}

void parkAtSafepointLocked(ParallelState& S, Task& t, std::unique_lock<std::mutex>& lock) {
    S.counters.safepointParks.fetch_add(1, std::memory_order_relaxed);
    ++t.safepointParkCount;
    flushEntries(S, t);
    auto* const site = S.activeSite;
    const auto parkedAt = Clock::now();
    releasePermitLocked(S, t, TaskState::ParkedSafepoint);
    acquirePermitLocked(S, t, lock, true, PermitSafepoint); // Resumes first once the exclusive scope ends.
    chargeParkLocked(site, parkedAt);
}

// v110: the accounting record of (site context of the calling thread, kind); nullptr when the table is full.
ExclusiveSite* exclusiveSiteLocked(ParallelState& S, unsigned kind) noexcept {
    const char* context = exclusiveSiteContext ? exclusiveSiteContext : "unlabelled";
    for (auto& site : S.exclusiveSites) {
        if (site.context == context && site.kind == kind) return &site;
        if (!site.context) { site.context = context; site.kind = kind; return &site; }
    }
    ++S.exclusiveSiteOverflow;
    return nullptr;
}
void recordExclusiveEndLocked(ExclusiveSite* site, std::uint64_t ns) noexcept {
    if (!site) return;
    ++site->count;
    site->totalNs += ns;
    site->maxNs = std::max(site->maxNs, ns);
}

void stopYieldLocked(bool yield) { // v241 (under the scheduler lock: begin/clear of the outermost scope)
#if defined(__ANDROID__) || defined(__linux__)
    if (yield == stopYieldApplied) return;
    if (yield && &recomp_v241_baseline && recomp_v241_baseline.load(std::memory_order_relaxed)) return;
    for (const auto& t : stopYieldTids) if (const int tid = t.load(std::memory_order_relaxed)) (void)setpriority(PRIO_PROCESS, tid, yield ? 10 : 0);
    stopYieldApplied = yield;
    if (yield) stopYields.fetch_add(1, std::memory_order_relaxed);
#else
    (void)yield;
#endif
}
void clearExclusiveLocked(ParallelState& S) {
    stopYieldLocked(false); // v241
    S.exclusiveOwner = nullptr;
    S.exclusiveOwnerView.store(nullptr, std::memory_order_release);
    setExclusiveDomainsLocked(S, ~0ull); // v191
    S.exclusiveCv.notify_all();           // v191: outside-domain tasks waiting to open their own scope
    S.exclusiveDepth = 0;
    S.activeSite = nullptr; // v110
    lowerAttention(AttentionExclusive);
    for (auto& d : S.domains) grantNextLocked(S, d);
}

// Returns true when this call acquired exclusivity (outermost scope of this task).
bool beginExclusiveLocked(ParallelState& S, Task& t, std::unique_lock<std::mutex>& lock, unsigned kind = ExclusiveSpan) {
    if (S.exclusiveOwner == &t) {
        ++S.exclusiveDepth;
        if (kind < S.nestedExclusive.size()) ++S.nestedExclusive[kind]; // v110
        return false;
    }
    while (S.exclusiveOwner) { // Another task's scope: yield this permit until it ends.
        if (S.terminal) cancelRun(lock);
        if (!domainStopped(S, t.domain)) { // v191: a scoped span that does not stop this domain: wait for its end
            S.exclusiveCv.wait(lock, [&] { return !S.exclusiveOwner || S.terminal.has_value(); });
            continue;
        }
        S.counters.safepointParks.fetch_add(1, std::memory_order_relaxed);
        ++t.safepointParkCount;
        flushEntries(S, t);
        auto* const site = S.activeSite;
        const auto parkedAt = Clock::now();
        releasePermitLocked(S, t, TaskState::ParkedSafepoint);
        acquirePermitLocked(S, t, lock, true, PermitSafepoint);
        chargeParkLocked(site, parkedAt);
    }
    if (S.terminal) cancelRun(lock);
    S.exclusiveOwner = &t;
    S.exclusiveOwnerView.store(&t, std::memory_order_release);
    S.exclusiveDepth = 1;
    setExclusiveDomainsLocked(S, scopedExclusiveMask(t)); // v191 (all domains unless a scoped tile-IRQ span)
    recomp::diag::timeline::mark(recomp::diag::timeline::ExclusiveBegin, std::uint16_t(kind)); // v159
    S.activeSite = exclusiveSiteLocked(S, kind); // v110: parks from here on are charged to this site
    raiseAttention(AttentionExclusive);
    stopYieldLocked(true); // v241
    const auto started = Clock::now();
    stopAudit.active = true; stopAudit.owner = &t; stopAudit.started = started; stopAudit.done = false; // v224
    stopAudit.atStartCount = 0; // v226
    for (const auto& d : S.domains)
        if (d.owner && d.owner != &t && stopAudit.atStartCount < stopAudit.atStart.size())
            stopAudit.atStart[stopAudit.atStartCount++] = {d.owner, d.owner->hostActivity.load(std::memory_order_relaxed)};
    const auto reached = [&] {
        if (S.terminal.has_value()) return true;
        if (S.exclusiveDomains == ~0ull) return S.running <= 1;
        for (unsigned i = 0; i < PermitDomains; ++i) // v191: every stopped domain is free or held by the owner
            if (domainStopped(S, i) && S.domains[i].owner && S.domains[i].owner != &t) return false;
        return true;
    };
    auto report = started + SafepointReportAfter;
    while (!S.exclusiveCv.wait_until(lock, report, reached)) {
        // Diagnostics only (L3): the scope keeps waiting. Log with sched released (leaf rule).
        std::array<std::pair<std::uint32_t, const char*>, PermitDomains> holders{};
        unsigned count = 0;
        for (const auto& d : S.domains)
            if (d.owner && d.owner != &t) holders[count++] = {d.owner->token, d.owner->lastLabel.load(std::memory_order_relaxed)};
        const auto waitedMs = static_cast<unsigned long long>(elapsedNs(started) / 1000000);
        lock.unlock();
        for (unsigned i = 0; i < count; ++i)
            executorLog("RENDER_DIAG_EXEC_SAFEPOINT_BLOCKED owner=%08x holder=%08x last=%s waited_ms=%llu",
                        t.token, holders[i].first, holders[i].second ? holders[i].second : "none", waitedMs);
        RECOMP_SCHED_RELOCK(lock);
        report = Clock::now() + SafepointReportEvery;
    }
    const auto safepoint = elapsedNs(started);
    { // v224: owner wake after the completing release (0 when nothing had to stop)
        auto& a = stopAudit; ++a.stops;
        if (a.done) { const auto w = elapsedNs(a.doneAt); a.ownerWakeNs += w; a.ownerWakeMaxNs = std::max(a.ownerWakeMaxNs, w); }
        else ++a.noneNeeded;
        a.active = false;
    }
    S.counters.safepointNs.fetch_add(safepoint, std::memory_order_relaxed);
    S.counters.safepointMaxNs.update(safepoint);
    if (S.terminal) {
        clearExclusiveLocked(S);
        cancelRun(lock);
    }
    if (S.activeSite) { // v110
        S.activeSite->ttsNs += safepoint;
        S.activeSite->maxTtsNs = std::max(S.activeSite->maxTtsNs, safepoint);
    }
    return true;
}

void endExclusiveLocked(ParallelState& S, Task& t) {
    if (S.exclusiveOwner != &t || !S.exclusiveDepth) return;
    if (--S.exclusiveDepth) return;
    recomp::diag::timeline::mark(recomp::diag::timeline::ExclusiveEnd); // v159
    clearExclusiveLocked(S);
}

// v124: files/private-trace/diag-exec-locked-rotation-check restores v119 (the scheduler lock at every slice end).
bool lockFreeRotationCheckEnabled() noexcept {
    static const bool enabled = !diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-locked-rotation-check"));
    return enabled;
}
std::atomic<std::uint64_t> rotationChecksSkipped{0};
// v154: a slice-end rotation waits until the queue head has waited this long (0 = v153: rotate at once). Batches the
// domain-2 ping-pong (render thread ~273 rotations/s, ~130 us wake-up per handoff); the waiter still gets the permit at
// the next slice end after the bound, or earlier whenever the owner parks. files/private-trace/diag-exec-rotate-after-1ms
// / -3ms. Domain 0 (many workers) is never deferred.
std::chrono::nanoseconds rotationMinWait() noexcept {
    static const std::chrono::nanoseconds wait = diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-rotate-after-3ms"))
        ? std::chrono::milliseconds(3) : diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-exec-rotate-after-1ms"))
        ? std::chrono::milliseconds(1) : std::chrono::nanoseconds{0};
    return wait;
}
void rotateIfContended(ParallelState& S, Task& t) {
    // v124: nothing is queued for this domain in the common case; v119 took the scheduler lock to find that out at every
    // slice end (render thread ~5.9k/s, 54 ms/s waiting for the lock, v119 device). Domain::queued mirrors the queue length
    // (v117, updated under the lock at every queue change): a stale 0 delays a rotation to the next slice end, as if the
    // waiter had queued a moment later; a non-zero reading takes the lock and decides exactly as before.
    if (lockFreeRotationCheckEnabled() && S.domains[t.domain].queued.load(std::memory_order_relaxed) == 0) {
        rotationChecksSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RECOMP_SCHED_LOCK(lock);
    auto& d = S.domains[t.domain];
    if (d.queue.empty() || (S.exclusiveOwner && domainStopped(S, t.domain)) || S.terminal) return; // (v191)
    if (const auto minWait = rotationMinWait(); minWait.count() && t.domain != 0 &&
            Clock::now() - d.queue.front()->queuedAt < minWait) {
        ++t.rotationsDeferred; // v154: keep the permit; the head is re-checked at the next slice end
        return;
    }
    flushEntries(S, t);
    S.counters.rotations.fetch_add(1, std::memory_order_relaxed);
    releasePermitLocked(S, t, TaskState::AwaitingPermit);
    acquirePermitLocked(S, t, lock, false, PermitRotation);
    lock.unlock();
    resumeTask(S, t);
}

// ---- Quiescence (the cooperative "no runnable signal producer" detector) ----
bool quiescentLocked(const ParallelState& S) {
    if (S.running || S.exclusiveOwner || S.handoffActive.load(std::memory_order_relaxed)) return false;
    for (const auto& d : S.domains) if (!d.queue.empty()) return false;
    bool any = false;
    for (const auto& task : S.tasks) {
        if (!task || task->finished) continue;
        if (task->state != TaskState::ParkedWait || task->timedPark) return false;
        if (task->wakeSeq != task->parkSeq || S.epoch != task->parkEpoch) return false; // Woken, not yet requeued.
        any = true;
    }
    return any;
}

void startProbeLocked(ParallelState& S, Task* initiator) {
    S.probeActive = true;
    S.probeDone = false;
    S.probeInitiator = initiator;
    S.probeActivity = S.activity;
    S.probeRemaining = 0;
    for (auto& task : S.tasks) {
        if (!task || task.get() == initiator || task->finished || task->state != TaskState::ParkedWait) continue;
        task->probeFlag = true;
        ++task->wakeSeq;
        ++S.probeRemaining;
        task->cv.notify_all();
    }
    S.counters.probes.fetch_add(1, std::memory_order_relaxed);
    if (!S.probeRemaining) S.probeDone = true;
}

void probeReportedLocked(ParallelState& S) {
    if (!S.probeActive || !S.probeRemaining) return;
    if (!--S.probeRemaining) {
        S.probeDone = true;
        if (S.probeInitiator) S.probeInitiator->cv.notify_all();
    }
}

void endProbeLocked(ParallelState& S) {
    S.probeActive = S.probeDone = false;
    S.probeInitiator = nullptr;
    S.probeRemaining = 0;
    for (auto& task : S.tasks) if (task) task->probeFlag = false;
}

void wakeAllLocked(ParallelState& S) {
    ++S.epoch;
    ++S.activity;
    for (auto& task : S.tasks)
        if (task && (task->state == TaskState::ParkedWait || task->state == TaskState::ParkedDelay)) task->cv.notify_all();
}

// parkedTask()-based predicates (renderer handshakes) must re-evaluate when a task parks.
void notifyParkObserversLocked(ParallelState& S, const Task& parked) {
    for (auto& task : S.tasks) {
        if (!task || task.get() == &parked || task->finished || !task->observesParking) continue;
        ++task->wakeSeq;
        task->cv.notify_all();
    }
}

void recordLateWakeup(ParallelState& S, const char* site) noexcept {
    S.counters.lateWakeups.fetch_add(1, std::memory_order_relaxed);
    try {
        std::lock_guard lock(S.lateMutex);
        for (auto& entry : S.lateSites) {
            if (entry.count && entry.site == site) { ++entry.count; return; }
            if (!entry.count) { entry.site = site; entry.count = 1; return; }
        }
        ++S.lateOverflow;
    } catch (...) {}
}

void leaveWaitLocked(ParallelState& S, Task& t) {
    t.inWait = false;
    t.timedPark = false;
    t.observesParking = false;
    if (t.probeFlag) {
        t.probeFlag = false;
        ++S.activity; // A probed task became runnable: the probe cannot confirm quiescence.
        probeReportedLocked(S);
    }
    ++S.activity;
}

bool reserveWait(ParallelState& S) noexcept {
    auto current = S.waits.load(std::memory_order_relaxed);
    do {
        if (current >= S.limits.functionEntries) return false;
    } while (!S.waits.compare_exchange_weak(current, current + 1, std::memory_order_relaxed));
    return true;
}

bool evaluateReady(ParallelState& S, Task& t, const Ready& ready) {
    t.evaluating = true;
    try {
        const bool value = ready();
        t.evaluating = false;
        return value;
    } catch (const std::exception& e) { // v192 DIAGNOSTIC: what the readiness check threw
        t.evaluating = false;
        executorLog("RENDER_DIAG_EXEC_READY_THREW token=%08x what=%s", t.token, e.what());
        RECOMP_SCHED_LOCK(lock);
        setTerminalLocked(S, stopResult(GuestExecutionStopReason::HostException,
                "Cooperative readiness check failed", flushEntries(S, t)));
        cancelRun(lock);
    } catch (...) {
        t.evaluating = false;
        executorLog("RENDER_DIAG_EXEC_READY_THREW token=%08x what=non-std", t.token);
        RECOMP_SCHED_LOCK(lock);
        setTerminalLocked(S, stopResult(GuestExecutionStopReason::HostException,
                "Cooperative readiness check failed", flushEntries(S, t)));
        cancelRun(lock);
    }
}

extern "C" std::atomic<bool> recomp_v407_swap_delay; // v407 (defined with the v346 switch below)
bool parallelWait(ParallelState& S, Task& t, const Ready& ready, std::optional<Clock::time_point> deadline) {
    const char* site = pendingWaitSite ? pendingWaitSite : t.lastLabel.load(std::memory_order_relaxed);
    pendingWaitSite = nullptr;
    RECOMP_SCHED_LOCK(lock);
    auto seq = t.wakeSeq; // Snapshot before the first evaluation (eventcount order).
    auto epoch = S.epoch;
    lock.unlock();
    {   // Immediate check: exceptions propagate to the caller, as in cooperative mode.
        t.evaluating = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{t.evaluating};
        if (ready()) {
            if (t.boostPending.exchange(false, std::memory_order_acq_rel)) t.sliceBoost = true;
            return true;
        }
    }
    t.sliceStart = t.entries;
    t.sliceBoost = false;
    // v245: the render thread's first not-ready wait after a present lends cpu7 to main until the wait completes (main
    // releases it itself when it wakes this thread); the guard also releases on the probe/terminal exits.
    struct V245Release { bool render; ~V245Release() { if (render) recomp_v245::state().release(false); } }
        v245Release{t.token == recomp_v245::RenderToken};
    const bool v407Delay = v245Release.render && recomp_v407_swap_delay.load(std::memory_order_relaxed); // v407
    const auto v407Start = v407Delay ? Clock::now() : Clock::time_point{};
    if (v245Release.render && !v407Delay) recomp_v245::state().beforePark();
    recomp_v266::beforePark(); // v266: a Sleep(0) poller may be waiting for this park (poll_sleep_v266.h)
    RECOMP_SCHED_RELOCK(lock);
    ++t.waitCalls; // v110 (parked-capable waits only; immediate completions return above)
    t.inWait = true;
    bool parkedOnce = false, polled = false, evaluated = true; // The immediate check was the first evaluation.
    // job-path (park_poll_jp.h): the first poll step is PollInitial (250 us) unless diag-jp-park-poll is on and this is not
    // one of the frame's critical threads (then 2 ms); the doubling to PollMaximum below is unchanged.
    const Clock::duration pollFirst = recomp_jp::initialPoll(t.token, recomp_jp_park_poll.load(std::memory_order_relaxed));
    Clock::duration poll = pollFirst;
    for (;;) {
        bool ok = false;
        if (!evaluated) {
            seq = t.wakeSeq;
            epoch = S.epoch;
            lock.unlock();
            ok = evaluateReady(S, t, ready);
            if (!ok && v407Delay && recomp_v245::v407SwapNow(true, true, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - v407Start).count()))
                recomp_v245::state().beforePark(); // v407: a long render-thread wait (lock released here; one swap per present)
            if (!ok && deadline && Clock::now() >= *deadline) {
                ok = evaluateReady(S, t, ready); // Once more at/after expiry, as the cooperative scheduler.
                if (!ok) {
                    RECOMP_SCHED_RELOCK(lock);
                    t.inWait = false;
                    setTerminalLocked(S, stopResult(GuestExecutionStopReason::HostException,
                            "Cooperative readiness check failed", flushEntries(S, t)));
                    cancelRun(lock);
                }
            }
            RECOMP_SCHED_RELOCK(lock);
        }
        evaluated = false;
        if (ok) {
            // Late: completed after a poll timeout with no notification at all since parking, before
            // its own deadline. Lanes audit L4: completing at/after the deadline is the timeout the
            // caller asked for (e.g. the 1 ms audio backpressure wait whose permit re-acquisition
            // outlasted the deadline), not a missed notification.
            if (polled && !(deadline && Clock::now() >= *deadline) && t.wakeSeq == t.parkSeq && S.epoch == t.parkEpoch)
                recordLateWakeup(S, site);
            leaveWaitLocked(S, t);
            lock.unlock();
            if (t.boostPending.exchange(false, std::memory_order_acq_rel)) t.sliceBoost = true;
            if (parkedOnce) resumeTask(S, t);
            return true;
        }
        if (t.probeFlag) {
            t.probeFlag = false;
            probeReportedLocked(S);
        }
        if (t.wakeSeq != seq || S.epoch != epoch) { // Notified while evaluating: evaluate again.
            polled = false;
            continue;
        }
        flushEntries(S, t);
        releasePermitLocked(S, t, TaskState::ParkedWait);
        parkedOnce = true;
        t.parkSeq = seq;
        t.parkEpoch = epoch;
        t.timedPark = deadline.has_value();
        S.counters.parks.fetch_add(1, std::memory_order_relaxed);
        notifyParkObserversLocked(S, t);
        recomp::diag::timeline::mark(recomp::diag::timeline::ParkBegin, 0, recomp::diag::timeline::waitObject); // v159 (v195: object)
        bool initiator = false;
        if (!t.timedPark && !S.probeActive && quiescentLocked(S)) {
            startProbeLocked(S, &t);
            initiator = true;
        }
        bool notified;
        // v346 (A/B, scheduling only: files/private-trace/diag-v346-main-spin, polled by the renderer's windows): main, after
        // releasing its permit, watches its wake sequence for up to 40 us before blocking. A wake in that time finds it not in
        // cv.wait (the waker then skips the futex notify, v184) and main runs on without a sleep/wake cycle; the predicate below
        // decides exactly as before. Off-CPU profile (v345 outdoor): main had ~2200 off-CPU intervals/s, 60% shorter than 50 us.
        if (const unsigned spinUs = recomp_v346_main_spin_us.load(std::memory_order_relaxed);
            !initiator && t.token == recomp_v245::MainToken && spinUs) { // (v347: the watch length in us; 0 = off)
            lock.unlock();
            const auto until = Clock::now() + std::chrono::microseconds(spinUs);
            unsigned k = 0;
            while (__atomic_load_n(&t.wakeSeq, __ATOMIC_ACQUIRE) == seq && __atomic_load_n(&S.epoch, __ATOMIC_ACQUIRE) == epoch) {
                if ((++k & 15u) == 0 && Clock::now() >= until) break;
#if defined(__aarch64__)
                asm volatile("yield");
#endif
            }
            RECOMP_SCHED_RELOCK(lock);
            if (t.wakeSeq != seq || S.epoch != epoch) S.counters.v346SpinHits.fetch_add(1, std::memory_order_relaxed);
            else S.counters.v346SpinMisses.fetch_add(1, std::memory_order_relaxed);
        }
        if (initiator) { // No poll timeout while the probe runs: it must be able to complete.
            t.cvWaiting = true; // v184
            ++t.cvGen; t.seqWait = true; // async-wake: this wait's predicate reads wakeSeq (a repeat notify is redundant)
            t.cv.wait(lock, [&] {
                return S.terminal.has_value() || t.wakeSeq != seq || S.epoch != epoch || S.probeDone;
            });
            t.cvWaiting = false;
            t.seqWait = false; // async-wake
            notified = true;
        } else {
            const auto wakeAt = std::min(deadline.value_or(Clock::time_point::max()), Clock::now() + poll);
            t.cvWaiting = true; // v184
            ++t.cvGen; t.seqWait = true; // async-wake: this wait's predicate reads wakeSeq (a repeat notify is redundant)
            notified = t.cv.wait_until(lock, wakeAt, [&] {
                return S.terminal.has_value() || t.wakeSeq != seq || S.epoch != epoch;
            });
            t.cvWaiting = false;
            t.seqWait = false; // async-wake
            if (notified && t.wakeSeq != seq && Clock::now() >= wakeAt) {
                S.counters.wakeAtTimeout.fetch_add(1, std::memory_order_relaxed);
                // v185: the latest wake skipped its notify and the park still ran to its timer: a needed notify was
                // skipped (expected 0; the others are notifies that arrived after the timer).
                if (t.skippedSeq == t.wakeSeq) S.counters.skippedAtTimeout.fetch_add(1, std::memory_order_relaxed);
            }
        }
        t.timedPark = false;
        recomp::diag::timeline::mark(recomp::diag::timeline::ParkEnd, notified ? 1 : 2); // v159
        if (S.terminal) {
            t.inWait = false;
            cancelRun(lock);
        }
        if (initiator && S.probeInitiator == &t) {
            const bool confirmed = S.probeDone && S.activity == S.probeActivity && quiescentLocked(S);
            endProbeLocked(S);
            if (confirmed) {
                S.counters.probesConfirmed.fetch_add(1, std::memory_order_relaxed);
                acquirePermitLocked(S, t, lock, false, PermitWaitReturn);
                leaveWaitLocked(S, t);
                lock.unlock();
                resumeTask(S, t);
                return false; // No runnable signal producer (cooperative waitUntilImpl result).
            }
        }
        const bool deadlineReached = deadline && Clock::now() >= *deadline;
        polled = !notified && !deadlineReached;
        if (notified) {
            S.counters.wakeNotified.fetch_add(1, std::memory_order_relaxed);
            poll = pollFirst; // job-path: = PollInitial with diag-jp-park-poll off
        } else if (deadlineReached) {
            S.counters.wakeDeadline.fetch_add(1, std::memory_order_relaxed);
        } else {
            S.counters.wakePoll.fetch_add(1, std::memory_order_relaxed);
            poll = std::min<Clock::duration>(poll * 2, PollMaximum);
        }
        acquirePermitLocked(S, t, lock, false, PermitWaitReturn);
    }
}

bool parallelDelay(ParallelState& S, Task& t, Clock::time_point deadline) {
    t.sliceStart = t.entries;
    t.sliceBoost = false;
    S.counters.delays.fetch_add(1, std::memory_order_relaxed);
    RECOMP_SCHED_LOCK(lock);
    flushEntries(S, t);
    ++t.delayCalls; // v110
    t.inWait = true;
    releasePermitLocked(S, t, TaskState::ParkedDelay); // Relinquishes the slice once, even at zero.
    notifyParkObserversLocked(S, t);
    recomp::diag::timeline::mark(recomp::diag::timeline::DelayBegin); // v159
    t.cvWaiting = true; // v184
    while (!S.terminal && Clock::now() < deadline) t.cv.wait_until(lock, deadline);
    t.cvWaiting = false;
    recomp::diag::timeline::mark(recomp::diag::timeline::DelayEnd);
    if (S.terminal) {
        t.inWait = false;
        cancelRun(lock);
    }
    acquirePermitLocked(S, t, lock, false, PermitDelayReturn);
    t.inWait = false;
    ++S.activity;
    lock.unlock();
    resumeTask(S, t);
    return true;
}

// The per-entry slow path (and every GuestExecutionGuard::checkpoint call of a task).
void parallelCheckpoint(ParallelState& S, Task& t, GuestCheckpointCountdown* countdown, std::string_view label) {
    S.counters.slowCheckpoints.fetch_add(1, std::memory_order_relaxed);
    // prime-exchange (prime_swap_v245.h; diag-px-*): the render thread's check point inside its frame - past its deadline it
    // exchanges back (main -> its ranking mask, then itself -> {7}). Out of line, noexcept: one direct call in this block; with
    // no exchange in progress (always, with the switches absent) it returns after one relaxed load.
    recomp_v245::pxRenderPoll(t.token);
    const auto consumed = consumedEntries(t, countdown);
    t.armedCountdown = nullptr;
    for (;;) {
        const auto bits = attentionBits();
        if (bits & AttentionStop) throwCancelled();
        if (t.handoffWorker && t.handoffEntries >= t.handoffBudget) {
            RECOMP_SCHED_LOCK(lock);
            setTerminalLocked(S, stopResult(GuestExecutionStopReason::FunctionEntryBudgetExceeded,
                    "Exclusive native worker handoff budget or ownership boundary", flushEntries(S, t)));
            cancelRun(lock);
        }
        if ((bits & AttentionExclusive) && !t.scopeActive &&
            S.exclusiveOwnerView.load(std::memory_order_acquire) != &t && domainStoppedView(S, t.domain)) { // (v191)
            RECOMP_SCHED_LOCK(lock);
            if (S.exclusiveOwner && S.exclusiveOwner != &t && domainStopped(S, t.domain)) {
                parkAtSafepointLocked(S, t, lock);
                lock.unlock();
                resumeTask(S, t);
                continue; // Re-check stop and budgets after the scope.
            }
        }
        break;
    }
    if (t.scopeActive && t.scopeEntries >= t.scopeBudget) {
        RECOMP_SCHED_LOCK(lock);
        setTerminalLocked(S, stopResult(GuestExecutionStopReason::FunctionEntryBudgetExceeded,
                "Native non-yielding function-entry budget exhausted", flushEntries(S, t)));
        cancelRun(lock);
    }
    // Entries already executed on the fast path since the last arm, then the budget check for
    // this entry (checked before it counts, as the cooperative scheduler does).
    t.entries += consumed - 1;
    t.unflushed += consumed - 1;
    const auto total = flushEntries(S, t);
    if (total >= S.limits.functionEntries) {
        RECOMP_SCHED_LOCK(lock);
        setTerminalLocked(S, stopResult(GuestExecutionStopReason::FunctionEntryBudgetExceeded,
                "Global cooperative function-entry budget exhausted", total));
        cancelRun(lock);
    }
    ++t.entries;
    ++t.unflushed;
    if (t.scopeActive) ++t.scopeEntries;
    if (t.handoffWorker) ++t.handoffEntries;
    t.lastLabel.store(label.data(), std::memory_order_relaxed);
    if (S.traceSink) { // Sampled: one call per slow path (every entry inside scopes and stops).
        if constexpr (PermitDomains > 1) {
            std::lock_guard sink(S.sinkMutex);
            S.traceSink(label);
        } else {
            S.traceSink(label);
        }
    }
    if (!t.children.empty()) {
        RECOMP_SCHED_LOCK(lock);
        armChildrenLocked(S, t);
    }
    if (!t.scopeActive) {
        const auto slice = t.entries - t.sliceStart;
        if (slice >= quantumFor(S, t) || t.rescheduleHint) {
            t.rescheduleHint = false;
            t.sliceStart = t.entries;
            t.sliceBoost = false;
            rotateIfContended(S, t);
        }
    }
    armCountdown(S, t, countdown);
}

void finishTask(ParallelState& S, const std::shared_ptr<Task>& task, GuestExecutionResult result) {
    auto& t = *task;
    RECOMP_SCHED_LOCK(lock);
    const auto total = flushEntries(S, t);
    t.finished = true;
    t.inWait = t.timedPark = t.observesParking = false;
    if (t.probeFlag) {
        t.probeFlag = false;
        probeReportedLocked(S);
    }
    releasePermitLocked(S, t, TaskState::Finished); // Also arms its unstarted children.
    ++S.activity;
    if (S.handoffTarget == &t && S.handoffParent) S.handoffParent->cv.notify_all();
    if (!S.terminal && !result.completed()) {
        result.completedFunctionEntries = total;
        setTerminalLocked(S, std::move(result));
    }
    if (!S.terminal) {
        bool allFinished = true;
        for (const auto& other : S.tasks) if (other && !other->finished) { allFinished = false; break; }
        if (allFinished) {
            setTerminalLocked(S, stopResult(GuestExecutionStopReason::Completed, "", total));
        } else if (!S.probeActive && quiescentLocked(S)) {
            startProbeLocked(S, &t);
            t.cvWaiting = true; // v184
            t.cv.wait(lock, [&] { return S.probeDone || S.terminal.has_value() || S.probeInitiator != &t; });
            t.cvWaiting = false;
            if (!S.terminal && S.probeInitiator == &t) {
                const bool confirmed = S.probeDone && S.activity == S.probeActivity && quiescentLocked(S);
                endProbeLocked(S);
                if (confirmed) {
                    S.counters.probesConfirmed.fetch_add(1, std::memory_order_relaxed);
                    setTerminalLocked(S, stopResult(GuestExecutionStopReason::NoRunnableTask,
                            "All remaining tasks wait without a runnable signal producer",
                            S.globalEntries.load(std::memory_order_relaxed)));
                }
            }
        } else {
            wakeAllLocked(S); // Thread-exit handles and parkedTask observers.
        }
    }
    S.coordinator.notify_all();
}

void runTaskBody(ParallelState& S, const std::shared_ptr<Task>& task) {
    auto& t = *task;
    t.sliceStart = t.entries;
    recomp::diag::timeline::setThreadToken(t.token); // v159
    // v29: name each task's host thread once, so profiles (simpleperf tids) and the placement
    // monitor can be read by guest role.
#if defined(__ANDROID__)
    if (taskLineSampled(t.token, 1)) // v218
        executorLog("RENDER_DIAG_TASK_HOST token=%08x tid=%d domain=%u", t.token, static_cast<int>(gettid()), t.domain);
    // v34: optional host hook (trace_runner's core placement gives the D3D render thread the prime core).
    if (recomp_diag_task_host_started) recomp_diag_task_host_started(t.token, static_cast<int>(gettid()));
#endif
    GuestExecutionGuard guard;
    auto result = guard.run(S.limits.functionEntries, [&] {
        resumeTask(S, t);
        {
            RECOMP_SCHED_LOCK(lock);
            t.entered = true;
        }
        if (t.index != 0) S.workersEntered.fetch_add(1, std::memory_order_relaxed);
        try {
            t.entry();
        } catch (const ThreadExited&) {
            // Only terminateCurrentThread constructs this private signal; the registry has already
            // published the thread's completion.
        }
    }, [&](std::string_view label) { parallelCheckpoint(S, t, nullptr, label); });
    finishTask(S, task, std::move(result));
    releaseUnwind(S, t);
}

void workerMain(ParallelState& S, const std::shared_ptr<Task>& task) {
    currentTask = task.get();
    // v362: the D3D render thread claims the fast thread_local path (fast_tls_v362.h) before its task runs; the claim is
    // released at this host thread's exit (a thread_local destructor), so a later thread with the same thread pointer never
    // matches. Speed only: every access still names the same variables.
#if defined(RECOMP_V362_TEST_CLAIM) // host tests: the first worker's thread claims and the switch is on (no render token there)
    if (recomp::v362::claim()) recomp::v362::fastThread.on.store(true, std::memory_order_relaxed);
#else
    if (task->token == 0x6408e000u) (void)recomp::v362::claim();
#endif
    bool run = false;
    {
        RECOMP_SCHED_LOCK(lock);
        auto& d = S.domains[task->domain];
        task->cvWaiting = true; // v184
        task->cv.wait(lock, [&] { return d.owner == task.get() || S.terminal.has_value(); });
        task->cvWaiting = false;
        run = d.owner == task.get() && !S.terminal;
        if (run && task->handoffWorker && S.activeSite) { // v110: grant -> this host thread running
            const auto ns = elapsedNs(task->handoffGrantedAt);
            ++S.activeSite->workerStarts;
            S.activeSite->workerStartNs += ns;
            S.activeSite->maxWorkerStartNs = std::max(S.activeSite->maxWorkerStartNs, ns);
        }
        if (!run) { // Stopped before this worker ever entered (cooperative worker() parity).
            d.queue.erase(std::remove(d.queue.begin(), d.queue.end(), task.get()), d.queue.end());
            syncQueued(d);
            releasePermitLocked(S, *task, TaskState::Finished);
            task->finished = true;
            ++S.activity;
            S.coordinator.notify_all();
        }
    }
    if (run) {
        if (task->boostPending.exchange(false, std::memory_order_acq_rel)) task->sliceBoost = true;
        runTaskBody(S, task);
    }
    currentTask = nullptr;
}

// v240 (A/B: diag-v240-baseline -> recomp_v240_baseline): pre-started host threads for the tile-scene IRQ workers. v231
// outdoor timeline: of each ~1.1 ms world stop, enqueue's thread creation (pthread_create under the scheduler lock) and the
// new thread's start took ~0.3 ms and the worker's exit path ~0.27 ms more (a render-thread-created worker shares cpu7 with
// the waiting render thread). A pooled host runs exactly the task enqueue would have given a new thread (workerMain), with
// the creator's CPU mask and nice as an inherited thread has them, then parks; it keeps no task state (workerMain sets and
// clears currentTask, runTaskBody sets the timeline token and builds its guard on the stack). At most four hosts; none idle
// -> a new thread as before. The run's end joins every task host first, then stops the pool.
struct HostPool {
    struct Host {
        std::thread thread; std::condition_variable cv; std::function<void()> job; bool exit = false, busy = false;
#if defined(__ANDROID__) || defined(__linux__)
        cpu_set_t mask{}; bool haveMask = false; int nice = 0;
#endif
    };
    std::mutex m; std::vector<std::unique_ptr<Host>> hosts;
    static void loop(HostPool* pool, Host* h) {
#if defined(__ANDROID__) || defined(__linux__)
        pthread_setname_np(pthread_self(), "recomp-irq-host");
#endif
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(pool->m);
                h->cv.wait(lock, [&] { return h->exit || static_cast<bool>(h->job); });
                if (!h->job) return;
                job = std::move(h->job); h->job = nullptr;
#if defined(__ANDROID__) || defined(__linux__)
                if (h->haveMask) (void)sched_setaffinity(0, sizeof h->mask, &h->mask);
                (void)setpriority(PRIO_PROCESS, 0, h->nice);
#endif
            }
            job();
            std::lock_guard lock(pool->m);
            h->busy = false;
        }
    }
    bool run(std::function<void()>&& job) {
        std::lock_guard lock(m);
        Host* h = nullptr;
        for (auto& x : hosts) if (!x->busy) { h = x.get(); break; }
        if (!h) {
            if (hosts.size() >= 4) return false;
            try {
                auto fresh = std::make_unique<Host>();
                fresh->thread = std::thread(&HostPool::loop, this, fresh.get());
                hosts.push_back(std::move(fresh));
            } catch (...) { return false; }
            h = hosts.back().get(); pooledCreated.fetch_add(1, std::memory_order_relaxed);
        }
#if defined(__ANDROID__) || defined(__linux__)
        h->haveMask = sched_getaffinity(0, sizeof h->mask, &h->mask) == 0; // the creator's, as an inherited thread gets them
        errno = 0; const int n = getpriority(PRIO_PROCESS, 0); h->nice = errno ? 0 : n;
#endif
        h->busy = true; h->job = std::move(job);
        h->cv.notify_one();
        return true;
    }
    void shutdown() {
        std::vector<std::unique_ptr<Host>> old;
        { std::lock_guard lock(m); for (auto& h : hosts) h->exit = true; for (auto& h : hosts) h->cv.notify_one(); old.swap(hosts); }
        for (auto& h : old) if (h->thread.joinable()) h->thread.join();
    }
};
HostPool& hostPool() { static auto* pool = new HostPool; return *pool; }
bool v240Baseline() { return &recomp_v240_baseline && recomp_v240_baseline.load(std::memory_order_relaxed); }

void logCounters(ParallelState& S, bool final) {
    std::size_t tasks = 0, queued = 0;
    unsigned live = 0, parked = 0, runningNow = 0;
    // v44: per-task permit accounting for the tasks outside domain 0 (render domain 2, audio domain 1).
    struct TaskPermits { std::uint32_t token; unsigned domain; std::uint64_t held, waited, parks; };
    std::array<TaskPermits, 64> laneTasks{}; // v171: 64 domains
    unsigned laneCount = 0;
    // v46: name every task that holds a permit (running guest code) with its last slow-path label and
    // entry count, so a livelock (one task spinning, the rest parked) identifies itself in the log.
    struct RunningTask { std::uint32_t token; unsigned domain; const char* label; std::uint64_t entries; };
    std::array<RunningTask, 8> runningTasks{};
    unsigned runningCount = 0;
    // v78: the domain-0 tasks with the most permit time (held, then waited), for the next domain split decision
    // (RENDER_DIAG_EXEC_D0, cumulative like EXEC_TASKS: difference two lines for a window).
    struct DomainZero { std::uint32_t token; std::uint64_t held, waited, parks, entries; };
    std::array<DomainZero, 8> domainZero{};
    unsigned domainZeroCount = 0, domainZeroTasks = 0;
    // v110: permit waits by reason for the lane tasks, and the exclusive sites (copied under sched).
    struct LanePermits {
        std::uint32_t token; unsigned domain; std::uint64_t held, waits, delays;
        std::array<std::uint64_t, PermitReasons> count, ns;
        std::uint64_t grants, grantNs, spinHits, spinMisses, spinNs; // v151
        std::uint64_t deferred; // v154
    };
    std::array<LanePermits, 64> lanePermits{}; // v171: 64 domains
    unsigned lanePermitCount = 0;
    std::array<ExclusiveSite, 32> sites{};
    StopAudit stopAuditCopy{}; // v224
    std::array<std::uint64_t, 5> nested{};
    std::uint64_t siteOverflow = 0;
    try {
        RECOMP_SCHED_LOCK(lock);
        sites = S.exclusiveSites;
        nested = S.nestedExclusive;
        stopAuditCopy = stopAudit; // v224
        siteOverflow = S.exclusiveSiteOverflow;
        for (const auto& task : S.tasks) { // v110: finished lane tasks too (the final line keeps their totals)
            if (!task || task->domain == 0 || lanePermitCount >= lanePermits.size()) continue;
            auto held = task->permitHeldNs;
            if (S.domains[task->domain].owner == task.get()) held += elapsedNs(task->permitSince);
            lanePermits[lanePermitCount++] = {task->token, task->domain, held, task->waitCalls, task->delayCalls,
                                              task->reasonWaits, task->reasonWaitNs, task->grantCount,
                                              task->grantLatencyNs, task->spinHits, task->spinMisses, task->spinNs,
                                              task->rotationsDeferred};
        }
        tasks = S.tasks.size();
        for (const auto& task : S.tasks) {
            if (!task || task->finished) continue;
            ++live;
            if (task->state == TaskState::Running && runningCount < runningTasks.size())
                runningTasks[runningCount++] = {task->token, task->domain, task->lastLabel.load(std::memory_order_relaxed), task->entries};
            if (task->state == TaskState::ParkedWait || task->state == TaskState::ParkedDelay) ++parked;
            if (task->domain != 0 && laneCount < laneTasks.size()) {
                auto held = task->permitHeldNs;
                if (S.domains[task->domain].owner == task.get()) held += elapsedNs(task->permitSince);
                laneTasks[laneCount++] = {task->token, task->domain, held, task->permitWaitedNs, task->safepointParkCount};
            }
            if (task->domain == 0) { // v78: keep the 8 largest by held time (insertion into a sorted array)
                ++domainZeroTasks;
                auto held = task->permitHeldNs;
                if (S.domains[0].owner == task.get()) held += elapsedNs(task->permitSince);
                const DomainZero entry{task->token, held, task->permitWaitedNs, task->safepointParkCount, task->entries};
                unsigned at = domainZeroCount < domainZero.size() ? domainZeroCount++ : static_cast<unsigned>(domainZero.size());
                if (at == domainZero.size()) {
                    if (held <= domainZero.back().held) continue;
                    at = static_cast<unsigned>(domainZero.size()) - 1;
                }
                while (at > 0 && domainZero[at - 1].held < held) { domainZero[at] = domainZero[at - 1]; --at; }
                domainZero[at] = entry;
            }
        }
        for (const auto& d : S.domains) queued += d.queue.size();
        runningNow = S.running;
    } catch (...) {}
    auto& c = S.counters;
    const auto v = [](const std::atomic<std::uint64_t>& a) {
        return static_cast<unsigned long long>(a.load(std::memory_order_relaxed));
    };
    const auto us = [](const MaxCounter& m) { return static_cast<unsigned long long>(m.value.load(std::memory_order_relaxed) / 1000); };
    executorLog("RENDER_DIAG_EXEC mode=%s final=%u elapsed_ms=%llu tasks=%zu live=%u running=%u parked=%u "
                "queued=%zu entries=%llu slow=%llu permits=%llu contended=%llu wait_ms=%llu wait_max_us=%llu "
                "hold_max_us=%llu max_concurrent=%llu rotations=%llu parks=%llu wakes=%llu/%llu/%llu late=%llu "
                "wake_calls=%llu/%llu wake_notify=%llu/%llu/%llu/%llu excl=%llu/%llu/%llu/%llu excl_ms=%llu excl_max_us=%llu tts_max_us=%llu "
                "safepoint_parks=%llu probes=%llu/%llu boosts=%llu/%llu delays=%llu v346_spin=%llu/%llu",
                guestThreadModeName(), final ? 1U : 0U, static_cast<unsigned long long>(elapsedNs(S.startedAt) / 1000000),
                tasks, live, runningNow, parked, queued, v(S.globalEntries), v(c.slowCheckpoints), v(c.permitAcquired),
                v(c.permitContended), v(c.permitWaitNs) / 1000000, us(c.permitWaitMaxNs), us(c.permitHoldMaxNs),
                static_cast<unsigned long long>(c.maxConcurrent.value.load(std::memory_order_relaxed)), v(c.rotations),
                v(c.parks), v(c.wakeNotified), v(c.wakePoll), v(c.wakeDeadline), v(c.lateWakeups), v(c.wakeCalls),
                v(c.wakeAllCalls), v(c.wakeNotifySent), v(c.wakeNotifySkipped), v(c.wakeAtTimeout), v(c.skippedAtTimeout), v(c.exclusiveCount[0]), v(c.exclusiveCount[1]), v(c.exclusiveCount[2]),
                v(c.exclusiveCount[ExclusiveHandoff]),
                (v(c.exclusiveNs[0]) + v(c.exclusiveNs[1]) + v(c.exclusiveNs[2]) + v(c.exclusiveNs[ExclusiveHandoff])) / 1000000,
                us(c.exclusiveMaxNs), us(c.safepointMaxNs), v(c.safepointParks), v(c.probes), v(c.probesConfirmed),
                v(c.boostsParked), v(c.boostsPending), v(c.delays), v(c.v346SpinHits), v(c.v346SpinMisses));
    for (unsigned i = 0; i < runningCount; ++i)
        executorLog("RENDER_DIAG_EXEC_RUNNING token=%08x domain=%u last=%s entries=%llu", runningTasks[i].token,
                    runningTasks[i].domain, runningTasks[i].label ? runningTasks[i].label : "none",
                    static_cast<unsigned long long>(runningTasks[i].entries));
    executorLog("RENDER_DIAG_EXEC_GATE lock_gates=%llu lock_gate_ms=%llu lock_gate_wait_ms=%llu lock_gate_max_us=%llu",
                v(c.lockGates), v(c.lockGateNs) / 1000000, v(c.lockGateWaitNs) / 1000000, us(c.lockGateMaxNs));
    executorLog("RENDER_DIAG_EXEC_YIELD enabled=%u fast=%llu delays=%llu rotation_lockfree=%u rotation_checks_skipped=%llu", zeroYieldFastEnabled() ? 1U : 0U,
                v(c.fastYields), v(c.delays), lockFreeRotationCheckEnabled() ? 1U : 0U, v(rotationChecksSkipped)); // v124
    if (laneCount) { // v44: cumulative since the run started; difference two lines for a window.
        char line[640];
        int w = std::snprintf(line, sizeof line, "RENDER_DIAG_EXEC_TASKS final=%u elapsed_ms=%llu", final ? 1U : 0U,
                              static_cast<unsigned long long>(elapsedNs(S.startedAt) / 1000000));
        for (unsigned i = 0; i < laneCount && w > 0 && w < static_cast<int>(sizeof line) - 80; ++i)
            w += std::snprintf(line + w, sizeof line - w, " %08x:d%u:held_ms=%llu:wait_ms=%llu:parks=%llu", laneTasks[i].token,
                               laneTasks[i].domain, static_cast<unsigned long long>(laneTasks[i].held / 1000000),
                               static_cast<unsigned long long>(laneTasks[i].waited / 1000000),
                               static_cast<unsigned long long>(laneTasks[i].parks));
        executorLog("%s", line);
    }
    if (domainZeroCount) { // v78
        char line[768];
        int w = std::snprintf(line, sizeof line, "RENDER_DIAG_EXEC_D0 final=%u elapsed_ms=%llu tasks=%u", final ? 1U : 0U,
                              static_cast<unsigned long long>(elapsedNs(S.startedAt) / 1000000), domainZeroTasks);
        for (unsigned i = 0; i < domainZeroCount && w > 0 && w < static_cast<int>(sizeof line) - 96; ++i)
            w += std::snprintf(line + w, sizeof line - w, " %08x:held_ms=%llu:wait_ms=%llu:parks=%llu:entries=%llu", domainZero[i].token,
                               static_cast<unsigned long long>(domainZero[i].held / 1000000),
                               static_cast<unsigned long long>(domainZero[i].waited / 1000000),
                               static_cast<unsigned long long>(domainZero[i].parks),
                               static_cast<unsigned long long>(domainZero[i].entries));
        executorLog("%s", line);
    }
    // v110 (cumulative since the run started, every 10 s; difference two lines for a window).
    const auto ms = [](std::uint64_t ns) { return static_cast<unsigned long long>(ns / 1000000); };
    for (unsigned i = 0; i < lanePermitCount; ++i) {
        const auto& p = lanePermits[i];
        executorLog("RENDER_DIAG_EXEC_PERMIT token=%08x d=%u held_ms=%llu rot=%llu/%llums safepoint=%llu/%llums "
                    "wait_return=%llu/%llums delay_return=%llu/%llums start=%llu/%llums waits=%llu delays=%llu "
                    "grant=%llu/%lluus spin_hits=%llu spin_misses=%llu spin_ms=%llu deferred=%llu",
                    p.token, p.domain, ms(p.held), static_cast<unsigned long long>(p.count[PermitRotation]), ms(p.ns[PermitRotation]),
                    static_cast<unsigned long long>(p.count[PermitSafepoint]), ms(p.ns[PermitSafepoint]),
                    static_cast<unsigned long long>(p.count[PermitWaitReturn]), ms(p.ns[PermitWaitReturn]),
                    static_cast<unsigned long long>(p.count[PermitDelayReturn]), ms(p.ns[PermitDelayReturn]),
                    static_cast<unsigned long long>(p.count[PermitStart]), ms(p.ns[PermitStart]),
                    static_cast<unsigned long long>(p.waits), static_cast<unsigned long long>(p.delays),
                    static_cast<unsigned long long>(p.grants), static_cast<unsigned long long>(p.grantNs / 1000),
                    static_cast<unsigned long long>(p.spinHits), static_cast<unsigned long long>(p.spinMisses), ms(p.spinNs),
                    static_cast<unsigned long long>(p.deferred));
    }
    std::sort(sites.begin(), sites.end(), [](const ExclusiveSite& a, const ExclusiveSite& b) { return a.totalNs > b.totalNs; });
    static constexpr const char* KindNames[5]{"b1", "b256", "b4096", "handoff", "span"};
    for (unsigned i = 0; i < 12 && i < sites.size() && sites[i].context; ++i) {
        const auto& s = sites[i];
        executorLog("RENDER_DIAG_EXCL_SITE rank=%u site=%s kind=%s n=%llu total_ms=%llu max_us=%llu tts_ms=%llu tts_max_us=%llu "
                    "parked_ms=%llu parks=%llu blocked_ms=%llu blocked=%llu worker_start_ms=%llu worker_start_max_us=%llu",
                    i + 1, s.context, s.kind < 5 ? KindNames[s.kind] : "?", static_cast<unsigned long long>(s.count), ms(s.totalNs),
                    static_cast<unsigned long long>(s.maxNs / 1000), ms(s.ttsNs), static_cast<unsigned long long>(s.maxTtsNs / 1000),
                    ms(s.parkedNs), static_cast<unsigned long long>(s.parks), ms(s.blockedNs),
                    static_cast<unsigned long long>(s.blockedWaits), ms(s.workerStartNs),
                    static_cast<unsigned long long>(s.maxWorkerStartNs / 1000));
    }
    { // v224: the tasks that complete world stops (cumulative)
        auto rows = stopAuditCopy.last;
        std::sort(rows.begin(), rows.end(), [](const LastStopper& a, const LastStopper& b) { return a.ttsNs > b.ttsNs; });
        executorLog("RENDER_DIAG_V224_STOPS stops=%llu none_needed=%llu owner_wake_ms=%llu owner_wake_max_us=%llu",
                    static_cast<unsigned long long>(stopAuditCopy.stops), static_cast<unsigned long long>(stopAuditCopy.noneNeeded),
                    static_cast<unsigned long long>(stopAuditCopy.ownerWakeNs / 1000000), static_cast<unsigned long long>(stopAuditCopy.ownerWakeMaxNs / 1000));
        for (unsigned i = 0; i < 8 && rows[i].count; ++i)
            executorLog("RENDER_DIAG_V224_LAST_STOPPER rank=%u token=%08x label=%s n=%llu tts_ms=%llu mean_us=%llu max_us=%llu", i + 1,
                        rows[i].token, rows[i].label ? rows[i].label : "none", static_cast<unsigned long long>(rows[i].count),
                        static_cast<unsigned long long>(rows[i].ttsNs / 1000000),
                        static_cast<unsigned long long>(rows[i].ttsNs / 1000 / rows[i].count), static_cast<unsigned long long>(rows[i].maxNs / 1000));
    }
    executorLog("RENDER_DIAG_EXCL_NESTED b1=%llu b256=%llu b4096=%llu handoff=%llu span=%llu site_overflow=%llu span_enabled=%d",
                static_cast<unsigned long long>(nested[0]), static_cast<unsigned long long>(nested[1]),
                static_cast<unsigned long long>(nested[2]), static_cast<unsigned long long>(nested[3]),
                static_cast<unsigned long long>(nested[4]), static_cast<unsigned long long>(siteOverflow), int(exclusiveSpanEnabled()));
    { // v110: scheduler-lock waits by call site (cumulative), top 12 by wait, with the waiting role split.
        std::array<SchedLockSite*, 12> top{};
        std::uint64_t acquisitions = 0, contended = 0, spun = 0, waitNs = 0;
        std::array<std::uint64_t, SchedRoles> roles{};
        for (auto* site = schedSites.load(std::memory_order_acquire); site; site = site->next) {
            acquisitions += site->acquisitions.load(std::memory_order_relaxed);
            contended += site->contended.load(std::memory_order_relaxed);
            spun += site->spun.load(std::memory_order_relaxed);
            const auto wait = site->waitNs.load(std::memory_order_relaxed);
            waitNs += wait;
            for (unsigned r = 0; r < SchedRoles; ++r) roles[r] += site->roleWaitNs[r].load(std::memory_order_relaxed);
            for (std::size_t i = 0; wait && i < top.size(); ++i) {
                if (!top[i] || wait > top[i]->waitNs.load(std::memory_order_relaxed)) {
                    for (std::size_t j = top.size() - 1; j > i; --j) top[j] = top[j - 1];
                    top[i] = site;
                    break;
                }
            }
        }
        const auto u = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
        executorLog("RENDER_DIAG_EXEC_SCHED_TOTAL spin=%d acq=%llu contended=%llu spun=%llu wait_ms=%llu main_ms=%llu render_ms=%llu "
                    "tile_ms=%llu audio_ms=%llu other_ms=%llu host_ms=%llu", int(schedSpinEnabled()), u(acquisitions), u(contended), u(spun),
                    ms(waitNs), ms(roles[SchedRoleMain]), ms(roles[SchedRoleRender]), ms(roles[SchedRoleTile]), ms(roles[SchedRoleAudio]),
                    ms(roles[SchedRoleOther]), ms(roles[SchedRoleHost]));
        for (std::size_t i = 0; i < top.size() && top[i]; ++i) {
            const auto& s = *top[i];
            const auto r = [&](unsigned role) { return static_cast<unsigned long long>(s.roleWaitNs[role].load(std::memory_order_relaxed) / 1000); };
            executorLogQuiet("RENDER_DIAG_EXEC_SCHED rank=%zu site=%s:%d acq=%llu contended=%llu spun=%llu wait_us=%llu max_wait_us=%llu "
                        "main_us=%llu render_us=%llu tile_us=%llu audio_us=%llu other_us=%llu host_us=%llu", i + 1, s.function, s.line,
                        u(s.acquisitions.load(std::memory_order_relaxed)), u(s.contended.load(std::memory_order_relaxed)),
                        u(s.spun.load(std::memory_order_relaxed)), u(s.waitNs.load(std::memory_order_relaxed) / 1000),
                        u(s.maxWaitNs.value.load(std::memory_order_relaxed) / 1000), r(SchedRoleMain), r(SchedRoleRender),
                        r(SchedRoleTile), r(SchedRoleAudio), r(SchedRoleOther), r(SchedRoleHost));
        }
    }
    try {
        std::lock_guard lock(S.lateMutex);
        for (const auto& entry : S.lateSites)
            if (entry.count) executorLogQuiet("RENDER_DIAG_EXEC_LATE site=%s count=%llu", entry.site ? entry.site : "unknown",
                                         static_cast<unsigned long long>(entry.count));
        if (S.lateOverflow) executorLogQuiet("RENDER_DIAG_EXEC_LATE site=other count=%llu", static_cast<unsigned long long>(S.lateOverflow));
    } catch (...) {}
}

// Supervisor: joins recycled worker threads with no lock held and logs counters at a low rate.
// v49: freeze forensics that do not depend on the trace sink. While files/private-trace/diag-dump-now
// exists (the harness touches it when a freeze rule fires), at most every 30 s: every live task's state,
// domain, priority and last slow-path label, then the registry's per-thread last-import dump (waits
// carry handle creator/signaler and guest stack return addresses). The file is left for the old path.
void dumpTasksForFreeze(ParallelState& S) {
    struct Row { std::uint32_t token; unsigned domain; int priority; TaskState state; bool inWait; const char* label; std::uint64_t entries; };
    std::vector<Row> rows;
    try {
        RECOMP_SCHED_LOCK(lock);
        for (const auto& task : S.tasks)
            if (task && !task->finished)
                rows.push_back({task->token, task->domain, task->priority.load(std::memory_order_relaxed), task->state, task->inWait,
                                task->lastLabel.load(std::memory_order_relaxed), task->entries});
    } catch (...) { return; }
    static const char* const names[] = {"created", "awaiting-permit", "running", "parked-wait", "parked-delay",
                                        "parked-safepoint", "in-handoff", "finished"};
    for (const auto& r : rows)
        executorLog("RENDER_DIAG_FREEZE_TASK token=%08x domain=%u priority=%d state=%s in_wait=%u last=%s entries=%llu", r.token,
                    r.domain, r.priority, names[static_cast<unsigned>(r.state)], unsigned(r.inWait), r.label ? r.label : "none",
                    static_cast<unsigned long long>(r.entries));
    diagnosticDumpImports();
    executorLog("RENDER_DIAG_FREEZE_DUMP tasks=%zu", rows.size());
}

// v199: native stacks at a stall, independent of the supervisor (which needs the scheduler lock to run at all).
// Every 5 s it samples the live tasks' guest entry counts, taking S.sched only by try_lock (up to 1 s). Three samples
// (10 s) without any task making an entry, or two samples in a row without the scheduler lock, dump every thread's
// native stack (diag_native_stacks.h) once per stall; progress re-arms it. files/private-trace/diag-native-stacks-now
// forces one dump per appearance (the file is left in place; remove it to re-arm). No effect while tasks progress.
void stallWatchdogMain(ParallelState& S, const std::atomic<bool>& stop) {
#if defined(__aarch64__) && defined(__linux__)
    std::uint64_t lastEntries = ~std::uint64_t{0};
    unsigned sameSamples = 0, lockMisses = 0;
    bool dumped = false, forcedSeen = false;
    const auto dump = [](const char* why, unsigned long long entries) {
        executorLog("RENDER_DIAG_NATIVE_STACKS_BEGIN reason=%s entries=%llu", why, entries);
        const int captured = recomp::diag::nativestacks::dumpAll([](const char* line) { executorLog("%s", line); });
        executorLog("RENDER_DIAG_NATIVE_STACKS_END captured=%d", captured);
    };
    while (!stop.load(std::memory_order_acquire)) {
        for (int i = 0; i < 50 && !stop.load(std::memory_order_acquire); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stop.load(std::memory_order_acquire)) return;
        const bool forced = diagSwitchPresent(RECOMP_DIAG_SWITCH("diag-native-stacks-now"));
        if (forced && !forcedSeen) dump("forced", 0);
        forcedSeen = forced;
        bool locked = false;
        for (int i = 0; i < 100 && !(locked = S.sched.try_lock()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!locked) {
            sameSamples = 0;
            if (++lockMisses >= 2 && !dumped) { dumped = true; dump("sched-lock", 0); }
            continue;
        }
        std::uint64_t entries = 0;
        for (const auto& task : S.tasks) if (task && !task->finished) entries += task->entries;
        S.sched.unlock();
        lockMisses = 0;
        if (entries != lastEntries) { lastEntries = entries; sameSamples = 0; dumped = false; continue; }
        if (++sameSamples >= 2 && !dumped) { dumped = true; dump("no-entries", static_cast<unsigned long long>(entries)); }
    }
#else
    (void)S; (void)stop;
#endif
}

void supervisorMain(ParallelState& S) {
    std::atomic<bool> watchdogStop{false};
    std::thread watchdog([&S, &watchdogStop] { stallWatchdogMain(S, watchdogStop); });
    struct JoinWatchdog { std::atomic<bool>& stop; std::thread& thread;
        ~JoinWatchdog() { stop.store(true, std::memory_order_release); if (thread.joinable()) thread.join(); } } joinWatchdog{watchdogStop, watchdog};
    RECOMP_SCHED_LOCK(lock);
    Clock::time_point lastFreezeDump{};
    for (;;) {
        while (!S.zombies.empty()) {
            std::thread zombie = std::move(S.zombies.back());
            S.zombies.pop_back();
            lock.unlock();
            if (zombie.joinable()) zombie.join();
            RECOMP_SCHED_RELOCK(lock);
        }
        if (S.supervisorStop) return;
        if (Clock::now() >= S.nextLog) {
            S.nextLog = Clock::now() + LogInterval;
            lock.unlock();
            logCounters(S, false);
#if defined(__ANDROID__)
            if (::access(RECOMP_APP_PATH("/private-trace/diag-dump-now"), F_OK) == 0 &&
                (lastFreezeDump == Clock::time_point{} || Clock::now() - lastFreezeDump > std::chrono::seconds(30))) {
                lastFreezeDump = Clock::now();
                dumpTasksForFreeze(S);
            }
#endif
            RECOMP_SCHED_RELOCK(lock);
            continue;
        }
        const auto wakeAt = S.nextLog;
        S.supervisorCv.wait_until(lock, wakeAt, [&] { return S.supervisorStop || !S.zombies.empty(); });
    }
}

// v362: the caller's ParallelState (the gate and its admission check already hold it: parallelState() is a function-local static,
// an acquire load of its guard per call). Same tests in the same order as liveTask(self).
Task* liveTaskIn(const ParallelState& S, const GuestCooperativeExecutor* self) noexcept {
    const bool fast = recomp::v362::onFastThread(); // v362
    Task* t = currentTaskOf(fast);
    if (!t) return nullptr;
    if (!S.active.load(std::memory_order_acquire) || S.executor != self || !guardActive(fast) || t->finished)
        return nullptr;
    return t;
}
Task* liveTask(const GuestCooperativeExecutor* self) noexcept { return liveTaskIn(parallelState(), self); }

bool stopping() noexcept { return (attentionBits() & AttentionStop) != 0; }

// For entry points never called under the registry mutex (waits, delays, native scopes, thread
// exit): a task that sees the stop there unwinds next, so it joins the serialised unwinding. Not
// used by enqueue/setPriority/boostEventWaiters/parkedTask, which run under the registry mutex.
bool stopObserved(ParallelState& S, Task& t) {
    if (!stopping()) return false;
    if (!t.unwinding) {
        S.unwindMutex.lock();
        t.unwinding = true;
    }
    return true;
}
} // namespace

// main-wait (mw_cs_spin.h): true when another task waits for the calling task's permit domain (its relaxed queue
// mirror, v117): a critical-section watch would then hold a permit a runnable domain-mate needs, so it does not watch.
extern "C" bool recomp_mw_domain_contended() noexcept {
    const Task* t = currentTask;
    if (!t || t->domain >= PermitDomains) return false;
    return parallelState().domains[t->domain].queued.load(std::memory_order_relaxed) != 0;
}

thread_local GuestCooperativeExecutor::Current* GuestCooperativeExecutor::current_ = nullptr; // Unused here.

GuestCooperativeExecutor::GuestCooperativeExecutor(ThreadFactory factory)
    : factory_(factory ? std::move(factory) : ThreadFactory([](Entry entry) {
          return std::thread(std::move(entry));
      })) {}

bool GuestCooperativeExecutor::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

GuestCooperativeResult GuestCooperativeExecutor::run(
        std::uint32_t mainToken,
        const Entry& entry,
        const GuestCooperativeLimits& limits,
        const BindThread& bindThread,
        const GuestExecutionGuard::FunctionTraceSink& traceSink) {
    if (mainToken == 0 || !entry || !bindThread) {
        return {{GuestExecutionStopReason::InvalidEntry, 0, "Executor entry or binding is empty"}};
    }
    if (limits.functionEntries == 0 || limits.quantum == 0 || limits.quantum > 1000000
            || limits.tasks == 0 || limits.tasks > MaximumTasks) {
        return {{GuestExecutionStopReason::InvalidBudget, 0, "Executor limits are outside their bounds"}};
    }
    if (currentTask != nullptr || GuestExecutionGuard::isActive()) {
        return {{GuestExecutionStopReason::NestedRun, 0, "Nested cooperative execution is not allowed"}};
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return {{GuestExecutionStopReason::NestedRun, 0, "Executor already has an active run"}};
    }
    auto& S = parallelState();
    const auto note = takeStartNote(mainToken);
    const unsigned mainDomain = PermitDomains > 1 ? evaluateDomainPolicy(note, PermitDomains) : 0U;
    std::shared_ptr<Task> main;
    bool setupFailed = false;
    {
        RECOMP_SCHED_LOCK(lock);
        if (S.active.load(std::memory_order_relaxed)) {
            lock.unlock();
            running_.store(false, std::memory_order_release);
            return {{GuestExecutionStopReason::NestedRun, 0, "Another parallel executor run is active"}};
        }
        try {
            S.tasks.clear();
            S.tasks.reserve(MaximumTasks);
            for (auto& d : S.domains) { setOwner(d, nullptr); d.queue.clear(); syncQueued(d); }
            S.running = 0;
            S.exclusiveOwner = nullptr;
            S.exclusiveOwnerView.store(nullptr);
            S.exclusiveDepth = 0;
            S.handoffActive.store(false);
            S.handoffParent = S.handoffTarget = nullptr;
            S.epoch = S.activity = 0;
            S.pendingArm = 0;
            S.terminal.reset();
            S.secondaryStops.clear();
            S.globalEntries.store(0);
            S.waits.store(0);
            S.switches.store(0);
            S.workersCreated.store(0);
            S.workersEntered.store(0);
            S.lastBound.store(0);
            S.probeActive = S.probeDone = false;
            S.probeInitiator = nullptr;
            S.probeRemaining = 0;
            S.probeActivity = 0;
            S.supervisorStop = false;
            S.zombies.clear();
            S.zombies.reserve(256);
            S.counters.~Counters();
            new (&S.counters) Counters();
            {
                std::lock_guard late(S.lateMutex);
                S.lateSites = {};
                S.lateOverflow = 0;
            }
            S.executor = this;
            S.limits = limits;
            S.bindThread = bindThread;
            S.traceSink = traceSink;
            S.factory = &factory_;
            S.startedAt = Clock::now();
            S.nextLog = S.startedAt + LogInterval;
            main = std::make_shared<Task>();
            main->token = mainToken;
            main->index = 0;
            main->domain = mainDomain;
            main->entry = entry;
            main->armed = true;
            S.tasks.push_back(main);
            S.taskTokens[0] = mainToken; // v185
            lowerAttention(AttentionStop | AttentionExclusive | AttentionArm);
            S.active.store(true, std::memory_order_release);
            S.supervisor = std::thread([&S] { supervisorMain(S); });
            lowerAttention(AttentionIdle);
        } catch (...) {
            setupFailed = true;
            S.active.store(true, std::memory_order_release);
        }
    }
    if (!setupFailed) {
        logTaskCreated(note, 0, main->domain);
        currentTask = main.get();
        bool granted = false;
        try {
            RECOMP_SCHED_LOCK(lock);
            acquirePermitLocked(S, *main, lock);
            granted = true;
        } catch (...) {
            releaseUnwind(S, *main);
        }
        if (granted) runTaskBody(S, main);
        currentTask = nullptr;
    }
    std::vector<std::thread> hosts;
    {
        RECOMP_SCHED_LOCK(lock);
        if (setupFailed && !S.terminal) {
            setTerminalLocked(S, stopResult(GuestExecutionStopReason::HostException, "Executor setup failed",
                    S.globalEntries.load(std::memory_order_relaxed)));
        }
        S.coordinator.wait(lock, [&] { return S.terminal.has_value(); });
        for (auto& task : S.tasks) if (task && task->host.joinable()) hosts.push_back(std::move(task->host));
    }
    for (auto& host : hosts) host.join(); // Never join with a lock held.
    hostPool().shutdown(); // v240: every pooled task has returned (terminal set: workerMain returns at once)
    {
        RECOMP_SCHED_LOCK(lock);
        S.supervisorStop = true;
        S.supervisorCv.notify_all();
    }
    if (S.supervisor.joinable()) S.supervisor.join();
    logCounters(S, true);
    GuestCooperativeResult result;
    {
        RECOMP_SCHED_LOCK(lock);
        result = GuestCooperativeResult{std::move(*S.terminal), S.workersCreated.load(), S.workersEntered.load(),
                                        S.switches.load(), S.waits.load()};
        result.execution.completedFunctionEntries = S.globalEntries.load();
        S.tasks.clear();
        for (auto& d : S.domains) { setOwner(d, nullptr); d.queue.clear(); syncQueued(d); }
        S.terminal.reset();
        S.bindThread = {};
        S.traceSink = {};
        S.factory = nullptr;
        S.executor = nullptr;
        raiseAttention(AttentionIdle);
        S.active.store(false, std::memory_order_release);
    }
    running_.store(false, std::memory_order_release);
    return result;
}

std::uint32_t GuestCooperativeExecutor::currentTaskToken() const noexcept {
    const Task* t = liveTask(this);
    return t && !stopping() ? t->token : 0;
}

bool GuestCooperativeExecutor::parkedTask(std::uint32_t token) const noexcept {
    if (!currentTaskToken() || !token) return false;
    auto& S = parallelState();
    Task* self = currentTask;
    try {
        RECOMP_SCHED_LOCK(lock);
        if (self->evaluating) self->observesParking = true; // Wake this waiter when tasks park.
        for (const auto& task : S.tasks)
            if (task && task->token == token)
                return task->entered && !task->finished && task->inWait && task->state != TaskState::Running;
    } catch (...) {}
    return false;
}

bool GuestCooperativeExecutor::terminateCurrentThread(std::uint32_t token, const Ready& commitExit) {
    auto& S = parallelState();
    Task* t = liveTask(this);
    if (!t || stopObserved(S, *t) || t->scopeActive || (S.handoffActive.load(std::memory_order_acquire) && domainStoppedView(S, t->domain)) || /* v193 */ !commitExit ||
        t->token != token) return false;
    if (!commitExit()) return false;
    throw ThreadExited{};
}

bool GuestCooperativeExecutor::enqueue(std::uint32_t token, const Entry& entry, std::int32_t priority) {
    const bool pooled = std::exchange(poolNextEnqueue, false) && !v240Baseline(); // v240
    const auto note = takeStartNote(token);
    auto& S = parallelState();
    Task* creator = liveTask(this);
    if (!creator || stopping() || token == 0 || !entry ||
        !validPortableSchedulerPriority(priority, S.limits.nativeHighPriorityTiers)) return false;
    const unsigned domain = PermitDomains > 1 ? evaluateDomainPolicy(note, PermitDomains) : 0U; // Before sched.
    RECOMP_SCHED_LOCK(lock);
    if (S.terminal) return false;
    std::size_t reuse = MaximumTasks;
    for (std::size_t i = 0; i < S.tasks.size(); ++i) {
        const auto& task = S.tasks[i];
        if (task && task->token == token) {
            if (i == 0 || !task->finished) return false;
            reuse = i;
        }
    }
    if (reuse == MaximumTasks && S.tasks.size() >= S.limits.tasks)
        for (std::size_t i = 1; i < S.tasks.size(); ++i) if (S.tasks[i] && S.tasks[i]->finished) { reuse = i; break; }
    if (reuse == MaximumTasks && S.tasks.size() >= S.limits.tasks) return false;
    std::shared_ptr<Task> task;
    try {
        task = std::make_shared<Task>();
        task->entry = entry;
        creator->children.reserve(creator->children.size() + 1);
    } catch (const std::bad_alloc&) {
        return false;
    }
    task->token = token;
    task->index = reuse == MaximumTasks ? S.tasks.size() : reuse;
    task->domain = domain;
    task->priority.store(priority, std::memory_order_relaxed);
    std::thread host;
    bool onPool = false; // v240: the task's host is a pooled thread (task->host stays empty)
    if (pooled) {
        try { onPool = hostPool().run([&S, task] { workerMain(S, task); }); } catch (...) { onPool = false; }
        (onPool ? pooledTasks : pooledFallbacks).fetch_add(1, std::memory_order_relaxed);
    }
    if (!onPool) {
    try {
        // The new thread blocks on sched until this call publishes the task.
        host = (*S.factory)([&S, task] { workerMain(S, task); });
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::system_error&) {
        return false;
    }
    if (!host.joinable()) return false;
    task->host = std::move(host);
    }
    if (reuse != MaximumTasks) {
        auto& old = S.tasks[reuse];
        if (old->host.joinable()) {
            S.zombies.push_back(std::move(old->host)); // Joined by the supervisor, no lock held.
            S.supervisorCv.notify_all();
        }
        old = task;
        if (reuse < S.taskTokens.size()) S.taskTokens[reuse] = token; // v185
    } else {
        S.tasks.push_back(task);
        if (S.tasks.size() <= S.taskTokens.size()) S.taskTokens[S.tasks.size() - 1] = token; // v185
    }
    if (creator->children.empty() && !S.pendingArm++) raiseAttention(AttentionArm);
    creator->children.push_back(task);
    ++S.activity;
    S.workersCreated.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    creator->rescheduleHint = true;
    logTaskCreated(note, priority, domain);
    return true;
}

bool GuestCooperativeExecutor::boostEventWaiters(const std::uint32_t* tokens, std::size_t count) {
    auto& S = parallelState();
    Task* self = liveTask(this);
    if (!self || stopping() || !tokens || !count || count > MaximumTasks) return false;
    RECOMP_SCHED_LOCK(lock);
    std::array<Task*, MaximumTasks> selected{};
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = 0; j < i; ++j) if (tokens[i] == tokens[j]) return false;
        for (const auto& task : S.tasks) {
            if (!task || task->token != tokens[i]) continue;
            if (task.get() == self || task->finished) return false;
            selected[i] = task.get();
            break;
        }
        if (!selected[i]) return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& task = *selected[i];
        // A waiter that registered with the registry but has not parked yet still gets its boost.
        const bool parked = task.inWait && task.state != TaskState::Running;
        (parked ? S.counters.boostsParked : S.counters.boostsPending).fetch_add(1, std::memory_order_relaxed);
        task.boostPending.store(true, std::memory_order_release);
    }
    return true;
}

bool GuestCooperativeExecutor::setPriority(std::uint32_t token, std::int32_t priority) {
    auto& S = parallelState();
    Task* self = liveTask(this);
    if (!self || stopping() || !validPortableSchedulerPriority(priority, S.limits.nativeHighPriorityTiers)) return false;
    RECOMP_SCHED_LOCK(lock);
    for (const auto& task : S.tasks) {
        if (task && task->token == token && !task->finished) {
            task->priority.store(priority, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool GuestCooperativeExecutor::waitUntil(const Ready& ready,
        std::optional<std::chrono::steady_clock::time_point> deadline) {
    RECOMP_IOS_MAIN_QOS_FROM_WAIT();
    auto& S = parallelState();
    Task* t = liveTask(this);
    if (!t || stopObserved(S, *t) || t->scopeActive || (S.handoffActive.load(std::memory_order_acquire) && domainStoppedView(S, t->domain)) || /* v193 */ !ready ||
        !reserveWait(S)) return false;
    [[maybe_unused]] iosWaits::Scope iosWaitSlot(recomp::bootstrap::currentGuestThreadToken()); // (ios-wait-slots)
    return parallelWait(S, *t, ready, deadline);
}

// v117: a zero-length delay (the registry passes the epoch time_point for 0 ticks) keeps the permit when nothing could
// run instead: no other task queued for the caller's domain and no stop or exclusive request pending. v115 took the
// scheduler lock, released and re-acquired the (uncontended) permit and read the clock for each; the main thread's
// Sleep(0) loop does that ~62k times/s (v114 device: parallelDelay 51 ms/s scheduler-lock waits on main, and the
// render thread's rotateIfContended 34 ms/s behind them). Any other case takes v115's parallelDelay unchanged.
// files/private-trace/diag-exec-slow-zero-yield restores v115 for every delay.
bool GuestCooperativeExecutor::delayUntil(std::chrono::steady_clock::time_point deadline) {
    auto& S = parallelState();
    Task* t = liveTask(this);
    if (!t || stopObserved(S, *t) || t->scopeActive || (S.handoffActive.load(std::memory_order_acquire) && domainStoppedView(S, t->domain)) || /* v193 */ !reserveWait(S))
        return false;
    if (deadline == std::chrono::steady_clock::time_point{} && zeroYieldFastEnabled() &&
        (attentionBits() & (AttentionStop | AttentionExclusive)) == 0 &&
        S.domains[t->domain].queued.load(std::memory_order_relaxed) == 0 && t->children.empty()) {
        S.counters.delays.fetch_add(1, std::memory_order_relaxed);
        S.counters.fastYields.fetch_add(1, std::memory_order_relaxed);
        ++t->delayCalls;
        t->sliceStart = t->entries; // relinquishes the slice, as parallelDelay does
        t->sliceBoost = false;
        return true;
    }
    return parallelDelay(S, *t, deadline);
}

bool GuestCooperativeExecutor::runNativeNonYielding(const Entry& entry, std::uint32_t maximumEntries) {
    auto& S = parallelState();
    Task* t = liveTask(this);
    if (!t || stopObserved(S, *t) || t->scopeActive || !entry || !maximumEntries || maximumEntries > 4096) return false;
    const unsigned kind = maximumEntries <= 1 ? 0U : maximumEntries <= 256 ? 1U : 2U;
    const auto started = Clock::now();
    ExclusiveSite* site = nullptr; // v110: set when this call opened the outermost scope
    {
        RECOMP_SCHED_LOCK(lock);
        if (beginExclusiveLocked(S, *t, lock, kind)) site = S.activeSite;
    }
    t->scopeActive = true;
    t->scopeBudget = maximumEntries;
    t->scopeEntries = 0;
    struct Clear {
        ParallelState& S;
        Task& t;
        unsigned kind;
        Clock::time_point started;
        ExclusiveSite* site;
        ~Clear() {
            t.scopeActive = false;
            const auto ns = elapsedNs(started);
            try {
                RECOMP_SCHED_LOCK(lock);
                if (site) recordExclusiveEndLocked(site, ns); // v110 (before the scope's end clears activeSite)
                endExclusiveLocked(S, t);
            } catch (...) {}
            S.counters.exclusiveCount[kind].fetch_add(1, std::memory_order_relaxed);
            S.counters.exclusiveNs[kind].fetch_add(ns, std::memory_order_relaxed);
            S.counters.exclusiveMaxNs.update(ns);
        }
    } clear{S, *t, kind, started, site};
    entry();
    return true;
}

// v29 (attempt): the renderer's inlineGate bodies are host-only (budget 1) and every caller is a
// domain-0 D3D/driver entry, so they need exclusion among renderer users, not a stop of the world.
// In v28 the stop-the-world form ran about 22,000 times a second, parked the audio lane each time
// (21,000 safepoint parks/s) and kept 60% of wall time inside exclusive scopes. Here the scope keeps
// the non-yielding contract (budget 1, no safepoint park, no rotation, waits rejected, nesting
// refused) and takes one mutex instead. It waits for an exclusive owner first, so a lock holder is
// never a task that an exclusive scope is waiting to park. Budget-1 scopes that need every writer
// stopped (the tile-scene admission) keep runNativeNonYielding.
// v43 (declared in trace_runner.cpp): 1 when runNativeLockedGate would run a body for the calling task now,
// 0 when it would refuse, by its own refusal checks in the same order (including stopObserved's joining
// of the serialised unwind). It takes no lock and does not wait for an exclusive owner, as the gate would
// before running the body; a caller that proceeds meets that wait at its next gate or safepoint.
int nativeLockedGateAdmits(GuestCooperativeExecutor& executor) {
    auto& S = parallelState();
    Task* t = liveTaskIn(S, &executor); // v362
    return (!t || stopObserved(S, *t) || t->scopeActive) ? 0 : 1;
}
extern "C" { std::atomic<bool> recomp_v330c_baseline{false}; }
extern "C" { std::atomic<unsigned> recomp_v346_main_spin_us{0}; }
extern "C" { std::atomic<bool> recomp_v407_swap_delay{false}; } // v407 (the renderer's switch poll: diag-v407-swap-delay) // v346/v347 (the renderer's switch poll: diag-v346-main-spin = 40 us, diag-v347-spin100 = 100 us) // v330c: diag-v330c-baseline = the thread_local tick (a TLS resolver call)
bool runNativeLockedGate(GuestCooperativeExecutor& executor, const GuestExecutionGuard::Entry& entry) {
    auto& S = parallelState();
    Task* t = liveTaskIn(S, &executor); // v362
    if (!t || stopObserved(S, *t) || t->scopeActive || !entry) return false;
    // v34: three clock reads per gate were 80% of the render thread's clock_gettime calls (~5% of its
    // CPU at ~54k gates/s); count every gate and time one in 256.
    static thread_local unsigned timingTick = 0;
    const bool timed = recomp_v330c_baseline.load(std::memory_order_relaxed) ? !(timingTick++ & 255u) : !(t->gateTimingTick++ & 255u); // v330c
    const auto started = timed ? Clock::now() : Clock::time_point{};
    if (S.exclusiveOwnerView.load(std::memory_order_acquire) &&
        S.exclusiveOwnerView.load(std::memory_order_acquire) != t && domainStoppedView(S, t->domain)) { // (v191)
        RECOMP_SCHED_LOCK(lock);
        while (S.exclusiveOwner && S.exclusiveOwner != t && domainStopped(S, t->domain)) {
            if (S.terminal) cancelRun(lock);
            parkAtSafepointLocked(S, *t, lock);
        }
        lock.unlock();
        resumeTask(S, *t);
    }
    std::unique_lock gate(S.nativeLockGate);
    const auto locked = timed ? Clock::now() : Clock::time_point{};
    t->scopeActive = true;
    t->scopeBudget = 1;
    t->scopeEntries = 0;
    struct Clear {
        ParallelState& S;
        Task& t;
        bool timed;
        Clock::time_point started, locked;
        ~Clear() {
            t.scopeActive = false;
            // v286: counted per thread, published every 256 gates (v284 outdoor render profile: this shared fetch_add 0.63% of
            // the render thread); the diagnostic lock_gates line is off by < 256 per thread. v292: published with the timed
            // gate (1 in 256 of this thread's timingTick) instead of a second thread_local counter (a TLS resolver call).
            if (!timed) return;
            S.counters.lockGates.fetch_add(256, std::memory_order_relaxed);
            const auto ns = elapsedNs(locked); // sampled 1/256: lock_gate_ms and wait_ms are x256 estimates
            S.counters.lockGateNs.fetch_add(ns * 256, std::memory_order_relaxed);
            S.counters.lockGateWaitNs.fetch_add(
                256 * static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(locked - started).count()),
                std::memory_order_relaxed);
            S.counters.lockGateMaxNs.update(ns);
        }
    } clear{S, *t, timed, started, locked};
    // v226 label; v286: a load and a store, not an exchange (only this task's thread writes its label; 0.69% as an RMW)
    const char* previousActivity = t->hostActivity.load(std::memory_order_relaxed);
    t->hostActivity.store("gate", std::memory_order_relaxed);
    struct RestoreActivity { Task& t; const char* p; ~RestoreActivity() { t.hostActivity.store(p, std::memory_order_relaxed); } } restoreActivity{*t, previousActivity};
    entry();
    return true;
}

bool GuestCooperativeExecutor::runNativeWorkerNext(std::uint32_t token, std::uint32_t maximumEntries) {
    auto& S = parallelState();
    Task* parent = liveTask(this);
    if (!parent || stopObserved(S, *parent) || parent->scopeActive || (S.handoffActive.load(std::memory_order_acquire) && domainStoppedView(S, parent->domain)) || /* v193 */
        !token || !maximumEntries || maximumEntries > 4096) return false;
    const auto started = Clock::now();
    RECOMP_SCHED_LOCK(lock);
    Task* worker = nullptr;
    for (std::size_t i = 1; i < S.tasks.size(); ++i)
        if (S.tasks[i] && S.tasks[i]->token == token) { worker = S.tasks[i].get(); break; }
    if (!worker || worker == parent || worker->entered || worker->finished || worker->inWait ||
        worker->state == TaskState::Running || S.terminal) return false;
    const bool outermost = beginExclusiveLocked(S, *parent, lock, ExclusiveHandoff);
    auto* const site = outermost ? S.activeSite : nullptr; // v110
    auto* const startSite = S.activeSite;                 // v110: worker start latency (outermost or span)
    const auto parentDepth = S.exclusiveDepth;
    flushEntries(S, *parent);
    auto& workerDomain = S.domains[worker->domain];
    workerDomain.queue.erase(std::remove(workerDomain.queue.begin(), workerDomain.queue.end(), worker),
                             workerDomain.queue.end());
    syncQueued(workerDomain);
    worker->armed = true;
    worker->handoffWorker = true;
    worker->handoffBudget = maximumEntries;
    S.handoffActive.store(true, std::memory_order_release);
    S.handoffParent = parent;
    S.handoffTarget = worker;
    // Ownership moves to the worker; the parent's permit is not passed to its domain queue.
    S.exclusiveOwner = worker;
    S.exclusiveOwnerView.store(worker, std::memory_order_release);
    S.exclusiveDepth = 1;
    auto& parentDomain = S.domains[parent->domain];
    if (parentDomain.owner == parent) {
        setOwner(parentDomain, nullptr);
        --S.running;
        parent->permitHeldNs += elapsedNs(parent->permitSince); // v44: the handoff moves the permit away.
    }
    parent->state = TaskState::InHandoff;
    if (S.exclusiveDomains != ~0ull) { // v191: a scoped span also stops the worker's domain; its owner parks at a checkpoint
        setExclusiveDomainsLocked(S, S.exclusiveDomains | (1ull << (worker->domain & 63)));
        S.exclusiveCv.wait(lock, [&] { return !workerDomain.owner || workerDomain.owner == worker || S.terminal.has_value(); });
    }
    worker->handoffGrantedAt = Clock::now(); // v110
    if (!workerDomain.owner) grantLocked(S, *worker);
    worker->cv.notify_all();
    parent->cvWaiting = true; // v184
    parent->cv.wait(lock, [&] { return worker->finished || S.terminal.has_value(); });
    parent->cvWaiting = false;
    const bool finished = worker->finished;
    S.handoffActive.store(false, std::memory_order_release);
    S.handoffParent = S.handoffTarget = nullptr;
    S.exclusiveOwner = parent;
    S.exclusiveOwnerView.store(parent, std::memory_order_release);
    S.exclusiveDepth = parentDepth;
    S.activeSite = startSite; // v110 (the worker's own nested scopes do not change it)
    const auto ns = elapsedNs(started);
    if (site) recordExclusiveEndLocked(site, ns);
    S.counters.exclusiveCount[ExclusiveHandoff].fetch_add(1, std::memory_order_relaxed);
    S.counters.exclusiveNs[ExclusiveHandoff].fetch_add(ns, std::memory_order_relaxed);
    S.counters.exclusiveMaxNs.update(ns);
    if (S.terminal) {
        clearExclusiveLocked(S);
        cancelRun(lock);
    }
    if (!parentDomain.owner) grantLocked(S, *parent);
    if (outermost) endExclusiveLocked(S, *parent);
    lock.unlock();
    resumeTask(S, *parent);
    return finished;
}

// v110: one exclusive scope around a sequence of nested ones (the tile-scene admission and its worker
// handoff): the world stops once, not once per scope. Strictly more exclusive than the sequence alone:
// no other task runs between the nested scopes. The worker handoff restores the owner and depth it
// found; the span ends the whole exclusive ownership of this task (it is the outermost scope).
bool runNativeExclusiveSpan(GuestCooperativeExecutor& executor, const GuestExecutionGuard::Entry& entry) {
    auto& S = parallelState();
    Task* t = liveTask(&executor);
    if (!t || stopObserved(S, *t) || t->scopeActive || (S.handoffActive.load(std::memory_order_acquire) && domainStoppedView(S, t->domain)) || /* v193 */ !entry) return false;
    if (!exclusiveSpanEnabled()) { // v109: the nested scopes each stop the world themselves
        entry();
        return true;
    }
    const auto started = Clock::now();
    bool outermost = false;
    ExclusiveSite* site = nullptr;
    {
        RECOMP_SCHED_LOCK(lock);
        outermost = beginExclusiveLocked(S, *t, lock, ExclusiveSpan);
        if (outermost) site = S.activeSite;
    }
    struct End {
        ParallelState& S;
        Task& t;
        bool outermost;
        ExclusiveSite* site;
        Clock::time_point started;
        ~End() {
            try {
                RECOMP_SCHED_LOCK(lock);
                if (site) recordExclusiveEndLocked(site, elapsedNs(started));
                if (!outermost) endExclusiveLocked(S, t);
                else if (S.exclusiveOwner == &t) clearExclusiveLocked(S); // every nested level ended inside
            } catch (...) {}
        }
    } end{S, *t, outermost, site, started};
    entry();
    return true;
}

bool guestExclusiveScopeActive() noexcept {
    return parallelState().exclusiveOwnerView.load(std::memory_order_acquire) != nullptr;
}

namespace {
// async-wake: the waker (aw_waker.h) behind diag-aw-all / -all2k; created at main's first handed notify, never destroyed.
struct AwNotify { void operator()(std::shared_ptr<Task>& t) const noexcept { if (t) t->cv.notify_all(); } };
recomp_aw::Waker<std::shared_ptr<Task>, AwNotify>& awWaker() {
    static auto* w = new recomp_aw::Waker<std::shared_ptr<Task>, AwNotify>(AwNotify{});
    return *w;
}
// async-wake: wakeGuestWaiters while diag-aw-* is on. Under the scheduler lock exactly today's steps (wakeSeq bump, cvWaiting
// test, counters), except that a task already notified in its current parallelWait park wait gets no second notify (its
// predicate is already true) - unless the first came from main (it may still sit in the waker's queue) and this one from
// another thread (whose inline notify may arrive first, as today). After the unlock, main's notify goes to the waker; any
// other caller's stays inline. A waker switched off still drains its outstanding notifies in order (main's later notifies
// queue behind them).
[[gnu::noinline]] void wakeGuestWaitersAw(ParallelState& S, std::uint32_t token, unsigned aw) noexcept {
    std::shared_ptr<Task> notified; // v185: destroyed after the lock (declared before it)
    const Task* c = currentTask;
    const bool byMain = c && c->token == recomp_v245::MainToken;
    try {
        RECOMP_SCHED_LOCK(lock);
        if (!S.active.load(std::memory_order_relaxed)) return;
        S.counters.wakeCalls.fetch_add(1, std::memory_order_relaxed);
        ++S.activity;
        const bool always = alwaysNotifyWakes();
        const std::size_t count = S.tasks.size(), dense = std::min(count, S.taskTokens.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (i < dense ? S.taskTokens[i] != token : (!S.tasks[i] || S.tasks[i]->token != token)) continue; // v185
            auto& task = S.tasks[i];
            if (!task || task->finished) continue;
            ++task->wakeSeq;
            if (task->cvWaiting || always) {
#if !defined(RECOMP_AW_MUTANT_DEDUPE_ANY) // mutant: dedupe across park waits (a later park's first notify is lost)
                if (task->cvWaiting && task->seqWait && task->notifiedGen == task->cvGen && (byMain || !task->notifiedByMain) && !always) {
#else
                if (task->cvWaiting && task->notifiedGen != 0 && !always) {
#endif
                    recomp_aw::stats().deduped.fetch_add(1, std::memory_order_relaxed); // repeat notify of this park wait
                    continue;
                }
                if (task->seqWait) { task->notifiedGen = task->cvGen; task->notifiedByMain = byMain; }
                if (!notified && !notifyUnderLock()) notified = task;
                else task->cv.notify_all();
                S.counters.wakeNotifySent.fetch_add(1, std::memory_order_relaxed);
            } else {
                S.counters.wakeNotifySkipped.fetch_add(1, std::memory_order_relaxed);
                task->skippedSeq = task->wakeSeq;
            }
        }
        lock.unlock();
        if (!notified) return;
        auto& w = awWaker();
        if (byMain && ((aw & recomp_aw::ModeWaker) || w.outstanding()))
            w.deliver(&notified, 1, (aw & recomp_aw::ModeWaker) != 0, notified->token == recomp_v245::RenderToken,
                      recomp_aw_spin_us.load(std::memory_order_relaxed));
        else
            notified->cv.notify_all();
    } catch (...) {}
}
} // namespace
} // namespace recomp::bootstrap
extern "C" unsigned long long recomp_aw_outstanding() noexcept { return recomp::bootstrap::awWaker().outstanding(); }
namespace recomp::bootstrap {

void wakeGuestWaiters(std::uint32_t token) noexcept {
    if (!token) return;
    if (token == recomp_v245::RenderToken) { // v245: main leaves cpu7 before the render thread can run there again
        const Task* c = currentTask;
        if (c && c->token == recomp_v245::MainToken) recomp_v245::state().release(true);
    }
    auto& S = parallelState();
    if (!S.active.load(std::memory_order_acquire)) return;
    if (const unsigned aw = recomp_aw::mode()) { wakeGuestWaitersAw(S, token, aw); return; } // async-wake (diag-aw-*)
    std::shared_ptr<Task> notified; // v185: destroyed after the lock (declared before it)
    try {
        RECOMP_SCHED_LOCK(lock);
        if (!S.active.load(std::memory_order_relaxed)) return;
        S.counters.wakeCalls.fetch_add(1, std::memory_order_relaxed);
        ++S.activity;
        const std::size_t count = S.tasks.size(), dense = std::min(count, S.taskTokens.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (i < dense ? S.taskTokens[i] != token : (!S.tasks[i] || S.tasks[i]->token != token)) continue; // v185
            auto& task = S.tasks[i];
            if (!task || task->finished) continue;
            ++task->wakeSeq;
            // v184: a task not blocked in cv.wait re-reads wakeSeq under this lock before it sleeps (every wait
            // predicate reads it, or does not depend on it), so only a blocked one needs the notify (bionic's
            // pthread_cond_broadcast makes a futex syscall even with no waiter). diag-exec-always-notify restores v183.
            if (task->cvWaiting || alwaysNotifyWakes()) {
                // v185: notify after the unlock (the wakeSeq change is already published under the lock; the woken
                // thread then does not block on the lock this thread still holds). The reference keeps the Task
                // (and its condition variable) alive; a notify that arrives after the waiter left is spurious and
                // every wait re-checks its predicate. At most one live task per token: the rest notify here.
                if (!notified && !notifyUnderLock()) notified = task;
                else task->cv.notify_all();
                S.counters.wakeNotifySent.fetch_add(1, std::memory_order_relaxed);
            } else {
                S.counters.wakeNotifySkipped.fetch_add(1, std::memory_order_relaxed);
                task->skippedSeq = task->wakeSeq;
            }
        }
        lock.unlock();
        if (notified) notified->cv.notify_all();
    } catch (...) {}
}

void wakeAllGuestWaiters() noexcept {
    auto& S = parallelState();
    if (!S.active.load(std::memory_order_acquire)) return;
    try {
        RECOMP_SCHED_LOCK(lock);
        if (!S.active.load(std::memory_order_relaxed)) return;
        S.counters.wakeAllCalls.fetch_add(1, std::memory_order_relaxed);
        wakeAllLocked(S);
    } catch (...) {}
}

std::uint32_t currentGuestThreadToken() noexcept {
    const Task* t = currentTaskOf(recomp::v362::onFastThread()); // v362
    return t ? t->token : 0;
}
// v226 DIAGNOSTIC: the calling task's host-region label (RENDER_DIAG_V224_LAST_STOPPER groups world-stop delays by it).
// v312: skipped (no TLS lookup, no exchange) in the release profile (RECOMP_RELEASE_PROFILE) and while files/private-trace/
// diag-v312-release exists (the renderer's window poll sets recomp_v312_release); the world-stop lines then carry no labels.
} // namespace recomp::bootstrap
extern "C" std::atomic<bool> recomp_v312_release; std::atomic<bool> recomp_v312_release{false};
namespace recomp::bootstrap {
const char* exchangeGuestHostActivity(const char* label) noexcept {
#if defined(RECOMP_RELEASE_PROFILE)
    (void)label;return nullptr;
#endif
    if (::recomp_v312_release.load(std::memory_order_relaxed)) return nullptr;
    Task* t = currentTask;
    return t ? t->hostActivity.exchange(label, std::memory_order_relaxed) : nullptr;
}

// v184 host test hook (tests/wake_notify_test.cpp).
void wakeNotifyCountersForTest(std::uint64_t& sent, std::uint64_t& skipped, std::uint64_t& atTimeout) noexcept {
    auto& c = parallelState().counters;
    sent = c.wakeNotifySent.load(std::memory_order_relaxed);
    skipped = c.wakeNotifySkipped.load(std::memory_order_relaxed);
    atTimeout = c.wakeAtTimeout.load(std::memory_order_relaxed);
}
std::uint64_t skippedAtTimeoutForTest() noexcept { return parallelState().counters.skippedAtTimeout.load(std::memory_order_relaxed); }

GuestParallelStats guestParallelStats() noexcept {
    auto& S = parallelState();
    auto& c = S.counters;
    const auto v = [](const std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    GuestParallelStats s{};
    s.mode = RECOMP_GUEST_THREADS;
    s.slowCheckpoints = v(c.slowCheckpoints);
    s.permitAcquired = v(c.permitAcquired);
    s.permitContended = v(c.permitContended);
    s.permitWaitMaxNs = c.permitWaitMaxNs.value.load(std::memory_order_relaxed);
    s.permitHoldMaxNs = c.permitHoldMaxNs.value.load(std::memory_order_relaxed);
    s.maxConcurrentPermits = c.maxConcurrent.value.load(std::memory_order_relaxed);
    s.rotations = v(c.rotations);
    s.parks = v(c.parks);
    s.wakeNotified = v(c.wakeNotified);
    s.wakePoll = v(c.wakePoll);
    s.wakeDeadline = v(c.wakeDeadline);
    s.lateWakeups = v(c.lateWakeups);
    s.wakeCalls = v(c.wakeCalls);
    s.wakeAllCalls = v(c.wakeAllCalls);
    s.safepointParks = v(c.safepointParks);
    s.safepointMaxNs = c.safepointMaxNs.value.load(std::memory_order_relaxed);
    s.exclusiveScopes = v(c.exclusiveCount[0]) + v(c.exclusiveCount[1]) + v(c.exclusiveCount[2]);
    s.handoffs = v(c.exclusiveCount[ExclusiveHandoff]);
    s.probes = v(c.probes);
    s.probesConfirmed = v(c.probesConfirmed);
    s.boostsParked = v(c.boostsParked);
    s.boostsPending = v(c.boostsPending);
    s.delays = v(c.delays);
    try {
        std::lock_guard lock(S.lateMutex);
        std::array<GuestParallelLateSite, 32> sorted = S.lateSites;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
        std::copy_n(sorted.begin(), s.lateSites.size(), s.lateSites.begin());
    } catch (...) {}
    return s;
}

} // namespace recomp::bootstrap

extern "C" void recomp_guest_execution_checkpoint_slow(std::int32_t* countdown, const char* label, std::size_t length) {
    using namespace recomp::bootstrap;
    const auto view = boundedCheckpointLabel(label, length);
    Task* t = currentTask;
    auto& S = parallelState();
    if (!t || !S.active.load(std::memory_order_acquire)) {
        // Outside a parallel run (setup, the executor-less guard.run path): the old chain, every entry.
        if (countdown) *reinterpret_cast<GuestCheckpointCountdown*>(countdown) = 0;
        GuestExecutionGuard::checkpoint(view);
        return;
    }
    parallelCheckpoint(S, *t, reinterpret_cast<GuestCheckpointCountdown*>(countdown), view);
}
#endif
