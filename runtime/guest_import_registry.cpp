#include <atomic>
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v248_baseline; // v248 (generic_renderer.cpp switch poll)
extern "C" __attribute__((weak)) bool recomp_mw_domain_contended() noexcept; // main-wait (guest_cooperative_executor.cpp)
#include "guest_import_registry.h"
#include "native_gamepad_delivery.h"
#include "diag_timeline.h" // v195 DIAGNOSTIC: wait objects and signals on the timeline
#include "physical_adjacent_cover.h" // v207
#include "cs_range_query.h" // v217
#include "mw_cs_spin.h" // main-wait: diag-mw-cs-spin
#include <sched.h>
#include "prime_swap_v245.h" // main-sched: whether main holds cpu7 (v245 swap)
#include "main_sched_ms.h"   // main-sched: diag-ms-prime-noyield
namespace recomp_ms {
// The Sleep(0) fast path's host yield (main-sched): skipped while main holds cpu7 and diag-ms-prime-noyield is on (nothing of
// ours can be queued on cpu7 then; the yield only hands it to other processes); otherwise sched_yield exactly as before. Out
// of line: dispatch keeps its CFG (and PGO record).
[[gnu::noinline]] void sleepZeroYield(std::uint32_t token) noexcept {
    const auto& sw = recomp_v245::state();
    if (skipYield(state().mode.load(std::memory_order_relaxed), sw.swappedView.load(std::memory_order_relaxed),
                  sw.extrasEnabled.load(std::memory_order_relaxed), token == recomp_v245::MainToken)) {
        state().skipped.fetch_add(1, std::memory_order_relaxed); // (main only, on cpu7: uncontended)
        return;
    }
    ::sched_yield();
}
} // namespace recomp_ms
#include "diag_log_sample.h" // quiet-log: quietLog / quietLogThin (diag-quiet-log)
#include <array>
#include <atomic>
#include <mutex>
#include <unistd.h>
// v217 A/B (set by the renderer every 60 presents from files/private-trace/diag-v217-baseline): true restores v216's
// full critical-section scans and thread-return broadcasts.
extern "C" std::atomic<bool> recomp_v217_baseline;
std::atomic<bool> recomp_v217_baseline{false};
#include "recomp_app_dir.h" // data-paths: the app's files directory. imports-fast (agent F, imports_fast.h): the in-run switch (files/private-trace/diag-imports-fast, polled by the renderer).
extern "C" std::atomic<bool> recomp_imports_fast;
std::atomic<bool> recomp_imports_fast{false};
namespace {
struct TimelineWaitObject { // v195: the object the next park waits on (restored after the handler returns)
    std::uint32_t previous;
    explicit TimelineWaitObject(std::uint32_t object) noexcept : previous(recomp::diag::timeline::waitObject) {
        recomp::diag::timeline::waitObject = object; }
    ~TimelineWaitObject() { recomp::diag::timeline::waitObject = previous; }
};
inline void timelineSignal(std::uint16_t kind, std::uint32_t object) noexcept {
    recomp::diag::timeline::mark(recomp::diag::timeline::Signal, kind, object); }
// v197 (goal step 3, registry lock split): critical-section state (criticalSections_, criticalSectionWaits_ and the
// guest CRITICAL_SECTION words they shadow) is also guarded by 64 address stripes. The uncontended RtlEnter/Leave/
// TryEnter fast path (opt-in files/private-trace/diag-registry-cs-fast) holds only the stripe of its address, never R.
// Every other access holds R and ALL stripes (CsAll, re-entrant per thread, taken in stripe order), so the fast path
// never observes a structural change of either map and no slow path observes a half-updated entry.
constexpr unsigned CsStripeCount=64;
std::array<std::mutex,CsStripeCount> csStripeLocks;
thread_local unsigned csAllDepth=0;
inline unsigned csStripe(std::uint32_t address) noexcept { return (address>>4)&(CsStripeCount-1); }
class CsAll {
public:
    CsAll() { acquire(); }
    ~CsAll() { if(held_) release(); }
    void release() { held_=false; if(!--csAllDepth) for(auto it=csStripeLocks.rbegin();it!=csStripeLocks.rend();++it) it->unlock(); }
    void acquire() { held_=true; if(!csAllDepth++) for(auto& m:csStripeLocks) m.lock(); }
    bool held() const noexcept { return held_; }
    CsAll(const CsAll&)=delete; CsAll& operator=(const CsAll&)=delete;
private:
    bool held_=false;
};
std::atomic<unsigned long long> csFastEnters{0},csFastLeaves{0},csFastTries{0},csFastFallbacks{0};
// main-wait (mw_cs_spin.h): a domain-mate waits for this task's permit (executor; weak: absent = never).
inline bool mwDomainContended() noexcept { return &recomp_mw_domain_contended && recomp_mw_domain_contended(); }
// v197: a recorded critical-section rejection (lastCriticalSectionRejection_, R-protected) is cleared by the next
// locked RtlEnterCriticalSection: while one is recorded, enters take the locked path.
std::atomic<bool> csRejectionRecorded{false};
// v207: graphics views accepted across adjacent physical allocations (diag-registry-adjacent-graphics-views).
std::atomic<unsigned long long> adjacentGraphicsViews{0};
// v198 (goal step 3): handle-wait predicate pre-check (opt-in files/private-trace/diag-registry-wait-epoch). A handle's
// availability for a waiter changes only through a state change that calls wakeHandleWaiters(handle) (signal, release,
// close, timer set/cancel) or a broadcast (wakeAllIfRegistryWaits: thread exit, new producers, removed waits). Each
// bumps an epoch AFTER its change: a bucket per (registry, handle) and one global epoch. A parked waiter's poll that
// finds both unchanged since its last full (locked) check, which found the object unavailable, and is before its
// deadline/timer returns false without R, at most 3 polls in a row (the 4th checks under R: a change reaching a
// waiter by another path is late by at most three poll steps, never lost).
std::array<std::atomic<std::uint32_t>,1024> handleEpochs{};
std::atomic<unsigned long long> v217QuietReturns{0}; // v217: thread returns that broadcast no wake
std::atomic<std::uint32_t> globalHandleEpoch{0};
std::atomic<unsigned long long> waitEpochSkips{0},waitEpochChecks{0};
inline std::atomic<std::uint32_t>& handleEpoch(const void* registry,std::uint32_t handle) noexcept {
    const auto key=(reinterpret_cast<std::uintptr_t>(registry)>>4)^(std::uintptr_t{handle}*0x9E3779B1u);
    return handleEpochs[(key>>7)&1023];
}
#if defined(RECOMP_DIAG_SWITCH_DIR)
#define RECOMP_WAIT_EPOCH_SWITCH RECOMP_DIAG_SWITCH("diag-registry-wait-epoch")
#else
#define RECOMP_WAIT_EPOCH_SWITCH RECOMP_APP_PATH("/private-trace/diag-registry-wait-epoch")
#endif
bool waitEpochEnabled() { static const bool on=::access(RECOMP_WAIT_EPOCH_SWITCH,F_OK)==0; return on; }
#if defined(RECOMP_DIAG_SWITCH_DIR)
#define RECOMP_CS_FAST_SWITCH RECOMP_DIAG_SWITCH("diag-registry-cs-fast") // host tests
#else
#define RECOMP_CS_FAST_SWITCH RECOMP_APP_PATH("/private-trace/diag-registry-cs-fast")
#endif
bool csFastEnabled() {
    static const bool on=::access(RECOMP_CS_FAST_SWITCH,F_OK)==0;
    return on;
}
}
extern "C" { std::atomic<unsigned> recomp_mw_cs_spin_us{0}; } // main-wait: diag-mw-cs-spin budget (0 = off)
namespace recomp::bootstrap {
bool guestExclusiveScopeActive() noexcept;
// main-wait host test hook: watches, cleared, expired, stopped, acquired, queued, watch ns.
std::array<unsigned long long,7> criticalSectionSpinCounters() noexcept {
    const auto& c=recomp_mw::counters();
    return {c.watches.load(),c.cleared.load(),c.expired.load(),c.stopped.load(),c.acquired.load(),c.queued.load(),c.watchNs.load()};
}
// v197 host test hook (tests/cs_fast_stress_test.cpp): enters, leaves, tries, fallbacks of the lock-free path.
std::array<unsigned long long,4> criticalSectionFastCounters() noexcept {
    return {csFastEnters.load(),csFastLeaves.load(),csFastTries.load(),csFastFallbacks.load()};
}
// v198 host test hook (tests/wait_epoch_test.cpp): predicate polls that skipped R, and locked checks.
std::array<unsigned long long,2> waitEpochCounters() noexcept { return {waitEpochSkips.load(),waitEpochChecks.load()}; }
}
#include <android/log.h>
#include <unistd.h> // v126: access()
#include <sched.h> // v168: sched_yield()
#include "poll_sleep_v266.h" // v266
#include <deque>
#include <array>
#include <chrono>
#include <mutex>
#include <functional>
#include <map>
#include <unordered_map> // v160: diagnostic stack ranges
#include <optional>
#include "native_scaler_timing.h"
#include "guest_clock.h"
#include "guest_me_init.h"
#include "guest_thread_priority.h"
#include "native_display_information.h"
#include "native_serialized_resource_batch.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <thread> // v42: diagnostic record spin (before guest_registry_concurrency.h)
#include <utility>
#include <type_traits>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

// PARALLEL (multicore plan WP2, local-analysis/parallel-design/plan.md): only with
// RECOMP_GUEST_THREADS > 0 (1 gil, 2 lanes). The default cooperative build keeps
// today's code: every helper below collapses to the registry-wide member and to
// empty wake collectors.
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS > 0
#define RECOMP_PARALLEL_REGISTRY 1
#include "guest_execution_guard.h"
#include "guest_parallel_wake.h"
#include "audio_ahead.h" // audio-ahead: engine-signalled handles, main's waits on them, the worker hint (diag-au-ahead)
#include <atomic>
#include <vector>
#else
#define RECOMP_PARALLEL_REGISTRY 0
#endif

namespace recomp::bootstrap {
// v183 (goal step 2): the graphics-submission handler (trace_runner) runs the ring walk's completion stores after the
// renderer's pending reads (queued behind them) instead of draining the renderer before every submission. A native
// submission handler that passed its read-only admission checks and may publish completion calls this first; the
// hook (thread-local, set by the handler on the submitting thread) drains the renderer. Callable under R: the
// renderer's worker never takes R.
thread_local void (*nativeSubmissionCommitHook)() = nullptr;
void setNativeSubmissionCommitHook(void (*hook)()) noexcept { nativeSubmissionCommitHook = hook; }
void noteNativeSubmissionMayCommit() noexcept { if (const auto hook = nativeSubmissionCommitHook) hook(); }
// v183: per-site counts of admissions that drained (RENDER_DIAG_DEFERRED_RETIRE_SITES, trace_runner, every 10 s).
std::array<std::atomic<unsigned long long>,32> nativeSubmissionCommitSites{};
// v188 DIAGNOSTIC: owner of each plain spinlock word (token, acquisitions), kept under R by the plain spinlock path;
// read by the scene-IRQ entry refusal. Per-token acquisition counts of the ordinary tile lock (device+11156) go to
// RENDER_DIAG_PLAIN_SPIN lines (first 16 acquirers, then every 4096th acquisition).
static std::map<std::uint32_t,std::pair<std::uint32_t,std::uint64_t>> plainSpinOwnersV188;
static std::map<std::uint32_t,std::uint64_t> plainSpinAcquirersV188;
unsigned long long v217QuietReturnCount() noexcept { return v217QuietReturns.load(std::memory_order_relaxed); } // v217
void noteNativeSubmissionMayCommitAt(unsigned site) noexcept {
    if (const auto hook = nativeSubmissionCommitHook) {
        nativeSubmissionCommitSites[site < 32 ? site : 31].fetch_add(1, std::memory_order_relaxed);
        hook();
    }
}
// Attempt v42: defined before guest_registry_concurrency.h poisons the member name mutex_.
std::mutex& GuestImportRegistry::registryMutex() const noexcept { return mutex_; }
// ios-handle-describe (Claude 2026-10-21; DIAGNOSTIC): c412 hung with main waiting on a job-completion queue and every worker
// idle. The stall watcher (ios_sampler.h) asks, for each waited handle, the registry's kernel object state: a signalled
// object (count > 0) under a sleeping waiter is a lost wake in the runtime. Installed once from dispatch(); it try-locks the
// registry mutex for up to 50 ms and reports "registry-busy" otherwise (the hang may hold it).
namespace { std::function<int(std::uint32_t,char*,std::size_t)> iosHandleDescriber; std::atomic<bool> iosHandleDescriberReady{false}; }
extern "C" int recomp_ios_describe_handle(std::uint32_t handle,char* out,std::size_t size) noexcept {
    if(!iosHandleDescriberReady.load(std::memory_order_acquire))return std::snprintf(out,size,"describer-not-installed");
    try {return iosHandleDescriber(handle,out,size);} catch(...) {return std::snprintf(out,size,"describer-failed");}
}
} // namespace recomp::bootstrap
#include "save_sandbox.h" // save-sandbox: diag-save-sandbox (filesRootFor: the root of the saves and the profile)
#include "guest_local_profile.h" // v142: local signed-in profile
#include "guest_save_store.h" // v142: persistent saved-game content
#include <random>
#include <set>
#include <cstdlib>
#include "fast_tls_v362.h" // v362 (before the registry-section header, which poisons member names)
#include "guest_registry_concurrency.h" // Last include: every R holder below is a registry section.
#include "guest_registry_concurrency.inc"
// imports-fast D (imports_fast.h): kernelHandles_.find through the per-thread node cache while the switch is on. Callers
// hold R; every erase / clear of kernelHandles_ bumps its key's generation (eraseKey / clearAll, also under R).
#define RECOMP_FIND_HANDLE(key) (::recomp::imports_fast::on() \
        ? ::recomp::imports_fast::find(kernelHandles_, (key), ::recomp::imports_fast::handleGenerations(), ::recomp::imports_fast::handleCache()) \
        : kernelHandles_.find(key))

namespace recomp::bootstrap {
namespace {

#if RECOMP_PARALLEL_REGISTRY
// Identity (R2): the calling host thread's own guest thread. bindGuestThreadToken
// stores it for the calling thread. initialize()/reset() of any registry bump the
// epoch, invalidating every earlier binding. It is consulted only while the calling
// thread executes guest code (GuestExecutionGuard::isActive): host threads, setup
// and post-run inspection on the coordinator keep today's registry-wide member.
struct BoundGuestThread {
    const GuestImportRegistry* registry{nullptr};
    std::uint64_t epoch{0};
    std::uint32_t token{0};
};
thread_local BoundGuestThread boundGuestThread;
std::atomic<std::uint64_t> guestIdentityEpoch{1};
std::uint32_t registryCurrentToken(const GuestImportRegistry* registry, std::uint32_t member) noexcept {
    const auto& bound = boundGuestThread;
    return bound.registry == registry && bound.token != 0 &&
            bound.epoch == guestIdentityEpoch.load(std::memory_order_acquire) &&
            GuestExecutionGuard::isActive() ? bound.token : member;
}
void bindRegistryCurrentToken(const GuestImportRegistry* registry, std::uint32_t token) noexcept {
    boundGuestThread = {registry, guestIdentityEpoch.load(std::memory_order_acquire), token};
}
void clearRegistryCurrentToken(const GuestImportRegistry* registry, std::uint32_t token = 0) noexcept {
    if (boundGuestThread.registry == registry && (!token || boundGuestThread.token == token)) boundGuestThread = {};
}
void invalidateRegistryIdentities() noexcept { guestIdentityEpoch.fetch_add(1, std::memory_order_acq_rel); }

// Waits that may park right now, over all registries. A hint only: broadcasts are
// sent while it is nonzero. A waiter increments it under the registry mutex before
// it unlocks to park, so a signaller holding the mutex cannot miss a registered wait.
std::atomic<std::uint32_t> registryParkedWaits{0};

// NT-handle waits keep no waiter list in registry state; this side table does.
// Its mutex is a leaf: taken under the registry mutex, or alone while unwinding.
struct HandleWaiterTable {
    std::mutex lock;
    std::map<std::pair<const void*, std::uint32_t>, std::vector<std::uint32_t>> waiters;
    bool incomplete{false}; // A registration failed to allocate: handle signals broadcast.
};
HandleWaiterTable& handleWaiterTable() { static HandleWaiterTable table; return table; }

// Collects the wake requests of one import and delivers them after the registry
// mutex is released (open call). Construct it BEFORE the lock so that it is
// destroyed after the unlock. Thread-local; a nested collector restores the outer.
class RegistryWakes;
thread_local RegistryWakes* activeRegistryWakes = nullptr;
class RegistryWakes final {
public:
    RegistryWakes() noexcept : previous_(activeRegistryWakes) { activeRegistryWakes = this; }
    ~RegistryWakes() { flush(); activeRegistryWakes = previous_; }
    RegistryWakes(const RegistryWakes&) = delete;
    RegistryWakes& operator=(const RegistryWakes&) = delete;
    // A known waiter must re-evaluate its wait.
    static void wake(std::uint32_t token) noexcept {
        auto* active = activeRegistryWakes;
        if (!token) return;
        if (!active) { wakeGuestWaiters(token); return; } // No import in progress: deliver now.
        if (active->all_) return;
        for (std::size_t i = 0; i < active->count_; ++i) if (active->tokens_[i] == token) return;
        if (active->count_ == active->tokens_.size()) { active->all_ = true; return; }
        active->tokens_[active->count_++] = token;
    }
    // Unknown waiter set.
    static void wakeAll() noexcept {
        auto* active = activeRegistryWakes;
        if (!active) { wakeAllGuestWaiters(); return; }
        active->all_ = true;
    }
    // Deliver now. Callers have released the registry mutex (before parking).
    static void flushActive() noexcept { if (activeRegistryWakes) activeRegistryWakes->flush(); }
    void flush() noexcept {
        if (all_) wakeAllGuestWaiters();
        else for (std::size_t i = 0; i < count_; ++i) wakeGuestWaiters(tokens_[i]);
        all_ = false; count_ = 0;
    }
private:
    RegistryWakes* previous_;
    std::array<std::uint32_t, 16> tokens_{};
    std::size_t count_{0};
    bool all_{false};
};

// A registry wait that may park: counted in registryParkedWaits and, for an NT
// handle, registered in the side table. Constructed under the registry mutex.
class ParkedRegistryWait final {
public:
    ParkedRegistryWait(const GuestImportRegistry* registry, std::uint32_t handle, std::uint32_t token) noexcept
        : registry_(registry), handle_(handle), token_(token) {
        registryParkedWaits.fetch_add(1, std::memory_order_relaxed);
        if (!handle_) return;
        auto& table = handleWaiterTable();
        std::lock_guard lock(table.lock);
        try { table.waiters[{registry_, handle_}].push_back(token_); registered_ = true; }
        catch (...) { table.incomplete = true; }
    }
    ~ParkedRegistryWait() {
        if (registered_) {
            auto& table = handleWaiterTable();
            std::lock_guard lock(table.lock);
            if (const auto found = table.waiters.find({registry_, handle_}); found != table.waiters.end()) {
                auto& tokens = found->second;
                if (const auto at = std::find(tokens.begin(), tokens.end(), token_); at != tokens.end()) tokens.erase(at);
                if (tokens.empty()) table.waiters.erase(found);
            }
        }
        registryParkedWaits.fetch_sub(1, std::memory_order_relaxed);
    }
    ParkedRegistryWait(const ParkedRegistryWait&) = delete;
    ParkedRegistryWait& operator=(const ParkedRegistryWait&) = delete;
private:
    const GuestImportRegistry* registry_;
    std::uint32_t handle_, token_;
    bool registered_{false};
};

// A handle object changed state: wake the tasks waiting on that handle.
void wakeHandleWaiters(const GuestImportRegistry* registry, std::uint32_t handle) noexcept {
    timelineSignal(1, handle); // v195
    recomp_au_ahead::onHandleSignal(handle); // audio-ahead (out of line, atomics only)
    handleEpoch(registry, handle).fetch_add(1, std::memory_order_acq_rel); // v198 (after the state change)
    if (!registryParkedWaits.load(std::memory_order_relaxed)) return;
    auto& table = handleWaiterTable();
    std::lock_guard lock(table.lock);
    if (table.incomplete) { globalHandleEpoch.fetch_add(1, std::memory_order_acq_rel); RegistryWakes::wakeAll(); return; } // (v198)
    if (const auto found = table.waiters.find({registry, handle}); found != table.waiters.end())
        for (const auto token : found->second) RegistryWakes::wake(token);
}
// Unknown dependent set (thread exit, resume, new workers): broadcast only while a
// registry wait may be parked.
void wakeAllIfRegistryWaits() noexcept {
    globalHandleEpoch.fetch_add(1, std::memory_order_acq_rel); // v198
    if (registryParkedWaits.load(std::memory_order_relaxed)) RegistryWakes::wakeAll();
}
#else
inline void bindRegistryCurrentToken(const GuestImportRegistry*, std::uint32_t) noexcept {}
inline void clearRegistryCurrentToken(const GuestImportRegistry*, std::uint32_t = 0) noexcept {}
inline void invalidateRegistryIdentities() noexcept {}
class RegistryWakes final {
public:
    static void wake(std::uint32_t) noexcept {}
    static void wakeAll() noexcept {}
    static void flushActive() noexcept {}
};
class ParkedRegistryWait final {
public:
    ParkedRegistryWait(const GuestImportRegistry*, std::uint32_t, std::uint32_t) noexcept {}
};
inline void wakeHandleWaiters(const GuestImportRegistry*, std::uint32_t) noexcept {}
inline void wakeAllIfRegistryWaits() noexcept {}
#endif
// The calling guest thread (replaces every former read of the member). Member
// functions only. Cooperative builds read the registry-wide member exactly as before.
#if RECOMP_PARALLEL_REGISTRY
#define RECOMP_CURRENT_GUEST_TOKEN registryCurrentToken(this, boundGuestThreadToken_)
#else
#define RECOMP_CURRENT_GUEST_TOKEN boundGuestThreadToken_
#endif

constexpr std::uint32_t StatusSuccess = 0x00000000U;
constexpr std::uint32_t StatusTimeout = 0x00000102U;
constexpr std::uint32_t StatusInvalidHandle = 0xC0000008U;
constexpr std::uint32_t StatusInvalidParameter = 0xC000000DU;
constexpr std::uint32_t StatusNoMemory = 0xC0000017U;
constexpr std::uint32_t StatusObjectTypeMismatch = 0xC0000024U;
constexpr std::uint32_t StatusSemaphoreLimitExceeded = 0xC0000047U;
constexpr std::uint32_t StatusNotSupported = 0xC00000BBU;
constexpr std::uint32_t MemCommit = 0x00001000U;
constexpr std::uint32_t MemReserve = 0x00002000U;
constexpr std::uint32_t MemRelease = 0x00008000U;
constexpr std::uint32_t MemLargePages = 0x20000000U;
constexpr std::uint32_t MemHeap = 0x40000000U;
constexpr std::uint32_t Mem16MbPages = 0x80000000U;
constexpr std::uint32_t PageReadOnly = 0x00000002U;
constexpr std::uint32_t PageReadWrite = 0x00000004U;
constexpr std::uint32_t PageNoCache = 0x00000200U;
constexpr std::uint32_t PageWriteCombine = 0x00000400U;
constexpr std::uint32_t LargePageSize = 0x00010000U;
constexpr std::uint32_t SixteenMbPageSize = 0x01000000U;
constexpr std::uint32_t MaximumPhysicalMemoryType = 2;
constexpr std::uint32_t SupportedPhysicalProtection = PageReadOnly
        | PageReadWrite
        | PageNoCache
        | PageWriteCombine
        | MemLargePages
        | Mem16MbPages;
constexpr std::uint32_t UserProcessType = 1;
constexpr std::size_t MaximumDebugMessageBytes = 4096;
constexpr std::size_t MaximumAnsiStringBytes = 65534;
constexpr std::size_t MaximumUnicodeCodeUnits = 32766;
constexpr std::uint32_t CriticalSectionSize = 28;
constexpr std::uint32_t CriticalSectionLockCountOffset = 16;
constexpr std::uint32_t CriticalSectionRecursionCountOffset = 20;
constexpr std::uint32_t CriticalSectionOwningThreadOffset = 24;
constexpr std::uint32_t MaximumCriticalSectionRecursion =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
constexpr std::uint32_t XexHeaderSystemFlags = 0x00030000U;

bool isXboxRawPartition(std::string_view path) {
    const auto consume = [&](std::string_view prefix) {
        if (path.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            const auto c = path[i];
            if ((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != prefix[i]) return false;
        }
        path.remove_prefix(prefix.size());
        return true;
    };
    const auto number = [&] {
        std::size_t size = 0;
        while (size < path.size() && path[size] >= '0' && path[size] <= '9') ++size;
        path.remove_prefix(size);
        return size != 0;
    };
    return consume("\\device\\harddisk") && number() && consume("\\partition")
        && number() && path.empty();
}

constexpr std::array<GuestImportDescriptor, 137> ImplementedBootImports{{
        {"__imp__DbgPrint", GuestImportCategory::Logging, 1, true},
        {"__imp__KfAcquireSpinLock", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KfReleaseSpinLock", GuestImportCategory::Synchronization, 4294967295, false},
        {"__imp__KeAcquireSpinLockAtRaisedIrql", GuestImportCategory::Synchronization, 4294967295, false},
        {"__imp__KeReleaseSpinLockFromRaisedIrql", GuestImportCategory::Synchronization, 4294967295, false},
        {"__imp__VdInitializeScalerCommandBuffer", GuestImportCategory::Process, 4294967295, true},
        {"__imp__VdPersistDisplay", GuestImportCategory::Title, 4294967295, true},
        {"__imp__KeInitializeDpc", GuestImportCategory::Synchronization, 4294967295, false},
        {"__imp__KeEnterCriticalRegion", GuestImportCategory::Synchronization, 0, false},
        {"__imp__KeLeaveCriticalRegion", GuestImportCategory::Synchronization, 0, false},
        {"__imp__KeWaitForSingleObject", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeWaitForMultipleObjects", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeSetEvent", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeResetEvent", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__ExRegisterTitleTerminateNotification", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdInitializeEngines", GuestImportCategory::Title, 4294967295, true},
        {"__imp__VdRetrainEDRAMWorker", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdRetrainEDRAM", GuestImportCategory::Title, 4294967295, true},
        {"__imp__VdIsHSIOTrainingSucceeded", GuestImportCategory::Title, 4294967295, true},
        {"__imp__VdSetGraphicsInterruptCallback", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdSetSystemCommandBufferGpuIdentifierAddress", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdGetSystemCommandBuffer", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdSwap", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdSetDisplayMode", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdGetCurrentDisplayInformation", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdInitializeRingBuffer", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdEnableRingBufferRPtrWriteBack", GuestImportCategory::Title, 4294967295, false},
        {"__imp__KiApcNormalRoutineNop", GuestImportCategory::Title, 4294967295, false},
        {"__imp__XGetVideoMode", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdQueryVideoMode", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdCallGraphicsNotificationRoutines", GuestImportCategory::Title, 4294967295, false},
        {"__imp__VdGetCurrentDisplayGamma", GuestImportCategory::Title, 4294967295, false},
        {"__imp__ExGetXConfigSetting", GuestImportCategory::Title, 4294967295, true},
        {"__imp__RtlTimeFieldsToTime", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamContentGetDeviceData", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamContentCreateEx", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamContentClose", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamContentCreateEnumerator", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamEnumerate", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamGetSystemVersion", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamInputGetKeystrokeEx", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamInputGetState", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamInputGetCapabilities", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamInputSetState", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XAudioGetSpeakerConfig", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XMACreateContext", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XMAReleaseContext", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XAudioRegisterRenderDriverClient", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XAudioSubmitRenderDriverFrame", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XAudioGetVoiceCategoryVolume", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XAudioEnableDucker", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XMsgStartIORequest", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XMsgInProcessCall", GuestImportCategory::Title, 4294967295, true},
        {"__imp__NtQueryFullAttributesFile", GuestImportCategory::Title, 4294967295, true},
        {"__imp__NtCreateFile", GuestImportCategory::Title, 4294967295, true},
        {"__imp__ObCreateSymbolicLink", GuestImportCategory::Title, 4294967295, true},
        {"__imp__ObDeleteSymbolicLink", GuestImportCategory::Title, 4294967295, true},
        {"__imp__NtReadFile", GuestImportCategory::Title, 4294967295, true},
        {"__imp__NtQueryInformationFile", GuestImportCategory::Title, 4294967295, true},
        {"__imp__NtSetInformationFile", GuestImportCategory::Title, 4294967295, true},
        {"__imp__NetDll_WSACleanup", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_WSAGetLastError", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_XNetCleanup", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_XNetStartup", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_XNetGetOpt", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_XNetGetEthernetLinkStatus", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_XNetGetTitleXnAddr", GuestImportCategory::Process, 4294967295, true},
        {"__imp__NetDll_WSAStartup", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XexLoadImage", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XexGetProcedureAddress", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XexGetModuleHandle", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XexUnloadImage", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamNotifyCreateListener", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__XNotifyGetNext", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__XNotifyPositionUI", GuestImportCategory::Process, 4294967295, false},
        {"__imp__XamUserGetSigninState", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamUserCheckPrivilege", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamUserGetXUID", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamUserGetSigninInfo", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamUserGetName", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamUserGetDeviceContext", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamVoiceIsActiveProcess", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamUserReadProfileSettings", GuestImportCategory::Process, 4294967295, true},
        {"__imp__XamLoaderGetLaunchDataSize", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamLoaderGetLaunchData", GuestImportCategory::Title, 4294967295, true},
        {"__imp__XamSwapDisc", GuestImportCategory::Title, 4294967295, true},
        {"__imp__RtlRaiseException", GuestImportCategory::Logging, 4294967295, false},
        {"__imp__RtlNtStatusToDosError", GuestImportCategory::Process, 4294967295, true},
        {"__imp__ExCreateThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__ExTerminateThread", GuestImportCategory::Synchronization, 4294967295, false},
        {"__imp__KeGetCurrentProcessType", GuestImportCategory::Process, 4, true},
        {"__imp__KeQueryBasePriorityThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeQuerySystemTime", GuestImportCategory::Process, 4294967295, false},
        {"__imp__KeQueryPerformanceFrequency", GuestImportCategory::Process, 4294967295, true},
        {"__imp__KeSetAffinityThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeSetBasePriorityThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeSetDisableBoostThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__KeTlsAlloc", GuestImportCategory::ThreadLocal, 2, true},
        {"__imp__KeTlsFree", GuestImportCategory::ThreadLocal, 3, true},
        {"__imp__KeTlsGetValue", GuestImportCategory::ThreadLocal, 5, true},
        {"__imp__KeTlsSetValue", GuestImportCategory::ThreadLocal, 2, true},
        // UINT32_MAX means a static direct-call distance has not been assigned.
        {"__imp__MmAllocatePhysicalMemoryEx", GuestImportCategory::Memory, 4294967295, true},
        {"__imp__MmFreePhysicalMemory", GuestImportCategory::Memory, 4294967295, false},
        {"__imp__MmGetPhysicalAddress", GuestImportCategory::Memory, 4294967295, true},
        {"__imp__MmQueryAddressProtect", GuestImportCategory::Memory, 4294967295, true},
        {"__imp__MmQueryAllocationSize", GuestImportCategory::Memory, 4294967295, true},
        {"__imp__MmQueryStatistics", GuestImportCategory::Memory, 4294967295, true},
        {"__imp__NtAllocateVirtualMemory", GuestImportCategory::Heap, 4, true},
        {"__imp__NtClearEvent", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtClose", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtCreateEvent", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtCreateTimer", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtSetTimerEx", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtCancelTimer", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtDuplicateObject", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtCreateMutant", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtReleaseMutant", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtCreateSemaphore", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtFreeVirtualMemory", GuestImportCategory::Heap, 4, true},
        {"__imp__NtReleaseSemaphore", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtResumeThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtSetEvent", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__NtWaitForSingleObjectEx", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__ObDereferenceObject", GuestImportCategory::Synchronization, 4294967295, false},
        {"__imp__ObReferenceObjectByHandle", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__RtlCompareMemoryUlong", GuestImportCategory::Memory, 7, true},
        {"__imp__RtlFillMemoryUlong", GuestImportCategory::Memory, 4294967295, false},
        {"__imp__RtlInitAnsiString", GuestImportCategory::String, 7, false},
        {"__imp__RtlInitUnicodeString", GuestImportCategory::String, 7, false},
        {"__imp__RtlImageXexHeaderField", GuestImportCategory::Title, 3, true},
        {"__imp__RtlInitializeCriticalSection", GuestImportCategory::Synchronization, 4, false},
        {"__imp__RtlInitializeCriticalSectionAndSpinCount", GuestImportCategory::Synchronization, 4, true},
        {"__imp__RtlEnterCriticalSection", GuestImportCategory::Synchronization, 4, false},
        {"__imp__RtlTryEnterCriticalSection", GuestImportCategory::Synchronization, 4, true},
        {"__imp__KeDelayExecutionThread", GuestImportCategory::Synchronization, 4294967295, true},
        {"__imp__RtlLeaveCriticalSection", GuestImportCategory::Synchronization, 4, false},
        {"__imp__XexCheckExecutablePrivilege", GuestImportCategory::Title, 2, true},
}};

void defaultLogSink(std::string_view message) {
    constexpr std::string_view Prefix{"[Guest DbgPrint] "};
    std::fwrite(Prefix.data(), 1, Prefix.size(), stderr);
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Attempt v42: import results are constructed while R is held, and their two strings cost two heap
// allocations each time (the names and most literals exceed the small-string buffer). A result made
// from a string literal now carries short tagged strings instead (no allocation: 13 and 9 bytes);
// dispatch() replaces them with the real text after R is released (materializeDispatchResult).
// Every result leaves the registry through dispatch(), the only public member returning one; the
// name view is the dispatched name or a static `Name` literal, both alive until dispatch returns.
constexpr char DeferredNameTag = '\x01', DeferredTextTag = '\x02';
std::string deferredName(std::string_view name) {
    std::string tagged(1 + sizeof(const char*) + sizeof(std::uint32_t), DeferredNameTag);
    const char* data = name.data();
    const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(name.size(), UINT32_MAX));
    std::memcpy(&tagged[1], &data, sizeof data);
    std::memcpy(&tagged[1 + sizeof data], &size, sizeof size);
    return tagged;
}
std::string deferredText(const char* literal) {
    std::string tagged(1 + sizeof(const char*), DeferredTextTag);
    std::memcpy(&tagged[1], &literal, sizeof literal);
    return tagged;
}
// Never throws: the import has already taken effect, and handlers such as ExCreateThread promise
// that an allocation failure can only happen before they change state (they preallocate their
// results). Without memory for the text the result keeps its state and gets empty strings.
void materializeDispatchResult(GuestImportDispatchResult& result) noexcept {
    auto& name = result.importName;
    if (name.size() == 1 + sizeof(const char*) + sizeof(std::uint32_t) && name[0] == DeferredNameTag) {
        const char* data = nullptr; std::uint32_t size = 0;
        std::memcpy(&data, &name[1], sizeof data);
        std::memcpy(&size, &name[1 + sizeof data], sizeof size);
        try { name.assign(data, size); } catch (...) { name.clear(); }
    }
    auto& text = result.diagnostic;
    if (text.size() == 1 + sizeof(const char*) && text[0] == DeferredTextTag) {
        const char* literal = nullptr;
        std::memcpy(&literal, &text[1], sizeof literal);
        try { text.assign(literal); } catch (...) { text.clear(); }
    }
}

GuestImportDispatchResult handled(std::string_view name, const char* diagnostic) {
    return {GuestImportDispatchState::Handled, deferredName(name), deferredText(diagnostic)};
}
GuestImportDispatchResult handled(std::string_view name, std::string diagnostic) {
    return {GuestImportDispatchState::Handled, deferredName(name), std::move(diagnostic)};
}

GuestImportDispatchResult memoryFault(std::string_view name, const char* diagnostic) {
    return {GuestImportDispatchState::GuestMemoryFault, deferredName(name), deferredText(diagnostic)};
}
GuestImportDispatchResult memoryFault(std::string_view name, std::string diagnostic) {
    return {GuestImportDispatchState::GuestMemoryFault, deferredName(name), std::move(diagnostic)};
}

void logRejectedFileQuery(std::string_view name, std::string_view diagnostic) {
#if defined(__ANDROID__)
    // Cooperative waits also use Rejected transiently. Restrict this trace to
    // the file-query boundary so polling cannot flood logcat or perturb timing.
    if(name=="__imp__NtQueryFullAttributesFile")__android_log_print(ANDROID_LOG_WARN,"RecompCompatibility","REJECT %.*s: %.*s",
            static_cast<int>(std::min<std::size_t>(name.size(),128)),name.data(),
            static_cast<int>(std::min<std::size_t>(diagnostic.size(),1536)),diagnostic.data());
#else
    (void)name; (void)diagnostic;
#endif
}
GuestImportDispatchResult rejected(std::string_view name, const char* diagnostic) {
    logRejectedFileQuery(name, diagnostic);
    return {GuestImportDispatchState::Rejected, deferredName(name), deferredText(diagnostic)};
}
GuestImportDispatchResult rejected(std::string_view name, std::string diagnostic) {
    logRejectedFileQuery(name, diagnostic);
    return {GuestImportDispatchState::Rejected, deferredName(name), std::move(diagnostic)};
}

} // namespace

// The calling guest thread for GuestImportRegistry members defined in OTHER translation units
// (guest_native_initialization.cpp): exactly what RECOMP_CURRENT_GUEST_TOKEN gives in this one.
// While the calling host thread executes guest code that is its own binding; host threads, setup
// and post-run inspection keep the registry-wide member. Lanes audit L1: that translation unit read
// the member directly, and in lanes mode every resume in the other permit domain rebinds it
// (resumeTask -> bindGuestThreadToken), so a render-thread VdSwap saw the audio worker's token.
std::uint32_t guestImportRegistryCallerToken(const GuestImportRegistry* registry, std::uint32_t member) noexcept {
#if RECOMP_PARALLEL_REGISTRY
    return registryCurrentToken(registry, member);
#else
    (void)registry;
    return member;
#endif
}

const char* guestImportCategoryName(GuestImportCategory category) noexcept {
    switch (category) {
    case GuestImportCategory::Memory:
        return "memory";
    case GuestImportCategory::String:
        return "string";
    case GuestImportCategory::Heap:
        return "heap";
    case GuestImportCategory::Logging:
        return "logging";
    case GuestImportCategory::Process:
        return "process";
    case GuestImportCategory::Title:
        return "title";
    case GuestImportCategory::Synchronization:
        return "synchronization";
    case GuestImportCategory::ThreadLocal:
        return "thread-local";
    }
    return "unknown";
}

const char* guestImportDispatchStateName(GuestImportDispatchState state) noexcept {
    switch (state) {
    case GuestImportDispatchState::Handled:
        return "handled";
    case GuestImportDispatchState::NotRegistered:
        return "not-registered";
    case GuestImportDispatchState::RegistryUnavailable:
        return "registry-unavailable";
    case GuestImportDispatchState::GuestMemoryFault:
        return "guest-memory-fault";
    case GuestImportDispatchState::Rejected:
        return "rejected";
    }
    return "unknown";
}

namespace {
// v168: the zero-delay repeat cache (see GuestImportRegistry::dispatch). Written only under R by the calling host
// thread, read only by it.
// Its delays are still counted: per-registry atomic counters that threadDelayStatistics() adds to the locked ones
// (calls [0], completed [1], failed [2], zero-length [3], in flight [6]); zeroed by initialize() and reset().
struct FastDelayCounters {
    std::atomic<const GuestImportRegistry*> owner{nullptr};
    std::atomic<std::uint64_t> calls{0}, completed{0}, failed{0}, zeros{0}, inflight{0};
    void clear() noexcept { calls = 0; completed = 0; failed = 0; zeros = 0; inflight = 0; }
};
std::array<FastDelayCounters, 32> fastDelayCounterTable;
FastDelayCounters* fastDelayCountersFor(const GuestImportRegistry* registry, bool claim) noexcept {
    for (auto& c : fastDelayCounterTable) if (c.owner.load(std::memory_order_acquire) == registry) return &c;
    if (!claim) return nullptr;
    for (auto& c : fastDelayCounterTable) {
        const GuestImportRegistry* expected = nullptr;
        if (c.owner.compare_exchange_strong(expected, registry, std::memory_order_acq_rel)) { c.clear(); return &c; }
    }
    return nullptr;
}
struct ZeroYieldCache {
    const GuestImportRegistry* registry = nullptr;
    FastDelayCounters* counters = nullptr;
    std::uint32_t token = 0, interval = 0;
    std::uint64_t generation = ~std::uint64_t{0};
};
thread_local ZeroYieldCache zeroYieldCache;
// v252 (A/B: files/private-trace/diag-v252-baseline): two lock-free repeats of locked reads, both under the v42 snapshot
// invariant (guest_registry_concurrency.h point 4: a slot word with no open window and the cached generation means V is
// unchanged since the locked check that filled the cache).
//  a. copySerializedGuestArgument of the caller's own stack: the locked path validated the WHOLE stack range (so every
//     4-aligned sub-range passes validGuestDataBuffer: its rejections are overlaps/containment, monotone in the range)
//     and the stack layout (threadStorage_, in V). The caller is running (the live/not-returned guards hold, as for the
//     v168 zero-yield path).
//  b. KeTlsGetValue of an allocated slot of the caller's bound TLS storage (threadTlsStorage_, in V): the slot bit comes
//     from an atomic mirror of tlsSlots_ written under R after every change; the value is the same guest word the
//     locked path reads (a concurrent KeTlsFree linearizes before or after, both answers the locked path can give).
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v252_baseline;
bool v252Baseline() noexcept { return &recomp_v252_baseline && recomp_v252_baseline.load(std::memory_order_relaxed); }
struct ArgStackCache {const GuestImportRegistry* registry=nullptr;std::uint32_t token=0;std::uint64_t generation=~std::uint64_t{0};
    std::uint32_t stackBase=0;std::uint64_t stackEnd=0;};
thread_local ArgStackCache argStackCache;
struct TlsReadCache {const GuestImportRegistry* registry=nullptr;std::uint32_t token=0;std::uint64_t generation=~std::uint64_t{0};
    std::uint32_t base=0,slotCount=0;};
thread_local TlsReadCache tlsReadCache;
struct TlsSlotMirror {std::atomic<const GuestImportRegistry*> owner{nullptr};std::array<std::atomic<std::uint64_t>,32> bits{};};
std::array<TlsSlotMirror,4> tlsSlotMirrors;
TlsSlotMirror* tlsSlotMirror(const GuestImportRegistry* registry,bool claim) noexcept {
    for(auto& m:tlsSlotMirrors)if(m.owner.load(std::memory_order_acquire)==registry)return &m;
    if(!claim)return nullptr;
    for(auto& m:tlsSlotMirrors){const GuestImportRegistry* expected=nullptr;if(m.owner.compare_exchange_strong(expected,registry,std::memory_order_acq_rel))return &m;}
    return nullptr;
}
template<class Bits> void syncTlsSlotMirror(const GuestImportRegistry* registry,const Bits& slots) noexcept { // under R
    auto* m=tlsSlotMirror(registry,true);if(!m)return;
    static_assert(sizeof(std::uint64_t)*8*32>=2048);
    for(std::size_t w=0;w<32;++w){std::uint64_t v=0;for(std::size_t b=0;b<64;++b)if(w*64+b<slots.size() && slots.test(w*64+b))v|=std::uint64_t{1}<<b;
        m->bits[w].store(v,std::memory_order_release);}
}
std::atomic<unsigned long long> v252ArgFast{0},v252TlsFast{0};
}
extern "C" unsigned long long recomp_v252_arg_fast() noexcept { return v252ArgFast.load(std::memory_order_relaxed); }
extern "C" unsigned long long recomp_v252_tls_fast() noexcept { return v252TlsFast.load(std::memory_order_relaxed); }

// v168 test seam: lock-free zero-delay repeats of a registry (tests declare it weak; v167 has none).
extern "C" unsigned long long recomp_registry_zero_yield_fast_hits(const void* registry) {
    const auto* counters = fastDelayCountersFor(static_cast<const GuestImportRegistry*>(registry), false);
    return counters ? counters->calls.load(std::memory_order_relaxed) : 0;
}
bool GuestImportRegistry::initialize(
        GuestAddressSpace& addressSpace,
        std::uint32_t heapBase,
        std::uint32_t heapSize,
        GuestLogSink logSink) {
    [[maybe_unused]] RegistryWakes wakes; // PARALLEL: fired after the unlock.
    RECOMP_REGISTRY_LOCK(lock);
    clearPhysicalMemoryArena(true);
    clearThreadTlsStorage();
    RECOMP_V(threadStorage_).reset();
    RECOMP_V(threadStorageConfig_) = {};
    exactFunctionValidator_ = {};
    createdThreads_.clear();
    RECOMP_V(threadObjects_).clear();
    lastThreadPriorityRequest_.reset();
    RECOMP_V(threadObjectTypeAddress_) = 0;
    threadResumeHandler_ = {};
    threadExitHandler_ = {};
    fileMetadataQuery_ = {};
    symbolicLinks_.clear();
    lastFileCreateRequest_.reset();
    threadPriorityHandler_ = {};nativeEventWakeBoostHandler_={};nativeEventWakeBoostStatistics_={};
    virtualCpuAffinityBound_ = false;
    threadDebugNamingBound_ = false;
    threadWaitHandler_ = {};
    threadTimedWaitHandler_ = {};
    threadDelayHandler_ = {};
    threadDelayStatistics_ = {};
    if (auto* fast = fastDelayCountersFor(this, true)) fast->clear(); // v168
    clearNativeXamModule();
    // v42: atomic store; read without R by fastValidatedView to reject a stale snapshot.
    __atomic_store_n(&RECOMP_V(addressSpace_), static_cast<GuestAddressSpace*>(nullptr), __ATOMIC_RELEASE);
    RECOMP_V(heapBase_) = 0;
    RECOMP_V(heapSize_) = 0;
    freeBlocks_.clear();
    RECOMP_V(allocations_).clear();
    RECOMP_V(virtualCommitBits_).clear();
    virtualCommittedPages_ = 0;
    lastMemoryStatistics_.reset();
    imageProtections_ = {};
    { CsAll csAllGuard; // v197
    ::recomp::imports_fast::clearAll(criticalSections_, ::recomp::imports_fast::csGenerations());
    nativeCriticalSectionWaitsBound_=false;criticalSectionWaits_.clear();criticalSectionWaitStatistics_={}; }
    nativeHighPriorityTiersBound_=false;nativeHighPriorityTiersRequests_=0;
    lastCriticalSectionRejection_.reset();csRejectionRecorded.store(false,std::memory_order_release); // (v197)
    ::recomp::imports_fast::clearAll(kernelHandles_, ::recomp::imports_fast::handleGenerations());
    nativeEventReferences_.clear(); RECOMP_V(nativeEventPool_) = nativeEventSlots_ = 0;
    mountedDisc_ = mountedDiscCount_ = nativeDiscCompletions_ = 0;
    dispatcherEventWaits_.clear();
    openFiles_.clear(); readOnlyFileOpen_ = {};
    contentEnumerators_.clear(); installedContent_ = {};
    contentMounts_.clear(); contentPackages_.clear();
    contentMountRejection_ = 0;
    fileIoStatistics_ = {};
    notificationListeners_.clear();
    notificationServiceBound_ = false;
    notificationUiPosition_.reset();
    signedOutUsersBound_ = false;
    noLaunchDataBound_ = false;
    noRawStorageDevicesBound_ = false;
    userLanguage_.reset();
    userCountry_.reset();
    systemTimeSource_ = {};
    performanceFrequency_ = 0;
    virtualVideoModeBound_ = false;
    nativeScalerTimingBinding_.reset();nativeScalerTimingQueries_=0;
    nativeIdentityScalerBound_=false;pendingNativeScaler_.reset();nativeCommandInterruptWorker_.reset();postOutputSwapInterruptWorker_.reset();
    nativeInterruptScope_.reset();nativeInterruptScopeStatus_={};postOutputSwapInterruptScopeStatus_={};
    nativeTileLockScope_.reset();nativeTileLockScopeStatus_={};
    nativeTileSceneInterruptScope_.reset();nativeTileSceneInterruptStatus_={};nativeTileSceneWorker_.reset();
    retiredNativeTileInitialWorker_=0;retiredNativeTileFirstWorker_=0;retiredNativeTileFinalWorker_=0;
    postOutputWrappedSwapAttempted_=false;completedPostOutputWrappedSwap_.reset();
    postOutputSwapInterruptSnapshot_.reset();postOutputSwapInterruptSequence_.reset();
    postOutputSwapPacingRecord_.reset();
    deferredSwapInterruptWorker_.reset();deferredSwapInterruptScopeStatus_={};
    deferredSwapInterruptSnapshot_.reset();deferredSwapInterruptSequence_.reset();deferredSwapPacingRecord_.reset();
    deferredWrappedSwapAttempted_=false;completedDeferredWrappedSwap_.reset();
    nativeScalerInterruptStatus_={};nativeScalerInterruptPlan_.reset();nativeScalerInterruptScratchOwner_.reset();nativeScalerInterruptScratchBefore_={};
    nativeScalerTailAttempted_=false;completedNativeScaler_.reset();
    nativePostScalerStorageFailed_=false;completedPostScalerStorage_.reset();
    nativePostDisplayStorageFailed_=false;completedPostDisplayStorage_.reset();
    nativePostOutputStorageFailed_=false;completedPostOutputStorage_.reset();
    nativeRuntimeCommandStorageFailed_=false;completedRuntimeCommandStorage_.reset();
    nativeRuntimeMaintenanceFailed_=false;completedRuntimeMaintenance_.reset();
    postOutputColorAttempted_=false;completedPostOutputColor_.reset();pendingPostOutputColorImage_.reset();
    postOutputColorRejection_=0;postOutputColorDetail_=0;
    postOutputSystemContext_.reset();postSwapSystemContext_.reset();
    pendingPostOutputSwap_.reset();postOutputSwapStatus_={};pendingDeferredSwap_.reset();
    completedDeferredSwapStorage_.reset();deferredSwapStorageAttempted_=false;
    deferredColorPrefixAttempted_=false;deferredColorPrefixRejection_=deferredColorPrefixDetail_=0;
    completedDeferredColorPrefix_.reset();pendingDeferredColorImage_.reset();deferredColorSubmission_.reset();
    deferredColorPinnedAllocations_={};deferredColorPinCount_=0;
    postOutputSwapStorageAttempted_=false;completedPostOutputSwapStorage_.reset();
    nativeDisplayGammaAttempted_=false;completedDisplayGamma_.reset();nativeScanoutRequest_.reset();
    nativeDisplayOutputAttempted_=false;completedDisplayOutput_.reset();
    nativeInterruptDeviceRoot_={};
    nativeScalerStorageOwner_.reset();nativeScalerRing_.reset();completedScalerStorageCoherency_.reset();
    RECOMP_V(graphicsBackend_).reset(); graphicsCallbackValidator_ = {}; dpcCallbackValidator_ = {};
    graphicsImageBase_ = graphicsImageBytes_ = 0; RECOMP_V(graphicsInitialized_) = false; graphicsConfiguration_ = {};
    graphicsInterruptRegistration_.reset();
    graphicsRingConfiguration_.reset(); graphicsRingAllocation_.reset(); graphicsWriteBackAllocation_.reset();
    terminationImageBase_ = terminationImageBytes_ = 0;
    terminationCallbackValidator_ = {}; titleTerminationRegistrations_.clear();
    networkLifecycleBound_ = false;
    disconnectedInputBound_ = false;
    nativeGamepadReader_ = {};
    nativeGamepadCapabilities_ = {};
    audioCallbackEnqueue_={};audioRenderClient_.reset();audioBackend_.reset();audioSpeakerQueries_=0;
    nativeCategoryOneGainBits_.reset();nativeCategoryOneGainQueries_=0;
    xnetConfigured_ = false; xnetParameters_ = {}; winsockReferences_ = 0;
    boundGuestThreadToken_ = 0;
    invalidateRegistryIdentities(); clearRegistryCurrentToken(this); // PARALLEL: no stale thread binding.
    boundExecutableXexHeader_ = 0;
    tlsSlots_.reset();
    syncTlsSlotMirror(this, tlsSlots_); // v252
    threadTlsValues_.clear();
    logSink_ = {};

    if (!addressSpace.isReserved()
            || heapBase == 0
            || heapSize == 0
            || (heapBase % PageSize) != 0
            || (heapSize % PageSize) != 0
            || static_cast<std::uint64_t>(heapBase) + heapSize > GuestAddressSpace::Size) {
        return false;
    }

    try { RECOMP_V(virtualCommitBits_).assign((std::uint64_t{heapSize} / PageSize + 63) / 64, 0); }
    catch (const std::bad_alloc&) { return false; }
    __atomic_store_n(&RECOMP_V(addressSpace_), &addressSpace, __ATOMIC_RELEASE); // v42: see reset()
    RECOMP_V(heapBase_) = heapBase;
    RECOMP_V(heapSize_) = heapSize;
    freeBlocks_.emplace(heapBase, heapSize);
    logSink_ = logSink ? std::move(logSink) : GuestLogSink(defaultLogSink);
    return true;
}

void GuestImportRegistry::reset() {
    [[maybe_unused]] RegistryWakes wakes; // PARALLEL: fired after the unlock.
    RECOMP_REGISTRY_LOCK(lock);
    clearPhysicalMemoryArena(true);
    clearThreadTlsStorage();
    RECOMP_V(threadStorage_).reset();
    RECOMP_V(threadStorageConfig_) = {};
    exactFunctionValidator_ = {};
    createdThreads_.clear();
    RECOMP_V(threadObjects_).clear();
    lastThreadPriorityRequest_.reset();
    RECOMP_V(threadObjectTypeAddress_) = 0;
    threadResumeHandler_ = {};
    threadExitHandler_ = {};
    fileMetadataQuery_ = {};
    symbolicLinks_.clear();
    lastFileCreateRequest_.reset();
    threadPriorityHandler_ = {};nativeEventWakeBoostHandler_={};nativeEventWakeBoostStatistics_={};
    virtualCpuAffinityBound_ = false;
    threadDebugNamingBound_ = false;
    threadWaitHandler_ = {};
    threadTimedWaitHandler_ = {};
    threadDelayHandler_ = {};
    threadDelayStatistics_ = {};
    if (auto* fast = fastDelayCountersFor(this, true)) fast->clear(); // v168
    clearNativeXamModule();
    // v42: atomic store; read without R by fastValidatedView to reject a stale snapshot.
    __atomic_store_n(&RECOMP_V(addressSpace_), static_cast<GuestAddressSpace*>(nullptr), __ATOMIC_RELEASE);
    RECOMP_V(heapBase_) = 0;
    RECOMP_V(heapSize_) = 0;
    freeBlocks_.clear();
    RECOMP_V(allocations_).clear();
    RECOMP_V(virtualCommitBits_).clear();
    virtualCommittedPages_ = 0;
    lastMemoryStatistics_.reset();
    imageProtections_ = {};
    { CsAll csAllGuard; // v197
    ::recomp::imports_fast::clearAll(criticalSections_, ::recomp::imports_fast::csGenerations());
    nativeCriticalSectionWaitsBound_=false;criticalSectionWaits_.clear();criticalSectionWaitStatistics_={}; }
    nativeHighPriorityTiersBound_=false;nativeHighPriorityTiersRequests_=0;
    lastCriticalSectionRejection_.reset();csRejectionRecorded.store(false,std::memory_order_release); // (v197)
    ::recomp::imports_fast::clearAll(kernelHandles_, ::recomp::imports_fast::handleGenerations());
    nativeEventReferences_.clear(); RECOMP_V(nativeEventPool_) = nativeEventSlots_ = 0;
    mountedDisc_ = mountedDiscCount_ = nativeDiscCompletions_ = 0;
    dispatcherEventWaits_.clear();
    openFiles_.clear(); readOnlyFileOpen_ = {};
    contentEnumerators_.clear(); installedContent_ = {};
    contentMounts_.clear(); contentPackages_.clear();
    contentMountRejection_ = 0;
    fileIoStatistics_ = {};
    notificationListeners_.clear();
    notificationServiceBound_ = false;
    notificationUiPosition_.reset();
    signedOutUsersBound_ = false;
    noLaunchDataBound_ = false;
    noRawStorageDevicesBound_ = false;
    userLanguage_.reset();
    userCountry_.reset();
    systemTimeSource_ = {};
    performanceFrequency_ = 0;
    networkLifecycleBound_ = false;
    disconnectedInputBound_ = false;
    nativeGamepadReader_ = {};
    nativeGamepadCapabilities_ = {};
    audioCallbackEnqueue_={};audioRenderClient_.reset();audioBackend_.reset();audioSpeakerQueries_=0;
    nativeCategoryOneGainBits_.reset();nativeCategoryOneGainQueries_=0;
    virtualVideoModeBound_ = false;
    nativeScalerTimingBinding_.reset();nativeScalerTimingQueries_=0;
    nativeIdentityScalerBound_=false;pendingNativeScaler_.reset();nativeCommandInterruptWorker_.reset();postOutputSwapInterruptWorker_.reset();
    nativeInterruptScope_.reset();nativeInterruptScopeStatus_={};postOutputSwapInterruptScopeStatus_={};
    nativeTileLockScope_.reset();nativeTileLockScopeStatus_={};
    nativeTileSceneInterruptScope_.reset();nativeTileSceneInterruptStatus_={};nativeTileSceneWorker_.reset();
    retiredNativeTileInitialWorker_=0;retiredNativeTileFirstWorker_=0;retiredNativeTileFinalWorker_=0;
    postOutputWrappedSwapAttempted_=false;completedPostOutputWrappedSwap_.reset();
    postOutputSwapInterruptSnapshot_.reset();postOutputSwapInterruptSequence_.reset();
    postOutputSwapPacingRecord_.reset();
    deferredSwapInterruptWorker_.reset();deferredSwapInterruptScopeStatus_={};
    deferredSwapInterruptSnapshot_.reset();deferredSwapInterruptSequence_.reset();deferredSwapPacingRecord_.reset();
    deferredWrappedSwapAttempted_=false;completedDeferredWrappedSwap_.reset();
    nativeScalerInterruptStatus_={};nativeScalerInterruptPlan_.reset();nativeScalerInterruptScratchOwner_.reset();nativeScalerInterruptScratchBefore_={};
    nativeScalerTailAttempted_=false;completedNativeScaler_.reset();
    nativePostScalerStorageFailed_=false;completedPostScalerStorage_.reset();
    nativePostDisplayStorageFailed_=false;completedPostDisplayStorage_.reset();
    nativePostOutputStorageFailed_=false;completedPostOutputStorage_.reset();
    nativeRuntimeCommandStorageFailed_=false;completedRuntimeCommandStorage_.reset();
    nativeRuntimeMaintenanceFailed_=false;completedRuntimeMaintenance_.reset();
    postOutputColorAttempted_=false;completedPostOutputColor_.reset();pendingPostOutputColorImage_.reset();
    postOutputColorRejection_=0;postOutputColorDetail_=0;
    postOutputSystemContext_.reset();postSwapSystemContext_.reset();
    pendingPostOutputSwap_.reset();postOutputSwapStatus_={};pendingDeferredSwap_.reset();
    completedDeferredSwapStorage_.reset();deferredSwapStorageAttempted_=false;
    deferredColorPrefixAttempted_=false;deferredColorPrefixRejection_=deferredColorPrefixDetail_=0;
    completedDeferredColorPrefix_.reset();pendingDeferredColorImage_.reset();deferredColorSubmission_.reset();
    deferredColorPinnedAllocations_={};deferredColorPinCount_=0;
    postOutputSwapStorageAttempted_=false;completedPostOutputSwapStorage_.reset();
    nativeDisplayGammaAttempted_=false;completedDisplayGamma_.reset();nativeScanoutRequest_.reset();
    nativeDisplayOutputAttempted_=false;completedDisplayOutput_.reset();
    nativeInterruptDeviceRoot_={};
    nativeScalerStorageOwner_.reset();nativeScalerRing_.reset();completedScalerStorageCoherency_.reset();
    RECOMP_V(graphicsBackend_).reset(); graphicsCallbackValidator_ = {}; dpcCallbackValidator_ = {};
    graphicsImageBase_ = graphicsImageBytes_ = 0; RECOMP_V(graphicsInitialized_) = false; graphicsConfiguration_ = {};
    graphicsInterruptRegistration_.reset();
    graphicsRingConfiguration_.reset(); graphicsRingAllocation_.reset(); graphicsWriteBackAllocation_.reset();
    terminationImageBase_ = terminationImageBytes_ = 0;
    terminationCallbackValidator_ = {}; titleTerminationRegistrations_.clear();
    xnetConfigured_ = false; xnetParameters_ = {}; winsockReferences_ = 0;
    boundGuestThreadToken_ = 0;
    invalidateRegistryIdentities(); clearRegistryCurrentToken(this); // PARALLEL: no stale thread binding.
    boundExecutableXexHeader_ = 0;
    tlsSlots_.reset();
    syncTlsSlotMirror(this, tlsSlots_); // v252
    threadTlsValues_.clear();
    logSink_ = {};
}

#if RECOMP_PARALLEL_REGISTRY
namespace {
// v126: the executor rebinds a task's token at every resume (~65-70k/s over all threads, each under the registry lock,
// v119 device: bindGuestThreadToken 24.9 ms/s of lock waits). A task keeps its own host thread, so after the first bind
// the call only re-publishes what this thread already holds. files/private-trace/diag-registry-locked-bind restores v124.
thread_local const GuestImportRegistry* fastBoundRegistry = nullptr;
thread_local std::uint32_t fastBoundToken = 0;
std::atomic<unsigned long long> fastBinds{0};
bool fastBindEnabled() noexcept {
    static const bool enabled = ::access(RECOMP_APP_PATH("/private-trace/diag-registry-locked-bind"), F_OK) != 0;
    return enabled;
}
}
#endif
bool GuestImportRegistry::bindGuestThreadToken(std::uint32_t token) {
#if RECOMP_PARALLEL_REGISTRY
    // Same registry, same token, and this thread's identity still current (same epoch: no invalidation since): the
    // locked path below would change only the registry-wide member, which guest threads do not read in this mode
    // (RECOMP_CURRENT_GUEST_TOKEN uses the thread's own binding); threadTlsValues_ already holds the token.
    if (token != 0 && fastBoundToken == token && fastBoundRegistry == this && fastBindEnabled() &&
        registryCurrentToken(this, 0) == token) {
        const auto n = fastBinds.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0 && n >= 1024)
            __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_FAST_BIND n=%llu", n);
        return true;
    }
#endif
    RECOMP_REGISTRY_LOCK(lock);
    if (token == 0 || addressSpace_ == nullptr || !addressSpace_->isReserved()) {
        return false;
    }
    boundGuestThreadToken_ = token;
    threadTlsValues_.try_emplace(token);
    // PARALLEL: the calling host thread's own identity. Call on the task's own
    // host thread (the executor's bind does, as today).
    bindRegistryCurrentToken(this, token);
#if RECOMP_PARALLEL_REGISTRY
    fastBoundRegistry = this; fastBoundToken = token; // v126
#endif
    return true;
}

std::optional<std::uint32_t> GuestImportRegistry::guestThreadCriticalRegionDepth(std::uint32_t token) const {
    RECOMP_REGISTRY_LOCK(lock);
    const auto object = threadObjects_.find(token);
    std::uint32_t id = 0;
    if (object == threadObjects_.end() || !readU32(token+0x14C,id) || id != object->second.threadId) return std::nullopt;
    return object->second.criticalRegionDepth;
}

void GuestImportRegistry::unbindGuestThreadToken() {
    RECOMP_REGISTRY_LOCK(lock);
    boundGuestThreadToken_ = 0;
    clearRegistryCurrentToken(this); // PARALLEL: the calling thread's binding only.
}

bool GuestImportRegistry::bindGuestThreadTlsStorage(
        std::uint32_t token,
        std::uint32_t dynamicSlotBase,
        std::uint32_t slotCount) {
    RECOMP_REGISTRY_LOCK(lock);
    const auto byteCount = static_cast<std::uint64_t>(slotCount) * 4U;
    if (token == 0
            || threadTlsValues_.find(token) == threadTlsValues_.end()
            || dynamicSlotBase == 0
            || (dynamicSlotBase % 4U) != 0
            || slotCount == 0
            || slotCount > TlsSlotCount
            || !contains(dynamicSlotBase, byteCount)) {
        return false;
    }
    const auto existing = threadTlsStorage_.find(token);
    if (existing != threadTlsStorage_.end()) {
        return existing->second.dynamicSlotBase == dynamicSlotBase
                && existing->second.slotCount == slotCount;
    }
    const auto rangeEnd = static_cast<std::uint64_t>(dynamicSlotBase) + byteCount;
    const auto overlaps = [dynamicSlotBase, rangeEnd](
            std::uint32_t base, std::uint64_t size) {
        return dynamicSlotBase < static_cast<std::uint64_t>(base) + size
                && base < rangeEnd;
    };
    if (overlaps(heapBase_, heapSize_)
            || overlapsThreadArena(dynamicSlotBase, byteCount)
            || overlapsThreadTypeIdentity(dynamicSlotBase, byteCount)
            || (physicalMemoryArenaBound_
                && overlaps(physicalArenaBase_, physicalArenaSize_))
            || overlapsThreadTlsStorage(dynamicSlotBase, byteCount)) {
        return false;
    }
    CsAll csAllGuard; // v197
    // v219: range-bounded (cs_range_query.h), same predicate as the scan of every section (one per IRQ worker thread).
    if (anyActiveOverlap(criticalSections_, dynamicSlotBase, byteCount, CriticalSectionSize, [](std::uint32_t,const auto&){return true;})) {
        return false;
    }
    // Slot allocation is process-wide: every bound thread must be able to
    // address every allocated index, regardless of which thread allocated it.
    for (std::size_t index = slotCount; index < TlsSlotCount; ++index) {
        if (tlsSlots_.test(index)) {
            return false;
        }
    }
    const auto& values = threadTlsValues_.at(token);
    RECOMP_V(threadTlsStorage_).emplace(token, ThreadTlsStorage{dynamicSlotBase, slotCount});
    for (std::uint32_t index = 0; index < slotCount; ++index) {
        const auto value = values.find(index);
        (void)writeU32(dynamicSlotBase + index * 4U,
                tlsSlots_.test(index) && value != values.end() ? value->second : 0);
    }
    return true;
}

void GuestImportRegistry::unbindGuestThreadTlsStorage(std::uint32_t token) {
    RECOMP_REGISTRY_LOCK(lock);
    const auto backing = threadTlsStorage_.find(token);
    if (backing == threadTlsStorage_.end()) {
        return;
    }
    auto& values = threadTlsValues_.at(token);
    for (std::uint32_t index = 0; index < backing->second.slotCount; ++index) {
        std::uint32_t value = 0;
        if (tlsSlots_.test(index) && readU32(
                backing->second.dynamicSlotBase + index * 4U, value)) {
            values[index] = value;
        }
    }
    scrubThreadTlsStorage(backing->second);
    RECOMP_V(threadTlsStorage_).erase(backing);
}

bool GuestImportRegistry::teardownGuestThread(std::uint32_t token) {
    RECOMP_REGISTRY_LOCK(lock);
    if (threadObjects_.find(token) != threadObjects_.end()
            || token == 0 || threadTlsValues_.find(token) == threadTlsValues_.end()) {
        return false;
    }
    CsAll csAllGuard; // v197
    for (const auto& [unusedAddress, state] : criticalSections_) {
        (void)unusedAddress;
        if (state.ownerToken == token) {
            return false;
        }
    }
    for (const auto& [unusedHandle, object] : kernelHandles_) {
        (void)unusedHandle;
        if (object.kind == KernelObjectKind::Mutant && object.ownerToken == token) { return false; }
    }
    const auto backing = threadTlsStorage_.find(token);
    if (backing != threadTlsStorage_.end()) {
        scrubThreadTlsStorage(backing->second);
        RECOMP_V(threadTlsStorage_).erase(backing);
    }
    threadTlsValues_.erase(token);
    if (boundGuestThreadToken_ == token) {
        boundGuestThreadToken_ = 0;
    }
    clearRegistryCurrentToken(this, token); // PARALLEL: only if the caller was bound as token.
    return true;
}

std::size_t GuestImportRegistry::tlsAllocationCapacity() const noexcept {
    std::size_t capacity = threadStorage_.ready() ? threadStorageConfig_.tlsSlotCount : TlsSlotCount;
    for (const auto& [unusedToken, storage] : threadTlsStorage_) {
        (void)unusedToken;
        capacity = std::min(capacity, static_cast<std::size_t>(storage.slotCount));
    }
    return capacity;
}

bool GuestImportRegistry::overlapsThreadTlsStorage(
        std::uint32_t address, std::uint64_t size) const noexcept {
    const auto rangeEnd = static_cast<std::uint64_t>(address) + size;
    // v42: every hot import validates guest buffers under R; with ~60 TLS blocks this loop was a
    // node walk per check. The published snapshot answers in O(log n) when it provably equals the
    // live state (see registryCurrentSnapshotLocked); otherwise the loop below, unchanged. A merged
    // range list equals the loop for every query of positive size (for size 0 it differs exactly at
    // the boundary of two touching blocks), so size 0 keeps the loop.
    if (size != 0)
        if (const auto* snapshot = registryCurrentSnapshotLocked(this))
            return registryRangesOverlap(snapshot->tlsRanges, address, rangeEnd);
    for (const auto& [unusedToken, storage] : threadTlsStorage_) {
        (void)unusedToken;
        const auto storageEnd = static_cast<std::uint64_t>(storage.dynamicSlotBase)
                + static_cast<std::uint64_t>(storage.slotCount) * 4U;
        if (address < storageEnd && storage.dynamicSlotBase < rangeEnd) {
            return true;
        }
    }
    return false;
}

void GuestImportRegistry::scrubThreadTlsStorage(
        const ThreadTlsStorage& storage) noexcept {
    const auto byteCount = static_cast<std::uint64_t>(storage.slotCount) * 4U;
    if (contains(storage.dynamicSlotBase, byteCount)) {
        std::memset(addressSpace_->translate(storage.dynamicSlotBase), 0,
                static_cast<std::size_t>(byteCount));
    }
}

void GuestImportRegistry::clearThreadTlsStorage() noexcept {
    for (const auto& [unusedToken, storage] : threadTlsStorage_) {
        (void)unusedToken;
        scrubThreadTlsStorage(storage);
    }
    RECOMP_V(threadTlsStorage_).clear();
}

bool GuestImportRegistry::bindExecutableXexHeader(std::uint32_t address) {
    RECOMP_REGISTRY_LOCK(lock);
    if (address == 0 || addressSpace_ == nullptr || !addressSpace_->isReserved()) {
        return false;
    }
    const auto lookup = GuestXexHeader::lookup(
            *addressSpace_, address, XexHeaderSystemFlags);
    if (lookup.state != GuestXexHeaderLookupState::Found
            && lookup.state != GuestXexHeaderLookupState::NotFound) {
        return false;
    }
    if (lookup.found() && lookup.kind != GuestXexHeaderValueKind::Immediate) {
        return false;
    }
    boundExecutableXexHeader_ = address;
    return true;
}

void GuestImportRegistry::unbindExecutableXexHeader() {
    RECOMP_REGISTRY_LOCK(lock);
    boundExecutableXexHeader_ = 0;
}

bool GuestImportRegistry::bindImagePageProtections(std::uint32_t header, std::uint32_t headerBytes,
                                                 std::uint32_t imageBase, std::uint32_t imageBytes) {
    RECOMP_REGISTRY_LOCK(lock);
    const auto overlap = [](std::uint32_t a, std::uint64_t n, std::uint32_t b, std::uint64_t m) {
        return n && m && a < std::uint64_t{b} + m && b < std::uint64_t{a} + n;
    };
    if (!header || (header & 3) || !contains(header, headerBytes) || imageProtections_.ready() ||
            overlap(header, headerBytes, 0x7FC80000, 0x10000) ||
            overlap(header, headerBytes, imageBase, imageBytes) ||
            overlap(imageBase, imageBytes, heapBase_, heapSize_) ||
            (physicalMemoryArenaBound_ && overlap(imageBase, imageBytes, physicalArenaBase_, physicalArenaSize_)) ||
            overlapsThreadArena(imageBase, imageBytes) || overlapsThreadTypeIdentity(imageBase, imageBytes) ||
            overlapsThreadTlsStorage(imageBase, imageBytes)) return false;
    return imageProtections_.initialize(addressSpace_->translate(header), headerBytes, imageBase, imageBytes);
}

bool GuestImportRegistry::bindPhysicalMemoryArena(
        std::uint32_t arenaBase,
        std::uint32_t arenaSize,
        std::uint32_t physicalAddressOffset) {
    [[maybe_unused]] RegistryWakes wakes; // PARALLEL: fired after the unlock.
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr
            || !addressSpace_->isReserved()
            || arenaBase == 0
            || arenaSize == 0
            || (arenaBase % PageSize) != 0
            || (arenaSize % PageSize) != 0
            || (physicalAddressOffset % PageSize) != 0) {
        return false;
    }
    const auto arenaEnd = static_cast<std::uint64_t>(arenaBase) + arenaSize;
    const auto physicalEnd = static_cast<std::uint64_t>(physicalAddressOffset) + arenaSize;
    const auto heapEnd = static_cast<std::uint64_t>(heapBase_) + heapSize_;
    if (arenaEnd > GuestAddressSpace::Size
            || imageProtections_.overlaps(arenaBase, arenaSize)
            || overlapsThreadArena(arenaBase, arenaSize)
            || overlapsThreadTypeIdentity(arenaBase, arenaSize)
            || physicalEnd > GuestAddressSpace::Size
            || overlapsThreadTlsStorage(arenaBase, arenaSize)
            || (static_cast<std::uint64_t>(arenaBase) < heapEnd
                && static_cast<std::uint64_t>(heapBase_) < arenaEnd)) {
        return false;
    }
    if (physicalMemoryArenaBound_ && !physicalAllocations_.empty()) {
        return arenaBase == physicalArenaBase_
                && arenaSize == physicalArenaSize_
                && physicalAddressOffset == physicalAddressOffset_;
    }
    clearPhysicalMemoryArena(false);
    RECOMP_V(physicalMemoryArenaBound_) = true;
    RECOMP_V(physicalArenaBase_) = arenaBase;
    RECOMP_V(physicalArenaSize_) = arenaSize;
    RECOMP_V(physicalAddressOffset_) = physicalAddressOffset;
    physicalFreeBlocks_.emplace(arenaBase, arenaSize);
    return true;
}

void GuestImportRegistry::unbindPhysicalMemoryArena() {
    [[maybe_unused]] RegistryWakes wakes; // PARALLEL: fired after the unlock.
    RECOMP_REGISTRY_LOCK(lock);
    clearPhysicalMemoryArena(true);
}

bool GuestImportRegistry::overlapsThreadArena(std::uint32_t address, std::uint64_t size) const {
    return threadStorage_.ready() && size != 0
            && address < static_cast<std::uint64_t>(threadStorageConfig_.arena.base)
                    + threadStorageConfig_.arena.size
            && threadStorageConfig_.arena.base < static_cast<std::uint64_t>(address) + size;
}

bool GuestImportRegistry::bindThreadStorageArena(
        const GuestThreadStorageConfig& config,
        const std::vector<GuestThreadMemoryRange>& excludedRanges,
        std::function<bool(std::uint32_t)> exactFunctionValidator) {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || !addressSpace_->isReserved() || threadStorage_.ready()
            || !exactFunctionValidator || config.tlsSlotCount == 0
            || config.tlsSlotCount > TlsSlotCount
            || excludedRanges.size() > GuestThreadStoragePool::MaximumExcludedRanges) {
        return false;
    }
    for (std::size_t index = config.tlsSlotCount; index < TlsSlotCount; ++index) {
        if (tlsSlots_.test(index)) {
            return false;
        }
    }
    auto exclusions = excludedRanges;
    CsAll csAllGuard; // v197
    const auto requiredExclusions = exclusions.size() + 1U
            + (physicalMemoryArenaBound_ ? 1U : 0U) + threadTlsValues_.size()
            + threadTlsStorage_.size() + criticalSections_.size()
            + (threadObjectTypeAddress_ != 0 ? 1U : 0U);
    if (requiredExclusions > GuestThreadStoragePool::MaximumExcludedRanges) {
        return false;
    }
    exclusions.push_back({heapBase_, heapSize_});
    if (threadObjectTypeAddress_ != 0) { exclusions.push_back({threadObjectTypeAddress_, 4}); }
    if (physicalMemoryArenaBound_) {
        exclusions.push_back({physicalArenaBase_, physicalArenaSize_});
    }
    for (const auto& [token, unusedValues] : threadTlsValues_) {
        (void)unusedValues;
        exclusions.push_back({token, 4});
    }
    for (const auto& [token, storage] : threadTlsStorage_) {
        (void)token;
        exclusions.push_back({storage.dynamicSlotBase, storage.slotCount * 4U});
    }
    for (const auto& [address, unusedState] : criticalSections_) {
        (void)unusedState;
        exclusions.push_back({address, CriticalSectionSize});
    }
    if (!RECOMP_V(threadStorage_).initialize(*addressSpace_, config, exclusions)) {
        return false;
    }
    RECOMP_V(threadStorageConfig_) = config;
    exactFunctionValidator_ = std::move(exactFunctionValidator);
    return true;
}

std::optional<GuestPreparedThread> GuestImportRegistry::lookupSuspendedThread(
        std::uint32_t handle) const {
    RECOMP_REGISTRY_LOCK(lock);
    const auto object = kernelHandles_.find(handle);
    if (object == kernelHandles_.end() || object->second.kind != KernelObjectKind::Thread) {
        return std::nullopt;
    }
    const auto thread = threadStorage_.lookup(object->second.threadId);
    if (!thread) { return std::nullopt; }
    const auto state = threadObjects_.find(thread->layout.threadAddress);
    return state != threadObjects_.end() && state->second.suspendCount != 0
            && !state->second.returned ? thread : std::nullopt;
}

std::size_t GuestImportRegistry::createdThreadCount() const {
    RECOMP_REGISTRY_LOCK(lock);
    return threadStorage_.size();
}

bool GuestImportRegistry::overlapsThreadTypeIdentity(std::uint32_t address, std::uint64_t size) const {
    return threadObjectTypeAddress_ != 0 && size != 0
            && address < static_cast<std::uint64_t>(threadObjectTypeAddress_) + 4
            && threadObjectTypeAddress_ < static_cast<std::uint64_t>(address) + size;
}

bool GuestImportRegistry::overlapsProtectedThreadStorage(std::uint32_t address, std::uint64_t size) const {
    if (nativeEventPool_ && address < std::uint64_t{nativeEventPool_} + PageSize
            && nativeEventPool_ < std::uint64_t{address} + size) return true;
    if (nativeXamAddress_ != 0 && address < static_cast<std::uint64_t>(nativeXamAddress_) + NativeModuleRecordSize
            && nativeXamAddress_ < static_cast<std::uint64_t>(address) + size) { return true; }
    if (nativeTitleAddress_ != 0 && address < static_cast<std::uint64_t>(nativeTitleAddress_) + NativeModuleRecordSize
            && nativeTitleAddress_ < static_cast<std::uint64_t>(address) + size) { return true; }
    if (overlapsThreadTypeIdentity(address, size)) { return true; }
    // v42: O(log n). Thread objects are keyed by address with one fixed size, so of all keys below
    // address+size only the largest can reach address (every smaller key ends earlier). Same result
    // as the former loop over every thread object.
    const auto rangeEnd = static_cast<std::uint64_t>(address) + size;
    auto candidate = rangeEnd > UINT32_MAX ? threadObjects_.end()
                                          : threadObjects_.lower_bound(static_cast<std::uint32_t>(rangeEnd));
    if (candidate != threadObjects_.begin()
            && address < static_cast<std::uint64_t>(std::prev(candidate)->first) + GuestThreadEnvironment::ThreadSize) {
        return true;
    }
    if (!overlapsThreadArena(address, size)) { return false; }
    // A resumed worker's own stack is guest-owned output storage, unlike its
    // PCR/TLS/thread record or unused pool space.
    const auto inStack = [&](const auto& layout) {
        return address >= layout.stackBase && static_cast<std::uint64_t>(address) + size
                <= static_cast<std::uint64_t>(layout.stackBase) + layout.stackSize;
    };
    const auto current = threadObjects_.find(RECOMP_CURRENT_GUEST_TOKEN);
    if (current != threadObjects_.end() && !current->second.initial
            && current->second.suspendCount == 0 && !current->second.returned) {
        if (const auto thread = threadStorage_.lookup(current->second.threadId); thread && inStack(thread->layout)) {
            return false;
        }
    }
    // v161: so is the stack of another live, running created thread. On the 360 (and in Xenia, whose stacks are
    // ordinary 64 KiB-page guest allocations with no per-thread protection) any thread may write any thread's stack,
    // and GTA V relies on it: at the end of "Franklin and Lamar" thread 641c8000 hands a 44-byte buffer on its own
    // stack (641d9300) to the file worker 64080000, whose NtReadFile writes it (v160 RENDER_DIAG_READ_FAULT,
    // run app-game-cache-replay-sicdeq16). The whole range must lie inside one stack; PCR/KTHREAD/TLS pages, the
    // stacks of suspended (not yet started) or returned threads, and unallocated pool space stay protected.
    for (const auto& [token, object] : threadObjects_) {
        (void)token;
        if (object.initial || object.suspendCount != 0 || object.returned) { continue; }
        if (const auto thread = threadStorage_.lookup(object.threadId); thread && inStack(thread->layout)) {
            return false;
        }
    }
    return true;
}

bool GuestImportRegistry::bindThreadObjectType(std::uint32_t identityAddress) {
    RECOMP_REGISTRY_LOCK(lock);
    if (threadObjectTypeAddress_ != 0) { return identityAddress == threadObjectTypeAddress_; }
    std::uint32_t identityWord = 0;
    if (identityAddress == 0 || (identityAddress & 3U) != 0
            || !readU32(identityAddress, identityWord) || identityWord != 0
            || overlapsThreadArena(identityAddress, 4)
            || overlapsThreadTlsStorage(identityAddress, 4)) {
        return false;
    }
    const auto overlap = [identityAddress](std::uint32_t base, std::uint64_t size) {
        return identityAddress < static_cast<std::uint64_t>(base) + size
                && base < static_cast<std::uint64_t>(identityAddress) + 4;
    };
    if (overlap(heapBase_, heapSize_)
            || (physicalMemoryArenaBound_ && overlap(physicalArenaBase_, physicalArenaSize_))
            || (threadStorage_.ready() && overlap(threadStorageConfig_.image.base,
                                                  threadStorageConfig_.image.size))) {
        return false;
    }
    for (const auto& [token, unused] : threadObjects_) {
        (void)unused;
        if (overlap(token, GuestThreadEnvironment::ThreadSize)) { return false; }
    }
    CsAll csAllGuard; // v197
    for (const auto& [address, unused] : criticalSections_) {
        (void)unused;
        if (overlap(address, CriticalSectionSize)) { return false; }
    }
    RECOMP_V(threadObjectTypeAddress_) = identityAddress;
    return true;
}

bool GuestImportRegistry::bindInitialThreadObject(std::uint32_t token, std::uint32_t threadId) {
    RECOMP_REGISTRY_LOCK(lock);
    std::uint32_t stagedId = 0;
    if (token == 0 || token != RECOMP_CURRENT_GUEST_TOKEN || threadId != 1 || (token & 3U) != 0
            || !contains(token, GuestThreadEnvironment::ThreadSize)
            || !readU32(token + 0x14CU, stagedId) || stagedId != threadId
            || overlapsThreadArena(token, GuestThreadEnvironment::ThreadSize)
            || overlapsThreadTypeIdentity(token, GuestThreadEnvironment::ThreadSize)) {
        return false;
    }
    const auto existing = threadObjects_.find(token);
    if (existing != threadObjects_.end()) {
        return existing->second.initial && existing->second.threadId == threadId;
    }
    if (std::any_of(threadObjects_.begin(), threadObjects_.end(),
                   [](const auto& pair) { return pair.second.initial; })) {
        return false;
    }
    RECOMP_V(threadObjects_).emplace(token, ThreadObjectState{threadId, 0, 0, true, 0});
    return true;
}

std::uint32_t GuestImportRegistry::threadObjectReferenceCount(std::uint32_t token) const {
    RECOMP_REGISTRY_LOCK(lock);
    const auto object = threadObjects_.find(token);
    return object == threadObjects_.end() ? 0 : object->second.references;
}

GuestImportRegistry::ThreadObjectState* GuestImportRegistry::findThreadObject(std::uint32_t token) {
    return const_cast<ThreadObjectState*>(static_cast<const GuestImportRegistry*>(this)->findThreadObject(token));
}
const GuestImportRegistry::ThreadObjectState* GuestImportRegistry::findThreadObject(std::uint32_t token) const {
    const auto object = threadObjects_.find(token);
    std::uint32_t stagedId = 0;
    if (object == threadObjects_.end() || !readU32(token + 0x14CU, stagedId)
            || stagedId != object->second.threadId) {
        return nullptr;
    }
    return &object->second;
}

GuestImportDispatchResult GuestImportRegistry::dispatchObReferenceObjectByHandle(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__ObReferenceObjectByHandle"};
    if (threadObjectTypeAddress_ == 0) {
        return rejected(Name, "Thread object references require an explicit guest type identity");
    }
    const auto output = call.argument32(2);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4)
            || overlapsProtectedThreadStorage(output, 4)) {
        return memoryFault(Name, "Object output must be a bounded ULONG outside kernel-owned storage");
    }
    for (const auto& [token, unused] : threadObjects_) {
        (void)unused;
        if (output < static_cast<std::uint64_t>(token) + GuestThreadEnvironment::ThreadSize
                && token < static_cast<std::uint64_t>(output) + 4) {
            return rejected(Name, "Object output aliases a thread record");
        }
    }
    const auto handle = call.argument32(0);
    std::uint32_t token = 0;
    if (handle == 0xFFFFFFFEU) {
        token = RECOMP_CURRENT_GUEST_TOKEN; // Explicit current-thread pseudohandle.
    } else {
        const auto found = kernelHandles_.find(handle);
        if (found == kernelHandles_.end()) {
            call.returnValue = StatusInvalidHandle;
            return handled(Name, "Object reference handle is invalid or closed");
        }
        if (found->second.kind != KernelObjectKind::Thread) {
            if (call.argument32(1) == threadObjectTypeAddress_) {
                call.returnValue = StatusObjectTypeMismatch;
                return handled(Name, "Requested thread type does not match the handle");
            }
            return rejected(Name, "Non-thread object pointer exposure is not implemented");
        }
        const auto thread = threadStorage_.lookup(found->second.threadId);
        if (thread) { token = thread->layout.threadAddress; }
        else {
            for (const auto& [at, state] : threadObjects_) {
                if (state.initial && state.threadId == found->second.threadId) { token = at; break; }
            }
        }
    }
    auto* object = findThreadObject(token);
    if (object == nullptr) {
        return rejected(Name, "Thread object identity is unbound or has been corrupted");
    }
    if (call.argument32(1) != 0 && call.argument32(1) != threadObjectTypeAddress_) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(Name, "Requested object type does not match the thread");
    }
    if (object->references == MaximumThreadObjectReferences) {
        call.returnValue = StatusNoMemory;
        return handled(Name, "Bounded thread reference count is exhausted");
    }
    auto result = handled(Name, "Acquired one explicit reference to a live guest thread object");
    ++object->references;
    (void)writeU32(output, token);
    call.returnValue = StatusSuccess;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchObDereferenceObject(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__ObDereferenceObject"};
    const auto event = nativeEventReferences_.find(call.argument32(0));
    if (event != nativeEventReferences_.end()) {
        if (!event->second.references) return rejected(Name, "Unbalanced native event dereference");
        --event->second.references;
        return handled(Name, "Released an owned native event reference");
    }
    auto* object = findThreadObject(call.argument32(0));
    if (object == nullptr || object->references == 0) {
        return rejected(Name, "Unowned or unbalanced thread object dereference");
    }
    auto result = handled(Name, "Released one explicit reference; live-thread ownership is retained");
    --object->references;
    // Void NT-style contract: do not synthesize a return value in the bridge.
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeQueryBasePriorityThread(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeQueryBasePriorityThread"};
    const auto* object = findThreadObject(call.argument32(0));
    if (object == nullptr) { return rejected(Name, "Priority query requires a known live thread object"); }
    call.returnValue = static_cast<std::uint32_t>(object->priorityIncrement);
    return handled(Name, "Returned the tracked relative base-priority increment");
}

std::optional<GuestImportRegistry::ThreadPriorityRequest> GuestImportRegistry::lastThreadPriorityRequest() const {
    RECOMP_REGISTRY_LOCK(lock);
    return lastThreadPriorityRequest_;
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeSetBasePriorityThread(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeSetBasePriorityThread"};
    lastThreadPriorityRequest_.reset();
    auto* object = findThreadObject(call.argument32(0));
    if (object == nullptr) { return rejected(Name, "Priority change requires a known live thread object"); }
    const auto raw = call.argument32(1);
    const auto increment = raw <= INT32_MAX ? static_cast<std::int64_t>(raw)
            : static_cast<std::int64_t>(raw) - (std::int64_t{1} << 32);
    lastThreadPriorityRequest_ = ThreadPriorityRequest{static_cast<std::int32_t>(increment),
            object->priorityIncrement, object->initial, call.argument32(0) == RECOMP_CURRENT_GUEST_TOKEN,
            object->suspendCount != 0, object->returned};
    const auto normalized = normalizePortableThreadPriority(increment,nativeHighPriorityTiersBound_);
    if (!normalized || object->returned || ((increment==3 || increment==4) && nativeHighPriorityTiersRequests_==UINT64_MAX)) {
        return rejected(Name, "Only live normal-range or saturated NT-compatible priorities are implemented");
    }
    auto result = handled(Name, "Updated relative priority under the explicit scheduler binding");
    if (object->suspendCount == 0 && *normalized != object->priorityIncrement
            && (!threadPriorityHandler_ || !threadPriorityHandler_(call.argument32(0),
                    *normalized))) {
        return rejected(Name, "Running thread priority change requires an accepting scheduler binding");
    }
    call.returnValue = static_cast<std::uint32_t>(object->priorityIncrement);
    object->priorityIncrement = *normalized;
    if(increment==3 || increment==4)++nativeHighPriorityTiersRequests_;
    return result;
}

bool GuestImportRegistry::bindThreadResumeHandler(ThreadResumeHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadStorage_.ready() || !handler || threadResumeHandler_) { return false; }
    threadResumeHandler_ = std::move(handler);
    return true;
}

bool GuestImportRegistry::bindThreadExitHandler(ThreadExitHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadResumeHandler_ || !handler || threadExitHandler_) return false;
    threadExitHandler_=std::move(handler); return true;
}

std::optional<std::uint32_t> GuestImportRegistry::guestThreadExitCode(std::uint32_t token) const {
    RECOMP_REGISTRY_LOCK(lock);
    const auto found=threadObjects_.find(token);
    if (found==threadObjects_.end() || !found->second.returned) return std::nullopt;
    return found->second.exitCode;
}

bool GuestImportRegistry::bindThreadPriorityHandler(ThreadPriorityHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadResumeHandler_ || !handler || threadPriorityHandler_) { return false; }
    threadPriorityHandler_ = std::move(handler);
    return true;
}

bool GuestImportRegistry::bindVirtualCpuAffinity() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadStorage_.ready() || !threadResumeHandler_) { return false; }
    virtualCpuAffinityBound_ = true;
    return true;
}

bool GuestImportRegistry::bindNativeHighPriorityTiers() {
    RECOMP_REGISTRY_LOCK(lock);
    if(!threadPriorityHandler_ || nativeHighPriorityTiersBound_)return false;
    nativeHighPriorityTiersBound_=true;return true;
}
std::uint64_t GuestImportRegistry::nativeHighPriorityTiersRequests() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeHighPriorityTiersRequests_;
}

bool GuestImportRegistry::bindThreadDebugNaming() {
    RECOMP_REGISTRY_LOCK(lock);
    if (findThreadObject(RECOMP_CURRENT_GUEST_TOKEN) == nullptr) { return false; }
    threadDebugNamingBound_ = true;
    return true;
}

bool GuestImportRegistry::bindThreadWaitHandler(ThreadWaitHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadStorage_.ready() || !threadResumeHandler_ || !handler || threadWaitHandler_) { return false; }
    threadWaitHandler_ = std::move(handler);
    return true;
}

bool GuestImportRegistry::bindThreadTimedWaitHandler(ThreadTimedWaitHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadWaitHandler_ || !handler || threadTimedWaitHandler_) { return false; }
    threadTimedWaitHandler_ = std::move(handler);
    return true;
}

bool GuestImportRegistry::bindNativeCriticalSectionWaits() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadWaitHandler_ || nativeCriticalSectionWaitsBound_) return false;
    nativeCriticalSectionWaitsBound_=true;return true;
}

bool GuestImportRegistry::bindThreadDelayHandler(ThreadDelayHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!threadTimedWaitHandler_ || !handler || threadDelayHandler_) return false;
    threadDelayHandler_ = std::move(handler);
    return true;
}

std::array<std::uint64_t,7> GuestImportRegistry::threadDelayStatistics() const {
    RECOMP_REGISTRY_LOCK(lock);
    auto statistics = threadDelayStatistics_;
    if (const auto* fast = fastDelayCountersFor(this, false)) { // v168: the lock-free zero-delay repeats
        statistics[0] += fast->calls.load(std::memory_order_relaxed);
        statistics[1] += fast->completed.load(std::memory_order_relaxed);
        statistics[2] += fast->failed.load(std::memory_order_relaxed);
        statistics[3] += fast->zeros.load(std::memory_order_relaxed);
        statistics[6] += fast->inflight.load(std::memory_order_acquire);
    }
    return statistics;
}

std::array<std::uint64_t,4> GuestImportRegistry::nativeCriticalSectionWaitStatistics() const {
    RECOMP_REGISTRY_LOCK(lock);return criticalSectionWaitStatistics_;
}

std::optional<std::string> GuestImportRegistry::guestThreadDebugName(std::uint32_t token) const {
    RECOMP_REGISTRY_LOCK(lock);
    const auto object = threadObjects_.find(token);
    return object == threadObjects_.end() ? std::nullopt : object->second.debugName;
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlRaiseException(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlRaiseException"};
    if (!threadDebugNamingBound_) {
        return rejected(Name, "Debug thread naming requires explicit diagnostic binding");
    }
    const auto record = call.argument32(0);
    std::array<std::uint32_t, 9> words{};
    if (record == 0 || (record & 3U) != 0 || !contains(record, words.size() * 4)) {
        return memoryFault(Name, "Exception record header and four parameters are not bounded");
    }
    for (std::uint32_t i = 0; i < words.size(); ++i) {
        (void)readU32(record + i * 4, words[i]);
    }
    // 32-bit EXCEPTION_RECORD: code, flags, chained record, exception address,
    // parameter count; THREADNAME_INFO: type, guest string, thread ID, flags.
    if (words[0] != 0x406D1388U || words[1] != 0 || words[2] != 0
            || words[4] != 4 || words[5] != 0x1000U || words[8] != 0) {
        return rejected(Name, "Only a continuable unchained four-word debug thread-name notification is handled");
    }
    ThreadObjectState* target = nullptr;
    if (words[7] == 0xFFFFFFFFU) {
        target = findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    } else {
        for (auto& [token, object] : threadObjects_) {
            if (object.threadId == words[7]) { target = findThreadObject(token); break; }
        }
    }
    if (target == nullptr || target->returned) {
        return rejected(Name, "Debug naming target is not a registered live thread ID");
    }
    std::string name;
    if (!readAnsiString(words[6], 256, name)) {
        return memoryFault(Name, "Debug thread name is not a terminated string of at most 255 bytes");
    }
    auto success = handled(Name, "Recorded a bounded debug thread name without logging guest text");
    target->debugName = std::move(name);
    return success; // Void ABI: preserve r3. All other exception codes remain stops.
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeSetAffinityThread(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeSetAffinityThread"};
    if (!virtualCpuAffinityBound_) {
        return rejected(Name, "Virtual CPU affinity requires explicit serialized-executor policy");
    }
    const auto token = call.argument32(0), mask = call.argument32(1), output = call.argument32(2);
    if (mask == 0 || (mask & ~0x3FU) != 0) {
        call.returnValue = StatusInvalidParameter;
        return handled(Name, "Affinity mask is outside the six virtual CPU domain");
    }
    if ((mask & (mask - 1)) != 0) {
        return rejected(Name, "Multi-CPU affinity scheduling is not implemented");
    }
    auto* object = findThreadObject(token);
    if (object == nullptr || object->initial || object->returned) {
        return rejected(Name, "Affinity requires a live created thread with owned PCR storage");
    }
    const auto thread = threadStorage_.lookup(object->threadId);
    if (!thread || (output != 0 && ((output & 3U) != 0 || !contains(output, 4)
            || overlapsProtectedThreadStorage(output, 4)))) {
        return memoryFault(Name, "Affinity output or thread storage is invalid");
    }
    // Xenia's inspected Xbox layout: KPCR byte +0x10C and KTHREAD byte +0xBF.
    // Singleton masks make the previous mask unambiguous. No emulator sentinel
    // or host processor number is exposed, and no physical affinity is applied.
    auto* pcrCpu = addressSpace_->translate(thread->layout.pcrAddress + 0x10CU);
    auto* threadCpu = addressSpace_->translate(token + 0xBFU);
    if (*pcrCpu >= 6 || *threadCpu != *pcrCpu || (1U << *pcrCpu) != object->virtualCpuMask) {
        return rejected(Name, "Guest CPU identity bytes disagree with registered state");
    }
    std::uint8_t cpu = 0;
    while ((1U << cpu) != mask) { ++cpu; }
    auto success = handled(Name, "Updated singleton virtual CPU identity; host execution remains serialized");
    const auto previous = object->virtualCpuMask;
    *pcrCpu = *threadCpu = cpu;
    object->virtualCpuMask = mask;
    if (output != 0) { (void)writeU32(output, previous); }
    call.returnValue = StatusSuccess;
    return success;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtResumeThread(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtResumeThread"};
    const auto output = call.argument32(1);
    if (output != 0 && ((output & 3U) != 0 || !contains(output, 4)
            || overlapsProtectedThreadStorage(output, 4))) {
        return memoryFault(Name, "Suspend-count output is not a bounded external ULONG");
    }
    const auto handle = kernelHandles_.find(call.argument32(0));
    if (handle == kernelHandles_.end()) {
        call.returnValue = StatusInvalidHandle;
        return handled(Name, "Resume handle is invalid or closed");
    }
    if (handle->second.kind != KernelObjectKind::Thread) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(Name, "Resume handle is not a thread");
    }
    const auto thread = threadStorage_.lookup(handle->second.threadId);
    auto* object = thread ? findThreadObject(thread->layout.threadAddress) : nullptr;
    if (object == nullptr || object->returned) {
        return rejected(Name, "Cannot resume an unknown or returned thread");
    }
    if (object->suspendCount != 0 && !threadResumeHandler_) {
        return rejected(Name, "No real guest execution handler is bound for resume");
    }
    auto success = handled(Name, "Resumed through the bound executor or reported an already running thread");
    auto failure = handled(Name, "The executor could not prepare a worker");
    const auto previous = object->suspendCount;
    if (previous != 0 && !threadResumeHandler_(*thread, object->priorityIncrement)) {
        call.returnValue = StatusNoMemory;
        return failure;
    }
    object->suspendCount = 0;
    if (output != 0) { (void)writeU32(output, previous); }
    call.returnValue = StatusSuccess;
    if (previous != 0) wakeAllIfRegistryWaits(); // PARALLEL: a new producer; waiter set unknown.
    return success;
}

bool GuestImportRegistry::markGuestThreadReturned(std::uint32_t token, std::uint32_t exitCode) {
    [[maybe_unused]] RegistryWakes wakes; // PARALLEL: fired after the unlock.
    RECOMP_REGISTRY_LOCK(lock);
    auto* object = findThreadObject(token);
    if (token != RECOMP_CURRENT_GUEST_TOKEN || object == nullptr
            || object->returned || object->suspendCount != 0) {
        return false;
    }
    object->returned = true;
    object->exitCode = exitCode;
    bool signaled = false; // v217: a thread handle or an abandoned mutant changed state
    for (auto& [handle, kernelObject] : kernelHandles_) {
        (void)handle;
        if (kernelObject.kind == KernelObjectKind::Thread && kernelObject.threadId == object->threadId) {
            kernelObject.signalCount = 1;signaled = true;
        }
        if (kernelObject.kind == KernelObjectKind::Mutant && kernelObject.ownerToken == token) {
            kernelObject.ownerToken = 0;
            kernelObject.recursion = 0;
            kernelObject.signalCount = 1;
            kernelObject.abandoned = true;signaled = true;
        }
    }
    // PARALLEL: thread handles and abandoned mutants are signaled, and waits may
    // depend on this thread's liveness: broadcast (rare) while a wait may be parked.
    // v217: a thread no handle names and that owned no mutant (the per-IRQ scene/command interrupt workers, ~46/s:
    // each broadcast woke every parked guest wait to re-check under R as the world resumed) changes no handle a
    // registered waiter watches; the global epoch still moves (v198), and every parked wait re-polls on its own
    // backoff, so a dependent the registry cannot name is late by at most one poll step, never lost.
    if (signaled || recomp_v217_baseline.load(std::memory_order_relaxed)) wakeAllIfRegistryWaits();
    else { globalHandleEpoch.fetch_add(1, std::memory_order_acq_rel); v217QuietReturns.fetch_add(1, std::memory_order_relaxed); }
    // Retain referenced objects and owned storage until reset. APC submission
    // is not implemented, so no fabricated APC rundown callbacks are invoked.
    return true;
}

std::optional<GuestPreparedThread> GuestImportRegistry::createNativeCallbackWorker(
        std::uint32_t callback,std::uint32_t context,const NativeCallbackEnqueue& enqueue) {
    RECOMP_REGISTRY_LOCK(lock);
    return createNativeCallbackWorkerLocked(callback,context,enqueue);
}
std::optional<GuestPreparedThread> GuestImportRegistry::createNativeCallbackWorkerLocked(
        std::uint32_t callback,std::uint32_t context,const NativeCallbackEnqueue& enqueue) {
    return createNativeWorkerLocked(callback,context,true,0,enqueue);
}
std::optional<GuestPreparedThread> GuestImportRegistry::createNativeWorkerLocked(
        std::uint32_t callback,std::uint32_t context,bool wrapped,std::uint32_t cpu,const NativeCallbackEnqueue& enqueue) {
    if (!threadStorage_.ready() || !exactFunctionValidator_ || !RECOMP_CURRENT_GUEST_TOKEN ||
        !enqueue || cpu>=6 || !exactFunctionValidator_(callback)) return std::nullopt;
    const auto prepared=RECOMP_V(threadStorage_).prepare({0,0,callback,context,0,wrapped});
    if (!prepared.succeeded()) return std::nullopt;
    const auto thread=*prepared.thread;const auto token=thread.layout.threadAddress;
    { // v160 DIAGNOSTIC: native worker placement (first 32, then powers of two).
        static unsigned created=0;++created;
        if(created<=32 || !(created&(created-1)))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
            "RENDER_DIAG_THREAD_LAYOUT kind=worker token=%08x id=%u stack=%08x+%x alloc=%08x+%x start=%08x ctx=%08x wrapped=%d creator=%08x n=%u",
            token,thread.layout.threadId,thread.layout.stackBase,thread.layout.stackSize,thread.allocation.base,
            thread.allocation.size,callback,context,wrapped?1:0,RECOMP_CURRENT_GUEST_TOKEN,created);
    }
    if (threadTlsValues_.count(token) || threadTlsStorage_.count(token) ||
        createdThreads_.count(token) || threadObjects_.count(token)) {
        (void)RECOMP_V(threadStorage_).release(thread.layout.threadId);return std::nullopt;
    }
    const auto rollback=[&] {
        threadTlsValues_.erase(token);RECOMP_V(threadTlsStorage_).erase(token);
        createdThreads_.erase(token);RECOMP_V(threadObjects_).erase(token);
        (void)RECOMP_V(threadStorage_).release(thread.layout.threadId);
    };
    try {
        threadTlsValues_.try_emplace(token);
        RECOMP_V(threadTlsStorage_).emplace(token,ThreadTlsStorage{thread.dynamicTlsAddress,thread.layout.tlsSlotCount});
        createdThreads_.emplace(token,thread.layout.threadId);
        RECOMP_V(threadObjects_).emplace(token,ThreadObjectState{thread.layout.threadId});
        // Newly prepared PCR/KTHREAD storage is zeroed and privately owned.
        // Preserve the same singleton identity contract as KeSetAffinityThread.
        *addressSpace_->translate(thread.layout.pcrAddress+0x10C)=static_cast<std::uint8_t>(cpu);
        *addressSpace_->translate(token+0xBF)=static_cast<std::uint8_t>(cpu);
        findThreadObject(token)->virtualCpuMask=1U<<cpu;
        if (!enqueue(thread)) {rollback();return std::nullopt;}
    } catch (const std::bad_alloc&) {rollback();return std::nullopt;}
      catch (...) {rollback();throw;}
    // No allocation or guest execution after successful enqueue. The caller
    // publishes its service handle only after this complete operation succeeds.
    findThreadObject(token)->suspendCount=0;
    return thread;
}

GuestImportDispatchResult GuestImportRegistry::dispatchExCreateThread(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__ExCreateThread"};
    call.returnValue = StatusInvalidParameter;
    if (!threadStorage_.ready() || !exactFunctionValidator_ || RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "Thread creation requires explicit storage and current-thread binding");
    }
    const auto flags = call.argument32(6);
    const auto cpuMask = flags >> 24U;
    const bool suspended = (flags & 1U) != 0;
    if (!GuestThreadStoragePool::validCreationFlags(flags) || call.argument32(0) == 0
            || (!suspended && !threadResumeHandler_)) {
        return rejected(Name, "Only suspended/runnable singleton-CPU flags with opaque handle output are supported; runnable creation requires a bound executor");
    }
    if (cpuMask != 0 && !virtualCpuAffinityBound_) {
        return rejected(Name, "Creation affinity requires explicit serialized-executor policy");
    }
    const auto outputHandle = call.argument32(0);
    const auto outputId = call.argument32(2);
    const auto validOutput = [&](std::uint32_t address) {
        return (address & 3U) == 0 && contains(address, 4)
                && !overlapsProtectedThreadStorage(address, 4);
    };
    if (!validOutput(outputHandle) || (outputId != 0 && !validOutput(outputId))) {
        return memoryFault(Name, "Thread outputs must be bounded ULONGs outside owned thread storage");
    }
    if (outputHandle == outputId) {
        return rejected(Name, "Aliased thread handle and ID outputs are unsupported");
    }
    const auto xapi = call.argument32(3);
    const auto start = call.argument32(4);
    if (!exactFunctionValidator_(start) || (xapi != 0 && !exactFunctionValidator_(xapi))) {
        return rejected(Name, "Thread entry must resolve to an exact translated function");
    }
    // Preallocate all diagnostic strings before changing guest state. Publishing
    // the outputs below is the final step after every fallible host insertion.
    auto success = handled(Name, suspended ? "Created a suspended guest thread; no routine has run"
            : "Created and enqueued a guest thread; execution begins only at a scheduler handoff");
    auto noMemory = handled(Name, "Bounded thread or handle resources are exhausted");
    if (kernelHandles_.size() >= MaximumKernelHandles || nextKernelHandle_ > 0x7FFFFFFCU) {
        call.returnValue = StatusNoMemory;
        return noMemory;
    }
    const auto prepared = RECOMP_V(threadStorage_).prepare({call.argument32(1), xapi, start,
            call.argument32(5), call.argument32(6)});
    if (!prepared.succeeded()) {
        if (prepared.state == GuestThreadStorageState::InvalidRequest) {
            return handled(Name, "Thread stack or entry parameters exceed the supported contract");
        }
        call.returnValue = StatusNoMemory;
        return noMemory;
    }
    const auto& thread = *prepared.thread;
    const auto token = thread.layout.threadAddress;
    // Rebinding tokens without storage is permitted by the synthetic registry
    // API; a conflicting identity must never overwrite that caller's state.
    if (threadTlsValues_.find(token) != threadTlsValues_.end()
            || threadTlsStorage_.find(token) != threadTlsStorage_.end()) {
        (void)RECOMP_V(threadStorage_).release(thread.layout.threadId);
        return rejected(Name, "Prepared thread token conflicts with an existing identity");
    }
    std::uint32_t handle = 0;
    try {
        threadTlsValues_.try_emplace(token);
        RECOMP_V(threadTlsStorage_).emplace(token,
                ThreadTlsStorage{thread.dynamicTlsAddress, thread.layout.tlsSlotCount});
        createdThreads_.emplace(token, thread.layout.threadId);
        RECOMP_V(threadObjects_).emplace(token, ThreadObjectState{thread.layout.threadId});
        findThreadObject(token)->virtualCpuMask = cpuMask != 0 ? cpuMask : 1U;
        handle = createKernelHandle({KernelObjectKind::Thread, 0, 0, thread.layout.threadId});
    } catch (const std::bad_alloc&) {
        threadTlsValues_.erase(token);
        RECOMP_V(threadTlsStorage_).erase(token);
        createdThreads_.erase(token);
        RECOMP_V(threadObjects_).erase(token);
        (void)RECOMP_V(threadStorage_).release(thread.layout.threadId);
        call.returnValue = StatusNoMemory;
        return noMemory;
    }
    if (handle == 0) {
        threadTlsValues_.erase(token);
        RECOMP_V(threadTlsStorage_).erase(token);
        createdThreads_.erase(token);
        RECOMP_V(threadObjects_).erase(token);
        (void)RECOMP_V(threadStorage_).release(thread.layout.threadId);
        call.returnValue = StatusNoMemory;
        return noMemory;
    }
    if (!suspended) {
        // Last fallible operation before publishing guest outputs. The enqueue
        // contract forbids inline execution and leaves no worker on failure.
        const auto rollback = [&] {
            ::recomp::imports_fast::eraseKey(kernelHandles_, handle, ::recomp::imports_fast::handleGenerations()); threadTlsValues_.erase(token); RECOMP_V(threadTlsStorage_).erase(token);
            createdThreads_.erase(token); RECOMP_V(threadObjects_).erase(token);
            (void)RECOMP_V(threadStorage_).release(thread.layout.threadId);
        };
        bool enqueued = false;
        try { enqueued = threadResumeHandler_(thread, 0); }
        catch (const std::bad_alloc&) { rollback(); call.returnValue = StatusNoMemory; return noMemory; }
        catch (...) { rollback(); throw; }
        if (!enqueued) { rollback(); call.returnValue = StatusNoMemory; return noMemory; }
        findThreadObject(token)->suspendCount = 0;
        wakeAllIfRegistryWaits(); // PARALLEL: a new producer; waiter set unknown.
    }
    // v160 DIAGNOSTIC: every title thread's placement, requested stack and creator chain.
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_THREAD_LAYOUT kind=title token=%08x id=%u requested=%x stack=%08x+%x alloc=%08x+%x start=%08x xapi=%08x "
            "ctx=%08x flags=%08x suspended=%d creator=%08x creator_sp=%08x frames=%s",
            token, thread.layout.threadId, call.argument32(1), thread.layout.stackBase, thread.layout.stackSize,
            thread.allocation.base, thread.allocation.size, start, xapi, call.argument32(5), call.argument32(6),
            suspended ? 1 : 0, RECOMP_CURRENT_GUEST_TOKEN, call.stackPointer, [&] {
                std::string out; char s[16]; std::uint32_t sp = call.stackPointer;
                for (int i = 0; i < 8; ++i) {
                    std::uint32_t back = 0, lr = 0;
                    if (!sp || !contains(sp, 4) || !readU32(sp, back) || back <= sp || !contains(back - 8, 4)
                            || !readU32(back - 8, lr)) break;
                    std::snprintf(s, sizeof s, "%s%08x", out.empty() ? "" : ",", lr); out += s; sp = back;
                }
                return out;
            }().c_str());
    (void)writeU32(outputHandle, handle);
    if (outputId != 0) {
        (void)writeU32(outputId, thread.layout.threadId);
    }
    call.returnValue = StatusSuccess;
    return success;
}

bool GuestImportRegistry::isReady() const {
    RECOMP_REGISTRY_LOCK(lock);
    return addressSpace_ != nullptr && addressSpace_->isReserved();
}

bool GuestImportRegistry::bindFileMetadataQuery(FileMetadataQuery query) {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || !query || fileMetadataQuery_) { return false; }
    fileMetadataQuery_ = std::move(query);
    return true;
}

std::optional<GuestImportRegistry::FileCreateRequest> GuestImportRegistry::lastFileCreateRequest() const {
    RECOMP_REGISTRY_LOCK(lock);
    return lastFileCreateRequest_;
}

bool GuestImportRegistry::bindReadOnlyFiles(ReadOnlyFileOpen open) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!fileMetadataQuery_ || !open || readOnlyFileOpen_) { return false; }
    readOnlyFileOpen_ = std::move(open);
    return true;
}

GuestImportRegistry::FileIoStatistics GuestImportRegistry::fileIoStatistics() const {
    RECOMP_REGISTRY_LOCK(lock);
    return fileIoStatistics_;
}

bool GuestImportRegistry::bindVirtualVideoMode720p60() {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || virtualVideoModeBound_) { return false; }
    virtualVideoModeBound_ = true;
    return true;
}
bool GuestImportRegistry::bindExperimentalNativeScalerTiming(std::uint32_t slot,std::uint32_t storage) {
    RECOMP_REGISTRY_LOCK(lock);
    std::uint32_t pointer=0,clock=0;
    if(!virtualVideoModeBound_ || !nativeSwapConsumer_ || graphicsInitialized_ || nativeScalerTimingBinding_ ||
        !slot || !storage || (slot|storage)&3 || slot<graphicsImageBase_ ||
        std::uint64_t{slot}+4>std::uint64_t{graphicsImageBase_}+graphicsImageBytes_ || slot==storage ||
        (storage<std::uint64_t{graphicsImageBase_}+graphicsImageBytes_ && graphicsImageBase_<std::uint64_t{storage}+4) ||
        !validGuestDataBuffer(storage,4,false) || !readU32(slot,pointer) || pointer!=storage ||
        !readU32(storage,clock) || clock!=recomp::gpu::NativeScalerTiming::NominalGpuMHz)return false;
    nativeScalerTimingBinding_=std::array<std::uint32_t,2>{slot,storage};return true;
}
std::uint64_t GuestImportRegistry::nativeScalerTimingQueries() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeScalerTimingQueries_;
}
bool GuestImportRegistry::bindExperimentalNativeIdentityScaler() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!nativeScalerTimingBinding_ || !nativeSwapConsumer_ || graphicsInitialized_ || nativeIdentityScalerBound_) return false;
    nativeIdentityScalerBound_=true;return true;
}
GuestImportRegistry::NativeScalerCommandStatus GuestImportRegistry::nativeScalerCommandStatus() const {
    RECOMP_REGISTRY_LOCK(lock);
    if (!pendingNativeScaler_) return {};
    const auto& identity=pendingNativeScaler_->identity();
    return {true,identity.previousReadWord,recomp::gpu::NativeScalerCommand::Words,identity.issuerThread};
}

bool GuestImportRegistry::bindDpcInitialization(std::function<bool(std::uint32_t)> callbackValidator) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !callbackValidator || dpcCallbackValidator_) return false;
    dpcCallbackValidator_ = std::move(callbackValidator);
    return true;
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeInitializeDpc(GuestImportCall& call) {
    constexpr auto Name = "__imp__KeInitializeDpc";
    constexpr std::uint32_t Bytes = 28;
    const auto address = call.argument32(0), routine = call.argument32(1);
    if (!dpcCallbackValidator_ || !routine || (routine & 3U) || !dpcCallbackValidator_(routine))
        return rejected(Name, "DPC initialization requires an exact validated guest routine");
    if (!address || (address & 3U) || !contains(address, Bytes) ||
        (address < 0x7FC90000 && std::uint64_t{address} + Bytes > 0x7FC80000) ||
        overlapsProtectedThreadStorage(address, Bytes) || overlapsThreadTlsStorage(address, Bytes))
        return memoryFault(Name, "DPC record is outside supported ordinary guest storage");
    bool owned = false;
    const auto after = allocations_.upper_bound(address);
    if (after != allocations_.begin()) {
        const auto& a = *std::prev(after);
        owned = std::uint64_t{address} + Bytes <= std::uint64_t{a.first} + a.second;
    }
    const auto physical = physicalAllocations_.upper_bound(address);
    if (!owned && physical != physicalAllocations_.begin()) {
        const auto& a = *std::prev(physical);
        owned = (a.second.protection & PageReadWrite) &&
            std::uint64_t{address} + Bytes <= std::uint64_t{a.first} + a.second.size;
    }
    if (!owned) return rejected(Name, "DPC record must fit an owned writable allocation");
    // No DPC queue exists yet; this is only the public guest record initializer.
    // 28 bytes: type/importance/number, two links, routine, context, two args.
    // All output has been validated before the first write. Void ABI preserves r3.
    const std::array<std::uint32_t, 7> words{0x13000000, 0, 0, routine, call.argument32(2), 0, 0};
    for (std::uint32_t i = 0; i < words.size(); ++i) (void)writeU32(address + i * 4, words[i]);
    return handled(Name, "Initialized an owned big-endian DPC record; no callback queued or dispatched");
}

GuestImportDispatchResult GuestImportRegistry::dispatchGraphicsDisplayNotification(GuestImportCall& call) {
    constexpr auto Name = "__imp__VdCallGraphicsNotificationRoutines";
    if (call.argument32(0) != 1 || !virtualVideoModeBound_ || !graphicsInitialized_ ||
        !graphicsBackend_ || !graphicsBackend_->ready())
        return rejected(Name, "Only event-1 native display configuration with a live software display profile is supported");
    const auto address = call.argument32(1);
    // Observed sub_836BF490: 240-byte frame, complete 64-byte payload at +144.
    // This is larger than the eight geometry bytes described by the old stub.
    if (!call.stackPointer || (call.stackPointer & 15U) ||
        std::uint64_t{call.stackPointer} + 144 != address || !contains(address, 64) ||
        (address < 0x7FC90000 && std::uint64_t{address} + 64 > 0x7FC80000) ||
        overlapsProtectedThreadStorage(address, 64) || overlapsThreadTlsStorage(address, 64))
        return memoryFault(Name, "Display notification does not fit the observed bounded stack payload");
    NativeDisplayConfiguration configuration;
    for (unsigned i = 0; i < 4; ++i) (void)readU16(address + i * 2, configuration.dimensions[i]);
    for (unsigned i = 0; i < 14; ++i) (void)readU32(address + 8 + i * 4, configuration.scaler[i]);
    if (!validNativeDisplayConfiguration(configuration) || configuration.dimensions[2] != 1280 ||
        configuration.dimensions[3] != 720)
        return rejected(Name, "Display geometry/filter metadata is inconsistent with the native 720p profile");
    auto result = handled(Name, "Native backend accepted display metadata; no GPU work or system callbacks executed");
    if (!RECOMP_V_BACKEND()->configureDisplay(configuration))
        return rejected(Name, "Native display-configuration consumer rejected the notification");
    // Observed callers ignore r3. Preserve the native void profile, without
    // claiming the undocumented original routine's return-value semantics.
    return result;
}

bool GuestImportRegistry::bindGraphicsBackend(std::shared_ptr<GuestGraphicsBackend> backend,
        std::uint32_t imageBase, std::uint32_t imageBytes,
        std::function<bool(std::uint32_t)> callbackValidator) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || graphicsBackend_ || !backend || backend->ready() || !callbackValidator ||
        !imageBase || (imageBase & 3U) || imageBytes < 4 || !contains(imageBase, imageBytes)) return false;
    RECOMP_V(graphicsBackend_) = std::move(backend);
    graphicsCallbackValidator_ = std::move(callbackValidator);
    graphicsImageBase_ = imageBase; graphicsImageBytes_ = imageBytes;
    return true;
}

bool GuestImportRegistry::graphicsInitialized() const {
    RECOMP_REGISTRY_LOCK(lock);
    return graphicsInitialized_ && graphicsBackend_ && graphicsBackend_->ready();
}

std::optional<GuestImportRegistry::GraphicsInterruptRegistration>
GuestImportRegistry::graphicsInterruptRegistration() const {
    RECOMP_REGISTRY_LOCK(lock);
    return graphicsInterruptRegistration_;
}

std::optional<GuestImportRegistry::GraphicsRingConfiguration>
GuestImportRegistry::graphicsRingConfiguration() const {
    RECOMP_REGISTRY_LOCK(lock);
    return graphicsRingConfiguration_;
}

std::optional<std::uint32_t> GuestImportRegistry::graphicsSystemIdentifierAddress() const {
    RECOMP_REGISTRY_LOCK(lock);
    return graphicsSystemIdentifier_;
}

std::optional<std::vector<std::uint8_t>> GuestImportRegistry::snapshotStoppedGraphicsRing() const {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
        !graphicsRingConfiguration_ || !graphicsRingAllocation_) return std::nullopt;
    const auto& ring = *graphicsRingConfiguration_;
    if (!ring.bytes || ring.bytes > 0x1000000) return std::nullopt;
    const auto range = resolvePhysicalRange(ring.physicalAddress, ring.bytes, false);
    if (!range || range->virtualAddress != ring.virtualAddress || range->allocationBase != *graphicsRingAllocation_)
        return std::nullopt;
    try {
        const auto* bytes = addressSpace_->translate(ring.virtualAddress);
        return std::vector<std::uint8_t>(bytes, bytes + ring.bytes);
    } catch (const std::bad_alloc&) { return std::nullopt; }
}

std::optional<std::vector<std::uint8_t>> GuestImportRegistry::snapshotStoppedGraphicsPhysical(
        std::uint32_t physicalAddress, std::uint32_t byteCount) const {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
        !byteCount || byteCount > 0x400000 || ((physicalAddress | byteCount) & 3)) return std::nullopt;
    const auto range = resolvePhysicalRange(physicalAddress, byteCount, false);
    if (!range) return std::nullopt;
    try {
        const auto* bytes = addressSpace_->translate(range->virtualAddress);
        return std::vector<std::uint8_t>(bytes, bytes + byteCount);
    } catch (const std::bad_alloc&) { return std::nullopt; }
}

std::optional<std::vector<std::uint8_t>> GuestImportRegistry::snapshotStoppedGuestHeap(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const {
    return copySerializedGuestHeap(virtualAddress,byteCount);
}
std::optional<std::vector<std::uint8_t>> GuestImportRegistry::copySerializedGuestHeap(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const {
    // v42: validated against the published snapshot without R when it is current; else under R.
    const auto* bytes = fastValidatedView(virtualAddress, byteCount, false);
    if (!bytes && registryFastHeapBoundsRejected()) return std::nullopt; // v152: the locked test below would refuse it
    if (!bytes) {
        RECOMP_REGISTRY_READER(lock);
        if (!addressSpace_ || !byteCount || byteCount > 65536 || virtualAddress < heapBase_ ||
            std::uint64_t{virtualAddress} + byteCount > std::uint64_t{heapBase_} + heapSize_ ||
            !validGuestDataBuffer(virtualAddress, byteCount, false)) return std::nullopt;
        bytes = addressSpace_->translate(virtualAddress);
        lock.unlock(); // v33: validated; the mapping is permanent and R never guarded the bytes themselves.
    }
    try {
        return std::vector<std::uint8_t>(bytes, bytes + byteCount);
    } catch (const std::bad_alloc&) { return std::nullopt; }
}
// Attempt v32: the same validation as copySerializedGuestHeap, returning the guest bytes in place
// (the guest mappings are permanent; contents may change like any guest memory read).
const std::uint8_t* GuestImportRegistry::viewSerializedGuestHeap(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const {
    if (const auto* bytes = fastValidatedView(virtualAddress, byteCount, false)) return bytes; // v42
    if (registryFastHeapBoundsRejected()) return nullptr; // v152: the locked test below would refuse it
    RECOMP_REGISTRY_READER(lock);
    if (!addressSpace_ || !byteCount || byteCount > 65536 || virtualAddress < heapBase_ ||
        std::uint64_t{virtualAddress} + byteCount > std::uint64_t{heapBase_} + heapSize_ ||
        !validGuestDataBuffer(virtualAddress, byteCount, false)) return nullptr;
    return addressSpace_->translate(virtualAddress);
}

std::optional<std::vector<std::uint8_t>> GuestImportRegistry::snapshotStoppedGraphicsVirtual(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const {
    if(virtualAddress<0x20000000)return std::nullopt;
    return copySerializedGraphicsData(virtualAddress,byteCount);
}
std::optional<std::vector<std::uint8_t>> GuestImportRegistry::copySerializedGuestArgument(
        std::uint32_t address,std::uint32_t bytes) const {
    if(address>=heapBase_ && std::uint64_t{address}<std::uint64_t{heapBase_}+heapSize_)
        return copySerializedGuestHeap(address,bytes);
    if(!v252Baseline()) { // v252a
        const auto& c=argStackCache;const auto token=RECOMP_CURRENT_GUEST_TOKEN;
        if(token && c.registry==this && c.token==token && bytes && bytes<=65536 && !(address&3) && addressSpace_ &&
           address>=c.stackBase && std::uint64_t{address}+bytes<=c.stackEnd)
            if(const auto* slot=registryFindSlot(this)) {
                const auto word=slot->word.load(std::memory_order_acquire);
                if((word&RegistryWindowMask)==0 && (word>>16)==c.generation) {
                    try {
                        const auto* source=addressSpace_->translate(address);
                        auto copy=std::vector<std::uint8_t>(source,source+bytes);
                        v252ArgFast.fetch_add(1,std::memory_order_relaxed);
                        return copy;
                    } catch(const std::bad_alloc&) {return std::nullopt;}
                }
            }
    }
    RECOMP_REGISTRY_LOCK(lock);
    if(!addressSpace_ || !bytes || bytes>65536 || !validGuestDataBuffer(address,bytes,false))return std::nullopt;
    const auto current=threadObjects_.find(RECOMP_CURRENT_GUEST_TOKEN);
    if(current==threadObjects_.end() || current->second.initial || current->second.returned ||
        current->second.suspendCount)return std::nullopt;
    const auto owner=threadStorage_.lookup(current->second.threadId);
    if(!owner || address<owner->layout.stackBase || std::uint64_t{address}+bytes>
        std::uint64_t{owner->layout.stackBase}+owner->layout.stackSize)return std::nullopt;
    if(!v252Baseline()) { // v252a: the whole stack validated at this generation (R held: no window can open now)
        const std::uint64_t stackEnd=std::uint64_t{owner->layout.stackBase}+owner->layout.stackSize;
        const bool outsideHeap=stackEnd<=heapBase_ || owner->layout.stackBase>=std::uint64_t{heapBase_}+heapSize_;
        if(outsideHeap && owner->layout.stackSize && validGuestDataBuffer(owner->layout.stackBase,owner->layout.stackSize,false))
            if(const auto* slot=registryFindSlot(this)) {
                const auto word=slot->word.load(std::memory_order_acquire);
                if((word&RegistryWindowMask)==0)
                    argStackCache={this,RECOMP_CURRENT_GUEST_TOKEN,word>>16,owner->layout.stackBase,stackEnd};
            }
    }
    try {
        const auto* source=addressSpace_->translate(address);
        return std::vector<std::uint8_t>(source,source+bytes);
    } catch(const std::bad_alloc&) {return std::nullopt;}
}
std::optional<std::vector<std::uint8_t>> GuestImportRegistry::copySerializedTitleImage(
        std::uint32_t address, std::uint32_t byteCount) const {
    RECOMP_REGISTRY_LOCK(lock);
    if(!addressSpace_ || !imageProtections_.ready() || !byteCount || byteCount>65536 ||
       (address&3) || address<imageProtections_.base() ||
       std::uint64_t{address}+byteCount>std::uint64_t{imageProtections_.base()}+imageProtections_.size() ||
       !validGuestDataBuffer(address,byteCount,false))return std::nullopt;
    try {
        const auto* bytes=addressSpace_->translate(address);
        return std::vector<std::uint8_t>(bytes,bytes+byteCount);
    } catch(const std::bad_alloc&) {return std::nullopt;}
}
std::optional<std::vector<std::uint8_t>> GuestImportRegistry::copySerializedGraphicsData(
        std::uint32_t address, std::uint32_t byteCount) const {
    const auto* bytes=fastValidatedView(address,byteCount,true); // v42: snapshot, else under R
    if(!bytes) {
        RECOMP_REGISTRY_READER(lock);
        bytes=viewSerializedGraphicsDataLocked(address,byteCount);
        if(!bytes)return std::nullopt;
        lock.unlock(); // v33: validated; copy (up to MBs of vertex/index data) without holding R.
    }
    try {
        return std::vector<std::uint8_t>(bytes,bytes+byteCount);
    } catch (const std::bad_alloc&) { return std::nullopt; }
}
// Attempt v32: the same validation as copySerializedGraphicsData, returning the guest bytes in place.
const std::uint8_t* GuestImportRegistry::viewSerializedGraphicsData(
        std::uint32_t address, std::uint32_t byteCount) const {
    if (const auto* bytes = fastValidatedView(address, byteCount, true)) return bytes; // v42
    RECOMP_REGISTRY_READER(lock);
    return viewSerializedGraphicsDataLocked(address,byteCount);
}
const std::uint8_t* GuestImportRegistry::viewSerializedGraphicsDataLocked(
        std::uint32_t address, std::uint32_t byteCount) const {
    if (!addressSpace_ || !physicalMemoryArenaBound_ || !graphicsInitialized_ ||
        !graphicsBackend_ || !graphicsBackend_->ready() || !byteCount || byteCount > 64u*1024*1024 ||
        (address&3))return nullptr;
    // Explicitly disjoint namespaces. There is no masked/unowned RAM fallback.
    std::uint64_t physical=address;
    if(address>=0x20000000) {
        if(address<physicalArenaBase_ || std::uint64_t{address}+byteCount>
            std::uint64_t{physicalArenaBase_}+physicalArenaSize_)return nullptr;
        physical=std::uint64_t{physicalAddressOffset_}+address-physicalArenaBase_;
    }
    if(physical>UINT32_MAX)return nullptr;
    const auto range=resolvePhysicalRange(static_cast<std::uint32_t>(physical),byteCount,false);
    if(!range) {
        // v207 (opt-in files/private-trace/diag-registry-adjacent-graphics-views): a read-only graphics view may span
        // owned physical allocations that follow one another without a gap (physical_adjacent_cover.h): the arena maps
        // physical addresses linearly, so the host bytes are contiguous, and the Xenos fetches physical memory
        // linearly. Same bounds as resolvePhysicalRange otherwise (arena, the reserved 0x7FC80000 window).
        static const bool adjacent=::access(RECOMP_APP_PATH("/private-trace/diag-registry-adjacent-graphics-views"),F_OK)==0;
        if(!adjacent || physical<physicalAddressOffset_)return nullptr;
        const auto offset=physical-physicalAddressOffset_;
        if(offset+byteCount>physicalArenaSize_)return nullptr;
        const auto arena=static_cast<std::uint32_t>(physicalArenaBase_+offset);
        if(arena<0x7FC90000 && std::uint64_t{arena}+byteCount>0x7FC80000)return nullptr;
        if(!adjacentAllocationCover(physicalAllocations_,arena,byteCount))return nullptr;
        adjacentGraphicsViews.fetch_add(1,std::memory_order_relaxed);
        return addressSpace_->translate(arena);
    }
    return addressSpace_->translate(range->virtualAddress);
}

std::optional<GuestImportRegistry::StoppedPhysicalOwnership> GuestImportRegistry::stoppedGraphicsPhysicalOwnership(
        std::uint32_t physicalAddress,std::uint32_t byteCount) const {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
        !byteCount || byteCount>0x1000000 || ((physicalAddress|byteCount)&3) ||
        std::uint64_t{physicalAddress}+byteCount>0x20000000) return std::nullopt;
    const auto range=resolvePhysicalRange(physicalAddress,byteCount,false);
    if (!range) return std::nullopt;
    const auto found=physicalAllocations_.find(range->allocationBase);
    if (found==physicalAllocations_.end()) return std::nullopt;
    const auto base=physicalAddress-(range->virtualAddress-found->first);
    if (std::uint64_t{base}+found->second.size>0x20000000) return std::nullopt;
    return StoppedPhysicalOwnership{base,found->first,found->second.size,found->second.protection,
        bool(found->second.protection&PageReadWrite)};
}

bool GuestImportRegistry::publishSerializedGraphicsData(std::uint32_t address,
        const std::vector<std::uint8_t>& expected,const std::vector<std::uint8_t>& completed) {
    const SerializedGraphicsUpdate update{address,&expected,&completed};
    return publishSerializedGraphicsBatch(&update,1);
}
bool GuestImportRegistry::publishSerializedGraphicsBatch(const SerializedGraphicsUpdate* updates,std::size_t count) {
    RECOMP_REGISTRY_LOCK(lock);
    if(!addressSpace_ || !physicalMemoryArenaBound_ || !graphicsInitialized_ || !graphicsBackend_ ||
        !graphicsBackend_->ready() || !updates || !count || count>64)return false;
    std::array<recomp::gpu::NativeSerializedResourceUpdate,64> resolved{};
    for(std::size_t i=0;i<count;++i) {
        const auto& u=updates[i];if(!u.expected || !u.completed)return false;
        const auto address=u.virtualAddress;const auto size=u.expected->size();
        if(!size || size!=u.completed->size() || size>64u*1024*1024 || (address&3) || address<physicalArenaBase_ ||
            std::uint64_t{address}+size>std::uint64_t{physicalArenaBase_}+physicalArenaSize_)return false;
        const auto physical=std::uint64_t{physicalAddressOffset_}+address-physicalArenaBase_;
        if(physical>UINT32_MAX)return false;
        const auto range=resolvePhysicalRange(static_cast<std::uint32_t>(physical),static_cast<std::uint32_t>(size),true);
        if(!range || range->virtualAddress!=address)return false;
        resolved[i]={address,{addressSpace_->translate(address),size},u.expected,u.completed};
    }
    return recomp::gpu::publishNativeSerializedResourceBatch(resolved.data(),count);
}

bool GuestImportRegistry::stoppedGraphicsWordIsWritable(std::uint32_t physicalAddress) const {
    RECOMP_REGISTRY_LOCK(lock);
    return addressSpace_ && graphicsInitialized_ && graphicsBackend_ && graphicsBackend_->ready() &&
        !(physicalAddress & 3) && std::uint64_t{physicalAddress} + 4 <= 0x20000000 &&
        resolvePhysicalRange(physicalAddress, 4, true).has_value();
}

bool GuestImportRegistry::stoppedGraphicsWordAliasesIdentifier(std::uint32_t physicalAddress) const {
    RECOMP_REGISTRY_LOCK(lock);
    if (!graphicsSystemIdentifier_ || (physicalAddress & 3) ||
        std::uint64_t{physicalAddress} + 4 > 0x20000000) return false;
    const auto range = resolvePhysicalRange(physicalAddress, 4, false);
    return range && range->virtualAddress == *graphicsSystemIdentifier_;
}

bool GuestImportRegistry::graphicsMeInitVerified() const {
    RECOMP_REGISTRY_LOCK(lock);
    return graphicsMeInitVerified_;
}

bool GuestImportRegistry::bindNativeInitialCoherencySubmission() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !graphicsBackend_ || graphicsInitialized_ || !physicalMemoryArenaBound_ ||
        initialCoherencySubmissionBound_) return false;
    initialCoherencySubmissionBound_ = true; return true;
}

std::optional<NativeInitialCoherency> GuestImportRegistry::completedNativeInitialCoherency() const {
    RECOMP_REGISTRY_LOCK(lock); return completedInitialCoherency_;
}

bool GuestImportRegistry::submitNativeInitialCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    const bool warm=retiredGraphicsRingReadWord_.has_value();
    if (!initialCoherencySubmissionBound_ || (!warm && completedInitialCoherency_) ||
        (warm && (!nativeStateBatchesBound_ || !completedInitialCoherency_ || completedWarmRingCoherency_ ||
            graphicsWriteBackRebindAllowed_ || !graphicsSystemIdentifier_)) || !addressSpace_ ||
        !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsMeInitVerified_ ||
        !graphicsRingConfiguration_ || !graphicsRingAllocation_ || !graphicsWriteBackAllocation_) return false;
    auto& ring = *graphicsRingConfiguration_;
    if (writeWord != 22 || ring.readWord != 0 || ring.bytes < 88 || !ring.writeBack) return false;
    noteNativeSubmissionMayCommitAt(19); // v183 site 19: may publish completion: the renderer's pending reads first
    const auto ownedRing = resolvePhysicalRange(ring.physicalAddress, ring.bytes, false);
    const auto& wb = *ring.writeBack;
    const auto ownedWb = resolvePhysicalRange(wb.physicalAddress, 4, true);
    if (!ownedRing || ownedRing->allocationBase != *graphicsRingAllocation_ || ownedRing->virtualAddress != ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase != *graphicsWriteBackAllocation_ || ownedWb->virtualAddress != wb.virtualAddress) return false;
    try {
        std::vector<std::uint32_t> root(22), child(11);
        for (std::uint32_t i = 0; i < 22; ++i) if (!readU32(ring.virtualAddress + 4 * i, root[i])) return false;
        if (root[19] != 0xC0013F00 || root[21] != 11 || (root[20] & 3) ||
            std::uint64_t{root[20]} + 44 > 0x20000000) return false;
        const auto ownedChild = resolvePhysicalRange(root[20], 44, false);
        if (!ownedChild || (root[20] < std::uint64_t{ring.physicalAddress} + ring.bytes &&
                ring.physicalAddress < std::uint64_t{root[20]} + 44) ||
            (root[20] < std::uint64_t{wb.physicalAddress} + 4 && wb.physicalAddress < std::uint64_t{root[20]} + 44) ||
            (graphicsSystemIdentifier_ && ownedChild->virtualAddress < std::uint64_t{*graphicsSystemIdentifier_}+4 &&
                *graphicsSystemIdentifier_ < std::uint64_t{ownedChild->virtualAddress}+44)) return false;
        for (std::uint32_t i = 0; i < 11; ++i) if (!readU32(ownedChild->virtualAddress + 4 * i, child[i])) return false;
        NativeInitialCoherency request;
        if (!parseNativeInitialCoherency(root, child, request) ||
            !(warm ? RECOMP_V_BACKEND()->completeWarmRingInitialization(root,child) :
                RECOMP_V_BACKEND()->completeInitialEmptyCacheCoherency(request))) return false;
        // Both complete command buffers validated before native work. Backend
        // completion satisfies the one observed busy-bit wait. No guest shader,
        // resource payload, event/callback, draw or MMIO backing is touched.
        if (warm) completedWarmRingCoherency_=request;
        else completedInitialCoherency_ = request;
        (void)writeU32(wb.virtualAddress, 22); // Prevalidated owned writable word.
        ring.readWord = 22;
        return true;
    } catch (...) { return false; }
}

std::optional<GuestImportRegistry::PhysicalRange> GuestImportRegistry::resolvePhysicalRange(
        std::uint32_t physical, std::uint32_t bytes, bool writable) const {
    if (!physicalMemoryArenaBound_ || !bytes || physical < physicalAddressOffset_) return std::nullopt;
    const auto offset = std::uint64_t{physical} - physicalAddressOffset_;
    if (offset + bytes > physicalArenaSize_) return std::nullopt;
    const auto address = static_cast<std::uint32_t>(physicalArenaBase_ + offset);
    if (address < 0x7FC90000 && std::uint64_t{address} + bytes > 0x7FC80000) return std::nullopt;
    auto found = physicalAllocations_.upper_bound(address);
    if (found == physicalAllocations_.begin()) return std::nullopt;
    --found;
    if (std::uint64_t{address} + bytes > std::uint64_t{found->first} + found->second.size ||
        (writable && !(found->second.protection & PageReadWrite))) return std::nullopt;
    return PhysicalRange{address, found->first};
}

GuestImportDispatchResult GuestImportRegistry::dispatchGraphicsRing(GuestImportCall& call, bool writeBack) {
    const auto name = writeBack ? "__imp__VdEnableRingBufferRPtrWriteBack" : "__imp__VdInitializeRingBuffer";
    if (!graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready()) {
        return rejected(name, "Ring configuration requires a live native graphics backend");
    }
    const auto physical = call.argument32(0), exponent = call.argument32(1);
    if (writeBack) {
        if (!graphicsRingConfiguration_ || (graphicsRingConfiguration_->writeBack && !graphicsWriteBackRebindAllowed_) || !physical ||
            (physical & 3U) || exponent > 19) return rejected(name, "Unsupported read-pointer writeback configuration");
        const auto range = resolvePhysicalRange(physical, 4, true);
        if (!range) return rejected(name, "Writeback word is not inside a writable owned physical allocation");
        const auto& ring = *graphicsRingConfiguration_;
        if (physical < std::uint64_t{ring.physicalAddress} + ring.bytes &&
            ring.physicalAddress < std::uint64_t{physical} + 4) return rejected(name, "Writeback word overlaps the command ring");
        graphicsRingConfiguration_->writeBack = GraphicsRingWriteBack{physical, range->virtualAddress, exponent};
        graphicsWriteBackAllocation_ = range->allocationBase;
        graphicsWriteBackRebindAllowed_=false;
        return handled(name, "Configured read-pointer destination without advancing or writing a completion value");
    }
    if ((physical & 7U) || exponent > 21) {
        return rejected(name, "Unsupported ring alignment, size or reconfiguration");
    }
    // CP_RB_CNTL encodes log2(bytes)-3, not log2(bytes). Same bounded policy
    // as pm4RingBytes; the observed title requests exponent 13 => 64 KiB.
    const auto bytes = std::uint32_t{1} << (exponent + 3);
    const auto range = resolvePhysicalRange(physical, bytes, false);
    if (!range) return rejected(name, "Full command ring is not inside one owned physical allocation");
    if (graphicsRingConfiguration_) {
        auto& ring=*graphicsRingConfiguration_;
        const auto previous=std::max(completedNativeStateBatches_?completedNativeStateBatches_->readWord:0,
            completedEmptyCacheCoherency_?completedEmptyCacheCoherency_->readWord:0);
        if (!nativeStateBatchesBound_ || !completedNativeStateBatches_ || retiredGraphicsRingReadWord_ ||
            graphicsSystemIdentifier_ || !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ || !ring.writeBack ||
            ring.physicalAddress!=physical || ring.bytes!=bytes || ring.virtualAddress!=range->virtualAddress ||
            *graphicsRingAllocation_!=range->allocationBase || ring.readWord!=previous) {
            return rejected(name,"Only a released, completed same-allocation startup ring can be reconfigured");
        }
        const auto wb=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
        if (!wb || wb->allocationBase!=*graphicsWriteBackAllocation_ || wb->virtualAddress!=ring.writeBack->virtualAddress)
            return rejected(name,"Retiring ring writeback ownership is invalid");
        try {
            if (!RECOMP_V_BACKEND()->drainForRingReconfiguration()) return rejected(name,"Native queue did not confirm ring retirement");
        } catch (...) { return rejected(name,"Native queue drain failed"); }
        retiredGraphicsRingReadWord_=ring.readWord;
        ring.readWord=0; graphicsMeInitVerified_=false; graphicsWriteBackRebindAllowed_=true;
        // Keep the previous writeback registration pinned until its replacement
        // is validated. Neither the old nor the new destination is written here.
        return handled(name,"Drained native queue and restarted the same command ring without replaying completed work");
    }
    graphicsRingConfiguration_ = GraphicsRingConfiguration{physical, range->virtualAddress, bytes, 0, std::nullopt};
    graphicsRingAllocation_ = range->allocationBase;
    return handled(name, "Configured bounded command ring; MMIO and execution remain separately guarded");
}

bool GuestImportRegistry::bindTitleTerminationNotifications(std::uint32_t imageBase,
        std::uint32_t imageBytes, std::function<bool(std::uint32_t)> callbackValidator) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || terminationCallbackValidator_ || !callbackValidator || !imageBase ||
        (imageBase & 3U) || imageBytes < 16 || !contains(imageBase, imageBytes)) return false;
    terminationImageBase_ = imageBase; terminationImageBytes_ = imageBytes;
    terminationCallbackValidator_ = std::move(callbackValidator);
    return true;
}

std::vector<GuestImportRegistry::TitleTerminationRegistration>
GuestImportRegistry::titleTerminationRegistrations() const {
    RECOMP_REGISTRY_LOCK(lock);
    return titleTerminationRegistrations_;
}

GuestImportDispatchResult GuestImportRegistry::dispatchTitleTerminationRegistration(GuestImportCall& call) {
    constexpr auto Name = "__imp__ExRegisterTitleTerminateNotification";
    const auto record = call.argument32(0), create = call.argument32(1);
    if (!terminationCallbackValidator_ || create > 1) return rejected(Name, "Termination registration service/profile unavailable");
    // The observed profile places full 16-byte records in the loaded image.
    // List links are not followed or rewritten by this host-owned collection.
    if ((record & 3U) || record < terminationImageBase_ ||
        std::uint64_t{record} + 16 > std::uint64_t{terminationImageBase_} + terminationImageBytes_) {
        return memoryFault(Name, "Termination record outside the explicitly bound image");
    }
    std::uint32_t routine = 0, priority = 0;
    if (!readU32(record, routine) || !readU32(record + 4, priority) || !routine ||
        !terminationCallbackValidator_(routine)) return rejected(Name, "Termination callback has no exact guest mapping");
    auto found = std::find_if(titleTerminationRegistrations_.begin(), titleTerminationRegistrations_.end(),
        [&](const auto& item) { return item.record == record || item.routine == routine; });
    if (create) {
        if (found != titleTerminationRegistrations_.end() ||
            titleTerminationRegistrations_.size() >= MaximumTitleTerminationRegistrations) {
            return rejected(Name, "Duplicate or excessive termination registration");
        }
        titleTerminationRegistrations_.push_back({record, routine, priority});
    } else {
        if (found == titleTerminationRegistrations_.end() || found->record != record ||
            found->routine != routine || found->priority != priority) {
            return rejected(Name, "Unknown or mutated termination registration");
        }
        titleTerminationRegistrations_.erase(found);
    }
    // Void ABI: do not overwrite the caller's r3 with an invented result.
    return handled(Name, "Updated bounded title termination metadata; callback execution remains guarded");
}

bool GuestImportRegistry::bindNativeEdramTrainingPolicy(std::uint32_t calibrationLock) {
    RECOMP_REGISTRY_LOCK(lock);
    CsAll csAllGuard; // v197
    const auto section = criticalSections_.find(calibrationLock);
    if (!experimentalInitializationBound_ || graphicsInitialized_ || edramCalibrationLock_ ||
        section == criticalSections_.end() || section->second.ownerToken || section->second.recursionCount ||
        !criticalSectionStorageMatches(calibrationLock,section->second)) return false;
    edramCalibrationLock_ = calibrationLock; return true;
}

GuestImportRegistry::NativeEdramTrainingState GuestImportRegistry::nativeEdramTrainingState() const {
    RECOMP_REGISTRY_LOCK(lock); return edramTrainingState_;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNativeEdramTraining(std::string_view name, GuestImportCall& call) {
    // The native driver manages its memory links; there is no Xenos HSIO bus.
    // This explicit policy only answers the observed startup's no-command case,
    // not electrical-training success, rendering or arbitrary command emission.
    if (name == "__imp__VdIsHSIOTrainingSucceeded") {
        // The public BOOL tests whether initialization can proceed. On this
        // explicit native policy it means driver-managed readiness, not an
        // assertion that an Xbox electrical calibration has taken place.
        if (!experimentalInitializationBound_ || !edramCalibrationLock_ || !completedExperimentalInitialization_ ||
            !edramTrainingState_.noCommandQueries || edramPreparedThread_ || !graphicsInitialized_ ||
            !graphicsBackend_ || !graphicsBackend_->ready() || edramTrainingState_.statusQueries == UINT32_MAX)
            return rejected(name,"Native training status requires completed no-command calibration and a live backend");
        try {
            if (RECOMP_V_BACKEND()->queryEdramTrainingPolicy() != GuestGraphicsBackend::EdramTrainingPolicy::DriverManagedNoXenosLink)
                return rejected(name,"Native training status backend is unavailable");
        } catch (...) { return rejected(name,"Native training status backend query failed"); }
        ++edramTrainingState_.statusQueries;
        call.returnValue = 1;
        return handled(name,"Native driver-managed memory is ready; Xbox HSIO training was not performed");
    }
    auto* thread = findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    CsAll csAllGuard; // v197
    const auto section = criticalSections_.find(edramCalibrationLock_);
    if (!experimentalInitializationBound_ || !completedExperimentalInitialization_ || !graphicsInitialized_ ||
        !graphicsBackend_ || !graphicsBackend_->ready() || !thread || thread->returned || thread->suspendCount ||
        !thread->criticalRegionDepth || !edramCalibrationLock_ || section == criticalSections_.end() ||
        section->second.ownerToken != RECOMP_CURRENT_GUEST_TOKEN || !section->second.recursionCount ||
        !criticalSectionStorageMatches(edramCalibrationLock_,section->second))
        return rejected(name,"Native EDRAM policy requires completed startup and current ownership of the calibration lock/critical region");
    const bool worker = name == "__imp__VdRetrainEDRAMWorker";
    const bool direct = !worker && call.arguments[0]==0;
    if (worker) {
        if (call.argument32(0) || edramPreparedThread_ || edramTrainingState_.workerChecks == UINT32_MAX)
            return rejected(name,"Unsupported or already pending native calibration-worker request");
    } else {
        // Observed caller's two zero in/out words and 2048-byte stack scratch.
        // No buffers are altered on the no-command path. Unknown nonzero command
        // pointers must not be reported consumed or silently dropped.
        const auto sp = std::uint64_t{call.stackPointer};
        if (direct) {
            // The later observed caller8227D2B0 explicitly skips Worker when
            // its preparation argument is zero. Re-query the native driver's
            // no-link capability; do not invent a worker or consume commands.
            const auto owner=threadStorage_.lookup(thread->threadId);
            if (edramPreparedThread_ || !edramTrainingState_.noCommandQueries || !completedDisplayOutput_ ||
                !validateNativeScalerStorageLocked() ||
                !(completedPostOutputColor_?validateNativePostOutputScanoutLocked():validateNativeScanoutLocked()) || thread->initial || !owner ||
                edramTrainingState_.directNoCommandQueries>=256 || sp<owner->layout.stackBase ||
                sp+2144>std::uint64_t{owner->layout.stackBase}+owner->layout.stackSize ||
                !validGuestDataBuffer(call.stackPointer,2144,true,16) || imageProtections_.overlaps(call.stackPointer,2144))
                return rejected(name,"Direct native EDRAM query requires completed display, owned stack and no pending worker");
            for(unsigned i=0;i<6;++i)if(call.arguments[i]>UINT32_MAX)
                return rejected(name,"Direct native EDRAM arguments must be32bit");
            { CsAll csAllGuard2; for(const auto& cs:criticalSections_)if(sp<std::uint64_t{cs.first}+28 && cs.first<sp+2144)
                return rejected(name,"Direct native EDRAM stack aliases a critical section"); } // (v197)
            if(hasDispatcherWaitInRange(call.stackPointer,2144))
                return rejected(name,"Direct native EDRAM stack aliases an event wait");
        }
        if ((!direct && (edramPreparedThread_ != RECOMP_CURRENT_GUEST_TOKEN || call.argument32(0) != 1)) ||
            call.argument32(2) != 4096 || call.argument32(5) != 2048 || !sp || (sp & 15U) ||
            sp + 2144 > UINT32_MAX || call.argument32(1) != sp+80 || call.argument32(3) != sp+84 ||
            call.argument32(4) != sp+96 || !contains(static_cast<std::uint32_t>(sp+80),2064) ||
            (sp+80 < 0x7FC90000 && sp+2144 > 0x7FC80000) ||
            overlapsProtectedThreadStorage(static_cast<std::uint32_t>(sp+80),2064) ||
            overlapsThreadTlsStorage(static_cast<std::uint32_t>(sp+80),2064) ||
            edramTrainingState_.noCommandQueries == UINT32_MAX)
            return rejected(name,"Unsupported native EDRAM command/scratch shape or missing worker preparation");
        std::uint32_t command = 0, count = 0;
        if (!readU32(call.argument32(1),command) || !readU32(call.argument32(3),count) || command || count)
            return rejected(name,"Native no-link policy cannot consume nonempty guest training commands");
    }
    try {
        if (RECOMP_V_BACKEND()->queryEdramTrainingPolicy() != GuestGraphicsBackend::EdramTrainingPolicy::DriverManagedNoXenosLink)
            return rejected(name,"Backend does not currently provide the native no-Xenos-link capability");
    } catch (...) { return rejected(name,"Backend native EDRAM policy query failed"); }
    auto result=handled(name,"Validated driver-managed native memory policy; no Xenos electrical training or EDRAM rendering claimed");
    if (worker) {
        edramPreparedThread_ = RECOMP_CURRENT_GUEST_TOKEN; ++edramTrainingState_.workerChecks;
        // The observed caller ignores the worker return. Preserve void ABI.
    } else {
        edramPreparedThread_ = 0; ++edramTrainingState_.noCommandQueries;
        if(direct)++edramTrainingState_.directNoCommandQueries;
        call.returnValue = 0; // No guest hardware-training commands required.
    }
    return result;
}

const std::array<GuestImportDescriptor, 137>&
GuestImportRegistry::implementedBootImports() noexcept {
    return ImplementedBootImports;
}

bool GuestImportRegistry::isImplemented(std::string_view importName) noexcept {
    return std::any_of(
            ImplementedBootImports.begin(),
            ImplementedBootImports.end(),
            [importName](const GuestImportDescriptor& descriptor) {
                return descriptor.name == importName;
            });
}

// DIAGNOSTIC v693: last import per guest thread, dumped by the runner.
// v42: every import of every thread used to take one global mutex here (a second serializer beside
// R, held for map lookups, string copies and wait stack walks). Now each thread record has its own
// spin flag (a record is written only by the thread whose token owns it, and read by the dump), the
// handle history keeps a mutex that only handle-creating and signalling imports take, and time is
// the ARM generic timer instead of a steady_clock::now() call per import. Same RENDER_DIAG_IMPORT
// output. Lock order: a record flag and the handle mutex are never held together by a recorder; the
// dump takes the handle mutex, then each record flag briefly.
namespace {
struct DiagnosticImport {std::atomic<bool> busy{false};std::uint32_t token=0,r3=0,r4=0;std::string name;std::uint64_t at=0;unsigned long long calls=0;std::array<std::uint32_t,4> multi{};
    std::array<std::uint32_t,12> stack{}; // Attempt v5: guest return addresses of the last wait, innermost first.
    std::string pendingCreate;std::uint32_t pendingOut=0;}; // NtCreate* of this thread: its handle is read at the next import
struct DiagnosticHandle {std::uint32_t creator=0,signaler=0;std::string created,signal;std::uint64_t createdAt=0,signalAt=0;unsigned long long signals=0;};
std::mutex diagnosticHandleMutex; // guards diagnosticHandles
std::map<std::uint32_t,DiagnosticHandle> diagnosticHandles; // NT handle or dispatcher object address
std::array<DiagnosticImport,128> diagnosticImports{};
class DiagnosticRecordLock {
public:
    explicit DiagnosticRecordLock(std::atomic<bool>& busy) noexcept : busy_(busy) {
        while (busy_.exchange(true, std::memory_order_acquire))
            while (busy_.load(std::memory_order_relaxed)) std::this_thread::yield();
    }
    ~DiagnosticRecordLock() { busy_.store(false, std::memory_order_release); }
    DiagnosticRecordLock(const DiagnosticRecordLock&) = delete;
    DiagnosticRecordLock& operator=(const DiagnosticRecordLock&) = delete;
private:
    std::atomic<bool>& busy_;
};
long long diagnosticAgeMs(std::uint64_t now, std::uint64_t at) {
    return static_cast<long long>(registryTicksToNs(now - at) / 1000000);
}
using DiagnosticRead=std::function<std::optional<std::uint32_t>(std::uint32_t)>;
void diagnosticRecordImport(std::uint32_t token,std::string_view name,std::uint32_t r3,std::uint32_t r4,const DiagnosticRead& read,std::uint32_t sp=0) {
    if(!token)return;const auto now=registryTicks();
    std::string created;std::uint32_t createdHandle=0;
    unsigned i=(token>>12)&127;
    for(unsigned probe=0;probe<128;++probe,i=(i+1)&127){auto& d=diagnosticImports[i];
        DiagnosticRecordLock record(d.busy);
        if(d.token!=token && d.token)continue;
        if(d.token==token && !d.pendingCreate.empty()) {
            if(const auto h=read(d.pendingOut);h && *h){createdHandle=*h;created.swap(d.pendingCreate);}
            d.pendingCreate.clear();
        }
        if(name.starts_with("__imp__NtCreate")){d.pendingCreate.assign(name);d.pendingOut=r3;}
        d.token=token;d.name.assign(name);d.r3=r3;d.r4=r4;d.at=now;++d.calls;d.multi={};
        // Attempt v5: walk the guest stack of waits. Xbox 360 frames: back chain at 0(r1), the
        // caller's LR saved at back_chain-8 (mflr r12; stw r12,-8(r1); stwu r1,-N(r1)).
        if(sp && (name.find("Wait")!=std::string_view::npos || name.find("Delay")!=std::string_view::npos ||
                  name.find("Signal")!=std::string_view::npos)) {
            d.stack={};std::uint32_t frame=sp;
            for(unsigned k=0;k<d.stack.size();++k) {
                const auto back=read(frame);if(!back || *back<=frame || *back-frame>0x40000 || (*back&3))break;
                const auto lr=read(*back-8);if(!lr || *lr<0x82000000u || *lr>=0x84000000u)break;
                d.stack[k]=*lr;frame=*back;
            }
        }
        if(name.find("Multiple")!=std::string_view::npos)
            for(unsigned k=0;k<4 && k<r3;++k)if(const auto h=read(r4+k*4))d.multi[k]=*h;
        break;
    }
    const bool initializes=name.starts_with("__imp__KeInitialize");
    const bool signals=name=="__imp__NtSetEvent" || name=="__imp__NtPulseEvent" || name=="__imp__NtReleaseSemaphore" ||
       name=="__imp__NtReleaseMutant" || name=="__imp__KeSetEvent" || name=="__imp__KePulseEvent" ||
       name=="__imp__KeReleaseSemaphore" || name=="__imp__KeReleaseMutant" || name=="__imp__NtSignalAndWaitForSingleObjectEx";
    if(!createdHandle && !initializes && !signals)return;
    std::lock_guard handles(diagnosticHandleMutex);
    if(createdHandle){auto& h=diagnosticHandles[createdHandle];h.creator=token;h.created.swap(created);h.createdAt=now;}
    if(initializes){auto& h=diagnosticHandles[r3];h.creator=token;h.created.assign(name);h.createdAt=now;}
    if(signals){auto& h=diagnosticHandles[r3];h.signaler=token;h.signal.assign(name);h.signalAt=now;++h.signals;}
}
std::string diagnosticDescribe(std::uint32_t handle,std::uint64_t now) {
    const auto f=diagnosticHandles.find(handle);char t[260];
    if(f==diagnosticHandles.end()){std::snprintf(t,sizeof t," [%08x unknown]",handle);return t;}
    const auto& h=f->second;
    std::snprintf(t,sizeof t," [%08x creator=%08x %s %lldms signaler=%08x %s %lldms signals=%llu]",handle,h.creator,h.created.c_str(),
        h.creator?diagnosticAgeMs(now,h.createdAt):-1LL,h.signaler,h.signal.c_str(),h.signaler?diagnosticAgeMs(now,h.signalAt):-1LL,h.signals);
    return t;
}
}
void diagnosticDumpImports() {
    if(!diagnosticHandleMutex.try_lock())return;std::lock_guard lock(diagnosticHandleMutex,std::adopt_lock);
    const auto now=registryTicks();
    for(auto& record:diagnosticImports) {
        std::uint32_t token=0,r3=0,r4=0;std::string name;std::uint64_t at=0;unsigned long long calls=0;
        std::array<std::uint32_t,4> multi{};std::array<std::uint32_t,12> frames{};
        {
            DiagnosticRecordLock guard(record.busy);
            token=record.token;if(!token)continue;
            r3=record.r3;r4=record.r4;name=record.name;at=record.at;calls=record.calls;multi=record.multi;frames=record.stack;
        }
        std::string about;
        if(name.find("Multiple")!=std::string::npos){for(auto h:multi)if(h)about+=diagnosticDescribe(h,now);}
        else if(name.find("Wait")!=std::string::npos)about=diagnosticDescribe(r3,now);
        char stack[12*9+16]{};int w=0;
        for(auto a:frames)if(a && w>=0 && w<(int)sizeof stack-10)w+=std::snprintf(stack+w,sizeof stack-w,"%s%08x",w?",":" stack=",a);
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
            "RENDER_DIAG_IMPORT token=%08x last=%s r3=%08x r4=%08x age_ms=%lld calls=%llu%s%s",token,name.c_str(),r3,r4,
            diagnosticAgeMs(now,at),calls,about.c_str(),stack);
    }
}

extern "C" std::atomic<unsigned> recomp_v348_audio_spin_us; // v348 (generic_renderer.cpp's switch poll)
GuestImportDispatchResult GuestImportRegistry::dispatch(
        std::string_view importName,
        GuestImportCall& call) {
    if (!iosHandleDescriberReady.load(std::memory_order_relaxed)) { // ios-handle-describe (DIAGNOSTIC), once
        static std::once_flag once;
        std::call_once(once, [this] {
            iosHandleDescriber = [this](std::uint32_t handle, char* out, std::size_t size) -> int {
                std::unique_lock<std::mutex> l(registryMutex(), std::defer_lock); // (try-lock only: never blocks the watcher)
                for (int i = 0; i < 50 && !l.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!l.owns_lock()) return std::snprintf(out, size, "registry-busy");
                const auto it = kernelHandles_.find(handle);
                if (it == kernelHandles_.end()) return std::snprintf(out, size, "no-kernel-object");
                const auto& k = it->second;
                return std::snprintf(out, size, "kind=%d signal=%u max=%u owner=%08x recursion=%u abandoned=%d", int(k.kind),
                                     k.signalCount, k.maximumCount, k.ownerToken, k.recursion, int(k.abandoned));
            };
            iosHandleDescriberReady.store(true, std::memory_order_release);
        });
    }
    // v348 (diagnostic causal profiling, diag-v348-audio-spin): a busy wait before each audio frame submission (no lock held),
    // to measure how much the frame follows the audio thread's work.
    if (const unsigned us = recomp_v348_audio_spin_us.load(std::memory_order_relaxed); us && importName == "__imp__XAudioSubmitRenderDriverFrame") {
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        while (std::chrono::steady_clock::now() < until) {}
    }
    // v168 (goal step 1, baseline evidence 2026-09-27): the main thread's Sleep(0) poll loop (sub_8239DF60 ->
    // SleepEx sub_824F1388, ~62k calls/s) spent 44% of that thread's CPU in this import path (name dispatch, R taken
    // and released twice, interval validation, result text allocation). A repeat of a zero delay that the locked
    // path already accepted needs none of it. The locked path records (registry, calling token, interval address,
    // validation generation) when it accepted a zero interval whose validity cannot depend on other threads' flags
    // (the address is outside the thread arena, or inside the caller's own stack); a call that repeats it while the
    // generation word shows the same generation and no open mutation window has, by the registry's snapshot
    // invariant (guest_registry_concurrency.h, point 4), the same validGuestDataBuffer answer. It then reads the
    // interval (still zero), runs the same delay handler with the same epoch deadline, and sched_yield()s: Sleep(0)
    // yields the processor on the 360. The caller is running, so the slow path's live/started/not-returned guards
    // hold. Not counted in threadDelayStatistics_ (diagnostic counters).
    // files/private-trace/diag-registry-slow-zero-yield restores v167 for every delay.
    // v197: uncontended critical sections without R (see CsAll). The same checks and effects as the locked paths'
    // uncontended branches for an entry that already exists; anything else (a new or adopted section, contention,
    // waiters to wake, the EDRAM calibration lock, an open exclusive scope, a storage mismatch) takes the locked path.
    if (csFastEnabled() && importName.size() == 30 && addressSpace_ != nullptr) {
        const bool enter = importName == "__imp__RtlEnterCriticalSection";
        const bool leave = !enter && importName == "__imp__RtlLeaveCriticalSection";
        constexpr bool tryEnter = false; // TryEnter validates writability against R-protected state first: locked path
        const auto address = call.argument32(0);
        const auto token = RECOMP_CURRENT_GUEST_TOKEN;
        if ((enter || leave || tryEnter) && token != 0 && address != 0 && !(address & 3) &&
                !(enter && csRejectionRecorded.load(std::memory_order_acquire)) &&
                address != edramCalibrationLock_ && !guestExclusiveScopeActive()) {
            std::unique_lock stripe(csStripeLocks[csStripe(address)]);
            // imports-fast D: the node cache answers under this address's stripe (every erase holds CsAll and bumps).
            const auto section = ::recomp::imports_fast::on()
                    ? ::recomp::imports_fast::find(criticalSections_, address, ::recomp::imports_fast::csGenerations(),
                                                   ::recomp::imports_fast::csCache())
                    : criticalSections_.find(address);
            if (section != criticalSections_.end() && criticalSectionStorageMatches(address, section->second)) {
                auto next = section->second;
                const bool queued = criticalSectionWaits_.count(address) != 0;
                bool done = false;
                if (enter || tryEnter) {
                    const bool free = !next.ownerToken && !queued, mine = next.ownerToken == token;
                    if ((free || mine) && next.recursionCount != MaximumCriticalSectionRecursion &&
                            (!tryEnter || validGuestDataBuffer(address, CriticalSectionSize, true))) {
                        next.ownerToken = token; ++next.recursionCount;
                        if (writeCriticalSectionState(address, next)) {
                            section->second = next; done = true;
                            if (tryEnter) { call.returnValue = 1; csFastTries.fetch_add(1, std::memory_order_relaxed); }
                            else csFastEnters.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (tryEnter && next.ownerToken && next.ownerToken != token) { // busy: nothing acquired or queued
                        call.returnValue = 0; done = true; csFastTries.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (next.ownerToken == token && next.recursionCount != 0 && !(next.recursionCount == 1 && queued)) {
                    --next.recursionCount;
                    if (!next.recursionCount) next.ownerToken = 0;
                    if (writeCriticalSectionState(address, next)) {
                        section->second = next; done = true; csFastLeaves.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (done) {
                    GuestImportDispatchResult quick;
                    quick.state = GuestImportDispatchState::Handled;
                    return quick;
                }
            }
            csFastFallbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (importName.size() == 20 && importName == "__imp__KeTlsGetValue" && !v252Baseline() && addressSpace_ != nullptr) { // v252
        const auto& c = tlsReadCache;
        const auto token = RECOMP_CURRENT_GUEST_TOKEN;
        const auto index = call.argument32(0);
        if (token != 0 && c.registry == this && c.token == token && index < c.slotCount && index < TlsSlotCount) {
            const auto* mirror = tlsSlotMirror(this, false);
            const auto* slot = mirror ? registryFindSlot(this) : nullptr;
            if (slot && ((mirror->bits[index / 64].load(std::memory_order_acquire) >> (index % 64)) & 1U)) {
                const auto word = slot->word.load(std::memory_order_acquire);
                std::uint32_t value = 0;
                if ((word & RegistryWindowMask) == 0 && (word >> 16) == c.generation && readU32(c.base + index * 4U, value)) {
                    call.returnValue = value;
                    v252TlsFast.fetch_add(1, std::memory_order_relaxed);
                    GuestImportDispatchResult quick;
                    quick.state = GuestImportDispatchState::Handled;
                    return quick;
                }
            }
        }
    }
    if (importName.size() == 29 && importName == "__imp__KeDelayExecutionThread") {
        static const bool fast = ::access(RECOMP_APP_PATH("/private-trace/diag-registry-slow-zero-yield"), F_OK) != 0;
        auto& cached = zeroYieldCache;
        const auto interval = call.argument32(2);
        if (fast && cached.registry == this && cached.counters != nullptr && cached.interval == interval && call.argument32(0) <= 1 &&
                call.argument32(1) == 0 && addressSpace_ != nullptr && threadDelayHandler_) {
            const auto token = RECOMP_CURRENT_GUEST_TOKEN;
            const auto* slot = token != 0 && token == cached.token ? registryFindSlot(this) : nullptr;
            const auto word = slot ? slot->word.load(std::memory_order_acquire) : ~std::uint64_t{0};
            const auto* first = slot && (word & RegistryWindowMask) == 0 && (word >> 16) == cached.generation
                    ? addressSpace_->translate(interval) : nullptr;
            if (first != nullptr) {
                bool zero = true;
                for (unsigned i = 0; i < 8; ++i) zero = zero && first[i] == 0;
                if (zero) {
                    if (recomp::diag::timeline::armed.load(std::memory_order_relaxed)) { // v201: poll phases, one event each
                        thread_local std::uint64_t phaseStart = 0, lastYield = 0;
                        const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                        if (now - lastYield > 50'000) {
                            const auto previous = phaseStart ? (lastYield - phaseStart) / 1000 : 0;
                            recomp::diag::timeline::mark(recomp::diag::timeline::PollPhase, 0,
                                    static_cast<std::uint32_t>(std::min<std::uint64_t>(previous, 0xFFFFFFFFu)));
                            phaseStart = now;
                        }
                        lastYield = now;
                    }
                    auto& counters = *cached.counters;
                    RegistryWakes::flushActive(); // PARALLEL: never yield holding undelivered wakes.
                    counters.calls.fetch_add(1, std::memory_order_relaxed);
                    counters.zeros.fetch_add(1, std::memory_order_relaxed);
                    counters.inflight.fetch_add(1, std::memory_order_acq_rel);
                    bool completed = false;
                    try { completed = threadDelayHandler_(std::chrono::steady_clock::time_point{}); }
                    catch (...) { counters.inflight.fetch_sub(1, std::memory_order_acq_rel); counters.failed.fetch_add(1); throw; }
                    counters.inflight.fetch_sub(1, std::memory_order_acq_rel);
                    if (!completed || RECOMP_CURRENT_GUEST_TOKEN != token) {
                        // Same outcome as the locked path's cancelled/lost-identity case.
                        counters.failed.fetch_add(1, std::memory_order_relaxed);
                        auto refused = rejected("__imp__KeDelayExecutionThread",
                                                "Native delay cancelled, resumed early or lost its original live thread identity");
                        materializeDispatchResult(refused);
                        return refused;
                    }
                    counters.completed.fetch_add(1, std::memory_order_relaxed);
                    { // v266 (poll_sleep_v266.h): a poll phase older than SpinNs sleeps until the next guest park or the timeout
                        // (enabled: set by the renderer at its 60-present A/B window boundaries, RENDER_GENERIC_V210 v266_poll_sleep).
                        // v270: nothing but the flag test while the switch is off (v266 read steady_clock twice per call:
                        // ~215 ns each at ~110 k Sleep(0)/s on main, ~5% of main's samples); the ARM generic timer when on.
                        auto& ps = recomp_v266::pollSleep();
                        if (ps.enabled.load(std::memory_order_relaxed)) {
                            thread_local std::uint64_t v266PhaseStart = 0, v266LastReturn = 0;
                            const auto t0 = registryTicks();
                            if (registryTicksToNs(t0 - v266LastReturn) > recomp_v266::PollSleep::PhaseGapNs) v266PhaseStart = t0;
                            if (registryTicksToNs(t0 - v266PhaseStart) > recomp_v266::PollSleep::SpinNs) recomp_v266::sleepPoll();
                            else { ps.yields.fetch_add(1, std::memory_order_relaxed); ::sched_yield(); }
                            v266LastReturn = registryTicks();
                        } else {
                            if (ps.clockBaseline.load(std::memory_order_relaxed)) { // v270 A/B: v266's two reads per call
                                thread_local std::uint64_t baselineSink = 0;
                                baselineSink += static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                                ::sched_yield();
                                baselineSink += static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                            } else recomp_ms::sleepZeroYield(token); // main-sched: diag-ms-prime-noyield (else sched_yield as before)
                        }
                    }
                    call.returnValue = StatusSuccess;
                    GuestImportDispatchResult quick;
                    quick.state = GuestImportDispatchState::Handled;
                    return quick;
                }
            }
        }
    }
    GuestImportDispatchResult result;
    {
        // PARALLEL: wake requests collected by this import are delivered after the registry
        // mutex is released (declared first, destroyed after dispatchUnderRegistryLock returns).
        // Waits flush it before parking.
        [[maybe_unused]] RegistryWakes wakes;
        // v34: the diagnostic record has its own mutex and reads only guest memory through the fixed
        // address space, so it runs before R is taken (it was part of every import's R hold time).
        // v168: release profile: off unless files/private-trace/diag-import-records exists (it feeds only the
        // freeze report's "waiting threads" section; turn it on when chasing a freeze).
        static const bool records = ::access(RECOMP_APP_PATH("/private-trace/diag-import-records"), F_OK) == 0;
        if (records && addressSpace_ != nullptr)
            diagnosticRecordImport(RECOMP_CURRENT_GUEST_TOKEN,importName,call.argument32(0),call.argument32(1),
                [&](std::uint32_t a)->std::optional<std::uint32_t>{const auto* p=addressSpace_->translate(a);if(!p)return std::nullopt;
                    return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];},call.stackPointer); // DIAGNOSTIC v695, v5 stack
        result = dispatchUnderRegistryLock(importName, call);
    }
#if defined(RECOMP_IMPORT_TEXT_ON_FAILURE_ONLY) && RECOMP_IMPORT_TEXT_ON_FAILURE_ONLY
    // v168 (device build define): a handled import's name/text are never read by the import bridge (it reads the
    // diagnostic only for a refused import), and materializing them cost two heap allocations per call.
    if (!result.handled()) materializeDispatchResult(result);
#else
    materializeDispatchResult(result); // v42: the result's text is allocated after R (and the wakes)
#endif
    return result;
}

namespace {
// v142 (saves): state for guest_saves_dispatch.inc: the local profile of user 0 and its saved-game content. One per
// registry instance, created on the first eligible dispatch, and only touched with that registry's lock held.
// Device storage: files/saves (content) and files/profile (identity, settings); RECOMP_SAVES_DIR overrides the base
// directory (host tests). files/private-trace/diag-no-local-profile disables it (v141 behaviour).
struct SaveFile {
    int fd = -1; bool directory = false, write = false, deleteOnClose = false; std::uint64_t position = 0;
    std::uint64_t bytesRead = 0, bytesWritten = 0; // v166: logged at close (SAVES_FILE_CLOSE)
    saves::ContentKey key; std::string path, root;
};
struct SaveEnumerator { std::vector<std::vector<std::uint8_t>> records; std::size_t cursor = 0; std::uint32_t capacity = 0;
                        std::uint32_t itemSize = GuestContentCatalog::RecordBytes; }; // v143: achievement enumerators use 36/500
struct SavesState {
    std::uint32_t titleId = 0; profile::Identity identity; std::string profileDir;
    std::map<std::uint32_t, std::unique_ptr<profile::Settings>> settings;
    std::optional<saves::SaveStore> store;
    std::map<std::string, saves::ContentKey> mounts;
    std::map<std::uint32_t, SaveFile> files;
    std::map<std::uint32_t, SaveEnumerator> enums;
    std::uint64_t writes = 0, bytesWritten = 0, contentOpens = 0;
    std::set<std::uint32_t> skipped;
    profile::Settings& settingsFor(std::uint32_t title) {
        auto& slot = settings[title];
        if (!slot) { slot = std::make_unique<profile::Settings>(profileDir, title); }
        return *slot;
    }
    void noteSkippedSetting(std::uint32_t id) {
        if (skipped.insert(id).second) { __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_PROFILE_SETTING_UNMODELED id=%08x", id); }
    }
    void closeFd(int fd) { if (fd >= 0) { (void)store->close(fd); } }
    void closeFile(SaveFile& file) {
        if (file.fd >= 0) { if (!file.directory) { (void)store->flush(file.fd); } (void)store->close(file.fd); file.fd = -1; }
        if (file.deleteOnClose) { (void)store->removeFile(file.key, file.path); }
    }
    void recordAchievement(std::uint32_t id) {
        __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_ACHIEVEMENT title=%08x id=%u (local profile)", titleId, id);
        if (std::FILE* f = std::fopen((profileDir + "/achievements.txt").c_str(), "a")) { std::fprintf(f, "%08x %u\n", titleId, id); std::fclose(f); }
    }
};
std::uint64_t savesRandom64() { std::random_device device; return (std::uint64_t{device()} << 32) ^ device(); }
// imports-fast B: the last registry whose saves slot was settled (slot.first set: a later eligible call only looks it up;
// slots are never erased). Set under R (savesStateFor runs only in dispatchUnderRegistryLock), read under R.
std::atomic<const void*> savesSettledRegistry{nullptr};
bool savesStateSettled(const void* registry) noexcept { return savesSettledRegistry.load(std::memory_order_acquire) == registry; }
} // namespace
// imports-fast host test seams (tests/imports_fast_saves_test.cpp; the cache counters only in -DRECOMP_IMPORTS_FAST_COUNTERS builds).
extern "C" const void* recomp_imports_fast_saves_settled() noexcept { return savesSettledRegistry.load(std::memory_order_acquire); }
#if defined(RECOMP_IMPORTS_FAST_COUNTERS)
extern "C" void recomp_imports_fast_cache_counters(unsigned long long* hits, unsigned long long* misses) noexcept {
    *hits = ::recomp::imports_fast::cacheHits().load(); *misses = ::recomp::imports_fast::cacheMisses().load();
}
#endif
namespace {
SavesState* savesStateFor(const void* registry, bool eligible, std::uint32_t installedTitle) {
    if (!eligible) { return nullptr; }
    static std::mutex mutex; static std::map<const void*, std::pair<bool, std::unique_ptr<SavesState>>> slots;
    std::lock_guard guard(mutex);
    auto& slot = slots[registry];
    if (!slot.first) {
        slot.first = true;
        savesSettledRegistry.store(registry, std::memory_order_release); // imports-fast B
        const char* base = std::getenv("RECOMP_SAVES_DIR");
        const std::string root = save_sandbox::filesRootFor(base ? base : RECOMP_APP_FILES); // save-sandbox
        const bool disabled = !base && access(RECOMP_APP_PATH("/private-trace/diag-no-local-profile"), F_OK) == 0;
        if (!disabled) {
            auto state = std::make_unique<SavesState>();
            state->titleId = installedTitle ? installedTitle : 0x545408A7U;
            state->profileDir = root + "/profile";
            std::string error;
            state->identity = profile::loadOrCreateIdentity(state->profileDir, &savesRandom64, &error);
            if (state->identity.xuid) {
                state->store.emplace(root + "/saves");
                __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_INIT user0=local xuid=%016llx gamertag=%s title=%08x root=%s",
                                    (unsigned long long)state->identity.xuid, state->identity.gamertag.c_str(), state->titleId, root.c_str());
                slot.second = std::move(state);
            } else {
                __android_log_print(ANDROID_LOG_WARN, "RecompSaves", "SAVES_INIT_FAILED %s", error.c_str());
            }
        } else {
            __android_log_print(ANDROID_LOG_INFO, "RecompSaves", "SAVES_INIT disabled (diag-no-local-profile)");
        }
    }
    return slot.second.get();
}
}

// v279 / v282 (see validateNativeTileSceneInterruptScopeLocked and the v282 scene IRQ below)
namespace { thread_local bool nativeSceneInlineActive=false; bool nativeSceneLastInline=false; } // (last: under the registry lock)
extern "C" void recomp_v279_scene_inline(bool on) { nativeSceneInlineActive=on; }
GuestImportDispatchResult GuestImportRegistry::dispatchUnderRegistryLock(
        std::string_view importName,
        GuestImportCall& call) {
    // v42: every import's R section is accounted per import name (RENDER_DIAG_REGISTRY_HOLD).
    // imports-fast: which hot import this is (0 while the switch is off: every statement below then runs as in v414).
    const unsigned importsFastHot = ::recomp::imports_fast::on() ? ::recomp::imports_fast::hotImport(importName) : 0u;
    // imports-fast A: the hot imports' section skips the hold accounting (RegistryUniqueQuiet: same R acquisition).
    std::optional<RegistryUnique> lockSectionAccounted;
    std::optional<RegistryUniqueQuiet> lockSectionQuiet;
    static RegistryLockSite importsFastSites[8] = {{"__imp__NtWaitForSingleObjectEx", 0}, {"__imp__NtSetEvent", 0},
            {"__imp__NtReleaseSemaphore", 0}, {"__imp__NtClearEvent", 0}, {"__imp__RtlEnterCriticalSection", 0},
            {"__imp__RtlLeaveCriticalSection", 0}, {"__imp__KeTlsGetValue", 0}, {"__imp__KeDelayExecutionThread", 0}}; // (bit order)
    if (importsFastHot) lockSectionQuiet.emplace(registryThreadState(), this, importsFastSites[__builtin_ctz(importsFastHot)]);
    else lockSectionAccounted.emplace(this, importName);
    auto& lock = importsFastHot ? lockSectionQuiet->native() : lockSectionAccounted->native();
    if (addressSpace_ == nullptr || !addressSpace_->isReserved()) {
        return {
                GuestImportDispatchState::RegistryUnavailable,
                std::string(importName),
                "Guest import registry has no reserved address space",
        };
    }
    // v282: the scene ISR (sub_836B1768 source 1 + callback sub_836BE1C8, replayed as v279 does) applied in ONE registry-lock
    // section with no world stop, for a caller that serializes the native packets itself (the renderer gate). Arguments:
    // device, context. returnValue: 0 applied, 1 busy (the raised-IRQL lock +11032 held by a guest thread: nothing written;
    // the console's ISR spins until that release: retry), 2 refused (a precondition of the scene scope's entry does not hold: nothing written), 3 failed
    // after a write (the KeSetEvent dispatch refused; must not happen after the checks). Preconditions are the scope entry's
    // and validation's except those that describe the stopped world or the worker (the whole-device compare, the worker's
    // identity and PCR): no other scope open; registration 0x836B1768 / userData device, exact functions; device buffer and
    // allocation; scratch = *(device+11028) aligned in the physical arena, mask 4, callback 0x836BE1C8, argument = context;
    // CPU2 slot root / cpu / empty queue / unsignalled event; scratch and context physical ranges; no critical section or
    // dispatcher wait over +11032, the event or the scratch; the device root. Guest threads acquire +11032 only through
    // this registry (under R), so a free word stays free for this section; the queue, event and mask words are then
    // written exactly as the ISR writes them, and read back.
    if (importName.size() == 23 && importName == "__recomp_v282_scene_irq") {
        const auto device = call.argument32(0), context = call.argument32(1);
        constexpr unsigned Slot = 11576 + 2 * 108;
        const auto answer = [&](std::uint64_t code, const char* why) { call.returnValue = code; return handled(importName, why); };
        if (nativeTileSceneInterruptScope_ || nativeInterruptScope_ || nativeTileLockScope_) return answer(2, "scope-active");
        if (!virtualCpuAffinityBound_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
            !nativeInterruptDeviceRoot_ || !nativeEventWakeBoostHandler_ || !graphicsInterruptRegistration_ || !exactFunctionValidator_ ||
            graphicsInterruptRegistration_->routine != 0x836B1768 || graphicsInterruptRegistration_->userData != device ||
            !exactFunctionValidator_(0x836B1768) || !exactFunctionValidator_(0x836BE1C8)) return answer(2, "registration");
        if (!device || device % 4 || !validGuestDataBuffer(device, 21944, true) || context < 0xC0000004 || context >= 0xE0000000 || context % 4)
            return answer(2, "device-context");
        std::uint32_t scratch = 0, ack = 1, ordinary = 1, header = 0, signal = 1, root = 0, cpu = 0, q = 0, read = 0, mask = 0, callback = 0, argument = 0;
        if (!readU32(device + 11028, scratch) || scratch % 4 || scratch < physicalArenaBase_) return answer(2, "scratch");
        // (the ISR takes +11032 only; the ordinary tile lock +11156 is a DISPATCH_LEVEL lock the scene callback never takes and
        // that does not mask the CPU2 interrupt, v189: its state does not matter here)
        (void)ordinary;
        if (!readU32(device + 11032, ack)) return answer(2, "locks");
        if (ack) return answer(1, "busy");
        if (!readDispatcherEvent(device + Slot + 60, header, signal) || (header >> 24) != 0 || signal) return answer(2, "slot-event");
        if (!readU32(device + Slot, root) || root != device + 11080 || !readU32(device + Slot + 4, cpu) || cpu != 2) return answer(2, "slot");
        if (!readU32(device + Slot + 84, q) || !readU32(device + Slot + 88, read) || q != read) return answer(2, "slot-queue");
        const auto physical = std::uint64_t{physicalAddressOffset_} + scratch - physicalArenaBase_;
        if (physical + 32 > 0x20000000) return answer(2, "physical");
        const auto scratchRange = resolvePhysicalRange(static_cast<unsigned>(physical), 32, true);
        const auto contextRange = resolvePhysicalRange(context - 0xC0000000, 4, false);
        if (!scratchRange || !contextRange || scratchRange->virtualAddress != scratch ||
            (std::uint64_t{context - 0xC0000000} < physical + 32 && physical < std::uint64_t{context - 0xC0000000} + 4)) return answer(2, "ranges");
        if (!readU32(scratch, mask) || mask != 4 || !readU32(scratch + 16, callback) || callback != 0x836BE1C8 ||
            !readU32(scratch + 20, argument) || argument != context) return answer(2, "scratch-words");
        if (hasActiveCriticalSectionInRange(device + 11032, 4) || hasDispatcherWaitInRange(device + 11032, 4) ||
            hasActiveCriticalSectionInRange(device + Slot + 60, 16) || hasActiveCriticalSectionInRange(device + Slot + 84, 24) ||
            hasDispatcherWaitInRange(device + Slot + 84, 24) || hasActiveCriticalSectionInRange(scratch, 32) || hasDispatcherWaitInRange(scratch, 32))
            return answer(2, "section-or-wait");
        bool rootOk = false; try { rootOk = nativeInterruptDeviceRoot_(device); } catch (...) {}
        if (!rootOk) return answer(2, "device-root");
        // the callback: queue entry (q&3), q+1, KeSetEvent(slot+60, 1, 0)
        if (!writeU32(device + Slot + 92 + (q & 3) * 4, context) || !writeU32(device + Slot + 84, q + 1)) return answer(3, "queue-write");
        GuestImportCall set; set.arguments[0] = device + Slot + 60; set.arguments[1] = 1; set.arguments[2] = 0;
        if (!dispatchKeEvent(set, true).handled()) return answer(3, "set-event");
        // the ISR after the callback: the CPU2 bit of the mask cleared under the (free, see above) raised-IRQL lock
#if !defined(RECOMP_V282_MUTANT_KEEP_MASK)
        if (!writeU32(scratch, (mask & ~4u) & 0x3Fu)) return answer(3, "mask-write");
#endif
        std::uint32_t entry = 0, q1 = 0, m1 = 1, ev = 0, lock1 = 1;
        if (!readU32(device + Slot + 92 + (q & 3) * 4, entry) || entry != context || !readU32(device + Slot + 84, q1) || q1 != q + 1 ||
            !readU32(scratch, m1) || m1 != 0 || !readU32(device + Slot + 60 + 4, ev) || !readU32(device + 11032, lock1) || lock1)
            return answer(3, "read-back");
        (void)ev; // (an auto-reset event may already be consumed by the waiter the set released)
        nativeTileSceneInterruptStatus_ = {true, true, true, false, RECOMP_CURRENT_GUEST_TOKEN, device, 1, 1, false};
        nativeSceneLastInline = true; // (a later worker creation accepts this balanced status, v279)
        return answer(0, "applied");
    }
    // v160 DIAGNOSTIC: which created-thread storage an address lies in. The 2026-09-27 Franklin-and-Lamar stop was an
    // NtReadFile with r1=6408d1d0 (by the pool layout, inside thread 6408e000's PCR page) and a buffer 641e0000 (by the
    // same layout, in the stack of thread 641db000, which had called ExTerminateThread 2.4 h earlier).
    const auto v160DescribeArena = [&](std::uint32_t address) -> std::string {
        if (!threadStorage_.ready() || !overlapsThreadArena(address, 1)) return "outside-arena";
        for (const auto& [token, object] : threadObjects_) {
            const auto prepared = threadStorage_.lookup(object.threadId);
            if (!prepared) continue;
            const auto& a = prepared->allocation; const auto& l = prepared->layout;
            if (address < a.base || address >= std::uint64_t{a.base} + a.size) continue;
            const char* part = address < l.threadAddress ? "pcr" : address < l.tlsAddress ? "kthread"
                    : address < l.stackBase ? "tls" : "stack";
            char s[200];
            std::snprintf(s, sizeof s, "%s-of-%08x(id=%u,%s%s%s,stack=%08x+%x,alloc=%08x+%x)", part, token, object.threadId,
                    token == RECOMP_CURRENT_GUEST_TOKEN ? "current," : "", object.returned ? "returned" : "live",
                    object.suspendCount ? ",suspended" : "", l.stackBase, l.stackSize, a.base, a.size);
            return s;
        }
        return "unallocated-arena";
    };
    // v160 DIAGNOSTIC: the PPC back chain from the caller's r1 (saved LR at back_chain-8, as every prologue stores it).
    const auto v160Frames = [&](std::uint32_t sp) -> std::string {
        std::string out; char s[16];
        for (int i = 0; i < 10; ++i) {
            std::uint32_t back = 0, lr = 0;
            if (!sp || !contains(sp, 4) || !readU32(sp, back) || back <= sp || !contains(back - 8, 4) || !readU32(back - 8, lr)) break;
            std::snprintf(s, sizeof s, "%s%08x", out.empty() ? "" : ",", lr); out += s; sp = back;
        }
        return out;
    };
    // v160 DIAGNOSTIC: a created thread calling an import with r1 outside its own pool stack (overflow, a stack switch,
    // or a layout mismatch). First 4 per thread, then powers of two.
    if (!importsFastHot && threadStorage_.ready() && call.stackPointer != 0) { // (imports-fast C: not for the hot imports)
        struct Range { std::uint32_t id, base, size, minSp; unsigned level; };
        static std::unordered_map<std::uint32_t, Range> v160Stacks;
        static std::unordered_map<std::uint32_t, unsigned> v160Reported;
        const auto token = RECOMP_CURRENT_GUEST_TOKEN;
        if (const auto* object = findThreadObject(token); object && !object->initial) {
            auto found = v160Stacks.find(token);
            if (found == v160Stacks.end() || found->second.id != object->threadId) {
                if (const auto prepared = threadStorage_.lookup(object->threadId))
                    found = v160Stacks.insert_or_assign(token, Range{object->threadId, prepared->layout.stackBase,
                            prepared->layout.stackSize, prepared->layout.stackBase + prepared->layout.stackSize, 0}).first;
                else found = v160Stacks.end();
            }
            if (found != v160Stacks.end()) {
                const auto sp = call.stackPointer; const auto r = found->second;
                // v160 DIAGNOSTIC: stack high-water mark at imports (a lower bound on the real depth), logged once at
                // 50/75/90/100% of the pool stack, with the chain at that depth.
                if (sp >= r.base && sp < found->second.minSp) {
                    found->second.minSp = sp;
                    const auto used = std::uint64_t{r.base} + r.size - sp;
                    const unsigned level = used * 10 >= std::uint64_t{r.size} * 9 ? 3 : used * 4 >= std::uint64_t{r.size} * 3 ? 2
                            : used * 2 >= r.size ? 1 : 0;
                    if (level > found->second.level) {
                        found->second.level = level;
                        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
                                "RENDER_DIAG_STACK_HIGH token=%08x id=%u import=%.*s used=%llx/%x pct=%llu sp=%08x frames=%s",
                                token, r.id, int(importName.size()), importName.data(), static_cast<unsigned long long>(used), r.size,
                                static_cast<unsigned long long>(used * 100 / r.size), sp, v160Frames(sp).c_str());
                    }
                }
                if (sp < r.base || sp >= std::uint64_t{r.base} + r.size) {
                    auto& n = v160Reported[token]; ++n;
                    if (n <= 4 || !(n & (n - 1)))
                        __android_log_print(ANDROID_LOG_WARN, "RecompRenderer",
                                "RENDER_DIAG_SP_OUTSIDE_STACK token=%08x id=%u import=%.*s sp=%08x stack=%08x+%x sp_in=%s frames=%s n=%u",
                                token, r.id, int(importName.size()), importName.data(), sp, r.base, r.size,
                                v160DescribeArena(sp).c_str(), v160Frames(sp).c_str(), n);
                }
            }
        }
    }
    if (importName=="__imp__KfAcquireSpinLock" || importName=="__imp__KfReleaseSpinLock" ||
        importName=="__imp__KeAcquireSpinLockAtRaisedIrql" || importName=="__imp__KeReleaseSpinLockFromRaisedIrql") {
        // v193: a scope's routing applies to its own worker only (with a stopped world no other thread runs; a
        // scoped tile-IRQ span, diag-exec-irq-scoped, leaves unrelated threads running).
        if(nativeTileSceneInterruptScope_ && RECOMP_CURRENT_GUEST_TOKEN==nativeTileSceneInterruptScope_->worker)
            return dispatchNativeTileSceneInterrupt(importName,call);
        if(nativeTileLockScope_ && RECOMP_CURRENT_GUEST_TOKEN==nativeTileLockScope_->worker)return dispatchNativeTileSpinLock(importName,call);
        if(!(nativeInterruptScope_ && RECOMP_CURRENT_GUEST_TOKEN==nativeInterruptScope_->worker) || !validateNativeInterruptScopeLocked()) { // (v193)
            // DIAGNOSTIC SKIP BUILD ONLY: plain spinlock outside every modelled scope.
            const bool acquire=importName=="__imp__KfAcquireSpinLock" || importName=="__imp__KeAcquireSpinLockAtRaisedIrql";
            const bool raises=importName=="__imp__KfAcquireSpinLock" || importName=="__imp__KfReleaseSpinLock";
            const auto address=call.argument32(0);std::uint32_t word=0;
            // v138: guest domains run concurrently, so an acquire can meet a lock another guest thread holds (word 1;
            // v136 play stopped here after 26 min: 1004f894). The hardware acquire spins until the holder's release;
            // here the acquirer parks cooperatively (the holder may be parked in this very domain) until the word is 0,
            // then takes it under the registry lock. Releases wake the waiters. Unowned releases, a thread already
            // waiting, cancellation and any other word value stay refused. The waiter list is local to this dispatcher
            // (the shared header fixes the registry's members) and only touched under the registry lock.
            static std::map<std::uint32_t,std::vector<std::uint32_t>> plainSpinWaiters;
            if(!readU32(address,word) || (acquire?word!=0:word!=1)) {
                const auto token=RECOMP_CURRENT_GUEST_TOKEN;
                if(!(acquire && word==1 && threadWaitHandler_ && token!=0 && validGuestDataBuffer(address,4,true)))
                    return rejected(importName,"DIAGNOSTIC plain spinlock contention or unowned release; no spin");
                auto& waiting=plainSpinWaiters[address];
                if(std::find(waiting.begin(),waiting.end(),token)!=waiting.end())
                    return rejected(importName,"DIAGNOSTIC plain spinlock: this thread is already waiting");
                waiting.push_back(token);
                const auto leave=[&]{
                    const auto f=plainSpinWaiters.find(address);if(f==plainSpinWaiters.end())return;
                    f->second.erase(std::remove(f->second.begin(),f->second.end(),token),f->second.end());
                    if(f->second.empty())plainSpinWaiters.erase(f);
                };
                const auto handler=threadWaitHandler_;
                try {
                    for(unsigned attempt=0;attempt<4096;++attempt) {
                        bool completed=false;
                        {
                            [[maybe_unused]] ParkedRegistryWait parkedWait(this,0,token);
                            RECOMP_REGISTRY_UNLOCK(lock);
                            RegistryWakes::flushActive(); // PARALLEL: never park holding undelivered wakes.
                            const ThreadWaitReady ready=[&]{
                                RECOMP_REGISTRY_LOCK(pollLock);
                                std::uint32_t w=1;return !readU32(address,w) || w==0;
                            };
                            TimelineWaitObject timelineObject(address); // v195
                            completed=handler(ready);
                            RECOMP_REGISTRY_RELOCK(lock);
                        }
                        std::uint32_t now=1;
                        if(!completed || RECOMP_CURRENT_GUEST_TOKEN!=token || !readU32(address,now)) {
                            leave();return rejected(importName,"DIAGNOSTIC plain spinlock wait cancelled");
                        }
                        if(now==0) {
                            (void)writeU32(address,1);leave();
                            plainSpinOwnersV188[address]={token,++plainSpinAcquirersV188[token]}; // v188
                            if(raises)call.returnValue=0;
                            return handled(importName,"DIAGNOSTIC plain spinlock acquired after its concurrent holder released it");
                        }
                        if(now!=1){leave();return rejected(importName,"DIAGNOSTIC plain spinlock word is neither free nor held");}
                    }
                    leave();return rejected(importName,"DIAGNOSTIC plain spinlock still held after 4096 waits");
                } catch(...) {
                    if(!lock.owns_lock())RECOMP_REGISTRY_RELOCK(lock);
                    leave();throw;
                }
            }
            (void)writeU32(address,acquire?1:0);
            { // v188 DIAGNOSTIC owner tracking
                const auto token=RECOMP_CURRENT_GUEST_TOKEN;
                if(acquire) {
                    const auto n=++plainSpinAcquirersV188[token];plainSpinOwnersV188[address]={token,n};
                    if((address&0xFFFFu)==((0x1004cd00u+11156u)&0xFFFFu) && (plainSpinAcquirersV188.size()<=16 && n<=2 || !(n&4095)))
                        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PLAIN_SPIN address=%08x token=%08x n=%llu raises=%d",
                            address,token,(unsigned long long)n,int(raises));
                } else plainSpinOwnersV188.erase(address);
            }
#if RECOMP_PARALLEL_REGISTRY
            if(!acquire)if(const auto f=plainSpinWaiters.find(address);f!=plainSpinWaiters.end())
                for(const auto t:f->second)RegistryWakes::wake(t); // PARALLEL: delivered after the unlock.
#endif
            if(acquire && raises)call.returnValue=0;
            return handled(importName,"DIAGNOSTIC plain spinlock outside the modelled interrupt scope");
        }
        return dispatchNativeInterruptSpinLock(importName,call);
    }
    // imports-fast B: guest_saves_dispatch.inc handles none of the hot N-imports (NtWaitForSingleObjectEx, NtSetEvent,
    // NtReleaseSemaphore, NtClearEvent: every statement there is gated by another name); for them its only effect is
    // savesStateFor's one-time creation of the state, which this call cannot trigger when it is not eligible or the state
    // was already settled for this registry. Then the gate (a static mutex and a std::map lookup under R) is skipped.
    const bool importsFastSkipSaves = (importsFastHot & ::recomp::imports_fast::HotHandleImports) != 0 &&
            (!(nativeXamAddress_ != 0 && signedOutUsersBound_) || savesStateSettled(this));
#include "guest_saves_dispatch.inc" // v142: local profile + saved games (returns only for what it handles)
    if(nativeTileSceneInterruptScope_ && RECOMP_CURRENT_GUEST_TOKEN==nativeTileSceneInterruptScope_->worker) { // (v193)
        if(importName!="__imp__KeGetCurrentProcessType" || !validateNativeTileSceneInterruptScopeLocked())
            return dispatchNativeTileSceneInterrupt(importName,call);
    }
    if(nativeTileLockScope_ && RECOMP_CURRENT_GUEST_TOKEN==nativeTileLockScope_->worker && // (v193)
        (nativeTileLockScope_->failed || !validateNativeTileLockScopeLocked() ||
        (importName!="__imp__KeGetCurrentProcessType" && importName!="__imp__KeQueryPerformanceFrequency"))) {
        nativeTileLockScope_->failed=true;
        return rejected(importName,"Import is outside the ordinary native tile-lock scope; no wait or allocation admitted");
    }
    if (nativeInterruptScope_ && RECOMP_CURRENT_GUEST_TOKEN==nativeInterruptScope_->worker && (!validateNativeInterruptScopeLocked() || // (v193)
        (importName!="__imp__KeGetCurrentProcessType" && importName!="__imp__KeQueryPerformanceFrequency")))
        return rejected(importName,"Import is outside the bounded native command-interrupt profile");
    if (importName=="__imp__ExTerminateThread") {
        const auto* thread=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
        if (!threadExitHandler_ || !thread || thread->returned || thread->suspendCount)
            return rejected(importName,"Thread exit requires a live current thread and a native unwind handler");
        auto handler=threadExitHandler_;
        const auto token=RECOMP_CURRENT_GUEST_TOKEN,code=call.argument32(0);
        RECOMP_REGISTRY_UNLOCK(lock);
        RegistryWakes::flushActive(); // PARALLEL: the handler unwinds this task.
        (void)handler(token,code);
        return rejected(importName,"Native thread-exit handler rejected or returned without unwinding");
    }
    if (importName == "__imp__VdSetDisplayMode" || importName == "__imp__VdGetCurrentDisplayInformation") {
        if (!nativeSwapConsumer_)
            return {GuestImportDispatchState::NotRegistered,std::string(importName),"Native window output contract is not bound"};
        if (!virtualVideoModeBound_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
            !pendingNativeSwap_ || !nativeSystemContext_ || RECOMP_CURRENT_GUEST_TOKEN!=nativeSystemContextThread_ ||
            !graphicsRingConfiguration_ || !graphicsRingConfiguration_->writeBack ||
            graphicsRingConfiguration_->readWord!=pendingNativeSwap_->command.identity().previousReadWord)
            return rejected(importName,"Fixed native display mode requires the current pending owned-window transaction");
        std::uint32_t read=0;
        if (!readU32(graphicsRingConfiguration_->writeBack->virtualAddress,read) || read!=graphicsRingConfiguration_->readWord)
            return rejected(importName,"Native display configuration has a stale ring writeback");
        if (importName == "__imp__VdSetDisplayMode") {
            if (call.argument32(0)!=0x40000000 || nativeDisplayModeStatus_.selected)
                return rejected(importName,"Only one fixed RGB8 non-SD native display mode selection is supported");
            auto result=handled(importName,"Selected the configured logical720p60 RGB8 native output; no physical display change or completion");
            if (!RECOMP_V_BACKEND()->selectNativeDisplayMode(call.argument32(0)))
                return rejected(importName,"Native backend rejected display-mode selection");
            nativeDisplayModeStatus_={true,call.argument32(0),0};
            return result; // Observed caller ignores return; preserve r3.
        }
        if (!nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries)
            return rejected(importName,"One information readback requires a selected native output mode");
        const auto sp=std::uint64_t{call.stackPointer};const auto output=call.argument32(0);
        // Exact nested240byte frame observed at836B9E80, below the live swap
        // caller's480byte frame. Never allow aliases with commands/heap/image.
        if (!sp || (sp&15) || sp+240!=nativeSystemContext_->stackPointer || sp+128!=output ||
            !validGuestDataBuffer(call.stackPointer,240,true,16) || imageProtections_.overlaps(call.stackPointer,240) ||
            (sp<std::uint64_t{heapBase_}+heapSize_ && sp+240>heapBase_) ||
            (physicalMemoryArenaBound_ && sp<std::uint64_t{physicalArenaBase_}+physicalArenaSize_ && sp+240>physicalArenaBase_) ||
            hasActiveCriticalSectionInRange(call.stackPointer,240))
            return memoryFault(importName,"Display information does not fit the owned nested caller frame");
        const auto* thread=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
        if (!thread || thread->returned || thread->suspendCount)
            return rejected(importName,"Display information requires a live current guest thread");
        if (!thread->initial) {const auto owner=threadStorage_.lookup(thread->threadId);
            if (!owner || sp<owner->layout.stackBase || sp+240>std::uint64_t{owner->layout.stackBase}+owner->layout.stackSize)
                return memoryFault(importName,"Display information belongs outside the current worker stack");
        }
        const auto aliases=[&](std::uint32_t address,std::uint32_t size){
            return std::uint64_t{output}<std::uint64_t{address}+size && std::uint64_t{address}<std::uint64_t{output}+88;
        };
        { CsAll csAllGuard; for(const auto& entry:criticalSections_)if(aliases(entry.first,CriticalSectionSize))
            return memoryFault(importName,"Display information aliases a tracked synchronization object"); } // (v197)
        for(const auto& entry:dispatcherEventWaits_)if(aliases(entry.first,16))
            return memoryFault(importName,"Display information aliases an active event wait");
        NativeDisplayConfiguration configuration;std::array<std::uint8_t,88> bytes{};
        if (!graphicsBackend_->querySelectedDisplayConfiguration(configuration) || !makeNativeDisplayInformation(configuration,bytes))
            return rejected(importName,"Native backend cannot read back the selected logical display configuration");
        auto result=handled(importName,"Wrote native logical display information; not measured physical timing or scaler completion");
        std::memcpy(addressSpace_->translate(output),bytes.data(),bytes.size());
        ++nativeDisplayModeStatus_.queries;return result;
    }
    if (importName == "__imp__VdSwap") {
        if (!nativeSwapConsumer_)
            return {GuestImportDispatchState::NotRegistered,std::string(importName),"Native swap consumer is not bound"};
        // Allocate result strings before the producer's first guest store.
        auto result=handled(importName,"Issued owned native swap work; presentation and retirement remain pending");
        if (!issueExperimentalNativeSwapCommandLocked(call))
            return rejected(importName,"Native swap request failed exact ownership, continuity or one-shot validation");
        return result; // Void ABI: preserve r3, no completion value invented.
    }
    if (importName == "__imp__VdInitializeScalerCommandBuffer") return dispatchNativeIdentityScaler(call);
    if (importName == "__imp__VdPersistDisplay") {
        // Native policy: no persisted display buffer is available. The observed
        // caller takes a Boolean false branch without reading/freeing a buffer.
        // This is NOT a successful persistence operation or GPU acknowledgement.
        if (!nativeSwapConsumer_)
            return {GuestImportDispatchState::NotRegistered,std::string(importName),"Native display consumer is not bound"};
        if (!completedDisplayGamma_ || !validateNativeScalerStorageLocked() || !validateNativeScanoutLocked())
            return rejected(importName,"No completed native display update for this caller profile");
        const auto sp=std::uint64_t{call.stackPointer};
        const auto input=call.argument32(0),output=call.argument32(1);
        const auto* thread=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
        if (!thread || thread->initial || thread->returned || thread->suspendCount)
            return rejected(importName,"Persisted-display query requires a live owned worker stack");
        const auto owner=threadStorage_.lookup(thread->threadId);
        if (!owner || sp<owner->layout.stackBase || sp+196>std::uint64_t{owner->layout.stackBase}+owner->layout.stackSize ||
            !sp || (sp&15) || sp+196>GuestAddressSpace::Size || input!=sp+184 || output!=sp+160 ||
            !validGuestDataBuffer(call.stackPointer,196,true,16) || imageProtections_.overlaps(call.stackPointer,196) ||
            (sp<std::uint64_t{heapBase_}+heapSize_ && sp+196>heapBase_) ||
            (physicalMemoryArenaBound_ && sp<std::uint64_t{physicalArenaBase_}+physicalArenaSize_ && sp+196>physicalArenaBase_))
            return memoryFault(importName,"Persisted-display query must use the observed independent caller stack layout");
        { CsAll csAllGuard; for (const auto& cs:criticalSections_)
            if (sp<std::uint64_t{cs.first}+28 && cs.first<sp+196)
                return memoryFault(importName,"Persisted-display caller frame aliases a synchronization object"); } // (v197)
        if (hasDispatcherWaitInRange(call.stackPointer,196))
            return memoryFault(importName,"Persisted-display caller frame aliases an event wait");
        for (unsigned i=0;i<3;++i) {std::uint32_t word=0;
            if (!readU32(input+i*4,word) || word)
                return rejected(importName,"Only the observed empty persisted-display request is supported");
        }
        auto result=handled(importName,"No persisted display buffer available: Boolean false and null output, no GPU or allocation work");
        (void)writeU32(output,0);call.returnValue=0;return result;
    }
    if (importName == "__imp__KeInitializeDpc") return dispatchKeInitializeDpc(call);
    if (importName == "__imp__VdRetrainEDRAMWorker" || importName == "__imp__VdRetrainEDRAM" ||
        importName == "__imp__VdIsHSIOTrainingSucceeded")
        return dispatchNativeEdramTraining(importName,call);
    if (importName == "__imp__KeEnterCriticalRegion" || importName == "__imp__KeLeaveCriticalRegion") {
        auto* thread = findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
        if (!thread || thread->returned || thread->suspendCount)
            return rejected(importName,"Critical region requires the live running current thread object");
        const bool enter = importName == "__imp__KeEnterCriticalRegion";
        if ((enter && thread->criticalRegionDepth == 65535) || (!enter && !thread->criticalRegionDepth))
            return rejected(importName,"Unbalanced or out-of-range native critical-region nesting");
        if (enter) ++thread->criticalRegionDepth;
        else --thread->criticalRegionDepth;
        if (!thread->criticalRegionDepth && edramPreparedThread_ == RECOMP_CURRENT_GUEST_TOKEN) edramPreparedThread_ = 0;
        // Normal kernel APC delivery is deferred until the final leave, not
        // thread scheduling, special APCs, interrupts or mutual exclusion.
        // All APC insertion/delivery paths are still guarded: the supported
        // queue is empty. Any future APC implementation MUST consult this
        // per-thread state. Do not copy Xenia's host-endian private KTHREAD
        // counter into guest bytes or invent an Xbox IRQL/layout contract.
        return handled(importName,"Updated per-thread normal-APC deferral with an empty supported APC queue");
    }
    if (importName == "__imp__VdCallGraphicsNotificationRoutines") return dispatchGraphicsDisplayNotification(call);
    if (importName == "__imp__KeSetEvent") return dispatchKeEvent(call, true);
    if (importName == "__imp__KeResetEvent") return dispatchKeEvent(call, false);
    if (importName == "__imp__KeWaitForSingleObject") return dispatchKeWaitForSingleObject(call, lock);
    if (importName == "__imp__KeDelayExecutionThread") return dispatchKeDelayExecutionThread(call, lock);
    if (importName == "__imp__KeWaitForMultipleObjects") return dispatchKeWaitForMultipleObjects(call, lock);
    if (importName == "__imp__XAudioRegisterRenderDriverClient") return dispatchAudioRegister(call);
    if (importName == "__imp__XAudioSubmitRenderDriverFrame") return dispatchAudioSubmit(call);
    if (importName == "__imp__XAudioEnableDucker") {
        const auto config = audioBackend_ ? audioBackend_->configuration() : std::nullopt;
        if (call.argument32(0) != 0 || nativeCategoryOneGainBits_ != 0x3F800000U ||
            !audioRenderClient_ || !config || !validNativeAudioConfiguration(*config) ||
            audioRenderClient_->statistics().failed)
            return rejected(importName, "Ducker disable requires the live, unducked native unity-gain renderer; enabling is unsupported");
        // The native single-title PCM path has no system-music attenuation.
        // Disabling ducking is therefore idempotent and leaves samples/gain
        // unchanged. This is not Xenia's unconditional-success enable stub.
        call.returnValue = 0;
        return handled(importName, "Disabled ducking on the native unducked PCM path");
    }
    if (importName == "__imp__XAudioGetVoiceCategoryVolume") {
        const auto c=audioBackend_?audioBackend_->configuration():std::nullopt;
        if (!nativeCategoryOneGainBits_ || !audioRenderClient_ || !c || !validNativeAudioConfiguration(*c) ||
            audioRenderClient_->statistics().failed || call.argument32(0)!=1 || nativeCategoryOneGainQueries_==UINT64_MAX)
            return rejected(importName,"Only explicitly bound native category1 unity gain is supported");
        const auto output=call.argument32(1);
        if (!validGuestDataBuffer(output,4,true)) return memoryFault(importName,"Category gain output is not bounded writable guest storage");
        auto result=handled(importName,"Reported immutable native category1 gain; title consumes it without host reapplication");
        (void)writeU32(output,*nativeCategoryOneGainBits_);++nativeCategoryOneGainQueries_;call.returnValue=0;
        return result;
    }
    if (importName == "__imp__XAudioGetSpeakerConfig") {
        const auto c=audioBackend_?audioBackend_->configuration():std::nullopt;
        if (!c || !validNativeAudioConfiguration(*c) || audioSpeakerQueries_==UINT32_MAX)
            return rejected(importName,"Experimental speaker query requires a live native output stream");
        const auto output=call.argument32(0);
        if (!output || (output&3) || !contains(output,4) ||
            (output<0x7FC90000 && std::uint64_t{output}+4>0x7FC80000) ||
            overlapsProtectedThreadStorage(output,4) || overlapsThreadTlsStorage(output,4) ||
            (imageProtections_.overlaps(output,4) && !imageProtections_.writable(output,4)) ||
            (physicalMemoryArenaBound_ && output<std::uint64_t{physicalArenaBase_}+physicalArenaSize_ &&
                std::uint64_t{output}+4>physicalArenaBase_ && (output<physicalArenaBase_ ||
                    !resolvePhysicalRange(physicalAddressOffset_+(output-physicalArenaBase_),4,true))))
            return memoryFault(importName,"Speaker word output is not bounded writable guest storage");
        if (output<std::uint64_t{heapBase_}+heapSize_ && std::uint64_t{output}+4>heapBase_) {
            const auto after=allocations_.upper_bound(output);
            if (output<heapBase_ || after==allocations_.begin() ||
                std::uint64_t{output}+4>std::uint64_t{std::prev(after)->first}+std::prev(after)->second)
                return memoryFault(importName,"Speaker word output is not owned virtual storage");
            const auto page=(output-heapBase_)/PageSize;
            if (!(virtualCommitBits_[page/64]&(std::uint64_t{1}<<(page%64))))
                return memoryFault(importName,"Speaker word output is in an uncommitted virtual page");
        }
        auto result=handled(importName,"Experimental reference configuration for an open native audio stream; no playback");
        (void)writeU32(output,0x00010001); // Pinned Xenia contract, not measured console settings.
        ++audioSpeakerQueries_;call.returnValue=0;
        return result;
    }
    if (importName == "__imp__DbgPrint") {
        return dispatchDbgPrint(call);
    }
    if (importName == "__imp__ExRegisterTitleTerminateNotification") {
        return dispatchTitleTerminationRegistration(call);
    }
    if (importName == "__imp__VdInitializeRingBuffer" || importName == "__imp__VdEnableRingBufferRPtrWriteBack") {
        return dispatchGraphicsRing(call, importName == "__imp__VdEnableRingBufferRPtrWriteBack");
    }
    if (importName == "__imp__KiApcNormalRoutineNop") {
        const auto address = call.argument32(0);
        if (!graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
            !graphicsSystemIdentifier_ || graphicsMeInitVerified_ || call.argument32(1) != 19) {
            return rejected(importName, "Only the initial native graphics ME_INIT verification profile is modeled");
        }
        if (!call.stackPointer || (call.stackPointer & 15U) ||
            std::uint64_t{call.stackPointer} + 80 != address || !contains(address, 76) ||
            (address < 0x7FC90000 && std::uint64_t{address} + 76 > 0x7FC80000) ||
            overlapsProtectedThreadStorage(address, 76) || overlapsThreadTlsStorage(address, 76)) {
            return memoryFault(importName, "ME_INIT input does not fit the bounded observed stack-call profile");
        }
        std::array<std::uint32_t, 19> words{};
        for (std::uint32_t i = 0; i < words.size(); ++i) (void)readU32(address + i * 4, words[i]);
        if (!matchesNativeMeInitProfile(words)) return rejected(importName, "ME_INIT packet differs from the supported native profile");
        graphicsMeInitVerified_ = true;
        // Return is unused by this observed call. Preserve r3; no general APC
        // ABI or original kernel behavior is claimed. The packet stays intact.
        return handled(importName, "Validated native ME_INIT profile without consuming or executing its packet");
    }
    if (importName == "__imp__VdGetSystemCommandBuffer") {
        if(completedPostOutputWrappedSwap_){
            auto result=handled(importName,"Fresh no-overlay context after the second native swap; no new swap/copy/frame completion");
            if(!issueNativePostOutputSystemContextLocked(call,true))
                return rejected(importName,"Post-swap context requires current independent image lease and original owned caller");
            return result;
        }
        if(completedPostOutputColor_){
            auto result=handled(importName,"Issued a fresh owned no-overlay context after the post-output color batch; no GPU work or presentation");
            if(!issueNativePostOutputSystemContextLocked(call))return rejected(importName,"Post-output system context requires fresh copy, ring and caller ownership");
            return result; // Same observed void ABI; preserve r3 and original history.
        }
        if (!nativeNoSystemOverlayBound_ || nativeSystemContext_ || !completedOwnedColor_ ||
            !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
            !graphicsSystemIdentifier_ || !graphicsRingConfiguration_ || !graphicsRingConfiguration_->writeBack ||
            graphicsRingConfiguration_->readWord != completedOwnedColor_->readWord)
            return rejected(importName, "One empty native system context requires a completed owned color batch and explicit no-overlay policy");
        const auto sp=std::uint64_t{call.stackPointer};
        const auto buffer=call.argument32(0), identifier=call.argument32(1);
        // Observed 480-byte caller frame: 148-byte context at +208, word at
        // +116. No other guest ABI is admitted. In the thread pool the entire
        // frame must belong to the current resumed worker, not another thread.
        if (!sp || (sp&15) || sp+480>GuestAddressSpace::Size || sp+208!=buffer || sp+116!=identifier ||
            !validGuestDataBuffer(call.stackPointer,480,true,16) || imageProtections_.overlaps(call.stackPointer,480) ||
            (sp<std::uint64_t{heapBase_}+heapSize_ && sp+480>heapBase_) ||
            (physicalMemoryArenaBound_ && sp<std::uint64_t{physicalArenaBase_}+physicalArenaSize_ && sp+480>physicalArenaBase_) ||
            hasActiveCriticalSectionInRange(call.stackPointer,480))
            return memoryFault(importName, "System context outputs do not fit the bounded writable caller stack profile");
        std::uint32_t currentRead=0;
        if (!readU32(graphicsRingConfiguration_->writeBack->virtualAddress,currentRead) || currentRead!=completedOwnedColor_->readWord)
            return rejected(importName, "System context cannot be issued against a stale read pointer");
        auto result=handled(importName, "Issued an empty native no-system-overlay context; no swap, GPU identifier write or command retirement");
        // Native empty-system-work representation, NOT a retail structure
        // reconstruction or Xenia BEEF token. Caller sees no system surface,
        // scaling override or pending system identifier. VdSwap remains guarded
        // until a separate native consumer validates this context and presents.
        std::memset(addressSpace_->translate(buffer),0,148);
        (void)writeU32(identifier,0);
        nativeSystemContext_=NativeSystemContext{call.stackPointer,buffer,identifier,currentRead,1};
        nativeSystemContextThread_=RECOMP_CURRENT_GUEST_TOKEN;
        return result; // Void ABI: preserve r3.
    }
    if (importName == "__imp__VdSetSystemCommandBufferGpuIdentifierAddress") {
        if (!graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready()) {
            return rejected(importName, "System-command identifier requires live native graphics");
        }
        const auto address = call.argument32(0);
        if (!address) {
            graphicsSystemIdentifier_.reset(); graphicsIdentifierAllocation_.reset();
            return handled(importName, "Cleared system-command identifier registration without a completion write");
        }
        // Register an opaque address only. Four bytes establish minimum word
        // ownership/alignment, NOT a claim about the full identifier payload.
        // Its consumer and any future payload access require separate modeling.
        if (!graphicsRingConfiguration_ || !graphicsRingConfiguration_->writeBack || graphicsSystemIdentifier_ ||
            !physicalMemoryArenaBound_ || (address & 3U) || address < physicalArenaBase_ ||
            std::uint64_t{address} + 4 > std::uint64_t{physicalArenaBase_} + physicalArenaSize_) {
            return rejected(importName, "Unsupported opaque identifier lifecycle or virtual address");
        }
        const auto physical = static_cast<std::uint32_t>(std::uint64_t{physicalAddressOffset_} + address - physicalArenaBase_);
        const auto range = resolvePhysicalRange(physical, 4, true);
        const auto& ring = *graphicsRingConfiguration_;
        if (!range || (physical < std::uint64_t{ring.physicalAddress} + ring.bytes &&
            ring.physicalAddress < std::uint64_t{physical} + 4) ||
            physical == ring.writeBack->physicalAddress) {
            return rejected(importName, "Identifier address is unowned, read-only or aliases ring/writeback storage");
        }
        graphicsSystemIdentifier_ = address; graphicsIdentifierAllocation_ = range->allocationBase;
        return handled(importName, "Registered opaque system-command identifier address; payload/completion remains unimplemented");
    }
    if (importName == "__imp__VdSetGraphicsInterruptCallback") {
        if (!graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready()) {
            return rejected(importName, "Graphics interrupt registration requires a live native backend");
        }
        const auto routine = call.argument32(0), userData = call.argument32(1);
        if ((!routine && userData) || (routine && !graphicsCallbackValidator_(routine))) {
            return rejected(importName, "Unsupported null callback/context pair or missing exact guest callback");
        }
        if (routine) graphicsInterruptRegistration_ = GraphicsInterruptRegistration{routine, userData};
        else graphicsInterruptRegistration_.reset();
        // Configuration only. No callback is dispatched without an implemented
        // interrupt source and a serialized guest execution contract.
        return handled(importName, "Updated graphics interrupt callback metadata; no interrupt synthesized");
    }
    if (importName == "__imp__VdInitializeEngines") {
        if (!graphicsBackend_ || graphicsInitialized_) return rejected(importName, "Native graphics backend is absent or already initialized");
        // Bounded observed initialization profile. Firmware pointers are retained
        // as metadata: native command processing replaces GPU firmware execution.
        const auto inImage = [&](std::uint32_t address) {
            return (address & 3U) == 0 && address >= graphicsImageBase_ &&
                std::uint64_t{address} + 4 <= std::uint64_t{graphicsImageBase_} + graphicsImageBytes_;
        };
        if (call.argument32(0) != 0x53020000 || !call.argument32(1) || call.argument32(2) != 0 ||
            !graphicsCallbackValidator_(call.argument32(1)) || !inImage(call.argument32(3)) ||
            !inImage(call.argument32(4)) || call.argument32(3) == call.argument32(4)) {
            return rejected(importName, "Unsupported graphics initialization flags, context, callback or firmware pointer metadata");
        }
        if (!RECOMP_V_BACKEND()->initialize() || !graphicsBackend_->ready()) {
            return rejected(importName, "Native graphics resource/queue initialization failed");
        }
        for (std::size_t i = 0; i < graphicsConfiguration_.size(); ++i) graphicsConfiguration_[i] = call.argument32(i);
        RECOMP_V(graphicsInitialized_) = true;
        call.returnValue = 1;
        return handled(importName, "Initialized native graphics resources and verified queue operation; guest commands remain separately guarded");
    }
    if (importName == "__imp__XamGetSystemVersion") { return dispatchXamGetSystemVersion(call); }
    if(importName=="__imp__XamInputGetCapabilities" || importName=="__imp__XamInputSetState") {
        const bool caps=importName=="__imp__XamInputGetCapabilities";
        const auto output=call.argument32(2),flags=call.argument32(1);
        if(!output){call.returnValue=0xA0;return handled(importName,"Null controller buffer");}
        if(caps && (flags&0xFFU) && !(flags&1U)) {
            call.returnValue=0x48F;return handled(importName,"No non-gamepad native input device");
        }
        if(!nativeGamepadReader_) {
            if(!disconnectedInputBound_)return rejected(importName,"Native input is unbound");
            call.returnValue=0x48F;return handled(importName,"Explicit disconnected input");
        }
        auto user=call.argument32(0);
        if((user&0xFFU)==0xFFU || (caps && (flags&0x40000000U)))user=0;
        recomp::input::GamepadState supported;
        const auto connected=nativeGamepadCapabilities_(user,supported);
        if(connected!=0 && connected!=1)return rejected(importName,"Native controller capabilities query failed");
        if(!connected){call.returnValue=0x48F;return handled(importName,"Native controller disconnected");}
        if(!validGuestDataBuffer(output,caps?20:4,caps,caps?4:2))return memoryFault(importName,"Controller buffer is not accessible guest storage");
        if(caps) {
            std::array<std::uint8_t,20> packet{};packet[0]=1;packet[1]=1;
            const auto state=recomp::input::encodeGamepadState(supported);
            std::copy(state.begin()+4,state.end(),packet.begin()+4);
            std::memcpy(addressSpace_->translate(output),packet.data(),packet.size());
            call.returnValue=0;return handled(importName,"Actual Android input capabilities; no native force-feedback backend");
        }
        const auto* vibration=addressSpace_->translate(output);
        // No FFB is advertised. A zero command is already satisfied; a nonzero
        // command fails explicitly, never claims an unperformed motor action.
        call.returnValue=(vibration[0]|vibration[1]|vibration[2]|vibration[3])?50:0;
        return handled(importName,"Native force feedback is unsupported; zero output is quiescent");
    }
    if (importName == "__imp__XamInputGetState") {
        if(nativeGamepadReader_) {
            const auto flags=call.argument32(1),output=call.argument32(2);
            if((flags&0xFFU) && !(flags&1U)) {
                call.returnValue=0x48F;return handled(importName,"No non-gamepad native input device");
            }
            auto user=call.argument32(0);
            if((user&0xFFU)==0xFFU || (flags&0x40000000U))user=0;
            recomp::input::GamepadState state;
            const auto connected=nativeGamepadReader_(user,state);
            if(connected!=0 && connected!=1)return rejected(importName,"Native input snapshot failed");
            if(!connected) {call.returnValue=0x48F;return handled(importName,"Native controller is disconnected");}
            if(output) {
                if(!validGuestDataBuffer(output,16,true,4))return memoryFault(importName,"Input state output is not writable guest storage");
                const auto packet=recomp::input::encodeGamepadState(state);
                recomp::input::delivery::copyAndObserve(addressSpace_->translate(output),packet,user,output,state);
            }
            call.returnValue=0;return handled(importName,"Read actual native gamepad snapshot");
        }
        if (!disconnectedInputBound_) {
            return rejected(importName, "Input state requires an explicit device configuration");
        }
        // No guest devices are registered. Like an empty native input-driver
        // set, every user query returns disconnected without writing a state
        // packet (including the documented null-output presence query).
        call.returnValue = 0x48F;
        return handled(importName, "Explicit headless input configuration has no connected devices");
    }
    if (importName == "__imp__XamInputGetKeystrokeEx") {
        // Pinned Xenia xam_input.cc checks these two conditions BEFORE reading
        // the user-index pointer. Only the no-non-gamepad-device path is
        // implemented here; this is not an empty queue or a connected gamepad.
        if (!call.argument32(2)) {
            call.returnValue = 0xA0; // ERROR_BAD_ARGUMENTS
            return handled(importName, "Keystroke output pointer is null");
        }
        const auto flags = call.argument32(1);
        if ((flags & 0xFFU) != 0 && (flags & 1U) == 0) {
            call.returnValue = 0x48F; // ERROR_DEVICE_NOT_CONNECTED
            return handled(importName, "No non-gamepad input device; no guest output or user index changed");
        }
        return rejected(importName, "Gamepad or unfiltered keystroke query requires a native input backend");
    }
    if (importName == "__imp__ObCreateSymbolicLink" || importName == "__imp__ObDeleteSymbolicLink")
        return dispatchSymbolicLink(call, importName == "__imp__ObCreateSymbolicLink");
    if (importName == "__imp__NtCreateFile") {
        lastFileCreateRequest_.reset();
        std::uint32_t options = 0;
        if (!call.readArgument32(*addressSpace_, 8, options)) {
            return memoryFault(importName, "Ninth file-create argument is not in a bounded guest stack slot");
        }
        lastFileCreateRequest_ = FileCreateRequest{call.argument32(1), call.argument32(5),
                call.argument32(6), call.argument32(7), options};
        if (!fileMetadataQuery_ && contentMounts_.empty() && !noRawStorageDevicesBound_) { return rejected(importName, "No explicit file namespace is bound"); }
        // Read-only OPEN only. A present file requires an actual preopened source,
        // never a metadata-only handle or a path-based host create/write.
        const auto& request = *lastFileCreateRequest_;
        constexpr std::uint32_t ReadOnlyAccess = 0x801200A9U;
        const auto handle = call.argument32(0), object = call.argument32(2), iosb = call.argument32(3);
        std::uint32_t root = 0, descriptor = 0, flags = 0, pointer = 0;
        std::uint16_t length = 0, maximum = 0;
        if (handle == 0 || (handle & 3U) != 0 || !contains(handle, 4) || overlapsProtectedThreadStorage(handle, 4)
                || iosb == 0 || (iosb & 3U) != 0 || !contains(iosb, 8) || overlapsProtectedThreadStorage(iosb, 8)
                || object == 0 || (object & 3U) != 0 || !contains(object, 12)) {
            return memoryFault(importName, "File open request/output structures are not bounded");
        }
        (void)readU32(object, root); (void)readU32(object + 4, descriptor); (void)readU32(object + 8, flags);
        lastFileCreateRequest_->objectRoot=root;lastFileCreateRequest_->objectFlags=flags;
        if (descriptor == 0 || (descriptor & 3U) != 0 || !contains(descriptor, 8)) {
            return memoryFault(importName, "File open name descriptor is not bounded");
        }
        (void)readU16(descriptor, length); (void)readU16(descriptor + 2, maximum); (void)readU32(descriptor + 4, pointer);
        if (length == 0 || length > 1024 || length > maximum || pointer == 0 || !contains(pointer, length)) {
            return memoryFault(importName, "File open path bytes are not bounded");
        }
        const auto overlap = [](std::uint32_t a, std::uint32_t size, std::uint32_t b, std::uint32_t otherSize) {
            return a < std::uint64_t{b} + otherSize && b < std::uint64_t{a} + size;
        };
        if (overlap(handle, 4, iosb, 8)) { return rejected(importName, "File open outputs alias"); }
        for (const auto& input : std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{{
                {object, 12}, {descriptor, 8}, {pointer, length}, {call.stackPointer + 0x54U, 4}}}) {
            if (overlap(handle, 4, input.first, input.second) || overlap(iosb, 8, input.first, input.second)) {
                return rejected(importName, "File open outputs overlap an input");
            }
        }
        lastFileCreateRequest_->path.assign(
            reinterpret_cast<const char*>(addressSpace_->translate(pointer)),length);
        if ((root != 0 && root != 0xFFFFFFFD) || flags != 0x40) {
            return rejected(importName, "File open requires absolute case-insensitive object attributes");
        }
        if (isXboxRawPartition(request.path)) {
            if (!noRawStorageDevicesBound_) return rejected(importName, "Native raw-storage namespace is unbound");
            if (request.disposition != 1 || call.argument32(4) != 0 || request.share > 7
                    || (options & ~0x866U) != 0 || (options & 0x804U) == 0x804U || (options & 0x20U) == 0)
                return rejected(importName, "Raw storage supports non-alertable existing-device lookup only");
            // Existence is checked before access rights. There are no registered
            // Xbox raw devices, regardless of requested read/write access.
            // Xenia VFS OpenFile uses NO_SUCH_FILE / FILE_DOES_NOT_EXIST (5).
            lastFileCreateRequest_->lookup = 2;
            (void)writeU32(handle, 0xFFFFFFFFU);
            (void)writeU32(iosb, 0xC000000FU); (void)writeU32(iosb + 4, 5);
            call.returnValue = 0xC000000FU;
            return handled(importName, "Xbox raw partition is absent from the native namespace; no handle created");
        }
        const auto path = symbolicLinks_.resolve(lastFileCreateRequest_->path);
        if (!path) { return rejected(importName, "File open path syntax is outside the supported subset"); }
        lastFileCreateRequest_->path=*path;
        if ((request.access & ~ReadOnlyAccess) != 0 || (request.access & 0x80000081U) == 0
                || request.disposition != 1 || call.argument32(4) != 0
                || (request.attributes != 0 && request.attributes != 0x80)
                || request.share > 7 || (request.options & ~0x864U) != 0 || (request.options & 0x804U) == 0x804U
                || (request.options & 0x20) == 0) {
            return rejected(importName, "Only read-only non-alertable existing-file open is modeled");
        }
        lastFileCreateRequest_->mountedContent = contentPath(*path).first != 0;
        const auto result = queryFileMetadata(*path);
        lastFileCreateRequest_->lookup = result.state == GuestFileQueryState::Found ? 1
                : result.state == GuestFileQueryState::NotFound ? 2 : 3;
        if (result.state == GuestFileQueryState::Found) {
            if (result.metadata.attributes != 1) { return rejected(importName, "Only regular read-only content opens are supported"); }
            auto source = openFileSource(*path);
            if (!source || source->size() != result.metadata.values[5]) {
                return rejected(importName, "File content is not explicitly bound with matching metadata size");
            }
            auto success = handled(importName, "Opened a real explicitly bound read-only source");
            auto failed = handled(importName, "Bounded file handle storage is exhausted");
            std::uint32_t token = 0;
            try {
                token = createKernelHandle({KernelObjectKind::File, 1, 1});
                if (token != 0) {
                    const auto rootName = path->substr(0, path->find(':') + 1);
                    openFiles_.emplace(token, OpenFile{std::move(source), 0, result.metadata,
                            contentMounts_.count(rootName) ? rootName : std::string{}});
                }
            } catch (const std::bad_alloc&) {
                if (token != 0) { ::recomp::imports_fast::eraseKey(kernelHandles_, token, ::recomp::imports_fast::handleGenerations()); }
                token = 0;
            }
            if (token == 0) { call.returnValue = StatusNoMemory; return failed; }
            (void)writeU32(handle, token); (void)writeU32(iosb, 0); (void)writeU32(iosb + 4, 1); // FILE_OPENED.
            ++fileIoStatistics_.opens;
            call.returnValue = StatusSuccess;
            return success;
        }
        if (result.state != GuestFileQueryState::NotFound) {
            return rejected(importName, "Present file content or unmounted namespace requires a real file backend");
        }
        auto failedOpen = handled(importName, "Read-only file open failed: absent from the bound snapshot; no handle created");
        (void)writeU32(iosb, 0xC000000FU); (void)writeU32(iosb + 4, 0);
        // A failed open does not publish a valid handle; preserve caller storage.
        call.returnValue = 0xC000000FU;
        return failedOpen;
    }
    if (importName == "__imp__NtSetInformationFile") {
        const auto iosb = call.argument32(1), input = call.argument32(2), length = call.argument32(3);
        if (call.argument32(4) != 14) { return rejected(importName, "Only file-position changes are supported; no content/metadata writes"); }
        if (length != 8 || input == 0 || (input & 7U) != 0 || !contains(input, 8)
                || iosb == 0 || (iosb & 3U) != 0 || !contains(iosb, 8) || overlapsProtectedThreadStorage(iosb, 8)
                || (input < std::uint64_t{iosb} + 8 && iosb < std::uint64_t{input} + 8)) {
            return memoryFault(importName, "File-position request requires bounded disjoint 8-byte structures");
        }
        const auto found = openFiles_.find(call.argument32(0));
        if (found == openFiles_.end()) { call.returnValue = StatusInvalidHandle; return handled(importName, "File-position handle is not open"); }
        std::uint32_t high = 0, low = 0;
        (void)readU32(input, high); (void)readU32(input + 4, low);
        const auto position = (std::uint64_t{high} << 32) | low;
        if (position > INT64_MAX) {
            (void)writeU32(iosb, StatusInvalidParameter); (void)writeU32(iosb + 4, 0);
            call.returnValue = StatusInvalidParameter;
            return handled(importName, "Negative file-position request is invalid");
        }
        auto success = handled(importName, "Updated this handle's read cursor without changing file contents or size");
        found->second.position = position; // Seeking beyond EOF does not extend a read-only file.
        ++fileIoStatistics_.seeks;
        (void)writeU32(iosb, 0); (void)writeU32(iosb + 4, 8);
        call.returnValue = StatusSuccess;
        return success;
    }
    if (importName == "__imp__NtQueryInformationFile") {
        const auto iosb = call.argument32(1), output = call.argument32(2), length = call.argument32(3), type = call.argument32(4);
        const auto required = type == 14 ? 8U : type == 17 || type == 26 ? 4U : type == 34 ? 56U : 0U;
        if (required == 0) { return rejected(importName, "File information class is not implemented"); }
        if (iosb == 0 || (iosb & 3U) != 0 || !contains(iosb, 8) || overlapsProtectedThreadStorage(iosb, 8)) {
            return memoryFault(importName, "File query IO_STATUS_BLOCK is not bounded");
        }
        if (length < required) {
            call.returnValue = 0xC0000004U;
            return handled(importName, "File information buffer is too short");
        }
        if (length > 4096 || output == 0 || (output & (required > 4 ? 7U : 3U)) != 0
                || !contains(output, length) || overlapsProtectedThreadStorage(output, length)
                || (output < std::uint64_t{iosb} + 8 && iosb < std::uint64_t{output} + length)) {
            return memoryFault(importName, "File information output is not bounded disjoint storage");
        }
        const auto found = openFiles_.find(call.argument32(0));
        if (found == openFiles_.end()) {
            call.returnValue = StatusInvalidHandle;
            return handled(importName, "File information requires an owned open file handle");
        }
        auto result = handled(importName, "Returned actual open-file state or explicit unavailable physical-sector query");
        auto status = StatusSuccess;
        auto count = required;
        if (type == 26) {
            // Native preopened files do not have an Xbox physical sector/LBA.
            // Return the reference kernel adapter's failure, never an invented sector.
            status = StatusInvalidParameter; count = 0;
        } else if (type == 17) {
            (void)writeU32(output, 0); // Native buffered pread is byte-aligned; unbuffered opens are rejected.
        } else if (type == 14) {
            (void)writeU32(output, static_cast<std::uint32_t>(found->second.position >> 32));
            (void)writeU32(output + 4, static_cast<std::uint32_t>(found->second.position));
        } else {
            for (std::uint32_t i = 0; i < 6; ++i) {
                (void)writeU32(output + i * 8, static_cast<std::uint32_t>(found->second.metadata.values[i] >> 32));
                (void)writeU32(output + i * 8 + 4, static_cast<std::uint32_t>(found->second.metadata.values[i]));
            }
            (void)writeU32(output + 48, found->second.metadata.attributes); // Preserve trailing pad.
        }
        (void)writeU32(iosb, status); (void)writeU32(iosb + 4, count);
        call.returnValue = status;
        return result;
    }
    if (importName == "__imp__NtReadFile") {
        const auto iosb = call.argument32(4), buffer = call.argument32(5), length = call.argument32(6), offsetPtr = call.argument32(7);
        // v160 DIAGNOSTIC: every sub-check of a refused read, the owner of each address, the handle's source, the chain.
        const auto v160ReadFault = [&](int site) -> bool {
            const auto open = openFiles_.find(call.argument32(0));
            std::uint32_t offHi = 0, offLo = 0;
            if (offsetPtr && contains(offsetPtr, 8)) { (void)readU32(offsetPtr, offHi); (void)readU32(offsetPtr + 4, offLo); }
            __android_log_print(ANDROID_LOG_WARN, "RecompRenderer",
                    "RENDER_DIAG_READ_FAULT site=%d token=%08x sp=%08x sp_in=%s handle=%08x open=%d size=%llu root=%s "
                    "iosb=%08x ok=%d cs=%d in=%s buf=%08x+%u ok=%d cs=%d in=%s off=%08x ok=%d value=%08x%08x in=%s frames=%s",
                    site, RECOMP_CURRENT_GUEST_TOKEN, call.stackPointer, v160DescribeArena(call.stackPointer).c_str(),
                    call.argument32(0), open != openFiles_.end() ? 1 : 0,
                    open != openFiles_.end() && open->second.source ? static_cast<unsigned long long>(open->second.source->size()) : 0ULL,
                    open != openFiles_.end() ? open->second.contentRoot.c_str() : "-",
                    iosb, validGuestDataBuffer(iosb,8,true) ? 1 : 0, hasActiveCriticalSectionInRange(iosb,8) ? 1 : 0,
                    v160DescribeArena(iosb).c_str(), buffer, length,
                    length == 0 || validGuestDataBuffer(buffer,length,true,1) ? 1 : 0,
                    length != 0 && hasActiveCriticalSectionInRange(buffer,length) ? 1 : 0, v160DescribeArena(buffer).c_str(),
                    offsetPtr, offsetPtr == 0 || validGuestDataBuffer(offsetPtr,8,false,8) ? 1 : 0, offHi, offLo,
                    v160DescribeArena(offsetPtr).c_str(), v160Frames(call.stackPointer).c_str());
            // DIAGNOSTIC SWITCH (default off): files/private-trace/diag-read-fault-status returns NT's status for an
            // output the kernel cannot write (STATUS_ACCESS_VIOLATION) instead of stopping the guest. Nothing is written
            // (no IOSB, no buffer, no cursor); the refusal above is still logged in full.
            static const bool asStatus = ::access(RECOMP_APP_PATH("/private-trace/diag-read-fault-status"), F_OK) == 0;
            return asStatus;
        };
        if (call.argument32(1) != 0 || call.argument32(2) != 0 || length > MaximumFileReadBytes) {
            return rejected(importName, "Only bounded synchronous reads without event/APC/port completion are supported");
        }
        // With no APC routine and no supported completion-port association,
        // ApcContext is an unused caller token, not a pointer to dereference.
        // The title's synchronous OVERLAPPED wrapper supplies this token.
        if (!validGuestDataBuffer(iosb,8,true) || hasActiveCriticalSectionInRange(iosb,8)
                || (length != 0 && (!validGuestDataBuffer(buffer,length,true,1) || hasActiveCriticalSectionInRange(buffer,length)))
                || (offsetPtr != 0 && !validGuestDataBuffer(offsetPtr,8,false,8))) {
            if (v160ReadFault(1)) {
                call.returnValue = 0xC0000005U;
                return handled(importName, "DIAGNOSTIC: unwritable read outputs returned STATUS_ACCESS_VIOLATION; nothing written");
            }
            return memoryFault(importName, "Read buffers and offset are not bounded guest storage");
        }
        const auto overlaps = [](std::uint32_t a, std::uint32_t size, std::uint32_t b, std::uint32_t otherSize) {
            return size && otherSize && a < std::uint64_t{b} + otherSize && b < std::uint64_t{a} + size;
        };
        if (overlaps(iosb, 8, buffer, length) || (offsetPtr != 0
                && (overlaps(offsetPtr, 8, iosb, 8) || overlaps(offsetPtr, 8, buffer, length)))) {
            return rejected(importName, "Read outputs overlap another parameter");
        }
        const auto found = openFiles_.find(call.argument32(0));
        if (found == openFiles_.end()) {
            (void)writeU32(iosb, StatusInvalidHandle); (void)writeU32(iosb + 4, 0);
            call.returnValue = StatusInvalidHandle;
            return handled(importName, "Read handle is not an owned open file");
        }
#if RECOMP_PARALLEL_REGISTRY
        // PARALLEL (WP2): the host read runs WITHOUT the registry mutex (open call).
        // Source and cursor are snapshotted under the lock, bytes are read unlocked,
        // and the result is published under the lock after revalidating the handle
        // and every guest output. A failed chunk still preserves destination, IOSB
        // and cursor. An implicit-position read whose cursor moved meanwhile (a
        // concurrent same-handle read or seek) is re-serialized after it; the last
        // attempt reads under the lock, which guarantees progress.
        const auto fileHandle = call.argument32(0);
        std::optional<std::uint64_t> explicitOffset;
        if (offsetPtr != 0) {
            std::uint32_t high = 0, low = 0;
            (void)readU32(offsetPtr, high); (void)readU32(offsetPtr + 4, low);
            const auto requested = (std::uint64_t{high} << 32) | low;
            if (requested != 0xFFFFFFFFFFFFFFFEULL) {
                if (requested > INT64_MAX) { return rejected(importName, "Negative read offset is not a supported sentinel"); }
                explicitOffset = requested;
            }
        }
        const auto outputsValid = [&] {
            return validGuestDataBuffer(iosb,8,true) && !hasActiveCriticalSectionInRange(iosb,8)
                && (length == 0 || (validGuestDataBuffer(buffer,length,true,1) && !hasActiveCriticalSectionInRange(buffer,length)))
                && (offsetPtr == 0 || validGuestDataBuffer(offsetPtr,8,false,8));
        };
        constexpr unsigned UnlockedReadAttempts = 4;
        for (unsigned attempt = 0;; ++attempt) {
            const auto open = openFiles_.find(fileHandle);
            if (open == openFiles_.end()) { // Retry only: closed before this read was serialized.
                (void)writeU32(iosb, StatusInvalidHandle); (void)writeU32(iosb + 4, 0);
                call.returnValue = StatusInvalidHandle;
                return handled(importName, "Read handle is not an owned open file");
            }
            const auto source = open->second.source;
            const auto cursor = open->second.position;
            const auto offset = explicitOffset.value_or(cursor);
            const auto available = offset < source->size() ? source->size() - offset : 0;
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(length, available));
            auto success = handled(importName, "Read actual bytes from the explicitly bound source");
            std::vector<std::uint8_t> bytes(count);
            const bool unlocked = count != 0 && attempt < UnlockedReadAttempts;
            if (unlocked) RECOMP_REGISTRY_UNLOCK(lock);
            bool chunkRead = true;
            for(std::uint32_t done=0;done<count && chunkRead;) {
                const auto chunk=std::min(FileReadChunkBytes,count-done);
                chunkRead=source->read(offset+done,bytes.data()+done,chunk);
                done+=chunk;
            }
            if (unlocked) RECOMP_REGISTRY_RELOCK(lock);
            if (!chunkRead)
                return rejected(importName,"Explicit file source failed a bounded read chunk; outputs and position preserved");
            const auto current = openFiles_.find(fileHandle);
            const bool stillOpen = current != openFiles_.end() && current->second.source == source;
            if (unlocked && stillOpen && !explicitOffset && current->second.position != cursor) continue;
            if (unlocked && !outputsValid()) {
                if (v160ReadFault(2)) {
                    call.returnValue = 0xC0000005U;
                    return handled(importName, "DIAGNOSTIC: unwritable read outputs returned STATUS_ACCESS_VIOLATION; nothing written");
                }
                return memoryFault(importName, "Read buffers and offset are not bounded guest storage");
            }
            const auto status = length != 0 && count == 0 ? 0xC0000011U : StatusSuccess;
            // A handle closed during the read still completes its I/O, without a cursor.
            if (count != 0) {
                std::memcpy(addressSpace_->translate(buffer), bytes.data(), count);
                if (stillOpen) current->second.position = offset + count;
            }
            ++fileIoStatistics_.reads; fileIoStatistics_.bytes += count;
            ::recomp::diag::quietLogThin(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_FILE_READ size=%llu offset=%llu count=%u",
                static_cast<unsigned long long>(source->size()),static_cast<unsigned long long>(offset),count); // DIAGNOSTIC
            (void)writeU32(iosb, status); (void)writeU32(iosb + 4, count);
            call.returnValue = status;
            return success;
        }
#else
        auto& file = found->second;
        auto offset = file.position;
        if (offsetPtr != 0) {
            std::uint32_t high = 0, low = 0;
            (void)readU32(offsetPtr, high); (void)readU32(offsetPtr + 4, low);
            const auto requested = (std::uint64_t{high} << 32) | low;
            if (requested != 0xFFFFFFFFFFFFFFFEULL) {
                if (requested > INT64_MAX) { return rejected(importName, "Negative read offset is not a supported sentinel"); }
                offset = requested;
            }
        }
        const auto available = offset < file.source->size() ? file.source->size() - offset : 0;
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(length, available));
        auto success = handled(importName, "Read actual bytes from the explicitly bound source");
        std::vector<std::uint8_t> bytes(count);
        for(std::uint32_t done=0;done<count;) {
            const auto chunk=std::min(FileReadChunkBytes,count-done);
            if(!file.source->read(offset+done,bytes.data()+done,chunk))
                return rejected(importName,"Explicit file source failed a bounded read chunk; outputs and position preserved");
            done+=chunk;
        }
        const auto status = length != 0 && count == 0 ? 0xC0000011U : StatusSuccess;
        if (count != 0) { std::memcpy(addressSpace_->translate(buffer), bytes.data(), count); file.position = offset + count; }
        ++fileIoStatistics_.reads; fileIoStatistics_.bytes += count;
        ::recomp::diag::quietLogThin(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_FILE_READ size=%llu offset=%llu count=%u",
            static_cast<unsigned long long>(file.source->size()),static_cast<unsigned long long>(offset),count); // DIAGNOSTIC
        (void)writeU32(iosb, status); (void)writeU32(iosb + 4, count);
        call.returnValue = status;
        return success;
#endif
    }
    if (importName == "__imp__NtQueryFullAttributesFile") {
        if (!fileMetadataQuery_ && contentMounts_.empty()) { return rejected(importName, "No read-only file metadata namespace is bound"); }
        const auto object = call.argument32(0), output = call.argument32(1);
        std::uint32_t root = 0, name = 0, flags = 0, pointer = 0;
        std::uint16_t length = 0, maximum = 0;
        const auto rejectAttributes=[&](std::string reason,std::string_view path={}) {
            reason+=" object="+std::to_string(object)+" output="+std::to_string(output)+
                    " root="+std::to_string(root)+" flags="+std::to_string(flags)+" path=";
            for(unsigned char c:path.substr(0,1024))reason+=(c>=32&&c<127?char(c):'?');
            return rejected(importName,std::move(reason));
        };
        if (object == 0 || (object & 3U) != 0 || !contains(object, 12)
                || output == 0 || (output & 7U) != 0 || !contains(output, 56)
                || overlapsProtectedThreadStorage(output, 56)) {
            return memoryFault(importName, "File attribute object/output structures are not bounded");
        }
        (void)readU32(object, root); (void)readU32(object + 4, name); (void)readU32(object + 8, flags);
        if ((root != 0 && root != 0xFFFFFFFDU) || flags != 0x40) {
            return rejectAttributes("Only absolute case-insensitive DOS namespace attributes are supported");
        }
        if (name == 0 || (name & 3U) != 0 || !contains(name, 8)) {
            return memoryFault(importName, "File attribute ANSI_STRING descriptor is not bounded");
        }
        (void)readU16(name, length); (void)readU16(name + 2, maximum); (void)readU32(name + 4, pointer);
        if (length == 0 || length > 1024 || length > maximum || pointer == 0 || !contains(pointer, length)) {
            return memoryFault(importName, "File attribute ANSI_STRING bytes are not bounded");
        }
        const auto overlapsOutput = [&](std::uint32_t begin, std::uint32_t size) {
            return output < static_cast<std::uint64_t>(begin) + size
                    && begin < static_cast<std::uint64_t>(output) + 56;
        };
        if (overlapsOutput(object, 12) || overlapsOutput(name, 8) || overlapsOutput(pointer, length)) {
            return rejectAttributes("File attribute output overlaps its request");
        }
        const std::string_view path(reinterpret_cast<const char*>(addressSpace_->translate(pointer)), length);
        const auto normalized = symbolicLinks_.resolve(path);
        if (!normalized) {
            call.returnValue = 0xC0000033U; // STATUS_OBJECT_NAME_INVALID.
            return handled(importName, "File attribute path syntax is invalid");
        }
        const auto queried = queryFileMetadata(*normalized);
        if (queried.state == GuestFileQueryState::Unavailable) {
            return rejectAttributes("File attribute namespace is unavailable in the bound metadata snapshot",*normalized);
        }
        if (queried.state == GuestFileQueryState::NotFound) {
            call.returnValue = 0xC000000FU;
            return handled(importName, "File is absent from the complete bound metadata snapshot");
        }
        const auto& info = queried.metadata;
        if (queried.state != GuestFileQueryState::Found || (info.attributes != 1 && info.attributes != 0x11)
                || std::any_of(info.values.begin(), info.values.end(), [](auto v) { return v > INT64_MAX; })
                || info.values[4] < info.values[5]
                || (info.attributes == 0x11 && (info.values[4] != 0 || info.values[5] != 0))) {
            return rejectAttributes("File attribute provider returned invalid read-only metadata; attributes="+
                    std::to_string(info.attributes)+" allocation="+std::to_string(info.values[4])+
                    " eof="+std::to_string(info.values[5]),*normalized);
        }
        auto success = handled(importName, "Returned read-only snapshot metadata; no file contents opened");
        for (std::uint32_t i = 0; i < info.values.size(); ++i) {
            (void)writeU32(output + i * 8, static_cast<std::uint32_t>(info.values[i] >> 32));
            (void)writeU32(output + i * 8 + 4, static_cast<std::uint32_t>(info.values[i]));
        }
        (void)writeU32(output + 48, info.attributes);
        call.returnValue = StatusSuccess;
        return success;
    }
    if (importName == "__imp__XMsgInProcessCall" && call.argument32(0) == 0xFA && call.argument32(1) == 0x0007001B) {
        // v141: XMPGetPlaybackController (v138 play stopped here after 32 min, Franklin's first mission). Xenia
        // xmp_app.cc: {client, controller_ptr, locked_ptr}; writes the media player's controller and
        // locked = !title-in-control. With no dashboard music player the title is always in control: every
        // state GTA's own XMPSetPlaybackController calls can reach (Game, or Restore -> Game) reads back as
        // controller 0 (Game), locked 0, which is also Xenia's initial state.
        if (nativeXamAddress_ == 0 || (call.argument32(3) != 0 && call.argument32(3) != 12)) {
            return rejected(importName, "Only native music-player playback-controller queries are modeled");
        }
        const auto buffer = call.argument32(2);
        std::uint32_t client = 0, controller = 0, locked = 0;
        if (!validGuestDataBuffer(buffer, 12, false) || !readU32(buffer, client) || !readU32(buffer + 4, controller)
                || !readU32(buffer + 8, locked) || !validGuestDataBuffer(controller, 4, true) || !validGuestDataBuffer(locked, 4, true)
                || hasActiveCriticalSectionInRange(controller, 4) || hasActiveCriticalSectionInRange(locked, 4)) {
            return memoryFault(importName, "Playback-controller query/outputs are not bounded guest data");
        }
        const auto overlaps = [](std::uint64_t a, std::uint64_t an, std::uint64_t b, std::uint64_t bn) { return a < b + bn && b < a + an; };
        if (client != 2 || overlaps(controller, 4, locked, 4) || overlaps(controller, 4, buffer, 12) || overlaps(locked, 4, buffer, 12)) {
            return rejected(importName, "Playback-controller query requires the title client and separate outputs");
        }
        auto result = handled(importName, "Title is in background-music playback control (no dashboard music player)");
        (void)writeU32(controller, 0);
        (void)writeU32(locked, 0);
        call.returnValue = 0; // S_OK.
        return result;
    }
    if (importName == "__imp__XMsgInProcessCall") {
        // Native XAM's standalone music-player profile has no playlist or
        // playback controller. Its status is idle, independently of game audio.
        // Xenia xmp_app.cc: app FA/message 70009 takes {client, status_ptr};
        // xmp_app.h defines idle as zero. Other messages remain unsupported.
        if (nativeXamAddress_ == 0 || call.argument32(0) != 0xFA
                || call.argument32(1) != 0x00070009
                || (call.argument32(3) != 0 && call.argument32(3) != 8)) {
            return rejected(importName, "Only native idle music-player status is modeled");
        }
        const auto buffer = call.argument32(2);
        std::uint32_t client = 0, output = 0;
        if (!validGuestDataBuffer(buffer, 8, false)
                || !readU32(buffer, client) || !readU32(buffer + 4, output)
                || !validGuestDataBuffer(output, 4, true)
                || hasActiveCriticalSectionInRange(output, 4)) {
            return memoryFault(importName, "Music-player request/output is not bounded guest data");
        }
        if (client != 2 || (output < std::uint64_t{buffer} + 8
                && buffer < std::uint64_t{output} + 4)) {
            return rejected(importName, "Music-player status requires title client and separate output");
        }
        auto result = handled(importName, "Native standalone music player is idle; game audio unchanged");
        (void)writeU32(output, 0);
        call.returnValue = 0; // S_OK.
        return result;
    }
    if (importName == "__imp__XMsgStartIORequestEx") {
        // v136: music player (app FA) message 7001A, XMPSetPlaybackController, as GTA V's XDK wrapper
        // sub_836A8F08 sends it: synchronous (no overlapped), 12-byte {client, request, locked}, task args
        // only for the Dash client. Callers: sub_836A8FE8 (title client 2 takes control: request 0 Game,
        // locked 0/1) and sub_836A90C0 (request 4 Restore, locked 0). Xenia xmp_app.cc: records the
        // controller and returns X_E_SUCCESS; its controller-changed broadcast (area 0x20) has no GTA
        // listener (masks 1, 2, 5, 0x87). There is no dashboard music player here to pause or resume, so
        // the request changes no audio. The wrapper then reads the thread's last error: left unchanged.
        // Title playlist messages (7000D, 70013, 7002E) remain unsupported.
        // (Not in ImplementedBootImports: that table's size is fixed by the shared guest_import_registry.h,
        // which repo headers include directly, and dispatch does not consult it.)
        if (nativeXamAddress_ == 0 || call.argument32(0) != 0xFA || call.argument32(1) != 0x0007001A
                || call.argument32(2) != 0 || call.argument32(4) != 12 || call.argument32(5) != 0) {
            return rejected(importName, "Only synchronous title music-player playback-controller requests are modeled");
        }
        const auto buffer = call.argument32(3);
        std::uint32_t client = 0, request = 0, locked = 0;
        if (!validGuestDataBuffer(buffer, 12, false) || !readU32(buffer, client)
                || !readU32(buffer + 4, request) || !readU32(buffer + 8, locked)) {
            return memoryFault(importName, "Playback-controller request is not bounded guest data");
        }
        if (client != 2 || (request != 0 && request != 4) || locked > 1 || (request == 4 && locked != 0)) {
            return rejected(importName, "Playback-controller request is not a title Game/Restore request");
        }
        auto result = handled(importName, request == 0
                ? "Title took background-music playback control; no dashboard music player plays"
                : "Title restored background-music playback control; no dashboard music player plays");
        call.returnValue = 0; // X_E_SUCCESS; no output, completion or last error.
        return result;
    }
    if (importName == "__imp__XMsgStartIORequest") {
        // Deliberately unavailable native service, not a successful query or
        // general XAM app dispatcher. The observed wrapper has a synchronous
        // failure path. Its output semantics are still unverified; preserve it.
        if (nativeXamAddress_ == 0 || call.argument32(0) != 0xFE
                || call.argument32(1) != 0x00022005 || call.argument32(2) != 0
                || call.argument32(4) != 8) {
            return rejected(importName, "Only synchronous native XAM query 22005 unavailability is modeled");
        }
        const auto buffer = call.argument32(3);
        std::uint32_t output = 0, overlapped = 0;
        if (buffer == 0 || (buffer & 3U) != 0 || !contains(buffer, 8)
                || !readU32(buffer, output) || !readU32(buffer + 4, overlapped)
                || output == 0 || (output & 3U) != 0 || !contains(output, 4)
                || overlapsProtectedThreadStorage(output, 4)) {
            return memoryFault(importName, "Native XAM query buffer/output is not bounded external storage");
        }
        if (overlapped != 0 || (output < static_cast<std::uint64_t>(buffer) + 8
                && buffer < static_cast<std::uint64_t>(output) + 4)) {
            return rejected(importName, "Native XAM query requires nonoverlapping synchronous output");
        }
        call.returnValue = 0x80004005U; // E_FAIL: service unavailable; no completion or output fabricated.
        return handled(importName, "Native XAM query 22005 unavailable; output and last error preserved");
    }
    if (importName == "__imp__XamContentCreateEnumerator") { return dispatchContentEnumeration(call, true); }
    if (importName == "__imp__XamEnumerate") { return dispatchContentEnumeration(call, false); }
    if (importName == "__imp__XamContentCreateEx") { return dispatchContentMount(call, false); }
    if (importName == "__imp__XamContentClose") { return dispatchContentMount(call, true); }
    if (importName == "__imp__XamContentGetDeviceData") { return dispatchContentDeviceData(call); }
    if (importName == "__imp__RtlTimeFieldsToTime") { return dispatchRtlTimeFieldsToTime(call); }
    if (importName == "__imp__RtlTimeToTimeFields") {
        // v164: VOID RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields). GTA's first autosave reaches
        // it through the XDK GetLocalTime (sub_8363D5C8: KeQuerySystemTime - time-zone bias, then this call; it
        // copies Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday = 8 big-endian CSHORTs into a
        // SYSTEMTIME). Time is 100 ns units since 1601-01-01 (a Monday); proleptic Gregorian calendar.
        const auto input = call.argument32(0), output = call.argument32(1);
        if (!validGuestDataBuffer(input, 8, false, 4) || !validGuestDataBuffer(output, 16, true, 2)
                || hasActiveCriticalSectionInRange(output, 16)
                || (input < std::uint64_t{output} + 16 && output < std::uint64_t{input} + 8)) {
            return memoryFault(importName, "Time conversion requires bounded input and disjoint writable fields");
        }
        std::uint32_t hi = 0, lo = 0; (void)readU32(input, hi); (void)readU32(input + 4, lo);
        const auto ticks = (std::uint64_t{hi} << 32) | lo;
        if (ticks > 0x7FFFFFFFFFFFFFFFULL) { return rejected(importName, "Negative LARGE_INTEGER time is not modeled"); }
        const auto totalMs = ticks / 10000ULL;
        const auto totalSeconds = totalMs / 1000ULL;
        const auto daysSince1601 = static_cast<std::int64_t>(totalSeconds / 86400ULL);
        const auto secondOfDay = totalSeconds % 86400ULL;
        // Days since 1970-01-01, then Howard Hinnant's civil_from_days.
        const std::int64_t z = daysSince1601 - 134774 + 719468;
        const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<std::uint64_t>(z - era * 146097);
        const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const auto mp = (5 * doy + 2) / 153;
        const auto day = doy - (153 * mp + 2) / 5 + 1;
        const auto month = mp < 10 ? mp + 3 : mp - 9;
        const auto year = static_cast<std::int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
        const std::uint16_t fields[8] = {static_cast<std::uint16_t>(year), static_cast<std::uint16_t>(month),
                static_cast<std::uint16_t>(day), static_cast<std::uint16_t>(secondOfDay / 3600),
                static_cast<std::uint16_t>((secondOfDay / 60) % 60), static_cast<std::uint16_t>(secondOfDay % 60),
                static_cast<std::uint16_t>(totalMs % 1000), static_cast<std::uint16_t>((daysSince1601 + 1) % 7)};
        for (std::uint32_t i = 0; i < 8; ++i) (void)writeU16(output + i * 2, fields[i]);
        return handled(importName, "Split a FILETIME into Gregorian time fields (VOID; r3 unchanged)");
    }
    if (importName == "__imp__XMACreateContext") {
        if (!xmaBackend_) return rejected(importName,"No native XMA context pool bound");
        const auto output=call.argument32(0);
        if (!validGuestDataBuffer(output,4,true)) return memoryFault(importName,"XMA context output is not writable guest storage");
        const auto address=xmaBackend_->allocate();
        (void)writeU32(output,address);
        call.returnValue=address?StatusSuccess:StatusNoMemory;
        return handled(importName,"Allocated actual native XMA decoder context");
    }
    if (importName == "__imp__XMAReleaseContext") {
        if (!xmaBackend_ || !xmaBackend_->release(call.argument32(0)))
            return rejected(importName,"XMA release is not an allocated context");
        call.returnValue=StatusSuccess;
        return handled(importName,"Released native XMA decoder and scrubbed its context");
    }
    if (importName == "__imp__NetDll_WSACleanup") { return dispatchNetCleanup(call, true); }
    if (importName == "__imp__NetDll_XNetCleanup") { return dispatchNetCleanup(call, false); }
    if (importName == "__imp__NetDll_WSAGetLastError") { return dispatchWsaGetLastError(call); }
    if (importName == "__imp__NetDll_XNetStartup") { return dispatchXNetStartup(call); }
    if (importName == "__imp__NetDll_XNetGetOpt") { return dispatchXNetGetOpt(call); }
    if (importName == "__imp__NetDll_XNetGetEthernetLinkStatus") {
        if (!networkLifecycleBound_ || call.argument32(0) != 1)
            return rejected(importName, "Link query requires the explicit native title-network policy");
        // The current native network service has no connected interface or
        // transport. Zero flags means link inactive, not a successful login.
        call.returnValue = 0;
        return handled(importName, "Native title network has no active Ethernet link");
    }
    if (importName == "__imp__NetDll_XNetGetTitleXnAddr") {
        if (!networkLifecycleBound_ || call.argument32(0) != 1)
            return rejected(importName, "Address query requires the explicit disconnected title-network policy");
        const auto output = call.argument32(1);
        if (!validGuestDataBuffer(output, 36, true, 4))
            return memoryFault(importName, "XNADDR output is not a bounded writable 36-byte record");
        auto result = handled(importName, "Native title network has no acquired address or online identity");
        // XNADDR: IPv4+online IPv4+port+MAC+online ID = 4+4+2+6+20.
        // Configuration bookkeeping does not initialize a transport. The
        // disconnected native service reports NONE, not PENDING or fabricated
        // loopback/MAC/online credentials (Xenia's title-address stub).
        std::memset(addressSpace_->translate(output), 0, 36);
        call.returnValue = 1; // XNET_GET_XNADDR_NONE.
        return result;
    }
    if (importName == "__imp__NetDll_WSAStartup") { return dispatchWsaStartup(call); }
    if (importName == "__imp__XexLoadImage") { return dispatchXexLoadImage(call); }
    if (importName == "__imp__XexGetModuleHandle") { return dispatchXexGetModuleHandle(call); }
    if (importName == "__imp__XexGetProcedureAddress") { return dispatchXexGetProcedureAddress(call); }
    if (importName == "__imp__XexUnloadImage") { return dispatchXexUnloadImage(call); }
    if (importName == "__imp__KeQuerySystemTime") { return dispatchKeQuerySystemTime(call); }
    if (importName == "__imp__KeQueryPerformanceFrequency") { return dispatchKeQueryPerformanceFrequency(call); }
    if (importName == "__imp__XamNotifyCreateListener") { return dispatchXamNotifyCreateListener(call); }
    if (importName == "__imp__XNotifyGetNext") { return dispatchXNotifyGetNext(call); }
    if (importName == "__imp__XNotifyPositionUI") {
        if (!notificationServiceBound_) return rejected(importName, "Notification service is unbound");
        // Void DWORD setter (Xenia xam_notify.cc). Android has no Xbox system
        // overlay: retain the title's preference for a native overlay consumer.
        // Do not fake events, guest writes, or a DWORD success return.
        notificationUiPosition_ = call.argument32(0);
        return handled(importName, "Stored native notification overlay position preference");
    }
    if (importName == "__imp__XamUserGetSigninState") {
        if (!signedOutUsersBound_) return rejected(importName, "Native user-state policy is unbound");
        // Xenia xam_user.cc: DWORD state by user index; 0 means signed out,
        // including indices outside the four slots. No profile is created.
        call.returnValue = 0;
        return handled(importName, "No user is signed in under the native signed-out policy");
    }
    if (importName == "__imp__XamUserCheckPrivilege") {
        if (!signedOutUsersBound_) return rejected(importName, "Native signed-out user policy is unbound");
        const auto user = call.argument32(0), output = call.argument32(2);
        if (user == 255) return rejected(importName, "Aggregate privilege-query semantics are not established");
        if (!validGuestDataBuffer(output, 4, true))
            return memoryFault(importName, "Privilege result must point to a bounded writable BOOL");
        // Xenia xam_user.cc returns NO_SUCH_USER before writing output for an
        // absent user. Unlike its default profile, our four slots are ALL empty.
        // No privilege is granted, and error paths preserve the caller's BOOL.
        call.returnValue = user < 4 ? 1317 : 87;
        return handled(importName, "No native user profile exists for a per-user privilege query");
    }
    if (importName == "__imp__XamUserGetXUID" || importName == "__imp__XamUserGetSigninInfo"
            || importName == "__imp__XamUserGetName") {
        if (!signedOutUsersBound_) return rejected(importName, "Native signed-out user policy is unbound");
        constexpr std::uint32_t NoSuchUser = 0x80070525U, InvalidArgument = 0x80070057U;
        const auto user = call.argument32(0);
        // These XAM exports return HRESULTs, unlike CheckPrivilege's Win32
        // error. Xenia xam_user.cc clears XUID/signin info even on user absence,
        // but GetName returns before touching the caller's string buffer.
        if (importName == "__imp__XamUserGetName") {
            call.returnValue = user < 4 ? NoSuchUser : InvalidArgument;
            return handled(importName, "No name is available because the native user slot is empty");
        }
        const bool xuid = importName == "__imp__XamUserGetXUID";
        const auto output = call.argument32(2), flags = call.argument32(1);
        if (xuid && flags != 1 && flags != 2 && flags != 3 && flags != 4 && flags != 7)
            return rejected(importName, "Unknown XUID profile-type mask");
        if (!output) {
            call.returnValue = InvalidArgument;
            return handled(importName, "Null user identity output");
        }
        const auto bytes = xuid ? 8U : 40U;
        if (!validGuestDataBuffer(output, bytes, true, xuid ? 8U : 4U))
            return memoryFault(importName, "User identity output is not bounded writable storage");
        auto result = handled(importName, "Cleared user identity output; no native profile is registered");
        std::memset(addressSpace_->translate(output), 0, bytes);
        call.returnValue = xuid && user >= 4 ? InvalidArgument : NoSuchUser;
        return result;
    }
    if (importName == "__imp__XamUserGetDeviceContext") {
        if (!disconnectedInputBound_ && !nativeGamepadReader_)
            return rejected(importName, "Device context requires an explicit native input configuration");
        const auto output = call.argument32(2);
        if (!validGuestDataBuffer(output, 4, true))
            return memoryFault(importName, "Device context output is not bounded writable guest storage");
        // Zero context value, with HRESULT based on real controller presence.
        // This associates no signed-in user or profile with the controller.
        int connected=0;
        if(nativeGamepadReader_) {
            auto user=call.argument32(0);if((user&0xFFU)==0xFFU)user=0;
            recomp::input::GamepadState state;
            connected=nativeGamepadReader_(user,state);
            if(connected!=0 && connected!=1)return rejected(importName,"Native device context query failed");
        }
        auto result=handled(importName,"Native controller presence; no separate device context handle");
        (void)writeU32(output,0);
        call.returnValue=connected?0:0x8007048FU;
        return result;
    }
    if (importName == "__imp__XamVoiceIsActiveProcess") {
        if (!nativeXamAddress_ || !signedOutUsersBound_ || (!disconnectedInputBound_ && !nativeGamepadReader_))
            return rejected(importName, "Voice activity requires the explicit native signed-out/disconnected service profile");
        // No capture devices, signed-in users or voice-session backend are
        // registered in this profile. Game PCM playback is independent.
        call.returnValue = 0;
        return handled(importName, "No active native voice-capture process");
    }
    if (importName == "__imp__XamUserReadProfileSettings") {
        if (!signedOutUsersBound_) return rejected(importName, "Native signed-out user policy is unbound");
        if (call.argument32(2) || call.argument32(3))
            return rejected(importName, "Explicit XUID profile lookup is not implemented");
        const auto count = call.argument32(4), ids = call.argument32(5);
        const auto sizePointer = call.argument32(6), buffer = call.argument32(7);
        if (!count || count > 32 || !sizePointer || !ids) {
            call.returnValue = 87;
            return handled(importName, "Invalid profile setting count or null request storage");
        }
        if (!validGuestDataBuffer(ids, count * 4, false, 4) ||
                !validGuestDataBuffer(sizePointer, 4, true, 4) ||
                hasActiveCriticalSectionInRange(sizePointer, 4))
            return memoryFault(importName, "Profile setting IDs or size output are not bounded guest data");
        if (sizePointer < std::uint64_t{ids} + count * 4 && ids < std::uint64_t{sizePointer} + 4)
            return rejected(importName, "Profile size output aliases setting IDs");
        std::uint32_t size = 0;
        (void)readU32(sizePointer, size);
        if (size && !buffer) {
            call.returnValue = 87;
            return handled(importName, "Nonzero profile buffer size requires a buffer");
        }
        // X_USER_READ_PROFILE_SETTINGS header (8) plus each fixed record (40).
        // Packed keys carry type in bits28..31 and variable byte size16..27.
        std::uint32_t needed = 8 + 40 * count;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key = 0; (void)readU32(ids + 4 * i, key);
            if ((key >> 28) == 4 || (key >> 28) == 6) needed += (key >> 16) & 0xFFF;
        }
        if (!buffer || size < needed) {
            auto result = handled(importName, "Computed profile buffer requirement without reading a user profile");
            if (!size) (void)writeU32(sizePointer, needed);
            call.returnValue = 122; // ERROR_INSUFFICIENT_BUFFER, before user lookup.
            return result;
        }
        std::uint32_t overlapped = 0;
        if (!call.readArgument32(*addressSpace_, 8, overlapped))
            return memoryFault(importName, "Profile completion argument requires a bounded stack slot");
        if (overlapped) return rejected(importName, "Asynchronous profile completion is not implemented");
        // No native slot is signed in, including user0. Preserve the caller's
        // buffer; never manufacture preferences, a gamertag or an XUID.
        call.returnValue = 1317; // ERROR_NO_SUCH_USER.
        return handled(importName, "No signed-in native user profile exists");
    }
    if (importName == "__imp__XamLoaderGetLaunchDataSize"
            || importName == "__imp__XamLoaderGetLaunchData") {
        if (!noLaunchDataBound_) return rejected(importName, "Native launch-data policy is unbound");
        // Xenia xam_info.cc: absent payload returns ERROR_NOT_FOUND, not success.
        // Size query writes exactly one DWORD; data query leaves the buffer alone.
        if (importName == "__imp__XamLoaderGetLaunchDataSize") {
            const auto output = call.argument32(0);
            if (!output) {
                call.returnValue = 87; // ERROR_INVALID_PARAMETER
                return handled(importName, "Launch-data size output is null");
            }
            if (!validGuestDataBuffer(output, 4, true, 4))
                return memoryFault(importName, "Launch-data size output is protected or invalid");
            (void)writeU32(output, 0);
        }
        call.returnValue = 1168; // ERROR_NOT_FOUND
        return handled(importName, "No launch data for this standalone native launch");
    }
    if (importName == "__imp__XamSwapDisc") {
        if (!mountedDisc_) return rejected(importName, "Mounted disc identity is unbound");
        // A different disc requires an actual validated media change. Never
        // signal completion merely because the requested number looks valid.
        if (call.argument32(0) != mountedDisc_)
            return rejected(importName, "Requested disc is not the currently mounted media");
        const auto found = nativeEventReferences_.find(call.argument32(1));
        if (found == nativeEventReferences_.end() || !found->second.references)
            return rejected(importName, "Disc completion event reference is not owned");
        auto& reference = found->second;
        auto live = kernelHandles_.find(reference.handle);
        auto& event = live == kernelHandles_.end() ? reference.closedObject : live->second;
        if (event.kind != KernelObjectKind::NotificationEvent
                && event.kind != KernelObjectKind::SynchronizationEvent)
            return rejected(importName, "Disc completion reference is not an event");
        event.signalCount = 1;
        // PARALLEL: waiters of the live handle (a closed handle has none left).
        if (live != kernelHandles_.end()) wakeHandleWaiters(this, reference.handle);
        --reference.references; // Xam owns the SDK-acquired reference on completion.
        ++nativeDiscCompletions_;
        call.returnValue = 0;
        return handled(importName, "Requested media is already mounted; signaled its real event");
    }
    if (importName == "__imp__ExCreateThread") {
        return dispatchExCreateThread(call);
    }
    if (importName == "__imp__ObReferenceObjectByHandle") {
        return dispatchObReferenceObjectByHandle(call);
    }
    if (importName == "__imp__ObDereferenceObject") {
        return dispatchObDereferenceObject(call);
    }
    if (importName == "__imp__KeQueryBasePriorityThread") {
        return dispatchKeQueryBasePriorityThread(call);
    }
    if (importName == "__imp__KeSetBasePriorityThread") {
        return dispatchKeSetBasePriorityThread(call);
    }
    if (importName == "__imp__KeSetDisableBoostThread") { return dispatchKeSetDisableBoostThread(call); }
    if (importName == "__imp__NtResumeThread") {
        return dispatchNtResumeThread(call);
    }
    if (importName == "__imp__KeSetAffinityThread") {
        return dispatchKeSetAffinityThread(call);
    }
    if (importName == "__imp__RtlRaiseException") {
        return dispatchRtlRaiseException(call);
    }
    if (importName == "__imp__RtlNtStatusToDosError") { return dispatchRtlNtStatusToDosError(call); }
    if (importName == "__imp__KeGetCurrentProcessType") {
        return dispatchKeGetCurrentProcessType(call);
    }
    if (importName == "__imp__KeTlsAlloc") {
        return dispatchKeTlsAlloc(call);
    }
    if (importName == "__imp__KeTlsFree") {
        return dispatchKeTlsFree(call);
    }
    if (importName == "__imp__KeTlsGetValue") {
        return dispatchKeTlsGetValue(call);
    }
    if (importName == "__imp__KeTlsSetValue") {
        return dispatchKeTlsSetValue(call);
    }
    if (importName == "__imp__MmAllocatePhysicalMemory") {
        return dispatchMmAllocatePhysicalMemory(call);
    }
    if (importName == "__imp__MmAllocatePhysicalMemoryEx") {
        return dispatchMmAllocatePhysicalMemoryEx(call);
    }
    if (importName == "__imp__MmFreePhysicalMemory") {
        return dispatchMmFreePhysicalMemory(call);
    }
    if (importName == "__imp__MmGetPhysicalAddress") {
        return dispatchMmGetPhysicalAddress(call);
    }
    if (importName == "__imp__MmQueryAddressProtect") {
        return dispatchMmQueryAddressProtect(call);
    }
    if (importName == "__imp__MmQueryAllocationSize") {
        return dispatchMmQueryAllocationSize(call);
    }
    if (importName == "__imp__MmQueryStatistics") {
        return dispatchMmQueryStatistics(call);
    }
    if (importName == "__imp__NtAllocateVirtualMemory") {
        return dispatchNtAllocateVirtualMemory(call);
    }
    if (importName == "__imp__NtFreeVirtualMemory") {
        return dispatchNtFreeVirtualMemory(call);
    }
    if (importName == "__imp__NtCreateSemaphore") {
        return dispatchNtCreateSemaphore(call);
    }
    if (importName == "__imp__NtReleaseSemaphore") {
        return dispatchNtReleaseSemaphore(call);
    }
    if (importName == "__imp__NtCreateEvent") {
        return dispatchNtCreateEvent(call);
    }
    if (importName == "__imp__ExGetXConfigSetting") {
        // Explicit software configuration, not host/Xbox persistent settings.
        // Independently bound USER_LANGUAGE / USER_COUNTRY / USER_VIDEO_FLAGS.
        const auto category = call.argument32(0), setting = call.argument32(1);
        const auto output = call.argument32(2), bytes = call.argument32(3);
        const auto required = call.argument32(4);
        std::optional<std::uint32_t> value;
        std::uint32_t valueBytes = setting == 14 || setting == 15 ? 1 : 4;
        if (category == 3 && setting == 9) value = userLanguage_;
        if (category == 3 && setting == 14) value = userCountry_;
        if (category == 3 && setting == 10 && virtualVideoModeBound_) value = 0x00040000U;
        // v162: the user category's time zone (XCONFIG_USER_TIME_ZONE_* 1..7, 4 bytes each; the XDK's
        // GetTimeZoneInformation sub_8364A540 reads all seven; GTA reaches it right after the "Franklin and Lamar"
        // reward, before the first autosave). This console is configured to UTC without daylight saving: bias 0,
        // names "UTC", zero transition dates (no DST, as TIME_ZONE_INFORMATION with wMonth 0), standard/daylight
        // bias 0. Parental controls as Xenia's defaults (read-only reference xconfig.cc): PC_FLAGS (15, 1 byte)
        // XBLAllowed|XBLMembershipCreationAllowed, PC_GAME (22) NoGameRestrictions 0xFF, PC_GAME_RATING (25) 0.
        std::optional<std::array<std::uint8_t, 4>> raw;
        if (category == 3 && (setting == 2 || setting == 3)) raw = std::array<std::uint8_t, 4>{'U', 'T', 'C', 0};
        if (category == 3 && (setting == 1 || setting == 4 || setting == 5 || setting == 6 || setting == 7)) value = 0;
        if (category == 3 && setting == 15) value = 0x03;
        if (category == 3 && setting == 22) value = 0xFF;
        if (category == 3 && setting == 25) value = 0;
        // v163: the two remaining call sites of the executable's 16 (static scan of every bl to the thunk): the time-zone
        // function also reads RETAIL_FLAGS (3/12): DashboardInitialized 0x40 (Xenia default) | DSTOff 0x02 (this
        // console's zone has no daylight saving, see above); the video-setup wrapper reads SECURED_AV_REGION (2/2):
        // NTSC-M 0x00400100 (Xenia default).
        if (category == 3 && setting == 12) value = 0x42;
        if (category == 2 && setting == 2) value = 0x00400100U;
        if (raw) value = 0;
        if (!value || (output ? bytes != valueBytes : bytes != 0)) {
            return rejected(importName, "Configuration setting is unbound or its query size is unsupported");
        }
        const auto safeOutput = [&](std::uint32_t address, std::uint32_t size) {
            const auto end = std::uint64_t{address} + size;
            return contains(address, size) &&
                !(address < 0x7FC90000 && end > 0x7FC80000) &&
                !overlapsProtectedThreadStorage(address, size) && !overlapsThreadTlsStorage(address, size);
        };
        if ((output && ((output & (valueBytes - 1)) || !safeOutput(output, valueBytes))) ||
            (required && ((required & 1U) || !safeOutput(required, 2)))) {
            return memoryFault(importName, "Configuration outputs must be bounded, aligned and outside protected storage/MMIO");
        }
        if (output && required && output < std::uint64_t{required} + 2 &&
            required < std::uint64_t{output} + valueBytes) {
            return rejected(importName, "Configuration outputs alias");
        }
        // Validate both outputs before any mutation. RequiredSize is uint16_t*,
        // corroborated by the title's sth stack initialization before this call.
        if (output) {
            if (raw) std::memcpy(addressSpace_->translate(output), raw->data(), raw->size());
            else if (valueBytes == 1) *addressSpace_->translate(output) = static_cast<std::uint8_t>(*value);
            else (void)writeU32(output, *value);
        }
        if (required) (void)writeU16(required, valueBytes);
        call.returnValue = StatusSuccess;
        return handled(importName, "Returned explicitly bound native configuration preference");
    }
    if (importName == "__imp__VdGetCurrentDisplayGamma") {
        if (!virtualVideoModeBound_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready())
            return rejected(importName, "Display-transfer query requires the explicit live native profile");
        const auto type = call.argument32(0), power = call.argument32(1);
        for (const auto output : {type, power}) {
            if (!output || (output & 3U) || !contains(output, 4) ||
                (output < 0x7FC90000 && std::uint64_t{output} + 4 > 0x7FC80000) ||
                overlapsProtectedThreadStorage(output, 4) || overlapsThreadTlsStorage(output, 4))
                return memoryFault(importName, "Display-transfer output must be bounded ordinary guest storage");
        }
        if (type == power) return rejected(importName, "Display-transfer outputs overlap");
        std::uint32_t transfer = 0, powerBits = 0;
        if (!graphicsBackend_->queryDisplayGamma(transfer, powerBits) || transfer != 1 || powerBits != 0x3F800000)
            return rejected(importName, "Native backend has no configured sRGB transfer profile");
        auto result = handled(importName, "Returned configured native sRGB transfer, not measured display gamma");
        (void)writeU32(type, transfer); (void)writeU32(power, powerBits);
        return result;
    }
    if (importName == "__imp__XGetVideoMode" || importName == "__imp__VdQueryVideoMode") {
        if (!virtualVideoModeBound_) {
            return rejected(importName, "No explicit software video mode is bound");
        }
        const auto output = call.argument32(0);
        if (!validGuestDataBuffer(output,48,true)) {
            return memoryFault(importName, "Video-mode output must be bounded and outside protected thread storage");
        }
        const auto overlaps=[&](std::uint32_t base,std::uint32_t size){
            return std::uint64_t{output}<std::uint64_t{base}+size && std::uint64_t{base}<std::uint64_t{output}+48;
        };
        { CsAll csAllGuard; for(const auto& entry:criticalSections_)if(overlaps(entry.first,28))return rejected(importName,"Video output aliases a critical section"); } // (v197)
        for(const auto& entry:dispatcherEventWaits_)if(overlaps(entry.first,16))return rejected(importName,"Video output aliases a dispatcher event");
        if(graphicsRingConfiguration_ && (overlaps(graphicsRingConfiguration_->virtualAddress,graphicsRingConfiguration_->bytes) ||
            (graphicsRingConfiguration_->writeBack && overlaps(graphicsRingConfiguration_->writeBack->virtualAddress,4))))
            return rejected(importName,"Video output aliases ring control");
        if(graphicsSystemIdentifier_ && overlaps(*graphicsSystemIdentifier_,4))return rejected(importName,"Video output aliases GPU identifier");
        // XVIDEO_MODE: shared 48-byte native software profile for XAM/kernel
        // queries. Seven public words, then two unknown and three reserved words
        // deliberately zero. No inherited emulator magic values for unknowns.
        // 60.0f is encoded directly as IEEE binary32, not a live display sample.
        std::array<std::uint32_t, 12> Words{
                1280, 720, 0, 1, 1, 0x42700000, 1, 0, 0, 0, 0, 0};
        // (ios-duo 2026-11-05; 28s: the Duo's Top Screen 4:3 choice, duo_app.h loadMode sets it before the game starts, or a test
        // launch's environment) RECOMP_DUO_4X3=1 answers fIsWideScreen = 0 at the same 1280x720: the game stores it at 0x83BFBEDE (sub_83225428) and its
        // getter sub_824C3F80 picks the 4:3 sets of HUD positions / scales at 53 call sites. The presenter shows the frame as 4:3.
        {static const bool narrow=[]{const char* e=std::getenv("RECOMP_DUO_4X3");return e && e[0]=='1';}();
         if(narrow){Words[3]=0;std::fprintf(stderr,"APPLE_IOS_VIDEO_MODE widescreen=0 hidef=1 size=1280x720 source=RECOMP_DUO_4X3\n");}}
        if(nativeScalerTimingBinding_){const auto& binding=*nativeScalerTimingBinding_;
            std::uint32_t pointer=0,clock=0;
            if(nativeScalerTimingQueries_==UINT64_MAX || overlaps(binding[0],4) || overlaps(binding[1],4) ||
                !readU32(binding[0],pointer) || pointer!=binding[1] || !readU32(binding[1],clock) ||
                clock!=recomp::gpu::NativeScalerTiming::NominalGpuMHz)return rejected(importName,"Native scaler timing binding lost continuity");
            Words[7]=recomp::gpu::NativeScalerTiming::PixelClockWholeMHz;
        }
        auto result=handled(importName,"Returned explicitly configured virtual mode; native scaler timing only when separately enabled");
        for (std::size_t i = 0; i < Words.size(); ++i) {
            (void)writeU32(output + static_cast<std::uint32_t>(i * 4), Words[i]);
        }
        if(nativeScalerTimingBinding_)++nativeScalerTimingQueries_;
        return result;
    }
    if (importName == "__imp__NtCreateMutant") { return dispatchNtCreateMutant(call); }
    if (importName == "__imp__NtCreateTimer" || importName == "__imp__NtSetTimerEx"
            || importName == "__imp__NtCancelTimer") { return dispatchTimer(importName, call); }
    if (importName == "__imp__NtReleaseMutant") { return dispatchNtReleaseMutant(call); }
    if (importName == "__imp__NtSetEvent") {
        return dispatchNtSetEvent(call);
    }
    if (importName == "__imp__NtClearEvent") {
        return dispatchNtClearEvent(call);
    }
    if (importName == "__imp__NtWaitForSingleObjectEx") {
        bool pending = false;
        std::uint64_t relativeTicks = 0;
        auto result = dispatchNtWaitForSingleObjectEx(call, pending, relativeTicks);
        if (!pending) { return result; }
        const auto handler = threadWaitHandler_;
        const auto timedHandler = threadTimedWaitHandler_;
        const auto deadline = relativeTicks != 0
                ? std::optional(std::chrono::steady_clock::now() + std::chrono::nanoseconds(relativeTicks * 100))
                : std::nullopt;
        const auto handle = call.argument32(0);
        const auto waiter = RECOMP_CURRENT_GUEST_TOKEN; // Polling may occur on another task.
        bool resolved = false;
        [[maybe_unused]] bool auFirst = true; // audio-ahead: only the first pass of this wait's loop may raise the hint
        // PARALLEL: registered under the registry mutex before the first unlock, so
        // every signal of this handle (set/release/close/timer) wakes this waiter.
        [[maybe_unused]] ParkedRegistryWait parkedWait(this, handle, waiter);
        for (;;) {
            // Timer expiration is a signal deadline, not the caller's timeout.
            // Rearming/cancelling wakes this park so its scheduler deadline can be
            // updated; the guest wait itself remains pending until signal/timeout.
            const auto waitingObject = RECOMP_FIND_HANDLE(handle); // imports-fast D
            if (waitingObject == kernelHandles_.end()) {
                return rejected(importName, "Wait object closed while changing its timer deadline");
            }
            const auto expectedTimer = waitingObject->second.timer.due;
            auto wake = deadline;
            if (expectedTimer && (!wake || *expectedTimer < *wake)) { wake = expectedTimer; }
            if (wake && !timedHandler) { return rejected(importName, "Timer wait requires a timed scheduler binding"); }
#if RECOMP_PARALLEL_REGISTRY
            // audio-ahead (out of line, under R): main about to wait on an unsignalled engine-signalled handle raises the hint
            recomp_au_ahead::onWaitBegin(handle, waiter, auFirst, waitingObject->second.signalCount,
                                         waitingObject->second.kind == KernelObjectKind::Mutant);
            auFirst = false;
#endif
            RECOMP_REGISTRY_UNLOCK(lock); // Another guest task must be able to signal this object.
            RegistryWakes::flushActive(); // PARALLEL: never park holding undelivered wakes.
            struct { bool valid = false; unsigned skips = 0; std::uint32_t bucket = 0, global = 0; } epochs; // v198
            auto& bucketEpoch = handleEpoch(this, handle);
            const ThreadWaitReady ready = [&] {
                if (epochs.valid && epochs.skips < 3 && waitEpochEnabled() && !resolved &&
                        (!wake || std::chrono::steady_clock::now() < *wake) &&
                        bucketEpoch.load(std::memory_order_acquire) == epochs.bucket &&
                        globalHandleEpoch.load(std::memory_order_acquire) == epochs.global) {
                    ++epochs.skips; waitEpochSkips.fetch_add(1, std::memory_order_relaxed);
                    return false; // unchanged since the last locked check found it unavailable (v198)
                }
                epochs.valid = false; epochs.skips = 0;
                epochs.bucket = bucketEpoch.load(std::memory_order_acquire); // read BEFORE the locked check
                epochs.global = globalHandleEpoch.load(std::memory_order_acquire);
                waitEpochChecks.fetch_add(1, std::memory_order_relaxed);
                RECOMP_REGISTRY_LOCK(pollLock);
                if (resolved) { return true; }
                const auto object = kernelHandles_.find(handle);
                if (addressSpace_ == nullptr || !addressSpace_->isReserved() || object == kernelHandles_.end()) {
                    result = rejected(importName, "Wait object closed or registry became unavailable while parked");
                    resolved = true;
                    return true;
                }
                const auto acquired = acquireKernelObject(object->second, waiter, call.returnValue);
                if (acquired == ObjectAcquireState::RecursionLimit) {
                    result = rejected(importName, "Mutant recursion exceeds the diagnostic cap");
                    resolved = true;
                    return true;
                }
                if (acquired == ObjectAcquireState::Unavailable) {
                    if (!deadline || std::chrono::steady_clock::now() < *deadline) {
                        const bool rearmed = object->second.timer.due != expectedTimer;
                        if (!rearmed) epochs.valid = true; // v198: the next polls may skip R while nothing changed
                        return rearmed;
                    }
                    result = handled(importName, "Relative cooperative wait reached its monotonic deadline");
                    call.returnValue = StatusTimeout;
                    resolved = true;
                    return true;
                }
                result = handled(importName, "Completed an infinite cooperative wait after a real object signal");
                resolved = true;
                return true;
            };
            TimelineWaitObject timelineObject(handle); // v195
            const auto completed = wake ? timedHandler(ready, *wake) : handler(ready);
            RECOMP_REGISTRY_RELOCK(lock);
#if RECOMP_PARALLEL_REGISTRY
            recomp_au_ahead::onWaitReturned(handle, waiter, completed, resolved); // audio-ahead: main's wait ended (out of line)
#endif
            if (!completed) {
                return rejected(importName, "No runnable signal producer or cooperative wait budget exhausted");
            }
            if (resolved) { return result; }
        }
    }
    if (importName == "__imp__NtClose") {
        return dispatchNtClose(call);
    }
    if (importName == "__imp__NtDuplicateObject") { return dispatchNtDuplicateObject(call); }
    if (importName == "__imp__RtlCompareMemoryUlong") {
        return dispatchRtlCompareMemoryUlong(call);
    }
    if (importName == "__imp__RtlFillMemoryUlong") {
        return dispatchRtlFillMemoryUlong(call);
    }
    if (importName == "__imp__RtlInitAnsiString") {
        return dispatchRtlInitAnsiString(call);
    }
    if (importName == "__imp__RtlInitUnicodeString") {
        return dispatchRtlInitUnicodeString(call);
    }
    if (importName == "__imp__RtlImageXexHeaderField") {
        return dispatchRtlImageXexHeaderField(call);
    }
    if (importName == "__imp__RtlInitializeCriticalSection") {
        return dispatchRtlInitializeCriticalSection(call);
    }
    if (importName == "__imp__RtlInitializeCriticalSectionAndSpinCount") {
        return dispatchRtlInitializeCriticalSectionAndSpinCount(call);
    }
    if (importName == "__imp__RtlEnterCriticalSection") {
        return dispatchRtlEnterCriticalSection(call, lock);
    }
    if (importName == "__imp__RtlTryEnterCriticalSection") {
        return dispatchRtlTryEnterCriticalSection(call);
    }
    if (importName == "__imp__RtlLeaveCriticalSection") {
        return dispatchRtlLeaveCriticalSection(call);
    }
    if (importName == "__imp__XexCheckExecutablePrivilege") {
        return dispatchXexCheckExecutablePrivilege(call);
    }
    return {
            GuestImportDispatchState::NotRegistered,
            std::string(importName),
            "Import is intentionally outside the implemented portable boot slice",
    };
}

// Xbox argument layout follows Xenia's xboxkrnl_threading.cc at
// 95a5c3ee250f80c3b9d139658649d9ffb6db3eec. Counts are signed LONG values.
// This slice implements unnamed objects, immediate polls, and explicitly bound
// infinite cooperative waits. It never synthesizes success for an empty object
// or holds the registry mutex while a guest task is parked.
std::uint32_t GuestImportRegistry::createKernelHandle(KernelObject object) {
    if (kernelHandles_.size() >= MaximumKernelHandles
            || nextKernelHandle_ > 0x7FFFFFFCU) {
        return 0;
    }
    const auto handle = static_cast<std::uint32_t>(nextKernelHandle_);
    kernelHandles_.emplace(handle, object);
    nextKernelHandle_ += 4U;
    return handle;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtCreateSemaphore(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtCreateSemaphore"};
    call.returnValue = StatusInvalidParameter;
    const auto output = call.argument32(0);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4)) {
        return memoryFault(Name, "Semaphore handle output must be a bounded guest ULONG");
    }
    if (call.argument32(1) != 0) {
        return rejected(Name, "Named or attributed semaphore creation is not implemented");
    }
    const auto initial = call.argument32(2);
    const auto maximum = call.argument32(3);
    if (maximum == 0 || maximum > INT32_MAX || initial > maximum) {
        (void)writeU32(output, 0);
        return handled(Name, "Semaphore count or maximum is invalid");
    }
    const auto handle = createKernelHandle({KernelObjectKind::Semaphore, initial, maximum});
    (void)writeU32(output, handle);
    call.returnValue = handle == 0 ? StatusNoMemory : StatusSuccess;
    return handled(Name, "Created a bounded unnamed semaphore handle");
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtReleaseSemaphore(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtReleaseSemaphore"};
    call.returnValue = StatusInvalidParameter;
    const auto previous = call.argument32(2);
    if (previous != 0 && ((previous & 3U) != 0 || !contains(previous, 4))) {
        return memoryFault(Name, "Previous semaphore count output is outside guest memory");
    }
    const auto increment = call.argument32(1);
    if (increment == 0 || increment > INT32_MAX) {
        return handled(Name, "Semaphore release count must be a positive signed LONG");
    }
    const auto found = RECOMP_FIND_HANDLE(call.argument32(0)); // imports-fast D
    if (found == kernelHandles_.end()) {
        call.returnValue = StatusInvalidHandle;
        return handled(Name, "Semaphore handle is not owned by the registry");
    }
    auto& object = found->second;
    if (object.kind != KernelObjectKind::Semaphore) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(Name, "Kernel handle does not refer to a semaphore");
    }
    if (increment > object.maximumCount - object.signalCount) {
        call.returnValue = StatusSemaphoreLimitExceeded;
        return handled(Name, "Semaphore release would exceed its maximum count");
    }
    if (previous != 0) {
        (void)writeU32(previous, object.signalCount);
    }
    object.signalCount += increment;
    wakeHandleWaiters(this, call.argument32(0)); // PARALLEL: after the unlock.
    call.returnValue = StatusSuccess;
    return handled(Name, "Released semaphore tokens and returned the previous count");
}

GuestImportRegistry::ObjectAcquireState GuestImportRegistry::acquireKernelObject(
        KernelObject& object, std::uint32_t waiter, std::uint64_t& status) {
    if (object.timer.due && object.timer.refresh(std::chrono::steady_clock::now())) { object.signalCount = 1; }
    if (object.kind == KernelObjectKind::Mutant) {
        if (object.ownerToken != 0 && object.ownerToken != waiter) {
            return ObjectAcquireState::Unavailable;
        }
        if (object.recursion == MaximumMutantRecursion) { return ObjectAcquireState::RecursionLimit; }
        status = object.abandoned ? 0x80U : StatusSuccess; // STATUS_ABANDONED_WAIT_0.
        object.ownerToken = waiter;
        ++object.recursion;
        object.signalCount = 0;
        object.abandoned = false;
        return ObjectAcquireState::Acquired;
    }
    if (object.signalCount == 0) { return ObjectAcquireState::Unavailable; }
    if (object.kind == KernelObjectKind::Semaphore || object.kind == KernelObjectKind::SynchronizationEvent
            || object.kind == KernelObjectKind::SynchronizationTimer) {
        --object.signalCount;
    }
    status = StatusSuccess;
    return ObjectAcquireState::Acquired;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtCreateMutant(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtCreateMutant"};
    const auto output = call.argument32(0), initialOwner = call.argument32(2);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4)
            || overlapsProtectedThreadStorage(output, 4) || overlapsThreadTlsStorage(output, 4)) {
        return memoryFault(Name, "Mutant output must be a bounded ULONG outside protected storage");
    }
    if (call.argument32(1) != 0 || initialOwner > 1 || RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "Only unnamed mutant creation with a bound thread and Boolean ownership is supported");
    }
    auto result = handled(Name, "Created a real bounded unnamed recursive mutant or reported capacity exhaustion");
    KernelObject object{KernelObjectKind::Mutant, initialOwner ? 0U : 1U, 1};
    object.ownerToken = initialOwner ? RECOMP_CURRENT_GUEST_TOKEN : 0;
    object.recursion = initialOwner;
    std::uint32_t handle = 0;
    try { handle = createKernelHandle(object); }
    catch (const std::bad_alloc&) { handle = 0; }
    if (handle != 0) { (void)writeU32(output, handle); }
    call.returnValue = handle == 0 ? StatusNoMemory : StatusSuccess;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtReleaseMutant(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtReleaseMutant"};
    if (call.argument32(1) != 0 || RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "Mutant release requires a bound thread and the supported zero second argument");
    }
    const auto found = kernelHandles_.find(call.argument32(0));
    if (found == kernelHandles_.end()) {
        call.returnValue = StatusInvalidHandle;
        return handled(Name, "Mutant handle is invalid or closed");
    }
    auto& object = found->second;
    if (object.kind != KernelObjectKind::Mutant) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(Name, "Handle does not refer to a mutant");
    }
    if (object.ownerToken != RECOMP_CURRENT_GUEST_TOKEN || object.recursion == 0) {
        call.returnValue = 0xC0000046U; // STATUS_MUTANT_NOT_OWNED.
        return handled(Name, "The current guest thread does not own this mutant");
    }
    if (--object.recursion == 0) {
        object.ownerToken = 0; object.signalCount = 1;
        wakeHandleWaiters(this, call.argument32(0)); // PARALLEL: after the unlock.
    }
    call.returnValue = StatusSuccess;
    return handled(Name, "Released one level of current-thread mutant ownership");
}

GuestImportDispatchResult GuestImportRegistry::dispatchTimer(std::string_view name, GuestImportCall& call) {
    using Clock = GuestWaitableTimer::Clock;
    call.returnValue = StatusInvalidParameter;
    if (!threadWaitHandler_ || !threadTimedWaitHandler_) {
        return rejected(name, "Waitable timers require the native cooperative deadline scheduler");
    }
    if (name == "__imp__NtCreateTimer") {
        const auto output = call.argument32(0), type = call.argument32(2);
        if (!validGuestDataBuffer(output, 4, true)) {
            return memoryFault(name, "Timer handle output must be a bounded writable ULONG");
        }
        if (call.argument32(1) != 0) { return rejected(name, "Named timer objects are not implemented"); }
        if (type > 1) { return handled(name, "Invalid timer type"); }
        auto result = handled(name, "Created an inactive native waitable timer");
        const auto kind = type == 0 ? KernelObjectKind::NotificationTimer : KernelObjectKind::SynchronizationTimer;
        const auto handle = createKernelHandle({kind, 0, 1});
        call.returnValue = handle ? StatusSuccess : StatusNoMemory;
        (void)writeU32(output, handle);
        return result;
    }
    const auto found = kernelHandles_.find(call.argument32(0));
    if (found == kernelHandles_.end()) {
        call.returnValue = StatusInvalidHandle;
        return handled(name, "Timer handle is not owned by the registry");
    }
    auto& object = found->second;
    if (object.kind != KernelObjectKind::NotificationTimer && object.kind != KernelObjectKind::SynchronizationTimer) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(name, "Kernel handle does not refer to a timer");
    }
    if (name == "__imp__NtCancelTimer") {
        const auto output = call.argument32(1);
        if (output && !validGuestDataBuffer(output, 4, true)) {
            return memoryFault(name, "Timer state output must be a bounded writable ULONG");
        }
        auto result = handled(name, "Cancelled future expirations without clearing the timer signal");
        if (object.timer.refresh(Clock::now())) { object.signalCount = 1; }
        object.timer.due.reset();
        object.timer.period = Clock::duration::zero();
        if (output) { (void)writeU32(output, object.signalCount); }
        wakeHandleWaiters(this, call.argument32(0)); // PARALLEL: recompute the park deadline.
        call.returnValue = StatusSuccess;
        return result;
    }
    // Xbox ABI differs from desktop NtSetTimer: r6 is 1, r9 is period ms,
    // r10 is 0. Original 836DEA20 verifies this ordering. No APC is fabricated.
    if (call.argument32(2) || call.argument32(3) != 1 || call.argument32(4)
            || call.argument32(5) || call.argument32(7)) {
        return rejected(name, "Timer APC callbacks, wake-from-suspend and unknown flag combinations are unsupported");
    }
    const auto periodMs = call.argument32(6), input = call.argument32(1);
    if (periodMs > INT32_MAX) { return handled(name, "Timer period must be a nonnegative LONG"); }
    std::uint32_t high = 0, low = 0;
    if (!validGuestDataBuffer(input, 8, false, 8) || !readU32(input, high) || !readU32(input + 4, low)) {
        return memoryFault(name, "Timer due time must be a bounded aligned LARGE_INTEGER");
    }
    const auto encoded = (std::uint64_t{high} << 32U) | low;
    std::uint64_t ticks = 0;
    if (high & 0x80000000U) { ticks = ~encoded + 1U; }
    else if (encoded) {
        const auto systemNow = systemTimeSource_ ? systemTimeSource_() : std::nullopt;
        if (!systemNow) { return rejected(name, "Absolute timer requires the configured guest system-time source"); }
        ticks = encoded > *systemNow ? encoded - *systemNow : 0;
    }
    const auto now = Clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::time_point::max() - now).count();
    if (ticks > static_cast<std::uint64_t>(available) / 100U
            || std::chrono::milliseconds(periodMs) > Clock::time_point::max() - now) {
        return rejected(name, "Timer deadline exceeds the native monotonic clock range");
    }
    auto result = handled(name, "Armed a native one-shot or periodic waitable timer");
    object.timer.due = now + std::chrono::nanoseconds(ticks * 100U);
    object.timer.period = std::chrono::milliseconds(periodMs);
    object.signalCount = 0;
    wakeHandleWaiters(this, call.argument32(0)); // PARALLEL: recompute the park deadline.
    call.returnValue = StatusSuccess;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtCreateEvent(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtCreateEvent"};
    call.returnValue = StatusInvalidParameter;
    const auto output = call.argument32(0);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4)) {
        return memoryFault(Name, "Event handle output must be a bounded guest ULONG");
    }
    if (call.argument32(1) != 0) {
        return rejected(Name, "Named or attributed event creation is not implemented");
    }
    const auto type = call.argument32(2);
    if (type > 1) {
        return handled(Name, "Event type is outside NotificationEvent/SynchronizationEvent");
    }
    const auto kind = type == 0 ? KernelObjectKind::NotificationEvent
                                : KernelObjectKind::SynchronizationEvent;
    const auto handle = createKernelHandle({kind, call.argument32(3) != 0 ? 1U : 0U, 1});
    (void)writeU32(output, handle);
    call.returnValue = handle == 0 ? StatusNoMemory : StatusSuccess;
    return handled(Name, "Created a bounded unnamed event handle");
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtSetEvent(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtSetEvent"};
    call.returnValue = StatusInvalidHandle;
    const auto previous = call.argument32(1);
    if (previous != 0 && ((previous & 3U) != 0 || !contains(previous, 4))) {
        return memoryFault(Name, "Previous event state output is outside guest memory");
    }
    const auto found = RECOMP_FIND_HANDLE(call.argument32(0)); // imports-fast D
    if (found == kernelHandles_.end()) {
        return handled(Name, "Event handle is not owned by the registry");
    }
    auto& object = found->second;
    if (object.kind != KernelObjectKind::NotificationEvent
            && object.kind != KernelObjectKind::SynchronizationEvent) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(Name, "Kernel handle does not refer to an event");
    }
    if (previous != 0) {
        (void)writeU32(previous, object.signalCount);
    }
    object.signalCount = 1;
    wakeHandleWaiters(this, call.argument32(0)); // PARALLEL: after the unlock.
    call.returnValue = StatusSuccess;
    return handled(Name, "Set the event and returned its previous signal state");
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtClearEvent(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtClearEvent"};
    call.returnValue = StatusInvalidHandle;
    const auto found = RECOMP_FIND_HANDLE(call.argument32(0)); // imports-fast D
    if (found == kernelHandles_.end()) {
        return handled(Name, "Event handle is not owned by the registry");
    }
    if (found->second.kind != KernelObjectKind::NotificationEvent
            && found->second.kind != KernelObjectKind::SynchronizationEvent) {
        call.returnValue = StatusObjectTypeMismatch;
        return handled(Name, "Kernel handle does not refer to an event");
    }
    found->second.signalCount = 0;
    call.returnValue = StatusSuccess;
    return handled(Name, "Cleared the event signal state");
}

bool GuestImportRegistry::readDispatcherEvent(std::uint32_t address, std::uint32_t& header,
        std::uint32_t& signal, bool* observed) const {
    if (observed) *observed=false;
    constexpr std::uint32_t Bytes = 16;
    if (!address || (address & 3U) || !contains(address, Bytes) ||
        (address < 0x7FC90000 && std::uint64_t{address} + Bytes > 0x7FC80000) ||
        overlapsProtectedThreadStorage(address, Bytes) || overlapsThreadTlsStorage(address, Bytes)) return false;
    bool owned = false;
    const auto after = allocations_.upper_bound(address);
    if (after != allocations_.begin()) {
        const auto& a = *std::prev(after);
        owned = std::uint64_t{address} + Bytes <= std::uint64_t{a.first} + a.second;
    }
    const auto physical = physicalAllocations_.upper_bound(address);
    if (!owned && physical != physicalAllocations_.begin()) {
        const auto& a = *std::prev(physical);
        owned = (a.second.protection & PageReadWrite) &&
            std::uint64_t{address} + Bytes <= std::uint64_t{a.first} + a.second.size;
    }
    if (!owned) return false;
    std::uint32_t forward = 0, backward = 0;
    (void)readU32(address, header); (void)readU32(address + 4, signal);
    (void)readU32(address + 8, forward); (void)readU32(address + 12, backward);
    if (observed) *observed=true;
    // The original title's inline event constructor writes ONLY the type byte
    // at offset0, state at4 and list links at8/12. Bytes1..3 remain untouched
    // recycled allocation contents. Preserve them, but do not require zeros or
    // interpret them as initialized flags/size for this native event slice.
    // Event type0/1, boolean state and canonical empty list head remain required.
    // No arbitrary guest list traversal or Xenia magic-handle adoption.
    return (header >> 24) <= 1 &&
        signal <= 1 && forward == address + 8 && backward == address + 8;
}

std::optional<std::array<std::uint32_t, 3>> GuestImportRegistry::inspectDispatcherEvent(std::uint32_t address) const {
    RECOMP_REGISTRY_LOCK(lock);
    std::uint32_t header=0,signal=0;bool observed=false;
    if (!addressSpace_ || !addressSpace_->isReserved()) return std::nullopt;
    const bool valid=readDispatcherEvent(address,header,signal,&observed);
    if (!observed) return std::nullopt;
    return std::array<std::uint32_t,3>{header,signal,valid?1U:0U};
}

bool GuestImportRegistry::hasDispatcherWaitInRange(std::uint32_t address, std::uint64_t size) const {
    if (!size) return false;
    // Event headers occupy16 bytes. A header beginning before the queried
    // field can still overlap it (the old allocation-only query missed this).
    const auto found = dispatcherEventWaits_.lower_bound(address>15?address-15:0);
    return found != dispatcherEventWaits_.end() &&
        (found->first<=address || std::uint64_t{found->first}-address<size);
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeEvent(GuestImportCall& call, bool set) {
    const auto name = set ? "__imp__KeSetEvent" : "__imp__KeResetEvent";
    const auto increment=set?call.argument32(1):0;
    if (set && (increment>1 || (increment && !nativeEventWakeBoostHandler_) || call.argument32(2) != 0))
        return rejected(name, "Event priority boosts and signal-then-wait IRQL semantics are unsupported");
    const auto address = call.argument32(0);
    std::uint32_t header = 0, signal = 0;
    if (!readDispatcherEvent(address, header, signal))
        return rejected(name, "Direct event requires an initialized owned writable 16-byte header");
    const auto found = dispatcherEventWaits_.find(address);
    if (found != dispatcherEventWaits_.end()) {
        for (const auto& wait : found->second) {
            if (wait->header != header) return rejected(name, "Event type changed while a wait was parked");
        }
    }
    auto result = handled(name, "Updated real direct-event state and returned its previous value");
    auto next = set ? 1U : 0U;
    std::vector<std::shared_ptr<DispatcherEventWait>> selected;
    std::array<std::uint32_t,65> boosted{};std::size_t boostCount=0,suppressed=0;
    if (set && !signal && found != dispatcherEventWaits_.end()) {
        const auto now = std::chrono::steady_clock::now();
        selected.reserve(found->second.size());
        for (const auto& wait : found->second) {
            if (wait->signaled || wait->settled || (wait->deadline && now >= *wait->deadline)) continue;
            if (wait->group && (wait->group->winner || wait->group->settled)) continue;
            selected.push_back(wait);
            if(increment) {
                const auto* thread=findThreadObject(wait->token);
                if(!thread || thread->returned || thread->suspendCount)
                    return rejected(name,"Event boost requires a known live waiter identity");
                if(thread->disableBoost) ++suppressed;
                else if(std::find(boosted.begin(),boosted.begin()+boostCount,wait->token)==boosted.begin()+boostCount) {
                    if(boostCount==boosted.size()) return rejected(name,"Native wake boost capacity exhausted");
                    boosted[boostCount++]=wait->token;
                }
            }
            if (header >> 24) { next = 0; break; } // One FIFO auto-reset waiter.
        }
    }
    if(increment) {
        if(nativeEventWakeBoostStatistics_.signalRequests==UINT64_MAX ||
            nativeEventWakeBoostStatistics_.threadsBoosted>UINT64_MAX-boostCount ||
            nativeEventWakeBoostStatistics_.threadsSuppressed>UINT64_MAX-suppressed ||
            (boostCount && !nativeEventWakeBoostHandler_(boosted.data(),boostCount)))
            return rejected(name,"Atomic native event wake boost rejected without signaling");
        ++nativeEventWakeBoostStatistics_.signalRequests;
        nativeEventWakeBoostStatistics_.threadsBoosted+=boostCount;
        nativeEventWakeBoostStatistics_.threadsSuppressed+=suppressed;
    }
    // No fallible operation follows a successful scheduler batch. Preserve
    // one-winner reservations before another guest task can reset the event.
    for(const auto& wait:selected) {
        wait->signaled=true;if(wait->group) wait->group->winner=wait->index;
        timelineSignal(2, address); // v195
        RegistryWakes::wake(wait->token); // PARALLEL: exactly the reserved waiters, after the unlock.
    }
    (void)writeU32(address + 4, next);
    call.returnValue = signal;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeDelayExecutionThread(
        GuestImportCall& call, std::unique_lock<std::mutex>& lock) {
    constexpr auto Name = "__imp__KeDelayExecutionThread";
    constexpr std::uint64_t MaximumTicks = 600000000; // Same 60s cap as relative object waits.
    const auto token = RECOMP_CURRENT_GUEST_TOKEN;
    const auto* thread = findThreadObject(token);
    if (!threadDelayHandler_ || !thread || thread->returned || thread->suspendCount ||
            call.argument32(0) > 1 || call.argument32(1) != 0)
        return rejected(Name, "Delay requires a live current thread, native scheduler and non-alertable mode0/1");
    const auto address = call.argument32(2);
    std::uint32_t high = 0, low = 0;
    if (!validGuestDataBuffer(address, 8, false, 8) || !readU32(address, high) || !readU32(address + 4, low))
        return memoryFault(Name, "Delay interval must be an aligned bounded readable guest LARGE_INTEGER");
    // Snapshot before parking. Another task may modify/free the input later;
    // no guest pointer or thread-object iterator survives the scheduler call.
    const auto raw = (std::uint64_t{high} << 32) | low;
    const auto ticks = raw ? (~raw + 1) : 0;
    if ((raw && !(high & 0x80000000U)) || ticks > MaximumTicks)
        return rejected(Name, "Only zero-yield and relative delays up to60 seconds are supported, not absolute/APC/infinite waits");
    // v168: remember an accepted zero interval for the lock-free repeat path, only when its validity does not depend
    // on other threads' running flags (outside the thread arena, or inside the caller's own stack), at a generation
    // with no mutation window open (R is held, so none can open now: V equals generation g).
    if (!raw) {
        bool stable = !overlapsThreadArena(address, 8);
        if (!stable)
            if (const auto own = threadStorage_.lookup(thread->threadId))
                stable = address >= own->layout.stackBase &&
                        std::uint64_t{address} + 8 <= std::uint64_t{own->layout.stackBase} + own->layout.stackSize;
        if (stable)
            if (const auto* slot = registryFindSlot(this)) {
                const auto word = slot->word.load(std::memory_order_acquire);
                if ((word & RegistryWindowMask) == 0)
                    if (auto* counters = fastDelayCountersFor(this, false))
                        zeroYieldCache = {this, counters, token, address, word >> 16};
            }
    }
    if (threadDelayStatistics_[0] == UINT64_MAX || threadDelayStatistics_[4] > UINT64_MAX - ticks ||
            threadDelayStatistics_[6] >= 65)
        return rejected(Name, "Native delay accounting capacity exhausted");
    // v117: a zero-yield passes the epoch time_point (the executor's fast path key; already past, like now() + 0),
    // saving the clock reads of the ~62k/s Sleep(0) calls of the main thread.
    const auto deadline = ticks ? std::chrono::steady_clock::now() + std::chrono::nanoseconds(ticks * 100)
                                : std::chrono::steady_clock::time_point{};
    const auto threadId = thread->threadId;
    const auto handler = threadDelayHandler_;
    auto success = handled(Name, "Completed a snapshotted non-alertable monotonic delay after a real scheduling boundary");
    ++threadDelayStatistics_[0];
    if (!ticks) ++threadDelayStatistics_[3];
    threadDelayStatistics_[4] += ticks;
    threadDelayStatistics_[5] = std::max(threadDelayStatistics_[5], ticks);
    ++threadDelayStatistics_[6];
    RECOMP_REGISTRY_UNLOCK(lock);
    RegistryWakes::flushActive(); // PARALLEL: never sleep holding undelivered wakes.
    bool completed = false;
    try { completed = handler(deadline); }
    catch (...) {
        RECOMP_REGISTRY_RELOCK(lock);
        --threadDelayStatistics_[6];
        ++threadDelayStatistics_[2];
        throw;
    }
    RECOMP_REGISTRY_RELOCK(lock);
    --threadDelayStatistics_[6];
    const auto* live = findThreadObject(token);
    if (!completed || (ticks && std::chrono::steady_clock::now() < deadline) || RECOMP_CURRENT_GUEST_TOKEN != token ||
            !live || live->threadId != threadId || live->returned || live->suspendCount) {
        ++threadDelayStatistics_[2];
        return rejected(Name, "Native delay cancelled, resumed early or lost its original live thread identity");
    }
    ++threadDelayStatistics_[1];
    call.returnValue = StatusSuccess; // Timeout status belongs to object waits, not this elapsed delay.
    return success;
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeWaitForSingleObject(
        GuestImportCall& call, std::unique_lock<std::mutex>& lock) {
    constexpr auto Name = "__imp__KeWaitForSingleObject";
    // Observed UserRequest (3), plus Executive (0). No APC/alertable/IRQL model.
    if (!RECOMP_CURRENT_GUEST_TOKEN || (call.argument32(1) != 0 && call.argument32(1) != 3) ||
        call.argument32(2) > 1 || call.argument32(3) != 0)
        return rejected(Name, "Only bound non-alertable Executive/UserRequest event waits are supported");
    const auto address = call.argument32(0), timeout = call.argument32(4);
    std::uint32_t header = 0, signal = 0, high = 0, low = 0;
    if (!readDispatcherEvent(address, header, signal))
        return rejected(Name, "Direct wait requires an initialized owned writable event, not an NT handle");
    if (timeout && ((timeout & 7U) || !contains(timeout, 8) ||
        (timeout < 0x7FC90000 && std::uint64_t{timeout} + 8 > 0x7FC80000) ||
        !readU32(timeout, high) || !readU32(timeout + 4, low)))
        return memoryFault(Name, "Direct-event timeout must be an aligned bounded guest LARGE_INTEGER");
    if (const auto found = dispatcherEventWaits_.find(address); found != dispatcherEventWaits_.end()) {
        for (const auto& wait : found->second) {
            if (wait->header != header) return rejected(Name, "Event type changed while a wait was parked");
        }
    }
    if (signal) {
        auto result = handled(Name, "Acquired a real signaled direct event");
        if (header >> 24) (void)writeU32(address + 4, 0);
        call.returnValue = StatusSuccess;
        return result;
    }
    if (timeout && high == 0 && low == 0) {
        call.returnValue = StatusTimeout;
        return handled(Name, "Unsignaled direct event at explicit zero timeout");
    }
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (timeout) {
        const auto ticks = ~((std::uint64_t{high} << 32) | low) + 1;
        if (!(high & 0x80000000) || ticks > 600000000 || !threadTimedWaitHandler_)
            return rejected(Name, "Only bound relative direct-event waits up to 60 seconds are supported");
        deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ticks * 100);
    } else if (!threadWaitHandler_) return rejected(Name, "Infinite direct-event wait has no executor");
    const auto handler = threadWaitHandler_;
    const auto timedHandler = threadTimedWaitHandler_;
    auto result = rejected(Name, "Direct-event wait pending on the serialized executor");
    std::size_t count = 0;
    for (const auto& [unused, waits] : dispatcherEventWaits_) { (void)unused; count += waits.size(); }
    if (count >= MaximumKernelHandles) return rejected(Name, "Direct-event wait capacity exhausted");
    const auto wait = std::make_shared<DispatcherEventWait>(DispatcherEventWait{header, false, false, deadline,{},0,RECOMP_CURRENT_GUEST_TOKEN});
    auto [position, inserted] = dispatcherEventWaits_.try_emplace(address);
    try { position->second.push_back(wait); }
    catch (...) { if (inserted) dispatcherEventWaits_.erase(position); throw; }
    bool resolved = false;
    // PARALLEL: counted before the unlock; KeSetEvent wakes this token by name.
    [[maybe_unused]] ParkedRegistryWait parkedWait(this, 0, RECOMP_CURRENT_GUEST_TOKEN);
    RECOMP_REGISTRY_UNLOCK(lock);
    RegistryWakes::flushActive(); // PARALLEL: never park holding undelivered wakes.
    const auto cleanup = [&] {
        RECOMP_REGISTRY_LOCK(cleanupLock);
        const auto found = dispatcherEventWaits_.find(address);
        if (found != dispatcherEventWaits_.end()) {
            auto& waits = found->second;
            waits.erase(std::remove(waits.begin(), waits.end(), wait), waits.end());
            if (found->second.empty()) dispatcherEventWaits_.erase(found);
        }
    };
    try {
        const ThreadWaitReady ready = [&] {
            RECOMP_REGISTRY_LOCK(pollLock);
            if (resolved) return true;
            const auto found = dispatcherEventWaits_.find(address);
            std::uint32_t liveHeader = 0, liveSignal = 0;
            if (found == dispatcherEventWaits_.end() ||
                std::find(found->second.begin(), found->second.end(), wait) == found->second.end() ||
                !readDispatcherEvent(address, liveHeader, liveSignal) || liveHeader != wait->header) {
                result = rejected(Name, "Direct-event ownership/header changed while parked");
                wait->settled = true; resolved = true; return true;
            }
            if (wait->signaled) {
                result = handled(Name, "Direct-event signal reserved for this parked waiter");
                call.returnValue = StatusSuccess;
            } else if (deadline && std::chrono::steady_clock::now() >= *deadline) {
                result = handled(Name, "Relative direct-event wait expired on the monotonic clock");
                call.returnValue = StatusTimeout;
            } else if (liveSignal) {
                result = handled(Name, "Parked wait acquired a real direct-event signal");
                if (liveHeader >> 24) (void)writeU32(address + 4, 0);
                call.returnValue = StatusSuccess;
            } else return false;
            wait->settled = true; resolved = true; return true;
        };
        TimelineWaitObject timelineObject(address); // v195
        const auto completed = deadline ? timedHandler(ready, *deadline) : handler(ready);
        cleanup();
        if (!completed || !resolved) return rejected(Name, "Direct-event wait exhausted its executor/budget");
        return result;
    } catch (...) { cleanup(); throw; }
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeWaitForMultipleObjects(
        GuestImportCall& call, std::unique_lock<std::mutex>& lock) {
    constexpr auto Name = "__imp__KeWaitForMultipleObjects";
    const auto count = call.argument32(0), array = call.argument32(1), timeout = call.argument32(6);
    // WAIT_ANY only. WaitAll requires atomic multi-object acquisition; do not
    // emulate it as a sequence of single waits. Other object kinds stay guarded.
    if (!RECOMP_CURRENT_GUEST_TOKEN || count == 0 || count > 64 || call.argument32(2) != 1 ||
        (call.argument32(3) != 0 && call.argument32(3) != 3) || call.argument32(4) > 1 || call.argument32(5))
        return rejected(Name, "Only bounded non-alertable Executive/UserRequest WaitAny events are supported");
    const auto bounded = [&](std::uint32_t address, std::uint32_t bytes, std::uint32_t alignment) {
        return address && !(address & (alignment-1)) && contains(address, bytes) &&
            !(address < 0x7FC90000 && std::uint64_t{address}+bytes > 0x7FC80000);
    };
    if (!bounded(array, count*4, 4)) return memoryFault(Name, "Object-pointer array is outside bounded guest memory");
    // The guest wait-block pointer is opaque; native queues own all records.
    // No assumed Xbox KWAIT_BLOCK layout or guest list mutation is introduced.
    const auto blocks = call.argument32(7);
    if (blocks && !bounded(blocks, 4, 4)) return memoryFault(Name, "Opaque wait-block address is invalid");
    std::vector<std::uint32_t> addresses(count), headers(count), signals(count);
    for (std::uint32_t i=0;i<count;++i) {
        (void)readU32(array+i*4, addresses[i]);
        if (!readDispatcherEvent(addresses[i], headers[i], signals[i]))
            return rejected(Name, "WaitAny requires initialized owned writable direct events, not handles or other objects");
        for (std::uint32_t j=0;j<i;++j) {
            if (addresses[i] < std::uint64_t{addresses[j]}+16 && addresses[j] < std::uint64_t{addresses[i]}+16)
                return rejected(Name, "WaitAny event headers overlap or duplicate");
        }
        if (const auto found=dispatcherEventWaits_.find(addresses[i]);found!=dispatcherEventWaits_.end()) {
            for (const auto& wait:found->second) if (wait->header!=headers[i])
                return rejected(Name, "Event type changed while an existing wait was parked");
        }
    }
    std::uint32_t high=0,low=0;
    if (timeout && (!bounded(timeout,8,8) || !readU32(timeout,high) || !readU32(timeout+4,low)))
        return memoryFault(Name, "WaitAny timeout is not an aligned bounded LARGE_INTEGER");
    // Validate every object before consuming any signal. Lowest ready index wins.
    for (std::uint32_t i=0;i<count;++i) if (signals[i]) {
        if (headers[i]>>24) (void)writeU32(addresses[i]+4,0);
        call.returnValue=i;
        return handled(Name, "WaitAny acquired the lowest-index signaled direct event");
    }
    if (timeout && !high && !low) {
        call.returnValue=StatusTimeout;
        return handled(Name, "No WaitAny event signaled at explicit zero timeout");
    }
    std::optional<std::chrono::steady_clock::time_point> deadline;
    if (timeout) {
        const auto ticks=~((std::uint64_t{high}<<32)|low)+1;
        if (!(high&0x80000000) || ticks>600000000 || !threadTimedWaitHandler_)
            return rejected(Name, "Only bound relative WaitAny intervals up to 60 seconds are supported");
        deadline=std::chrono::steady_clock::now()+std::chrono::nanoseconds(ticks*100);
    } else if (!threadWaitHandler_) return rejected(Name, "Infinite WaitAny has no executor");
    std::size_t active=0;
    for (const auto& [unused,waits]:dispatcherEventWaits_) { (void)unused; active+=waits.size(); }
    if (active>MaximumKernelHandles-count) return rejected(Name, "Direct-event wait capacity exhausted");
    const auto handler=threadWaitHandler_;
    const auto timedHandler=threadTimedWaitHandler_;
    auto result=rejected(Name, "WaitAny pending on the serialized executor");
    const auto group=std::make_shared<DispatcherWaitAnyGroup>();
    std::vector<std::shared_ptr<DispatcherEventWait>> registrations;
    registrations.reserve(count);
    for (std::uint32_t i=0;i<count;++i)
        registrations.push_back(std::make_shared<DispatcherEventWait>(DispatcherEventWait{headers[i],false,false,deadline,group,i,RECOMP_CURRENT_GUEST_TOKEN}));
    const auto cleanupLocked=[&] {
        for (std::uint32_t i=0;i<count;++i) {
            const auto found=dispatcherEventWaits_.find(addresses[i]);
            if (found==dispatcherEventWaits_.end()) continue;
            auto& waits=found->second;
            waits.erase(std::remove(waits.begin(),waits.end(),registrations[i]),waits.end());
            if (waits.empty()) dispatcherEventWaits_.erase(found);
        }
    };
    try {
        for (std::uint32_t i=0;i<count;++i) dispatcherEventWaits_[addresses[i]].push_back(registrations[i]);
    } catch (...) { cleanupLocked(); throw; }
    bool resolved=false;
    // PARALLEL: counted before the unlock; KeSetEvent wakes this token by name.
    [[maybe_unused]] ParkedRegistryWait parkedWait(this, 0, RECOMP_CURRENT_GUEST_TOKEN);
    RECOMP_REGISTRY_UNLOCK(lock);
    RegistryWakes::flushActive(); // PARALLEL: never park holding undelivered wakes.
    const auto cleanup=[&] { RECOMP_REGISTRY_LOCK(cleanupLock); cleanupLocked(); };
    try {
        const ThreadWaitReady ready=[&] {
            RECOMP_REGISTRY_LOCK(pollLock);
            if (resolved) return true;
            for (std::uint32_t i=0;i<count;++i) {
                std::uint32_t liveHeader=0;
                const auto found=dispatcherEventWaits_.find(addresses[i]);
                if (found==dispatcherEventWaits_.end() ||
                    std::find(found->second.begin(),found->second.end(),registrations[i])==found->second.end() ||
                    !readDispatcherEvent(addresses[i],liveHeader,signals[i]) || liveHeader!=headers[i]) {
                    result=rejected(Name, "WaitAny ownership/header changed while parked");
                    resolved=group->settled=true; return true;
                }
            }
            if (group->winner) {
                call.returnValue=*group->winner;
            } else if (deadline && std::chrono::steady_clock::now()>=*deadline) {
                call.returnValue=StatusTimeout;
            } else {
                for (std::uint32_t i=0;i<count;++i) if (signals[i]) {
                    group->winner=i;
                    if (headers[i]>>24) (void)writeU32(addresses[i]+4,0);
                    break;
                }
                if (!group->winner) return false;
                call.returnValue=*group->winner;
            }
            result=handled(Name, "WaitAny resolved from one real signal or a monotonic deadline");
            resolved=group->settled=true; return true;
        };
        TimelineWaitObject timelineObject(count?addresses[0]:0u); // v195 (WaitAny: the first object)
        const auto completed=deadline?timedHandler(ready,*deadline):handler(ready);
        cleanup();
        if (!completed || !resolved) return rejected(Name, "WaitAny exhausted its executor/budget");
        return result;
    } catch (...) { cleanup(); throw; }
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtWaitForSingleObjectEx(
        GuestImportCall& call, bool& pending, std::uint64_t& relativeTicks) {
    constexpr std::string_view Name{"__imp__NtWaitForSingleObjectEx"};
    call.returnValue = StatusInvalidParameter;
    if (call.argument32(1) > 1 || call.argument32(2) > 1) {
        return handled(Name, "Wait mode or alertable flag is invalid");
    }
    const auto timeout = call.argument32(3);
    std::uint32_t high = 0, low = 0;
    if (timeout != 0 && ((timeout & 7U) != 0 || !contains(timeout, 8)
            || !readU32(timeout, high) || !readU32(timeout + 4U, low))) {
        return memoryFault(Name, "Wait timeout is not a bounded guest LARGE_INTEGER");
    }
    const auto found = RECOMP_FIND_HANDLE(call.argument32(0)); // imports-fast D
    if (found == kernelHandles_.end()) {
        call.returnValue = StatusInvalidHandle;
        return handled(Name, "Wait handle is not owned by the registry");
    }
    if (RECOMP_CURRENT_GUEST_TOKEN == 0 || call.argument32(2) != 0) {
        return rejected(Name, "Wait requires a bound thread and non-alertable mode");
    }
    auto& object = found->second;
    if (object.kind == KernelObjectKind::ContentEnumerator) {
        return rejected(Name, "Content-enumerator synchronization is unsupported");
    }
    const auto acquired = acquireKernelObject(object, RECOMP_CURRENT_GUEST_TOKEN, call.returnValue);
    if (acquired == ObjectAcquireState::RecursionLimit) {
        return rejected(Name, "Mutant recursion exceeds the diagnostic cap");
    }
    if (acquired == ObjectAcquireState::Acquired) {
        return handled(Name, "Completed an immediate wait on a signaled object");
    }
    if (timeout != 0 && high == 0 && low == 0) {
        call.returnValue = StatusTimeout;
        return handled(Name, "An explicit zero-timeout wait found no signal");
    }
    if (timeout == 0 && threadWaitHandler_) {
        pending = true;
        return rejected(Name, "Infinite wait is pending on the explicitly bound cooperative handler");
    }
    if (timeout != 0 && (high & 0x80000000U) != 0 && threadTimedWaitHandler_) {
        const auto encoded = (std::uint64_t{high} << 32) | low;
        const auto ticks = (~encoded) + 1; // Negative relative time, 100-nanosecond units.
        if (ticks <= 600000000ULL) { // Explicit diagnostic cap: 60 seconds.
            relativeTicks = ticks;
            pending = true;
            return rejected(Name, "Relative wait is pending on the explicitly bound timed handler");
        }
    }
    return rejected(Name, "Unsignaled timed waits or unbound infinite waits are unsupported");
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtDuplicateObject(GuestImportCall& call) {
    constexpr auto Name = "__imp__NtDuplicateObject";
    const auto source = call.argument32(0), output = call.argument32(1), options = call.argument32(2);
    call.returnValue = StatusInvalidParameter;
    if (options > 1 || (!output && options != 1) || (source == 0xFFFFFFFEU && options != 0)) {
        return rejected(Name, "Unsupported duplication flags or missing output; pseudohandle close is not implemented");
    }
    if (output && !validGuestDataBuffer(output, 4, true)) {
        return memoryFault(Name, "Duplicate handle output must be a bounded writable ULONG");
    }
    ThreadObjectState* state = nullptr;
    if (source == 0xFFFFFFFEU) { state = findThreadObject(RECOMP_CURRENT_GUEST_TOKEN); }
    else {
        const auto handle = kernelHandles_.find(source);
        if (handle == kernelHandles_.end()) {
            call.returnValue = StatusInvalidHandle;
            return handled(Name, "Duplicate source is not an owned live handle");
        }
        // Threads already share lifetime, TLS, priority and suspend state by ID.
        // Other kinds need shared object/IO ownership, not a copied signal count.
        if (handle->second.kind != KernelObjectKind::Thread) {
            return rejected(Name, "Non-thread duplication requires shared dispatcher or IO object ownership");
        }
        for (auto& [token, thread] : threadObjects_) {
            if (thread.threadId == handle->second.threadId) { state = findThreadObject(token); break; }
        }
    }
    if (!state) { return rejected(Name, "Duplicate thread identity is unavailable or corrupted"); }
    auto result = handled(Name, "Issued a distinct handle to the same guest thread identity");
    auto noMemory = handled(Name, "Duplicate handle table allocation failed");
    if (!output) {
        ::recomp::imports_fast::eraseKey(kernelHandles_, source, ::recomp::imports_fast::handleGenerations());
        wakeHandleWaiters(this, source); // PARALLEL: a waiter must observe the closure.
        call.returnValue = StatusSuccess;
        return result;
    }
    std::uint32_t duplicate = 0;
    try { duplicate = createKernelHandle({KernelObjectKind::Thread, state->returned ? 1U : 0U, 0, state->threadId}); }
    catch (const std::bad_alloc&) { /* Report bounded allocation failure below. */ }
    // Xbox DUPLICATE_CLOSE_SOURCE closes the original even if duplication fails.
    if (options == 1) { ::recomp::imports_fast::eraseKey(kernelHandles_, source, ::recomp::imports_fast::handleGenerations()); wakeHandleWaiters(this, source); } // PARALLEL: see NtClose.
    (void)writeU32(output, duplicate);
    call.returnValue = duplicate ? StatusSuccess : StatusNoMemory;
    if (duplicate) { return result; }
    return noMemory;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtClose(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtClose"};
    const auto closing = kernelHandles_.find(call.argument32(0));
    if (closing != kernelHandles_.end()) {
        for (auto& [address, reference] : nativeEventReferences_) {
            (void)address;
            if (reference.handle == closing->first) {
                reference.closedObject = closing->second;
                reference.handle = 0; // References retain the event after handle closure.
            }
        }
    }
    call.returnValue = ::recomp::imports_fast::eraseKey(kernelHandles_, call.argument32(0), ::recomp::imports_fast::handleGenerations()) == 1
            ? StatusSuccess : StatusInvalidHandle;
#if RECOMP_PARALLEL_REGISTRY
    // PARALLEL: a parked waiter of the closed handle must observe the closure.
    if (call.returnValue == StatusSuccess) wakeHandleWaiters(this, call.argument32(0));
#endif
    notificationListeners_.erase(call.argument32(0));
    openFiles_.erase(call.argument32(0));
    contentEnumerators_.erase(call.argument32(0));
    return handled(Name, "Closed an owned handle or reported an invalid handle");
}

bool GuestImportRegistry::bindInstalledContent(const GuestContentCatalog& catalog, std::uint32_t title) {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || title == 0 || catalog.title() != title || installedContent_.title() != 0) { return false; }
    installedContent_ = catalog;
    return true;
}

bool GuestImportRegistry::bindContentPackage(std::uint32_t index, const GuestFileMetadataCatalog& metadata, const ContentFiles& files) {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || index == 0 || index > installedContent_.records().size()
            || contentPackages_.count(index) || metadata.size() > 256
            || metadata.pathAt(1) != std::optional<std::string>{"content:\\"} || files.empty()) { return false; }
    std::size_t total = files.size(), regular = 0;
    for (const auto& [unused, package] : contentPackages_) { (void)unused; total += package.files.size(); }
    if (total > 32) { return false; }
    for (std::uint32_t i = 1; i <= metadata.size(); ++i) {
        const auto path = metadata.pathAt(i);
        if (!path) { return false; }
        const auto entry = metadata.query(*path);
        if (entry.state != GuestFileQueryState::Found) { return false; }
        if (entry.metadata.attributes == 0x11) { continue; }
        const auto source = files.find(*path);
        if (entry.metadata.attributes != 1 || source == files.end() || !source->second
                || source->second->size() != entry.metadata.values[5] || source->second->size() > (8ULL << 30)) { return false; }
        ++regular;
    }
    if (regular != files.size()) { return false; }
    contentPackages_.emplace(index, ContentPackage{metadata, files});
    return true;
}

std::pair<std::uint32_t, std::string> GuestImportRegistry::contentPath(std::string_view path) const {
    const auto colon = path.find(':');
    if (colon == path.npos) { return {}; }
    const auto found = contentMounts_.find(std::string(path.substr(0, colon + 1)));
    return found == contentMounts_.end() ? std::pair<std::uint32_t, std::string>{}
            : std::pair{found->second, std::string("content:") + std::string(path.substr(colon + 1))};
}

GuestFileQueryResult GuestImportRegistry::queryFileMetadata(std::string_view path) const {
    const auto [index, relative] = contentPath(path);
    if (index != 0) { return contentPackages_.at(index).metadata.query(relative); }
    return fileMetadataQuery_ ? fileMetadataQuery_(path) : GuestFileQueryResult{};
}

GuestImportDispatchResult GuestImportRegistry::dispatchSymbolicLink(GuestImportCall& call,bool create) {
    const auto name=create?"__imp__ObCreateSymbolicLink":"__imp__ObDeleteSymbolicLink";
    const auto bounded=[&](std::uint32_t address,std::uint32_t size) {
        return address && contains(address,size) &&
            !(address<0x7FC90000 && std::uint64_t{address}+size>0x7FC80000);
    };
    const auto read=[&](std::uint32_t descriptor,std::string& text) {
        std::uint16_t length=0,maximum=0; std::uint32_t pointer=0;
        if ((descriptor&3) || !bounded(descriptor,8) || !readU16(descriptor,length) ||
            !readU16(descriptor+2,maximum) || !readU32(descriptor+4,pointer) ||
            !length || length>1024 || length>maximum || !bounded(pointer,length)) return false;
        text.assign(reinterpret_cast<const char*>(addressSpace_->translate(pointer)),length);
        return true;
    };
    try {
        std::string source,target;
        if (!read(call.argument32(0),source) || (create && !read(call.argument32(1),target)))
            return memoryFault(name,"Symbolic-link ANSI_STRING inputs are not bounded");
        const auto root=GuestSymbolicLinks::rootName(source);
        if (!root || (create && !normalizeGuestFilePath(target)))
            return rejected(name,"Only absolute guest DOS-root links and DOS targets are modeled");
        // Bound content/device roots are not aliases owned by this table. Never
        // remove or shadow them, even if a caller supplies a similar spelling.
        if (contentMounts_.count(*root) || (fileMetadataQuery_ &&
            fileMetadataQuery_(*root+"\\").state!=GuestFileQueryState::Unavailable))
            return rejected(name,"Explicit mounted namespaces cannot be replaced by a guest link");
        const bool success=create?symbolicLinks_.create(source,target):symbolicLinks_.erase(source);
        call.returnValue=success?StatusSuccess:0xC0000001U;
        return handled(name,success?"Updated only the native guest link namespace":"Guest link was absent, duplicate or outside bounded graph capacity");
    } catch (const std::bad_alloc&) {
        call.returnValue=StatusNoMemory; return handled(name,"Guest symbolic-link storage is exhausted");
    } catch (...) { return rejected(name,"Guest namespace provider failed"); }
}

std::shared_ptr<const GuestReadOnlyFile> GuestImportRegistry::openFileSource(std::string_view path) const {
    const auto [index, relative] = contentPath(path);
    if (index != 0) {
        const auto& files = contentPackages_.at(index).files;
        const auto found = files.find(relative);
        return found == files.end() ? nullptr : found->second;
    }
    return readOnlyFileOpen_ ? readOnlyFileOpen_(path) : nullptr;
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlTimeFieldsToTime(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlTimeFieldsToTime"};
    const auto input = call.argument32(0), output = call.argument32(1);
    if (input == 0 || (input & 1U) != 0 || !contains(input, 16)
            || output == 0 || (output & 7U) != 0 || !contains(output, 8)
            || overlapsProtectedThreadStorage(output, 8) || overlapsThreadTlsStorage(output, 8)
            || (input < std::uint64_t{output} + 8 && output < std::uint64_t{input} + 16)) {
        return memoryFault(Name, "Time conversion requires bounded disjoint input fields and external output");
    }
    std::array<std::int32_t, 8> fields{};
    for (std::uint32_t i = 0; i < fields.size(); ++i) {
        std::uint16_t value = 0; (void)readU16(input + i * 2, value);
        fields[i] = value < 0x8000 ? value : std::int32_t{value} - 0x10000;
    }
    const auto ticks = fileTimeFromFields(fields);
    auto result = handled(Name, "Converted Gregorian time fields or reported an invalid/unrepresentable date");
    if (!ticks) { call.returnValue = 0; return result; }
    (void)writeU32(output, static_cast<std::uint32_t>(*ticks >> 32));
    (void)writeU32(output + 4, static_cast<std::uint32_t>(*ticks));
    call.returnValue = 1;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchContentDeviceData(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XamContentGetDeviceData"};
    if (installedContent_.title() == 0 || contentPackages_.size() != installedContent_.records().size()) {
        return rejected(Name, "Content device requires every supplied package completely preopened");
    }
    if (call.argument32(0) != 1) { call.returnValue = 1167; return handled(Name, "Content device is not connected"); }
    const auto output = call.argument32(1);
    if (output == 0 || (output & 7U) != 0 || !contains(output, 80)
            || overlapsProtectedThreadStorage(output, 80) || overlapsThreadTlsStorage(output, 80)) {
        return memoryFault(Name, "Content device output is not a bounded external 80-byte structure");
    }
    std::uint64_t capacity = 0;
    for (const auto& [unused, package] : contentPackages_) {
        (void)unused;
        for (const auto& [path, source] : package.files) {
            (void)source;
            const auto allocation = package.metadata.query(path).metadata.values[4];
            if (allocation > UINT64_MAX - capacity) { return rejected(Name, "Content capacity sum overflow"); }
            capacity += allocation;
        }
    }
    auto result = handled(Name, "Reported explicit virtual read-only content capacity; no writable free space");
    std::memset(addressSpace_->translate(output), 0, 80);
    (void)writeU32(output, 1);
    (void)writeU32(output + 4, 1); // Fixed-storage compatibility class; not a physical Xbox HDD claim.
    (void)writeU32(output + 8, static_cast<std::uint32_t>(capacity >> 32));
    (void)writeU32(output + 12, static_cast<std::uint32_t>(capacity));
    // Free bytes remain zero: this source supports no writes or installation.
    constexpr std::string_view Label{"Native read-only content"};
    for (std::size_t i = 0; i < Label.size(); ++i) {
        addressSpace_->translate(output)[25 + i * 2] = static_cast<std::uint8_t>(Label[i]);
    }
    call.returnValue = 0;
    return result;
}

std::uint32_t GuestImportRegistry::contentMountRejection() const {
    RECOMP_REGISTRY_LOCK(lock); return contentMountRejection_;
}

GuestImportDispatchResult GuestImportRegistry::dispatchContentMount(GuestImportCall& call, bool close) {
    contentMountRejection_ = 0;
    const std::string_view name = close ? "__imp__XamContentClose" : "__imp__XamContentCreateEx";
    if (installedContent_.title() == 0) { contentMountRejection_ = 128; return rejected(name, "Content mounting requires an explicit title snapshot"); }
    const auto rootPointer = call.argument32(close ? 0 : 1);
    std::string root;
    if (!readAnsiString(rootPointer, 33, root)) { return memoryFault(name, "Content root is not a bounded string"); }
    const auto normalized = normalizeGuestFilePath(root + ":\\");
    if (!normalized || normalized->size() != root.size() + 2 || root.find_first_of(":/\\") != root.npos) {
        contentMountRejection_ = 1 | (!root.empty() && root[0] >= '0' && root[0] <= '9' ? 512U : 0U)
                | (!root.empty() && root.back() == ':' ? 1024U : 0U)
                | (root.find_first_of("/\\") != root.npos ? 2048U : 0U) | (root.empty() ? 4096U : 0U);
        return rejected(name, "Content root must be a simple DOS mount name");
    }
    root = normalized->substr(0, normalized->size() - 1);
    auto result = handled(name, close ? "Closed owned content namespace and its synchronous file handles"
                                     : "Opened an actual prebound read-only content package");
    if (close) {
        if (call.argument32(1) != 0) { contentMountRejection_ = 64; return rejected(name, "Asynchronous content close is unsupported"); }
        if (contentMounts_.erase(root) == 0) { call.returnValue = 2; return result; }
        for (auto it = openFiles_.begin(); it != openFiles_.end();) {
            if (it->second.contentRoot == root) { ::recomp::imports_fast::eraseKey(kernelHandles_, it->first, ::recomp::imports_fast::handleGenerations()); it = openFiles_.erase(it); }
            else { ++it; }
        }
        call.returnValue = 0;
        return result;
    }
    std::uint32_t overlapped = 0;
    if (!call.readArgument32(*addressSpace_, 8, overlapped)) { return memoryFault(name, "Content overlapped argument requires a bounded stack slot"); }
    const auto data = call.argument32(2), disposition = call.argument32(4);
    contentMountRejection_ = (call.argument32(0) != 0xFF ? 2U : 0U) | (call.argument32(3) != 3 ? 4U : 0U)
            | (call.argument32(5) != 0 ? 8U : 0U) | (call.argument32(6) != 0 ? 16U : 0U)
            | (call.arguments[7] != 0 ? 32U : 0U) | (overlapped != 0 ? 64U : 0U);
    if (contentMountRejection_ != 0) {
        return rejected(name, "Only non-profile synchronous OPEN_EXISTING without license queries or writes is supported");
    }
    if (data == 0 || (data & 3U) != 0 || !contains(data, GuestContentCatalog::RecordBytes)) {
        return memoryFault(name, "Content identity is not a bounded XCONTENT_DATA record");
    }
    if (disposition != 0) {
        const auto overlaps = [&](std::uint32_t at, std::uint64_t length) {
            return disposition < std::uint64_t{at} + length && at < std::uint64_t{disposition} + 4;
        };
        if ((disposition & 3U) != 0 || !contains(disposition, 4) || overlapsProtectedThreadStorage(disposition, 4)
                || overlapsThreadTlsStorage(disposition, 4) || overlaps(data, GuestContentCatalog::RecordBytes)
                || overlaps(rootPointer, root.size()) || overlaps(call.stackPointer + 0x54, 4)) {
            return memoryFault(name, "Content disposition must be bounded disjoint external storage");
        }
    }
    const auto* record = addressSpace_->translate(data);
    std::uint32_t index = 0;
    for (const auto& candidate : installedContent_.records()) {
        ++index;
        if (std::memcmp(record, candidate.data(), 8) == 0 && std::memcmp(record + 264, candidate.data() + 264, 42) == 0) { break; }
    }
    if (index == 0 || index > installedContent_.records().size()
            || std::memcmp(record, installedContent_.records()[index - 1].data(), 8) != 0
            || std::memcmp(record + 264, installedContent_.records()[index - 1].data() + 264, 42) != 0) {
        call.returnValue = 3; if (disposition) { (void)writeU32(disposition, 0); } return result;
    }
    if (!contentPackages_.count(index)) { contentMountRejection_ = 256; return rejected(name, "Catalog identity has no complete preopened file backend"); }
    if (contentMounts_.count(root) || symbolicLinks_.contains(root) ||
        (fileMetadataQuery_ && fileMetadataQuery_(*normalized).state != GuestFileQueryState::Unavailable)) {
        call.returnValue = 183; if (disposition) { (void)writeU32(disposition, 0); } return result;
    }
    if (contentMounts_.size() == GuestContentCatalog::MaximumRecords) { call.returnValue = 8; return result; }
    try { contentMounts_.emplace(root, index); }
    catch (const std::bad_alloc&) { call.returnValue = 8; return result; }
    if (disposition) { (void)writeU32(disposition, 2); }
    call.returnValue = 0;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchContentEnumeration(GuestImportCall& call, bool create) {
    const std::string_view name = create ? "__imp__XamContentCreateEnumerator" : "__imp__XamEnumerate";
    if (installedContent_.title() == 0) { return rejected(name, "Installed content requires an explicitly bound snapshot"); }
    const auto writable = [&](std::uint32_t address, std::uint64_t size) {
        return address != 0 && (address & 3U) == 0 && contains(address, size)
                && !overlapsProtectedThreadStorage(address, size) && !overlapsThreadTlsStorage(address, size);
    };
    if (create) {
        const auto user = call.argument32(0), device = call.argument32(1), type = call.argument32(2);
        const auto flags = call.argument32(3), capacity = call.argument32(4);
        const auto sizeOut = call.argument32(5), handleOut = call.argument32(6);
        if (user != 0xFE || device > 1 || type != 2 || flags != 0 || capacity == 0 || capacity > 64) {
            return rejected(name, "Only bounded title-context read-only marketplace enumeration is supported");
        }
        if (!writable(sizeOut, 4) || !writable(handleOut, 4) || sizeOut == handleOut) {
            return memoryFault(name, "Enumerator outputs must be disjoint bounded external words");
        }
        auto result = handled(name, "Created an owned enumerator over actual supplied title content");
        std::uint32_t handle = 0;
        try {
            handle = createKernelHandle({KernelObjectKind::ContentEnumerator, 0, 0});
            if (handle != 0) { contentEnumerators_.emplace(handle, ContentEnumerator{capacity, 0}); }
        } catch (const std::bad_alloc&) {
            if (handle != 0) { ::recomp::imports_fast::eraseKey(kernelHandles_, handle, ::recomp::imports_fast::handleGenerations()); }
            handle = 0;
        }
        if (handle == 0) { call.returnValue = 8; return result; } // ERROR_NOT_ENOUGH_MEMORY.
        (void)writeU32(sizeOut, capacity * GuestContentCatalog::RecordBytes);
        (void)writeU32(handleOut, handle);
        call.returnValue = 0;
        return result;
    }
    const auto handle = call.argument32(0), flags = call.argument32(1), buffer = call.argument32(2);
    const auto length = call.argument32(3), itemsOut = call.argument32(4), overlapped = call.argument32(5);
    // Xenia Canary 0e1307bd2e6bfeeff29635a6b823e72e61c97ce9 documents 0x20
    // as inclusion of optical-disc results. This explicit installed snapshot
    // has no separate live optical source; its existing rows remain eligible.
    if ((flags != 0 && flags != 0x20) || overlapped != 0) {
        return rejected(name, "Only synchronous installed-content enumeration with flags 0/0x20 is supported");
    }
    if (!writable(itemsOut, 4)) { return memoryFault(name, "Enumerator count output is not bounded external storage"); }
    auto result = handled(name, "Enumerated actual supplied content or reported exhaustion");
    const auto found = contentEnumerators_.find(handle);
    if (found == contentEnumerators_.end()) { call.returnValue = 6; (void)writeU32(itemsOut, 0); return result; }
    auto& enumerator = found->second;
    const auto required = enumerator.capacity * GuestContentCatalog::RecordBytes;
    // Xenia's reference omits buffer_length checking. Do not inherit that
    // omission: require the capacity returned by creation, cap the entire span.
    if (length > 64 * GuestContentCatalog::RecordBytes || !writable(buffer, length)
            || (buffer < std::uint64_t{itemsOut} + 4 && itemsOut < std::uint64_t{buffer} + length)) {
        return memoryFault(name, "Enumerator buffer is unbounded, protected or aliases count output");
    }
    if (length < required) { call.returnValue = 122; (void)writeU32(itemsOut, 0); return result; }
    const auto count = std::min<std::size_t>(enumerator.capacity, installedContent_.records().size() - enumerator.cursor);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& record = installedContent_.records()[enumerator.cursor + i];
        std::memcpy(addressSpace_->translate(buffer + static_cast<std::uint32_t>(i * record.size())), record.data(), record.size());
    }
    enumerator.cursor += count;
    (void)writeU32(itemsOut, static_cast<std::uint32_t>(count));
    call.returnValue = count == 0 ? 18 : 0; // ERROR_NO_MORE_FILES, not invented rows.
    return result;
}

bool GuestImportRegistry::bindNotificationService() {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || !addressSpace_->isReserved()) { return false; }
    notificationServiceBound_ = true;
    return true;
}

std::optional<std::uint32_t> GuestImportRegistry::notificationUiPosition() const {
    RECOMP_REGISTRY_LOCK(lock);
    return notificationUiPosition_;
}

bool GuestImportRegistry::bindSignedOutUsers() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved()) return false;
    signedOutUsersBound_ = true;
    return true;
}

bool GuestImportRegistry::bindNoRawStorageDevices() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved()) return false;
    noRawStorageDevicesBound_ = true;
    return true;
}

bool GuestImportRegistry::bindNoLaunchData() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved()) return false;
    noLaunchDataBound_ = true;
    return true;
}

bool GuestImportRegistry::bindMountedDisc(std::uint32_t xexHeader) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || mountedDisc_) return false;
    const auto info = GuestXexHeader::lookup(*addressSpace_, xexHeader, 0x00040006);
    if (!info.found() || info.kind != GuestXexHeaderValueKind::DataPointer
            || !contains(info.value, 24)) return false;
    const auto* bytes = addressSpace_->translate(info.value);
    if (!(bytes[12] | bytes[13] | bytes[14] | bytes[15])
            || !bytes[18] || bytes[18] > bytes[19]) return false;
    mountedDisc_ = bytes[18]; mountedDiscCount_ = bytes[19];
    return true;
}

std::optional<std::uint32_t> GuestImportRegistry::referenceNativeEvent(std::uint32_t handle) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!mountedDisc_) return std::nullopt;
    const auto event = kernelHandles_.find(handle);
    if (event == kernelHandles_.end() || (event->second.kind != KernelObjectKind::NotificationEvent
            && event->second.kind != KernelObjectKind::SynchronizationEvent)) return 0U;
    for (auto& [address, reference] : nativeEventReferences_) {
        if (reference.handle != handle) continue;
        if (reference.references == 0xFFFFFFFFU) return std::nullopt;
        ++reference.references;
        return address;
    }
    if (!nativeEventPool_) {
        if (!findFreeBlock(PageSize, PageSize, RECOMP_V(nativeEventPool_))) return std::nullopt;
        markVirtualCommit(nativeEventPool_, PageSize, true);
    }
    if (nativeEventSlots_ >= PageSize / 4) return std::nullopt;
    const auto address = nativeEventPool_ + nativeEventSlots_ * 4;
    nativeEventReferences_.emplace(address, NativeEventReference{handle, 1, event->second});
    ++nativeEventSlots_; // Never reuse a pointer identity before execution reset.
    return address;
}

std::array<std::uint32_t, 3> GuestImportRegistry::nativeDiscStatistics() const {
    RECOMP_REGISTRY_LOCK(lock);
    return {mountedDisc_, mountedDiscCount_, nativeDiscCompletions_};
}

bool GuestImportRegistry::bindUserLanguage(std::uint32_t language) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved() || userLanguage_
            || language == 0 || language == 10 || language > 17) return false;
    userLanguage_ = language;
    return true;
}

bool GuestImportRegistry::bindUserCountry(std::uint32_t country) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved() || userCountry_ || country == 0
            || country == 17 || country == 94 || country > 109) return false;
    userCountry_ = country;
    return true;
}

bool GuestImportRegistry::bindDisconnectedInput() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved() || disconnectedInputBound_ || nativeGamepadReader_) return false;
    disconnectedInputBound_ = true;
    return true;
}

bool GuestImportRegistry::bindNativeGamepad(NativeGamepadReader reader,NativeGamepadReader capabilities) {
    RECOMP_REGISTRY_LOCK(lock);
    if(!addressSpace_ || !addressSpace_->isReserved() || disconnectedInputBound_ || nativeGamepadReader_ || !reader || !capabilities)return false;
    nativeGamepadReader_=std::move(reader);nativeGamepadCapabilities_=std::move(capabilities);return true;
}

bool GuestImportRegistry::bindExperimentalAudioOutput(std::shared_ptr<GuestAudioBackend> backend) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved() || audioBackend_ || !backend) return false;
    const auto c=backend->configuration();
    if (!c || !validNativeAudioConfiguration(*c)) return false;
    audioBackend_=std::move(backend);return true;
}
bool GuestImportRegistry::bindNativeEventWakeBoostHandler(NativeEventWakeBoostHandler handler) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved() || !threadResumeHandler_ || !threadWaitHandler_ ||
        nativeEventWakeBoostHandler_ || !handler) return false;
    nativeEventWakeBoostHandler_=std::move(handler);return true;
}
GuestImportRegistry::NativeEventWakeBoostStatistics GuestImportRegistry::nativeEventWakeBoostStatistics() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeEventWakeBoostStatistics_;
}
bool GuestImportRegistry::bindExperimentalAudioRenderer(
        std::shared_ptr<audio::NativeAudioRenderClient> client,NativeCallbackEnqueue enqueue) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || !addressSpace_->isReserved() || !threadStorage_.ready() ||
        !exactFunctionValidator_ || audioRenderClient_ || !client || !enqueue ||
        client->backend().get()!=audioBackend_.get() || !client->canActivate()) return false;
    audioRenderClient_=std::move(client);audioCallbackEnqueue_=std::move(enqueue);return true;
}
bool GuestImportRegistry::bindNativeCategoryOneUnityGain() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!audioRenderClient_ || !audioRenderClient_->canActivate() || nativeCategoryOneGainBits_) return false;
    nativeCategoryOneGainBits_=0x3F800000;return true;
}
bool GuestImportRegistry::bindNativeXma(std::shared_ptr<GuestXmaBackend> backend) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!addressSpace_ || xmaBackend_ || !backend || (backend->virtualBase()&255) ||
        (backend->physicalBase()&255)) return false;
    const auto span=resolvePhysicalRange(backend->physicalBase(),GuestXmaBackend::PoolBytes,true);
    if (!span || span->virtualAddress!=backend->virtualBase() || span->allocationBase!=span->virtualAddress) return false;
    xmaBackend_=std::move(backend); return true;
}
std::uint8_t* GuestImportRegistry::mapNativeAudioPhysical(std::uint32_t physical,std::uint32_t bytes,bool writable) {
    // v152: the audio thread maps the same XMA buffers ~750k times/s (v148 prologue: 971M R acquisitions, 107 s of R
    // waits); the validation generation changes ~130 times/s. A hit returns the answer the locked path gave for the
    // same arguments at the current generation (see AudioMapEntry); anything else takes the unchanged locked path.
    const bool cacheOn=RECOMP_REGISTRY_READ_SNAPSHOT && audioMapCacheEnabled();
    auto* const live=__atomic_load_n(&addressSpace_,__ATOMIC_ACQUIRE);
    RegistryValidationSlot* slot=nullptr;
    if (cacheOn && live && (slot=registryFindSlot(this))) {
        const auto word=slot->word.load(std::memory_order_acquire);
        if (!(word&RegistryWindowMask)) {
            const auto& e=audioMapCache().entries[AudioMapCache::slot(physical,bytes,writable)];
            if (e.registry==this && e.space==live && e.generation==(word>>16) && e.physical==physical &&
                e.bytes==bytes && e.writable==writable) {
                registryCounters.audioMapHits.fetch_add(1,std::memory_order_relaxed);
                return e.pointer;
            }
        } else registryCounters.audioMapWindowOpen.fetch_add(1,std::memory_order_relaxed);
    }
    RECOMP_REGISTRY_LOCK(lock);
    if (cacheOn) registryCounters.audioMapLocked.fetch_add(1,std::memory_order_relaxed);
    const auto span=resolvePhysicalRange(physical,bytes,writable);
    if (!addressSpace_ || !span ||
        (span->virtualAddress<0x7FEB0000 && std::uint64_t(span->virtualAddress)+bytes>0x7FEA0000)) return nullptr;
    auto* const pointer=addressSpace_->translate(span->virtualAddress);
    if (cacheOn && pointer) {
        if (!slot) slot=registryFindSlot(this);
        const auto word=slot ? slot->word.load(std::memory_order_acquire) : RegistryWindowOne;
        if (!(word&RegistryWindowMask)) { // under R with no window open: V equals generation word>>16
            audioMapCache().entries[AudioMapCache::slot(physical,bytes,writable)]={this,addressSpace_,word>>16,physical,bytes,writable,pointer};
            registryCounters.audioMapStored.fetch_add(1,std::memory_order_relaxed);
        }
    }
    return pointer;
}
std::uint64_t GuestImportRegistry::nativeCategoryOneGainQueries() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeCategoryOneGainQueries_;
}
bool GuestImportRegistry::validGuestDataBuffer(std::uint32_t address,std::uint32_t bytes,bool writable,std::uint32_t alignment) const {
    const auto end=std::uint64_t{address}+bytes;
    if (!address || !bytes || !alignment || (alignment&(alignment-1)) || (address&(alignment-1)) || !contains(address,bytes) ||
        (address<0x7FC90000 && end>0x7FC80000) || (address<0x7FEB0000 && end>0x7FEA0000) || overlapsProtectedThreadStorage(address,bytes) ||
        overlapsThreadTlsStorage(address,bytes) ||
        (writable && imageProtections_.overlaps(address,bytes) && !imageProtections_.writable(address,bytes))) return false;
    if (physicalMemoryArenaBound_ && address<std::uint64_t{physicalArenaBase_}+physicalArenaSize_ && end>physicalArenaBase_) {
        if (address<physicalArenaBase_ ||
            !resolvePhysicalRange(physicalAddressOffset_+(address-physicalArenaBase_),bytes,writable)) return false;
    }
    if (address<std::uint64_t{heapBase_}+heapSize_ && end>heapBase_) {
        const auto after=allocations_.upper_bound(address);
        if (address<heapBase_ || after==allocations_.begin() ||
            end>std::uint64_t{std::prev(after)->first}+std::prev(after)->second) return false;
        for(auto page=(address-heapBase_)/PageSize;page<=(end-1-heapBase_)/PageSize;++page)
            if (!(virtualCommitBits_[page/64]&(std::uint64_t{1}<<(page%64)))) return false;
    }
    return true;
}
std::optional<bool> GuestImportRegistry::snapshotNativeInterruptDeviceRoot(std::uint32_t device) const {
    RECOMP_REGISTRY_LOCK(lock);
    if(!graphicsInitialized_ || !nativeInterruptDeviceRoot_ || !device ||
       !validGuestDataBuffer(device,21944,false,4))return std::nullopt;
    return nativeInterruptDeviceRoot_(device);
}
bool GuestImportRegistry::bindNativeInterruptDeviceRoot(std::function<bool(std::uint32_t)> validator) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!nativeIdentityScalerBound_ || graphicsInitialized_ || nativeInterruptDeviceRoot_ || !validator) return false;
    nativeInterruptDeviceRoot_=std::move(validator);return true;
}
bool GuestImportRegistry::validateNativeInterruptScopeLocked() {
    if (!nativeInterruptScope_ || nativeTileLockScope_ || nativeTileSceneInterruptScope_ || !virtualCpuAffinityBound_ || !graphicsInterruptRegistration_)
        return false;
    const auto& scope=*nativeInterruptScope_;
    if(scope.postOutputSwap && scope.deferredSwap)return false;
    const auto worker=scope.deferredSwap?deferredSwapInterruptWorker_:
        (scope.postOutputSwap?postOutputSwapInterruptWorker_:nativeCommandInterruptWorker_);
    const auto* thread=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    const auto allocation=allocations_.find(scope.allocationBase);
    if (!worker || RECOMP_CURRENT_GUEST_TOKEN!=scope.worker || *worker!=scope.worker || !thread ||
        thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4 ||
        graphicsInterruptRegistration_->userData!=scope.device || allocation==allocations_.end() ||
        allocation->second!=scope.allocationBytes || !validGuestDataBuffer(scope.device,23792,true)) return false;
    const auto owner=threadStorage_.lookup(thread->threadId);
    std::uint32_t pcrThread=0;
    return owner && owner->layout.pcrAddress==scope.pcr && owner->startRoutine==graphicsInterruptRegistration_->routine &&
        readU32(scope.pcr+0x100,pcrThread) && pcrThread==scope.worker &&
        *addressSpace_->translate(scope.pcr+0x10C)==2 && *addressSpace_->translate(scope.worker+0xBF)==2;
}
bool GuestImportRegistry::validateNativeTileLockScopeLocked() {
    if(!nativeTileLockScope_ || nativeInterruptScope_ || nativeTileSceneInterruptScope_ || !addressSpace_ || !virtualCpuAffinityBound_ ||
       !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !nativeInterruptDeviceRoot_ ||
       !graphicsInterruptRegistration_)return false;
    const auto& s=*nativeTileLockScope_;
    const auto* thread=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    const auto owner=allocations_.find(s.allocationBase);
    if(RECOMP_CURRENT_GUEST_TOKEN!=s.worker || !thread || thread->returned || thread->suspendCount ||
       !thread->virtualCpuMask || thread->virtualCpuMask>=64 ||
       (thread->virtualCpuMask&(thread->virtualCpuMask-1)) ||
       owner==allocations_.end() || owner->second!=s.allocationBytes ||
       graphicsInterruptRegistration_->userData!=s.device || !validGuestDataBuffer(s.device,21944,true) ||
       hasActiveCriticalSectionInRange(s.device+11156,4) || hasDispatcherWaitInRange(s.device+11156,4))return false;
    // Never borrow one of the dedicated source1 IRQ workers for level0 work.
    for(const auto irq:{nativeCommandInterruptWorker_,postOutputSwapInterruptWorker_,deferredSwapInterruptWorker_})
        if(irq && *irq==s.worker)return false;
    if(!thread->initial) {
        const auto owned=threadStorage_.lookup(thread->threadId);std::uint32_t pcrThread=0;
        if(!owned || !readU32(owned->layout.pcrAddress+0x100,pcrThread) || pcrThread!=s.worker)return false;
        const auto cpu=*addressSpace_->translate(owned->layout.pcrAddress+0x10C);
        if(cpu>=6 || thread->virtualCpuMask!=(1u<<cpu) || *addressSpace_->translate(s.worker+0xBF)!=cpu)return false;
    }
    try{return nativeInterruptDeviceRoot_(s.device);}catch(...){return false;}
}
GuestImportDispatchResult GuestImportRegistry::dispatchNativeTileSpinLock(std::string_view name,GuestImportCall& call) {
    const auto refuse=[&] {
        if(nativeTileLockScope_)nativeTileLockScope_->failed=true;
        return rejected(name,"Ordinary tile lock requires its current owner, exact Kf pair and saved native level0; no spin");
    };
    if(!validateNativeTileLockScopeLocked() || nativeTileLockScope_->failed)return refuse();
    auto& s=*nativeTileLockScope_;const bool acquire=name=="__imp__KfAcquireSpinLock";
    if(!acquire && name!="__imp__KfReleaseSpinLock")return refuse();
    std::uint32_t word=0;
    if(call.argument32(0)!=s.device+11156 || !readU32(s.device+11156,word) ||
       (acquire ? (s.held || s.pairs || word) : (!s.held || word!=1 || call.argument32(1)!=0)))return refuse();
    auto result=handled(name,acquire?"Acquired current ordinary tile lock at native level2":"Released ordinary tile lock to native level0");
    if(!writeU32(s.device+11156,acquire?1:0))return refuse();
    s.held=acquire;
    if(acquire)call.returnValue=0;else ++s.pairs;
    return result;
}
bool GuestImportRegistry::runNativeTileLockScope(std::uint32_t device,const NativeSerializedGate& gate,
        const std::function<void()>& invoke) {
    if(!gate || !invoke)return false;
    bool entered=false,balanced=false;
    const bool gated=gate([&] {
        if(entered){balanced=false;return;}entered=true;
        RECOMP_REGISTRY_UNIQUE(lock);
        if(nativeTileLockScope_ || nativeInterruptScope_ || nativeTileSceneInterruptScope_ || !device || device%4 || device<heapBase_ ||
           std::uint64_t{device}+21944>std::uint64_t{heapBase_}+heapSize_)return;
        const auto after=allocations_.upper_bound(device);
        if(after==allocations_.begin())return;
        const auto allocation=*std::prev(after);std::uint32_t word=1;
        nativeTileLockScope_=NativeTileLockScope{RECOMP_CURRENT_GUEST_TOKEN,device,allocation.first,allocation.second};
        if(!validateNativeTileLockScopeLocked() || !readU32(device+11156,word) || word) {
            nativeTileLockScope_.reset();return;
        }
        nativeTileLockScopeStatus_={true,false,false,false,RECOMP_CURRENT_GUEST_TOKEN,device,0,false};
        RECOMP_REGISTRY_UNLOCK(lock);
        const auto finish=[&](bool returned) {
            RECOMP_REGISTRY_RELOCK(lock);
            if(!nativeTileLockScope_){balanced=false;return;}
            const auto s=*nativeTileLockScope_;std::uint32_t current=1;
            balanced=returned && !s.failed && !s.held && s.pairs==1 && validateNativeTileLockScopeLocked() &&
                readU32(s.device+11156,current) && !current;
            nativeTileLockScopeStatus_={true,returned,balanced,s.failed,s.worker,s.device,s.pairs,s.held};
            nativeTileLockScope_.reset(); // Preserve held guest words on failure.
        };
        try{invoke();}catch(...){finish(false);throw;}
        finish(true);
    });
    return gated && entered && balanced;
}
GuestImportRegistry::NativeTileLockScopeStatus GuestImportRegistry::nativeTileLockScopeStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeTileLockScopeStatus_;
}
// v279 (see validateNativeTileSceneInterruptScopeLocked)
std::optional<GuestPreparedThread> GuestImportRegistry::createNativeTileSceneInterruptWorker(
        std::uint32_t device,std::uint32_t context,const NativeCommandInterruptEnqueue& enqueue) {
    RECOMP_REGISTRY_LOCK(lock);
    const auto* issuer=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    if(!enqueue || !issuer || issuer->initial || issuer->returned || issuer->suspendCount ||
       nativeTileSceneInterruptScope_ || nativeInterruptScope_ || nativeTileLockScope_ ||
       !addressSpace_ || !physicalMemoryArenaBound_ || !virtualCpuAffinityBound_ ||
       !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
       !graphicsInterruptRegistration_ || graphicsInterruptRegistration_->routine!=0x836B1768 ||
       graphicsInterruptRegistration_->userData!=device || !exactFunctionValidator_ ||
       !exactFunctionValidator_(0x836B1768) || !exactFunctionValidator_(0x836BE1C8) ||
       !nativeInterruptDeviceRoot_ || !device || device%4 || !validGuestDataBuffer(device,21944,true) ||
       context<0xC0000004 || context>=0xE0000000 || context%4)return std::nullopt;
    if(nativeTileSceneWorker_) {
        const auto* previous=findThreadObject(nativeTileSceneWorker_->token);
        // (v279: after an inline scene IRQ the balanced status names the delivering task, not the previous worker)
        if(!previous || !previous->returned || !nativeTileSceneInterruptStatus_.balanced ||
           nativeTileSceneInterruptStatus_.failed || (nativeTileSceneInterruptStatus_.worker!=nativeTileSceneWorker_->token && !nativeSceneLastInline))
            return std::nullopt;
        // DIAGNOSTIC v691: retain the 8 most recent returned IRQ workers; release
        // older ones (the creation rollback's four maps + pool slot) only if the
        // SAME thread id is still present and returned and no kernel handle names it.
        static std::deque<std::pair<std::uint32_t,std::uint32_t>> retained;
        const auto token=nativeTileSceneWorker_->token;const auto id=previous->threadId;
        if(retained.empty() || retained.back()!=std::make_pair(token,id))retained.emplace_back(token,id);
        while(retained.size()>8) {
            const auto [oldToken,oldId]=retained.front();retained.pop_front();
            const auto* old=findThreadObject(oldToken);
            if(!old || old->threadId!=oldId || !old->returned || old->suspendCount)continue;
            bool handle=false;
            for(const auto& [h,k]:kernelHandles_){(void)h;if(k.kind==KernelObjectKind::Thread && k.threadId==oldId)handle=true;}
            if(handle)continue;
            threadTlsValues_.erase(oldToken);RECOMP_V(threadTlsStorage_).erase(oldToken);createdThreads_.erase(oldToken);RECOMP_V(threadObjects_).erase(oldToken);
            const bool released=RECOMP_V(threadStorage_).release(oldId);
            static unsigned recycled=0;++recycled;
            if(recycled<=4 || !(recycled&(recycled-1)))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_WORKER_RECYCLED token=%08x id=%u released=%u total=%u",oldToken,oldId,unsigned(released),recycled);
        }
    }
    unsigned scratch=0;
    if(!readU32(device+11028,scratch) || scratch<physicalArenaBase_ || scratch%4)return std::nullopt;
    const auto physical=std::uint64_t{physicalAddressOffset_}+scratch-physicalArenaBase_;
    if(physical+32>0x20000000 ||
       (std::uint64_t{context-0xC0000000}<physical+32 && physical<std::uint64_t{context-0xC0000000}+4))return std::nullopt;
    const auto scratchRange=resolvePhysicalRange(static_cast<unsigned>(physical),32,true);
    const auto contextRange=resolvePhysicalRange(context-0xC0000000,4,false);
    if(!scratchRange || scratchRange->virtualAddress!=scratch || !contextRange)return std::nullopt;
    try {if(!nativeInterruptDeviceRoot_(device))return std::nullopt;}catch(...){return std::nullopt;}
    const auto worker=createNativeWorkerLocked(0x836B1768,device,false,2,[&](const auto& thread) {
        const auto entry=makeGuestCommandInterruptEntry(thread);
        return entry && enqueue(thread,*entry);
    });
    if(worker) {
        nativeTileSceneWorker_=NativeTileSceneWorker{worker->layout.threadAddress,device,context};
        nativeTileSceneInterruptStatus_={};
    }
    return worker; // No fallible operation or guest execution after enqueue.
}
std::optional<std::uint32_t> GuestImportRegistry::nativeTileSceneWaitingWorker(std::uint32_t device) const {
    RECOMP_REGISTRY_LOCK(lock);
    constexpr unsigned Slot=11576+2*108;
    if(!addressSpace_ || !virtualCpuAffinityBound_ || !nativeInterruptDeviceRoot_ || !device || device%4 ||
       !validGuestDataBuffer(device,21944,true))return std::nullopt;
    unsigned root=0,cpu=0,header=0,signal=0;
    if(!readU32(device+Slot,root) || root!=device+11080 || !readU32(device+Slot+4,cpu) || cpu!=2 ||
       !readDispatcherEvent(device+Slot+60,header,signal) || (header>>24)!=0 || signal)return std::nullopt;
    try {if(!nativeInterruptDeviceRoot_(device))return std::nullopt;}catch(...){return std::nullopt;}
    const auto found=dispatcherEventWaits_.find(device+Slot+60);
    if(found==dispatcherEventWaits_.end())return std::nullopt;
    std::optional<unsigned> token;const auto now=std::chrono::steady_clock::now();
    for(const auto& wait:found->second) {
        if(wait->header!=header || wait->group || wait->signaled || wait->settled ||
           (wait->deadline && now>=*wait->deadline))continue;
        const auto* thread=findThreadObject(wait->token);
        if(!thread || thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4 || token)
            return std::nullopt;
        token=wait->token;
    }
    return token;
}
// v279: set (on the delivering host thread only) by executeNativeTileSceneInline around its scene scope: the scope's task
// is then the delivering task, not the registry's CPU2 IRQ worker, so the worker-identity and worker-PCR checks below do
// not apply (the inline replay uses CPU2 itself); every device, scratch, event, allocation, registration and function check
// still does.
bool GuestImportRegistry::validateNativeTileSceneInterruptScopeLocked() {
    if(!nativeTileSceneInterruptScope_ || nativeInterruptScope_ || nativeTileLockScope_ ||
       !addressSpace_ || !virtualCpuAffinityBound_ || !graphicsInitialized_ || !graphicsBackend_ ||
       !graphicsBackend_->ready() || !nativeInterruptDeviceRoot_ || !nativeEventWakeBoostHandler_ ||
       !graphicsInterruptRegistration_ || !exactFunctionValidator_)return false;
    const auto& s=*nativeTileSceneInterruptScope_;
    const bool inlineScene=nativeSceneInlineActive; // v279
    if(!inlineScene && (!nativeTileSceneWorker_ || nativeTileSceneWorker_->token!=s.worker ||
       nativeTileSceneWorker_->device!=s.device || nativeTileSceneWorker_->context!=s.contextPhysical+0xC0000000))return false;
    if(s.failed || RECOMP_CURRENT_GUEST_TOKEN!=s.worker || graphicsInterruptRegistration_->routine!=0x836B1768 ||
       graphicsInterruptRegistration_->userData!=s.device || !exactFunctionValidator_(0x836B1768) ||
       !exactFunctionValidator_(0x836BE1C8))return false;
    for(const auto irq:{nativeCommandInterruptWorker_,postOutputSwapInterruptWorker_,deferredSwapInterruptWorker_})
        if(irq && *irq==s.worker)return false;
    const auto* t=findThreadObject(s.worker);const auto allocation=allocations_.find(s.allocationBase);
    if(!t || t->initial || t->returned || t->suspendCount || (!inlineScene && t->virtualCpuMask!=4) ||
       allocation==allocations_.end() || allocation->second!=s.allocationBytes || s.device<s.allocationBase ||
       std::uint64_t{s.device}+21944>std::uint64_t{s.allocationBase}+s.allocationBytes ||
       !validGuestDataBuffer(s.device,21944,true))return false;
    const auto thread=threadStorage_.lookup(t->threadId);unsigned pcrThread=0,scratch=0;
    if(!thread || (!inlineScene && (thread->startRoutine!=0x836B1768 || thread->startContext!=s.device || !makeGuestCommandInterruptEntry(*thread) ||
       thread->layout.pcrAddress!=s.pcr || !readU32(s.pcr+0x100,pcrThread) || pcrThread!=s.worker ||
       *addressSpace_->translate(s.pcr+0x10C)!=2 || *addressSpace_->translate(s.worker+0xBF)!=2)) ||
       (inlineScene && thread->layout.pcrAddress!=s.pcr) ||
       !readU32(s.device+11028,scratch) || scratch!=s.scratch)return false;
    const auto scratchRange=resolvePhysicalRange(s.scratchPhysical,32,true);
    const auto contextRange=resolvePhysicalRange(s.contextPhysical,4,false);
    if(!scratchRange || scratchRange->allocationBase!=s.scratchOwner || scratchRange->virtualAddress!=s.scratch ||
       !contextRange || contextRange->allocationBase!=s.contextOwner ||
       hasActiveCriticalSectionInRange(s.device+11032,4) || hasDispatcherWaitInRange(s.device+11032,4) ||
       hasActiveCriticalSectionInRange(s.event,16) ||
       hasActiveCriticalSectionInRange(s.event+24,24) || hasDispatcherWaitInRange(s.event+24,24) ||
       hasActiveCriticalSectionInRange(s.scratch,32) || hasDispatcherWaitInRange(s.scratch,32))return false;
    try{return nativeInterruptDeviceRoot_(s.device);}catch(...){return false;}
}
// v186 DIAGNOSTIC (refusals only, first 32 then powers of two): which scene-IRQ check refused, and for a device
// mismatch the first differing words (offset expected->actual) and the running guest token. Written to find what
// the v119/v171 split-domain guest stop compares (diag-domain-split-render).
static void logSceneIrqRefusal(std::uint32_t token,const char* where,const char* detail,const std::vector<std::uint8_t>* expected,
        const std::uint8_t* actual,std::size_t bytes) {
    static std::atomic<unsigned> refusals{0};const unsigned n=refusals.fetch_add(1,std::memory_order_relaxed)+1;
    if(n>32 && (n&(n-1)))return;
    char diff[400]{};int w=0;unsigned count=0;
    if(expected && actual && expected->size()==bytes)
        for(std::size_t at=0;at+4<=bytes;at+=4)if(std::memcmp(expected->data()+at,actual+at,4)) {
            if(++count<=8 && w>=0 && w<(int)sizeof diff-40) {
                const auto word=[](const std::uint8_t* b){return (unsigned(b[0])<<24)|(unsigned(b[1])<<16)|(unsigned(b[2])<<8)|b[3];};
                w+=std::snprintf(diff+w,sizeof diff-w," +%zu=%08x->%08x",at,word(expected->data()+at),word(actual+at));
            }
        }
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCENE_IRQ_REFUSED where=%s detail=%s token=%08x n=%u differing_words=%u%s",
        where,detail?detail:"-",token,n,count,diff);
}
GuestImportDispatchResult GuestImportRegistry::dispatchNativeTileSceneInterrupt(std::string_view name,GuestImportCall& call) {
    const char* why="?";
    const auto refuse=[&]{if(nativeTileSceneInterruptScope_)nativeTileSceneInterruptScope_->failed=true;
        logSceneIrqRefusal(RECOMP_CURRENT_GUEST_TOKEN,"dispatch",why,nativeTileSceneInterruptScope_?&nativeTileSceneInterruptScope_->expectedDevice:nullptr,
            nativeTileSceneInterruptScope_ && addressSpace_?addressSpace_->translate(nativeTileSceneInterruptScope_->device):nullptr,21944);
        return rejected(name,"Scene IRQ requires exact owned CPU2 event and raised acknowledgement pair; no completion");};
    why="validate";if(!validateNativeTileSceneInterruptScopeLocked())return refuse();
    auto& s=*nativeTileSceneInterruptScope_;
    why="device-at-entry";if(s.expectedDevice.size()!=21944 || std::memcmp(s.expectedDevice.data(),addressSpace_->translate(s.device),21944))return refuse();
    const auto setExpected=[&](unsigned at,unsigned value){for(unsigned b=0;b<4;++b)s.expectedDevice[at+b]=value>>(24-b*8);};
    if(name=="__imp__KeSetEvent") {
        why="set-event-args";if(s.events || s.held || s.pairs || call.argument32(0)!=s.event || call.argument32(1)!=1 || call.argument32(2))return refuse();
        auto result=dispatchKeEvent(call,true);
        why="set-event-dispatch";if(!result.handled())return refuse();
        setExpected(s.event-s.device+4,1);++s.events;
        why="device-after-set-event";if(std::memcmp(s.expectedDevice.data(),addressSpace_->translate(s.device),21944))return refuse();
        return result;
    }
    const bool acquire=name=="__imp__KeAcquireSpinLockAtRaisedIrql";
    why="spinlock-args";if((!acquire && name!="__imp__KeReleaseSpinLockFromRaisedIrql") || s.events!=1 || s.pairs ||
       call.argument32(0)!=s.device+11032 || s.held==acquire)return refuse();
    why=acquire?"acquire-mask":"release-mask";unsigned mask=0;if(!readU32(s.scratch,mask) || mask!=(acquire?4u:0u))return refuse();
    auto result=handled(name,"Exact scene source1 raised lock transition; void ABI and native exclusion preserved");
    if(!writeU32(s.device+11032,acquire?1:0))return refuse();
    setExpected(11032,acquire?1:0);s.held=acquire;if(!acquire)++s.pairs;
    return result;
}
bool GuestImportRegistry::runNativeTileSceneInterruptScope(std::uint32_t device,std::uint32_t context,
        const NativeSerializedGate& gate,const std::function<void()>& invoke) {
    if(!gate || !invoke)return false;
    bool entered=false,balanced=false;
    const bool gated=gate([&] {
        if(entered){balanced=false;return;}entered=true;
        RECOMP_REGISTRY_UNIQUE(lock);
        // v187 DIAGNOSTIC: each early return names its check (refusals only).
        const auto no=[&](const char* cond,unsigned a=0,unsigned b=0){
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCENE_IRQ_ENTRY cond=%s a=%08x b=%08x token=%08x device=%08x",
                cond,a,b,unsigned(RECOMP_CURRENT_GUEST_TOKEN),device);};
        if(nativeTileSceneInterruptScope_ || nativeInterruptScope_ || nativeTileLockScope_){no("scope-active",
            unsigned(bool(nativeTileSceneInterruptScope_))|(unsigned(bool(nativeInterruptScope_))<<1)|(unsigned(bool(nativeTileLockScope_))<<2));return;}
        if(!addressSpace_ ||
           !device || device%4 || !validGuestDataBuffer(device,21944,true) || context<0xC0000004 ||
           context>=0xE0000000 || context%4){no("device-context",context);return;}
        const auto after=allocations_.upper_bound(device);if(after==allocations_.begin()){no("allocation");return;}
        const auto allocation=*std::prev(after);const auto* t=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
        if(!t || t->initial){no("thread-object");return;}
        const auto thread=threadStorage_.lookup(t->threadId);if(!thread){no("thread-storage");return;}
        unsigned scratch=0,ack=1,header=0,signal=1,q=0,read=0,slotRoot=0,cpu=0,ordinary=1;
        constexpr unsigned Slot=11576+2*108;
        if(!readU32(device+11028,scratch) || scratch%4 || scratch<physicalArenaBase_){no("scratch",scratch);return;}
        if(!readU32(device+11032,ack) || ack){no("ack-held",ack);return;}
        if(!readU32(device+11156,ordinary) || ordinary){
            const auto o=plainSpinOwnersV188.find(device+11156); // v188: who holds it
            const std::uint32_t holder=o!=plainSpinOwnersV188.end()?o->second.first:0u;
            const auto* holderObject=holder?findThreadObject(holder):nullptr;
            const unsigned holderMask=holderObject?holderObject->virtualCpuMask:0u;
            // v189 (opt-in, files/private-trace/diag-scene-irq-foreign-ordinary): the ordinary tile lock is a
            // KfAcquireSpinLock (DISPATCH_LEVEL) lock. Held by a thread on another virtual CPU it does not mask the CPU2
            // graphics interrupt on the console, and the scene callback never takes it; the scope still requires the
            // whole device (the held word included) unchanged throughout. Held on CPU2 (or by an unknown owner, or by
            // this worker) the entry refuses as before.
            static const bool foreignAllowed=::access(RECOMP_APP_PATH("/private-trace/diag-scene-irq-foreign-ordinary"),F_OK)==0;
            if(!(foreignAllowed && ordinary==1 && holder && holder!=RECOMP_CURRENT_GUEST_TOKEN && holderMask && !(holderMask&4u))) {
                no("ordinary",holder?holder:0xFFFFFFFFu,(holderMask<<8)|(ordinary&0xFF));return;}
            static std::atomic<unsigned long long> foreignEntries{0};const auto n=foreignEntries.fetch_add(1,std::memory_order_relaxed)+1;
            if(n<=8 || !(n&(n-1)))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_SCENE_IRQ_FOREIGN_ORDINARY holder=%08x holder_mask=%x n=%llu",holder,holderMask,n);
        }
        if(!readDispatcherEvent(device+Slot+60,header,signal) || (header>>24)!=0 || signal){no("slot-event",header,signal);return;}
        if(!readU32(device+Slot,slotRoot) || slotRoot!=device+11080){no("slot-root",slotRoot);return;}
        if(!readU32(device+Slot+4,cpu) || cpu!=2){no("slot-cpu",cpu);return;}
        if(!readU32(device+Slot+84,q) || !readU32(device+Slot+88,read) || q!=read){no("slot-queue",q,read);return;}
        const auto physical=std::uint64_t{physicalAddressOffset_}+scratch-physicalArenaBase_;
        if(physical+32>0x20000000){no("physical",unsigned(physical));return;}
        const auto scratchRange=resolvePhysicalRange(static_cast<unsigned>(physical),32,true);
        const auto contextRange=resolvePhysicalRange(context-0xC0000000,4,false);
        if(!scratchRange || !contextRange || scratchRange->virtualAddress!=scratch ||
           (std::uint64_t{context-0xC0000000}<physical+32 && physical<std::uint64_t{context-0xC0000000}+4)){no("ranges",unsigned(physical),context);return;}
        unsigned mask=0,callback=0,argument=0;
        if(!readU32(scratch,mask) || mask!=4){no("mask",mask);return;}
        if(!readU32(scratch+16,callback) || callback!=0x836BE1C8){no("callback",callback);return;}
        if(!readU32(scratch+20,argument) || argument!=context){no("argument",argument,context);return;}
        NativeTileSceneInterruptScope s;s.worker=RECOMP_CURRENT_GUEST_TOKEN;s.pcr=thread->layout.pcrAddress;
        s.device=device;s.allocationBase=allocation.first;s.allocationBytes=allocation.second;
        s.scratch=scratch;s.scratchPhysical=static_cast<unsigned>(physical);s.scratchOwner=scratchRange->allocationBase;
        s.contextPhysical=context-0xC0000000;s.contextOwner=contextRange->allocationBase;s.event=device+Slot+60;
        s.expectedDevice.assign(addressSpace_->translate(device),addressSpace_->translate(device)+21944);
        s.pcrBefore.assign(addressSpace_->translate(s.pcr),addressSpace_->translate(s.pcr)+GuestThreadEnvironment::PcrSize);
        std::copy_n(addressSpace_->translate(scratch),32,s.scratchBefore.begin());
        const auto expected=[&](unsigned at,unsigned value){for(unsigned b=0;b<4;++b)s.expectedDevice[at+b]=value>>(24-b*8);};
        expected(Slot+92+(q&3)*4,context);expected(Slot+84,q+1);
        nativeTileSceneInterruptScope_=std::move(s);
        if(!validateNativeTileSceneInterruptScopeLocked()){nativeTileSceneInterruptScope_.reset();no("validate");return;}
        nativeTileSceneInterruptStatus_={true,false,false,false,RECOMP_CURRENT_GUEST_TOKEN,device,0,0,false};
        RECOMP_REGISTRY_UNLOCK(lock);
        const auto finish=[&](bool returned) {
            RECOMP_REGISTRY_RELOCK(lock);if(!nativeTileSceneInterruptScope_)return;
            auto& active=*nativeTileSceneInterruptScope_;
            auto scratchExpected=active.scratchBefore;std::fill_n(scratchExpected.begin(),4,0);
            balanced=returned && !active.failed && !active.held && active.pairs==1 && active.events==1 &&
                validateNativeTileSceneInterruptScopeLocked() &&
                !std::memcmp(active.expectedDevice.data(),addressSpace_->translate(device),21944) &&
                !std::memcmp(active.pcrBefore.data(),addressSpace_->translate(active.pcr),active.pcrBefore.size()) &&
                !std::memcmp(scratchExpected.data(),addressSpace_->translate(active.scratch),32);
            if(!balanced) { // v186 DIAGNOSTIC: which part of the balance failed
                char b[160];std::snprintf(b,sizeof b,"returned=%d failed=%d held=%d pairs=%u events=%u validate=%d device=%d pcr=%d scratch=%d",
                    int(returned),int(active.failed),int(active.held),unsigned(active.pairs),unsigned(active.events),
                    int(validateNativeTileSceneInterruptScopeLocked()),
                    int(!std::memcmp(active.expectedDevice.data(),addressSpace_->translate(device),21944)),
                    int(!std::memcmp(active.pcrBefore.data(),addressSpace_->translate(active.pcr),active.pcrBefore.size())),
                    int(!std::memcmp(scratchExpected.data(),addressSpace_->translate(active.scratch),32)));
                logSceneIrqRefusal(RECOMP_CURRENT_GUEST_TOKEN,"finish",b,&active.expectedDevice,addressSpace_->translate(device),21944);
            }
            nativeTileSceneInterruptStatus_={true,returned,balanced,active.failed,active.worker,device,active.pairs,active.events,active.held};
            nativeSceneLastInline=nativeSceneInlineActive; // v279
            nativeTileSceneInterruptScope_.reset(); // Never clear failed guest state.
        };
        try{invoke();}catch(...){finish(false);throw;}finish(true);
    });
    return gated && entered && balanced;
}
GuestImportRegistry::NativeTileSceneInterruptStatus GuestImportRegistry::nativeTileSceneInterruptStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeTileSceneInterruptStatus_;
}
GuestImportDispatchResult GuestImportRegistry::dispatchNativeInterruptSpinLock(std::string_view name,GuestImportCall& call) {
    if (!validateNativeInterruptScopeLocked())
        return rejected(name,"Spinlock requires the owned CPU2 non-yielding native interrupt scope");
    auto& scope=*nativeInterruptScope_;
    const bool raises=name=="__imp__KfAcquireSpinLock" || name=="__imp__KfReleaseSpinLock";
    const bool acquire=name=="__imp__KfAcquireSpinLock" || name=="__imp__KeAcquireSpinLockAtRaisedIrql";
    const unsigned index=raises?1:0,bit=1u<<index;
    const auto address=call.argument32(0);
    std::uint32_t word=0;
    if (address!=scope.locks[index] || !readU32(address,word) ||
        (acquire ? (scope.held || scope.pairs[index] || word) : (scope.held!=bit || word!=1)) ||
        (!acquire && raises && call.argument32(1)!=2))
        return rejected(name,"Native IRQ lock address, zero/owner state, pairing or saved level2 mismatch; no spin");
    // Original native encoding: BE0 unlocked / BE1 owned. The serialized
    // execution gate is the exclusion mechanism, not host-endian atomics.
    // Scope starts/ends at logical level2. Kf's saved level is therefore2;
    // raised-IRQL variants and void releases preserve r3. No APC dispatch.
    auto result=handled(name,acquire?"Acquired owned native interrupt lock at logical level2":"Released owned native interrupt lock at logical level2");
    (void)writeU32(address,acquire?1:0);
    scope.held=acquire?bit:0;
    if (!acquire) ++scope.pairs[index];
    if (acquire && raises) call.returnValue=2;
    return result;
}
GuestImportDispatchResult GuestImportRegistry::dispatchAudioRegister(GuestImportCall& call) {
    constexpr std::string_view Name="__imp__XAudioRegisterRenderDriverClient";
    if (!audioRenderClient_ || !audioCallbackEnqueue_ || !audioRenderClient_->canActivate())
        return rejected(Name,"Registration requires an unused explicit native PCM renderer");
    const auto descriptor=call.argument32(0),output=call.argument32(1);
    if (!validGuestDataBuffer(descriptor,8,false) || !validGuestDataBuffer(output,4,true))
        return memoryFault(Name,"Audio callback descriptor/output is not bounded owned storage");
    std::uint32_t callback=0,context=0;
    if (!readU32(descriptor,callback) || !readU32(descriptor+4,context) ||
        !exactFunctionValidator_ || !exactFunctionValidator_(callback))
        return rejected(Name,"Audio callback is not an exact translated entry");
    // Allocate diagnostic strings before the last fallible enqueue. Descriptor
    // is snapshotted even if its two words alias the output handle.
    auto success=handled(Name,"Registered one backed native PCM callback worker");
    const auto worker=createNativeCallbackWorkerLocked(callback,context,audioCallbackEnqueue_);
    if (!worker) return rejected(Name,"Native audio worker preparation/enqueue failed without publication");
    audioRenderClient_->publishWorker(worker->layout.threadAddress);
    (void)writeU32(output,audio::NativeAudioRenderClient::Handle);call.returnValue=0;
    // PARALLEL: a new producer (the callback worker); dependent waiter set unknown.
    // Frame submission needs no wake: no registry wait reads render-client state,
    // and the worker's own backpressure wait is a 1 ms deadline (trace_runner).
    wakeAllIfRegistryWaits();
    return success;
}
GuestImportDispatchResult GuestImportRegistry::dispatchAudioSubmit(GuestImportCall& call) {
    constexpr std::string_view Name="__imp__XAudioSubmitRenderDriverFrame";
    if (!audioRenderClient_) return rejected(Name,"PCM submission requires an explicit native renderer");
    const auto samples=call.argument32(1);
    if (!validGuestDataBuffer(samples,audio::AudioGuestPacketBytes,false))
        return memoryFault(Name,"PCM source is not a complete bounded readable guest frame");
    std::array<std::uint8_t,audio::AudioGuestPacketBytes> snapshot;
    std::memcpy(snapshot.data(),addressSpace_->translate(samples),snapshot.size());
    auto success=handled(Name,"Queued one owned PCM snapshot; acceptance is not playback");
    if (!audioRenderClient_->submit(RECOMP_CURRENT_GUEST_TOKEN,call.argument32(0),snapshot.data(),snapshot.size()))
        return rejected(Name,"PCM client/worker/frame/credit rejected without submission");
    call.returnValue=0;return success;
}
std::uint32_t GuestImportRegistry::audioSpeakerQueries() const {
    RECOMP_REGISTRY_LOCK(lock);return audioSpeakerQueries_;
}

bool GuestImportRegistry::bindNetworkLifecycle() {
    RECOMP_REGISTRY_LOCK(lock);
    if (findThreadObject(RECOMP_CURRENT_GUEST_TOKEN) == nullptr) { return false; }
    networkLifecycleBound_ = true;
    return true;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNetCleanup(GuestImportCall& call, bool winsock) {
    const std::string_view name = winsock ? "__imp__NetDll_WSACleanup" : "__imp__NetDll_XNetCleanup";
    const auto* thread = findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    if (!networkLifecycleBound_ || call.argument32(0) != 1 || thread == nullptr || thread->returned) {
        return rejected(name, "Cleanup requires the explicit empty title-network state and a live guest thread");
    }
    auto result = handled(name, "Released native network lifecycle state or reported Winsock uninitialized");
    if (winsock && winsockReferences_ == 0) {
        (void)writeU32(RECOMP_CURRENT_GUEST_TOKEN + 0x160, 10093); // Xbox guest per-thread last-error field.
        call.returnValue = 0xFFFFFFFFU; // SOCKET_ERROR.
    } else {
        if (winsock) { --winsockReferences_; }
        else { xnetConfigured_ = false; xnetParameters_ = {}; }
        call.returnValue = 0;
    }
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchWsaGetLastError(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NetDll_WSAGetLastError"};
    const auto* thread = findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    std::uint32_t value = 0;
    if (!networkLifecycleBound_ || thread == nullptr || thread->returned
            || !readU32(RECOMP_CURRENT_GUEST_TOKEN + 0x160, value)) {
        return rejected(Name, "Winsock last-error access needs a live explicitly bound guest thread");
    }
    call.returnValue = value; // No caller argument; same thread error storage as the Xbox wrapper.
    return handled(Name, "Read the guest thread last-error field");
}

GuestImportDispatchResult GuestImportRegistry::dispatchXNetStartup(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NetDll_XNetStartup"};
    if (!networkLifecycleBound_ || call.argument32(0) != 1) { return rejected(Name, "XNet configuration requires the explicit title lifecycle binding"); }
    const auto at = call.argument32(1);
    if (at == 0 || !contains(at, 13)) { return memoryFault(Name, "An explicit bounded XNetStartupParams record is required"); }
    std::array<std::uint8_t, 13> parameters{};
    std::memcpy(parameters.data(), addressSpace_->translate(at), parameters.size());
    if (parameters[0] != 13 || (parameters[1] & ~1U) != 0) { return rejected(Name, "Only the observed 13-byte configuration and flags 0/1 are supported"); }
    if (xnetConfigured_ && parameters != xnetParameters_) { return rejected(Name, "Changing active XNet configuration requires cleanup first"); }
    auto result = handled(Name, "Initialized bounded XNet configuration; transport remains unavailable");
    xnetParameters_ = parameters; xnetConfigured_ = true; call.returnValue = 0;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXNetGetOpt(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NetDll_XNetGetOpt"};
    if (!networkLifecycleBound_ || !xnetConfigured_ || call.argument32(0) != 1 || call.argument32(1) != 1) {
        return rejected(Name, "Only configured title XNet startup-parameter query option 1 is supported");
    }
    const auto output = call.argument32(2), sizeOutput = call.argument32(3);
    std::uint32_t capacity = 0;
    if (sizeOutput == 0 || (sizeOutput & 3U) != 0 || !readU32(sizeOutput, capacity)
            || overlapsProtectedThreadStorage(sizeOutput, 4)) { return memoryFault(Name, "XNet size pointer is invalid"); }
    auto result = handled(Name, "Copied the actual XNet configuration or returned the required size");
    if (capacity < xnetParameters_.size()) {
        (void)writeU32(sizeOutput, 13); call.returnValue = 10040; return result; // WSAEMSGSIZE.
    }
    if (output == 0 || !contains(output, 13) || overlapsProtectedThreadStorage(output, 13)
            || (output < static_cast<std::uint64_t>(sizeOutput) + 4 && sizeOutput < static_cast<std::uint64_t>(output) + 13)) {
        return memoryFault(Name, "XNet output must be separate from its size field and protected metadata");
    }
    std::memcpy(addressSpace_->translate(output), xnetParameters_.data(), 13);
    call.returnValue = 0;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchWsaStartup(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NetDll_WSAStartup"};
    if (!networkLifecycleBound_ || call.argument32(0) != 1) { return rejected(Name, "Winsock version negotiation requires the explicit title lifecycle binding"); }
    const auto version = call.argument32(1), output = call.argument32(2);
    if (version != 2 && version != 0x102 && version != 0x202) {
        call.returnValue = 10092; return handled(Name, "Requested Winsock version is not supported");
    }
    if (output == 0 || (output & 3U) != 0 || !contains(output, 0x190) || overlapsProtectedThreadStorage(output, 0x190)) {
        return memoryFault(Name, "WSADATA must be a bounded 400-byte guest record");
    }
    if (winsockReferences_ == UINT16_MAX) { return rejected(Name, "Winsock startup reference cap reached"); }
    auto result = handled(Name, "Negotiated native Winsock 2.x lifecycle; socket operations remain guarded");
    // X_WSADATA's obsolete vendor pointer (+0x18C) and adjacent bytes are opaque.
    // Do not copy Xenia's guessed socket/datagram limits; those fields are unused by 2.x.
    std::memset(addressSpace_->translate(output), 0, 0x18C);
    (void)writeU16(output, static_cast<std::uint16_t>(version));
    (void)writeU16(output + 2, 0x202);
    constexpr char Description[] = "Native diagnostic network lifecycle";
    constexpr char Status[] = "Transport not connected";
    std::memcpy(addressSpace_->translate(output + 4), Description, sizeof(Description));
    std::memcpy(addressSpace_->translate(output + 261), Status, sizeof(Status));
    ++winsockReferences_; call.returnValue = 0;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlNtStatusToDosError(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlNtStatusToDosError"};
    const auto status = call.argument32(0);
    std::uint32_t error = 0;
    // Bounded verified subset, not a guessed universal NTSTATUS translation.
    switch (status) {
    case 0: error = 0; break;
    case 0xC0000263U: error = 127; break; // DRIVER_ENTRYPOINT_NOT_FOUND -> PROC_NOT_FOUND.
    case 0xC0000008U: error = 6; break;
    case 0xC000000DU: error = 87; break;
    case 0xC000000FU: error = 2; break;
    // v161: the file/save statuses this registry returns, as the Xbox kernel's table maps them (checked against the
    // read-only Xenia reference, xboxkrnl_error.cc). GTA's ReadFile wrapper converts NtReadFile's status with this.
    case 0x00000103U: error = 997; break; // STATUS_PENDING -> ERROR_IO_PENDING
    case 0x80000005U: error = 234; break; // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
    case 0xC0000005U: error = 998; break; // STATUS_ACCESS_VIOLATION -> ERROR_NOACCESS
    case 0xC0000011U: error = 38; break;  // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
    case 0xC0000017U: error = 8; break;   // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022U: error = 5; break;   // STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED
    case 0xC0000023U: error = 122; break; // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
    case 0xC0000034U: error = 2; break;   // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
    case 0xC0000035U: error = 183; break; // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
    case 0xC000003AU: error = 3; break;   // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
    case 0xC000007FU: error = 112; break; // STATUS_DISK_FULL -> ERROR_DISK_FULL
    default: return rejected(Name, "NTSTATUS is outside the verified conversion subset");
    }
    call.returnValue = error;
    return handled(Name, "Converted a verified NTSTATUS to its Win32 error value");
}

void GuestImportRegistry::clearNativeXamModule() {
    if (nativeXamAddress_ != 0 && contains(nativeXamAddress_, NativeModuleRecordSize)) {
        std::memset(addressSpace_->translate(nativeXamAddress_), 0, NativeModuleRecordSize);
    }
    RECOMP_V(nativeXamAddress_) = RECOMP_V(nativeTitleAddress_) = 0;
    nativeXamLoadCount_ = 0;
    nativeSystemVersion_ = 0;
}

bool GuestImportRegistry::bindNativeXamModule(std::uint32_t address, std::uint32_t titleModule,
        const std::vector<GuestThreadMemoryRange>& excluded, std::uint32_t compatibilityVersion) {
    RECOMP_REGISTRY_LOCK(lock);
    if ((compatibilityVersion != 0 && compatibilityVersion >> 28 != 2)
            || !threadStorage_.ready() || nativeXamAddress_ != 0 || address == 0 || (address & 3U) != 0
            || titleModule == 0 || (titleModule & 3U) != 0 || !contains(titleModule, NativeModuleRecordSize)
            || !contains(address, NativeModuleRecordSize) || excluded.size() > 64
            || overlapsThreadArena(address, NativeModuleRecordSize)
            || overlapsProtectedThreadStorage(address, NativeModuleRecordSize)
            || overlapsThreadTlsStorage(address, NativeModuleRecordSize)) { return false; }
    const auto overlap = [&](std::uint32_t base, std::uint64_t size) {
        return address < static_cast<std::uint64_t>(base) + size
                && base < static_cast<std::uint64_t>(address) + NativeModuleRecordSize;
    };
    if (overlap(heapBase_, heapSize_) || overlap(threadStorageConfig_.image.base, threadStorageConfig_.image.size)
            || overlap(titleModule, NativeModuleRecordSize)
            || (physicalMemoryArenaBound_ && overlap(physicalArenaBase_, physicalArenaSize_))) { return false; }
    for (const auto& range : excluded) {
        if (!contains(range.base, range.size) || overlap(range.base, range.size)) { return false; }
    }
    { CsAll csAllGuard; for (const auto& [at, unused] : criticalSections_) { (void)unused; if (overlap(at, CriticalSectionSize)) { return false; } } } // (v197)
    const auto* bytes = addressSpace_->translate(address);
    if (std::any_of(bytes, bytes + NativeModuleRecordSize, [](auto byte) { return byte != 0; })) { return false; }
    RECOMP_V(nativeXamAddress_) = address;
    RECOMP_V(nativeTitleAddress_) = titleModule;
    nativeXamLoadCount_ = 1; // Permanently resident native service module, not loaded firmware.
    nativeSystemVersion_ = compatibilityVersion;
    (void)writeU16(address + 0x40, nativeXamLoadCount_);
    return true;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXexLoadImage(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XexLoadImage"};
    if (nativeXamAddress_ == 0) { return rejected(Name, "Native module environment is unbound"); }
    const auto output = call.argument32(3);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4) || overlapsProtectedThreadStorage(output, 4)) {
        return memoryFault(Name, "Module handle output must be a bounded external ULONG");
    }
    std::string name;
    if (!readAnsiString(call.argument32(0), 256, name)) { return memoryFault(Name, "Module name is not bounded"); }
    for (auto& c : name) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); } }
    if (name != "xam.xex" || call.argument32(1) != 9 || call.argument32(2) != 0) {
        return rejected(Name, "Only the observed flags-9 existing native XAM lookup is supported; no arbitrary XEX loading");
    }
    std::uint16_t count = 0;
    if (!readU16(nativeXamAddress_ + 0x40, count) || count != nativeXamLoadCount_ || count == UINT16_MAX) {
        return rejected(Name, "Native module load count is corrupted or exhausted");
    }
    auto result = handled(Name, "Referenced the resident partial native XAM service module");
    ++nativeXamLoadCount_;
    (void)writeU16(nativeXamAddress_ + 0x40, nativeXamLoadCount_);
    (void)writeU32(output, nativeXamAddress_);
    call.returnValue = StatusSuccess;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXexGetModuleHandle(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XexGetModuleHandle"};
    if (nativeXamAddress_ == 0) { return rejected(Name, "Native module environment is unbound"); }
    const auto output = call.argument32(1);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4) || overlapsProtectedThreadStorage(output, 4)) {
        return memoryFault(Name, "Module lookup output is not a bounded external ULONG");
    }
    auto address = nativeTitleAddress_;
    if (call.argument32(0) != 0) {
        std::string name;
        if (!readAnsiString(call.argument32(0), 256, name)) { return memoryFault(Name, "Module name is not bounded"); }
        for (auto& c : name) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); } }
        if (name != "xam.xex") { return rejected(Name, "Unconfigured module lookup is unsupported"); }
        address = nativeXamAddress_;
    }
    auto result = handled(Name, "Looked up an explicitly bound native or title module");
    (void)writeU32(output, address); call.returnValue = 0;
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXexGetProcedureAddress(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XexGetProcedureAddress"};
    if (nativeXamAddress_ == 0) { return rejected(Name, "Native module environment is unbound"); }
    const auto output = call.argument32(2);
    if (output == 0 || (output & 3U) != 0 || !contains(output, 4) || overlapsProtectedThreadStorage(output, 4)) {
        return memoryFault(Name, "Procedure output is not a bounded external ULONG");
    }
    if (call.argument32(0) == 0 || call.argument32(0) == nativeTitleAddress_) {
        return rejected(Name, "Title export resolution is not bound");
    }
    if (call.argument32(0) != nativeXamAddress_) {
        call.returnValue = StatusInvalidHandle;
        return handled(Name, "Procedure query does not identify the native XAM module");
    }
    // The native module has no Kinect microphone-array capture service.
    // These optional lookups have original null-pointer fallback paths; expose
    // absence, never a fabricated callable or synthetic microphone data.
    constexpr std::array<std::uint32_t, 11> UnavailableNativeExports{
        0xAFF, 0xB00, 0xB0B, 0xB10, 0x305, 0x30B, 0x50, 0x24,
        0x318, 0x31A, 0x48C};
    if (std::find(UnavailableNativeExports.begin(), UnavailableNativeExports.end(), call.argument32(1))
            == UnavailableNativeExports.end()) {
        return rejected(Name, "This export needs an actual native adapter or exact translated function binding");
    }
    auto result = handled(Name, "Requested optional party/UI, network or microphone-array export is unavailable in the native module");
    (void)writeU32(output, 0);
    call.returnValue = 0xC0000263U; // STATUS_DRIVER_ENTRYPOINT_NOT_FOUND, not a fake function pointer.
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXexUnloadImage(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XexUnloadImage"};
    if (nativeXamAddress_ == 0) { return rejected(Name, "Native module environment is unbound"); }
    call.returnValue = call.argument32(0) == nativeXamAddress_ ? StatusSuccess : StatusInvalidHandle;
    // Kernel service modules remain resident; guest unload cannot remove the runtime.
    return handled(Name, "Kept the native kernel service module resident or rejected an invalid handle");
}

GuestImportDispatchResult GuestImportRegistry::dispatchXamGetSystemVersion(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XamGetSystemVersion"};
    if (nativeSystemVersion_ == 0) { return rejected(Name, "System compatibility version must be explicitly bound from title metadata"); }
    call.returnValue = nativeSystemVersion_;
    return handled(Name, "Reported the configured native compatibility target, not an executing Xbox firmware version");
}

bool GuestImportRegistry::bindClockSources(SystemTimeSource source, std::uint32_t frequency) {
    RECOMP_REGISTRY_LOCK(lock);
    if (addressSpace_ == nullptr || !addressSpace_->isReserved() || systemTimeSource_ || !source || frequency == 0) { return false; }
    systemTimeSource_ = std::move(source);
    performanceFrequency_ = frequency;
    return true;
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeQuerySystemTime(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeQuerySystemTime"};
    if (!systemTimeSource_) { return rejected(Name, "System time requires an explicit clock source"); }
    const auto output = call.argument32(0);
    if (output != 0 && ((output & 7U) != 0 || !contains(output, 8) || overlapsProtectedThreadStorage(output, 8))) {
        return memoryFault(Name, "System time output must be a bounded guest LARGE_INTEGER");
    }
    const auto value = systemTimeSource_();
    if (!value || *value > static_cast<std::uint64_t>(INT64_MAX)) { return rejected(Name, "System clock source is unavailable or outside FILETIME range"); }
    auto result = handled(Name, "Wrote system time in big-endian 100-nanosecond FILETIME units");
    if (output != 0) {
        (void)writeU32(output, static_cast<std::uint32_t>(*value >> 32));
        (void)writeU32(output + 4, static_cast<std::uint32_t>(*value));
    }
    return result; // Void ABI preserves r3.
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeQueryPerformanceFrequency(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeQueryPerformanceFrequency"};
    if (performanceFrequency_ == 0) { return rejected(Name, "Performance frequency requires a matching explicit counter source"); }
    call.returnValue = performanceFrequency_;
    return handled(Name, "Reported the configured translated timebase frequency");
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeSetDisableBoostThread(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeSetDisableBoostThread"};
    auto* object = findThreadObject(call.argument32(0));
    if (!threadResumeHandler_ || object == nullptr || object->returned || call.argument32(1) > 1) {
        return rejected(Name, "Boost flag requires a live thread, Boolean input, and the cooperative policy");
    }
    auto result = handled(Name, "Updated boost-disable metadata; suppresses future explicitly bound native wake boosts");
    call.returnValue = object->disableBoost ? 1U : 0U;
    object->disableBoost = call.argument32(1) != 0;
    return result;
}

bool GuestImportRegistry::postNotification(std::uint32_t id, std::uint32_t parameter) {
    [[maybe_unused]] RegistryWakes wakes; // PARALLEL: fired after the unlock (any host thread).
    RECOMP_REGISTRY_LOCK(lock);
    if (!notificationServiceBound_ || addressSpace_ == nullptr || !addressSpace_->isReserved()) { return false; }
    // signin-ui: bit 31 (never part of an XNotificationKey) asks for an opened/closed pair of a system-UI style
    // notification: (id, 1) then (id, 0), under this one lock, to every matching listener that has room for both. A
    // listener without room gets neither: a lone "opened" would leave the title believing the UI stays up (GTA V's
    // sub_83231210 keeps the last XN_SYS_UI parameter). UNLIKE the plain post, this is per listener: a full one is
    // skipped and the others still get the pair. `parameter` names the listener whose result is returned (0: return
    // true only when every matching listener got the pair); a handle that is not a live matching listener is refused
    // before anything is queued. (No caller in the v465 sources; the request comes from generated_guest_import_bridge.cpp.)
    const bool pair = (id & 0x80000000U) != 0;
    id &= 0x7FFFFFFFU;
    // XNotificationKey: local ID 0..15, version 16..24, mask index 25..30.
    const auto bit = std::uint64_t{1} << ((id >> 25) & 63);
    const auto version = (id >> 16) & 511;
    const auto matches = [&](const auto& listener) {
        return (listener.mask & bit) != 0 && version <= listener.maximumVersion;
    };
    if (pair) {
        if (parameter != 0) {
            const auto named = notificationListeners_.find(parameter);
            if (named == notificationListeners_.end() || !matches(named->second)
                    || kernelHandles_.find(parameter) == kernelHandles_.end()) { return false; }
        }
        bool all = true, reported = false;
        for (auto& [handle, listener] : notificationListeners_) {
            if (!matches(listener)) { continue; }
            const auto object = kernelHandles_.find(handle);
            if (object == kernelHandles_.end() || listener.size + 2 > MaximumQueuedNotifications) { all = false; continue; }
            listener.queue[listener.size++] = {id, 1};
            listener.queue[listener.size++] = {id, 0};
            object->second.signalCount = 1;
            wakeHandleWaiters(this, handle); // PARALLEL: listener waiters, after the unlock.
            reported = reported || handle == parameter;
        }
        return parameter != 0 ? reported : all;
    }
    // Fixed-capacity queues make broadcast atomic: a full recipient changes none.
    for (const auto& [handle, listener] : notificationListeners_) {
        if (matches(listener) && (listener.size == MaximumQueuedNotifications
                || kernelHandles_.find(handle) == kernelHandles_.end())) { return false; }
    }
    for (auto& [handle, listener] : notificationListeners_) {
        if (!matches(listener)) { continue; }
        listener.queue[listener.size++] = {id, parameter};
        kernelHandles_.find(handle)->second.signalCount = 1;
        wakeHandleWaiters(this, handle); // PARALLEL: listener waiters, after the unlock.
    }
    return true;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXamNotifyCreateListener(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XamNotifyCreateListener"};
    if (!notificationServiceBound_) { return rejected(Name, "Notification service requires explicit event-source binding"); }
    if (call.argument32(1) > 10) { return rejected(Name, "Notification versions above 10 are not implemented"); }
    auto success = handled(Name, "Created a bounded listener with no fabricated startup notifications");
    auto failure = handled(Name, "Notification listener capacity or host allocation is exhausted");
    call.returnValue = 0;
    if (notificationListeners_.size() >= MaximumNotificationListeners) { return failure; }
    std::uint32_t handle = 0;
    try {
        handle = createKernelHandle({KernelObjectKind::NotificationListener, 0, 1});
        if (handle == 0) { return failure; }
        NotificationListener listener{};
        listener.mask = call.arguments[0]; // qword in r3, not two 32-bit GPR arguments.
        listener.maximumVersion = call.argument32(1);
        notificationListeners_.emplace(handle, std::move(listener));
    } catch (const std::bad_alloc&) {
        if (handle != 0) { ::recomp::imports_fast::eraseKey(kernelHandles_, handle, ::recomp::imports_fast::handleGenerations()); }
        return failure;
    }
    call.returnValue = handle;
    return success;
}

GuestImportDispatchResult GuestImportRegistry::dispatchXNotifyGetNext(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XNotifyGetNext"};
    if (!notificationServiceBound_) { return rejected(Name, "Notification service is unbound"); }
    const auto idOutput = call.argument32(2), parameterOutput = call.argument32(3);
    const auto valid = [&](std::uint32_t address) {
        return address == 0 || ((address & 3U) == 0 && contains(address, 4)
                && !overlapsProtectedThreadStorage(address, 4));
    };
    if (!valid(idOutput) || !valid(parameterOutput) || (idOutput != 0 && idOutput == parameterOutput)) {
        return memoryFault(Name, "Notification outputs must be separate bounded guest ULONGs");
    }
    auto result = handled(Name, "Dequeued matching notification or reported an empty/invalid listener");
    call.returnValue = 0;
    if (idOutput != 0) { (void)writeU32(idOutput, 0); }
    if (parameterOutput != 0) { (void)writeU32(parameterOutput, 0); }
    if (idOutput == 0) { return result; }
    const auto found = notificationListeners_.find(call.argument32(0));
    if (found == notificationListeners_.end()) { return result; }
    auto& listener = found->second;
    const auto match = call.argument32(1);
    for (std::size_t i = 0; i < listener.size; ++i) {
        if (match != 0 && listener.queue[i].id != match) { continue; }
        const auto event = listener.queue[i];
        for (std::size_t j = i + 1; j < listener.size; ++j) { listener.queue[j - 1] = listener.queue[j]; }
        listener.queue[--listener.size] = {};
        kernelHandles_.find(found->first)->second.signalCount = listener.size != 0 ? 1U : 0U;
        (void)writeU32(idOutput, event.id);
        if (parameterOutput != 0) { (void)writeU32(parameterOutput, event.parameter); }
        call.returnValue = 1;
        return result;
    }
    return result;
}

bool GuestImportRegistry::contains(std::uint32_t address, std::uint64_t size) const noexcept {
    return addressSpace_ != nullptr
            && addressSpace_->isReserved()
            && size <= GuestAddressSpace::Size
            && static_cast<std::uint64_t>(address) <= GuestAddressSpace::Size - size;
}

bool GuestImportRegistry::readU16(std::uint32_t address, std::uint16_t& value) const noexcept {
    if (!contains(address, 2)) {
        return false;
    }
    const auto* bytes = addressSpace_->translate(address);
    value = static_cast<std::uint16_t>(bytes[0] << 8U) | bytes[1];
    return true;
}

bool GuestImportRegistry::readU32(std::uint32_t address, std::uint32_t& value) const noexcept {
    if (!contains(address, 4)) {
        return false;
    }
    const auto* bytes = addressSpace_->translate(address);
    value = (static_cast<std::uint32_t>(bytes[0]) << 24U)
            | (static_cast<std::uint32_t>(bytes[1]) << 16U)
            | (static_cast<std::uint32_t>(bytes[2]) << 8U)
            | static_cast<std::uint32_t>(bytes[3]);
    return true;
}

bool GuestImportRegistry::writeU16(std::uint32_t address, std::uint16_t value) noexcept {
    if (!contains(address, 2)) {
        return false;
    }
    auto* bytes = addressSpace_->translate(address);
    bytes[0] = static_cast<std::uint8_t>(value >> 8U);
    bytes[1] = static_cast<std::uint8_t>(value);
    return true;
}

bool GuestImportRegistry::writeU32(std::uint32_t address, std::uint32_t value) noexcept {
    if (!contains(address, 4)) {
        return false;
    }
    auto* bytes = addressSpace_->translate(address);
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
    return true;
}

bool GuestImportRegistry::readAnsiString(
        std::uint32_t address,
        std::size_t maximumBytes,
        std::string& value) const {
    value.clear();
    if (address == 0 || !contains(address, 1)) {
        return false;
    }
    value.reserve(std::min<std::size_t>(maximumBytes, 256));
    for (std::size_t index = 0; index < maximumBytes; ++index) {
        const auto guestAddress = static_cast<std::uint64_t>(address) + index;
        if (guestAddress >= GuestAddressSpace::Size) {
            return false;
        }
        const auto byte = *addressSpace_->translate(static_cast<std::uint32_t>(guestAddress));
        if (byte == 0) {
            return true;
        }
        value.push_back(static_cast<char>(byte));
    }
    return false;
}

GuestImportDispatchResult GuestImportRegistry::dispatchDbgPrint(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__DbgPrint"};
    std::string message;
    if (!readAnsiString(call.argument32(0), MaximumDebugMessageBytes, message)) {
        call.returnValue = 0;
        return memoryFault(Name, "DbgPrint format string is null, out of range, or unterminated");
    }
    // Logging the literal template is intentional. Guest varargs require a
    // complete Xenon ABI formatter; treating guest text as a host format string
    // would be unsafe and would pretend unsupported formatting is implemented.
    logSink_(message);
    call.returnValue = static_cast<std::uint32_t>(message.size());
    return handled(Name, "Logged bounded guest format text without interpreting varargs");
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeGetCurrentProcessType(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeGetCurrentProcessType"};
    // Xbox titles execute as X_PROCTYPE_USER. Idle and system process types
    // describe kernel-owned execution contexts that this title registry never
    // creates or impersonates.
    call.returnValue = UserProcessType;
    return handled(Name, "Returned the Xbox title user-process type");
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeTlsAlloc(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeTlsAlloc"};
    call.returnValue = std::numeric_limits<std::uint32_t>::max();
    if (RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "No nonzero guest thread token is bound");
    }
    const auto capacity = tlsAllocationCapacity();
    for (std::size_t index = 0; index < capacity; ++index) {
        if (tlsSlots_.test(index)) {
            continue;
        }
        tlsSlots_.set(index);
        threadTlsValues_[RECOMP_CURRENT_GUEST_TOKEN][static_cast<std::uint32_t>(index)] = 0;
        for (const auto& [unusedToken, storage] : threadTlsStorage_) {
            (void)unusedToken;
            (void)writeU32(storage.dynamicSlotBase + static_cast<std::uint32_t>(index) * 4U, 0);
        }
        call.returnValue = index;
        syncTlsSlotMirror(this, tlsSlots_); // v252 (after the zeroing: a lock-free reader sees the bit only then)
        return handled(
                Name,
                "Allocated and zeroed one process TLS slot for the bound guest thread");
    }
    return handled(Name, "Portable process TLS slot table is exhausted");
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeTlsFree(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeTlsFree"};
    const auto index = call.argument32(0);
    call.returnValue = 0;
    if (index == std::numeric_limits<std::uint32_t>::max()) {
        return handled(Name, "Rejected the TLS-out-of-indexes sentinel");
    }
    if (index >= TlsSlotCount || !tlsSlots_.test(index)) {
        return handled(Name, "TLS slot is outside or absent from the portable allocation table");
    }
    tlsSlots_.reset(index);
#if !defined(V252_MUTANT_NO_FREE_SYNC) // host-test mutant: the mirror keeps a freed slot's bit
    syncTlsSlotMirror(this, tlsSlots_); // v252
#endif
    for (auto& [unusedToken, values] : threadTlsValues_) {
        (void)unusedToken;
        values.erase(index);
    }
    for (const auto& [unusedToken, storage] : threadTlsStorage_) {
        (void)unusedToken;
        (void)writeU32(storage.dynamicSlotBase + index * 4U, 0);
    }
    call.returnValue = 1;
    return handled(
            Name,
            "Freed one process TLS slot and cleared every tracked guest-thread value");
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeTlsGetValue(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeTlsGetValue"};
    call.returnValue = 0;
    if (RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "No nonzero guest thread token is bound");
    }
    const auto index = call.argument32(0);
    if (index >= TlsSlotCount || !tlsSlots_.test(index)) {
        return handled(Name, "TLS slot is outside or absent from the portable allocation table");
    }
    const auto storage = threadTlsStorage_.find(RECOMP_CURRENT_GUEST_TOKEN);
    if (storage != threadTlsStorage_.end()) {
        std::uint32_t value = 0;
        if (index >= storage->second.slotCount
                || !readU32(storage->second.dynamicSlotBase + index * 4U, value)) {
            return memoryFault(Name, "TLS slot is outside the bound guest storage");
        }
        call.returnValue = value;
        if (!v252Baseline()) // v252: remember the bound storage at this generation (R held: no window can open now)
            if (const auto* slot = registryFindSlot(this)) {
                const auto word = slot->word.load(std::memory_order_acquire);
                if ((word & RegistryWindowMask) == 0)
                    tlsReadCache = {this, RECOMP_CURRENT_GUEST_TOKEN, word >> 16, storage->second.dynamicSlotBase, storage->second.slotCount};
            }
        return handled(Name, "Read one big endian TLS value from bound guest storage");
    }
    const auto& values = threadTlsValues_[RECOMP_CURRENT_GUEST_TOKEN];
    const auto value = values.find(index);
    call.returnValue = value == values.end() ? 0 : value->second;
    return handled(Name, "Read one TLS value for the explicitly bound guest thread");
}

GuestImportDispatchResult GuestImportRegistry::dispatchKeTlsSetValue(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__KeTlsSetValue"};
    call.returnValue = 0;
    if (RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "No nonzero guest thread token is bound");
    }
    const auto index = call.argument32(0);
    if (index >= TlsSlotCount || !tlsSlots_.test(index)) {
        return handled(Name, "TLS slot is outside or absent from the portable allocation table");
    }
    const auto storage = threadTlsStorage_.find(RECOMP_CURRENT_GUEST_TOKEN);
    if (storage != threadTlsStorage_.end()
            && (index >= storage->second.slotCount
                || !writeU32(storage->second.dynamicSlotBase + index * 4U, call.argument32(1)))) {
        return memoryFault(Name, "TLS slot is outside the bound guest storage");
    }
    threadTlsValues_[RECOMP_CURRENT_GUEST_TOKEN][index] = call.argument32(1);
    call.returnValue = 1;
    return handled(Name, "Stored one TLS value for the explicitly bound guest thread");
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlImageXexHeaderField(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlImageXexHeaderField"};
    call.returnValue = 0;
    const auto lookup = GuestXexHeader::lookup(
            *addressSpace_,
            call.argument32(0),
            call.argument32(1));
    if (lookup.found()) {
        call.returnValue = lookup.value;
        return handled(Name, lookup.diagnostic);
    }
    if (lookup.state == GuestXexHeaderLookupState::NotFound) {
        return handled(Name, lookup.diagnostic);
    }
    return memoryFault(Name, lookup.diagnostic);
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlInitializeCriticalSection(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlInitializeCriticalSection"};
    auto initialization = call;
    initialization.arguments[1] = 0;
    auto result = dispatchRtlInitializeCriticalSectionAndSpinCount(initialization);
    // The no-spin entry is void; preserve the caller's return register.
    return {result.state, std::string(Name), std::move(result.diagnostic)};
}

GuestImportDispatchResult
GuestImportRegistry::dispatchRtlInitializeCriticalSectionAndSpinCount(
        GuestImportCall& call) {
    constexpr std::string_view Name{
            "__imp__RtlInitializeCriticalSectionAndSpinCount"};
    CsAll csAllGuard; // v197
    const auto address = call.argument32(0);
    const auto spinCount = call.argument32(1);
    call.returnValue = StatusInvalidParameter;
    if (address == 0 || !contains(address, CriticalSectionSize)) {
        return memoryFault(Name, "Critical-section storage is outside guest memory");
    }
    if (!validGuestDataBuffer(address, CriticalSectionSize, true, 4)) {
        return rejected(Name, "Critical-section initialization requires writable guest storage");
    }
    if (hasActiveCriticalSectionInRange(address, CriticalSectionSize)) {
        return rejected(Name, "Cannot reinitialize an owned or queued critical section");
    }
    // Xbox titles do not call RtlDeleteCriticalSection. Their allocators may
    // reuse an object's storage at a different offset, so inactive host shadow
    // entries must not permanently reserve those bytes. Validate every overlap
    // before retiring any entry; never discard an owner or a queued waiter.
    // v248 (A/B: files/private-trace/diag-v248-baseline = v247's scan of every entry): the map is address-ordered and the
    // entries never shrink (no RtlDeleteCriticalSection), so the scan cost grew with the run (v246 run: 47 k inits, 88 us
    // of R each). Only keys in (address-28, address+28) can overlap; erasing exactly those is the same set.
    if (&recomp_v248_baseline && recomp_v248_baseline.load(std::memory_order_relaxed)) {
        for (auto it = criticalSections_.begin(); it != criticalSections_.end();) {
            if (it->first < std::uint64_t{address} + CriticalSectionSize &&
                    address < std::uint64_t{it->first} + CriticalSectionSize) {
                it = ::recomp::imports_fast::eraseIterator(criticalSections_, it, ::recomp::imports_fast::csGenerations());
            } else {
                ++it;
            }
        }
    } else {
        const std::uint32_t from = address >= CriticalSectionSize ? address - (CriticalSectionSize - 1) : 0;
        for (auto it = criticalSections_.lower_bound(from);
             it != criticalSections_.end() && it->first < std::uint64_t{address} + CriticalSectionSize;) {
            if (address < std::uint64_t{it->first} + CriticalSectionSize) it = ::recomp::imports_fast::eraseIterator(criticalSections_, it, ::recomp::imports_fast::csGenerations());
            else ++it;
        }
    }
    auto* criticalSection = addressSpace_->translate(address);
    std::memset(criticalSection, 0, CriticalSectionSize);
    criticalSection[0] = 1;
    criticalSection[1] = static_cast<std::uint8_t>(std::min<std::uint64_t>(
            (static_cast<std::uint64_t>(spinCount) + 255U) >> 8U,
            255U));
    (void)writeU32(address + CriticalSectionLockCountOffset, 0xFFFFFFFFU);
    CriticalSectionState state;
    std::copy_n(
            criticalSection,
            state.dispatcherHeader.size(),
            state.dispatcherHeader.begin());
    criticalSections_[address] = state;
    call.returnValue = StatusSuccess;
    return handled(Name, "Initialized a bounded Xbox critical section with spin metadata");
}

bool GuestImportRegistry::criticalSectionRangeAvailable(std::uint32_t address) const noexcept {
    CsAll csAllGuard; // v197
    if (overlapsThreadTlsStorage(address, CriticalSectionSize)) {
        return false;
    }
    // v219: range-bounded (cs_range_query.h): [address, address+28) against every existing 28-byte section.
    return !anyActiveOverlap(criticalSections_, address, CriticalSectionSize, CriticalSectionSize, [](std::uint32_t,const auto&){return true;});
}

bool GuestImportRegistry::hasActiveCriticalSectionInRange(std::uint32_t address,std::uint64_t bytes) const {
    CsAll csAllGuard; // v197
    if (recomp_v217_baseline.load(std::memory_order_relaxed)) { // v217 A/B: v216's scan of every critical section
        for (const auto& [at,state]:criticalSections_) {
            if (at<std::uint64_t{address}+bytes && address<std::uint64_t{at}+CriticalSectionSize &&
                (state.ownerToken || criticalSectionWaits_.count(at))) return true;
        }
        return false;
    }
    // v217: only sections that can overlap [address, address+bytes) (cs_range_query.h; the map is ordered by
    // address), the same condition on the same entries (the scene-IRQ scope validation ran six full scans of ~1500
    // sections per ISR import: 35% of the IRQ workers' CPU, all of it while every other guest thread is stopped).
    return anyActiveOverlap(criticalSections_,address,bytes,CriticalSectionSize,[&](std::uint32_t at,const auto& state) {
        return state.ownerToken || criticalSectionWaits_.count(at); });
}

bool GuestImportRegistry::criticalSectionStorageMatches(
        std::uint32_t address,
        const CriticalSectionState& state) const noexcept {
    if (!contains(address, CriticalSectionSize)) {
        return false;
    }
    const auto* bytes = addressSpace_->translate(address);
    if (!std::equal(
                state.dispatcherHeader.begin(),
                state.dispatcherHeader.end(),
                bytes)) {
        return false;
    }
    std::uint32_t lockCount = 0;
    std::uint32_t recursionCount = 0;
    std::uint32_t ownerToken = 0;
    if (!readU32(address + CriticalSectionLockCountOffset, lockCount)
            || !readU32(address + CriticalSectionRecursionCountOffset, recursionCount)
            || !readU32(address + CriticalSectionOwningThreadOffset, ownerToken)) {
        return false;
    }
    const auto expectedLockCount = state.recursionCount == 0
            ? 0xFFFFFFFFU
            : state.recursionCount - 1U;
    return lockCount == expectedLockCount
            && recursionCount == state.recursionCount
            && ownerToken == state.ownerToken
            && ((state.ownerToken == 0) == (state.recursionCount == 0));
}

bool GuestImportRegistry::adoptPreinitializedCriticalSection(std::uint32_t address) {
    CsAll csAllGuard; // v197
    if (address == 0
            || !contains(address, CriticalSectionSize)
            || !criticalSectionRangeAvailable(address)) {
        return false;
    }
    const auto* bytes = addressSpace_->translate(address);
    if (bytes[0] != 1 || bytes[1] != 0 || bytes[2] != 4 || bytes[3] != 0) {
        return false;
    }
    std::uint32_t signalState = 0;
    std::uint32_t forwardLink = 0;
    std::uint32_t backwardLink = 0;
    std::uint32_t lockCount = 0;
    std::uint32_t recursionCount = 0;
    std::uint32_t ownerToken = 0;
    const auto selfLink = address + 8U;
    if (!readU32(address + 4U, signalState)
            || !readU32(address + 8U, forwardLink)
            || !readU32(address + 12U, backwardLink)
            || !readU32(address + CriticalSectionLockCountOffset, lockCount)
            || !readU32(address + CriticalSectionRecursionCountOffset, recursionCount)
            || !readU32(address + CriticalSectionOwningThreadOffset, ownerToken)
            || signalState != 0
            || forwardLink != selfLink
            || backwardLink != selfLink
            || lockCount != 0xFFFFFFFFU
            || recursionCount != 0
            || ownerToken != 0) {
        return false;
    }
    CriticalSectionState state;
    std::copy_n(bytes, state.dispatcherHeader.size(), state.dispatcherHeader.begin());
    criticalSections_.emplace(address, state);
    return true;
}

bool GuestImportRegistry::writeCriticalSectionState(
        std::uint32_t address,
        const CriticalSectionState& state) noexcept {
    const auto lockCount = state.recursionCount == 0
            ? 0xFFFFFFFFU
            : state.recursionCount - 1U;
    return writeU32(address + CriticalSectionLockCountOffset, lockCount)
            && writeU32(address + CriticalSectionRecursionCountOffset, state.recursionCount)
            && writeU32(address + CriticalSectionOwningThreadOffset, state.ownerToken);
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlEnterCriticalSection(
        GuestImportCall& call, std::unique_lock<std::mutex>& lock) {
    constexpr std::string_view Name{"__imp__RtlEnterCriticalSection"};
    CsAll csAll; // v197 (released across the contended park below)
    const auto address = call.argument32(0);
    const auto token = RECOMP_CURRENT_GUEST_TOKEN; // Other tasks may bind while this one parks.
    lastCriticalSectionRejection_.reset();csRejectionRecorded.store(false,std::memory_order_release); // (v197)
    const auto fail=[&](std::uint32_t reason,const char* message) {
        std::array<std::uint32_t,16> state{};state[0]=reason;state[1]=address;state[2]=token;
        const auto known=criticalSections_.find(address);
        if(known!=criticalSections_.end()) {state[3]=1;state[4]=known->second.ownerToken;state[5]=known->second.recursionCount;}
        if(address && !(address&3) && contains(address,CriticalSectionSize) &&
            !(address<0x7FC90000 && std::uint64_t{address}+CriticalSectionSize>0x7FC80000) &&
            !overlapsProtectedThreadStorage(address,CriticalSectionSize) && !overlapsThreadTlsStorage(address,CriticalSectionSize)) {
            state[6]=1;for(unsigned i=0;i<7;++i)(void)readU32(address+i*4,state[7+i]);
            state[14]=criticalSectionRangeAvailable(address);
            state[15]=known!=criticalSections_.end() && criticalSectionStorageMatches(address,known->second);
        }
        lastCriticalSectionRejection_=state;csRejectionRecorded.store(true,std::memory_order_release);return rejected(Name,message);
    };
    if (RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return fail(1, "No nonzero guest thread token is bound");
    }
    std::uint64_t mwSpentNs = 0; // main-wait (mw_cs_spin.h): time this enter watched the section before queueing
    bool mwWatched = false, mwNoMore = false;
mwRetry:
    auto criticalSection = criticalSections_.find(address);
    if (criticalSection == criticalSections_.end()) {
        if (!adoptPreinitializedCriticalSection(address)) {
            return fail(
                    2,
                    "Critical section is neither registry-initialized nor canonically preinitialized");
        }
        criticalSection = criticalSections_.find(address);
    }
    if (!criticalSectionStorageMatches(address, criticalSection->second)) {
        return fail(3, "Critical-section guest storage no longer matches registry state");
    }
    auto next = criticalSection->second;
    if ((next.ownerToken && next.ownerToken != token) || (!next.ownerToken && criticalSectionWaits_.count(address))) {
        if (!nativeCriticalSectionWaitsBound_)
            return fail(4, "Critical-section contention requires unsupported guest waits without an explicit native policy");
#if RECOMP_PARALLEL_REGISTRY
        // main-wait (diag-mw-cs-spin): the guest gave this section a spin count; before queueing, watch its owner word
        // (the lock and stripes released, nothing written, not queued), then re-run the unchanged acquisition from the top.
        if (const unsigned mwBudgetUs = ::recomp_mw_cs_spin_us.load(std::memory_order_relaxed);
            !mwNoMore && recomp_mw::shouldWatch(mwBudgetUs, criticalSection->second.dispatcherHeader[1], next.ownerToken, token,
                                                 criticalSectionWaits_.count(address) != 0, address, mwSpentNs) &&
            !guestExclusiveScopeActive() && !mwDomainContended()) {
            auto& mwc = recomp_mw::counters();
            mwc.watches.fetch_add(1, std::memory_order_relaxed);
            const std::uint8_t* mwOwner = addressSpace_->translate(address) + CriticalSectionOwningThreadOffset;
            std::uint64_t mwSpent = 0;
#if defined(RECOMP_MW_MUTANT_HOLD_LOCK) // host-test mutant: watches while holding the registry lock and stripes
            const auto mwEnd = recomp_mw::watchOwnerClear(mwOwner, std::uint64_t{mwBudgetUs} * 1000u - mwSpentNs,
                [] { return false; }, &mwSpent);
#else
            csAll.release();
            RECOMP_REGISTRY_UNLOCK(lock);
            RegistryWakes::flushActive(); // (as before a park: never hold undelivered wakes)
            const auto mwEnd = recomp_mw::watchOwnerClear(mwOwner, std::uint64_t{mwBudgetUs} * 1000u - mwSpentNs,
                [] { return guestExclusiveScopeActive() || mwDomainContended(); }, &mwSpent);
            RECOMP_REGISTRY_RELOCK(lock);
            csAll.acquire();
#endif
            mwSpentNs += mwSpent;
            mwc.watchNs.fetch_add(mwSpent, std::memory_order_relaxed);
            (mwEnd == recomp_mw::WatchEnd::Cleared ? mwc.cleared : mwEnd == recomp_mw::WatchEnd::Stopped ? mwc.stopped : mwc.expired)
                .fetch_add(1, std::memory_order_relaxed);
            mwWatched = true;
            mwNoMore = mwEnd == recomp_mw::WatchEnd::Stopped;
#if defined(RECOMP_MW_MUTANT_NO_RECHECK) // host-test mutant: a cleared watch takes the section without the FIFO check
            if (mwEnd == recomp_mw::WatchEnd::Cleared) {
                criticalSection = criticalSections_.find(address);
                if (criticalSection != criticalSections_.end() && !criticalSection->second.ownerToken) {
                    auto stolen = criticalSection->second; stolen.ownerToken = token; stolen.recursionCount = 1;
                    if (writeCriticalSectionState(address, stolen)) { criticalSection->second = stolen;
                        mwc.acquired.fetch_add(1, std::memory_order_relaxed);
                        return handled(Name, "Acquired a cooperative single-guest-thread critical section"); }
                }
            }
#endif
            goto mwRetry;
        }
        if (mwWatched) recomp_mw::counters().queued.fetch_add(1, std::memory_order_relaxed);
#endif
        const auto* waiter=findThreadObject(token);
        const auto* owner=next.ownerToken?findThreadObject(next.ownerToken):nullptr;
        if (!threadWaitHandler_ || !waiter || waiter->returned || waiter->suspendCount ||
            (next.ownerToken && (!owner || owner->returned)) || overlapsThreadArena(address,CriticalSectionSize) ||
            !validGuestDataBuffer(address,CriticalSectionSize,true) ||
            criticalSectionWaitStatistics_[3]>=65 || criticalSectionWaitStatistics_[0]==UINT64_MAX)
            return fail(6,"Native critical-section wait requires live identities and stable writable storage");
        for(const auto& [unused,waiters]:criticalSectionWaits_) {
            (void)unused;
            if(std::find(waiters.begin(),waiters.end(),token)!=waiters.end())
                return fail(6,"Thread already has a pending critical-section wait");
        }
        // Prepare all allocating state before publishing a queued waiter. No
        // guest bytes change until this same task actually resumes and acquires.
        const auto handler=threadWaitHandler_;
        auto success=handled(Name,"Acquired a released critical section through the native FIFO wait queue");
        auto [queue,inserted]=criticalSectionWaits_.try_emplace(address);
        try {queue->second.push_back(token);}
        catch (...) {if(inserted)criticalSectionWaits_.erase(queue);throw;}
        ++criticalSectionWaitStatistics_[0];++criticalSectionWaitStatistics_[3];
        bool acquired=false,readyObserved=false,registered=true;
        const auto cleanup=[&] {
            if(!registered)return;
            registered=false;
            const auto found=criticalSectionWaits_.find(address);
            if(found!=criticalSectionWaits_.end()) {
                auto& waits=found->second;
#if RECOMP_PARALLEL_REGISTRY
                const bool wasFront=!waits.empty() && waits.front()==token;
#endif
                waits.erase(std::remove(waits.begin(),waits.end(),token),waits.end());
#if RECOMP_PARALLEL_REGISTRY
                // PARALLEL: a front waiter leaving without the lock passes an unowned
                // section to the next queued waiter.
                if(wasFront && !waits.empty()) {
                    const auto section=criticalSections_.find(address);
                    if(section!=criticalSections_.end() && !section->second.ownerToken) {timelineSignal(3, address);RegistryWakes::wake(waits.front());}
                }
#endif
                if(waits.empty())criticalSectionWaits_.erase(found);
            }
            --criticalSectionWaitStatistics_[3];++criticalSectionWaitStatistics_[acquired?1:2];
        };
        // PARALLEL: counted before the unlock; RtlLeaveCriticalSection wakes the front.
        [[maybe_unused]] ParkedRegistryWait parkedWait(this, 0, token);
        try {
            csAll.release(); // v197: never park holding the stripes (the owner's fast leave needs them)
            RECOMP_REGISTRY_UNLOCK(lock);
            RegistryWakes::flushActive(); // PARALLEL: never park holding undelivered wakes.
            const ThreadWaitReady ready=[&] {
                RECOMP_REGISTRY_LOCK(pollLock);
                CsAll csPoll; // v197
                const auto section=criticalSections_.find(address);
                const auto waits=criticalSectionWaits_.find(address);
                const auto* liveWaiter=findThreadObject(token);
                if(section==criticalSections_.end() || waits==criticalSectionWaits_.end() ||
                    !liveWaiter || liveWaiter->returned || liveWaiter->suspendCount ||
                    !validGuestDataBuffer(address,CriticalSectionSize,true) ||
                    !criticalSectionStorageMatches(address,section->second)) return readyObserved=true;
                // Readiness reserves nothing and never assigns another task's
                // ownership. Cancellation after a ready poll leaves the lock free.
                readyObserved=waits->second.front()==token && !section->second.ownerToken;
                return readyObserved;
            };
            TimelineWaitObject timelineObject(address); // v195 (critical section)
            const bool completed=handler(ready);
            RECOMP_REGISTRY_RELOCK(lock);
            csAll.acquire(); // v197
            const auto section=criticalSections_.find(address);
            const auto waits=criticalSectionWaits_.find(address);
            const auto* liveWaiter=findThreadObject(token);
            if (!completed || !readyObserved || RECOMP_CURRENT_GUEST_TOKEN!=token ||
                !liveWaiter || liveWaiter->returned || liveWaiter->suspendCount ||
                section==criticalSections_.end() || waits==criticalSectionWaits_.end() ||
                waits->second.front()!=token || section->second.ownerToken ||
                !validGuestDataBuffer(address,CriticalSectionSize,true) ||
                !criticalSectionStorageMatches(address,section->second)) {
                cleanup();return fail(6,"Native critical-section wait cancelled or live storage/identity changed");
            }
            auto owned=section->second;owned.ownerToken=token;owned.recursionCount=1;
            if(!writeCriticalSectionState(address,owned)) {
                cleanup();return memoryFault(Name,"Could not publish a resumed critical-section acquisition");
            }
            section->second=owned;acquired=true;cleanup();return success;
        } catch (...) {
            if(!lock.owns_lock())RECOMP_REGISTRY_RELOCK(lock);
            if(!csAll.held())csAll.acquire(); // v197
            cleanup();throw;
        }
    }
    if (next.recursionCount == MaximumCriticalSectionRecursion) {
        return fail(5, "Critical-section recursion would overflow its signed guest field");
    }
    next.ownerToken = RECOMP_CURRENT_GUEST_TOKEN;
    ++next.recursionCount;
    if (!writeCriticalSectionState(address, next)) {
        return memoryFault(Name, "Could not update bounded critical-section guest storage");
    }
    criticalSection->second = next;
    if (mwWatched) recomp_mw::counters().acquired.fetch_add(1, std::memory_order_relaxed); // main-wait
    return handled(
            Name,
            next.recursionCount == 1
                    ? "Acquired a cooperative single-guest-thread critical section"
                    : "Recursively acquired a cooperative single-guest-thread critical section");
}

std::optional<std::array<std::uint32_t,16>> GuestImportRegistry::lastCriticalSectionRejection() const {
    RECOMP_REGISTRY_LOCK(lock);return lastCriticalSectionRejection_;
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlTryEnterCriticalSection(GuestImportCall& call) {
    CsAll csAllGuard; // v197
    constexpr std::string_view Name="__imp__RtlTryEnterCriticalSection";
    call.returnValue=0;
    const auto address=call.argument32(0),token=RECOMP_CURRENT_GUEST_TOKEN;
    if(!token)return rejected(Name,"Try-enter requires a bound nonzero guest thread identity");
    if(!validGuestDataBuffer(address,CriticalSectionSize,true))
        return memoryFault(Name,"Try-enter requires a bounded writable critical section");
    auto section=criticalSections_.find(address);
    if(section==criticalSections_.end()) {
        if(!adoptPreinitializedCriticalSection(address))
            return rejected(Name,"Try-enter requires a registry-initialized or canonical static critical section");
        section=criticalSections_.find(address);
    }
    if(!criticalSectionStorageMatches(address,section->second))
        return rejected(Name,"Try-enter guest storage no longer matches registry ownership");
    auto next=section->second;
    if((next.ownerToken && next.ownerToken!=token) || (!next.ownerToken && criticalSectionWaits_.count(address)))
        return handled(Name,"Critical section is busy; nonblocking attempt did not acquire or enqueue");
    if(next.recursionCount==MaximumCriticalSectionRecursion)
        return rejected(Name,"Try-enter recursion would overflow its signed guest field");
    auto success=handled(Name,"Nonblocking critical-section acquisition succeeded for this guest identity");
    next.ownerToken=token;++next.recursionCount;
    if(!writeCriticalSectionState(address,next))return memoryFault(Name,"Could not publish try-enter ownership");
    section->second=next;call.returnValue=1;return success;
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlLeaveCriticalSection(
        GuestImportCall& call) {
    CsAll csAllGuard; // v197
    constexpr std::string_view Name{"__imp__RtlLeaveCriticalSection"};
    const auto address = call.argument32(0);
    if (RECOMP_CURRENT_GUEST_TOKEN == 0) {
        return rejected(Name, "No nonzero guest thread token is bound");
    }
    const auto criticalSection = criticalSections_.find(address);
    if (criticalSection == criticalSections_.end()) {
        return rejected(Name, "Critical section was not initialized by this registry");
    }
    if (!criticalSectionStorageMatches(address, criticalSection->second)) {
        return rejected(Name, "Critical-section guest storage no longer matches registry state");
    }
    auto next = criticalSection->second;
    if (next.ownerToken != RECOMP_CURRENT_GUEST_TOKEN || next.recursionCount == 0) {
        return rejected(Name, "Bound guest thread does not own this critical section");
    }
    --next.recursionCount;
    if (next.recursionCount == 0) {
        next.ownerToken = 0;
    }
    if (!writeCriticalSectionState(address, next)) {
        return memoryFault(Name, "Could not update bounded critical-section guest storage");
    }
    criticalSection->second = next;
    if (!next.recursionCount && address == edramCalibrationLock_) edramPreparedThread_ = 0;
#if RECOMP_PARALLEL_REGISTRY
    // PARALLEL: the FIFO front re-evaluates after the unlock (the section is free).
    if (!next.recursionCount)
        if (const auto waits = criticalSectionWaits_.find(address); waits != criticalSectionWaits_.end() && !waits->second.empty()) {
            timelineSignal(3, address); // v195
            RegistryWakes::wake(waits->second.front());
        }
#endif
    return handled(
            Name,
            next.recursionCount == 0
                    ? "Released a cooperative single-guest-thread critical section"
                    : "Released one recursive critical-section acquisition");
}

GuestImportDispatchResult GuestImportRegistry::dispatchXexCheckExecutablePrivilege(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__XexCheckExecutablePrivilege"};
    call.returnValue = 0;
    const auto privilege = call.argument32(0);
    if (privilege >= 32) {
        return rejected(Name, "Executable privilege index exceeds the 32-bit system-flags field");
    }
    if (boundExecutableXexHeader_ == 0) {
        return rejected(Name, "No validated executable XEX header is bound");
    }
    const auto lookup = GuestXexHeader::lookup(
            *addressSpace_, boundExecutableXexHeader_, XexHeaderSystemFlags);
    if (lookup.state == GuestXexHeaderLookupState::NotFound) {
        return handled(Name, "Executable has no system-flags header; privilege is absent");
    }
    if (!lookup.found() || lookup.kind != GuestXexHeaderValueKind::Immediate) {
        return memoryFault(Name, lookup.diagnostic);
    }
    call.returnValue = (lookup.value & (std::uint32_t{1} << privilege)) != 0 ? 1 : 0;
    return handled(Name, "Checked one executable privilege bit in the validated XEX header");
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlCompareMemoryUlong(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlCompareMemoryUlong"};
    const auto source = call.argument32(0);
    const auto length = call.argument32(1);
    const auto pattern = call.argument32(2);
    call.returnValue = 0;
    if (source == 0 && length != 0) {
        return memoryFault(Name, "RtlCompareMemoryUlong source is null");
    }
    if ((source % alignof(std::uint32_t)) != 0 || (length % sizeof(std::uint32_t)) != 0) {
        return handled(Name, "Rejected unaligned source or non-ULONG length with a zero match");
    }
    if (!contains(source, length)) {
        return memoryFault(Name, "RtlCompareMemoryUlong range is outside guest memory");
    }
    std::uint32_t matched = 0;
    while (matched < length) {
        std::uint32_t value = 0;
        if (!readU32(source + matched, value)) {
            return memoryFault(Name, "RtlCompareMemoryUlong could not read the guest range");
        }
        if (value != pattern) {
            break;
        }
        matched += sizeof(std::uint32_t);
    }
    call.returnValue = matched;
    return handled(Name, "Compared a bounded big-endian ULONG range");
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlFillMemoryUlong(GuestImportCall& call) {
    constexpr std::string_view Name = "__imp__RtlFillMemoryUlong";
    // Native bounded import policy, not an Xbox API maximum. No partial fill on
    // unsupported requests. ULONG alignment and byte-count ABI follow ntifs.h.
    constexpr std::uint32_t MaximumBytes = 1024 * 1024;
    const auto destination = call.argument32(0), length = call.argument32(1), pattern = call.argument32(2);
    if ((destination & 3U) || (length & 3U) || length > MaximumBytes)
        return rejected(Name, "ULONG fill requires aligned storage, whole words and at most1MiB");
    // An empty aligned range has no memory access, including a null pointer.
    if (!length) return handled(Name, "Empty ULONG fill; no memory accessed");
    if (!validGuestDataBuffer(destination, length, true))
        return memoryFault(Name, "ULONG fill destination is not bounded writable guest data");
    const auto overlaps = [&](std::uint32_t base, std::uint32_t bytes) {
        return std::uint64_t{destination} < std::uint64_t{base} + bytes &&
            std::uint64_t{base} < std::uint64_t{destination} + length;
    };
    { CsAll csAllGuard; if (anyActiveOverlap(criticalSections_, destination, length, CriticalSectionSize, [](std::uint32_t,const auto&){return true;})) // v219 (v197 guard)
        return rejected(Name, "ULONG fill aliases a registered critical section"); }
    if (anyActiveOverlap(dispatcherEventWaits_, destination, length, 16, [](std::uint32_t,const auto&){return true;})) // v219: range-bounded
        return rejected(Name, "ULONG fill aliases a registered dispatcher event");
    if (nativeScalerTimingBinding_ && (overlaps((*nativeScalerTimingBinding_)[0], 4) ||
            overlaps((*nativeScalerTimingBinding_)[1], 4)))
        return rejected(Name, "ULONG fill aliases native clock binding");
    // A pinned allocation is a live graphics dependency, not scratch storage.
    const auto aliasesPinned = [&](std::uint32_t base) {
        const auto owner = physicalAllocations_.find(base);
        return owner == physicalAllocations_.end() || overlaps(base, owner->second.size);
    };
    for (const auto& owner : {graphicsRingAllocation_, graphicsWriteBackAllocation_, graphicsIdentifierAllocation_})
        if (owner && aliasesPinned(*owner)) return rejected(Name, "ULONG fill aliases pinned graphics control storage");
    if (pendingNativeSwap_)
        for (const auto owner : pendingNativeSwap_->pinnedAllocations)
            if (aliasesPinned(owner)) return rejected(Name, "ULONG fill aliases a pending native swap allocation");
    if (pendingPostOutputSwap_)
        for (const auto owner : pendingPostOutputSwap_->pinnedAllocations)
            if (aliasesPinned(owner)) return rejected(Name, "ULONG fill aliases a pending post-output swap allocation");
    if (pendingDeferredSwap_)
        for (const auto owner : pendingDeferredSwap_->pinnedAllocations)
            if (aliasesPinned(owner)) return rejected(Name, "ULONG fill aliases a deferred native swap allocation");
    for(unsigned i=0;i<deferredColorPinCount_;++i)
        if(aliasesPinned(deferredColorPinnedAllocations_[i]))return rejected(Name,"ULONG fill aliases the unretired deferred prefix");
    if (nativeScalerStorageOwner_ && aliasesPinned(nativeScalerStorageOwner_->virtualBase))
        return rejected(Name,"ULONG fill aliases pinned native scaler command storage");
    // Preflight and diagnostic allocation precede the first write. The mutex
    // remains held; no callback, allocation or fallible operation during commit.
    auto result = handled(Name, "Filled a preflighted big-endian ULONG range; void ABI preserved");
    auto* output = addressSpace_->translate(destination);
    const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(pattern >> 24),
        static_cast<std::uint8_t>(pattern >> 16), static_cast<std::uint8_t>(pattern >> 8), static_cast<std::uint8_t>(pattern)};
    for (std::uint32_t offset = 0; offset < length; offset += 4)
        std::memcpy(output + offset, bytes.data(), bytes.size());
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchNativeIdentityScaler(GuestImportCall& call) {
    constexpr std::string_view Name = "__imp__VdInitializeScalerCommandBuffer";
    using Candidate = recomp::gpu::NativeIdentityScalerCandidate;
    using Plan = recomp::gpu::NativeIdentityScalerPlan;
    using Command = recomp::gpu::NativeScalerCommand;
    if (!nativeIdentityScalerBound_)
        return {GuestImportDispatchState::NotRegistered,std::string(Name),"Native identity scaler producer is not explicitly bound"};
    if (pendingNativeScaler_ || !validateNativeScalerStorageLocked() || !nativeScalerTimingBinding_ || !virtualVideoModeBound_ ||
        !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
        !graphicsRingConfiguration_ || !graphicsRingConfiguration_->writeBack || !completedWrappedSwap_ ||
        !nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries!=1 ||
        pendingNativeSwap_ || !nativeSwapStatus_.presentQueued ||
        graphicsRingConfiguration_->readWord!=completedWrappedSwap_->readWord ||
        nativeSwapStatus_.readWord!=graphicsRingConfiguration_->readWord)
        return rejected(Name,"Native identity scaler requires one completed ordered swap and selected output, with no prior scaler request");
    const auto& ring=*graphicsRingConfiguration_;const auto& timing=*nativeScalerTimingBinding_;
    std::uint32_t read=0,pointer=0,clock=0;
    if (!readU32(ring.writeBack->virtualAddress,read) || read!=ring.readWord ||
        !readU32(timing[0],pointer) || pointer!=timing[1] || !readU32(timing[1],clock) ||
        clock!=recomp::gpu::NativeScalerTiming::NominalGpuMHz)
        return rejected(Name,"Native identity scaler lost ring or timing continuity");
    constexpr std::uint32_t FrameBytes=2464, CapacityWords=200;
    const auto sp=std::uint64_t{call.stackPointer};
    const auto* thread=findThreadObject(RECOMP_CURRENT_GUEST_TOKEN);
    if (!thread || thread->initial || thread->returned || thread->suspendCount)
        return rejected(Name,"Native identity scaler requires a live owned worker stack");
    const auto worker=threadStorage_.lookup(thread->threadId);
    if (!worker || !sp || (sp&15) || sp<worker->layout.stackBase ||
        sp+FrameBytes>std::uint64_t{worker->layout.stackBase}+worker->layout.stackSize ||
        !validGuestDataBuffer(call.stackPointer,FrameBytes,true,16) || imageProtections_.overlaps(call.stackPointer,FrameBytes))
        return memoryFault(Name,"Scaler inputs and destination do not fit the current worker frame");
    const auto overlaps=[&](std::uint32_t base,std::uint32_t size) {
        return sp<std::uint64_t{base}+size && std::uint64_t{base}<sp+FrameBytes;
    };
    if (overlaps(heapBase_,heapSize_) || (physicalMemoryArenaBound_ && overlaps(physicalArenaBase_,physicalArenaSize_)) ||
        overlaps(timing[0],4) || overlaps(timing[1],4)) return memoryFault(Name,"Scaler frame aliases non-stack state");
    { CsAll csAllGuard; for (const auto& entry:criticalSections_) if (overlaps(entry.first,CriticalSectionSize))
        return rejected(Name,"Scaler frame aliases a registered critical section"); } // (v197)
    for (const auto& entry:dispatcherEventWaits_) if (overlaps(entry.first,16))
        return rejected(Name,"Scaler frame aliases a registered dispatcher wait");
    // The complete frame is owned before reading any stack arguments/pointers.
    // This exact observed12argument layout is deliberately a bounded profile,
    // not a general Xbox scaler implementation or an arbitrary stack write.
    std::array<std::uint32_t,4> tail{};
    for (unsigned i=0;i<4;++i) if (!call.readArgument32(*addressSpace_,8+i,tail[i]))
        return memoryFault(Name,"Scaler stack arguments are unavailable");
    if (call.argument32(6)!=sp+156 || tail!=std::array<std::uint32_t,4>{
        static_cast<std::uint32_t>(sp+172),static_cast<std::uint32_t>(sp+992),
        static_cast<std::uint32_t>(sp+192),CapacityWords})
        return rejected(Name,"Scaler parameter/record/output pointers or capacity differ from the bounded native profile");
    Candidate::Inputs inputs{{call.argument32(0),call.argument32(1),call.argument32(2),
        call.argument32(3),call.argument32(4),call.argument32(5),call.argument32(7)}, {}};
    for (unsigned i=0;i<6;++i) (void)readU32(call.stackPointer+(i<3?156+i*4:172+(i-3)*4),inputs.filterParameterBits[i]);
    const auto candidate=Candidate::inspect(inputs,addressSpace_->translate(tail[1]),Candidate::RecordBytes);
    const auto plan=candidate?Plan::make(*candidate):std::nullopt;
    if (!plan) return rejected(Name,"Scaler geometry, filters, coefficients or controls are outside the fixed native identity policy");
    // Require the observed debug prefill across the complete caller capacity.
    // Only actual generated bytes are overwritten, and return value is WORDS.
    for (unsigned i=0;i<CapacityWords;++i) {
        std::uint32_t value=0;(void)readU32(tail[2]+i*4,value);
        if (value!=0x80000000) return rejected(Name,"Scaler destination no longer matches its caller prefill");
    }
    const auto command=Command::make({1,ring.readWord,RECOMP_CURRENT_GUEST_TOKEN},*plan);
    if (!command) return rejected(Name,"Native scaler command identity is invalid");
    auto result=handled(Name,"Generated64DWORD native identity scaler request; ordered execution is still pending");
    static_assert(std::is_nothrow_copy_assignable_v<decltype(pendingNativeScaler_)>);
    static_assert(std::is_nothrow_copy_assignable_v<decltype(nativeScalerRing_)>);
    pendingNativeScaler_=command;
    nativeScalerRing_=ring;
    std::memcpy(addressSpace_->translate(tail[2]),command->bytes().data(),Command::Bytes);
    call.returnValue=Command::Words;
    return result; // No backend callback, event store, fence, RPTR or presentation.
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlInitAnsiString(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlInitAnsiString"};
    const auto destination = call.argument32(0);
    const auto source = call.argument32(1);
    if (destination == 0 || !contains(destination, 8)) {
        return memoryFault(Name, "ANSI_STRING destination is outside guest memory");
    }
    if (source == 0) {
        (void)writeU16(destination, 0);
        (void)writeU16(destination + 2, 0);
        (void)writeU32(destination + 4, 0);
        return handled(Name, "Initialized an empty ANSI_STRING");
    }
    std::string value;
    if (!readAnsiString(source, MaximumAnsiStringBytes + 1, value)
            || value.size() > MaximumAnsiStringBytes) {
        return memoryFault(Name, "ANSI source is out of range or exceeds USHORT capacity");
    }
    const auto length = static_cast<std::uint16_t>(value.size());
    (void)writeU16(destination, length);
    (void)writeU16(destination + 2, static_cast<std::uint16_t>(length + 1U));
    (void)writeU32(destination + 4, source);
    return handled(Name, "Initialized a bounded big-endian ANSI_STRING descriptor");
}

GuestImportDispatchResult GuestImportRegistry::dispatchRtlInitUnicodeString(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__RtlInitUnicodeString"};
    const auto destination = call.argument32(0);
    const auto source = call.argument32(1);
    if (destination == 0 || !contains(destination, 8)) {
        return memoryFault(Name, "UNICODE_STRING destination is outside guest memory");
    }
    if (source == 0) {
        (void)writeU16(destination, 0);
        (void)writeU16(destination + 2, 0);
        (void)writeU32(destination + 4, 0);
        return handled(Name, "Initialized an empty UNICODE_STRING");
    }
    std::size_t codeUnits = 0;
    for (; codeUnits <= MaximumUnicodeCodeUnits; ++codeUnits) {
        const auto offset = static_cast<std::uint64_t>(codeUnits) * 2U;
        const auto address = static_cast<std::uint64_t>(source) + offset;
        if (address > std::numeric_limits<std::uint32_t>::max()) {
            return memoryFault(Name, "Unicode source crosses the 32-bit guest boundary");
        }
        std::uint16_t value = 0;
        if (!readU16(static_cast<std::uint32_t>(address), value)) {
            return memoryFault(Name, "Unicode source is out of range or unterminated");
        }
        if (value == 0) {
            break;
        }
    }
    if (codeUnits > MaximumUnicodeCodeUnits) {
        return memoryFault(Name, "Unicode source exceeds USHORT byte-length capacity");
    }
    const auto length = static_cast<std::uint16_t>(codeUnits * 2U);
    (void)writeU16(destination, length);
    (void)writeU16(destination + 2, static_cast<std::uint16_t>(length + 2U));
    (void)writeU32(destination + 4, source);
    return handled(Name, "Initialized a bounded big-endian UNICODE_STRING descriptor");
}

void GuestImportRegistry::clearPhysicalMemoryArena(bool scrub) noexcept {
    xmaBackend_.reset();
    for (auto it = dispatcherEventWaits_.begin(); it != dispatcherEventWaits_.end();) {
        if (it->first >= physicalArenaBase_ &&
            it->first < std::uint64_t{physicalArenaBase_} + physicalArenaSize_) {
            it = dispatcherEventWaits_.erase(it);
            wakeAllIfRegistryWaits(); // PARALLEL: a removed wait must observe its failure.
        }
        else ++it;
    }
    graphicsMeInitVerified_ = false;
    initialCoherencySubmissionBound_ = false; completedInitialCoherency_.reset();
    experimentalInitializationBound_ = false; completedExperimentalInitialization_.reset();
    emptyCacheCoherencyBound_ = false; completedEmptyCacheCoherency_.reset();
    completedWarmEmptyCacheCoherency_.reset();
    nativeStateBatchesBound_ = false; completedNativeStateBatches_.reset();
    completedSideEffectFreeBatches_.reset();
    experimentalOwnedColorBound_=false;completedOwnedColor_.reset();completedOwnedColorImage_.reset();
    nativeSwapConsumer_={};pendingNativeSwap_.reset();nativeSwapStatus_={};nativeDisplayModeStatus_={};
    completedCommandStorageCoherency_.reset();
    completedWrappedSwap_.reset();
    nativeScalerTimingBinding_.reset();nativeScalerTimingQueries_=0;
    nativeIdentityScalerBound_=false;pendingNativeScaler_.reset();nativeCommandInterruptWorker_.reset();postOutputSwapInterruptWorker_.reset();
    nativeInterruptScope_.reset();nativeInterruptScopeStatus_={};postOutputSwapInterruptScopeStatus_={};
    postOutputWrappedSwapAttempted_=false;completedPostOutputWrappedSwap_.reset();
    postOutputSwapInterruptSnapshot_.reset();postOutputSwapInterruptSequence_.reset();
    postOutputSwapPacingRecord_.reset();
    deferredSwapInterruptWorker_.reset();deferredSwapInterruptScopeStatus_={};
    deferredSwapInterruptSnapshot_.reset();deferredSwapInterruptSequence_.reset();deferredSwapPacingRecord_.reset();
    deferredWrappedSwapAttempted_=false;completedDeferredWrappedSwap_.reset();
    nativeScalerInterruptStatus_={};nativeScalerInterruptPlan_.reset();nativeScalerInterruptScratchOwner_.reset();nativeScalerInterruptScratchBefore_={};
    nativeScalerTailAttempted_=false;completedNativeScaler_.reset();
    nativePostScalerStorageFailed_=false;completedPostScalerStorage_.reset();
    nativePostDisplayStorageFailed_=false;completedPostDisplayStorage_.reset();
    nativePostOutputStorageFailed_=false;completedPostOutputStorage_.reset();
    nativeRuntimeCommandStorageFailed_=false;completedRuntimeCommandStorage_.reset();
    nativeRuntimeMaintenanceFailed_=false;completedRuntimeMaintenance_.reset();
    postOutputColorAttempted_=false;completedPostOutputColor_.reset();pendingPostOutputColorImage_.reset();
    postOutputColorRejection_=0;postOutputColorDetail_=0;
    postOutputSystemContext_.reset();postSwapSystemContext_.reset();
    pendingPostOutputSwap_.reset();postOutputSwapStatus_={};pendingDeferredSwap_.reset();
    completedDeferredSwapStorage_.reset();deferredSwapStorageAttempted_=false;
    deferredColorPrefixAttempted_=false;deferredColorPrefixRejection_=deferredColorPrefixDetail_=0;
    completedDeferredColorPrefix_.reset();pendingDeferredColorImage_.reset();deferredColorSubmission_.reset();
    deferredColorPinnedAllocations_={};deferredColorPinCount_=0;
    postOutputSwapStorageAttempted_=false;completedPostOutputSwapStorage_.reset();
    nativeDisplayGammaAttempted_=false;completedDisplayGamma_.reset();nativeScanoutRequest_.reset();
    nativeDisplayOutputAttempted_=false;completedDisplayOutput_.reset();
    nativeInterruptDeviceRoot_={};
    nativeScalerStorageOwner_.reset();nativeScalerRing_.reset();completedScalerStorageCoherency_.reset();
    experimentalIdleCachePairBound_ = false; experimentalIdleCachePairs_ = 0;
    completedWarmStateBatches_.reset();
    completedWarmSideEffectFreeBatches_.reset();
    retiredGraphicsRingReadWord_.reset(); graphicsWriteBackRebindAllowed_=false;
    completedWarmRingCoherency_.reset();
    edramCalibrationLock_ = 0; edramPreparedThread_ = 0; edramTrainingState_ = {};
    graphicsSystemIdentifier_.reset(); graphicsIdentifierAllocation_.reset();
    nativeNoSystemOverlayBound_=false;nativeSystemContext_.reset();nativeSystemContextThread_=0;
    // Host-only arena teardown invalidates configuration before releasing its
    // backing. All guest workers must already be joined, as for registry reset.
    graphicsRingConfiguration_.reset(); graphicsRingAllocation_.reset(); graphicsWriteBackAllocation_.reset();
    if (scrub && addressSpace_ != nullptr && addressSpace_->isReserved()) {
        for (const auto& [address, allocation] : physicalAllocations_) {
            if (contains(address, allocation.size)) {
                std::memset(addressSpace_->translate(address), 0, allocation.size);
            }
        }
    }
    RECOMP_V(physicalAllocations_).clear();
    physicalFreeBlocks_.clear();
    RECOMP_V(physicalMemoryArenaBound_) = false;
    RECOMP_V(physicalArenaBase_) = 0;
    RECOMP_V(physicalArenaSize_) = 0;
    RECOMP_V(physicalAddressOffset_) = 0;
}

bool GuestImportRegistry::findPhysicalBlock(
        std::uint32_t size,
        std::uint32_t alignment,
        std::uint32_t minimumPhysicalAddress,
        std::uint32_t maximumPhysicalAddress,
        std::uint32_t& address) {
    if (!physicalMemoryArenaBound_
            || size == 0
            || alignment == 0
            || minimumPhysicalAddress > maximumPhysicalAddress
            || (physicalArenaBase_ % alignment) != (physicalAddressOffset_ % alignment)) {
        return false;
    }

    const auto arenaPhysicalBegin = static_cast<std::uint64_t>(physicalAddressOffset_);
    const auto arenaPhysicalEnd = arenaPhysicalBegin + physicalArenaSize_;
    const auto allowedPhysicalBegin = std::max(
            arenaPhysicalBegin,
            static_cast<std::uint64_t>(minimumPhysicalAddress));
    const auto allowedPhysicalEnd = std::min(
            arenaPhysicalEnd,
            static_cast<std::uint64_t>(maximumPhysicalAddress) + 1U);
    if (allowedPhysicalEnd <= allowedPhysicalBegin
            || size > allowedPhysicalEnd - allowedPhysicalBegin) {
        return false;
    }

    const auto allowedGuestBegin = static_cast<std::uint64_t>(physicalArenaBase_)
            + (allowedPhysicalBegin - arenaPhysicalBegin);
    const auto allowedGuestEnd = static_cast<std::uint64_t>(physicalArenaBase_)
            + (allowedPhysicalEnd - arenaPhysicalBegin);
    for (auto reverse = physicalFreeBlocks_.rbegin();
            reverse != physicalFreeBlocks_.rend();
            ++reverse) {
        const auto blockBegin = static_cast<std::uint64_t>(reverse->first);
        const auto blockEnd = blockBegin + reverse->second;
        const auto candidateRangeBegin = std::max(blockBegin, allowedGuestBegin);
        const auto candidateRangeEnd = std::min(blockEnd, allowedGuestEnd);
        if (candidateRangeEnd <= candidateRangeBegin
                || size > candidateRangeEnd - candidateRangeBegin) {
            continue;
        }
        const auto latestStart = candidateRangeEnd - size;
        const auto alignedStart = latestStart - (latestStart % alignment);
        if (alignedStart < candidateRangeBegin) {
            continue;
        }

        const auto originalAddress = reverse->first;
        const auto originalEnd = blockEnd;
        physicalFreeBlocks_.erase(originalAddress);
        if (alignedStart != blockBegin) {
            physicalFreeBlocks_.emplace(
                    originalAddress,
                    static_cast<std::uint32_t>(alignedStart - blockBegin));
        }
        const auto allocationEnd = alignedStart + size;
        if (allocationEnd != originalEnd) {
            physicalFreeBlocks_.emplace(
                    static_cast<std::uint32_t>(allocationEnd),
                    static_cast<std::uint32_t>(originalEnd - allocationEnd));
        }
        address = static_cast<std::uint32_t>(alignedStart);
        return true;
    }
    return false;
}

void GuestImportRegistry::releasePhysicalBlock(
        std::uint32_t address,
        std::uint32_t size) {
    auto [current, inserted] = physicalFreeBlocks_.emplace(address, size);
    if (!inserted) {
        return;
    }
    if (current != physicalFreeBlocks_.begin()) {
        auto previous = std::prev(current);
        if (static_cast<std::uint64_t>(previous->first) + previous->second == current->first) {
            previous->second += current->second;
            physicalFreeBlocks_.erase(current);
            current = previous;
        }
    }
    auto next = std::next(current);
    if (next != physicalFreeBlocks_.end()
            && static_cast<std::uint64_t>(current->first) + current->second == next->first) {
        current->second += next->second;
        physicalFreeBlocks_.erase(next);
    }
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmAllocatePhysicalMemoryEx(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmAllocatePhysicalMemoryEx"};
    call.returnValue = 0;
    if (!physicalMemoryArenaBound_) {
        return rejected(Name, "No bounded guest physical-memory arena is bound");
    }

    const auto type = call.argument32(0);
    const auto requestedSize = call.argument32(1);
    const auto protection = call.argument32(2);
    const auto minimumPhysicalAddress = call.argument32(3);
    const auto maximumPhysicalAddress = call.argument32(4);
    const auto requestedAlignment = call.argument32(5);
    if (type > MaximumPhysicalMemoryType) {
        return rejected(Name, "Physical-memory allocation type is unsupported");
    }
    const auto baseProtection = protection & (PageReadOnly | PageReadWrite);
    if ((baseProtection != PageReadOnly && baseProtection != PageReadWrite)
            || (protection & ~SupportedPhysicalProtection) != 0
            || ((protection & MemLargePages) != 0
                && (protection & Mem16MbPages) != 0)) {
        return rejected(Name, "Physical-memory protection or page flags are unsupported");
    }
    if (requestedSize == 0 || minimumPhysicalAddress > maximumPhysicalAddress) {
        return handled(Name, "Physical-memory request has no allocatable range");
    }

    std::uint32_t pageSize = PageSize;
    if ((protection & MemLargePages) != 0) {
        pageSize = LargePageSize;
    } else if ((protection & Mem16MbPages) != 0) {
        pageSize = SixteenMbPageSize;
    }
    const auto roundedSize64 = (static_cast<std::uint64_t>(requestedSize)
            + pageSize - 1U) / pageSize * pageSize;
    const auto alignmentInput = requestedAlignment == 0 ? pageSize : requestedAlignment;
    const auto roundedAlignment64 = (static_cast<std::uint64_t>(alignmentInput)
            + pageSize - 1U) / pageSize * pageSize;
    if (roundedSize64 == 0
            || roundedSize64 > std::numeric_limits<std::uint32_t>::max()
            || roundedAlignment64 == 0
            || roundedAlignment64 > std::numeric_limits<std::uint32_t>::max()) {
        return handled(Name, "Physical-memory size or alignment cannot be rounded safely");
    }
    const auto roundedSize = static_cast<std::uint32_t>(roundedSize64);
    const auto roundedAlignment = static_cast<std::uint32_t>(roundedAlignment64);
    std::uint32_t address = 0;
    if (!findPhysicalBlock(
                roundedSize,
                roundedAlignment,
                minimumPhysicalAddress,
                maximumPhysicalAddress,
                address)) {
        return handled(Name, "Bounded guest physical-memory arena has no matching block");
    }

    const auto [unused, inserted] = RECOMP_V(physicalAllocations_).emplace(
            address,
            PhysicalAllocation{roundedSize, pageSize, type, protection});
    (void)unused;
    if (!inserted) {
        releasePhysicalBlock(address, roundedSize);
        return rejected(Name, "Physical-memory ownership table rejected a duplicate address");
    }
    call.returnValue = address;
    return handled(Name, "Allocated a deterministic top-down guest physical-memory block");
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmAllocatePhysicalMemory(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmAllocatePhysicalMemory"};
    GuestImportCall expanded = call;
    expanded.arguments[3] = 0;
    expanded.arguments[4] = std::numeric_limits<std::uint32_t>::max();
    expanded.arguments[5] = 0;
    auto result = dispatchMmAllocatePhysicalMemoryEx(expanded);
    call.returnValue = expanded.returnValue;
    result.importName = std::string(Name);
    return result;
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmFreePhysicalMemory(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmFreePhysicalMemory"};
    if (!physicalMemoryArenaBound_) {
        return rejected(Name, "No bounded guest physical-memory arena is bound");
    }
    const auto type = call.argument32(0);
    const auto address = call.argument32(1);
    if (type > MaximumPhysicalMemoryType) {
        return rejected(Name, "Physical-memory free type is unsupported");
    }
    const auto allocation = physicalAllocations_.find(address);
    if (allocation == physicalAllocations_.end()) {
        return rejected(Name, "Physical-memory address is not an owned allocation base");
    }
    if (allocation->second.type != type) {
        return rejected(Name, "Physical-memory free type does not match its allocation");
    }
    if (xmaBackend_ && xmaBackend_->virtualBase()==address)
        return rejected(Name,"Physical allocation is pinned by the native XMA context pool");
    if (graphicsRingAllocation_ == address || graphicsWriteBackAllocation_ == address || graphicsIdentifierAllocation_ == address) {
        return rejected(Name, "Physical allocation is pinned by graphics ring configuration until registry reset");
    }
    if(pendingNativeSwap_ && std::find(pendingNativeSwap_->pinnedAllocations.begin(),pendingNativeSwap_->pinnedAllocations.end(),address)!=
        pendingNativeSwap_->pinnedAllocations.end())
        return rejected(Name,"Physical allocation is pinned by an outstanding native swap command");
    if(pendingPostOutputSwap_ && std::find(pendingPostOutputSwap_->pinnedAllocations.begin(),pendingPostOutputSwap_->pinnedAllocations.end(),address)!=
        pendingPostOutputSwap_->pinnedAllocations.end())
        return rejected(Name,"Physical allocation is pinned by an outstanding post-output swap command");
    if(pendingDeferredSwap_ && std::find(pendingDeferredSwap_->pinnedAllocations.begin(),pendingDeferredSwap_->pinnedAllocations.end(),address)!=
        pendingDeferredSwap_->pinnedAllocations.end())
        return rejected(Name,"Physical allocation is pinned by an outstanding deferred swap request");
    if(std::find(deferredColorPinnedAllocations_.begin(),deferredColorPinnedAllocations_.begin()+deferredColorPinCount_,address)!=
        deferredColorPinnedAllocations_.begin()+deferredColorPinCount_)
        return rejected(Name,"Physical allocation is pinned by the unretired deferred color prefix");
    if (nativeScalerStorageOwner_ && nativeScalerStorageOwner_->virtualBase==address)
        return rejected(Name,"Physical allocation is pinned for the native scaler command stream");
    if (hasDispatcherWaitInRange(address, allocation->second.size) || hasActiveCriticalSectionInRange(address,allocation->second.size))
        return rejected(Name, "Physical allocation is pinned by an active synchronization object");
    const auto allocationSize = allocation->second.size;
    if(completedOwnedColorImage_ && completedOwnedColorImage_->allocationBase==address)
        completedOwnedColorImage_.reset();
    if(pendingPostOutputColorImage_ && pendingPostOutputColorImage_->allocationBase==address)
        pendingPostOutputColorImage_.reset(); // History is not a lease on a reused address.
    std::memset(addressSpace_->translate(address), 0, allocationSize);
    RECOMP_V(physicalAllocations_).erase(allocation);
    releasePhysicalBlock(address, allocationSize);
    return handled(Name, "Released and scrubbed an owned guest physical-memory block");
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmGetPhysicalAddress(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmGetPhysicalAddress"};
    call.returnValue = 0;
    if (!physicalMemoryArenaBound_) {
        return rejected(Name, "No bounded guest physical-memory arena is bound");
    }
    const auto address = call.argument32(0);
    auto allocation = physicalAllocations_.upper_bound(address);
    if (allocation == physicalAllocations_.begin()) {
        return rejected(Name, "Guest address is not inside an owned physical allocation");
    }
    --allocation;
    const auto allocationEnd = static_cast<std::uint64_t>(allocation->first)
            + allocation->second.size;
    if (address < allocation->first || address >= allocationEnd) {
        return rejected(Name, "Guest address is not inside an owned physical allocation");
    }
    call.returnValue = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(physicalAddressOffset_)
            + (address - physicalArenaBase_));
    return handled(Name, "Translated an owned guest pointer to its exact physical address");
}

std::optional<std::array<std::uint32_t, 26>> GuestImportRegistry::lastMemoryStatistics() const {
    RECOMP_REGISTRY_LOCK(lock);
    return lastMemoryStatistics_;
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmQueryStatistics(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmQueryStatistics"};
    constexpr std::uint32_t Bytes = 104;
    const auto output = call.argument32(0);
    if (!output) { call.returnValue = StatusInvalidParameter; return handled(Name, "Null memory statistics output"); }
    if ((output & 3) || !contains(output, Bytes) ||
            (output < 0x7FC90000 && std::uint64_t{output}+Bytes > 0x7FC80000) ||
            overlapsProtectedThreadStorage(output, Bytes) || overlapsThreadTlsStorage(output, Bytes))
        return memoryFault(Name, "Memory statistics output is not a bounded writable guest structure");
    if (!physicalMemoryArenaBound_) return rejected(Name, "Memory statistics require both explicitly managed native arenas");
    const auto end = std::uint64_t{output} + Bytes;
    if (imageProtections_.overlaps(output, Bytes) && !imageProtections_.writable(output, Bytes))
        return memoryFault(Name, "Memory statistics output crosses a non-writable image page");
    if (output < std::uint64_t{physicalArenaBase_} + physicalArenaSize_ && end > physicalArenaBase_ &&
            (output < physicalArenaBase_ ||
             !resolvePhysicalRange(physicalAddressOffset_ + (output - physicalArenaBase_), Bytes, true)))
        return memoryFault(Name, "Memory statistics output is not writable owned physical storage");
    if (output < std::uint64_t{heapBase_} + heapSize_ && end > heapBase_) {
        const auto after = allocations_.upper_bound(output);
        if (output < heapBase_ || after == allocations_.begin() ||
                end > std::uint64_t{std::prev(after)->first} + std::prev(after)->second)
            return memoryFault(Name, "Memory statistics output crosses an unowned virtual range");
        const auto firstPage = (output - heapBase_) / PageSize;
        const auto lastPage = (end - 1 - heapBase_) / PageSize;
        for (auto page = firstPage; page <= lastPage; ++page)
            if (!(virtualCommitBits_[page / 64] & (std::uint64_t{1} << (page % 64))))
                return memoryFault(Name, "Memory statistics output crosses an uncommitted virtual page");
    }
    std::uint32_t size = 0;
    if (!readU32(output, size)) return memoryFault(Name, "Memory statistics size is unreadable");
    if (size != Bytes) {
        call.returnValue = 0xC0000023U;
        return handled(Name, "Memory statistics structure size must be 104");
    }
    std::uint64_t physicalFree = 0, physicalUsed = 0, virtualReserved = 0;
    for (const auto& item : physicalFreeBlocks_) physicalFree += item.second;
    for (const auto& item : physicalAllocations_) physicalUsed += item.second.size;
    for (const auto& item : allocations_) virtualReserved += item.second;
    if (physicalFree+physicalUsed != physicalArenaSize_ || virtualReserved > heapSize_ ||
            std::uint64_t{virtualCommittedPages_}*PageSize > virtualReserved)
        return rejected(Name, "Native memory accounting invariants are inconsistent");
    // Native two-arena contract, NOT Xbox system RAM or host RSS. Physical
    // allocation capacity and portable virtual reservations are independent.
    // Native host/kernel, image, worker-stack, page-table and file-cache storage
    // is outside these arenas and is not charged as fabricated guest pages.
    std::array<std::uint32_t,26> words{};
    words[0]=Bytes;
    words[1]=physicalArenaSize_/PageSize;
    words[3]=static_cast<std::uint32_t>(physicalFree/PageSize);
    words[4]=heapSize_;
    words[5]=static_cast<std::uint32_t>(virtualReserved);
    words[6]=static_cast<std::uint32_t>(physicalUsed/PageSize);
    words[11]=virtualCommittedPages_;
    words[25]=static_cast<std::uint32_t>((std::uint64_t{physicalAddressOffset_}+physicalArenaSize_-1)/PageSize);
    std::array<std::uint8_t,Bytes> bytes{};
    for (std::size_t i=0;i<words.size();++i)
        for (unsigned b=0;b<4;++b) bytes[i*4+b]=static_cast<std::uint8_t>(words[i]>>(24-b*8));
    auto success=handled(Name,"Reported live managed-native-arena statistics; not console RAM or host residency");
    std::memcpy(addressSpace_->translate(output),bytes.data(),bytes.size());
    lastMemoryStatistics_ = words;
    call.returnValue=StatusSuccess;
    return success;
}

void GuestImportRegistry::markVirtualCommit(std::uint32_t address, std::uint32_t size, bool committed) noexcept {
    // Callers have already validated one owned page-aligned allocation/range.
    const auto first=(address-heapBase_)/PageSize;
    for (std::uint32_t i=first;i<first+size/PageSize;++i) {
        const auto mask=std::uint64_t{1}<<(i%64);
        if (static_cast<bool>(virtualCommitBits_[i/64]&mask)==committed) continue;
        auto& word=RECOMP_V(virtualCommitBits_)[i/64]; // v42: only an actual change opens a window
        if (committed) {
            // Recommit must preserve existing data. Zero only a new commit.
            std::memset(addressSpace_->translate(heapBase_ + i * PageSize), 0, PageSize);
            word|=mask; ++virtualCommittedPages_;
        }
        else { word&=~mask; --virtualCommittedPages_; }
    }
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmQueryAllocationSize(GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmQueryAllocationSize"};
    const auto address = call.argument32(0);
    const auto inside = [address](std::uint32_t base, std::uint32_t size) {
        return address >= base && std::uint64_t{address} < std::uint64_t{base} + size;
    };
    // Query allocation/reservation extent, not the remaining bytes from an
    // interior pointer or a host mapping's size. No guest payload is accessed.
    std::uint32_t size = 0;
    if (physicalMemoryArenaBound_ && inside(physicalArenaBase_, physicalArenaSize_)) {
        const auto after = physicalAllocations_.upper_bound(address);
        if (after != physicalAllocations_.begin()) {
            const auto& allocation = *std::prev(after);
            if (inside(allocation.first, allocation.second.size)) size = allocation.second.size;
        }
    } else if (inside(heapBase_, heapSize_)) {
        const auto after = allocations_.upper_bound(address);
        if (after != allocations_.begin()) {
            const auto& allocation = *std::prev(after);
            if (inside(allocation.first, allocation.second)) size = allocation.second;
        }
    } else {
        return rejected(Name, "Allocation metadata is unavailable for this guest region");
    }
    call.returnValue = size;
    return handled(Name, "Queried the recorded allocation extent in an owned arena");
}

GuestImportDispatchResult GuestImportRegistry::dispatchMmQueryAddressProtect(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__MmQueryAddressProtect"};
    const auto address = call.argument32(0);
    if (const auto protection = imageProtections_.query(address)) {
        call.returnValue = *protection;
        return handled(Name, "Queried snapshotted XEX image page metadata; no host mprotect or cache override");
    }
    const auto inside = [address](std::uint32_t base, std::uint32_t size) {
        return address >= base && std::uint64_t{address} < std::uint64_t{base} + size;
    };
    if (physicalMemoryArenaBound_ && inside(physicalArenaBase_, physicalArenaSize_)) {
        auto after = physicalAllocations_.upper_bound(address);
        std::uint32_t protection = 0;
        if (after != physicalAllocations_.begin()) {
            const auto& allocation = *std::prev(after);
            if (inside(allocation.first, allocation.second.size)) {
                // Allocation-size modifiers are not page access/cache attributes.
                protection = allocation.second.protection
                        & (PageReadOnly | PageReadWrite | PageNoCache | PageWriteCombine);
            }
        }
        call.returnValue = protection;
        return handled(Name, "Queried tracked protection in the owned physical arena");
    }
    if (inside(heapBase_, heapSize_)) {
        const auto after = allocations_.upper_bound(address);
        bool allocated = false;
        if (after != allocations_.begin()) {
            const auto& allocation = *std::prev(after);
            allocated = inside(allocation.first, allocation.second);
        }
        // The portable heap accepts only PAGE_READWRITE. This queries the
        // recorded protection, not commit state or host mprotect permissions.
        // Reserve-only allocations retain their requested protection too.
        call.returnValue = allocated ? PageReadWrite : 0;
        return handled(Name, "Queried recorded protection in the owned virtual arena");
    }
    return rejected(Name, "Protection metadata is unavailable for this guest region");
}

bool GuestImportRegistry::findFreeBlock(
        std::uint32_t size,
        std::uint32_t alignment,
        std::uint32_t& address) {
    for (auto iterator = freeBlocks_.begin(); iterator != freeBlocks_.end(); ++iterator) {
        const auto blockStart = static_cast<std::uint64_t>(iterator->first);
        const auto blockEnd = blockStart + iterator->second;
        const auto alignedStart = (blockStart + alignment - 1U) & ~(alignment - 1U);
        const auto allocationEnd = alignedStart + size;
        if (allocationEnd > blockEnd) {
            continue;
        }
        const auto originalAddress = iterator->first;
        freeBlocks_.erase(iterator);
        if (alignedStart != blockStart) {
            freeBlocks_.emplace(
                    originalAddress,
                    static_cast<std::uint32_t>(alignedStart - blockStart));
        }
        if (allocationEnd != blockEnd) {
            freeBlocks_.emplace(
                    static_cast<std::uint32_t>(allocationEnd),
                    static_cast<std::uint32_t>(blockEnd - allocationEnd));
        }
        address = static_cast<std::uint32_t>(alignedStart);
        RECOMP_V(allocations_).emplace(address, size);
        return true;
    }
    return false;
}

void GuestImportRegistry::releaseBlock(std::uint32_t address, std::uint32_t size) {
    auto [current, inserted] = freeBlocks_.emplace(address, size);
    if (!inserted) {
        return;
    }
    if (current != freeBlocks_.begin()) {
        auto previous = std::prev(current);
        if (static_cast<std::uint64_t>(previous->first) + previous->second == current->first) {
            previous->second += current->second;
            freeBlocks_.erase(current);
            current = previous;
        }
    }
    auto next = std::next(current);
    if (next != freeBlocks_.end()
            && static_cast<std::uint64_t>(current->first) + current->second == next->first) {
        current->second += next->second;
        freeBlocks_.erase(next);
    }
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtAllocateVirtualMemory(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtAllocateVirtualMemory"};
    const auto baseAddressPointer = call.argument32(0);
    const auto regionSizePointer = call.argument32(1);
    const auto allocationType = call.argument32(2);
    const auto protection = call.argument32(3);
    const auto debugMemory = call.argument32(4);
    std::uint32_t requestedAddress = 0;
    std::uint32_t requestedSize = 0;
    if (baseAddressPointer == 0
            || regionSizePointer == 0
            || !readU32(baseAddressPointer, requestedAddress)
            || !readU32(regionSizePointer, requestedSize)) {
        call.returnValue = StatusInvalidParameter;
        return memoryFault(Name, "Virtual-memory parameter pointers are outside guest memory");
    }
    if (requestedSize == 0) {
        call.returnValue = StatusInvalidParameter;
        return handled(Name, "Rejected a zero-size virtual-memory allocation");
    }
    constexpr std::uint32_t TypeMask = MemCommit | MemReserve;
    constexpr std::uint32_t ModifierMask = MemLargePages | MemHeap;
    const auto type = allocationType & TypeMask;
    const auto unsupportedFlags = allocationType & ~(TypeMask | ModifierMask);
    if (debugMemory != 0
            || protection != PageReadWrite
            || unsupportedFlags != 0
            || type == 0) {
        call.returnValue = StatusNotSupported;
        return handled(
                Name,
                "Virtual-memory request uses an unsupported devkit-memory mode, flag, or protection");
    }
    const auto pageSize = (allocationType & MemLargePages) != 0
            ? LargePageSize
            : PageSize;
    if (requestedSize > std::numeric_limits<std::uint32_t>::max() - (pageSize - 1U)) {
        call.returnValue = StatusNoMemory;
        return handled(Name, "Virtual-memory size cannot be page aligned safely");
    }
    const auto roundedSize = (requestedSize + pageSize - 1U) & ~(pageSize - 1U);

    if (type == MemCommit && requestedAddress != 0) {
        if ((requestedAddress % pageSize) != 0) {
            call.returnValue = StatusInvalidParameter;
            return handled(Name, "Fixed commit address is not aligned to its requested page size");
        }
        const auto after = allocations_.upper_bound(requestedAddress);
        if (after == allocations_.begin()) {
            call.returnValue = StatusInvalidParameter;
            return handled(Name, "Fixed commit does not belong to a portable reservation");
        }
        const auto reservation = std::prev(after);
        const auto reservationEnd = static_cast<std::uint64_t>(reservation->first)
                + reservation->second;
        const auto commitEnd = static_cast<std::uint64_t>(requestedAddress) + roundedSize;
        if (requestedAddress < reservation->first || commitEnd > reservationEnd) {
            call.returnValue = StatusInvalidParameter;
            return handled(Name, "Fixed commit exceeds its portable reservation");
        }
        markVirtualCommit(requestedAddress,roundedSize,true);
        (void)writeU32(baseAddressPointer, requestedAddress);
        (void)writeU32(regionSizePointer, roundedSize);
        call.returnValue = StatusSuccess;
        return handled(Name, "Committed new pages inside a portable reservation, preserving existing commits");
    }
    if (requestedAddress != 0 || type == MemCommit) {
        call.returnValue = StatusNotSupported;
        return handled(Name, "Only new reservations or commits inside an existing reservation are supported");
    }

    std::uint32_t allocationAddress = 0;
    if (!findFreeBlock(roundedSize, pageSize, allocationAddress)) {
        call.returnValue = StatusNoMemory;
        return handled(Name, "Portable guest heap has no block large enough for the request");
    }
    if (type == (MemCommit | MemReserve)) {
        markVirtualCommit(allocationAddress, roundedSize, true);
    }
    if (!writeU32(baseAddressPointer, allocationAddress)
            || !writeU32(regionSizePointer, roundedSize)) {
        markVirtualCommit(allocationAddress, roundedSize, false);
        RECOMP_V(allocations_).erase(allocationAddress);
        releaseBlock(allocationAddress, roundedSize);
        call.returnValue = StatusInvalidParameter;
        return memoryFault(Name, "Could not write virtual-memory results to guest memory");
    }
    call.returnValue = StatusSuccess;
    return handled(
            Name,
            type == MemReserve
                    ? "Reserved pages from the caller-selected portable guest heap"
                    : "Reserved and committed zeroed pages from the caller-selected portable guest heap");
}

GuestImportDispatchResult GuestImportRegistry::dispatchNtFreeVirtualMemory(
        GuestImportCall& call) {
    constexpr std::string_view Name{"__imp__NtFreeVirtualMemory"};
    const auto baseAddressPointer = call.argument32(0);
    const auto regionSizePointer = call.argument32(1);
    const auto freeType = call.argument32(2);
    const auto unknown = call.argument32(3);
    std::uint32_t allocationAddress = 0;
    std::uint32_t regionSize = 0;
    if (baseAddressPointer == 0
            || regionSizePointer == 0
            || !readU32(baseAddressPointer, allocationAddress)
            || !readU32(regionSizePointer, regionSize)) {
        call.returnValue = StatusInvalidParameter;
        return memoryFault(Name, "Virtual-memory free parameter pointers are outside guest memory");
    }
    if (freeType != MemRelease || unknown != 0 || regionSize != 0) {
        call.returnValue = StatusNotSupported;
        return handled(Name, "Only MEM_RELEASE with zero region size is supported");
    }
    const auto allocation = allocations_.find(allocationAddress);
    if (allocation == allocations_.end()) {
        call.returnValue = StatusInvalidParameter;
        return handled(Name, "Virtual-memory address is not owned by the portable guest heap");
    }
    const auto allocationSize = allocation->second;
    if (nativeEventPool_ && allocationAddress == nativeEventPool_)
        return rejected(Name, "Native event reference storage is runtime-owned");
    if (hasDispatcherWaitInRange(allocationAddress, allocationSize) || hasActiveCriticalSectionInRange(allocationAddress,allocationSize))
        return rejected(Name, "Virtual allocation is pinned by an active synchronization object");
    std::memset(addressSpace_->translate(allocationAddress), 0, allocationSize);
    markVirtualCommit(allocationAddress,allocationSize,false);
    RECOMP_V(allocations_).erase(allocation);
    releaseBlock(allocationAddress, allocationSize);
    // Match the Xbox kernel contract: on release, the base remains the released
    // base and RegionSize reports the complete released allocation.
    (void)writeU32(baseAddressPointer, allocationAddress);
    (void)writeU32(regionSizePointer, allocationSize);
    call.returnValue = StatusSuccess;
    return handled(Name, "Released and scrubbed pages owned by the portable guest heap");
}

} // namespace recomp::bootstrap
