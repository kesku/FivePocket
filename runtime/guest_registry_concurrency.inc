#include <array> // v239
#include <pthread.h> // v292
#include "sorted_lookup.h" // v228
#include "span_move.h" // span-move (diag-sm-worker): registryGraphicsSnapshotFor / registrySnapshotGraphicsView (end of this file)
// Attempt v42: implementation of guest_registry_concurrency.h. Included exactly once, by
// guest_import_registry.cpp, right after the header (so after every standard include).
#if !defined(RECOMP_REGISTRY_READ_SNAPSHOT)
#define RECOMP_REGISTRY_READ_SNAPSHOT 1 // 0: renderer reads always take R (v35 behaviour, same counters)
#endif
// v110: runtime switches (files/private-trace/diag-...); host tests point this at a scratch directory.
#if !defined(RECOMP_DIAG_SWITCH_DIR)
#define RECOMP_DIAG_SWITCH(name) RECOMP_APP_PATH("/private-trace/" name) // data-paths (host tests: recomp_app_dir.h)
#endif
#include <unistd.h>

namespace recomp::bootstrap {

namespace {

struct RegistryCounters {
    // Snapshot fast path.
    std::atomic<std::uint64_t> hits{0}, adopted{0}, rebuilds{0}, rebuildTicks{0}, maxRebuildTicks{0}, rateLimited{0};
    std::atomic<std::uint64_t> windowOpen{0}, noSlot{0}, mismatch{0}, rejected{0}, arena{0}, windows{0}, poisoned{0};
    std::atomic<std::size_t> lastAllocations{0}, lastPhysical{0}, lastProtected{0}, lastCommitWords{0};
    // Legacy RENDER_DIAG_REGISTRY_READ_LOCK counters: locked renderer reads (fallbacks and refreshes).
    std::atomic<std::uint64_t> readFast{0}, readSpun{0}, readBlocked{0}, readBlockedNs{0}, readMaxBlockedNs{0};
    std::atomic<std::uint64_t> nextLogTicks{0};
    // v110: contended acquisitions of every section kind (spin phase / futex), and the 10 s lines.
    std::atomic<std::uint64_t> spun{0}, blocked{0}, spinTicks{0}, blockTicks{0}, nextWaitLogTicks{0};
    // v152: mapNativeAudioPhysical's generation-validated per-thread cache (RENDER_DIAG_AUDIO_MAP_CACHE).
    std::atomic<std::uint64_t> audioMapHits{0}, audioMapLocked{0}, audioMapStored{0}, audioMapWindowOpen{0};
    // v152: heap reads refused without R because the current snapshot puts them outside the heap bounds.
    std::atomic<std::uint64_t> heapBoundsRejects{0};
};
RegistryCounters registryCounters;
// v152: files/private-trace/diag-audio-map-locked restores v151 (every mapNativeAudioPhysical takes R).
// v152: files/private-trace/diag-registry-heap-bounds-locked restores v151 (an out-of-heap read still takes R to be refused).
bool heapBoundsRejectEnabled() noexcept {
    static const bool enabled = ::access(RECOMP_DIAG_SWITCH("diag-registry-heap-bounds-locked"), F_OK) != 0;
    return enabled;
}
bool audioMapCacheEnabled() noexcept {
    static const bool enabled = ::access(RECOMP_DIAG_SWITCH("diag-audio-map-locked"), F_OK) != 0;
    return enabled;
}
// v152: one thread's recent mapNativeAudioPhysical answers. An entry is the locked answer for (registry, address space,
// physical, bytes, writable) at `generation`, recorded under R while no mutation window was open, i.e. while the
// validation state V (physicalAllocations_ and the arena fields are in V) equalled generation `generation`
// (guest_registry_concurrency.h, points 2-4). It is used only while the slot word still reads that generation with no
// window open: then no write to V happened since, and the locked predicate would give the same answer at that load.
struct AudioMapEntry {
    const GuestImportRegistry* registry{nullptr};
    const GuestAddressSpace* space{nullptr};
    std::uint64_t generation{~std::uint64_t{0}};
    std::uint32_t physical{0}, bytes{0};
    bool writable{false};
    std::uint8_t* pointer{nullptr};
};
// v153: direct-mapped, 256 entries (v152's 8-entry round robin missed ~90%: GTA keeps dozens of XMA voices, each with
// an output and up to two input buffers, all decoded by one thread).
struct AudioMapCache {
    std::array<AudioMapEntry, 256> entries{};
    static unsigned slot(std::uint32_t physical, std::uint32_t bytes, bool writable) noexcept {
        std::uint32_t h = (physical >> 8) * 0x9E3779B1u ^ bytes * 0x85EBCA77u ^ (writable ? 0x27D4EB2Fu : 0u);
        return (h ^ (h >> 15)) & 255u;
    }
};
AudioMapCache& audioMapCache() noexcept { static thread_local AudioMapCache cache; return cache; }
std::atomic<RegistryLockSite*> registrySiteList{nullptr};
std::mutex registryClaimMutex; // leaf: slot and import-site claims only
std::array<RegistryValidationSlot, 64> registrySlots;
struct RegistryImportSiteEntry {
    std::atomic<std::uint64_t> key{0};
    char name[64]{};
    RegistryLockSite site{name, 0};
};
std::array<RegistryImportSiteEntry, 512> registryImportSites;
RegistryLockSite registryImportOverflow{"dispatch-overflow", 0};
// v110: threads waiting for R right now (any registry): a holder that releases while it is nonzero
// made someone wait (RegistryLockSite::blockingHolds). Written only on contended acquisitions.
std::atomic<std::uint64_t> registryWaiters{0};

void registryMaxStore(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
}

#if defined(__aarch64__)
std::uint64_t registryTickFrequency() noexcept {
    static const std::uint64_t frequency = [] {
        std::uint64_t value;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
        return value ? value : 1;
    }();
    return frequency;
}
#endif
std::uint64_t registryNsToTicks(std::uint64_t ns) noexcept {
#if defined(__aarch64__)
    return static_cast<std::uint64_t>(static_cast<unsigned __int128>(ns) * registryTickFrequency() / 1000000000u);
#else
    return ns;
#endif
}

void registryPoison(const GuestImportRegistry* self, const char* where, bool inSection) noexcept {
    auto* slot = registryFindSlot(self);
    if (!slot) slot = registryClaimSlot(self);
    // A window that never closes: every later fast read of this registry uses R (fail closed).
    if (slot) slot->word.fetch_add(RegistryWindowOne, std::memory_order_acq_rel);
    if (registryCounters.poisoned.fetch_add(1, std::memory_order_relaxed) < 4) {
        __android_log_print(ANDROID_LOG_WARN, "RecompRenderer",
            "RENDER_DIAG_REGISTRY_SNAPSHOT_POISONED where=%s section=%d (a validation-state write outside a registry section holding R)",
            where ? where : "?", inSection);
    }
}

} // namespace

std::uint64_t registryTicksToNs(std::uint64_t ticks) noexcept {
#if defined(__aarch64__)
    return static_cast<std::uint64_t>(static_cast<unsigned __int128>(ticks) * 1000000000u / registryTickFrequency());
#else
    return ticks;
#endif
}

void registryListSite(RegistryLockSite& site) noexcept {
    if (site.listed.exchange(true, std::memory_order_acq_rel)) return;
    auto* head = registrySiteList.load(std::memory_order_acquire);
    do site.next.store(head, std::memory_order_relaxed);
    while (!registrySiteList.compare_exchange_weak(head, &site, std::memory_order_release, std::memory_order_acquire));
}

namespace {
RegistryLockSite& registryImportSiteSlow(std::string_view importName) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : importName) { hash ^= c; hash *= 1099511628211ull; }
    if (!hash) hash = 1;
    for (std::size_t probe = 0, i = hash & 511; probe < registryImportSites.size(); ++probe, i = (i + 1) & 511) {
        auto& entry = registryImportSites[i];
        const auto key = entry.key.load(std::memory_order_acquire);
        if (key == hash) return entry.site;
        if (key) continue;
        std::lock_guard claim(registryClaimMutex);
        const auto again = entry.key.load(std::memory_order_acquire);
        if (again == hash) return entry.site;
        if (again) continue;
        const auto length = std::min<std::size_t>(importName.size(), sizeof entry.name - 1);
        std::memcpy(entry.name, importName.data(), length);
        entry.name[length] = 0;
        entry.key.store(hash, std::memory_order_release);
        return entry.site;
    }
    return registryImportOverflow;
}
} // namespace

RegistryValidationSlot* registryFindSlot(const GuestImportRegistry* registry) noexcept {
    for (auto& slot : registrySlots) { // Filled in order, never released: the first empty slot ends the search.
        const auto* owner = slot.owner.load(std::memory_order_acquire);
        if (owner == registry) return &slot;
        if (!owner) return nullptr;
    }
    return nullptr;
}

} // namespace recomp::bootstrap
// v335: the registry's generation word (generation << 16 | open windows) for the hook layer's device-shadow view cache. Slots are
// never released, so the pointer stays valid; nullptr when the registry has no slot (no fast path).
extern "C" const std::atomic<std::uint64_t>* recomp_registry_generation_word(const void* registry) noexcept {
    auto* slot=recomp::bootstrap::registryFindSlot(static_cast<const recomp::bootstrap::GuestImportRegistry*>(registry));
    return slot?&slot->word:nullptr;
}
namespace recomp::bootstrap {
RegistryValidationSlot* registryClaimSlot(const GuestImportRegistry* registry) noexcept {
    std::lock_guard claim(registryClaimMutex);
    for (auto& slot : registrySlots) {
        const auto* owner = slot.owner.load(std::memory_order_relaxed);
        if (owner == registry) return &slot;
        if (!owner) { slot.owner.store(registry, std::memory_order_release); return &slot; }
    }
    return nullptr; // Table full: this registry never gets a fast path (its readers find no slot).
}

bool registryRangesOverlap(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
        std::uint64_t begin, std::uint64_t end) noexcept {
    // Disjoint ascending ranges: the first range ending after `begin` is the only candidate.
    const auto next = std::upper_bound(ranges.begin(), ranges.end(), begin,
        [](std::uint64_t value, const std::pair<std::uint64_t, std::uint64_t>& range) { return value < range.second; });
    return next != ranges.end() && next->first < end;
}

// ---- sections

void RegistrySection::openWindow() noexcept {
    if (window) return;
    auto* slot = registryFindSlot(registry);
    if (!slot) slot = registryClaimSlot(registry);
    if (!slot) return;
    slot->word.fetch_add(RegistryWindowOne, std::memory_order_acq_rel); // sequenced before the write
    window = slot;
    if (!site.listed.load(std::memory_order_relaxed)) registryListSite(site); // visible in the log before its first batch flush
    site.windows.fetch_add(1, std::memory_order_relaxed);
    registryCounters.windows.fetch_add(1, std::memory_order_relaxed);
}

void RegistrySection::closeWindow(bool atEnd) noexcept {
    if (!window || (windowPinned && !atEnd)) return;
    windowPinned = false;
    // generation += 1 and windows -= 1 in one release RMW, after every write of the window.
    window->word.fetch_add(RegistryGenerationOne - RegistryWindowOne, std::memory_order_acq_rel);
    window = nullptr;
}

namespace {
struct RegistrySiteBatch {
    RegistryLockSite* site{nullptr};
    std::uint64_t acquisitions{0}, contended{0}, waitTicks{0}, holdTicks{0}, maxHoldTicks{0}, sections{0};
    std::uint64_t blockingHolds{0}, blockingHoldTicks{0}, maxBlockingHoldTicks{0}; // v110
    void flush() noexcept {
        if (!site) return;
        if (!site->listed.load(std::memory_order_relaxed)) registryListSite(*site);
        site->acquisitions.fetch_add(acquisitions, std::memory_order_relaxed);
        if (contended) {
            site->contended.fetch_add(contended, std::memory_order_relaxed);
            site->waitTicks.fetch_add(waitTicks, std::memory_order_relaxed);
        }
        site->holdTicks.fetch_add(holdTicks, std::memory_order_relaxed);
        registryMaxStore(site->maxHoldTicks, maxHoldTicks);
        if (blockingHolds) {
            site->blockingHolds.fetch_add(blockingHolds, std::memory_order_relaxed);
            site->blockingHoldTicks.fetch_add(blockingHoldTicks, std::memory_order_relaxed);
            registryMaxStore(site->maxBlockingHoldTicks, maxBlockingHoldTicks);
        }
        *this = {};
    }
};
struct RegistrySiteBatches {
    std::array<RegistrySiteBatch, 4> entries{};
    unsigned next{0};
    ~RegistrySiteBatches() { for (auto& e : entries) e.flush(); }
};
// Import names reach dispatch() as string_views of the same literals every time: a direct-mapped
// per-thread cache keyed by (pointer, length) avoids hashing the name on every import.
struct RegistryImportSiteCacheEntry { const char* data{nullptr}; std::size_t size{0}; RegistryLockSite* site{nullptr}; };
struct RegistryReaderCache {
    const GuestImportRegistry* registry{nullptr};
    RegistryValidationSlot* slot{nullptr};
    std::uint64_t generation{~std::uint64_t{0}};
    std::shared_ptr<const GuestRegistryValidationSnapshot> snapshot;
    std::uint64_t readsSinceRebuild{std::uint64_t{1} << 40}; // the first rebuild of a thread is never deferred
    std::uint64_t lastRebuildNs{0};
    std::uint64_t unflushedHits{0};
    // v152: the last fastValidatedView call of this thread found, in the current snapshot, a heap read outside
    // [heapBase, heapBase + heapSize) or of 0 / >64 KiB bytes: the locked heap predicate's first test rejects it too.
    bool outsideHeap{false};
};
} // namespace

namespace {
struct ViewMemoEntry {const void* registry = nullptr; const void* snapshot = nullptr; std::uint64_t generation = 0; std::uint32_t address = 0, bytes = 0;
    bool graphics = false; const std::uint8_t* view = nullptr;}; // v239 (see fastValidatedView)
} // namespace
struct RegistryThreadState {
    RegistrySection* current{nullptr};
    unsigned logCheck{0};
    unsigned role{RegistryRoles}; // v110: cached once the thread runs a guest task (RegistryRoles: unknown)
    std::array<RegistryImportSiteCacheEntry, 64> importSites{};
    RegistrySiteBatches batches;
    RegistryReaderCache reader;
    std::array<ViewMemoEntry, 1024> viewMemo{}; // v292: was its own thread_local (a second resolver call per view); v304b: 64 -> 1024
    std::array<ViewMemoEntry, 8> viewMru{}; unsigned viewMruNext{0}; // v322: the last 8 distinct views (L1-resident, checked first)
};
// v292: the state behind a trivially destructible thread_local pointer. A thread_local object with a destructor is
// reached through its TU's __tls_guard first: two resolver calls per access in this dlopen'ed library (TLSDESC; the
// initial-exec model is refused for dlopen'ed libraries, and lld has no AArch64 local-dynamic relocations). The pointer
// is one call. A pthread key deletes the state at thread exit, as the thread_local's destructor did.
namespace {
thread_local RegistryThreadState* registryThreadStatePointer = nullptr;
pthread_key_t registryThreadStateKey;
pthread_once_t registryThreadStateOnce = PTHREAD_ONCE_INIT;
void destroyRegistryThreadState(void* state) {
    registryThreadStatePointer = nullptr; // (runs on the exiting thread: a later access would build a new one)
    delete static_cast<RegistryThreadState*>(state);
}
[[gnu::noinline]] RegistryThreadState& registryThreadStateSlow() noexcept {
    (void)pthread_once(&registryThreadStateOnce, [] { (void)pthread_key_create(&registryThreadStateKey, destroyRegistryThreadState); });
    auto* state = new RegistryThreadState; // (bad_alloc: terminate, as a failed TLS block allocation aborts)
    (void)pthread_setspecific(registryThreadStateKey, state);
    registryThreadStatePointer = state;
    return *state;
}
}
// v362 (exact, fast_tls_v362.h): the claimed (render) thread reads registryThreadStatePointer through the variable's own address,
// cached by that thread (every validated view starts here: one TLS resolver call each in v349, ~0.3% of the render thread).
RegistryThreadState& registryThreadState() noexcept {
    auto*& pointer = ::recomp::v362::tls<::recomp::v362::Slot::RegistryState,RegistryThreadState*>(
                                     []() noexcept -> RegistryThreadState*& { return registryThreadStatePointer; });
    if (auto* state = pointer) [[likely]] return *state;
    return registryThreadStateSlow();
}

RegistrySection::RegistrySection(RegistryThreadState& state, const GuestImportRegistry* r, RegistryLockSite& s) noexcept
    : thread(state), registry(r), site(s), previous(state.current) { state.current = this; }
RegistrySection::~RegistrySection() { thread.current = previous; }

namespace {
RegistryLockSite& registryImportSite(RegistryThreadState& state, std::string_view importName) noexcept {
    auto& cached = state.importSites[(reinterpret_cast<std::uintptr_t>(importName.data()) >> 3) & 63];
    if (cached.data == importName.data() && cached.size == importName.size() && cached.site) return *cached.site;
    auto& site = registryImportSiteSlow(importName);
    cached = {importName.data(), importName.size(), &site};
    return site;
}
} // namespace

const GuestRegistryValidationSnapshot* registryCurrentSnapshotLocked(const GuestImportRegistry* registry) noexcept {
#if !RECOMP_REGISTRY_READ_SNAPSHOT
    (void)registry;
    return nullptr;
#else
    auto& state = registryThreadState();
    const auto* section = state.current;
    if (!section || section->registry != registry || section->window || !section->holdsMutex()) return nullptr;
    auto* slot = state.reader.registry == registry && state.reader.slot ? state.reader.slot : registryFindSlot(registry);
    if (!slot) return nullptr;
    const auto word = slot->word.load(std::memory_order_acquire);
    if (word & RegistryWindowMask) return nullptr;
    const auto* published = slot->published.get(); // guarded by R, which the caller holds
    return published && published->generation == (word >> 16) ? published : nullptr;
#endif
}

void registryAccount(RegistryThreadState& state, RegistryLockSite& site, std::uint64_t acquisitions, std::uint64_t contended,
        std::uint64_t waitTicks, std::uint64_t holdTicks, std::uint64_t maxIntervalTicks, std::uint64_t blockingHolds,
        std::uint64_t blockingHoldTicks, std::uint64_t maxBlockingHoldTicks) noexcept {
    auto& batches = state.batches;
    RegistrySiteBatch* batch = nullptr;
    for (auto& e : batches.entries) if (e.site == &site) { batch = &e; break; }
    if (!batch) {
        batch = &batches.entries[batches.next++ & 3];
        batch->flush();
        batch->site = &site;
    }
    batch->acquisitions += acquisitions;
    batch->contended += contended;
    batch->waitTicks += waitTicks;
    batch->holdTicks += holdTicks;
    if (maxIntervalTicks > batch->maxHoldTicks) batch->maxHoldTicks = maxIntervalTicks;
    batch->blockingHolds += blockingHolds;
    batch->blockingHoldTicks += blockingHoldTicks;
    if (maxBlockingHoldTicks > batch->maxBlockingHoldTicks) batch->maxBlockingHoldTicks = maxBlockingHoldTicks;
    if (++batch->sections >= 64) { auto* keep = batch->site; batch->flush(); batch->site = keep; }
}

void RegistrySection::noteAcquired(bool contended, std::uint64_t waitTicks) noexcept {
    // Acquisitions and waits are accounted with the hold at the end of the section (publishHold);
    // a relock inside a unique section adds one acquisition and its wait.
    ++acquisitionCount;
    if (contended) { ++contendedCount; contendedWaitTicks += waitTicks; }
    acquiredAt = registryTicks();
}

void RegistrySection::noteReleasedInterval() noexcept {
    const auto interval = registryTicks() - acquiredAt;
    heldTicks += interval;
    if (interval > maxIntervalTicks) maxIntervalTicks = interval;
    // v110: someone waited for R while this interval ended (one relaxed load of a line that only
    // contended acquisitions write).
    if (registryWaiters.load(std::memory_order_relaxed)) {
        ++blockingHolds;
        blockingHoldTicks += interval;
        if (interval > maxBlockingHoldTicks) maxBlockingHoldTicks = interval;
    }
}

void RegistrySection::publishHold() noexcept {
    registryAccount(thread, site, acquisitionCount, contendedCount, contendedWaitTicks, heldTicks, maxIntervalTicks,
        blockingHolds, blockingHoldTicks, maxBlockingHoldTicks);
}

// ---- v110: contended acquisition (spin, then futex), with per-role accounting.
bool registrySpinEnabled() noexcept {
    static const bool enabled = [] {
        const bool off = ::access(RECOMP_DIAG_SWITCH("diag-registry-no-spin"), F_OK) == 0;
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_REGISTRY_SPIN_MODE spin=%d rounds=256 (diag-registry-no-spin %s)", off ? 0 : 1, off ? "present: v109 futex-only" : "absent");
        return !off;
    }();
    return enabled;
}

namespace {
unsigned registryRole(RegistryThreadState& state) noexcept {
    if (state.role != RegistryRoles) return state.role;
#if RECOMP_PARALLEL_REGISTRY
    const auto token = currentGuestThreadToken();
#else
    const std::uint32_t token = 0;
#endif
    if (!token) return RegistryRoleHost; // not cached: setup code may run guest tasks later
    state.role = token == 0x63001000u ? RegistryRoleMain : token == 0x6408e000u ? RegistryRoleRender :
                 token == 0x640b1000u ? RegistryRoleTile : token == 0x64141000u ? RegistryRoleAudio : RegistryRoleOther;
    return state.role;
}
} // namespace

std::uint64_t registryLockContended(std::mutex& mutex, RegistrySection& section) noexcept {
    registryWaiters.fetch_add(1, std::memory_order_relaxed);
    const auto started = registryTicks();
    bool spun = false;
    if (registrySpinEnabled()) {
        // Same bound as the v33 reader policy: 256 rounds of 16 yields and one try_lock (~20 us on
        // the device). A try_lock-only waiter never sets the futex waiter state, so the holder's
        // unlock stays a plain store.
        for (unsigned attempt = 0; attempt < 256 && !spun; ++attempt) {
            for (unsigned k = 0; k < 16; ++k) {
#if defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
            }
            spun = mutex.try_lock();
        }
    }
    if (!spun) mutex.lock();
    const auto wait = registryTicks() - started;
    registryWaiters.fetch_sub(1, std::memory_order_relaxed);
    auto& site = section.site;
    site.roleWaitTicks[registryRole(section.thread)].fetch_add(wait, std::memory_order_relaxed);
    (spun ? site.spun : site.blocked).fetch_add(1, std::memory_order_relaxed);
    (spun ? registryCounters.spun : registryCounters.blocked).fetch_add(1, std::memory_order_relaxed);
    (spun ? registryCounters.spinTicks : registryCounters.blockTicks).fetch_add(wait, std::memory_order_relaxed);
    if (!site.listed.load(std::memory_order_relaxed)) registryListSite(site);
    return wait;
}

RegistryContentionTotals registryContentionTotals() noexcept {
    const auto& c = registryCounters;
    return {c.spun.load(std::memory_order_relaxed), c.blocked.load(std::memory_order_relaxed),
            c.spinTicks.load(std::memory_order_relaxed), c.blockTicks.load(std::memory_order_relaxed),
            registryWaiters.load(std::memory_order_relaxed)};
}

RegistryGuard::RegistryGuard(const GuestImportRegistry* r, RegistryLockSite& s) noexcept
    : RegistrySection(registryThreadState(), r, s), mutex_ref_(r->registryMutex()) {
    bool contended = false;
    std::uint64_t wait = 0;
    if (!mutex_ref_.try_lock()) {
        contended = true;
        wait = registryLockContended(mutex_ref_, *this); // v110 (v109: lock() only)
    }
    held = true;
    noteAcquired(contended, wait);
}

RegistryGuard::~RegistryGuard() {
    closeWindow(true);
    noteReleasedInterval();
    held = false;
    mutex_ref_.unlock();
    publishHold(); // thread-local accounting, after R
    registryMaybeLog(thread);
}

namespace {
void registryUniqueAcquire(RegistrySection& section, std::unique_lock<std::mutex>& lock) noexcept {
    section.unique = &lock;
    bool contended = false;
    std::uint64_t wait = 0;
    if (!lock.try_lock()) {
        contended = true;
        wait = registryLockContended(*lock.mutex(), section); // v110 (v109: lock() only)
        lock = std::unique_lock<std::mutex>(*lock.mutex(), std::adopt_lock); // deferred -> owning, same object
    }
    section.noteAcquired(contended, wait);
}
} // namespace

RegistryUnique::RegistryUnique(const GuestImportRegistry* r, std::string_view importName) noexcept
    : RegistryUnique(registryThreadState(), r, importName) {}
RegistryUnique::RegistryUnique(RegistryThreadState& state, const GuestImportRegistry* r, std::string_view importName) noexcept
    : RegistrySection(state, r, registryImportSite(state, importName)), lock_(r->registryMutex(), std::defer_lock) {
    registryUniqueAcquire(*this, lock_);
}

RegistryUnique::RegistryUnique(const GuestImportRegistry* r, RegistryLockSite& s) noexcept
    : RegistrySection(registryThreadState(), r, s), lock_(r->registryMutex(), std::defer_lock) {
    registryUniqueAcquire(*this, lock_);
}

RegistryUnique::~RegistryUnique() {
    closeWindow(true);
    if (lock_.owns_lock()) {
        noteReleasedInterval();
        lock_.unlock();
    }
    publishHold(); // thread-local accounting, after R
    registryMaybeLog(thread);
}

// imports-fast A (guest_registry_concurrency.h): RegistryUnique(state, r, importName) minus the hold accounting.
RegistryUniqueQuiet::RegistryUniqueQuiet(RegistryThreadState& state, const GuestImportRegistry* r, RegistryLockSite& s) noexcept
    : RegistrySection(state, r, s), lock_(r->registryMutex(), std::defer_lock) {
    unique = &lock_;
#if !defined(RECOMP_IMPORTS_FAST_MUTANT_QUIET_NO_LOCK)
    if (!lock_.try_lock()) {
        (void)registryLockContended(*lock_.mutex(), *this); // (its role / spun / blocked accounting is unchanged)
        lock_ = std::unique_lock<std::mutex>(*lock_.mutex(), std::adopt_lock); // deferred -> owning, same object
    }
#endif
}
RegistryUniqueQuiet::~RegistryUniqueQuiet() {
    closeWindow(true);
    if (lock_.owns_lock()) lock_.unlock();
    registryMaybeLog(thread);
}

RegistryReader::RegistryReader(const GuestImportRegistry* r, RegistryLockSite& s) noexcept
    : RegistrySection(registryThreadState(), r, s), mutex_ref_(r->registryMutex()) {
    // v32/v33 policy for renderer reads: spin up to ~20 us (256 rounds) before blocking.
    bool spun = false, blocked = false;
    std::uint64_t started = 0, blockedNs = 0;
    if (!mutex_ref_.try_lock()) {
        registryWaiters.fetch_add(1, std::memory_order_relaxed); // v110 accounting only; policy unchanged
        started = registryTicks();
        for (unsigned attempt = 0; attempt < 256 && !spun; ++attempt) {
            for (unsigned k = 0; k < 16; ++k) {
#if defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
            }
            spun = mutex_ref_.try_lock();
        }
        if (!spun) {
            const auto blockStart = registryTicks();
            mutex_ref_.lock();
            blocked = true;
            blockedNs = registryTicksToNs(registryTicks() - blockStart);
        }
        registryWaiters.fetch_sub(1, std::memory_order_relaxed);
        const auto wait = registryTicks() - started;
        site.roleWaitTicks[registryRole(thread)].fetch_add(wait, std::memory_order_relaxed);
        (spun ? site.spun : site.blocked).fetch_add(1, std::memory_order_relaxed);
        if (!site.listed.load(std::memory_order_relaxed)) registryListSite(site);
    }
    held = true;
    registryReadLockAcquired(spun, blocked, blockedNs);
    noteAcquired(started != 0, started ? registryTicks() - started : 0);
}

void RegistryReader::unlock() noexcept {
    if (!held) return;
    closeWindow(true); // a reader section ends at its unlock
    noteReleasedInterval();
    held = false;
    mutex_ref_.unlock();
}

RegistryReader::~RegistryReader() {
    unlock();
    publishHold();
    registryMaybeLog(thread);
}

void registryUnlock(std::unique_lock<std::mutex>& lock) {
    auto* section = registryThreadState().current;
    if (section && section->unique == &lock && lock.owns_lock()) {
        section->closeWindow();
        section->noteReleasedInterval();
    }
    lock.unlock();
}

void registryRelock(std::unique_lock<std::mutex>& lock) {
    bool contended = false;
    std::uint64_t wait = 0;
    auto* section = registryThreadState().current;
    if (!lock.try_lock()) {
        contended = true;
        if (section && section->unique == &lock) {
            wait = registryLockContended(*lock.mutex(), *section); // v110 (v109: lock() only)
            lock = std::unique_lock<std::mutex>(*lock.mutex(), std::adopt_lock); // deferred -> owning, same object
        } else { // not this thread's innermost unique section: v109 form, no site to account to
            const auto started = registryTicks();
            lock.lock();
            wait = registryTicks() - started;
        }
    }
    if (section && section->unique == &lock) section->noteAcquired(contended, wait);
}

void registryNoteValidationMutation(const GuestImportRegistry* self, const char* where, bool pin) noexcept {
    auto* section = registryThreadState().current;
    if (section && section->registry == self && section->holdsMutex()) {
        section->openWindow();
        if (pin && section->window) section->windowPinned = true;
        return;
    }
    registryPoison(self, where, section != nullptr);
}

void registryReadLockAcquired(bool spun, bool blocked, std::uint64_t blockedNs) noexcept {
    if (blocked) {
        registryCounters.readBlocked.fetch_add(1, std::memory_order_relaxed);
        registryCounters.readBlockedNs.fetch_add(blockedNs, std::memory_order_relaxed);
        registryMaxStore(registryCounters.readMaxBlockedNs, blockedNs);
    } else if (spun) {
        registryCounters.readSpun.fetch_add(1, std::memory_order_relaxed);
    } else {
        registryCounters.readFast.fetch_add(1, std::memory_order_relaxed);
    }
}

// Every ~5 s, from whichever thread ends a section or a fast read first, after R is released.
// Checked on every 64th call per thread (no clock read on the others).
void registryMaybeLog(RegistryThreadState& state) noexcept {
    if ((++state.logCheck & 63) != 0) return;
    const auto now = registryTicks();
    auto next = registryCounters.nextLogTicks.load(std::memory_order_relaxed);
    if (now < next) return;
    if (!registryCounters.nextLogTicks.compare_exchange_strong(next, now + registryNsToTicks(5'000'000'000ull),
            std::memory_order_relaxed)) return;
    if (next == 0) return; // First call arms the timer.
    const auto u = [](const std::atomic<std::uint64_t>& v) { return static_cast<unsigned long long>(v.load(std::memory_order_relaxed)); };
    const auto us = [](std::uint64_t ticks) { return static_cast<unsigned long long>(registryTicksToNs(ticks) / 1000); };
    auto& c = registryCounters;
    // Same line as v33-v35 (cumulative): only locked renderer reads (fallbacks, refreshes) count now.
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
        "RENDER_DIAG_REGISTRY_READ_LOCK fast=%llu spun=%llu blocked=%llu blocked_ms=%llu max_blocked_us=%llu",
        u(c.readFast), u(c.readSpun), u(c.readBlocked), u(c.readBlockedNs) / 1000000, u(c.readMaxBlockedNs) / 1000);
    std::uint64_t generation = 0, windowsOpen = 0;
    if (const auto& slot = registrySlots[0]; slot.owner.load(std::memory_order_acquire)) {
        const auto word = slot.word.load(std::memory_order_acquire);
        generation = word >> 16;
        windowsOpen = word & RegistryWindowMask;
    }
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
        "RENDER_DIAG_REGISTRY_SNAPSHOT enabled=%d hits=%llu adopted=%llu rebuilds=%llu rebuild_us=%llu max_rebuild_us=%llu "
        "rate_limited=%llu window_open=%llu no_slot=%llu mismatch=%llu rejected=%llu arena=%llu windows=%llu gen=%llu "
        "open_now=%llu poisoned=%llu heap_allocations=%zu physical_allocations=%zu protected_ranges=%zu commit_words=%zu",
        RECOMP_REGISTRY_READ_SNAPSHOT, u(c.hits), u(c.adopted), u(c.rebuilds), us(c.rebuildTicks.load(std::memory_order_relaxed)),
        us(c.maxRebuildTicks.load(std::memory_order_relaxed)), u(c.rateLimited), u(c.windowOpen), u(c.noSlot), u(c.mismatch),
        u(c.rejected), u(c.arena), u(c.windows), static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(windowsOpen), u(c.poisoned), c.lastAllocations.load(std::memory_order_relaxed),
        c.lastPhysical.load(std::memory_order_relaxed), c.lastProtected.load(std::memory_order_relaxed),
        c.lastCommitWords.load(std::memory_order_relaxed));
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
        "RENDER_DIAG_AUDIO_MAP_CACHE enabled=%d hits=%llu locked=%llu stored=%llu window_open=%llu heap_bounds_enabled=%d "
        "heap_bounds_rejects=%llu",
        audioMapCacheEnabled() ? 1 : 0, u(c.audioMapHits), u(c.audioMapLocked), u(c.audioMapStored), u(c.audioMapWindowOpen),
        heapBoundsRejectEnabled() ? 1 : 0, u(c.heapBoundsRejects));
    // Cumulative R accounting per section site (dispatch: per import name), top 12 by hold time,
    // and the top 6 by mutation windows (the sites that invalidate the snapshot).
    std::array<RegistryLockSite*, 12> top{};
    std::array<RegistryLockSite*, 6> bumpers{};
    std::uint64_t acquisitions = 0, contended = 0, waitTicks = 0, holdTicks = 0, windows = 0, sites = 0;
    for (auto* site = registrySiteList.load(std::memory_order_acquire); site; site = site->next.load(std::memory_order_relaxed)) {
        ++sites;
        acquisitions += site->acquisitions.load(std::memory_order_relaxed);
        contended += site->contended.load(std::memory_order_relaxed);
        waitTicks += site->waitTicks.load(std::memory_order_relaxed);
        const auto hold = site->holdTicks.load(std::memory_order_relaxed);
        holdTicks += hold;
        const auto opened = site->windows.load(std::memory_order_relaxed);
        windows += opened;
        for (std::size_t i = 0; opened && i < bumpers.size(); ++i) {
            if (!bumpers[i] || opened > bumpers[i]->windows.load(std::memory_order_relaxed)) {
                for (std::size_t j = bumpers.size() - 1; j > i; --j) bumpers[j] = bumpers[j - 1];
                bumpers[i] = site;
                break;
            }
        }
        for (std::size_t i = 0; i < top.size(); ++i) {
            if (!top[i] || hold > top[i]->holdTicks.load(std::memory_order_relaxed)) {
                for (std::size_t j = top.size() - 1; j > i; --j) top[j] = top[j - 1];
                top[i] = site;
                break;
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
        "RENDER_DIAG_REGISTRY_HOLD_TOTAL acq=%llu contended=%llu wait_ms=%llu hold_ms=%llu windows=%llu sites=%llu",
        static_cast<unsigned long long>(acquisitions), static_cast<unsigned long long>(contended),
        us(waitTicks) / 1000, us(holdTicks) / 1000, static_cast<unsigned long long>(windows),
        static_cast<unsigned long long>(sites));
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", // v197
        "RENDER_DIAG_REGISTRY_CS_FAST enabled=%d enters=%llu leaves=%llu tries=%llu fallbacks=%llu wait_epoch=%d skips=%llu checks=%llu",
        int(csFastEnabled()), csFastEnters.load(std::memory_order_relaxed), csFastLeaves.load(std::memory_order_relaxed),
        csFastTries.load(std::memory_order_relaxed), csFastFallbacks.load(std::memory_order_relaxed), int(waitEpochEnabled()),
        waitEpochSkips.load(std::memory_order_relaxed), waitEpochChecks.load(std::memory_order_relaxed)); // v197/v198
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_REGISTRY_ADJACENT_VIEWS views=%llu", // v207
        adjacentGraphicsViews.load(std::memory_order_relaxed));
    // "function:line" for member sections, the import name for dispatch sections (line 0).
    const auto label = [](const RegistryLockSite& site, char (&text)[96]) {
        if (site.line) std::snprintf(text, sizeof text, "%s:%d", site.function, site.line);
        else std::snprintf(text, sizeof text, "%s", site.function);
        return text;
    };
    for (std::size_t i = 0; i < top.size() && top[i]; ++i) {
        const auto& site = *top[i];
        char text[96];
        ::recomp::diag::quietLog(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_REGISTRY_HOLD rank=%zu site=%s acq=%llu contended=%llu wait_us=%llu hold_us=%llu max_hold_us=%llu windows=%llu",
            i + 1, label(site, text), u(site.acquisitions), u(site.contended),
            us(site.waitTicks.load(std::memory_order_relaxed)), us(site.holdTicks.load(std::memory_order_relaxed)),
            us(site.maxHoldTicks.load(std::memory_order_relaxed)), u(site.windows));
    }
    for (std::size_t i = 0; i < bumpers.size() && bumpers[i]; ++i) {
        const auto& site = *bumpers[i];
        char text[96];
        ::recomp::diag::quietLog(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_REGISTRY_WINDOWS rank=%zu site=%s windows=%llu acq=%llu",
            i + 1, label(site, text), u(site.windows), u(site.acquisitions));
    }
    // v110, every ~10 s (cumulative; difference two lines for a window): who waits for R (by site and
    // waiting role), how the waits ended (spin phase or futex), and whose holds made others wait.
    auto nextWait = c.nextWaitLogTicks.load(std::memory_order_relaxed);
    if (now < nextWait || !c.nextWaitLogTicks.compare_exchange_strong(nextWait, now + registryNsToTicks(9'900'000'000ull),
            std::memory_order_relaxed)) return;
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
        "RENDER_DIAG_REGISTRY_SPIN enabled=%d spun=%llu blocked=%llu spin_ms=%llu block_ms=%llu waiting_now=%llu",
        int(registrySpinEnabled()), u(c.spun), u(c.blocked), us(c.spinTicks.load(std::memory_order_relaxed)) / 1000,
        us(c.blockTicks.load(std::memory_order_relaxed)) / 1000, u(registryWaiters));
    std::array<std::uint64_t, RegistryRoles> roles{};
    std::array<RegistryLockSite*, 12> waiters{};
    std::array<RegistryLockSite*, 10> blockers{};
    for (auto* site = registrySiteList.load(std::memory_order_acquire); site; site = site->next.load(std::memory_order_relaxed)) {
        for (unsigned r = 0; r < RegistryRoles; ++r) roles[r] += site->roleWaitTicks[r].load(std::memory_order_relaxed);
        const auto wait = site->waitTicks.load(std::memory_order_relaxed);
        for (std::size_t i = 0; wait && i < waiters.size(); ++i) {
            if (!waiters[i] || wait > waiters[i]->waitTicks.load(std::memory_order_relaxed)) {
                for (std::size_t j = waiters.size() - 1; j > i; --j) waiters[j] = waiters[j - 1];
                waiters[i] = site;
                break;
            }
        }
        const auto blocking = site->blockingHoldTicks.load(std::memory_order_relaxed);
        for (std::size_t i = 0; blocking && i < blockers.size(); ++i) {
            if (!blockers[i] || blocking > blockers[i]->blockingHoldTicks.load(std::memory_order_relaxed)) {
                for (std::size_t j = blockers.size() - 1; j > i; --j) blockers[j] = blockers[j - 1];
                blockers[i] = site;
                break;
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
        "RENDER_DIAG_REGISTRY_ROLES wait_ms main=%llu render=%llu tile=%llu audio=%llu other=%llu host=%llu",
        us(roles[RegistryRoleMain]) / 1000, us(roles[RegistryRoleRender]) / 1000, us(roles[RegistryRoleTile]) / 1000,
        us(roles[RegistryRoleAudio]) / 1000, us(roles[RegistryRoleOther]) / 1000, us(roles[RegistryRoleHost]) / 1000);
    const auto roleUs = [&](const RegistryLockSite& site, unsigned role) { return us(site.roleWaitTicks[role].load(std::memory_order_relaxed)); };
    for (std::size_t i = 0; i < waiters.size() && waiters[i]; ++i) {
        const auto& site = *waiters[i];
        char text[96];
        ::recomp::diag::quietLog(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_REGISTRY_WAIT rank=%zu site=%s wait_us=%llu contended=%llu spun=%llu blocked=%llu acq=%llu "
            "main_us=%llu render_us=%llu tile_us=%llu audio_us=%llu other_us=%llu host_us=%llu",
            i + 1, label(site, text), us(site.waitTicks.load(std::memory_order_relaxed)), u(site.contended), u(site.spun),
            u(site.blocked), u(site.acquisitions), roleUs(site, RegistryRoleMain), roleUs(site, RegistryRoleRender),
            roleUs(site, RegistryRoleTile), roleUs(site, RegistryRoleAudio), roleUs(site, RegistryRoleOther),
            roleUs(site, RegistryRoleHost));
    }
    for (std::size_t i = 0; i < blockers.size() && blockers[i]; ++i) {
        const auto& site = *blockers[i];
        char text[96];
        ::recomp::diag::quietLog(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_REGISTRY_BLOCKER rank=%zu site=%s blocking_holds=%llu blocking_hold_us=%llu max_blocking_hold_us=%llu "
            "hold_us=%llu max_hold_us=%llu acq=%llu",
            i + 1, label(site, text), u(site.blockingHolds), us(site.blockingHoldTicks.load(std::memory_order_relaxed)),
            us(site.maxBlockingHoldTicks.load(std::memory_order_relaxed)), us(site.holdTicks.load(std::memory_order_relaxed)),
            us(site.maxHoldTicks.load(std::memory_order_relaxed)), u(site.acquisitions));
    }
}

// ---- snapshot predicates: exactly the v35 locked predicates, evaluated on a copy of V.
namespace {

bool snapshotOverlapsProtected(const GuestRegistryValidationSnapshot& s, std::uint64_t begin, std::uint64_t end) noexcept {
    return registryRangesOverlap(s.protectedRanges, begin, end);
}

// Sort by begin and merge ranges that overlap or touch. Merging keeps the set of query ranges that
// overlap some member: a merged range overlaps a query iff one of its parts does (degenerate parts
// included: [b,b) inside or at the edge of a merged range only adds queries it already overlaps).
void mergeRanges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::size_t merged = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (merged && ranges[i].first <= ranges[merged - 1].second)
            ranges[merged - 1].second = std::max(ranges[merged - 1].second, ranges[i].second);
        else ranges[merged++] = ranges[i];
    }
    ranges.resize(merged);
}

// resolvePhysicalRange(physical, bytes, false): the virtual address, or nothing.
// v228: per-thread last hit of the two sorted-allocation searches, keyed by the immutable snapshot (pointer +
// generation). A cached index i is used only if it is exactly what upper_bound(address)-1 returns: entry i starts at or
// below `address` and entry i+1 (if any) starts above it; otherwise the search runs and refills it. Same results.
struct SnapshotLookupCache {
    const GuestRegistryValidationSnapshot* s{nullptr}; std::uint64_t generation{~std::uint64_t{0}};
    std::size_t heap{~std::size_t{0}}, physical{~std::size_t{0}};
};
thread_local SnapshotLookupCache snapshotLookupCache;
inline SnapshotLookupCache& snapshotLookupFor(const GuestRegistryValidationSnapshot& s) noexcept {
    auto& c = snapshotLookupCache;
    if (c.s != &s || c.generation != s.generation) c = {&s, s.generation, ~std::size_t{0}, ~std::size_t{0}};
    return c;
}
std::optional<std::uint32_t> snapshotResolvePhysical(const GuestRegistryValidationSnapshot& s,
        std::uint32_t physical, std::uint32_t bytes) noexcept {
    if (!s.physicalBound || !bytes || physical < s.physicalOffset) return std::nullopt;
    const auto offset = std::uint64_t{physical} - s.physicalOffset;
    if (offset + bytes > s.physicalSize) return std::nullopt;
    const auto address = static_cast<std::uint32_t>(s.physicalBase + offset);
    if (address < 0x7FC90000 && std::uint64_t{address} + bytes > 0x7FC80000) return std::nullopt;
    const auto i = lastAllocationAtOrBelow(s.physicalAllocations, address, snapshotLookupFor(s).physical); // v228
    if (i == ~std::size_t{0}) return std::nullopt;
    const auto& found = s.physicalAllocations[i];
    if (std::uint64_t{address} + bytes > std::uint64_t{found.first} + found.second) return std::nullopt;
    return address;
}

// validGuestDataBuffer(address, bytes, writable=false, alignment=4), with contains()'s live
// isReserved() checked by the caller. `arena` reports the one case whose answer depends on the
// calling thread (its own stack in the thread arena): the caller then uses R.
bool snapshotReadable(const GuestRegistryValidationSnapshot& s, std::uint32_t address, std::uint32_t bytes,
        bool& arena) noexcept {
    const auto end = std::uint64_t{address} + bytes;
    if (!address || !bytes || (address & 3) || !s.addressSpace ||
        std::uint64_t{bytes} > GuestAddressSpace::Size || std::uint64_t{address} > GuestAddressSpace::Size - bytes) return false;
    if (snapshotOverlapsProtected(s, address, end)) return false; // fixed ranges, event pool, XAM/title records,
                                                                   // thread type word, thread objects, TLS blocks
    if (s.threadArena && address < s.threadArenaEnd && s.threadArenaBegin < end) { arena = true; return false; }
    if (s.physicalBound && address < std::uint64_t{s.physicalBase} + s.physicalSize && end > s.physicalBase) {
        if (address < s.physicalBase ||
            !snapshotResolvePhysical(s, s.physicalOffset + (address - s.physicalBase), bytes)) return false;
    }
    if (address < std::uint64_t{s.heapBase} + s.heapSize && end > s.heapBase) {
        const auto i = lastAllocationAtOrBelow(s.heapAllocations, address, snapshotLookupFor(s).heap); // v228
        if (address < s.heapBase || i == ~std::size_t{0} ||
            end > std::uint64_t{s.heapAllocations[i].first} + s.heapAllocations[i].second) return false;
        for (auto page = (address - s.heapBase) / GuestImportRegistry::PageSize;
                page <= (end - 1 - s.heapBase) / GuestImportRegistry::PageSize; ++page) {
            if (page / 64 >= s.commitBits.size() || !(s.commitBits[page / 64] & (std::uint64_t{1} << (page % 64)))) return false;
        }
    }
    return true;
}

// copySerializedGuestHeap's predicate.
const std::uint8_t* snapshotHeapView(const GuestRegistryValidationSnapshot& s, std::uint32_t address, std::uint32_t bytes,
        bool& arena) noexcept {
    if (!s.addressSpace || !bytes || bytes > 65536 || address < s.heapBase ||
        std::uint64_t{address} + bytes > std::uint64_t{s.heapBase} + s.heapSize ||
        !snapshotReadable(s, address, bytes, arena)) return nullptr;
    return s.addressSpace->translate(address);
}

// viewSerializedGraphicsDataLocked's predicate.
const std::uint8_t* snapshotGraphicsView(const GuestRegistryValidationSnapshot& s, std::uint32_t address,
        std::uint32_t bytes) noexcept {
    if (!s.addressSpace || !s.physicalBound || !s.graphicsReadable || !bytes || bytes > 64u * 1024 * 1024 ||
        (address & 3)) return nullptr;
    std::uint64_t physical = address;
    if (address >= 0x20000000) {
        if (address < s.physicalBase || std::uint64_t{address} + bytes > std::uint64_t{s.physicalBase} + s.physicalSize)
            return nullptr;
        physical = std::uint64_t{s.physicalOffset} + address - s.physicalBase;
    }
    if (physical > UINT32_MAX) return nullptr;
    const auto virtualAddress = snapshotResolvePhysical(s, static_cast<std::uint32_t>(physical), bytes);
    if (!virtualAddress) return nullptr;
    return s.addressSpace->translate(*virtualAddress);
}

} // namespace

void GuestImportRegistry::captureValidationSnapshotLocked(GuestRegistryValidationSnapshot& s) const {
    s.addressSpace = addressSpace_;
    s.heapBase = heapBase_;
    s.heapSize = heapSize_;
    s.heapAllocations.assign(allocations_.begin(), allocations_.end());
    s.commitBits = virtualCommitBits_;
    auto& ranges = s.protectedRanges;
    ranges.clear();
    ranges.reserve(threadObjects_.size() + threadTlsStorage_.size() + 8);
    ranges.emplace_back(0x7FC80000u, 0x7FC90000u);
    ranges.emplace_back(0x7FEA0000u, 0x7FEB0000u);
    if (nativeEventPool_) ranges.emplace_back(nativeEventPool_, std::uint64_t{nativeEventPool_} + PageSize);
    if (nativeXamAddress_) ranges.emplace_back(nativeXamAddress_, std::uint64_t{nativeXamAddress_} + NativeModuleRecordSize);
    if (nativeTitleAddress_) ranges.emplace_back(nativeTitleAddress_, std::uint64_t{nativeTitleAddress_} + NativeModuleRecordSize);
    if (threadObjectTypeAddress_) ranges.emplace_back(threadObjectTypeAddress_, std::uint64_t{threadObjectTypeAddress_} + 4);
    for (const auto& [token, unused] : threadObjects_) {
        (void)unused;
        ranges.emplace_back(token, std::uint64_t{token} + GuestThreadEnvironment::ThreadSize);
    }
    s.tlsRanges.clear();
    s.tlsRanges.reserve(threadTlsStorage_.size());
    for (const auto& [unusedToken, storage] : threadTlsStorage_) {
        (void)unusedToken; // slotCount 0 gives [base,base): overlapsThreadTlsStorage's formula, kept exactly
        s.tlsRanges.emplace_back(storage.dynamicSlotBase, std::uint64_t{storage.dynamicSlotBase} + std::uint64_t{storage.slotCount} * 4U);
    }
    ranges.insert(ranges.end(), s.tlsRanges.begin(), s.tlsRanges.end());
    mergeRanges(ranges);
    mergeRanges(s.tlsRanges);
    s.threadArena = threadStorage_.ready();
    s.threadArenaBegin = threadStorageConfig_.arena.base;
    s.threadArenaEnd = std::uint64_t{threadStorageConfig_.arena.base} + threadStorageConfig_.arena.size;
    s.physicalBound = physicalMemoryArenaBound_;
    s.physicalBase = physicalArenaBase_;
    s.physicalSize = physicalArenaSize_;
    s.physicalOffset = physicalAddressOffset_;
    s.physicalAllocations.clear();
    s.physicalAllocations.reserve(physicalAllocations_.size());
    for (const auto& [base, allocation] : physicalAllocations_) s.physicalAllocations.emplace_back(base, allocation.size);
    s.graphicsReadable = graphicsInitialized_ && graphicsBackend_ && graphicsBackend_->ready();
}

// v239: the last successful snapshot views of this thread (64 direct-mapped entries), keyed by the registry, the snapshot object, its
// generation and the exact (address, byteCount, graphics) query. snapshotHeapView / snapshotGraphicsView are pure functions
// of an immutable snapshot and the query, so an entry returns what the call would; it is consulted only where the call is
// made (after every check above it). v227 outdoor render-thread profile: the two views 4.3% (the device shadow, index
// headers and shader code are viewed again on every draw). A/B: diag-v239-baseline (recomp_v239_baseline, set by the renderer).
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v239_baseline;
namespace {
// v292: the 64 entries live in RegistryThreadState (one TLS resolution per fastValidatedView instead of two).
inline bool viewMemoOn() { return !(&recomp_v239_baseline && recomp_v239_baseline.load(std::memory_order_relaxed)); }
}
// v304b: the memo's slots: 1024 (hash bits 54-63) unless diag-v304b-baseline (v239's 64: bits 58-63, slots 0-63). The v300 outdoor
// render-thread profile: snapshotHeapView (the memo's misses) 2.3% of the thread. Entries are keyed exactly as before.
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v304b_baseline;
namespace {
inline unsigned viewMemoShift() { return (&recomp_v304b_baseline && recomp_v304b_baseline.load(std::memory_order_relaxed)) ? 58u : 54u; }
}
// v322: an 8-entry most-recent front of the memo, keyed exactly as its entries (the v318 drive profile: the memo's hashed
// slot lookup, a likely L1 miss in a 1024-entry table, ~1% of the render thread; the hooks repeat the same few views per
// draw: the device's constant banks and shadow, shader headers). A/B: diag-v322-baseline (recomp_v322_baseline).
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v322_baseline;
namespace {
inline bool viewMruOn() { return !(&recomp_v322_baseline && recomp_v322_baseline.load(std::memory_order_relaxed)); }
}
const std::uint8_t* GuestImportRegistry::fastValidatedView(std::uint32_t address, std::uint32_t byteCount, bool graphics) const {
#if !RECOMP_REGISTRY_READ_SNAPSHOT
    (void)address; (void)byteCount; (void)graphics;
    return nullptr;
#else
    auto& state = registryThreadState();
    auto& cache = state.reader;
    auto& c = registryCounters;
    cache.outsideHeap = false; // v152: set below only by a current snapshot's bounds test
    // Written only by initialize()/reset() (atomic stores): a registry that is not initialized (or a
    // new one constructed where an old one was) never matches a snapshot.
    auto* const live = __atomic_load_n(&addressSpace_, __ATOMIC_ACQUIRE);
    if (!live) return nullptr;
    auto* slot = cache.registry == this ? cache.slot : registryFindSlot(this);
    if (!slot) { c.noSlot.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    ++cache.readsSinceRebuild;
    const auto word = slot->word.load(std::memory_order_acquire);
    if (word & RegistryWindowMask) { c.windowOpen.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    if (cache.registry != this || cache.slot != slot || cache.generation != (word >> 16) || !cache.snapshot) {
        // Rebuild only when it costs at most ~1 us per read of this thread since its last rebuild;
        // otherwise go straight to the locked path (one R acquisition, as v35), unless another
        // reader already published this generation (then adopting it is one short R hold).
        const bool rebuildAllowed = cache.readsSinceRebuild >= 16 && cache.lastRebuildNs <= cache.readsSinceRebuild * 1000;
        if (!rebuildAllowed && slot->publishedGeneration.load(std::memory_order_acquire) != (word >> 16)) {
            c.rateLimited.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        std::shared_ptr<const GuestRegistryValidationSnapshot> retiredCache, retiredPublished; // freed after R
        {
            RECOMP_REGISTRY_READER(lock);
            const auto locked = slot->word.load(std::memory_order_acquire);
            if (locked & RegistryWindowMask) { c.windowOpen.fetch_add(1, std::memory_order_relaxed); return nullptr; }
            const auto generation = locked >> 16;
            if (slot->published && slot->published->generation == generation) {
                c.adopted.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (!rebuildAllowed) { c.rateLimited.fetch_add(1, std::memory_order_relaxed); return nullptr; }
                std::shared_ptr<GuestRegistryValidationSnapshot> fresh;
                const auto started = registryTicks();
                try {
                    fresh = std::make_shared<GuestRegistryValidationSnapshot>();
                    captureValidationSnapshotLocked(*fresh);
                } catch (...) { return nullptr; }
                fresh->generation = generation;
                const auto spent = registryTicks() - started;
                c.rebuilds.fetch_add(1, std::memory_order_relaxed);
                c.rebuildTicks.fetch_add(spent, std::memory_order_relaxed);
                registryMaxStore(c.maxRebuildTicks, spent);
                c.lastAllocations.store(fresh->heapAllocations.size(), std::memory_order_relaxed);
                c.lastPhysical.store(fresh->physicalAllocations.size(), std::memory_order_relaxed);
                c.lastProtected.store(fresh->protectedRanges.size(), std::memory_order_relaxed);
                c.lastCommitWords.store(fresh->commitBits.size(), std::memory_order_relaxed);
                cache.lastRebuildNs = registryTicksToNs(spent);
                cache.readsSinceRebuild = 0;
                retiredPublished = std::move(slot->published);
                slot->published = std::move(fresh);
                slot->publishedGeneration.store(generation, std::memory_order_release);
            }
            retiredCache = std::move(cache.snapshot);
            cache.snapshot = slot->published;
            cache.generation = generation;
            cache.slot = slot;
            cache.registry = this;
        }
    }
    const auto& snapshot = *cache.snapshot;
    if (snapshot.addressSpace != live) { c.mismatch.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    if (!live->isReserved()) return nullptr;
    // v152: exactly the first test of viewSerializedGuestHeap/copySerializedGuestHeap's locked predicate, on the snapshot
    // of the generation just loaded (heapBase_/heapSize_ are in V): its rejection is the locked answer at that load.
    if (!graphics && (!byteCount || byteCount > 65536 || address < snapshot.heapBase ||
            std::uint64_t{address} + byteCount > std::uint64_t{snapshot.heapBase} + snapshot.heapSize) && heapBoundsRejectEnabled()) {
        cache.outsideHeap = true;
        c.heapBoundsRejects.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    bool arena = false;
    const std::uint8_t* bytes = nullptr;
    ViewMemoEntry* memo = nullptr;
    bool mruHit = false;
    const bool mru = viewMemoOn() && viewMruOn();
    if (mru) { // v322
        for (const auto& e : state.viewMru)
            if (e.view && e.address == address && e.bytes == byteCount && e.graphics == graphics && e.snapshot == &snapshot &&
                e.generation == cache.generation && e.registry == this) { bytes = e.view; mruHit = true; break; }
    }
    if (!mruHit && viewMemoOn()) { // v239
        memo = &state.viewMemo[((std::uint64_t{address} * 0x9E3779B97F4A7C15ull) ^ (std::uint64_t{byteCount} << 1 | graphics)) >> viewMemoShift()];
        if (memo->view && memo->registry == this &&
#if !defined(RECOMP_V239_MUTANT_STALE_KEY) // test mutant: neither the snapshot nor its generation is compared
            memo->snapshot == &snapshot && memo->generation == cache.generation &&
#endif
            memo->address == address &&
            memo->bytes == byteCount && memo->graphics == graphics) bytes = memo->view;
    }
    if (!bytes) {
        bytes = graphics ? snapshotGraphicsView(snapshot, address, byteCount)
                         : snapshotHeapView(snapshot, address, byteCount, arena);
        if (memo && bytes) *memo = {this, &snapshot, cache.generation, address, byteCount, graphics, bytes};
    }
    if (mru && bytes && !mruHit) state.viewMru[state.viewMruNext++ & 7u] = {this, &snapshot, cache.generation, address, byteCount, graphics, bytes};
    if (bytes) {
        if (++cache.unflushedHits == 256) { // per-thread batch: the hit path touches no shared line
            c.hits.fetch_add(cache.unflushedHits, std::memory_order_relaxed);
            cache.unflushedHits = 0;
            registryMaybeLog(state);
        }
    } else (arena ? c.arena : c.rejected).fetch_add(1, std::memory_order_relaxed);
    return bytes;
#endif
}

// v152: true right after a fastValidatedView call of this thread that proved the read outside the heap bounds.
bool registryFastHeapBoundsRejected() noexcept {
#if !RECOMP_REGISTRY_READ_SNAPSHOT
    return false;
#else
    return registryThreadState().reader.outsideHeap;
#endif
}

// span-move (diag-sm-worker; span_move.h): the render thread's registry snapshot for the Vulkan worker's span validation.
// registryGraphicsSnapshotFor: the snapshot the CALLING thread's fast path holds, if it is exactly the one `word` names (no
// mutation window open, the same generation: guest_registry_concurrency.h point 4: it then equals V at the caller's load of
// `word`), else empty. It never takes R and never rebuilds (the caller then validates inline, as before); fastValidatedView's own
// cache refresh (the hooks' reads on this thread) keeps it current. The live addressSpace_ check of fastValidatedView is implied:
// addressSpace_ is written only through RECOMP_V (a window, then a new generation).
std::shared_ptr<const void> registryGraphicsSnapshotFor(const GuestImportRegistry* registry, std::uint64_t word) noexcept {
#if !RECOMP_REGISTRY_READ_SNAPSHOT
    (void)registry; (void)word;
    return {};
#else
    if (!registry || (word & RegistryWindowMask)) return {};
    auto& cache = registryThreadState().reader;
    if (cache.registry != registry || !cache.slot || !cache.snapshot || cache.generation != (word >> 16)) return {};
    if (cache.slot->word.load(std::memory_order_acquire) != word) return {}; // (the slot the word belongs to, still unchanged)
    const auto& s = *cache.snapshot;
    if (s.generation != (word >> 16) || !s.addressSpace || !s.addressSpace->isReserved()) return {};
    return std::shared_ptr<const void>(cache.snapshot);
#endif
}
namespace {
// physical_adjacent_cover.h's adjacentAllocationCover over the snapshot's ascending (base, size) list (the map it copies)
std::optional<std::uint32_t> snapshotAdjacentCover(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& allocations,
        std::uint32_t address, std::uint64_t bytes, unsigned maximumPieces = 16) noexcept {
    if (!bytes) return std::nullopt;
    auto found = std::upper_bound(allocations.begin(), allocations.end(), address,
        [](std::uint32_t value, const std::pair<std::uint32_t, std::uint32_t>& e) { return value < e.first; });
    if (found == allocations.begin()) return std::nullopt;
    --found;
    const std::uint32_t first = found->first;
    std::uint64_t cursor = address;
    const std::uint64_t end = std::uint64_t{address} + bytes;
    for (unsigned piece = 0; piece < maximumPieces; ++piece) {
        const std::uint64_t base = found->first, limit = base + found->second;
        if (cursor < base || cursor >= limit) return std::nullopt;
        if (end <= limit) return first;
        cursor = limit;
        if (++found == allocations.end() || std::uint64_t{found->first} != cursor) return std::nullopt; // a gap
    }
    return std::nullopt;
}
}
// registrySnapshotGraphicsView: viewSerializedGraphicsDataLocked's predicate on an immutable snapshot (any thread, no lock):
// snapshotGraphicsView, then the locked path's v207 opt-in (files/private-trace/diag-registry-adjacent-graphics-views, read once
// here as there): a range the single-allocation rule refuses, covered by owned physical allocations that follow one another.
const std::uint8_t* registrySnapshotGraphicsView(const void* snapshot, std::uint32_t address, std::uint32_t bytes) noexcept {
    if (!snapshot) return nullptr;
    const auto& s = *static_cast<const GuestRegistryValidationSnapshot*>(snapshot);
    if (const auto* view = snapshotGraphicsView(s, address, bytes)) return view;
    static const bool adjacent = ::access(RECOMP_APP_PATH("/private-trace/diag-registry-adjacent-graphics-views"), F_OK) == 0;
    if (!adjacent) return nullptr;
    // the locked path's tests before its resolvePhysicalRange (each refusal there is final), then its adjacency branch
    if (!s.addressSpace || !s.physicalBound || !s.graphicsReadable || !bytes || bytes > 64u * 1024 * 1024 || (address & 3)) return nullptr;
    std::uint64_t physical = address;
    if (address >= 0x20000000) {
        if (address < s.physicalBase || std::uint64_t{address} + bytes > std::uint64_t{s.physicalBase} + s.physicalSize) return nullptr;
        physical = std::uint64_t{s.physicalOffset} + address - s.physicalBase;
    }
    if (physical > UINT32_MAX || physical < s.physicalOffset) return nullptr;
    const auto offset = physical - s.physicalOffset;
    if (offset + bytes > s.physicalSize) return nullptr;
    const auto arena = static_cast<std::uint32_t>(s.physicalBase + offset);
    if (arena < 0x7FC90000 && std::uint64_t{arena} + bytes > 0x7FC80000) return nullptr;
    if (!snapshotAdjacentCover(s.physicalAllocations, arena, bytes)) return nullptr;
    adjacentGraphicsViews.fetch_add(1, std::memory_order_relaxed);
    return s.addressSpace->translate(arena);
}

} // namespace recomp::bootstrap
