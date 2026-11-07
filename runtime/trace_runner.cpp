#include "ios_renderer_async.hpp"
extern "C" void recomp_autopilot_note_human() noexcept; // (ios-duo: trainer_glue.inc, in indirect_call_resolver.cpp)
#include "ios_gpu_frame_bridge.h"
#include "ios_async_shared_dispatch.hpp"
#include "ios_guest_frame_limit.hpp"
#include "ios_bot_commit.hpp"
#include "ios_renderer_qos.hpp"
#if defined(__APPLE__)
#include "apple/thread_policy.h"
#endif
#include "vulkan_queue_lock_all.h" // v177: every VkQueue call in this file holds the shared queue lock
#include "diag_log_sample.h" // v218
#include <vector>
#include <atomic> // v183
#include <functional> // v183
#include <array> // v183
#include <string> // v183
#include "diag_timeline.h" // v159
#include "latency_probe_v318.h" // v318
#include "latency_lat2.h" // latency-path (lat2): per-thread pad reads
#include <chrono>
#include <mutex>
#include <map>
#include <thread>
#include "vulkan_queue_lock.h" // v119
#include "ppc_config.h"
#include "ppc_context.h"
#include "ppc_recomp_shared.h"
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include "generated_guest_import_bridge.h"
#include "guest_address_space.h"
#include "guest_clock.h"
#include "guest_execution_guard.h"
#include "guest_image_loader.h"
#include "guest_import_registry.h"
#include "private_gamepad_reader_selector.h"
#if defined(RECOMP_PRIVATE_TRACE_LIBRARY)
#include "private_gamepad_observer_lifecycle.h"
#endif
namespace recomp::bootstrap { const char* exchangeGuestHostActivity(const char* label) noexcept; } // v226 (executor)
namespace { struct HostActivityScope { const char* p; explicit HostActivityScope(const char* l) noexcept : p(recomp::bootstrap::exchangeGuestHostActivity(l)) {}
    ~HostActivityScope() { recomp::bootstrap::exchangeGuestHostActivity(p); } }; } // v226 DIAGNOSTIC
#if defined(RECOMP_NATIVE_XMA)
#include "native_xma_context_pool.h"
#include <android/log.h>
#include <cstdio>
#endif
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE)
#include "renderer_boundary_trace.h"
#include "native_tile_scene_handoff.h"
#endif
#include "guest_cooperative_executor.h"
namespace recomp::bootstrap {
// Attempt v29 (defined in guest_cooperative_executor.cpp): the renderer's budget-1 host-only gate as
// a lock instead of a stop of the world; the cooperative build keeps runNativeNonYielding(f,1).
bool runNativeLockedGate(GuestCooperativeExecutor& executor,const GuestExecutionGuard::Entry& entry);
// Attempt v43 (defined in guest_cooperative_executor.cpp): whether that gate would admit the calling task now
// (1), refuse it (0), or unknown (-1, cooperative build).
int nativeLockedGateAdmits(GuestCooperativeExecutor& executor);
// Attempt v110 (defined in guest_cooperative_executor.cpp): exclusive-scope site names (RENDER_DIAG_EXCL_SITE),
// and one exclusive span around nested scopes (diag-irq-split-scopes restores v109's separate scopes).
const char* noteGuestExclusiveSite(const char* site) noexcept;
bool runNativeExclusiveSpan(GuestCooperativeExecutor& executor,const GuestExecutionGuard::Entry& entry);
bool bindGuestGraphicsSubmissionWaiter(std::function<bool(const std::function<bool()>&)> waiter); // v119
std::uint64_t guestGraphicsSubmissionParkedEntries() noexcept; // v119
}
namespace {
// v110: names the exclusive scopes this host thread opens while it lives (restores the outer name).
struct ExclusiveSiteLabel {
    const char* previous;
    explicit ExclusiveSiteLabel(const char* site) noexcept : previous(recomp::bootstrap::noteGuestExclusiveSite(site)) {}
    ~ExclusiveSiteLabel() { recomp::bootstrap::noteGuestExclusiveSite(previous); }
    ExclusiveSiteLabel(const ExclusiveSiteLabel&) = delete;
    ExclusiveSiteLabel& operator=(const ExclusiveSiteLabel&) = delete;
};
}
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE)
// Attempt v43 (defined in renderer_boundary_trace.cpp): the asynchronous display for generic presents and the
// lock gate's admission check; cleared by bind/unbindRendererInlineCapture.
namespace recomp::gpu::generic {bool installWriteWatch(std::uint8_t* base);} // v260 (generic_renderer.cpp)
void bindRendererDiagnosticV43(std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)> present,
    std::function<int()> admits);
void bindRendererDiagnosticV158Pair(std::function<bool(const std::vector<std::uint8_t>&,const std::vector<std::uint8_t>&,unsigned,unsigned,std::chrono::microseconds)> pair); // v158
#endif
// Attempt v78 (defined in generic_renderer.cpp, linked into every attempt build): submit what the generic renderer
// recorded since the last guest ring submission (no-op when unavailable, idle, or diag-generic-no-chunk-submit).
namespace recomp::gpu::generic {void submitPending();
// Attempt v111 (generic_renderer.h): returns once the pipelined renderer has executed every draw queued so far (no-op
// with the pipeline off, files/private-trace/diag-generic-sync-renderer).
void guestReadBarrier();
bool queueGuestWork(std::function<void()> work); // v183 (generic_renderer.h)
void noteCompletionPublishing(bool starting); // v210 (generic_renderer.cpp)
std::uint64_t queueGuestWorkMarked(std::function<void()> work);bool guestQueueReached(std::uint64_t seq);void guestDrainTo(std::uint64_t seq);} // v211
namespace recomp::bootstrap { // v211 (guest_execution_guard.cpp, generated_guest_import_bridge.cpp)
bool tryEnterGuestGraphicsSubmissionExclusion() noexcept;void leaveGuestGraphicsSubmissionExclusion() noexcept;
bool insideGuestGraphicsSubmission() noexcept;void setGuestImportEntryHook(void (*hook)()) noexcept;}
// v183 (guest_import_registry.cpp): the submitting thread's barrier hook, called by native submission handlers and the
// ring walk before they publish completion.
namespace recomp::bootstrap {void setNativeSubmissionCommitHook(void (*hook)()) noexcept;void noteNativeSubmissionMayCommit() noexcept;
    void noteNativeSubmissionMayCommitAt(unsigned site) noexcept;extern std::array<std::atomic<unsigned long long>,32> nativeSubmissionCommitSites;
    unsigned long long v217QuietReturnCount() noexcept;}
// v183: one renderer drain per submission at most, on the submitting thread, only when a completion is about to be
// published synchronously (see the submission handler). Counted for RENDER_DIAG_DEFERRED_RETIRE.
namespace {
// v218: exact totals of the sampled RENDER_DIAG_TILE_IRQ / RENDER_DIAG_SUBMIT lines (RENDER_DIAG_TILE_IRQ_TOTALS, 10 s).
struct TileIrqTotals {std::atomic<unsigned long long> ran{0},refused{0},failed{0};} tileIrqTotals;
std::atomic<unsigned long long> submitLines{0},tileIrqLines{0};
template<class R> bool tileIrqNoted(const R& result) noexcept { // true: log this result (every failure, sampled successes)
    const bool ok=result.admitted && result.returned;
    (ok?tileIrqTotals.ran:result.admitted?tileIrqTotals.failed:tileIrqTotals.refused).fetch_add(1,std::memory_order_relaxed);
    return !ok || recomp::diag::sampled(tileIrqLines);
}
thread_local bool submissionBarrierDone=false;
thread_local bool completionSectionOpen=false; // v210: from the submission's drain to the end of its handler
// v211: the deferred scene interrupt's deliverer (set in runTraceMain) and the import-entry hook that calls it.
std::function<bool(int)> deferredIrqDeliver;
void deferredIrqImportHook(){if(deferredIrqDeliver)(void)deferredIrqDeliver(0);}
}
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v233_baseline; // v233 (generic_renderer.cpp)
extern "C" std::atomic<unsigned> recomp_v338_presents,recomp_v338_spin_main_us; // v338 (generic_renderer.cpp: causal spins)
namespace {
std::atomic<bool> tileRecordingSeen{false};
std::atomic<unsigned long long> submissionBarriers{0},deferredStoreFailures{0};
void submissionBarrier() {
    if(submissionBarrierDone)return;
    submissionBarrierDone=true;submissionBarriers.fetch_add(1,std::memory_order_relaxed);
    recomp::gpu::generic::guestReadBarrier();
    // v210: this submission may now publish completion (the drain above is the handler's "before any completion"
    // point); swapped-stream reuse pauses until the handler ends (see generic_renderer.cpp stableEpoch).
    if(!completionSectionOpen){completionSectionOpen=true;recomp::gpu::generic::noteCompletionPublishing(true);}
}
}
// Attempt v78 (defined in vulkan_engine.cpp): native queue fences so far.
namespace recomp::gpu {std::uint64_t nativeQueueFenceCount() noexcept;}
#include "diag_async_presenter.h"
#include "pooled_host.h" // v240
#include "guest_thread_entry.h"
#include "guest_title_environment.h"
#include "guest_thread_environment.h"
#include "guest_xex_header.h"
#include "guest_data_variable_binding.h"
#include "guest_kernel_version.h"
#include "guest_native_read_only_directory.h"
#include "guest_video_device_globals.h"
#include "native_scaler_timing.h"
#include "guest_stopped_gpu_submission.h"
#include "xenos_indirect_inspection.h"
#include "stopped_submission_capture.h"
#include "xenos_dma_triangle_draw.h"
#include <fstream>
#include "xenos_stopped_work.h"
#include "xenos_stopped_shader_capture.h"
#include "native_deferred_shader_capture.h"
#include "xenos_stopped_physical_capture.h"
#include "xenos_stopped_memory.h"
#include "xenos_stopped_word_staging.h"
#include "xenos_copy_shader.h"
#include "xenos_stopped_constant_initialization.h"
#include "native_swap_image_file.h"
#include "private_trace_api.h"
#include "scanout_map_timing.hpp"
#if defined(__APPLE__)
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#include <atomic>
// (Claude 2026-10-17) the host thread id of the game's main loop (stored by Main.mm's pad-poll hook): ios-qos-audio leaves it alone
extern "C" { std::atomic<std::uint64_t> recomp_ios_main_tid{0}; std::atomic<unsigned> recomp_ios_main_port{0},recomp_ios_audio_port{0}; } // (ports: the slow-frame line's per-thread CPU, generic_renderer.cpp)
#endif
#endif
#include "private_display_sequence.h"
#if defined(RECOMP_PRIVATE_TRACE_LIBRARY)
#include <atomic>
#endif
#include "indirect_call_resolver.h"
#if (defined(__ANDROID__) || defined(__APPLE__))
#include "vulkan_engine.h"
#include "direct_present.h" // v255
#ifdef RECOMP_THOR_EDITION
#include "private_trace_hud_api.h" // hud2 (v575): recomp_set_hud_window_v1
#include "map_tap.h" // thor-maptap (v658): the app's map-tap request -> the pause map, pressed by the library
#endif /* RECOMP_THOR_EDITION */
#if defined(__APPLE__)
#include "apple/audio_output.h"
#else
#include "aaudio_output_stream.h"
#endif
#include "audio_ahead.h" // audio-ahead: diag-au-ahead (the callback worker's bounded lead while main waits on the engine)
#include "native_color_program_input.h"
#include "native_color_batch_capture.h"
#include "native_deferred_color_capture.h"
#include "native_deferred_color_memory.h"
#include "native_deferred_color_result.h"
#include "native_deferred_color_write_guard.h"
#include "native_color_chain_execution.h"
#include "native_color_scratch_memory.h"
#include "native_color_output_publication.h"
#include "xenos_native_initialization.h"
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <optional>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// ===== DIAGNOSTIC SKIP BUILD ONLY (generated by build_diagnostic_skip_v680.py) =====
// Last-resort retirement of a graphics submission no native handler accepted.
// Nothing is drawn. Only CPU-observable words are written (SHD/MEM_WRITE values,
// FABRICATED ZPD counts), tile-command IRQs run through the qualified handoff,
// then the read pointer advances. Output after this point is wrong by construction.
#include <set>
#include "gta5_tile_scene_continuation.h"
#include "diagnostic_thread_labels.h" // Multicore plan WP4: concurrent-safe label records and ring.
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS != 0
#include "guest_parallel_wake.h"
#endif
// Attempt v44: the retirement walk, its namespace-scope state and the runtime-maintenance precheck
// live in diagnostic_retire.h (host-testable; R held only for the ring snapshot and the publication).
#include "diagnostic_retire.h"
#if defined(__APPLE__) && !defined(RECOMP_TOUCH_MAPTAP)
#define RECOMP_TOUCH_MAPTAP 1 // ios-qol (iPad build 231): the single-screen edition's map-tap macro (android-qol v700)
#endif
#if defined(__APPLE__) && !defined(RECOMP_TOUCH_CONTEXT)
#define RECOMP_TOUCH_CONTEXT 1 // ios-touchpad (iPad build 235): the control context for the universal touch controls (android-qol v702)
#endif
#if defined(RECOMP_TOUCH_CONTEXT)
#include "touch_context.h" // qol-touch (v702): the control context export
#endif /* RECOMP_TOUCH_CONTEXT */
#if defined(RECOMP_TOUCH_MAPTAP)
#include "map_tap.h" // thor-maptap (v658) / qol-touch (v700): the app's map-tap request -> the pause map, pressed by the library
#endif

namespace recomp::bootstrap { void diagnosticDumpImports(); }
namespace recomp::audio { void diagnosticDumpXma(); }
namespace diagnostic_skip {
// Attempt v2: views of static labels (no per-entry copies); the profile is keyed by the label's address.
// Multicore plan WP4: thread-confined records (diagnostic_thread_labels.h), safe for concurrent sinks.
ThreadLabelBook threadLabelBook;
void recordThreadLabel(std::string_view label) {
    // Attempt v9: sample 1 entry in 8 (entry counts stay in units of real entries). The ring, anchor and
    // profile were ~8% of busy threads' time when updated on every function entry.
    if(!ThreadLabelBook::sample())return;
    if(!executor)return;
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS != 0
    const auto token=recomp::bootstrap::currentGuestThreadToken(); // Thread-local identity (WP1).
#else
    const auto token=executor->currentTaskToken();
#endif
    if(!token)return;
    threadLabelBook.record(token,label);
}
extern "C" void recomp_native_probe_dump();
void logThreads() {
    // One dump at a time: a trigger that arrives while another thread dumps is skipped.
    static std::mutex dumpMutex;
    std::unique_lock dump(dumpMutex,std::try_to_lock);if(!dump.owns_lock())return;
    static std::chrono::steady_clock::time_point last{};const auto now=std::chrono::steady_clock::now();
    // Attempt v2: every 10 s, or at once when the harness drops a trigger file in the app's run root.
    constexpr const char* Trigger=RECOMP_APP_PATH("/private-trace/diag-dump-now");
    const bool forced=::access(Trigger,F_OK)==0;
    if(forced)::unlink(Trigger);
    if(!forced && last.time_since_epoch().count() && now-last<std::chrono::seconds(10))return;
    if(forced)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DUMP_TRIGGERED");
    last=now;
    recomp::bootstrap::diagnosticDumpImports(); // DIAGNOSTIC v693, same 30 s limit
    recomp::audio::diagnosticDumpXma(); // DIAGNOSTIC v697
    recomp_native_probe_dump(); // Attempt v10: script natives called since the last dump.
    threadLabelBook.dump(); // RENDER_DIAG_THREAD / RENDER_DIAG_PROFILE, formats unchanged.
}
void logScratch(Registry& registry,recomp::bootstrap::GuestAddressSpace& space) {
    auto& mutex=registry.*get(MutexTag{});if(!mutex.try_lock())return;
    std::lock_guard lock(mutex,std::adopt_lock);
    const auto& config=registry.*get(RingTag{});if(!config || !config->writeBack)return;
    char t[200];int w=std::snprintf(t,sizeof t,"read=%u",config->readWord);
    for(unsigned k=0;k<16 && w>0 && w<(int)sizeof t-12;++k) {
        if(k>=8 && k<15)continue;const auto* p=space.translate(config->writeBack->virtualAddress-60+k*4);if(!p)continue;
        w+=std::snprintf(t+w,sizeof t-w," %s%u=%02x%02x%02x%02x",k==15?"rptr":"s",k==15?0u:k,p[0],p[1],p[2],p[3]);
    }
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCRATCH %s",t);
    if(scratchAddr && std::uint64_t{scratchAddr}+32<=0x20000000u) { // Attempt v1: the SCRATCH_ADDR block itself.
        w=std::snprintf(t,sizeof t,"addr=%08x mask=%08x",scratchAddr,scratchMask);
        for(unsigned k=0;k<8 && w>0 && w<(int)sizeof t-16;++k){const auto* p=space.translate(0xA0000000u+scratchAddr+k*4);if(!p)continue;
            w+=std::snprintf(t+w,sizeof t-w," s%u=%02x%02x%02x%02x",k,p[0],p[1],p[2],p[3]);}
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCRATCH_BLOCK %s",t);
    }
    { // DIAGNOSTIC v699: D3D device fence/ring words and tile-slot fences (device 1004cd00).
        const auto be=[&](std::uint32_t a)->long long{const auto* p=space.translate(a);return p?(long long)((std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3]):-1LL;};
        constexpr std::uint32_t Device=0x1004cd00;char d[400];int n=std::snprintf(d,sizeof d,"dev");
        for(std::uint32_t o=11008;o<=11056 && n>0 && n<(int)sizeof d-24;o+=4)n+=std::snprintf(d+n,sizeof d-n," +%u=%08llx",o,(unsigned long long)be(Device+o));
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DEVICE %s",d);
        // Attempt v7: D3D tiling/recording gates of the kick (sub_8235E218) and kickoff (sub_8232E5C8):
        // +13600 non-zero or +11068 bit 0x80 keeps kicked commands in the recording list (+13940..13944)
        // instead of the ring; +11069 bit 0x20 skips the kickoff; +48 is D3D's command write pointer.
        n=std::snprintf(d,sizeof d,"tiling +13600=%08llx +11068=%08llx +48=%08llx +13920=%08llx +13940=%08llx +13944=%08llx +15332=%08llx",
            (unsigned long long)be(Device+13600),(unsigned long long)be(Device+11068),(unsigned long long)be(Device+48),
            (unsigned long long)be(Device+13920),(unsigned long long)be(Device+13940),(unsigned long long)be(Device+13944),
            (unsigned long long)be(Device+15332));
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DEVICE %s",d);
        n=std::snprintf(d,sizeof d,"slots");
        for(unsigned k=0;k<6 && n>0 && n<(int)sizeof d-60;++k){const auto s=Device+11588+k*16;const auto ptr=be(s+16);
            n+=std::snprintf(d+n,sizeof d-n," [%u m4=%08llx m0=%08llx p=%08llx *p=%08llx]",k,(unsigned long long)be(s+4),(unsigned long long)be(s),(unsigned long long)ptr,
                (unsigned long long)(ptr>0?be(std::uint32_t(ptr)):-1LL));}
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DEVICE %s",d);
    }
}
}
// ===== end DIAGNOSTIC SKIP helper =====

namespace {
std::atomic<recomp::bootstrap::GuestImportRegistry*> nativeDiscRegistry{nullptr};
std::uint8_t* nativeDiscBase = nullptr;
struct NativeDiscBridgeScope {
    NativeDiscBridgeScope(recomp::bootstrap::GuestImportRegistry* registry, std::uint8_t* base) {
        nativeDiscBase = base;
        nativeDiscRegistry.store(registry, std::memory_order_release);
    }
    ~NativeDiscBridgeScope() {
        nativeDiscRegistry.store(nullptr, std::memory_order_release);
        nativeDiscBase = nullptr; // Executor has joined every guest writer.
    }
};
}

// Native implementation of the verified SDK handle-to-event helper. Keep its
// surrounding critical section, registered callback and disc API in original
// code. No emulated ExEventObjectType/OBJECT_TYPE structure is needed here.
extern "C" void __imp__sub_8364D680(PPCContext& ctx, std::uint8_t* base);
void sub_8364D680(PPCContext& ctx, std::uint8_t* base) {
    auto* registry = nativeDiscRegistry.load(std::memory_order_acquire);
    if (!registry) { __imp__sub_8364D680(ctx, base); return; }
    const auto event = base == nativeDiscBase
        ? registry->referenceNativeEvent(ctx.r3.u32) : std::nullopt;
    if (!event) {
        recomp::bootstrap::GuestExecutionGuard::captureImportArguments({ctx.r1.u64, {{
            ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
            ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64}}});
        recomp::bootstrap::GuestExecutionGuard::stopForRejectedImport("__imp__ObReferenceObjectByHandle");
    }
    ctx.r3.u64 = *event;
}

namespace {

constexpr std::string_view ProtocolHeader{"HOST_GUEST_TRACE_V1"};
constexpr std::uint64_t MaximumStagedXexHeaderSize = 16U * 1024U * 1024U;
constexpr std::uint64_t HeaderReadChunkSize = 1024U * 1024U;
constexpr std::uint64_t PageSize = 0x1000U;
constexpr std::uint64_t ModuleStructureSize = 0x64U;

struct NumericRange {
    std::uint64_t base;
    std::uint64_t size;
};

class NativeReadOnlyFile final : public recomp::bootstrap::GuestReadOnlyFile {
public:
    NativeReadOnlyFile(int descriptor, std::uint64_t size) : descriptor_(descriptor), size_(size) {}
    ~NativeReadOnlyFile() override { close(descriptor_); }
    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }
    [[nodiscard]] bool read(std::uint64_t offset, std::uint8_t* destination, std::size_t bytes) const noexcept override {
        static_assert(sizeof(off_t) >= 8);
        if (offset > size_ || bytes > size_ - offset || bytes > 1024 * 1024 || (bytes && !destination)) { return false; }
        struct stat info {};
        if (fstat(descriptor_, &info) != 0 || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != size_) { return false; }
        std::size_t done = 0;
        while (done < bytes) {
            const auto count = pread(descriptor_, destination + done, bytes - done, static_cast<off_t>(offset + done));
            if (count < 0 && errno == EINTR) { continue; }
            if (count <= 0) { return false; }
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
private:
    int descriptor_;
    std::uint64_t size_;
};

class ScopedFile final {
public:
    explicit ScopedFile(int descriptor) noexcept : descriptor_(descriptor) {}
    ~ScopedFile() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] int release() noexcept { const auto descriptor = descriptor_; descriptor_ = -1; return descriptor; }

private:
    int descriptor_{-1};
};

bool parseUnsigned(std::string_view text, std::uint64_t& value) {
    value = 0;
    const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), value, 10);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parseArguments(
        int argc,
        char** argv,
        std::array<std::uint64_t, 9>& values,
        bool& hasPhysicalArena,
        std::array<std::uint64_t, 3>& physicalValues,
        bool& hasTitleEnvironment,
        std::array<std::uint64_t, 8>& titleValues,
        std::size_t& rawXexArgumentIndex,
        bool& hasThreadEnvironment,
        std::array<std::uint64_t, 7>& threadValues,
        bool& hasThreadPool,
        std::array<std::uint64_t, 2>& threadPoolValues,
        bool& hasThreadType,
        std::array<std::uint64_t, 2>& threadTypeValues) {
    int baseArgumentCount = argc;
    hasThreadType = argc >= 3 && std::string_view(argv[argc - 3]) == "--thread-object-type";
    if (hasThreadType) {
        baseArgumentCount -= 3;
        for (std::size_t i = 0; i < threadTypeValues.size(); ++i) {
            if (!parseUnsigned(argv[baseArgumentCount + 1 + i], threadTypeValues[i])
                    || threadTypeValues[i] > UINT32_MAX) { return false; }
        }
    }
    hasThreadPool = baseArgumentCount >= 3
            && std::string_view(argv[baseArgumentCount - 3]) == "--thread-pool";
    if (hasThreadPool) {
        baseArgumentCount -= 3;
        for (std::size_t i = 0; i < threadPoolValues.size(); ++i) {
            if (!parseUnsigned(argv[baseArgumentCount + 1 + i], threadPoolValues[i])
                    || threadPoolValues[i] > UINT32_MAX) {
                return false;
            }
        }
    }
    hasThreadEnvironment = baseArgumentCount == 28 || baseArgumentCount == 31;
    if ((hasThreadPool || hasThreadType) && !hasThreadEnvironment) { return false; }
    if (hasThreadEnvironment) {
        baseArgumentCount -= 8;
        if (std::string_view(argv[baseArgumentCount]) != "--thread-environment") {
            return false;
        }
        for (std::size_t index = 0; index < threadValues.size(); ++index) {
            if (!parseUnsigned(argv[baseArgumentCount + 1 + index], threadValues[index])
                    || threadValues[index] > UINT32_MAX) {
                return false;
            }
        }
    }
    if (baseArgumentCount != 11 && baseArgumentCount != 14
            && baseArgumentCount != 20 && baseArgumentCount != 23) {
        return false;
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!parseUnsigned(argv[index + 2], values[index])) {
            return false;
        }
    }
    hasPhysicalArena = baseArgumentCount == 14 || baseArgumentCount == 23;
    if (hasPhysicalArena) {
        for (std::size_t index = 0; index < physicalValues.size(); ++index) {
            if (!parseUnsigned(argv[index + 11], physicalValues[index])) {
                return false;
            }
        }
    }
    hasTitleEnvironment = baseArgumentCount == 20 || baseArgumentCount == 23;
    rawXexArgumentIndex = hasPhysicalArena ? 14 : 11;
    if (hasTitleEnvironment) {
        for (std::size_t index = 0; index < titleValues.size(); ++index) {
            if (!parseUnsigned(
                        argv[index + rawXexArgumentIndex + 1],
                        titleValues[index])) {
                return false;
            }
        }
    }
    return true;
}

bool fitsGuestRange(std::uint64_t address, std::uint64_t size) {
    constexpr std::uint64_t GuestAddressSpaceSize = 1ULL << 32U;
    return address <= std::numeric_limits<std::uint32_t>::max()
            && size <= GuestAddressSpaceSize
            && address <= GuestAddressSpaceSize - size;
}

bool rangesOverlap(const NumericRange& left, const NumericRange& right) {
    return left.base < right.base + right.size
            && right.base < left.base + left.size;
}

bool titleLayoutIsValid(
        const std::array<std::uint64_t, 9>& values,
        const std::array<std::uint64_t, 8>& titleValues) {
    const NumericRange fullImage{values[0], titleValues[1]};
    const NumericRange heap{values[2], values[3]};
    const NumericRange stack{values[4], values[5]};
    const NumericRange header{titleValues[2], titleValues[3]};
    const NumericRange exportedVariable{titleValues[5], 4};
    const NumericRange module{titleValues[6], ModuleStructureSize};
    const std::array ranges{fullImage, heap, stack, header, exportedVariable, module};
    for (const auto& range : ranges) {
        if (range.size == 0 || !fitsGuestRange(range.base, range.size)) {
            return false;
        }
    }
    for (std::size_t left = 0; left < ranges.size(); ++left) {
        for (std::size_t right = left + 1; right < ranges.size(); ++right) {
            if (rangesOverlap(ranges[left], ranges[right])) {
                return false;
            }
        }
    }
    const auto decodedImageEnd = values[0] + values[1];
    const auto fullImageEnd = fullImage.base + fullImage.size;
    return values[1] <= fullImage.size
            && titleValues[2] != 0
            && titleValues[5] != 0
            && titleValues[6] != 0
            && (titleValues[2] & 3U) == 0
            && (titleValues[5] & 3U) == 0
            && (titleValues[6] & 3U) == 0
            && titleValues[4] >= values[0]
            && titleValues[4] + 4 <= decodedImageEnd
            && titleValues[7] >= fullImage.base
            && titleValues[7] < fullImageEnd;
}

bool stageRawXexHeader(
        recomp::bootstrap::GuestAddressSpace& addressSpace,
        const char* rawXexPath,
        std::uint64_t expectedFileSize,
        std::uint32_t headerAddress,
        std::uint64_t headerSize) {
    if (rawXexPath == nullptr
            || !std::filesystem::path(rawXexPath).is_absolute()
            || headerSize < 24
            || headerSize > MaximumStagedXexHeaderSize
            || !fitsGuestRange(headerAddress, headerSize)) {
        return false;
    }
    int flags = O_RDONLY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    ScopedFile file(open(rawXexPath, flags));
    if (file.get() < 0) {
        return false;
    }
    struct stat information {};
    if (fstat(file.get(), &information) != 0
            || !S_ISREG(information.st_mode)
            || information.st_size < 0
            || static_cast<std::uint64_t>(information.st_size) != expectedFileSize
            || headerSize > expectedFileSize) {
        return false;
    }

    auto* destination = addressSpace.translate(headerAddress);
    std::uint64_t bytesRead = 0;
    while (bytesRead < headerSize) {
        const auto remaining = headerSize - bytesRead;
        const auto request = static_cast<std::size_t>(
                std::min(remaining, HeaderReadChunkSize));
        const auto result = read(file.get(), destination + bytesRead, request);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            std::memset(destination, 0, static_cast<std::size_t>(bytesRead));
            return false;
        }
        bytesRead += static_cast<std::uint64_t>(result);
    }
    const auto declaredHeaderSize =
            (static_cast<std::uint32_t>(destination[8]) << 24U)
            | (static_cast<std::uint32_t>(destination[9]) << 16U)
            | (static_cast<std::uint32_t>(destination[10]) << 8U)
            | static_cast<std::uint32_t>(destination[11]);
    if (destination[0] != 'X'
            || destination[1] != 'E'
            || destination[2] != 'X'
            || destination[3] != '2'
            || declaredHeaderSize != headerSize
            || fstat(file.get(), &information) != 0
            || information.st_size < 0
            || static_cast<std::uint64_t>(information.st_size) != expectedFileSize) {
        std::memset(destination, 0, static_cast<std::size_t>(headerSize));
        return false;
    }
    return true;
}

bool threadInputsMatchHeader(
        const recomp::bootstrap::GuestAddressSpace& addressSpace,
        std::uint32_t headerAddress,
        std::uint64_t headerSize,
        const std::array<std::uint64_t, 7>& threadValues) {
    const auto lookup = recomp::bootstrap::GuestXexHeader::lookup(
            addressSpace, headerAddress, 0x00020104U);
    if (!lookup.found()
            || lookup.kind != recomp::bootstrap::GuestXexHeaderValueKind::DataPointer
            || lookup.value < headerAddress
            || static_cast<std::uint64_t>(lookup.value) + 16
                    > static_cast<std::uint64_t>(headerAddress) + headerSize) {
        return false;
    }
    const auto* data = addressSpace.translate(lookup.value);
    const auto word = [&](std::size_t offset) -> std::uint32_t {
        return (static_cast<std::uint32_t>(data[offset]) << 24U)
                | (static_cast<std::uint32_t>(data[offset + 1]) << 16U)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 8U)
                | static_cast<std::uint32_t>(data[offset + 3]);
    };
    return word(0) == threadValues[6] && word(4) == threadValues[3]
            && word(8) == threadValues[5] && word(12) == threadValues[4];
}

void addSaturating(std::uint64_t& target, std::size_t value) {
    const auto converted = static_cast<std::uint64_t>(value);
    if (converted > std::numeric_limits<std::uint64_t>::max() - target) {
        target = std::numeric_limits<std::uint64_t>::max();
    } else {
        target += converted;
    }
}

} // namespace
// script-mods (iPad 237, 2026-10-25; local-analysis/trainer-mods v952): files/mods/ overrides of archive entries (settings/mods.txt scripts=on).
#include "mods_glue.inc"

// Attempt v21: core placement on the Thor (Snapdragon 8 Gen 2: cpu0-2 A510, cpu3-4 A715, cpu5-6
// A710, cpu7 X3). Android's scheduler kept the two busiest threads together on cpu7 (v12 sample:
// 7/10 and 8/10 samples). Every 2 s, rank this process's threads by CPU time in the window: the
// busiest gets cpu7 alone, the next four get cpu3-6, every other thread may use cpu0-6 (never
// forced onto the little cores: the audio callback is light but latency-sensitive). Placement only.
#include <sched.h>
#include <sys/resource.h> // v237 setpriority
#include <dirent.h>
#include <sys/syscall.h>
namespace {
// Attempt v34: the D3D render thread (token 6408e000) owns the X3 prime core by role. v33 ranked by
// the last 2 s window only: whenever the render thread waited more for a moment, the audio thread
// took cpu7 and the render thread ran ~40% slower on an A715/A710 (frames 111 -> 145 ms).
std::atomic<int> primeCoreTid{0};
extern "C" { std::atomic<int> recomp_v336_nice20_applied{0}; } // v336 (read by the renderer's A/B line)
}
extern "C" std::atomic<bool> recomp_v279_worker_isr; std::atomic<bool> recomp_v279_worker_isr{false}; // v279 (set by the renderer's A/B windows)
static std::atomic<unsigned long long> v279Inline{0},v279Worker{0}; // v279: scene IRQs run inline / through the worker handoff
extern "C" std::atomic<bool> recomp_v282_baseline; std::atomic<bool> recomp_v282_baseline{false}; // v282 (set by the renderer's A/B windows)
static std::atomic<unsigned long long> v282Concurrent{0},v282Declined{0},v282Busy{0}; // v282: concurrent deferred IRQs, declines, busy retries
extern "C" std::atomic<bool> recomp_v284_baseline; std::atomic<bool> recomp_v284_baseline{false}; // v284 (set by the renderer's A/B windows)
static std::atomic<unsigned long long> v284Concurrent{0},v284Declined{0},v284GateRefused{0}; // v284: concurrent tail IRQs
// v280 (goal step 5): the game's frame limiter. sub_824D52F8 (called from the D3D command loop sub_823B5BC8) sleeps until
// `limit` / 60 s have passed since the previous frame's timebase stamp; `limit` is the word at 0x837CA13C (2 in the title
// image: a 33.3 ms minimum frame, the 30 fps cap; 1 = 60 fps; <= 0 skips the limiter). With diag-v280-limit60 the renderer's
// A/B window calls this with on = true (limit 1), else false (limit 2). Only a word holding 1 or 2 is changed (anything
// else is left and reported), so a value the game itself set elsewhere is never overwritten.
extern "C" std::uint32_t recomp_apple_launcher_display_options() noexcept; // (launcher bridge: the saved choices word)
namespace {iosguestlimit::Policy guestFrameLimitPolicy() {
    // ios-defaults (Claude, 2026-10-16): without RECOMP_IOS_GUEST_FRAME_LIMIT the game's own limiter follows the launcher's cap:
    // Off / 60 / 40 -> 60 (limit 1), 30 / 24 / Auto or no saved choice -> 30 (limit 2, the title image's own value).
    // (ios-sidemenu, SPEC §12: the environment once; the launcher's word every call, so the in-game menu's live cap and frame generation
    // reach the game's own limiter at the renderer's next switch window - one atomic load)
    static const auto env=[]{
        if(const char* e=std::getenv("RECOMP_IOS_GUEST_FRAME_LIMIT")) {
            // (Claude) "inherit": the renderer's diag-v280-limit60/-limit30 switches decide (Android's v307 default 60), so an
            // in-run A/B (RECOMP_IOS_SWITCHES_AB=diag-v280-limit30;P) can compare the two limits on the same scene.
            if(std::string_view(e)=="inherit")return std::optional(iosguestlimit::Policy::Inherit);
            return std::optional(iosguestlimit::parse(e));
        }
        return std::optional<iosguestlimit::Policy>();
    }();
    if(env)return *env;
    const std::uint32_t w=recomp_apple_launcher_display_options();
    const unsigned cap=(w&0x80000000u)?(w&7u):3u;
    if((w&0x80000000u) && (w&0x8000u))return iosguestlimit::Policy::Thirty; // (Claude, 2026-10-18) Frame Generation On: the 30 fps game
    return (cap==0 || cap==1 || cap==2)?iosguestlimit::Policy::Sixty:iosguestlimit::Policy::Thirty;
}}
extern "C" unsigned recomp_ios_guest_frame_limit_mode() {return static_cast<unsigned>(guestFrameLimitPolicy());}
static std::uint8_t* v280GuestBase=nullptr;static std::atomic<unsigned> v280Last{0},v280Applied{0},v280Foreign{0};
#if defined(RECOMP_TOUCH_CONTEXT)
// qol-touch (v702, touch-controls-spec SPEC.md 1.3) / iPad 235: the player's control context for the universal touch controls
// (touch_context.h). The iOS app calls it directly (one binary) every 100 ms on its main thread. Read-only guest loads.
// Bit 15: the pause full map is on screen (map_tap.h mapShown, RECOMP_TOUCH_MAPTAP).
extern "C" __attribute__((visibility("default"))) int recomp_touch_context_v1(void) {
    if(!v280GuestBase)return -1;
    int w=recomp::touchctx::word(recomp::touchctx::GuestMemory{v280GuestBase});
#if defined(RECOMP_TOUCH_MAPTAP)
    if(recomp::gpu::maptap::machine().mapShown())w|=0x8000;
    if(recomp::gpu::maptap::machine().watching())w|=1<<30; // (v711, SPEC 3.10: a map-tap episode runs - opening or closing goes through the
                                                            //  pause menu for ~0.8 s; the owner saw the menu set flash: the app keeps MAP)
#endif
    return w;
}
#endif /* RECOMP_TOUCH_CONTEXT */
extern "C" void recomp_v280_apply_limiter(bool on) {
    if(!v280GuestBase)return;
    auto* p=v280GuestBase+0x837CA13Cu;
    const auto result=iosguestlimit::applyWord(p,on);
    v280Last.store(result.before,std::memory_order_relaxed);
    if(result.foreign)v280Foreign.fetch_add(1,std::memory_order_relaxed);
    if(result.changed)v280Applied.fetch_add(1,std::memory_order_relaxed);
    if(guestFrameLimitPolicy()!=iosguestlimit::Policy::Inherit)
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_IOS_GUEST_FRAME_LIMIT requested=%u desired_word=%u before_word=%u actual_word=%u changed=%u foreign=%u",
            recomp_ios_guest_frame_limit_mode(),result.wanted,result.before,result.after,unsigned(result.changed),unsigned(result.foreign));
}
// pace-caps (lane A, pace_caps.h; called by renderer_boundary_trace.cpp paceHold on the render thread only): a paced hold is the
// delay the game's own Sleep takes, in the two steps of the registry's delay handler (v211). recomp_pace_park_ready: a pending
// deferred interrupt is delivered first (nothing else delivers it while the render thread is parked); a step of its own so that
// the caller lends the prime core to main only after the handler has run (pace_caps.h PrimeLend). recomp_pace_park_until: the
// executor parks the task until the deadline (permit released, condition-variable wait, no spin). False: not parked (no executor
// yet, another thread, or the executor refused: a stop, an active scope or handoff); the caller then does not wait at all.
// The park throws the executor's cancellation like every wait.
extern "C" bool recomp_pace_park_ready() {
    if(!diagnostic_skip::executor || recomp::bootstrap::currentGuestThreadToken()!=0x6408e000u)return false;
    if(deferredIrqDeliver)(void)deferredIrqDeliver(1);
    return true;
}
extern "C" bool recomp_pace_park_until(long long steadyNs) {
    if(!diagnostic_skip::executor || recomp::bootstrap::currentGuestThreadToken()!=0x6408e000u)return false;
    return diagnostic_skip::executor->delayUntil(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(steadyNs)));
}
extern "C" std::atomic<int> recomp_place_scene_big_applied; // v217 (read by the renderer's A/B line)
std::atomic<int> recomp_place_scene_big_applied{0};
extern "C" std::atomic<int> recomp_place_v235_applied; // v235 (read by the renderer's A/B line: v235_place)
std::atomic<int> recomp_place_v235_applied{0};
extern "C" std::atomic<int> recomp_v237_nice_applied; // v237 (read by the renderer's A/B line: v237_nice, the applied nice)
std::atomic<int> recomp_v237_nice_applied{0};
extern "C" std::atomic<int> recomp_v244_prime_share_applied; // v244 (1 = main/scene workers 0-1 allowed on cpu7 at both ends)
std::atomic<int> recomp_v244_prime_share_applied{0};
extern "C" std::atomic<int> recomp_v238_nice_applied; // v238 (the nice applied to main and scene workers 0-1; 1 = not yet)
std::atomic<int> recomp_v238_nice_applied{1};
extern "C" std::atomic<int> recomp_v246_main_nice_applied; // v246 (1 = main at nice -8 at both ends; switch diag-v246-main-nice8)
std::atomic<int> recomp_v246_main_nice_applied{0};
extern "C" std::atomic<int> recomp_v246_present_little_applied; // v246 (1 = the presenter on {0-2}; switch diag-v246-present-little)
std::atomic<int> recomp_v246_present_little_applied{0};
extern "C" std::atomic<int> recomp_v249_jobs_nice_applied; // v249 (1 = job workers + scene workers 2-3 at nice -4)
std::atomic<int> recomp_v249_jobs_nice_applied{0};
// v250: audio health (min AAudio headroom in frames at each packet demand, starved demands, packets) for the renderer's
// A/B line, and the placement switch diag-v250-synth-little (guest thread 64249000 on {0-2}).
extern "C" std::atomic<long long> recomp_v250_audio_min; std::atomic<long long> recomp_v250_audio_min{1LL<<40};
extern "C" std::atomic<unsigned long long> recomp_v250_audio_starved; std::atomic<unsigned long long> recomp_v250_audio_starved{0};
extern "C" std::atomic<unsigned long long> recomp_v250_audio_packets; std::atomic<unsigned long long> recomp_v250_audio_packets{0};
extern "C" std::atomic<int> recomp_v250_synth_little_applied; std::atomic<int> recomp_v250_synth_little_applied{0};
extern "C" std::atomic<int> recomp_v346_audio_little_applied; std::atomic<int> recomp_v346_audio_little_applied{0}; // v346
extern "C" std::atomic<int> recomp_v272_jobs_little_applied; std::atomic<int> recomp_v272_jobs_little_applied{0}; // v272
std::atomic<int> v250SynthTid{0};
std::atomic<int> v346AudioTid{0}; // v346: guest thread 64141000 (the audio engine's update; ~0.48 big core in the house)
// drive-path (dp_jobs_big.h): diag-dp-jobs-big, v196's job-worker pinning as a runtime switch (re-read every 2 s); its applied
// state is reported on RENDER_GENERIC_V210Y dp_jobs_big=.
#include "dp_jobs_big.h"
extern "C" std::atomic<int> recomp_dp_jobs_big_applied; std::atomic<int> recomp_dp_jobs_big_applied{0};
namespace {
// v196 (H17, opt-in files/private-trace/diag-place-jobs-big): the RAGE job workers the main thread waits on (v195
// timeline: 640f5000, 64108000, 6411b000, 6412e000 signal its job-completion objects) are kept on the big cores {3-6}
// every placement tick instead of {0-6} (they are light on average, so the ranking left them the little cores).
constexpr std::array<std::uint32_t,4> JobWorkerTokens{0x640f5000u,0x64108000u,0x6411b000u,0x6412e000u};
std::array<std::atomic<int>,4> jobWorkerTids{};
bool placeJobsBigAtBoot() {
    static const bool on=::access(RECOMP_APP_PATH("/private-trace/diag-place-jobs-big"),F_OK)==0;
    return on;
}
// drive-path: the boot switch (v196) or the runtime switch diag-dp-jobs-big (dp_jobs_big.h).
bool placeJobsBig() { return placeJobsBigAtBoot() || recomp_dp::jobsBig().on.load(std::memory_order_relaxed); }
// v202 (H18, opt-in files/private-trace/diag-place-scene-big): the four workers the main thread's Sleep(0) loops wait
// on (v201 timeline_poll_phases: active 80/53/35/27% of main's poll time; ~30% of their samples on cpu0-2) are kept on
// the big cores {3-6} every placement tick instead of {0-6}.
constexpr std::array<std::uint32_t,4> SceneWorkerTokens{0x64026000u,0x64039000u,0x6404c000u,0x6405f000u};
std::array<std::atomic<int>,4> sceneWorkerTids{};
// v217: re-read by the placement thread every 2 s (sceneBigOn), so an in-run A/B can flip it; its applied state is
// reported to the renderer (recomp_place_scene_big_applied: RENDER_GENERIC_V210 scene_big). Switching it off returns
// the four workers to the ranking ({0-6} unless heavy).
std::atomic<bool> sceneBigOn{false};
bool placeSceneBig() { return sceneBigOn.load(std::memory_order_relaxed); }
// v235 (A/B: files/private-trace/diag-v235-place, re-read every 2 s by the placement thread; applied state reported as
// RENDER_GENERIC_V210 v235_place): the big cores {3-6} for the threads the frame waits on and the heavy ones it needs, the
// little cores {0-2} for every other thread. v232 house: system-wide busy cpu0-2 30-35%, cpu3-6 89-90%; main's poll
// (8.2 ms/frame outdoors) waits on scene workers 64026000/64039000, which wait for a CPU as long as they run. Big: main,
// audio, the tile thread (its IRQ workers inherit its mask), scene workers 0-1, the renderer thread (recomp-render-w) and the
// presenter (recomp-present). v217's scene-worker pinning added threads to the big cores; this also removes the rest.
constexpr std::array<std::uint32_t,3> V235BigTokens{0x63001000u,0x64141000u,0x640b1000u};
std::array<std::atomic<int>,3> v235BigTids{};
std::atomic<bool> v235On{false};
bool v235Big(pid_t tid,const char* comm) {
    for(const auto& t:v235BigTids)if(t.load(std::memory_order_relaxed)==tid)return true;
    for(unsigned i=0;i<2;++i)if(sceneWorkerTids[i].load(std::memory_order_relaxed)==tid)return true;
    return comm && (!std::strcmp(comm,"recomp-render-w") || !std::strcmp(comm,"recomp-present"));
}
bool pinnedBig(pid_t tid) {
    const auto in=[&](const auto& tids){return std::any_of(tids.begin(),tids.end(),[&](const auto& t){return t.load(std::memory_order_relaxed)==tid;});};
    return (placeJobsBig() && in(jobWorkerTids)) || (placeSceneBig() && in(sceneWorkerTids));
}
}
// v203 (H19, opt-in files/private-trace/diag-prime-main): the main thread (63001000), the head of the house frame's
// dependency chain (v195/v202 timelines: the render thread parks ~12 ms/frame on main's signals), owns cpu7 instead of
// the render thread; the render thread joins the ranking like every other thread.
bool primeMain() {
    static const bool on=::access(RECOMP_APP_PATH("/private-trace/diag-prime-main"),F_OK)==0;
    return on;
}
// v224 (A/B: files/private-trace/diag-v224-baseline, applied by the renderer every 60 presents): a new guest task's host
// thread inherits its creator's CPU mask, so the IRQ workers the render thread creates (deferred tile IRQs) start with the
// render thread's cpu7-only mask: the v218 exclusive-site stats show their start ~220 us vs ~88 us for the tile thread's.
// A task that is not the prime-core owner and starts with a cpu7-only mask gets the big cores {3-6} instead (the world
// is stopped while an IRQ worker runs, so the big cores are free); the placement monitor re-ranks every 2 s as before.
extern "C" std::atomic<bool> recomp_v224_baseline;
std::atomic<bool> recomp_v224_baseline{false};
extern "C" __attribute__((weak)) std::atomic<int> recomp_aw_waker_tid; // async-wake (guest_cooperative_executor.cpp): the waker's tid
std::atomic<unsigned long long> v224Unpinned{0};
// v245 (prime_swap_v245.h): main's affinity while the render thread waits for main's kick; the setter and main's tid.
#include "prime_swap_v245.h"
namespace {
#if defined(__APPLE__)
int v245SetAffinity(int tid,unsigned mask) {
    return recomp::apple::unavailableThreadAffinity(tid,mask);
}
#else
int v245SetAffinity(int tid,unsigned mask) {
    cpu_set_t set;CPU_ZERO(&set);
#pragma clang loop unroll(full) vectorize(disable) // (topology lane: 32 cpus, was 8; the hint keeps the generated code one move, as it was)
    for(int c=0;c<32;++c)if(mask>>c&1u)CPU_SET(c,&set);
    return sched_setaffinity(tid,sizeof set,&set)==0?0:(errno?errno:-1);
}
#endif
// main-sched (prime_swap_v245.h boostNice, switch diag-ms-prime-nice): main's nice while it holds cpu7, and the placement
// thread's v238/v246 re-application of main's nice through the same protocol (deferred to the release while boosted).
#if defined(__APPLE__)
int msSetNice(int tid,int nice) { return recomp::apple::unavailableThreadPriority(tid,nice); }
#else
int msSetNice(int tid,int nice) { return ::setpriority(PRIO_PROCESS,tid,nice)==0?0:(errno?errno:-1); }
#endif
#if defined(__APPLE__)
int msGetNice(int tid,int* nice) { return recomp::apple::unavailableThreadPriorityQuery(tid,nice); }
#else
int msGetNice(int tid,int* nice) { errno=0;const int v=::getpriority(PRIO_PROCESS,tid);if(v==-1 && errno)return errno;*nice=v;return 0; }
#endif
[[maybe_unused]] const bool v245Init=(recomp_v245::state().set=v245SetAffinity,recomp_v245::state().setNice=msSetNice,
                                      recomp_v245::state().getNice=msGetNice,true);
// topology lane: the library's one start-up step - the roles are detected (cpu_topology.h reads the kernel's description),
// then main's and the v251 extras' masks before the ranking first places them = the mid role ({3-6} on the Thor: the literal
// 0x78 PrimeSwap's members had). Runs while the library loads (out of line: one call in the initialiser).
[[gnu::noinline]] void topologySwapMasks() noexcept {
    recomp_topology::ensureDetected();
    auto& swap=recomp_v245::state();const unsigned mid=recomp_topology::roles().mid.mask;
    swap.mainMask.store(mid,std::memory_order_relaxed);
    for(auto& extra:swap.extraMask)extra.store(mid,std::memory_order_relaxed);
}
[[maybe_unused]] const bool topologyInit=(topologySwapMasks(),true);
// topology lane: a set's cpus 0-31 as a mask - what the ranking stores as main's / the v251 extras' mask to restore. It
// replaces every call of v458's maskBits (below: cpus 0-7 only, no longer called; its line is left untouched so that another
// lane's insertion next to it merges without a conflict - delete it once the lanes are merged).
#if defined(__APPLE__)
#else
unsigned maskBits32(const cpu_set_t& set){
    unsigned m=0;
#pragma clang loop unroll(full) vectorize(disable)
    for(int c=0;c<32;++c)if(CPU_ISSET(c,&set))m|=1u<<c;
    return m;
}
#endif
// main-waits-2 (mw2_producers.h): the producers' nice setter (the placement thread's v249 block goes through applyBase).
[[maybe_unused]] const bool mw2Init=(recomp_mw2::producers().set=msSetNice,true);
// prime-exchange (prime_swap_v245.h pxPlacementPin; diag-px-*): the placement thread's 250 ms pin of the prime owner to {7}.
// With the px switches absent (and the render thread not lent) this is v458's call, unlocked, with v458's arguments. While px is
// on the pin is made under the swap protocol's lock (same setter, same mask), and skipped while the render thread is lent to
// {3-6} - it must not return to {7} while main holds it; the render thread's own exchange-back restores {7}. Result as
// sched_setaffinity's: 0 = on cpu7 (or lent on purpose), else the caller's v36 fallback to {3-6} runs as before.
// Out of line: the placement lambda keeps its control flow (one call replaced by one call). Deliberately NOT declared noexcept:
// the call it replaces (sched_setaffinity, a plain C declaration) is an invoke inside the lambda, which has locals to destroy;
// a noexcept helper turned it into a call and changed the lambda's profile hash (measured with -Wbackend-plugin: "up to 1029674
// count discarded"). Nothing in here throws.
#if defined(__APPLE__)
#else
[[gnu::noinline]] int pxPinPrime(pid_t prime,const cpu_set_t* x3) {
    const int r=recomp_v245::pxPlacementPin(recomp_v245::state(),static_cast<int>(prime));
    return r<0?sched_setaffinity(prime,sizeof *x3,x3):(r==0?0:-1);
}
#endif
// prime-exchange: the render thread's host tid (not with diag-prime-main, where main owns cpu7: px stays inert).
[[gnu::noinline]] void pxTaskStarted(std::uint32_t token,int tid) noexcept {
    if(token==recomp_v245::RenderToken && !primeMain())recomp_px::state().renderTid.store(tid,std::memory_order_relaxed);
}
#if defined(__APPLE__)
#else
unsigned maskBits(const cpu_set_t& set){unsigned m=0;for(int c=0;c<8;++c)if(CPU_ISSET(c,&set))m|=1u<<c;return m;}
#endif
}
extern "C" void recomp_diag_task_host_started(std::uint32_t token,int tid) {
    pxTaskStarted(token,tid); // prime-exchange (out of line)
    if(token==recomp_v245::MainToken)recomp_v245::state().mainTid.store(tid,std::memory_order_relaxed); // v245
    // (v226: the v224 unpinning is removed: its A/B was 0.4-0.5 ms slower and the IRQ-worker start did not change.)
    if(token==(primeMain()?0x63001000u:0x6408e000u))primeCoreTid.store(tid,std::memory_order_relaxed);
    for(std::size_t i=0;i<JobWorkerTokens.size();++i)if(token==JobWorkerTokens[i])jobWorkerTids[i].store(tid,std::memory_order_relaxed); // v196
    for(std::size_t i=0;i<SceneWorkerTokens.size();++i)if(token==SceneWorkerTokens[i])sceneWorkerTids[i].store(tid,std::memory_order_relaxed); // v202
    for(std::size_t i=0;i<2;++i)if(token==SceneWorkerTokens[i])recomp_v245::state().extraTid[i].store(tid,std::memory_order_relaxed); // v251
    for(std::size_t i=0;i<V235BigTokens.size();++i)if(token==V235BigTokens[i])v235BigTids[i].store(tid,std::memory_order_relaxed); // v235
    if(token==0x64249000u)v250SynthTid.store(tid,std::memory_order_relaxed); // v250
    if(token==0x64141000u)v346AudioTid.store(tid,std::memory_order_relaxed); // v346
}
namespace {
// topology lane: the start-up line RENDER_DIAG_TOPOLOGY (clusters, role masks, thor_masks=1 when they are the literals every
// build up to v458 had). Out of line and noexcept: one plain call in the placement thread's first block.
// sets= / set_bits= / sets_ok= (review fix): thor_masks is computed from the roles' masks, but the sites hand the kernel a
// cpu_set_t. So every role's set is built HERE with the sites' own two builders - prime with cpuSet() (the owner's pin,
// tier 0), the other four with RECOMP_TOPOLOGY_ADD_CPUS (every other site) - and read back: maskBits32() for cpus 0-31,
// setBitCount() for every bit of the object the kernel is handed.
#if defined(__APPLE__)
void startCorePlacement() {
    // No Linux core placement monitor on Apple. Do not advertise applied masks
    // or translate Mach thread names into process IDs for setpriority().
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","APPLE_HOST_SCHEDULER os_default=1 affinity=unsupported remote_nice=unsupported");
}
#else
[[gnu::noinline]] void logTopology() noexcept {
    const recomp_topology::Topology& tp=recomp_topology::roles();
    char line[1536];const int used=recomp_topology::formatLine(tp,line,sizeof line);
    cpu_set_t set[5];set[0]=recomp_topology::cpuSet(tp.prime.mask);
    for(int i=1;i<5;++i)CPU_ZERO(&set[i]);
    RECOMP_TOPOLOGY_ADD_CPUS(set[1],tp.mid);RECOMP_TOPOLOGY_ADD_CPUS(set[2],tp.little);
    RECOMP_TOPOLOGY_ADD_CPUS(set[3],tp.allButPrime);RECOMP_TOPOLOGY_ADD_CPUS(set[4],tp.midPrime);
    recomp_topology::SetReadback back;
    for(int i=0;i<5;++i){back.bits[i]=maskBits32(set[i]);back.count[i]=recomp_topology::setBitCount(set[i]);}
    recomp_topology::formatSets(tp,back,line+used,sizeof line-static_cast<std::size_t>(used));
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","%s",line);
}
void startCorePlacement() {
    static std::atomic<bool> started{false};
    if(started.exchange(true))return;
    std::thread([] {
        const pid_t self=static_cast<pid_t>(syscall(SYS_gettid));
        // topology lane: every mask below is a discovered role (cpu_topology.h). On the Thor: prime {7}, mid {3-6}, little {0-2},
        // allButPrime {0-6}, midPrime {3-7} - the literals this loop had. The comments keep the Thor's numbers as the example.
        const recomp_topology::Topology& tp=recomp_topology::roles();
        logTopology();
        {cpu_set_t little;CPU_ZERO(&little);RECOMP_TOPOLOGY_ADD_CPUS(little,tp.little);sched_setaffinity(self,sizeof little,&little);}
        // Attempt v36. Qualcomm core_ctl pauses cpu7 (prime cluster min_cpus=0; paused after 100 ms
        // below 30% busy) and pauses one big core (min 3 of 4). A thread whose whole mask is paused
        // falls back to the cpuset (0-7). The load that keeps cpu7 unpaused is the render thread
        // pinned on it, so: the render thread (role owner) is pinned to {7} alone and re-pinned every
        // 250 ms; while cpu7 is paused (EINVAL) it runs on {3-6} until the next tick succeeds. No other
        // thread may use cpu7. Every 2 s the rest are ranked: the three busiest on {3-6}, the others on
        // {0-6}. (v35: uclamp.min was refused, EPERM; {3-7} let the scheduler leave cpu7 idle and
        // paused.) sched_getaffinity reports only active CPUs, so masks are re-applied, not compared.
        std::map<pid_t,unsigned long long> previous;std::map<pid_t,int> placed;std::map<pid_t,std::string> comms; // (v235: comms)
        unsigned ticks=0,ownerOnPrime=0,ownerFallback=0;int ownerState=-1;pid_t lastOwner=0;pid_t lastOwner237=0; // (v237)
        for(;;ticks++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const pid_t prime=primeCoreTid.load(std::memory_order_relaxed);
            if(prime) {
                if(prime!=lastOwner){lastOwner=prime;ownerState=-1;}
                cpu_set_t x3=recomp_topology::cpuSet(tp.prime.mask); // (topology lane: the prime seat)
                int state;
                if(pxPinPrime(prime,&x3)==0){state=0;++ownerOnPrime;} // prime-exchange: = sched_setaffinity(prime,sizeof x3,&x3) with px off
                else {
                    cpu_set_t big;CPU_ZERO(&big);RECOMP_TOPOLOGY_ADD_CPUS(big,tp.mid);
                    state=sched_setaffinity(prime,sizeof big,&big)==0?1:2;++ownerFallback;
                }
                { // v237 (A/B: files/private-trace/diag-v237-baseline, read every 2 s): the prime owner at nice -8 (Android's
                  // URGENT_DISPLAY; RLIMIT_NICE 40 allows it). v236 run: the render thread shares cpu7 with other processes
                  // (com.odin.dualscreen.assistant ~half its samples on cpu7, rcuop/kworker threads), all nice 0; its
                  // schedstat wait is ~180 ms/s with ~330 involuntary switches/s. Baseline: nice 0 as before.
                    static bool baseline237=false,nice336=false;static int applied237=1;
                    if(ticks%8==0)baseline237=::access(RECOMP_APP_PATH("/private-trace/diag-v237-baseline"),F_OK)==0;
                    // v336 (A/B: files/private-trace/diag-v336-nice20, read every 2 s): the render thread at nice -20 (the app's RLIMIT_NICE
                    // allows it) instead of -8: its weight against the nice-0 threads that share cpu7 (other processes, kworkers) is
                    // ~14x v237's. The v334 house profile: the render thread is on-CPU ~9.3 ms of its ~17 ms R and waits ~120 ms/s
                    // runnable (schedstat), on the frame's critical path.
                    if(ticks%8==0)nice336=::access(RECOMP_APP_PATH("/private-trace/diag-v336-nice20"),F_OK)==0;
                    ::recomp_v336_nice20_applied.store(nice336?1:0,std::memory_order_relaxed);
                    const int want=baseline237?0:nice336?-20:-8;
                    if(want!=applied237 || prime!=lastOwner237) {
                        if(::setpriority(PRIO_PROCESS,prime,want)==0){applied237=want;lastOwner237=prime;}
                        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V237_NICE tid=%d nice=%d applied=%d errno=%d",
                            prime,want,applied237,applied237==want?0:errno);
                    }
                    ::recomp_v237_nice_applied.store(applied237,std::memory_order_relaxed);
                }
                if(state!=ownerState) {
                    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                        "RENDER_DIAG_PLACEMENT_OWNER tid=%d cores=%s on_prime_ticks=%u fallback_ticks=%u",
                        prime,state==0?"x3":state==1?"big(cpu7 paused)":"unchanged",ownerOnPrime,ownerFallback);
                    ownerState=state;
                }
            }
            if(ticks%8==0) { // v217: the scene-worker switch, every 2 s (before this tick's ranking)
                const bool on=::access(RECOMP_APP_PATH("/private-trace/diag-place-scene-big"),F_OK)==0;
                if(on!=sceneBigOn.load(std::memory_order_relaxed)) {
                    sceneBigOn.store(on,std::memory_order_relaxed);
                    if(!on)for(auto& st:sceneWorkerTids)if(const int tid=st.load(std::memory_order_relaxed))placed.erase(tid); // re-ranked below
                    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PLACE_SCENE_BIG on=%d",int(on));
                }
            }
            if(ticks%8==0) { // drive-path: diag-dp-jobs-big, every 2 s (before this tick's ranking; off -> the ranking re-places them)
                const bool on=::access(RECOMP_APP_PATH("/private-trace/diag-dp-jobs-big"),F_OK)==0;
                static int dpLast=-1;
                (void)recomp_dp::pollJobsBig(recomp_dp::jobsBig(),on,placed,jobWorkerTids);
                if(int(on)!=dpLast){dpLast=int(on);__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_DIAG_DP_JOBS_BIG on=%d boot=%d pins=%llu failures=%llu",int(on),int(placeJobsBigAtBoot()),
                    recomp_dp::jobsBig().pins.load(std::memory_order_relaxed),recomp_dp::jobsBig().failures.load(std::memory_order_relaxed));}
            }
            if(placeJobsBig() || placeSceneBig()) { // v196/v202: every 250 ms, as the prime core owner
                cpu_set_t big;CPU_ZERO(&big);RECOMP_TOPOLOGY_ADD_CPUS(big,tp.mid);
                if(placeJobsBig())recomp_dp::pinJobsBig(recomp_dp::jobsBig(),jobWorkerTids,[](int tid,unsigned mask){ // drive-path (same {3-6})
                    cpu_set_t set;CPU_ZERO(&set);
#pragma clang loop unroll(full) vectorize(disable) // (topology lane: 32 cpus, was 8)
                    for(int c=0;c<32;++c)if(mask>>c&1u)CPU_SET(c,&set);
                    return sched_setaffinity(tid,sizeof set,&set)==0?0:-1;});
                if(placeSceneBig())for(auto& st:sceneWorkerTids)if(const int tid=st.load(std::memory_order_relaxed))(void)sched_setaffinity(tid,sizeof big,&big);
            }
            ::recomp_dp_jobs_big_applied.store(recomp_dp::jobsBig().on.load(std::memory_order_relaxed)?recomp_dp::jobsBig().applied.load(std::memory_order_relaxed):0,
                                               std::memory_order_relaxed); // drive-path (read by the renderer's A/B line)
            ::recomp_place_scene_big_applied.store(placeSceneBig()?1:0,std::memory_order_relaxed); // v217
            if(ticks%8==0) { // v235: the placement switch, every 2 s (a change re-applies every mask below)
                const bool on=::access(RECOMP_APP_PATH("/private-trace/diag-v235-place"),F_OK)==0;
                if(on!=v235On.load(std::memory_order_relaxed)) {
                    v235On.store(on,std::memory_order_relaxed);placed.clear();
                    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V235_PLACE on=%d",int(on));
                }
            }
            ::recomp_place_v235_applied.store(v235On.load(std::memory_order_relaxed)?1:0,std::memory_order_relaxed);
            if(ticks%8==0) { // v238 (A/B: files/private-trace/diag-v238-baseline): main and the two scene workers it polls on at
                // nice -4, above our other SCHED_OTHER threads on the saturated big cores (v232 house: cpu3-6 ~90% busy; main
                // waits ~180 ms/s for a CPU, 64026000/64039000 as long as they run; main's poll 8.2 ms/frame outdoors waits on
                // them). Audio, the renderer thread and the presenter keep nice 0.
                const bool baseline=::access(RECOMP_APP_PATH("/private-trace/diag-v238-baseline"),F_OK)==0;
                const int want=baseline?0:-4;int applied=want;
                const pid_t tids[3]{v235BigTids[0].load(std::memory_order_relaxed),sceneWorkerTids[0].load(std::memory_order_relaxed),
                                    sceneWorkerTids[1].load(std::memory_order_relaxed)};
                // v246 (A/B: files/private-trace/diag-v246-main-nice8): main alone at nice -8 (the render thread's level), the scene
                // workers 0-1 stay at -4. v244-tl outdoor critical path: signal->wake gaps 2.84 ms/frame, main woken by job workers
                // several times a frame and then queued behind nice-0 threads on the saturated big cores (runqueue wait 164 ms/s).
                const bool main8=!baseline && ::access(RECOMP_APP_PATH("/private-trace/diag-v246-main-nice8"),F_OK)==0;
                for(unsigned k=0;k<3;++k){const pid_t t=tids[k];if(!t || (k==0?recomp_v245::state().applyMainNice(t,main8?-8:want) : ::setpriority(PRIO_PROCESS,t,want))!=0)applied=1;} // (main-sched: main's nice via the v245 protocol)
                ::recomp_v246_main_nice_applied.store(main8 && applied==want?1:0,std::memory_order_relaxed);
                // v249 (A/B: files/private-trace/diag-v249-jobs-nice): the RAGE job workers main parks on (640f5000, 64108000,
                // 6411b000, 6412e000: ~1200 wakes/s each, ~65 us slices, ~106 us runqueue wait per wake) and scene workers 2-3
                // at nice -4 too; off = nice 0 as before. v244-tl outdoor critical path: job workers ~2.2 ms + wake gaps 2.84
                // ms/frame.
                {
                    const bool jobs=!baseline && ::access(RECOMP_APP_PATH("/private-trace/diag-v249-jobs-nice"),F_OK)==0;
                    int jobsApplied=jobs?1:0;
                    // main-waits-2: through recomp_mw2::producers().applyBase (slots 0-3 job workers, 4-5 scene workers 2-3): with
                    // the diag-mw2-* switches off it makes exactly this setpriority(t, jobs?-4:0); inside a boosted window it applies
                    // the window's value instead, so it never undoes a boost mid-window.
                    for(unsigned k=0;k<4;++k)if(const int t=jobWorkerTids[k].load(std::memory_order_relaxed)){if(recomp_mw2::producers().applyBase(k,t,jobs?-4:0)!=0)jobsApplied=0;}else jobsApplied=0;
                    for(unsigned k=2;k<4;++k)if(const int t=sceneWorkerTids[k].load(std::memory_order_relaxed)){if(recomp_mw2::producers().applyBase(k+2,t,jobs?-4:0)!=0)jobsApplied=0;}else jobsApplied=0;
                    ::recomp_v249_jobs_nice_applied.store(jobsApplied,std::memory_order_relaxed);
                    static int lastJobs=-1;if(int(jobs)!=lastJobs){lastJobs=int(jobs);__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                        "RENDER_DIAG_V249_JOBS_NICE on=%d applied=%d",int(jobs),jobsApplied);}
                }
                static int lastMain8=-1;if(int(main8)!=lastMain8){lastMain8=int(main8);__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_DIAG_V246_MAIN_NICE main=%d nice=%d applied=%d",tids[0],main8?-8:want,applied);}
                static int last238=1;
                if(applied!=last238)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V238_NICE main=%d scene0=%d scene1=%d nice=%d applied=%d",
                    tids[0],tids[1],tids[2],want,applied);
                last238=applied;::recomp_v238_nice_applied.store(applied,std::memory_order_relaxed);
            }
            if(ticks%8)continue; // the ranking below runs every 2 s
            std::vector<std::pair<unsigned long long,pid_t>> busy;
            if(DIR* dir=opendir("/proc/self/task")) {
                while(const auto* e=readdir(dir)) {
                    const pid_t tid=static_cast<pid_t>(std::atoi(e->d_name));
                    if(tid<=0 || tid==self)continue;
                    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/stat",tid);
                    FILE* f=std::fopen(path,"r");if(!f)continue;
                    char buf[512];const auto n=std::fread(buf,1,sizeof buf-1,f);std::fclose(f);buf[n]=0;
                    const char* p=std::strrchr(buf,')');if(!p)continue;
                    char state=0;unsigned long long ut=0,st=0;
                    if(std::sscanf(p+2,"%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",&state,&ut,&st)!=3)continue;
                    const auto total=ut+st;const auto it=previous.find(tid);
                    busy.push_back({it==previous.end()?0:total-it->second,tid});previous[tid]=total;
                    if(const char* l=std::strchr(buf,'(');l && l<p)comms[tid].assign(l+1,p); // v235
                }
                closedir(dir);
            }
            std::sort(busy.begin(),busy.end(),[](const auto& a,const auto& b){return a.first>b.first;});
            const bool v235=v235On.load(std::memory_order_relaxed);
            const bool v246Little=::access(RECOMP_APP_PATH("/private-trace/diag-v246-present-little"),F_OK)==0; // v246
            const bool v250Little=::access(RECOMP_APP_PATH("/private-trace/diag-v250-synth-little"),F_OK)==0; // v250
            ::recomp_v250_synth_little_applied.store(v250Little?1:0,std::memory_order_relaxed);
            // v346 (A/B: files/private-trace/diag-v346-audio-little, read with the ranking): the audio thread 64141000 on the
            // little cores {0-2}, off the big cores the RAGE job workers queue for (~170 ms/s run-queue wait each).
            const bool v346Little=::access(RECOMP_APP_PATH("/private-trace/diag-v346-audio-little"),F_OK)==0;
            {static bool last=false;if(v346Little!=last){last=v346Little;placed.clear();}} // (re-applies every mask)
            ::recomp_v346_audio_little_applied.store(v346Little?1:0,std::memory_order_relaxed);
            // v272 (A/B: files/private-trace/diag-v272-jobs-little, read with the ranking): the four RAGE job workers (640f5000,
            // 64108000, 6411b000, 6412e000; ~95 ms/s CPU but ~170 ms/s run-queue wait each on the big cores in ~1800 slices/s,
            // v269 outdoor) on the little cores {0-2} (~45% busy), off the big cores main and the scene workers share.
            const bool v272Little=::access(RECOMP_APP_PATH("/private-trace/diag-v272-jobs-little"),F_OK)==0;
            static bool v272Last=false;if(v272Little!=v272Last){v272Last=v272Little;placed.clear();} // (re-applies every mask)
            ::recomp_v272_jobs_little_applied.store(v272Little?1:0,std::memory_order_relaxed);
            ::recomp_v246_present_little_applied.store(v246Little?1:0,std::memory_order_relaxed);
            static bool v244Last=false; // v244: the switch at this ranking; a change re-applies every mask
            const bool v244=::access(RECOMP_APP_PATH("/private-trace/diag-v244-prime-share"),F_OK)==0;
            if(v244!=v244Last){placed.clear();v244Last=v244;__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V244_PRIME_SHARE on=%d",int(v244));}
            ::recomp_v244_prime_share_applied.store(v244?1:0,std::memory_order_relaxed);
            std::size_t rank=0;
            for(std::size_t i=0;i<busy.size();++i) {
                if(prime && busy[i].second==prime)continue; // placed above
                if(pinnedBig(busy[i].second))continue; // v196/v202: placed above
                if(&::recomp_aw_waker_tid && busy[i].second==::recomp_aw_waker_tid.load(std::memory_order_relaxed))continue; // async-wake: keeps its own little-core mask
                // 100 ticks = one core for the whole 2 s window; below 10 (5%) a thread is light.
                const bool heavy=busy[i].first>=10;
                int tier=prime?(heavy && rank<3?1:2):(heavy?(i==0?0:i<5?1:2):2);
                if(v235 && prime){const auto c=comms.find(busy[i].second);tier=v235Big(busy[i].second,c==comms.end()?nullptr:c->second.c_str())?1:3;} // v235
                // v246 (A/B: files/private-trace/diag-v246-present-little, read with the ranking): the presenter (recomp-present,
                // ~0.33 big core, 80% of it the APK's GPU-audited display) on the little cores {0-2} (40-45% busy), off the
                // saturated big cores. Watch RENDER_DIAG_PRESENTER replaced= (a presenter that falls behind drops frames).
                if(v246Little){const auto c=comms.find(busy[i].second);if(c!=comms.end() && c->second=="recomp-present")tier=3;}
                // v250 (A/B: files/private-trace/diag-v250-synth-little): guest thread 64249000 (audio-engine voice code, ~0.26 big
                // core, log2f/exp2f-heavy) on the little cores; judge with v250_audio_starved / v250_audio_min.
                if(v250Little && busy[i].second==v250SynthTid.load(std::memory_order_relaxed))tier=3;
                if(v346Little && busy[i].second==v346AudioTid.load(std::memory_order_relaxed))tier=3; // v346
                if(v272Little)for(const auto& jt:jobWorkerTids)if(busy[i].second==jt.load(std::memory_order_relaxed))tier=3; // v272
                // v244 (A/B: files/private-trace/diag-v244-prime-share, read with the ranking): main and scene workers 0-1 may also
                // run on cpu7 ({3-7}) while the render thread keeps {7} alone at nice -8. v231/v240 timelines: the render thread
                // parks ~10 ms/frame (cpu7 idle meanwhile) while main's poll waits on those scene workers, which queue on the
                // saturated big cores (cpu3-6 ~90% busy).
                if(v244 && prime && (busy[i].second==v235BigTids[0].load(std::memory_order_relaxed) ||
                   busy[i].second==sceneWorkerTids[0].load(std::memory_order_relaxed) || busy[i].second==sceneWorkerTids[1].load(std::memory_order_relaxed)))tier=4;
                if(prime)++rank;
                auto& current=placed[busy[i].second];
                if(current==tier+1)continue; // {3-6} and {0-6} always keep active CPUs: no fallback reset
                cpu_set_t set;CPU_ZERO(&set);
                if(tier==0)set=recomp_topology::cpuSet(tp.prime.mask);else if(tier==1)RECOMP_TOPOLOGY_ADD_CPUS(set,tp.mid);
                else if(tier==3)RECOMP_TOPOLOGY_ADD_CPUS(set,tp.little); // v235: the little cores
                else if(tier==4)RECOMP_TOPOLOGY_ADD_CPUS(set,tp.midPrime); // v244: the big cores and the prime core
                else RECOMP_TOPOLOGY_ADD_CPUS(set,tp.allButPrime);
                if(sched_setaffinity(busy[i].second,sizeof set,&set)==0) {
                    current=tier+1;
                    if(busy[i].second==recomp_v245::state().mainTid.load(std::memory_order_relaxed))
                        recomp_v245::state().mainMask.store(maskBits32(set),std::memory_order_relaxed); // v245: main's mask to restore
                    for(unsigned k=0;k<2;++k)if(busy[i].second==recomp_v245::state().extraTid[k].load(std::memory_order_relaxed))
                        recomp_v245::state().extraMask[k].store(maskBits32(set),std::memory_order_relaxed); // v251
                    if(tier<2)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                        "RENDER_DIAG_PLACEMENT tid=%d tier=%s busy_ticks=%llu rank=%zu",busy[i].second,
                        tier==0?"x3":"big",busy[i].first,i);
                } else if(tier==0) current=0; // cpu7 paused: retry next round
            }
        }
    }).detach();
}
#endif
// Attempt v36: an Android performance hint session (ADPF, API 33, resolved at runtime) for the render
// thread; the present path reports each frame's duration against a 33.3 ms target, so the vendor
// power HAL may raise the thread's capacity (placement and frequency). Diagnostic only.
struct PerformanceHint {
    void* session=nullptr;
    int (*report)(void*,std::int64_t)=nullptr;
    int (*updateTarget)(void*,std::int64_t)=nullptr; // v332 (APerformanceHint_updateTargetWorkDuration, API 33)
    // v333: a second session for main, the job and scene workers, the Vulkan worker and the presenter (API 33 has no setThreads:
    // created when diag-v333-wide-hint appears, closed when it goes), reported the same frame durations.
    void* manager=nullptr;void* (*create)(void*,const std::int32_t*,std::size_t,std::int64_t)=nullptr;void (*close)(void*)=nullptr;
    void* wide=nullptr;std::size_t wideThreads=0;
    std::int64_t targetNs=33'333'333;
    std::chrono::steady_clock::time_point last{};
    std::uint64_t reports=0;
};
PerformanceHint performanceHint;
void startPerformanceHint(pid_t tid) {
    if(performanceHint.session || !tid)return;
    void* library=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
    const auto getManager=library?reinterpret_cast<void*(*)()>(dlsym(library,"APerformanceHint_getManager")):nullptr;
    const auto create=library?reinterpret_cast<void*(*)(void*,const std::int32_t*,std::size_t,std::int64_t)>(
        dlsym(library,"APerformanceHint_createSession")):nullptr;
    const auto report=library?reinterpret_cast<int(*)(void*,std::int64_t)>(dlsym(library,"APerformanceHint_reportActualWorkDuration")):nullptr;
    const auto updateTarget=library?reinterpret_cast<int(*)(void*,std::int64_t)>(dlsym(library,"APerformanceHint_updateTargetWorkDuration")):nullptr;
    void* manager=getManager?getManager():nullptr;
    const std::int32_t tids[1]{tid};
    void* session=manager && create?create(manager,tids,1,33'333'333):nullptr;
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PERF_HINT tid=%d manager=%d session=%d",
        tid,manager?1:0,session?1:0);
    if(session && report){performanceHint.report=report;performanceHint.session=session;performanceHint.updateTarget=updateTarget;}
    performanceHint.manager=manager;performanceHint.create=create; // v333
    performanceHint.close=library?reinterpret_cast<void(*)(void*)>(dlsym(library,"APerformanceHint_closeSession")):nullptr;
}
extern "C" { std::atomic<bool> recomp_v330_no_hint{false}; }
extern "C" { std::atomic<bool> recomp_v332_target60{false}; }
extern "C" { std::atomic<bool> recomp_v333_wide_hint{false}; } // v333: diag-v333-wide-hint (set by the renderer's window poll) // v332: diag-v332-target60 (set by the renderer's window poll) // v330: diag-v330-no-hint = no per-frame ADPF report (a binder call, ~70 us)
void reportFrameToPerformanceHint() {
    const auto now=std::chrono::steady_clock::now();
    if(recomp_v330_no_hint.load(std::memory_order_relaxed)){performanceHint.last=now;return;}
    { // v333: the wide session follows its switch (created / closed on change)
        const bool wantWide=recomp_v333_wide_hint.load(std::memory_order_relaxed);
        if(wantWide && !performanceHint.wide && performanceHint.manager && performanceHint.create) {
            std::vector<std::int32_t> tids;
            const auto add=[&](int t){if(t>0 && std::find(tids.begin(),tids.end(),t)==tids.end())tids.push_back(t);};
            add(recomp_v245::state().mainTid.load(std::memory_order_relaxed));
            for(const auto& t:jobWorkerTids)add(t.load(std::memory_order_relaxed));
            for(const auto& t:sceneWorkerTids)add(t.load(std::memory_order_relaxed));
            if(DIR* dir=opendir("/proc/self/task")) { // the Vulkan worker and the presenter, by name
                while(const auto* e=readdir(dir)) {
                    const int tid=std::atoi(e->d_name);if(tid<=0)continue;
                    char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/comm",tid);
                    if(FILE* f=std::fopen(path,"r")){char c[32]{};if(std::fgets(c,sizeof c,f)){if(!std::strncmp(c,"recomp-render-w",15)||!std::strncmp(c,"recomp-dpresent",15))add(tid);}std::fclose(f);}
                }
                closedir(dir);
            }
            performanceHint.wide=tids.empty()?nullptr:performanceHint.create(performanceHint.manager,tids.data(),tids.size(),performanceHint.targetNs);
            performanceHint.wideThreads=tids.size();
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V333_WIDE on=1 threads=%zu session=%d",tids.size(),performanceHint.wide?1:0);
        } else if(!wantWide && performanceHint.wide) {
            if(performanceHint.close)performanceHint.close(performanceHint.wide);
            performanceHint.wide=nullptr;
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V333_WIDE on=0");
        }
    }
    { // v332: diag-v332-target60 = the session's target 16.67 ms (the 60 fps goal) instead of 33.33 ms, applied on change
        const std::int64_t want=recomp_v332_target60.load(std::memory_order_relaxed)?16'666'667:33'333'333;
        if(performanceHint.session && performanceHint.updateTarget && want!=performanceHint.targetNs) {
            const int rc=performanceHint.updateTarget(performanceHint.session,want);performanceHint.targetNs=want;
            if(performanceHint.wide)performanceHint.updateTarget(performanceHint.wide,want); // v333
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V332_TARGET ns=%lld rc=%d",static_cast<long long>(want),rc);
        }
    }
    if(!performanceHint.session) {
        static unsigned tries=0;
        if(tries<3 && primeCoreTid.load(std::memory_order_relaxed)){++tries;startPerformanceHint(primeCoreTid.load());}
        performanceHint.last=now;return;
    }
    if(performanceHint.last.time_since_epoch().count()) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now-performanceHint.last).count();
        if(ns>0 && ns<2'000'000'000){performanceHint.report(performanceHint.session,ns);if(performanceHint.wide)performanceHint.report(performanceHint.wide,ns);} // (v333)
        if(!(++performanceHint.reports%600))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
            "RENDER_DIAG_PERF_HINT_REPORTS n=%llu last_ms=%lld",static_cast<unsigned long long>(performanceHint.reports),
            static_cast<long long>(ns/1000000));
    }
    performanceHint.last=now;
}
}

// latency-path (lat2, measurement only; called by the armed latency probe for every user-0 pad read): InputRead with whether
// this read changed for THE CALLING THREAD (the audio engine's frequent reads take v318's InputPoll, so the game's own read of a
// change is only visible per reading thread) and the guest call site (lat2::padReadLr, set by the import bridge).
[[gnu::noinline]] static void lat2NoteRead(const recomp::input::GamepadState& out,bool synthetic,bool firstOfChange) noexcept {
    static thread_local lat2::ThreadView<recomp::input::GamepadState> mine;
    const bool threadChanged=mine.update(out,[](const recomp::input::GamepadState& a,const recomp::input::GamepadState& b){
        return v318::sameInput(a,b);});
    recomp::diag::timeline::mark(recomp::diag::timeline::InputRead,lat2::readFlags(threadChanged,synthetic,firstOfChange),lat2::padReadLr);
}

// Optional iOS bridge owns one sequence across CPU fallback and GPU frames.
class IosSharedDisplay final {
    RecompPrivateTraceCallbacksV4 cpu_;
    RecompIosGpuCallbacksV1 gpu_;
    recomp::private_trace::DisplaySequence sequence_;
    mutable std::recursive_mutex mutex_;
    bool adopted_=false,closed_=false;
public:
    IosSharedDisplay(const RecompPrivateTraceCallbacksV4& cpu,const RecompIosGpuCallbacksV1& gpu):cpu_(cpu),gpu_(gpu){}
    void close(bool terminal) noexcept {std::lock_guard lock(mutex_);if(closed_)return;closed_=true;gpu_.close(gpu_.user,terminal?1:0);}
    [[noreturn]] void fail() noexcept {close(true);std::_Exit(4);}
    bool adopted()const noexcept{std::lock_guard lock(mutex_);return adopted_;}
    int cpu(const uint8_t* bytes,uint32_t size,uint32_t width,uint32_t height) noexcept {
        std::lock_guard lock(mutex_);
        if(adopted_ || closed_ || !bytes || size!=1280U*720U*4U || width!=1280 || height!=720)fail();
        const auto seq=sequence_.next();if(!sequence_.begin(seq))fail();
        const int result=cpu_.orderedRgba(cpu_.user,seq,bytes,size,width,height);
        if(result!=1 || !sequence_.finish(true))fail();return 1;
    }
    int gpu(RecompIosGpuFrameV1& frame) noexcept {
        std::lock_guard lock(mutex_);
        if(closed_)fail();frame.sequence=sequence_.next();if(!sequence_.begin(frame.sequence))fail();
        adopted_=true;
        if(gpu_.frame(gpu_.user,&frame)!=RECOMP_IOS_GPU_ACCEPTED || !sequence_.finish(true))fail();
        return RECOMP_IOS_GPU_ACCEPTED;
    }
};
static IosSharedDisplay* iosSharedDisplay=nullptr; // one entry; installed before workers

namespace recomp::gpu::generic {void setAsyncModeForTest(int);}

static int runTraceMain(int argc, char** argv,const RecompPrivateTraceCallbacksV2* callbacks=nullptr,
        const RecompPrivateTraceCallbacksV3* liveCallbacks=nullptr,
        const RecompPrivateTraceCallbacksV5* inputCallbacks=nullptr) {
    try {(void)guestFrameLimitPolicy();}
    catch(const std::invalid_argument&) {std::fputs("APPLE_IOS_GUEST_FRAME_LIMIT invalid=1 guest_started=0\n",stderr);return 2;}
    try {
        const bool asyncShared=recomp::ios_async_shared::requested();
        if(asyncShared && (!iosrendererasync::parse(std::getenv("RECOMP_IOS_RENDERER_ASYNC"),"invalid renderer async") ||
           !recomp::ios_shared::requested() || iosrendererasync::parse(std::getenv("RECOMP_IOS_QUEUED_PRESENT"),"invalid queued present")))
            throw std::invalid_argument("async shared present requires async1 shared1 queued0");
        const auto policy=iosrendererasync::configure(
            [](int mode){recomp::gpu::generic::setAsyncModeForTest(mode);},
            [](iosrendererasync::Marker marker){
                switch(marker) {
                case iosrendererasync::Marker::SyncRenderer:return ::access(RECOMP_APP_PATH("/private-trace/diag-generic-sync-renderer"),F_OK)==0;
                case iosrendererasync::Marker::SyncPresent:return ::access(RECOMP_APP_PATH("/private-trace/diag-generic-sync-present"),F_OK)==0;
                case iosrendererasync::Marker::SubmitProxy:return ::access(RECOMP_APP_PATH("/private-trace/diag-generic-submit-proxy"),F_OK)==0;
                // ios-direct-present (Claude): the library's presenter reads nothing back, so the CPU presenter's sync-readback
                // requirement for a queued present does not apply when the shell selected V6 (RECOMP_IOS_DIRECT_PRESENT=1).
                case iosrendererasync::Marker::SyncReadback:return ::access(RECOMP_APP_PATH("/private-trace/diag-generic-sync"),F_OK)==0 ||
                    [] { const char* e=std::getenv("RECOMP_IOS_DIRECT_PRESENT"); return e && e[0]=='1' && !e[1]; }();
                }
                return true;
            });
        recomp::ios_async_shared::enabled=asyncShared;
        std::fprintf(stderr,"APPLE_IOS_ASYNC_SHARED requested=%u initial_adoption=synchronous no_submit_reservation=1\n",unsigned(asyncShared));
        std::fprintf(stderr,"APPLE_IOS_RENDERER_ASYNC requested=%u forced_mode=%d queued_present=%u queued_readback_match=1 queued_effective=%u shared=%u combined=%u before_core=1 effective_pending_initialize=1\n",
            unsigned(policy.requested),policy.forcedMode,unsigned(policy.queuedPresent),unsigned(policy.queuedPresent),unsigned(policy.shared),unsigned(policy.shared && policy.requested));
    } catch(const std::exception& error) {
        std::fprintf(stderr,"APPLE_IOS_RENDERER_ASYNC rejected=1 guest_started=0 why=%s\n",error.what());return 2;
    }
    startCorePlacement(); // attempt v21

    std::array<std::uint64_t, 3> kernelVersionValues{};
    const bool hasKernelVersion = argc >= 5 &&
        std::string_view(argv[argc - 4]) == "--native-kernel-version";
    if (hasKernelVersion) {
        for (unsigned i = 0; i < kernelVersionValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 3 + i], kernelVersionValues[i]) ||
                kernelVersionValues[i] > UINT32_MAX) return 2;
        if (!recomp::bootstrap::guestKernelVersionBytes(kernelVersionValues[2])) return 2;
        argc -= 4;
    }
    const bool liveDeferredColor=argc>=2 && std::string_view(argv[argc-1])=="--experimental-native-deferred-color-prefix";
    if(liveDeferredColor)--argc;
    const bool diagnoseDeferredColor=argc>=2 && std::string_view(argv[argc-1])=="--diagnose-stopped-deferred-color";
    if(diagnoseDeferredColor)--argc;
    const bool diagnosePostOutputColor=argc>=2 && std::string_view(argv[argc-1])=="--diagnose-stopped-post-output-color";
    if(diagnosePostOutputColor)--argc;
    const bool livePostOutputColor=argc>=2 && std::string_view(argv[argc-1])=="--experimental-native-post-output-color";
    if(livePostOutputColor)--argc;
    const bool nativeDisplayOutput=argc>=2 && std::string_view(argv[argc-1])=="--experimental-native-normal-display-output";
    if(nativeDisplayOutput)--argc;
    const bool nativeIdentityScaler=argc>=2 && std::string_view(argv[argc-1])=="--experimental-native-identity-scaler";
    if(nativeIdentityScaler)--argc;
    std::array<std::uint64_t,2> scalerTimingValues{};
    const bool nativeScalerTiming=argc>=4 && std::string_view(argv[argc-3])=="--experimental-native-scaler-timing";
    if(nativeScalerTiming){
        for(unsigned i=0;i<2;++i)if(!parseUnsigned(argv[argc-2+i],scalerTimingValues[i]) || scalerTimingValues[i]>UINT32_MAX)return 2;
        argc-=3;
    }
    if(nativeIdentityScaler && !nativeScalerTiming)return 2;
    if(nativeDisplayOutput && (!nativeIdentityScaler || !liveCallbacks))return 2;
    if(diagnosePostOutputColor && !nativeDisplayOutput)return 2;
    if(livePostOutputColor && (!nativeDisplayOutput || diagnosePostOutputColor))return 2;
    if(diagnoseDeferredColor && !livePostOutputColor)return 2;
    if(liveDeferredColor && (!livePostOutputColor || diagnoseDeferredColor))return 2;
    const bool nativeSwapCommands=argc>=2 && std::string_view(argv[argc-1])=="--experimental-native-swap-commands";
    if(nativeSwapCommands)--argc;
    const bool captureNativeSwapImage=argc>=2 && std::string_view(argv[argc-1])=="--capture-native-swap-image";
    if(captureNativeSwapImage)--argc;
    const bool nativeNoSystemOverlay=argc>=2 && std::string_view(argv[argc-1])=="--native-no-system-overlay";
    if(nativeNoSystemOverlay)--argc;
    if(captureNativeSwapImage && !nativeNoSystemOverlay)return 2;
    if(callbacks && (!nativeNoSystemOverlay || captureNativeSwapImage))return 2;
    if(nativeSwapCommands!=bool(liveCallbacks) || (nativeSwapCommands &&
        (!nativeNoSystemOverlay || captureNativeSwapImage || callbacks)))return 2;
    // Private bounded native-program experiments. Inspection and scratch modes
    // remain post-stop; live publication requires its separately named flag.
    std::string inspectColorDirectory;
    bool renderStoppedColor=false,lockedStoppedColor=false;
    unsigned commitStoppedColor=0;
    bool experimentalOwnedColor=false;
    if(argc>=3 && (std::string_view(argv[argc-2])=="--inspect-stopped-color-program" ||
        std::string_view(argv[argc-2])=="--render-stopped-color-program" ||
        std::string_view(argv[argc-2])=="--render-locked-stopped-color-program" ||
        std::string_view(argv[argc-2])=="--commit-scratch-stopped-color-program" ||
        std::string_view(argv[argc-2])=="--reject-scratch-stopped-color-program" ||
        std::string_view(argv[argc-2])=="--stale-scratch-stopped-color-program" ||
        std::string_view(argv[argc-2])=="--experimental-owned-color-program")){
        experimentalOwnedColor=std::string_view(argv[argc-2])=="--experimental-owned-color-program";
        if(std::string_view(argv[argc-2])=="--commit-scratch-stopped-color-program")commitStoppedColor=1;
        if(std::string_view(argv[argc-2])=="--reject-scratch-stopped-color-program")commitStoppedColor=2;
        if(std::string_view(argv[argc-2])=="--stale-scratch-stopped-color-program")commitStoppedColor=3;
        lockedStoppedColor=std::string_view(argv[argc-2])=="--render-locked-stopped-color-program";
        renderStoppedColor=lockedStoppedColor || std::string_view(argv[argc-2])=="--render-stopped-color-program";
        inspectColorDirectory=argv[argc-1];argc-=2;
        if(inspectColorDirectory.empty() || inspectColorDirectory.front()!='/' || inspectColorDirectory.size()>4096)return 2;
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
    std::optional<recomp::gpu::NativeColorChainProgram> inspectColorProgram;
    if(!inspectColorDirectory.empty()){
        inspectColorProgram=recomp::gpu::readNativeColorProgram(inspectColorDirectory,recomp::gpu::NativeColorCoordinateProfile::Padded1280W1);
        if(!inspectColorProgram){std::cerr<<"trace setup failed: stopped color program rejected\n";return 2;}
    }
#else
    if(!inspectColorDirectory.empty())return 2;
#endif
    recomp::gpu::StoppedPhysicalRanges inspectOwnershipRanges;
    while(argc>=4 && std::string_view(argv[argc-3])=="--inspect-stopped-ownership"){
        std::uint64_t a=0,n=0;
        if(inspectOwnershipRanges.size()==8 || !parseUnsigned(argv[argc-2],a) ||
            !parseUnsigned(argv[argc-1],n) || a>UINT32_MAX || n>UINT32_MAX)return 2;
        inspectOwnershipRanges.push_back({static_cast<std::uint32_t>(a),static_cast<std::uint32_t>(n)});argc-=3;
    }
    std::reverse(inspectOwnershipRanges.begin(),inspectOwnershipRanges.end());
    if(!inspectOwnershipRanges.empty() && !recomp::gpu::validStoppedOwnershipRanges(inspectOwnershipRanges))return 2;
    recomp::gpu::StoppedPhysicalRanges capturePhysicalRanges;
    while (argc >= 4 && std::string_view(argv[argc - 3]) == "--capture-stopped-range") {
        std::uint64_t a = 0, n = 0;
        if (capturePhysicalRanges.size() == 8 || !parseUnsigned(argv[argc - 2],a) ||
                !parseUnsigned(argv[argc - 1],n) || a > UINT32_MAX || n > UINT32_MAX) return 2;
        capturePhysicalRanges.push_back({static_cast<std::uint32_t>(a),static_cast<std::uint32_t>(n)});
        argc -= 3;
    }
    std::reverse(capturePhysicalRanges.begin(),capturePhysicalRanges.end());
    std::uint32_t captureByteBudget=recomp::gpu::DefaultStoppedPhysicalByteBudget;
    if(argc>=3 && std::string_view(argv[argc-2])=="--capture-stopped-byte-budget") {
        std::uint64_t value=0;
        if(!parseUnsigned(argv[argc-1],value) || value>UINT32_MAX)return 2;
        captureByteBudget=static_cast<std::uint32_t>(value);argc-=2;
    }
    if ((!capturePhysicalRanges.empty() && !recomp::gpu::validStoppedPhysicalRanges(capturePhysicalRanges,captureByteBudget)) ||
        (capturePhysicalRanges.empty() && captureByteBudget!=recomp::gpu::DefaultStoppedPhysicalByteBudget)) return 2;
    const bool experimentalIdleCachePair = argc >= 2 && std::string_view(argv[argc - 1]) == "--experimental-idle-cache-pair";
    if (experimentalIdleCachePair) --argc;
    const bool nativeHighPriorityTiers = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-high-priority-tiers";
    if (nativeHighPriorityTiers) --argc;
    const bool nativeCriticalSectionWaits = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-critical-section-waits";
    if (nativeCriticalSectionWaits) --argc;
    const bool nativeCategoryOneUnityGain = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-category1-unity-gain";
    if (nativeCategoryOneUnityGain) --argc;
    const bool nativeEventWakeBoost = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-event-wake-boost";
    if (nativeEventWakeBoost) --argc;
    const bool experimentalNativeAudioRenderer = argc >= 2 && std::string_view(argv[argc - 1]) == "--experimental-native-audio-renderer";
    if (experimentalNativeAudioRenderer) --argc;
    if (nativeCategoryOneUnityGain && !experimentalNativeAudioRenderer) {
        std::cerr << "trace setup failed: category1 gain requires the native PCM renderer\n";return 2;
    }
    const bool experimentalNativeAudio = argc >= 2 && std::string_view(argv[argc - 1]) == "--experimental-native-audio";
    if (experimentalNativeAudio) --argc;
    if (experimentalNativeAudioRenderer && !experimentalNativeAudio) {
        std::cerr << "trace setup failed: renderer requires explicit experimental native audio\n";return 2;
    }
#if !(defined(__ANDROID__) || defined(__APPLE__))
    if (experimentalNativeAudio) { std::cerr << "trace setup failed: native audio currently requires Android\n"; return 2; }
#endif
    const bool disconnectedInput = argc >= 2 && std::string_view(argv[argc - 1]) == "--disconnected-input";
    if (disconnectedInput) --argc;
    const bool nativeInput = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-input";
    if(nativeInput)--argc;
    if(nativeInput && (disconnectedInput || !inputCallbacks))return 2;
    const bool imagePageProtections = argc >= 2 && std::string_view(argv[argc - 1]) == "--xex-image-protections";
    if (imagePageProtections) --argc;
    const bool experimentalGpuClock = argc >= 2 && std::string_view(argv[argc - 1]) == "--experimental-gpu-clock-500mhz";
    if (experimentalGpuClock) --argc;
    const bool captureStoppedPm4 = argc >= 2 && std::string_view(argv[argc - 1]) == "--capture-stopped-pm4";
    if (captureStoppedPm4) --argc;
    const bool captureStoppedShaders = argc >= 2 && std::string_view(argv[argc - 1]) == "--capture-stopped-shaders";
    if (captureStoppedShaders) --argc;
    if(diagnoseDeferredColor && (!captureStoppedPm4 || !captureStoppedShaders || !capturePhysicalRanges.empty()))return 2;
    const bool nativeStateBatches = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-state-batches";
    if (nativeStateBatches) --argc;
    const bool nativeEmptyCacheCoherency = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-empty-cache-coherency";
    if (nativeEmptyCacheCoherency) --argc;
    const bool experimentalNativeInitialization = argc >= 2 && std::string_view(argv[argc - 1]) == "--experimental-native-initialization";
    if (experimentalNativeInitialization) --argc;
    const bool nativeInitialCoherency = argc >= 2 && std::string_view(argv[argc - 1]) == "--native-initial-coherency";
    if (nativeInitialCoherency) --argc;
    if (experimentalNativeInitialization && !nativeInitialCoherency) return 2;
    if (nativeEmptyCacheCoherency && !experimentalNativeInitialization) return 2;
    if (nativeStateBatches && !experimentalNativeInitialization) return 2;
    if (experimentalIdleCachePair && (!nativeStateBatches || !nativeEmptyCacheCoherency)) return 2;
    if(experimentalOwnedColor && !nativeStateBatches)return 2;
    if(nativeNoSystemOverlay && !experimentalOwnedColor)return 2;
    if(nativeScalerTiming && (!nativeSwapCommands || !experimentalGpuClock))return 2;
    if(!inspectColorDirectory.empty() && !nativeStateBatches)return 2;
    const bool hasGraphics = argc >= 3 && std::string_view(argv[argc - 2]) == "--graphics-backend";
    if ((experimentalGpuClock || nativeInitialCoherency || captureStoppedShaders || captureStoppedPm4 ||
            !capturePhysicalRanges.empty() || !inspectOwnershipRanges.empty() || !inspectColorDirectory.empty()) && !hasGraphics) return 2;
    if (hasGraphics) {
#if !(defined(__ANDROID__) || defined(__APPLE__))
        std::cerr << "trace setup failed: Vulkan backend currently requires Android\n"; return 2;
#else
        if (std::string_view(argv[argc - 1]) != "vulkan-headless") return 2;
        argc -= 2;
#endif
    }
    std::array<std::uint64_t, 2> hsioLockValues{};
    const bool hasHsioLock = argc >= 4 && std::string_view(argv[argc - 3]) == "--hsio-lock";
    if (hasHsioLock) {
        for (std::size_t i = 0; i < hsioLockValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 2 + i],hsioLockValues[i]) || hsioLockValues[i] > UINT32_MAX) return 2;
        argc -= 3;
    }
    std::array<std::uint64_t, 2> certMonitorValues{};
    const bool hasNoCertMonitor = argc >= 4 && std::string_view(argv[argc - 3]) == "--no-cert-monitor";
    if (hasNoCertMonitor) {
        for (std::size_t i = 0; i < certMonitorValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 2 + i], certMonitorValues[i]) || certMonitorValues[i] > UINT32_MAX) return 2;
        argc -= 3;
    }
    std::array<std::uint64_t, 2> debugMonitorValues{};
    const bool hasNoDebugMonitor = argc >= 4 && std::string_view(argv[argc - 3]) == "--no-debug-monitor";
    if (hasNoDebugMonitor) {
        for (std::size_t i = 0; i < debugMonitorValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 2 + i], debugMonitorValues[i]) || debugMonitorValues[i] > UINT32_MAX) return 2;
        argc -= 3;
    }
    std::array<std::uint64_t, 2> commandLineValues{};
    const bool hasCommandLine = argc >= 4 && std::string_view(argv[argc - 3]) == "--command-line";
    if (hasCommandLine) {
        for (std::size_t i = 0; i < commandLineValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 2 + i], commandLineValues[i]) || commandLineValues[i] > UINT32_MAX) return 2;
        argc -= 3;
    }
    std::array<std::uint64_t, 2> uptimeValues{};
    const bool hasUptime = argc >= 4 && std::string_view(argv[argc - 3]) == "--uptime-bundle";
    if (hasUptime) {
        for (std::size_t i = 0; i < uptimeValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 2 + i], uptimeValues[i]) || uptimeValues[i] > UINT32_MAX) return 2;
        argc -= 3;
    }
    std::array<std::uint64_t, 3> videoGlobalValues{};
    const bool hasVideoGlobals = argc >= 5 && std::string_view(argv[argc - 4]) == "--video-globals";
    if (hasVideoGlobals) {
        for (std::size_t i = 0; i < videoGlobalValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 3 + i], videoGlobalValues[i]) || videoGlobalValues[i] > UINT32_MAX) return 2;
        argc -= 4;
    }
    std::array<std::uint64_t, 2> hardwareInfoValues{};
    const bool hasHardwareInfo = argc >= 4 && std::string_view(argv[argc - 3]) == "--hardware-info";
    if (hasHardwareInfo) {
        for (std::size_t i = 0; i < hardwareInfoValues.size(); ++i)
            if (!parseUnsigned(argv[argc - 2 + i], hardwareInfoValues[i]) || hardwareInfoValues[i] > UINT32_MAX) return 2;
        argc -= 3;
    }
    std::vector<std::uint32_t> unboundDataSlots;
    if (argc >= 3 && std::string_view(argv[argc - 2]) == "--unbound-data-imports") {
        std::string_view remaining(argv[argc - 1]);
        do {
            const auto split = remaining.find(',');
            const auto token = remaining.substr(0, split);
            std::uint64_t address = 0;
            if (unboundDataSlots.size() == 256 || token.empty() ||
                !parseUnsigned(std::string(token).c_str(), address) || address > UINT32_MAX) return 2;
            unboundDataSlots.push_back(static_cast<std::uint32_t>(address));
            if (split == std::string_view::npos) break;
            remaining.remove_prefix(split + 1);
        } while (true);
        argc -= 2;
    }
    const bool hasVirtualVideoMode = argc >= 3 && std::string_view(argv[argc - 2]) == "--virtual-video-mode";
    if (hasVirtualVideoMode) {
        if (std::string_view(argv[argc - 1]) != "720p60") {
            std::cerr << "trace setup failed: unsupported virtual video mode\n"; return 2;
        }
        argc -= 2;
    }
    const bool cooperative = argc >= 3
            && std::string_view(argv[argc - 2]) == "--cooperative-threads";
    if ((experimentalNativeAudioRenderer || nativeEventWakeBoost || nativeCriticalSectionWaits || nativeHighPriorityTiers) && !cooperative) {
        std::cerr << "trace setup failed: native audio/wait scheduling requires serialized cooperative execution\n";return 2;
    }
    std::uint64_t quantum = 32;
    if (cooperative) {
        if (!parseUnsigned(argv[argc - 1], quantum) || quantum == 0 || quantum > 1000000) {
            std::cerr << "trace setup failed: invalid cooperative quantum\n";
            return 2;
        }
        argc -= 2;
    }
    const bool hasNativeXam = argc >= 4 && std::string_view(argv[argc - 3]) == "--native-xam-module";
    std::uint64_t nativeXamAddress = 0, nativeXamVersion = 0;
    if (hasNativeXam) {
        if (!cooperative || !parseUnsigned(argv[argc - 2], nativeXamAddress)
                || !parseUnsigned(argv[argc - 1], nativeXamVersion) || nativeXamVersion >> 28 != 2
                || nativeXamAddress == 0 || nativeXamAddress > UINT32_MAX || (nativeXamAddress & 3U) != 0
                || !fitsGuestRange(nativeXamAddress, 0x64)) {
            std::cerr << "trace setup failed: invalid native XAM module range\n";
            return 2;
        }
        argc -= 3;
    }
    std::unique_ptr<recomp::bootstrap::GuestNativeReadOnlyDirectory> nativeGameCache;
    if (argc >= 3 && std::string_view(argv[argc - 2]) == "--native-game-cache-directory") {
        nativeGameCache = std::make_unique<recomp::bootstrap::GuestNativeReadOnlyDirectory>("gamecache:", argv[argc - 1]);
        if (!nativeGameCache->ready()) { std::cerr << "trace setup failed: native game-cache directory is unavailable\n"; return 2; }
        argc -= 2;
    }
    std::unique_ptr<recomp::bootstrap::GuestNativeReadOnlyDirectory> nativeUpdate;
    if (argc >= 3 && std::string_view(argv[argc - 2]) == "--native-update-directory") {
        nativeUpdate = std::make_unique<recomp::bootstrap::GuestNativeReadOnlyDirectory>("update:", argv[argc - 1]);
        if (!nativeUpdate->ready()) { std::cerr << "trace setup failed: native update directory is unavailable\n"; return 2; }
        argc -= 2;
    }
    std::unique_ptr<recomp::bootstrap::GuestNativeReadOnlyDirectory> nativeCache;
    if (argc >= 3 && std::string_view(argv[argc - 2]) == "--native-cache-directory") {
        nativeCache = std::make_unique<recomp::bootstrap::GuestNativeReadOnlyDirectory>("cache:", argv[argc - 1]);
        if (!nativeCache->ready()) { std::cerr << "trace setup failed: native cache directory is unavailable\n"; return 2; }
        argc -= 2;
    }
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> fileRequests;
    while (argc >= 4 && std::string_view(argv[argc - 3]) == "--read-only-file") {
        std::uint64_t index = 0;
        if (fileRequests.size() == 16 || !parseUnsigned(argv[argc - 2], index) || index == 0 || index > 16384) {
            std::cerr << "trace setup failed: invalid explicit content binding\n"; return 2;
        }
        fileRequests.emplace_back(static_cast<std::uint32_t>(index), argv[argc - 1]);
        argc -= 3;
    }
    const bool hasFileMetadata = argc >= 3 && std::string_view(argv[argc - 2]) == "--file-metadata-catalog";
    recomp::bootstrap::GuestFileMetadataCatalog fileMetadata;
    if (hasFileMetadata) {
        const std::filesystem::path path(argv[argc - 1]);
        ScopedFile input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat info {};
        if (!path.is_absolute() || input.get() < 0 || fstat(input.get(), &info) != 0
                || !S_ISREG(info.st_mode) || info.st_size < 16
                || static_cast<std::uint64_t>(info.st_size) > fileMetadata.MaximumBytes) {
            std::cerr << "trace setup failed: file metadata catalog is not bounded\n";
            return 2;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = read(input.get(), bytes.data() + done, bytes.size() - done);
            if (count <= 0) { std::cerr << "trace setup failed: file metadata read failed\n"; return 2; }
            done += static_cast<std::size_t>(count);
        }
        if (!fileMetadata.load(bytes)) { std::cerr << "trace setup failed: invalid file metadata catalog\n"; return 2; }
        argc -= 2;
    }
    struct InstalledFileRequest { std::uint32_t package, record; std::filesystem::path path; };
    std::vector<InstalledFileRequest> installedFileRequests;
    while (argc >= 5 && std::string_view(argv[argc - 4]) == "--installed-file") {
        std::uint64_t package = 0, record = 0;
        if (installedFileRequests.size() == 32 || !parseUnsigned(argv[argc - 3], package) || !parseUnsigned(argv[argc - 2], record)
                || package == 0 || package > 16 || record == 0 || record > 256) {
            std::cerr << "trace setup failed: invalid installed file request\n"; return 2;
        }
        installedFileRequests.push_back({static_cast<std::uint32_t>(package), static_cast<std::uint32_t>(record), argv[argc - 1]});
        argc -= 4;
    }
    struct InstalledPackage { recomp::bootstrap::GuestFileMetadataCatalog metadata; recomp::bootstrap::GuestImportRegistry::ContentFiles files; };
    std::map<std::uint32_t, InstalledPackage> installedPackages;
    while (argc >= 4 && std::string_view(argv[argc - 3]) == "--installed-package") {
        std::uint64_t index = 0;
        const std::filesystem::path path(argv[argc - 1]);
        if (!parseUnsigned(argv[argc - 2], index) || index == 0 || index > 16 || installedPackages.count(index) || !path.is_absolute()) {
            std::cerr << "trace setup failed: invalid installed package request\n"; return 2;
        }
        ScopedFile input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat info {};
        if (input.get() < 0 || fstat(input.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 16
                || static_cast<std::uint64_t>(info.st_size) > recomp::bootstrap::GuestFileMetadataCatalog::MaximumBytes) {
            std::cerr << "trace setup failed: installed metadata is not bounded\n"; return 2;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = read(input.get(), bytes.data() + done, bytes.size() - done);
            if (count <= 0) { std::cerr << "trace setup failed: installed metadata read failed\n"; return 2; }
            done += static_cast<std::size_t>(count);
        }
        InstalledPackage package;
        if (!package.metadata.load(bytes) || package.metadata.size() > 256 || package.metadata.pathAt(1) != std::optional<std::string>{"content:\\"}) {
            std::cerr << "trace setup failed: installed package metadata rejected\n"; return 2;
        }
        installedPackages.emplace(static_cast<std::uint32_t>(index), std::move(package));
        argc -= 3;
    }
    for (const auto& request : installedFileRequests) {
        auto found = installedPackages.find(request.package);
        if (found == installedPackages.end() || !request.path.is_absolute()) {
            std::cerr << "trace setup failed: installed file lacks package metadata\n"; return 2;
        }
        auto& package = found->second;
        const auto path = package.metadata.pathAt(request.record);
        if (!path || package.files.count(*path)) { std::cerr << "trace setup failed: duplicate/invalid installed record\n"; return 2; }
        const auto entry = package.metadata.query(*path);
        ScopedFile source(open(request.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat info {};
        if (entry.state != recomp::bootstrap::GuestFileQueryState::Found || entry.metadata.attributes != 1
                || entry.metadata.values[5] > (8ULL << 30) || source.get() < 0 || fstat(source.get(), &info) != 0
                || !S_ISREG(info.st_mode) || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != entry.metadata.values[5]) {
            std::cerr << "trace setup failed: installed content size/type mismatch\n"; return 2;
        }
        auto file = std::make_shared<const NativeReadOnlyFile>(source.get(), entry.metadata.values[5]);
        (void)source.release();
        package.files.emplace(*path, std::move(file));
    }
    const bool hasInstalledContent = argc >= 3 && std::string_view(argv[argc - 2]) == "--installed-content-catalog";
    if (!hasInstalledContent && !installedPackages.empty()) { std::cerr << "trace setup failed: installed catalog missing\n"; return 2; }
    recomp::bootstrap::GuestContentCatalog installedContent;
    if (hasInstalledContent) {
        const std::filesystem::path path(argv[argc - 1]);
        ScopedFile input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat info {};
        if (!path.is_absolute() || input.get() < 0 || fstat(input.get(), &info) != 0
                || !S_ISREG(info.st_mode) || info.st_size < 16
                || static_cast<std::uint64_t>(info.st_size) > installedContent.MaximumBytes) {
            std::cerr << "trace setup failed: installed-content catalog is not bounded\n"; return 2;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto count = read(input.get(), bytes.data() + done, bytes.size() - done);
            if (count <= 0) { std::cerr << "trace setup failed: content catalog read failed\n"; return 2; }
            done += static_cast<std::size_t>(count);
        }
        if (!installedContent.load(bytes)) { std::cerr << "trace setup failed: invalid content catalog\n"; return 2; }
        argc -= 2;
    }
    std::map<std::string, std::shared_ptr<const NativeReadOnlyFile>> fileSources;
    for (const auto& request : fileRequests) {
        const auto guestPath = fileMetadata.pathAt(request.first);
        if (!guestPath || !request.second.is_absolute() || fileSources.count(*guestPath)) {
            std::cerr << "trace setup failed: content record is missing, duplicate or unmounted\n"; return 2;
        }
        const auto metadata = fileMetadata.query(*guestPath);
        const auto descriptor = open(request.second.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        struct stat info {};
        if (metadata.state != recomp::bootstrap::GuestFileQueryState::Found || metadata.metadata.attributes != 1
                || descriptor < 0 || fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0
                || static_cast<std::uint64_t>(info.st_size) != metadata.metadata.values[5]
                || metadata.metadata.values[5] > (8ULL << 30)) {
            if (descriptor >= 0) { close(descriptor); }
            std::cerr << "trace setup failed: explicit content is not a matching regular file\n"; return 2;
        }
        ScopedFile ownership(descriptor);
        auto source = std::make_shared<const NativeReadOnlyFile>(descriptor, metadata.metadata.values[5]);
        (void)ownership.release();
        fileSources.emplace(*guestPath, std::move(source));
    }
    // argv[1] is the one explicit decoded-image path. All numeric arguments
    // are validated by the Python driver and passed in decimal form:
    // image base/size, heap base/size, stack base/size, budget, label limit,
    // and the separately validated mapping count. An optional physical arena
    // contributes guest base/size and the physical address at that base.
    std::array<std::uint64_t, 9> values{};
    std::array<std::uint64_t, 3> physicalValues{};
    std::array<std::uint64_t, 8> titleValues{};
    std::array<std::uint64_t, 7> threadValues{};
    std::array<std::uint64_t, 2> threadPoolValues{};
    std::array<std::uint64_t, 2> threadTypeValues{};
    bool hasPhysicalArena = false;
    bool hasTitleEnvironment = false;
    bool hasThreadEnvironment = false;
    bool hasThreadPool = false;
    bool hasThreadType = false;
    std::size_t rawXexArgumentIndex = 0;
    if (!parseArguments(
                argc,
                argv,
                values,
                hasPhysicalArena,
                physicalValues,
                hasTitleEnvironment,
                titleValues,
                rawXexArgumentIndex,
                hasThreadEnvironment,
                threadValues,
                hasThreadPool,
                threadPoolValues,
                hasThreadType,
                threadTypeValues)
            || (cooperative && (!hasThreadPool || !hasThreadType))
            || values[0] > UINT32_MAX
            || !fitsGuestRange(values[0], values[1])
            || values[2] > UINT32_MAX
            || values[3] > UINT32_MAX
            || values[4] > UINT32_MAX
            || values[5] > UINT32_MAX
            || values[6] == 0
            || values[7] > values[6]
            || values[7] > std::numeric_limits<std::size_t>::max()
            || values[8] > std::numeric_limits<std::size_t>::max()
            || values[2] == 0
            || values[5] == 0
            || (values[2] % PageSize) != 0
            || (values[3] % PageSize) != 0
            || (values[4] % PageSize) != 0
            || (values[5] % PageSize) != 0
            || values[4] + values[5] > UINT32_MAX
            || ((values[4] + values[5]) & 15U) != 0) {
        std::cerr << "trace setup failed: invalid internal invocation\n";
        return 2;
    }
    const auto imageBase = static_cast<std::uint32_t>(values[0]);
    const auto imageSize = values[1];
    const auto heapBase = static_cast<std::uint32_t>(values[2]);
    const auto heapSize = static_cast<std::uint32_t>(values[3]);
    const auto stackBase = static_cast<std::uint32_t>(values[4]);
    const auto stackSize = static_cast<std::uint32_t>(values[5]);
    const auto functionEntryBudget = values[6];
    const auto functionLabelLimit = static_cast<std::size_t>(values[7]);
    const auto expectedMappingCount = static_cast<std::size_t>(values[8]);

    if (hasPhysicalArena) {
        const NumericRange physicalArena{physicalValues[0], physicalValues[1]};
        const NumericRange physicalAddressRange{physicalValues[2], physicalValues[1]};
        const NumericRange image{imageBase, imageSize};
        const NumericRange heap{heapBase, heapSize};
        const NumericRange stack{stackBase, stackSize};
        if (physicalValues[0] == 0
                || physicalValues[1] == 0
                || physicalValues[0] > UINT32_MAX
                || physicalValues[1] > UINT32_MAX
                || physicalValues[2] > UINT32_MAX
                || (physicalValues[0] % PageSize) != 0
                || (physicalValues[1] % PageSize) != 0
                || (physicalValues[2] % PageSize) != 0
                || !fitsGuestRange(physicalArena.base, physicalArena.size)
                || !fitsGuestRange(
                        physicalAddressRange.base,
                        physicalAddressRange.size)
                || rangesOverlap(physicalArena, image)
                || rangesOverlap(physicalArena, heap)
                || rangesOverlap(physicalArena, stack)) {
            std::cerr << "trace setup failed: invalid physical-memory invocation\n";
            return 2;
        }
    }

    std::size_t validatedMappingCount = 0;
    if (!recomp::bootstrap::initializeGuestIndirectCallResolver(
                expectedMappingCount, validatedMappingCount)) {
        std::cerr << "trace setup failed: generated function mapping rejected\n";
        return 2;
    }

    if (hasTitleEnvironment
            && (titleValues[1] > UINT32_MAX
                || titleValues[2] > UINT32_MAX
                || titleValues[4] > UINT32_MAX
                || titleValues[5] > UINT32_MAX
                || titleValues[6] > UINT32_MAX
                || titleValues[7] > UINT32_MAX
                || !titleLayoutIsValid(values, titleValues))) {
        std::cerr << "trace setup failed: invalid title-environment invocation\n";
        return 2;
    }
    if (hasPhysicalArena && hasTitleEnvironment) {
        const NumericRange physicalArena{physicalValues[0], physicalValues[1]};
        const std::array titleRanges{
                NumericRange{values[0], titleValues[1]},
                NumericRange{titleValues[2], titleValues[3]},
                NumericRange{titleValues[5], 4},
                NumericRange{titleValues[6], ModuleStructureSize},
        };
        if (std::any_of(
                    titleRanges.begin(),
                    titleRanges.end(),
                    [&](const NumericRange& range) {
                        return rangesOverlap(physicalArena, range);
                    })) {
            std::cerr << "trace setup failed: physical-memory/title overlap\n";
            return 2;
        }
    }
    if (hasThreadEnvironment) {
        using ThreadEnvironment = recomp::bootstrap::GuestThreadEnvironment;
        const std::array threadRanges{
                NumericRange{threadValues[0], ThreadEnvironment::PcrSize},
                NumericRange{threadValues[1], ThreadEnvironment::ThreadSize},
                NumericRange{threadValues[2], threadValues[5] + threadValues[6] * 4},
        };
        const std::array otherRanges{
                NumericRange{values[0], titleValues[1]},
                NumericRange{heapBase, heapSize},
                NumericRange{stackBase, stackSize},
                NumericRange{titleValues[2], titleValues[3]},
                NumericRange{titleValues[5], 4},
                NumericRange{titleValues[6], ModuleStructureSize},
        };
        for (const auto& threadRange : threadRanges) {
            if (!fitsGuestRange(threadRange.base, threadRange.size)
                    || std::any_of(
                            otherRanges.begin(), otherRanges.end(),
                            [&](const NumericRange& range) {
                                return rangesOverlap(threadRange, range);
                            })
                    || (hasPhysicalArena && rangesOverlap(
                            threadRange,
                            NumericRange{physicalValues[0], physicalValues[1]}))) {
                std::cerr << "trace setup failed: thread scratch range rejected\n";
                return 2;
            }
        }
    }

    recomp::bootstrap::GuestAddressSpace addressSpace;
    if (hasThreadType) {
        const NumericRange identity{threadTypeValues[1], 4};
        const std::array otherRanges{
                NumericRange{imageBase, titleValues[1]}, NumericRange{heapBase, heapSize},
                NumericRange{stackBase, stackSize}, NumericRange{titleValues[2], titleValues[3]},
                NumericRange{titleValues[5], 4}, NumericRange{titleValues[6], ModuleStructureSize},
                NumericRange{threadValues[0], recomp::bootstrap::GuestThreadEnvironment::PcrSize},
                NumericRange{threadValues[1], recomp::bootstrap::GuestThreadEnvironment::ThreadSize},
                NumericRange{threadValues[2], threadValues[5] + threadValues[6] * 4},
        };
        if (identity.base == 0 || identity.base % 4 != 0 || !fitsGuestRange(identity.base, 4)
                || std::any_of(otherRanges.begin(), otherRanges.end(),
                        [&](const auto& range) { return rangesOverlap(identity, range); })
                || (hasThreadPool && rangesOverlap(identity, {threadPoolValues[0], threadPoolValues[1]}))
                || (hasPhysicalArena && rangesOverlap(identity, {physicalValues[0], physicalValues[1]}))) {
            std::cerr << "trace setup failed: thread type identity range rejected\n";
            return 2;
        }
    }
    if (hasNativeXam) {
        const NumericRange xam{nativeXamAddress, 0x64};
        const std::array others{
                NumericRange{imageBase, titleValues[1]}, NumericRange{heapBase, heapSize}, NumericRange{stackBase, stackSize},
                NumericRange{titleValues[2], titleValues[3]}, NumericRange{titleValues[5], 4}, NumericRange{titleValues[6], ModuleStructureSize},
                NumericRange{threadValues[0], recomp::bootstrap::GuestThreadEnvironment::PcrSize},
                NumericRange{threadValues[1], recomp::bootstrap::GuestThreadEnvironment::ThreadSize},
                NumericRange{threadValues[2], threadValues[5] + threadValues[6] * 4},
                NumericRange{threadPoolValues[0], threadPoolValues[1]}, NumericRange{threadTypeValues[1], 4},
                NumericRange{physicalValues[0], hasPhysicalArena ? physicalValues[1] : 0},
        };
        if (std::any_of(others.begin(), others.end(), [&](const auto& range) { return rangesOverlap(xam, range); })) {
            std::cerr << "trace setup failed: native XAM module overlaps another range\n";
            return 2;
        }
    }
    std::vector<NumericRange> extraDataRanges;
    if (hasKernelVersion) extraDataRanges.push_back({kernelVersionValues[1], 8});
    if (hasHardwareInfo) extraDataRanges.push_back({hardwareInfoValues[1], 16});
    if (hasVideoGlobals) extraDataRanges.push_back({videoGlobalValues[2], 8});
    if (hasUptime) extraDataRanges.push_back({uptimeValues[1], 24});
    if (hasCommandLine) extraDataRanges.push_back({commandLineValues[1], 64});
    if (hasNoDebugMonitor) extraDataRanges.push_back({debugMonitorValues[1], 4});
    if (hasNoCertMonitor) extraDataRanges.push_back({certMonitorValues[1], 4});
    if (hasHsioLock) extraDataRanges.push_back({hsioLockValues[1],28});
    if(nativeScalerTiming)extraDataRanges.push_back({scalerTimingValues[1],4});
    const std::vector<NumericRange> otherDataRanges{
        {imageBase, hasTitleEnvironment ? titleValues[1] : imageSize}, {heapBase, heapSize}, {stackBase, stackSize},
        {titleValues[2], hasTitleEnvironment ? titleValues[3] : 0}, {titleValues[5], hasTitleEnvironment ? 4U : 0U},
        {titleValues[6], hasTitleEnvironment ? ModuleStructureSize : 0U},
        {threadValues[0], hasThreadEnvironment ? recomp::bootstrap::GuestThreadEnvironment::PcrSize : 0U},
        {threadValues[1], hasThreadEnvironment ? recomp::bootstrap::GuestThreadEnvironment::ThreadSize : 0U},
        {threadValues[2], hasThreadEnvironment ? threadValues[5] + threadValues[6] * 4 : 0},
        {threadPoolValues[0], hasThreadPool ? threadPoolValues[1] : 0}, {threadTypeValues[1], hasThreadType ? 4U : 0U},
        {nativeXamAddress, hasNativeXam ? 0x64U : 0U}, {physicalValues[0], hasPhysicalArena ? physicalValues[1] : 0}};
    if ((!extraDataRanges.empty() && !hasTitleEnvironment) || (hasVideoGlobals && imageSize > UINT32_MAX) ||
        (hasHardwareInfo && (!cooperative || !hasInstalledContent))) return 2;
    for (std::size_t i = 0; i < extraDataRanges.size(); ++i) {
        const auto range = extraDataRanges[i];
        if (!range.base || range.base % 4 || !fitsGuestRange(range.base, range.size) ||
            std::any_of(otherDataRanges.begin(), otherDataRanges.end(), [&](auto other) { return other.size && rangesOverlap(range, other); })) return 2;
        for (std::size_t j = 0; j < i; ++j) if (rangesOverlap(range, extraDataRanges[j])) return 2;
    }
    const bool physicalAcAlias=hasPhysicalArena && physicalValues[0]==0xA0000000 &&
        physicalValues[1]==0x20000000 && physicalValues[2]==0;
    if(physicalAcAlias) {
        const NumericRange alias{0xC0000000,0x20000000};
        const auto overlapsAlias=[&](auto range){return range.size && rangesOverlap(range,alias);};
        if(std::any_of(otherDataRanges.begin(),otherDataRanges.end(),overlapsAlias) ||
           std::any_of(extraDataRanges.begin(),extraDataRanges.end(),overlapsAlias)) {
            std::cerr<<"trace setup failed: physical alias overlaps configured guest data\n";
            return 2;
        }
    }
    // Configure coherent physical views before loading any guest bytes. Keep
    // the existing flat layout for other explicitly configured test profiles.
    if (!(physicalAcAlias?addressSpace.reserveWithPhysicalAcAlias():addressSpace.reserve())) {
        std::cerr << "trace setup failed: guest address reservation unavailable\n";
        return 2;
    }
    if(physicalAcAlias)std::cout<<"GUEST_PHYSICAL_AC_ALIAS_V1 shared_backing=1 bytes=536870912 e_view_implemented=0\n";
#if (defined(__ANDROID__) || defined(__APPLE__))
    // v260: the renderer's write watch over both physical views (opt-in, files/private-trace/diag-v260-write-watch).
    if(physicalAcAlias)(void)recomp::gpu::generic::installWriteWatch(addressSpace.base());
#endif

    recomp::bootstrap::GuestImageLoader loader;
    if (!loader.loadExplicitDecodedImage(
                addressSpace,
                argv[1],
                imageBase,
                imageSize)) {
        std::cerr << "trace setup failed: decoded image rejected\n";
        return 2;
    }

    recomp::bootstrap::GuestTitleEnvironment titleEnvironment;
    if (hasTitleEnvironment) {
        const auto headerBase = static_cast<std::uint32_t>(titleValues[2]);
        if (!stageRawXexHeader(
                    addressSpace,
                    argv[rawXexArgumentIndex],
                    titleValues[0],
                    headerBase,
                    titleValues[3])) {
            std::cerr << "trace setup failed: raw XEX header staging rejected\n";
            return 2;
        }
        const recomp::bootstrap::GuestTitleEnvironmentLayout titleLayout{
                static_cast<std::uint32_t>(titleValues[4]),
                static_cast<std::uint32_t>(titleValues[5]),
                static_cast<std::uint32_t>(titleValues[6]),
                headerBase,
                imageBase,
                static_cast<std::uint32_t>(titleValues[1]),
                static_cast<std::uint32_t>(titleValues[7]),
                hasThreadType ? static_cast<std::uint32_t>(threadTypeValues[0]) : 0U,
                hasThreadType ? static_cast<std::uint32_t>(threadTypeValues[1]) : 0U,
        };
        if (!titleEnvironment.initialize(addressSpace, titleLayout)) {
            std::cerr << "trace setup failed: title environment rejected\n";
            return 2;
        }
    }

    recomp::bootstrap::GuestThreadEnvironment threadEnvironment;
    if (hasThreadEnvironment) {
        if (!threadInputsMatchHeader(
                    addressSpace,
                    static_cast<std::uint32_t>(titleValues[2]),
                    titleValues[3],
                    threadValues)) {
            std::cerr << "trace setup failed: staged TLS metadata mismatch\n";
            return 2;
        }
        recomp::bootstrap::GuestThreadEnvironmentLayout threadLayout{};
        threadLayout.pcrAddress = static_cast<std::uint32_t>(threadValues[0]);
        threadLayout.threadAddress = static_cast<std::uint32_t>(threadValues[1]);
        threadLayout.tlsAddress = static_cast<std::uint32_t>(threadValues[2]);
        threadLayout.imageBase = imageBase;
        threadLayout.imageSize = static_cast<std::uint32_t>(imageSize);
        threadLayout.stackBase = stackBase;
        threadLayout.stackSize = stackSize;
        threadLayout.tlsRawDataAddress = static_cast<std::uint32_t>(threadValues[3]);
        threadLayout.tlsRawDataSize = static_cast<std::uint32_t>(threadValues[4]);
        threadLayout.tlsDataSize = static_cast<std::uint32_t>(threadValues[5]);
        threadLayout.tlsSlotCount = static_cast<std::uint32_t>(threadValues[6]);
        threadLayout.threadId = 1;
        if (!threadEnvironment.initialize(addressSpace, threadLayout)) {
            std::cerr << "trace setup failed: thread environment rejected\n";
            return 2;
        }
    }

    std::uint64_t debugMessageCount = 0;
    std::uint64_t debugMessageBytes = 0;
    recomp::bootstrap::GuestImportRegistry registry;
    if (!registry.initialize(
                addressSpace,
                heapBase,
                heapSize,
                [&](std::string_view message) {
                    ++debugMessageCount;
                    addSaturating(debugMessageBytes, message.size());
                    // Guest text is deliberately not printed or retained.
                })) {
        std::cerr << "trace setup failed: import registry rejected heap range\n";
        return 2;
    }
    if (hasVirtualVideoMode && !registry.bindVirtualVideoMode720p60()) {
        std::cerr << "trace setup failed: virtual video binding rejected\n"; return 2;
    }
    if (imagePageProtections && (!hasTitleEnvironment || !registry.bindImagePageProtections(
            static_cast<std::uint32_t>(titleValues[2]), static_cast<std::uint32_t>(titleValues[3]),
            imageBase, static_cast<std::uint32_t>(titleValues[1])))) {
        std::cerr << "trace setup failed: XEX image page metadata rejected\n";
        return 2;
    }
    if (!registry.bindDpcInitialization([](auto address) {
        return recomp::bootstrap::hasExactGuestFunction(address);
    })) { std::cerr << "trace setup failed: DPC initializer binding rejected\n"; return 2; }
    // The generated runtime hook reads the shared fixed-rate CPU timebase.
    // UTC system time is a separate domain; neither validates GPU dword 044B.
    using FileTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    const auto utc = recomp::bootstrap::fileTimeFromUnixTicks(
            std::chrono::duration_cast<FileTicks>(std::chrono::system_clock::now().time_since_epoch()).count());
    const auto monotonicStart = std::chrono::steady_clock::now();
    if (!utc || !registry.bindClockSources([utc, monotonicStart] {
                return recomp::bootstrap::advanceFileTime(*utc,
                        std::chrono::duration_cast<FileTicks>(std::chrono::steady_clock::now() - monotonicStart).count());
            }, recomp::bootstrap::GuestTimebaseFrequency)) {
        std::cerr << "trace setup failed: clock binding rejected\n";
        return 2;
    }
    const auto guestThreadToken = hasThreadEnvironment
            ? static_cast<std::uint32_t>(threadValues[1])
            : static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(stackBase) + stackSize - 16U);
    recomp::bootstrap::GuestImportCall clockProbe;
    if (!registry.dispatch("__imp__KeQueryPerformanceFrequency", clockProbe).handled()
            || clockProbe.returnValue != recomp::bootstrap::GuestTimebaseFrequency) {
        std::cerr << "trace setup failed: performance frequency disagrees with CPU timebase\n";
        return 2;
    }
    if (!registry.bindGuestThreadToken(guestThreadToken)) {
        std::cerr << "trace setup failed: explicit guest thread token rejected\n";
        return 2;
    }
    if (hasPhysicalArena
            && !registry.bindPhysicalMemoryArena(
                    static_cast<std::uint32_t>(physicalValues[0]),
                    static_cast<std::uint32_t>(physicalValues[1]),
                    static_cast<std::uint32_t>(physicalValues[2]))) {
        std::cerr << "trace setup failed: physical-memory arena binding rejected\n";
        return 2;
    }
#if defined(RECOMP_NATIVE_XMA)
    std::shared_ptr<recomp::audio::NativeXmaContextPool> nativeXma;
    struct XmaRegisterScope { ~XmaRegisterScope() { recomp::bootstrap::resetGuestXmaRegisters(); } } xmaRegisterScope;
    if (hasPhysicalArena) {
        recomp::bootstrap::GuestImportCall allocation;
        allocation.arguments={0,recomp::bootstrap::GuestXmaBackend::PoolBytes,0x204,0,UINT32_MAX,256,0,0};
        if (!registry.dispatch("__imp__MmAllocatePhysicalMemoryEx",allocation).handled() || !allocation.returnValue) return 2;
        const auto v=static_cast<std::uint32_t>(allocation.returnValue);
        recomp::bootstrap::GuestImportCall physical; physical.arguments[0]=v;
        if (!registry.dispatch("__imp__MmGetPhysicalAddress",physical).handled()) return 2;
        nativeXma=std::make_shared<recomp::audio::NativeXmaContextPool>(v,static_cast<std::uint32_t>(physical.returnValue),
            addressSpace.translate(v),[&](auto p,auto bytes,auto writable) { return registry.mapNativeAudioPhysical(p,bytes,writable); });
        if (!nativeXma->ready() || !registry.bindNativeXma(nativeXma) ||
            !recomp::bootstrap::bindGuestXmaRegisters([&](auto address,bool write,std::uint32_t& value) {
#if defined(__APPLE__)
                { // ios-qos-audio (Claude 2026-10-16; scheduling only, ADOPTED by c107-c109): this callback runs on the guest audio
                  // thread (the XMA register accesses; decode runs inside them). The thread asks for utility QoS (efficiency cores):
                  // the CPU and GPU share one power budget and the GPU limits (c105: higher CPU QoS made frames 6 ms worse); with audio
                  // there, frames were 0.3-2.9 ms shorter and display 1-4 ms in three in-run A/Bs, audio_starved 0 in every phase.
                  // files/private-trace/ios-qos-audio-keep (or RECOMP_IOS_SWITCHES_*) keeps the default QoS; checked every 4096 accesses.
                    static std::atomic<unsigned> calls{0};static std::atomic<int> want{1};thread_local int applied=0;
                    if((calls.fetch_add(1,std::memory_order_relaxed)&4095u)==0) {
                        static const std::string path=std::string(std::getenv("HOME")?std::getenv("HOME"):"")+"/Library/r/private-trace/ios-qos-audio-keep";
                        want.store(recomp_ios_switches_access(path.c_str(),F_OK)==0?0:1,std::memory_order_relaxed);
                    }
                    std::uint64_t self=0;pthread_threadid_np(nullptr,&self);
                    if(recomp::bootstrap::currentGuestThreadToken()==0x64141000u) // (the audio guest thread's host port: slow-frame CPU)
                        recomp_ios_audio_port.store(unsigned(pthread_mach_thread_np(pthread_self())),std::memory_order_relaxed);
                    // (c128: the XMA callback also runs on the main loop's host thread, at start-up and later; the first version
                    // demoted MAIN there. The main thread's QoS belongs to Main.mm's pad-poll hook.)
                    if(self!=recomp_ios_main_tid.load(std::memory_order_relaxed))
                    if(const int w=want.load(std::memory_order_relaxed);w!=applied) {
                        applied=w;const int r=pthread_set_qos_class_self_np(w?QOS_CLASS_UTILITY:QOS_CLASS_DEFAULT,0);
                        std::fprintf(stderr,"APPLE_IOS_QOS thread=audio tid=%llu want=%d result=%d\n",(unsigned long long)self,w,r);
                    }
                }
#endif
                const auto accepted=nativeXma->access(address,write,value);
                if (!accepted) {
                    char words[160]{};
                    for (unsigned i=0;i<16;++i) std::snprintf(words+i*9,10,"%08x,",nativeXma->failureContext()[i]);
                    __android_log_print(ANDROID_LOG_INFO,"RecompNativeXma","STOP address=%08x write=%u value=%08x reason=%s context=%s",
                        address,unsigned(write),value,nativeXma->failure().c_str(),words);
                    std::cerr<<"native XMA stopped: "<<nativeXma->failure()<<" context=";
                    for (auto word:nativeXma->failureContext()) std::cerr<<std::hex<<word<<',';
                    std::cerr<<std::dec<<"\n";
                }
                return accepted;
            })) return 2;
    }
#endif
    if (hasTitleEnvironment
            && !registry.bindExecutableXexHeader(
                    static_cast<std::uint32_t>(titleValues[2]))) {
        std::cerr << "trace setup failed: executable XEX header binding rejected\n";
        return 2;
    }
    if (hasThreadEnvironment
            && !registry.bindGuestThreadTlsStorage(
                    guestThreadToken,
                    threadEnvironment.dynamicTlsAddress(),
                    static_cast<std::uint32_t>(threadValues[6]))) {
        std::cerr << "trace setup failed: dynamic TLS binding rejected\n";
        return 2;
    }

    if (hasThreadPool) {
        const auto stackDefault = recomp::bootstrap::GuestXexHeader::lookup(
                addressSpace, static_cast<std::uint32_t>(titleValues[2]), 0x00020200U);
        if (!stackDefault.found()
                || stackDefault.kind != recomp::bootstrap::GuestXexHeaderValueKind::Immediate) {
            std::cerr << "trace setup failed: title default stack metadata is unavailable\n";
            return 2;
        }
        recomp::bootstrap::GuestThreadStorageConfig config{};
        config.arena = {static_cast<std::uint32_t>(threadPoolValues[0]),
                static_cast<std::uint32_t>(threadPoolValues[1])};
        config.image = {imageBase, static_cast<std::uint32_t>(titleValues[1])};
        config.code = {PPC_CODE_BASE, PPC_CODE_SIZE};
        config.tlsRawDataAddress = static_cast<std::uint32_t>(threadValues[3]);
        config.tlsRawDataSize = static_cast<std::uint32_t>(threadValues[4]);
        config.tlsDataSize = static_cast<std::uint32_t>(threadValues[5]);
        config.tlsSlotCount = static_cast<std::uint32_t>(threadValues[6]);
        config.defaultStackSize = stackDefault.value;
        const std::vector<recomp::bootstrap::GuestThreadMemoryRange> excluded{
                {stackBase, stackSize},
                {static_cast<std::uint32_t>(titleValues[2]), static_cast<std::uint32_t>(titleValues[3])},
                {static_cast<std::uint32_t>(titleValues[5]), 4},
                {static_cast<std::uint32_t>(titleValues[6]), ModuleStructureSize},
                {static_cast<std::uint32_t>(threadValues[0]), recomp::bootstrap::GuestThreadEnvironment::PcrSize},
                {static_cast<std::uint32_t>(threadValues[1]), recomp::bootstrap::GuestThreadEnvironment::ThreadSize},
        };
        if (!registry.bindThreadStorageArena(config, excluded,
                    recomp::bootstrap::hasExactGuestFunction)) {
            std::cerr << "trace setup failed: created-thread arena rejected\n";
            return 2;
        }
    }

    if (hasThreadType && (!registry.bindThreadObjectType(static_cast<std::uint32_t>(threadTypeValues[1]))
            || !registry.bindInitialThreadObject(guestThreadToken, 1))) {
        std::cerr << "trace setup failed: thread object binding rejected\n";
        return 2;
    }

    if (hasNativeXam && !registry.bindNativeXamModule(static_cast<std::uint32_t>(nativeXamAddress),
            static_cast<std::uint32_t>(titleValues[6]), {{stackBase, stackSize},
                {static_cast<std::uint32_t>(titleValues[2]), static_cast<std::uint32_t>(titleValues[3])},
                {static_cast<std::uint32_t>(titleValues[5]), 4},
                {static_cast<std::uint32_t>(threadValues[0]), recomp::bootstrap::GuestThreadEnvironment::PcrSize},
                {static_cast<std::uint32_t>(threadValues[1]), recomp::bootstrap::GuestThreadEnvironment::ThreadSize},
                {static_cast<std::uint32_t>(threadValues[2]), threadEnvironment.tlsStorageSize()}},
                static_cast<std::uint32_t>(nativeXamVersion))) {
        std::cerr << "trace setup failed: native XAM module binding rejected\n";
        return 2;
    }
    if (hasInstalledContent) {
        if (!hasTitleEnvironment) { std::cerr << "trace setup failed: content requires title metadata\n"; return 2; }
        const auto execution = recomp::bootstrap::GuestXexHeader::lookup(
                addressSpace, static_cast<std::uint32_t>(titleValues[2]), 0x00040006);
        if (!execution.found() || execution.kind != recomp::bootstrap::GuestXexHeaderValueKind::DataPointer
                || execution.value < titleValues[2] || std::uint64_t{execution.value} + 24 > titleValues[2] + titleValues[3]) {
            std::cerr << "trace setup failed: content title identity unavailable\n"; return 2;
        }
        const auto* p = addressSpace.translate(execution.value + 12);
        const auto titleId = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
        if (!registry.bindInstalledContent(installedContent, titleId)) {
            std::cerr << "trace setup failed: content title binding rejected\n"; return 2;
        }
        for (const auto& [index, package] : installedPackages) {
            if (!registry.bindContentPackage(index, package.metadata, package.files)) {
                std::cerr << "trace setup failed: package requires complete matching preopened sources\n"; return 2;
            }
        }
    }
    PPCContext context{};
    recomp::bootstrap::GuestDataVariableBinding hardwareInfo;
    recomp::bootstrap::GuestDataVariableBinding kernelVersionBinding;
    recomp::bootstrap::GuestDataVariableBinding uptimeBinding;
    recomp::bootstrap::GuestDataVariableBinding commandLineBinding;
    recomp::bootstrap::GuestDataVariableBinding debugMonitorBinding;
    recomp::bootstrap::GuestDataVariableBinding certMonitorBinding;
    recomp::bootstrap::GuestDataVariableBinding hsioLockBinding;
    recomp::bootstrap::GuestDataVariableBinding scalerClockBinding;
    recomp::bootstrap::GuestVideoDeviceGlobals videoGlobals;
    std::vector<recomp::bootstrap::GuestDataVariableBinding::Range> hardwareExcluded;
    std::vector<recomp::bootstrap::GuestVideoDeviceGlobals::Range> videoExcluded;
    for (auto range : otherDataRanges) if (range.size) {
        hardwareExcluded.push_back({static_cast<std::uint32_t>(range.base), range.size});
        videoExcluded.push_back({static_cast<std::uint32_t>(range.base), range.size});
    }
    if (hasVideoGlobals) hardwareExcluded.push_back({static_cast<std::uint32_t>(videoGlobalValues[2]), 8});
    if (hasHardwareInfo) videoExcluded.push_back({static_cast<std::uint32_t>(hardwareInfoValues[1]), 16});
    if (hasHardwareInfo && !hardwareInfo.initialize(addressSpace, {imageBase, imageSize},
            static_cast<std::uint32_t>(hardwareInfoValues[0]), 0x156,
            static_cast<std::uint32_t>(hardwareInfoValues[1]),
            {0,0,0,0x20,6,0,0,0,0,0,0,0,0,0,0,0}, hardwareExcluded)) {
        std::cerr << "trace setup failed: virtual hardware-info binding rejected\n"; return 2;
    }
    if (hasVideoGlobals && !videoGlobals.initialize(addressSpace,
            {imageBase, static_cast<std::uint32_t>(imageSize), static_cast<std::uint32_t>(videoGlobalValues[0]),
             static_cast<std::uint32_t>(videoGlobalValues[1]), static_cast<std::uint32_t>(videoGlobalValues[2])}, videoExcluded)) {
        std::cerr << "trace setup failed: video pointer variables rejected\n"; return 2;
    }
    if (hasUptime) {
        // The full request was checked against every other range above. This
        // binding reserves 24 bytes but only the observed +16 word is readable.
        if (!uptimeBinding.initialize(addressSpace, {imageBase, imageSize},
                static_cast<std::uint32_t>(uptimeValues[0]), 0xAD, static_cast<std::uint32_t>(uptimeValues[1]),
                std::vector<std::uint8_t>(24), hardwareExcluded)) {
            std::cerr << "trace setup failed: uptime data binding rejected\n"; return 2;
        }
        const auto uptimeStart = std::chrono::steady_clock::now();
        if (!recomp::bootstrap::bindGuestScalarUptimeClock(static_cast<std::uint32_t>(uptimeValues[1]), [uptimeStart]() -> std::optional<std::uint64_t> {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - uptimeStart).count();
                return elapsed < 0 ? std::nullopt : std::optional<std::uint64_t>(static_cast<std::uint64_t>(elapsed));
            })) return 2;
    }
    if (hasCommandLine) {
        constexpr std::string_view name = "\"default.xex\"";
        std::vector<std::uint8_t> command(64, 0);
        std::copy(name.begin(), name.end(), command.begin());
        if (!commandLineBinding.initialize(addressSpace, {imageBase, imageSize},
                static_cast<std::uint32_t>(commandLineValues[0]), 0x1AE, static_cast<std::uint32_t>(commandLineValues[1]),
                command, hardwareExcluded)) {
            std::cerr << "trace setup failed: launch command line binding rejected\n"; return 2;
        }
    }
    // DATA import -> distinct pointer variable -> null: there is no attached
    // Xbox remote debugger in this native profile. Never replace the slot with
    // null directly, invent a monitor object, or install a callback trampoline.
    if (hasNoDebugMonitor && !debugMonitorBinding.initialize(addressSpace, {imageBase, imageSize},
            static_cast<std::uint32_t>(debugMonitorValues[0]), 0x59,
            static_cast<std::uint32_t>(debugMonitorValues[1]), {0, 0, 0, 0}, hardwareExcluded)) {
        std::cerr << "trace setup failed: no-debug-monitor data binding rejected\n"; return 2;
    }
    // The certification monitor is an optional developer callback service.
    // DATA slot -> a distinct four-byte pointer variable -> null represents
    // its absence. Do not map the descriptor to null or invent a callback.
    if (hasNoCertMonitor && !certMonitorBinding.initialize(addressSpace, {imageBase,imageSize},
            static_cast<std::uint32_t>(certMonitorValues[0]),0x266,
            static_cast<std::uint32_t>(certMonitorValues[1]),{0,0,0,0},hardwareExcluded)) {
        std::cerr << "trace setup failed: no-cert-monitor data binding rejected\n"; return 2;
    }
    // Unlike monitor pointers, this export IS the critical-section object.
    // Use the actual registry synchronization implementation, not zero storage
    // mistaken for a valid unlocked lock. No claim of hardware calibration.
    if (hasHsioLock) {
        if (!hsioLockBinding.initialize(addressSpace,{imageBase,imageSize},
                static_cast<std::uint32_t>(hsioLockValues[0]),0x1C1,
                static_cast<std::uint32_t>(hsioLockValues[1]),std::vector<std::uint8_t>(28),hardwareExcluded)) {
            std::cerr << "trace setup failed: HSIO lock data binding rejected\n"; return 2;
        }
        recomp::bootstrap::GuestImportCall init;
        init.arguments[0] = hsioLockValues[1]; init.arguments[1] = 10000;
        if (!registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",init).handled() || init.returnValue != 0) {
            std::cerr << "trace setup failed: HSIO critical-section initialization rejected\n"; return 2;
        }
    }
    if(nativeScalerTiming){
        if(!hasVirtualVideoMode || std::count(unboundDataSlots.begin(),unboundDataSlots.end(),scalerTimingValues[0])!=1)return 2;
        const auto& payload=recomp::gpu::NativeScalerTiming::GpuClockBytes;
        if(!scalerClockBinding.initialize(addressSpace,{imageBase,imageSize},static_cast<std::uint32_t>(scalerTimingValues[0]),0x1C0,
            static_cast<std::uint32_t>(scalerTimingValues[1]),std::vector<std::uint8_t>(payload.begin(),payload.end()),hardwareExcluded))return 2;
        unboundDataSlots.erase(std::find(unboundDataSlots.begin(),unboundDataSlots.end(),scalerTimingValues[0]));
    }
    if (hasKernelVersion) {
        const auto slot = static_cast<std::uint32_t>(kernelVersionValues[0]);
        const auto payload = recomp::bootstrap::guestKernelVersionBytes(kernelVersionValues[2]);
        if (!hasNativeXam || !payload ||
            std::count(unboundDataSlots.begin(), unboundDataSlots.end(), slot) != 1 ||
            !kernelVersionBinding.initialize(addressSpace, {imageBase, imageSize}, slot,
                0x158, static_cast<std::uint32_t>(kernelVersionValues[1]),
                std::vector<std::uint8_t>(payload->begin(), payload->end()), hardwareExcluded)) {
            std::cerr << "trace setup failed: native kernel-version profile rejected\n";
            return 2;
        }
        unboundDataSlots.erase(std::find(unboundDataSlots.begin(), unboundDataSlots.end(), slot));
    }
    if (std::any_of(unboundDataSlots.begin(), unboundDataSlots.end(), [&](auto slot) {
            return !hasTitleEnvironment || slot < imageBase || std::uint64_t{slot} + 4 > imageBase + imageSize;
        }) || !recomp::bootstrap::bindGuestScalarDataImportGuard(unboundDataSlots)) {
        std::cerr << "trace setup failed: invalid unresolved data-import guard\n"; return 2;
    }
    if (!registry.bindTitleTerminationNotifications(static_cast<std::uint32_t>(imageBase),
            static_cast<std::uint32_t>(imageSize),
            [](auto callback) { return recomp::bootstrap::hasExactGuestFunction(callback); })) {
        std::cerr << "trace setup failed: title termination metadata binding rejected\n"; return 2;
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
#if defined(__APPLE__)
    auto nativeAudio = experimentalNativeAudio ? std::make_shared<recomp::audio::AppleAudioOutputStream>() : nullptr;
#else
    auto nativeAudio = experimentalNativeAudio ? std::make_shared<recomp::audio::AAudioOutputStream>() : nullptr;
#endif
    // audio-ahead: the render client and the registry see the stream through AheadBackend (pass-through; capacity reported
    // + 4 packets, so diag-au-ahead can toggle in-run); the worker's gate keeps v431's demand rule unless the switch is on.
    const auto auAheadBackend = experimentalNativeAudio && experimentalNativeAudioRenderer
        ? std::make_shared<recomp_au_ahead::AheadBackend>(nativeAudio) : nullptr;
    if (experimentalNativeAudio && (!nativeAudio->open() || !registry.bindExperimentalAudioOutput(
            auAheadBackend ? std::shared_ptr<recomp::bootstrap::GuestAudioBackend>(auAheadBackend)
                           : std::shared_ptr<recomp::bootstrap::GuestAudioBackend>(nativeAudio)))) {
        std::cerr << "trace setup failed: native audio stream binding rejected\n"; return 2;
    }
    auto nativeAudioRenderer = experimentalNativeAudioRenderer
        ?std::make_shared<recomp::audio::NativeAudioRenderClient>(auAheadBackend):nullptr;
    auto nativeGraphics = hasGraphics ? std::make_shared<recomp::gpu::VulkanEngine>() : nullptr;
    if (hasGraphics && !registry.bindGraphicsBackend(nativeGraphics,
            static_cast<std::uint32_t>(imageBase), static_cast<std::uint32_t>(imageSize),
            [](auto callback) { return recomp::bootstrap::hasExactGuestFunction(callback); })) {
        std::cerr << "trace setup failed: native graphics backend binding rejected\n"; return 2;
    }
    // v255 (direct_present.h): the presenter uses the engine's device once it is verified; it lets go of the window and
    // its Vulkan objects before the engine (declared earlier) is destroyed.
    if (nativeGraphics) recomp::gpu::direct::presenter().setDeviceSource([g = nativeGraphics.get()] { return g->diagnosticDevice(); });
    struct DirectPresentShutdown { ~DirectPresentShutdown() { recomp::gpu::direct::presenter().shutdown(); } } directPresentShutdown;
#endif
#if (defined(__ANDROID__) || defined(__APPLE__))
    // Attempt v43: one display path to the app for the whole run. The synchronous display of a frame (the
    // v35 inlinePresent body: scan-out mapping, then the app's ordered callback), run either on the
    // presenter thread for queued generic frames or here, after every queued frame, for everything else.
    // Declared after liveCallbacks/nativeGraphics and before every binding that uses it; its destructor
    // delivers a still-queued frame and joins the thread before those are destroyed.
    const auto displayLive=[&](const std::vector<std::uint8_t>& rgba,unsigned width,unsigned height) {
        if(!liveCallbacks || !liveCallbacks->orderedRgba || !nativeGraphics ||
            !nativeGraphics->nativeScalerConfigured())return false;
        static thread_local std::vector<std::uint8_t> pixels; // v227: reused per display thread (no 3.7 MB allocation per frame)
        static thread_local scanout_map_timing::Stats mapTiming;
        const bool mapped=scanout_map_timing::measure(mapTiming,width,height,[&] {
            return nativeGraphics->mapNativeScanout(rgba.data(),rgba.size(),width,height,pixels);
        },[](const scanout_map_timing::Snapshot& s) {
            std::uint64_t thread=0;bool threadValid=false;
#if defined(__APPLE__)
            threadValid=pthread_threadid_np(nullptr,&thread)==0;
#endif
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_SCANOUT_MAP clock=std-steady-unjoined source=displayLive thread=%llu thread_valid=%u calls=%llu ok=%llu fail=%llu ns=%llu invalid=%llu interval_calls=%llu interval_ok=%llu interval_fail=%llu interval_ns=%llu interval_invalid=%llu width=%u height=%u dimensions_consistent=%u interval_consistent=%u ns_valid_only=1",
                (unsigned long long)thread,unsigned(threadValid),
                (unsigned long long)s.total.calls,(unsigned long long)s.total.ok,(unsigned long long)s.total.fail,(unsigned long long)s.total.ns,(unsigned long long)s.total.invalid,
                (unsigned long long)s.interval.calls,(unsigned long long)s.interval.ok,(unsigned long long)s.interval.fail,(unsigned long long)s.interval.ns,(unsigned long long)s.interval.invalid,
                s.width,s.height,unsigned(s.dimensionsConsistent),unsigned(s.intervalConsistent));
        });
        return mapped && liveCallbacks->orderedRgba(liveCallbacks->user,pixels.data(),
                static_cast<std::uint32_t>(pixels.size()),width,height)==1;
    };
    recomp::diag::AsyncFramePresenter framePresenter(displayLive,[](const char* what,std::uint64_t sequence) {
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PRESENTER event=%s queued_frame=%llu later_frames=synchronous",
            what,static_cast<unsigned long long>(sequence));
    });
#else
    recomp::diag::AsyncFramePresenter framePresenter([](const std::vector<std::uint8_t>&,unsigned,unsigned){return false;});
#endif
    // Producer-side mapping remains under the original native ownership. Only
    // owned mapped bytes and a leased GPU source cross to the new worker.
    std::unique_ptr<recomp::ios_async_shared::Dispatcher> iosDispatcher;
    struct IosSharedBinding {
        recomp::diag::AsyncFramePresenter& presenter;
        std::unique_ptr<recomp::ios_async_shared::Dispatcher>& dispatcher;
        const std::thread::id owner=std::this_thread::get_id();
        ~IosSharedBinding(){
            if(!iosSharedDisplay)return;
            if(owner!=std::this_thread::get_id())std::terminate();
#if defined(__APPLE__)
            if(pthread_main_np())std::terminate();
#endif
            auto control=recomp::ios_async_shared::control.exchange(nullptr);
            if(control){control->close();control->drain();} // no new preparation; owned snapshots safe
            presenter.drain();
            if(dispatcher){
                dispatcher->closeAndJoin(); // no source/native/renderer lock
                const auto c=dispatcher->counts();
                std::fprintf(stderr,"APPLE_IOS_ASYNC_SHARED_STOP submitted=%llu replaced=%llu accepted=%llu cancelled_unproven=%llu failed=%llu quarantined=%llu published_while_active=%llu\n",
                    (unsigned long long)c.submitted,(unsigned long long)c.replaced,(unsigned long long)c.completed,(unsigned long long)c.cancelled,(unsigned long long)c.failed,(unsigned long long)c.quarantined,(unsigned long long)c.publishedWhileActive);
            }
            {
                std::lock_guard sourceLease(recomp::ios_shared::producerGate);
                recomp::ios_shared::producer=nullptr;recomp::ios_shared::producerUser=nullptr;
            }
            iosSharedDisplay->close(std::uncaught_exceptions()!=0);
            if(const auto retire=recomp::ios_async_shared::retireSources.load())
                if(!retire())std::_Exit(4);
            recomp::ios_async_shared::retireSources=nullptr;
        }
    };
#if defined(__APPLE__)
    std::atomic<bool> iosScanoutReady{false};
    recomp::gpu::direct::Lut iosNativeLut{};bool iosNativeLutReady=false;
    std::vector<uint8_t> iosMappedReference;
    std::function<bool(RecompIosGpuFrameV1&,const uint8_t*,uint64_t,std::vector<uint8_t>&)> iosPrepareMap;
    std::function<int(RecompIosGpuFrameV1&,const uint8_t*,uint64_t)> iosSharedMap;
#endif
    IosSharedBinding iosSharedBinding{framePresenter,iosDispatcher};
#if defined(__APPLE__)
    if(iosSharedDisplay) {
        iosPrepareMap=[&](RecompIosGpuFrameV1& f,const uint8_t* raw,uint64_t bytes,std::vector<uint8_t>& mapped) {
            if(f.sourceFenceComplete!=1)return false;
            if(!iosScanoutReady.load(std::memory_order_acquire))return false;
            auto& lut=iosNativeLut;auto& lutReady=iosNativeLutReady;
            if(!lutReady) {
                const auto ramp=recomp::gpu::direct::rampSource();std::vector<uint8_t> mappedRamp;
                lutReady=nativeGraphics->mapNativeScanout(ramp.data(),ramp.size(),1280,720,mappedRamp) &&
                    recomp::gpu::direct::lutFromMapped(mappedRamp.data(),mappedRamp.size(),lut);
            }
            if(!lutReady || !raw || bytes!=1280ULL*720*4 ||
               !nativeGraphics->mapNativeScanout(raw,bytes,1280,720,mapped) || mapped.size()!=bytes)return false;
            f.mappedRgba=mapped.data();f.mappedBytes=mapped.size();f.lutGeneration=1;
            std::memcpy(f.nativeLut,lut.data(),sizeof(f.nativeLut));return true;
        };
        iosSharedMap=[&](RecompIosGpuFrameV1& f,const uint8_t* raw,uint64_t bytes) {
            if(f.sourceFenceComplete!=1)iosSharedDisplay->fail();
            if(!iosScanoutReady.load(std::memory_order_acquire)) {
                if(iosSharedDisplay->adopted())iosSharedDisplay->fail();
                return int(RECOMP_IOS_GPU_INELIGIBLE);
            }
            if(!iosPrepareMap(f,raw,bytes,iosMappedReference))iosSharedDisplay->fail();
            const int result=iosSharedDisplay->gpu(f);
            if(result==RECOMP_IOS_GPU_ACCEPTED)recomp::ios_async_shared::adopted.store(true,std::memory_order_release);
            return result;
        };
        recomp::ios_shared::producerUser=&iosSharedMap;
        recomp::ios_shared::producer=[](void* user,RecompIosGpuFrameV1& f,const uint8_t* raw,uint64_t bytes) {
            return (*static_cast<std::function<int(RecompIosGpuFrameV1&,const uint8_t*,uint64_t)>*>(user))(f,raw,bytes);
        };
        if(recomp::ios_async_shared::enabled) {
            iosDispatcher=std::make_unique<recomp::ios_async_shared::Dispatcher>(
                [&](RecompIosGpuFrameV1& f,const std::vector<uint8_t>& raw,std::vector<uint8_t>& mapped){return iosPrepareMap(f,raw.data(),raw.size(),mapped);},
                [](RecompIosGpuFrameV1& f){return iosSharedDisplay->gpu(f);},
                []{iosSharedDisplay->fail();},
                [&](const RecompIosGpuFrameV1& f,recomp::ios_async_shared::Mailbox::Token token,bool accepted,bool cancelled){
                    const auto c=iosDispatcher->counts();
                    if(!accepted || c.completed<=4 || c.completed%60==0)
                        std::fprintf(stderr,"APPLE_IOS_ASYNC_SHARED_DELIVERY sequence=%u source_frame=%llu source_generation=%llu slot=%u token_generation=%llu epoch=%llu accepted=%u cancelled=%u submitted=%llu replaced=%llu published_while_active=%llu\n",
                            f.sequence,(unsigned long long)f.sourceFrame,(unsigned long long)f.sourceGeneration,token.slot,
                            (unsigned long long)token.generation,(unsigned long long)token.epoch,unsigned(accepted),unsigned(cancelled),
                            (unsigned long long)c.submitted,(unsigned long long)c.replaced,(unsigned long long)c.publishedWhileActive);
                });
            auto* dispatcher=iosDispatcher.get();
            recomp::ios_async_shared::control.store(std::make_shared<recomp::ios_async_shared::Control>(
                [dispatcher](std::shared_ptr<recomp::ios_async_shared::Mailbox> box,
                    recomp::ios_async_shared::Mailbox::Producer&& producer,RecompIosGpuFrameV1 f,std::vector<uint8_t>&& raw){
                    return dispatcher->submit(std::move(box),std::move(producer),f,std::move(raw));}));
        }
    }
#endif
    std::uint32_t lastFileMetadataIndex = 0;
    // script-mods: the original file of a guest path (the read-only binding below), and the view the guest gets instead.
    const auto openOriginal = [&](std::string_view path) -> std::shared_ptr<const recomp::bootstrap::GuestReadOnlyFile> {
        if (nativeGameCache && nativeGameCache->owns(path)) return nativeGameCache->open(path);
        if (nativeUpdate && nativeUpdate->owns(path)) return nativeUpdate->open(path);
        if (nativeCache && nativeCache->owns(path)) return nativeCache->open(path);
        const auto found = fileSources.find(std::string(path));
        return found == fileSources.end() ? nullptr : found->second;
    };
    recomp_mods::setup();
    if ((hasFileMetadata || nativeCache || nativeUpdate || nativeGameCache) && !registry.bindFileMetadataQuery([&](auto path) {
        auto result = [&] {
        if (nativeGameCache && nativeGameCache->owns(path)) {
            lastFileMetadataIndex = 0;
            return nativeGameCache->query(path);
        }
        if (nativeUpdate && nativeUpdate->owns(path)) {
            lastFileMetadataIndex = 0;
            return nativeUpdate->query(path);
        }
        if (nativeCache && nativeCache->owns(path)) {
            lastFileMetadataIndex = 0;
            return nativeCache->query(path);
        }
        lastFileMetadataIndex = fileMetadata.recordIndex(path);
        return fileMetadata.query(path);
        }();
        // script-mods: a patched archive (or a replaced file) has its own size; the open below must find the same size.
        if (result.state == recomp::bootstrap::GuestFileQueryState::Found && result.metadata.attributes == 1
                && recomp_mods::layer().covers(path)) {
            if (const auto original = openOriginal(path)) {
                const auto view = recomp_mods::view(path, original, addressSpace.base());
                if (view != original) {
                    result.metadata.values[5] = view->size();
                    result.metadata.values[4] = std::max<std::uint64_t>(result.metadata.values[4], view->size());
                }
            }
        }
        return result;
    })) {
        std::cerr << "trace setup failed: file metadata binding rejected\n";
        return 2;
    }
    if ((!fileSources.empty() || nativeCache || nativeUpdate || nativeGameCache) && !registry.bindReadOnlyFiles([&](auto path) -> std::shared_ptr<const recomp::bootstrap::GuestReadOnlyFile> {
        return recomp_mods::view(path, openOriginal(path), addressSpace.base()); // (the original unless a mod covers it)
    })) { std::cerr << "trace setup failed: read-only content binding rejected\n"; return 2; }
    context.r1.u64 = static_cast<std::uint64_t>(stackBase) + stackSize;
    if (hasThreadEnvironment) {
        context.r13.u64 = threadValues[0];
    }

    if (experimentalGpuClock) {
        const auto epoch = std::chrono::steady_clock::now();
        const auto elapsed=[epoch]() -> std::optional<std::uint64_t> {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - epoch).count();
            if (ns < 0) return std::nullopt;
            return static_cast<std::uint64_t>(ns);
        };
        if (!recomp::bootstrap::bindExperimentalGuestGpuClock([&,elapsed]() -> std::optional<std::uint64_t> {
            if (!registry.graphicsInitialized()) return std::nullopt;
            return elapsed();
        }))return 2;
#if (defined(__ANDROID__) || defined(__APPLE__))
        // Backend calls this under the registry mutex. Share the pure epoch,
        // not the MMIO readiness wrapper which would reenter that mutex.
        if(nativeSwapCommands && (!nativeGraphics || !nativeGraphics->bindNativeSwapCompletionClock(elapsed)))return 2;
#endif
    }
    const auto submitOwnedColor=[&](std::uint32_t word){
#if (defined(__ANDROID__) || defined(__APPLE__))
        if(!experimentalOwnedColor || !nativeGraphics || !inspectColorProgram)return false;
        const auto& state=nativeGraphics->stateBatchSnapshot();
        const recomp::gpu::DrawBinState bins{{{static_cast<std::uint32_t>(state.binMask),static_cast<std::uint32_t>(state.binMask>>32),
            static_cast<std::uint32_t>(state.binSelect),static_cast<std::uint32_t>(state.binSelect>>32)}},state.knownBinWords};
        if(registry.submitExperimentalOwnedColorBatch(*inspectColorProgram,bins,word) ||
            (livePostOutputColor && registry.submitExperimentalPostOutputColorBatch(*inspectColorProgram,bins,word)))return true;
        // Synchronous executor-token scope: every other guest writer is parked.
        // A successful PREFIX does not discharge the enclosing swap/IRQ tail.
        // Keep returning false so the ordinary scalar guard stops at that tail.
        if(liveDeferredColor)(void)registry.executeExperimentalDeferredColorPrefix(*inspectColorProgram,bins,word);
        return false;
#else
        (void)word;return false;
#endif
    };
    std::function<bool(std::uint32_t)> submitScalerInterrupt,submitPostOutputSwap;
    GraphicsSubmissionDiagnostic submissionDiagnostic;
    // Attempt v44: where the CP_RB_WPTR handler spends its time, per submitter (the D3D render thread
    // 6408e000, the CPU2 tile worker 640b1000, anyone else), logged every 10 s as
    // RENDER_DIAG_SUBMIT_TIMING. Only the handler touches it, inside the graphics-submission exclusion.
    using SubmitClock=std::chrono::steady_clock;
    struct SubmitTimingWho {
        unsigned long long n=0,handlerNs=0,handlerMaxNs=0,maintCalled=0,maintSkipped=0,maintNs=0,retireN=0,retireNs=0,
            walkNs=0,walkMaxNs=0,words=0,rWaitNs=0,rHoldNs=0,rHoldMaxNs=0,irqs=0,irqNs=0,rcsN=0,rcsNs=0,rcsMaxNs=0,logNs=0;
    };
    std::array<SubmitTimingWho,3> submitTiming{};
    SubmitClock::time_point submitTimingSince=SubmitClock::now();
    // Attempt v110: pre-fences. Runtime command-storage coherency holds the registry lock R across
    // VulkanEngine::fenceNativeQueue, and every tile-scene IRQ runs that fence inside its exclusive handoff
    // (the world stopped). Both fences wait for everything on the queue the generic renderer shares. A
    // vkQueueWaitIdle first, on the same submitting thread (domain 2, inside the graphics-submission
    // exclusion or the renderer hook that would fence next), with R free and the world running, leaves the
    // original fence an idle queue: its semantics (a fresh fence, in place, after all prior work) are
    // unchanged; only where the GPU wait happens moves. diag-rcs-no-prefence / diag-irq-no-prefence
    // restore v109. RENDER_DIAG_PREFENCE (with RENDER_DIAG_SUBMIT_TIMING) proves they ran.
    struct PrefenceTiming {std::atomic<unsigned long long> n{0},ns{0},maxNs{0};};
    std::array<PrefenceTiming,2> prefenceTiming{}; // [0] rcs, [1] irq
    std::atomic<unsigned long long> rcsPrefenceMissed{0}; // pre-fenced, then rcs refused the word
    const bool rcsPrefence=::access(RECOMP_APP_PATH("/private-trace/diag-rcs-no-prefence"),F_OK)!=0;
    const bool irqPrefence=::access(RECOMP_APP_PATH("/private-trace/diag-irq-no-prefence"),F_OK)!=0;
    const auto prefenceNativeQueue=[&](unsigned kind) {
        if(!(kind?irqPrefence:rcsPrefence) || !nativeGraphics)return;
        auto* device=nativeGraphics->diagnosticDevice();
        if(!device || device->queue==VK_NULL_HANDLE)return;
        // v170 (H2b): the pre-fence only moves the native fence's wait ahead of R; skip it exactly when that fence
        // will be skipped (no native submission since the last completed native fence).
        static const bool fenceOnly=::access(RECOMP_APP_PATH("/private-trace/diag-native-fence-only"),F_OK)==0;
        if(fenceOnly && recomp::gpu::nativeSubmissionTickets().load(std::memory_order_acquire)==
                        recomp::gpu::nativeFencedTickets().load(std::memory_order_acquire))return;
        const auto ticketsBefore=recomp::gpu::nativeSubmissionTickets().load(std::memory_order_acquire);
        const auto started=SubmitClock::now();
        (void)recomp::gpu::lockedQueueWaitIdle(device->device,device->queue); // v119: fence wait outside the queue lock; an error leaves the original fence to report it
        recomp::gpu::noteNativeTicketsFenced(ticketsBefore); // v170: the idle wait covered every native ticket issued before it
        const auto ns=static_cast<unsigned long long>(std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::nanoseconds>(SubmitClock::now()-started).count()));
        auto& e=prefenceTiming[kind];
        e.n.fetch_add(1,std::memory_order_relaxed);e.ns.fetch_add(ns,std::memory_order_relaxed);
        auto seen=e.maxNs.load(std::memory_order_relaxed);
        while(ns>seen && !e.maxNs.compare_exchange_weak(seen,ns,std::memory_order_relaxed)){}
    };
    // The shape submitNativeStorageCoherencyLocked(Runtime) accepts first: the 3 ring words after the
    // current read word are an 11-word indirect buffer. A cheap filter so only likely runtime-coherency
    // words are pre-fenced (a miss costs one early GPU wait, counted as rcs_missed).
    const auto rcsCandidate=[&](std::uint32_t word)->bool {
        const auto ring=registry.graphicsRingConfiguration();
        if(!ring || !ring->writeBack || std::uint64_t{ring->readWord}+3!=word || word>=ring->bytes/4)return false;
        const auto* root=addressSpace.translate(ring->virtualAddress+ring->readWord*4);
        if(!root)return false;
        const auto be=[](const std::uint8_t* q){return (std::uint32_t{q[0]}<<24)|(std::uint32_t{q[1]}<<16)|(std::uint32_t{q[2]}<<8)|q[3];};
        return be(root)==0xC0013F00u && be(root+8)==11u;
    };
    // v211: deferred scene-interrupt state (see the v211 comment before the submission handler's binding).
    struct DeferredIrq {bool pending=false;std::uint32_t owner=0;std::uint64_t marker=0;SubmitClock::time_point at{};
        diagnostic_skip::DeferredInterrupts d;std::size_t applied=0;}; // d.stores[0, applied) were queued with the marker
    static DeferredIrq deferredIrq;static std::mutex deferredIrqMutex;static std::atomic<bool> deferredIrqPending{false};
    static std::atomic<std::uint64_t> deferredIrqMarker{0}; // v233: deferredIrq.marker, stored before deferredIrqPending is set
    struct DeferredIrqStats {std::atomic<unsigned long long> deferred{0},delivered{0},atImport{0},atMust{0},drained{0},drainNs{0},
        notOpen{0},waitedOpen{0},refused{0},failed{0},latencyNs{0},latencyMaxNs{0},skipped{0};};
    static DeferredIrqStats deferredIrqStats;
    const auto irqDeferredOn=[]{
        static std::atomic<long long> checkedAt{0};static std::atomic<bool> on{false};
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(SubmitClock::now().time_since_epoch()).count();
        if(now-checkedAt.load(std::memory_order_relaxed)>=1000) {
            checkedAt.store(now,std::memory_order_relaxed);
            on.store(::access(RECOMP_APP_PATH("/private-trace/diag-irq-deferred"),F_OK)==0,std::memory_order_relaxed);
        }
        return on.load(std::memory_order_relaxed);
    };
    // v110: one tile-scene IRQ (admission scope + worker handoff, executeNativeTileSceneHandoff) after an irq
    // pre-fence, as ONE exclusive span (runNativeExclusiveSpan; diag-irq-split-scopes: v109's two stops).
    // A refused span (no live task, stop observed, inside a scope) runs the handoff exactly as v109 did.
    // v279: a scene IRQ runs inline (executeNativeTileSceneInline: no worker thread, the ISR's effects replayed natively and
    // verified by the registry's scene scope) unless diag-v279-baseline (recomp_v279_worker_isr, set by the renderer's A/B
    // windows) or a held tile lock selects the worker handoff.
    const auto sceneHandoff=[&](recomp::bootstrap::GuestImportRegistry& reg,recomp::bootstrap::GuestCooperativeExecutor& ex,unsigned device,
            const recomp::gpu::Gta5TileCommandInterrupt& request,const recomp::bootstrap::NativeTileScenePacketExecute& execute,
            const recomp::bootstrap::NativeTileSceneGuestInvoke& invoke) {
        if(!recomp_v279_worker_isr.load(std::memory_order_relaxed) && recomp::bootstrap::nativeSceneInlineEligible(addressSpace.base(),device)) {
            v279Inline.fetch_add(1,std::memory_order_relaxed);
            return recomp::bootstrap::executeNativeTileSceneInline(reg,ex,device,request,execute,addressSpace.base(),recomp::bootstrap::currentGuestThreadToken());
        }
        v279Worker.fetch_add(1,std::memory_order_relaxed);
        return recomp::bootstrap::executeNativeTileSceneHandoff(reg,ex,device,request,execute,invoke);
    };
    const auto tileSceneIrq=[&](recomp::bootstrap::GuestCooperativeExecutor& ex,const char* site,const auto& run) {
        prefenceNativeQueue(1);
        ExclusiveSiteLabel label(site);
        std::optional<decltype(run())> result;
        if(!recomp::bootstrap::runNativeExclusiveSpan(ex,[&]{result.emplace(run());}))result.emplace(run());
        return *result;
    };
    const auto submitNs=[](SubmitClock::time_point from,SubmitClock::time_point to)->unsigned long long {
        const auto d=std::chrono::duration_cast<std::chrono::nanoseconds>(to-from).count();return d>0?static_cast<unsigned long long>(d):0ULL;
    };
    const auto logSubmitTiming=[&](SubmitClock::time_point now) {
        static constexpr const char* Names[3]{"render","tile","other"};
        const auto windowMs=submitNs(submitTimingSince,now)/1000000ULL;
        for(unsigned k=0;k<3;++k) {
            const auto& t=submitTiming[k];if(!t.n)continue;
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_SUBMIT_TIMING window_ms=%llu who=%s n=%llu handler_ms=%.2f handler_max_us=%llu maint_called=%llu maint_skipped=%llu "
                "maint_ms=%.2f retire_n=%llu retire_ms=%.2f walk_ms=%.2f walk_max_us=%llu words=%llu r_wait_ms=%.3f r_hold_ms=%.3f "
                "r_hold_max_us=%llu irqs=%llu irq_ms=%.2f rcs_n=%llu rcs_ms=%.2f rcs_max_us=%llu log_ms=%.2f",
                windowMs,Names[k],t.n,t.handlerNs/1e6,t.handlerMaxNs/1000ULL,t.maintCalled,t.maintSkipped,t.maintNs/1e6,t.retireN,
                t.retireNs/1e6,t.walkNs/1e6,t.walkMaxNs/1000ULL,t.words,t.rWaitNs/1e6,t.rHoldNs/1e6,t.rHoldMaxNs/1000ULL,t.irqs,t.irqNs/1e6,
                t.rcsN,t.rcsNs/1e6,t.rcsMaxNs/1000ULL,t.logNs/1e6);
        }
        { // v110 (cumulative since start)
            const auto v=[](const std::atomic<unsigned long long>& a){return a.load(std::memory_order_relaxed);};
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_PREFENCE rcs=%d irq=%d rcs_n=%llu rcs_ms=%.2f rcs_max_us=%llu rcs_missed=%llu irq_n=%llu irq_ms=%.2f irq_max_us=%llu submit_parked=%llu native_tickets=%llu native_fenced=%llu native_fences_skipped=%llu",
                int(rcsPrefence),int(irqPrefence),v(prefenceTiming[0].n),v(prefenceTiming[0].ns)/1e6,v(prefenceTiming[0].maxNs)/1000ULL,
                v(rcsPrefenceMissed),v(prefenceTiming[1].n),v(prefenceTiming[1].ns)/1e6,v(prefenceTiming[1].maxNs)/1000ULL,
                static_cast<unsigned long long>(recomp::bootstrap::guestGraphicsSubmissionParkedEntries()), // v119
                static_cast<unsigned long long>(recomp::gpu::nativeSubmissionTickets().load()),static_cast<unsigned long long>(recomp::gpu::nativeFencedTickets().load()),
                static_cast<unsigned long long>(recomp::gpu::nativeFencesSkipped().load())); // v170
        }
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V279 inline=%llu worker=%llu worker_switch=%u", // (cumulative)
            v279Inline.load(std::memory_order_relaxed),v279Worker.load(std::memory_order_relaxed),unsigned(recomp_v279_worker_isr.load(std::memory_order_relaxed)));
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V284 tail_concurrent=%llu tail_declined=%llu tail_gate_refused=%llu baseline=%u", // (cumulative)
            v284Concurrent.load(std::memory_order_relaxed),v284Declined.load(std::memory_order_relaxed),v284GateRefused.load(std::memory_order_relaxed),
            unsigned(recomp_v284_baseline.load(std::memory_order_relaxed)));
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V282 concurrent=%llu declined=%llu busy_retries=%llu baseline=%u", // (cumulative)
            v282Concurrent.load(std::memory_order_relaxed),v282Declined.load(std::memory_order_relaxed),v282Busy.load(std::memory_order_relaxed),
            unsigned(recomp_v282_baseline.load(std::memory_order_relaxed)));
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_V280 limiter_value=%u applied=%u foreign=%u", // (cumulative)
            v280Last.load(std::memory_order_relaxed),v280Applied.load(std::memory_order_relaxed),v280Foreign.load(std::memory_order_relaxed));
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer", // v183 (cumulative)
            "RENDER_DIAG_DEFERRED_RETIRE deferred=%llu deferred_stores=%llu sync=%llu barriers=%llu store_failures=%llu tile_recording=%u irq_no_drain=%llu",
            diagnostic_skip::deferredSubmissions.load(),diagnostic_skip::deferredStores.load(),diagnostic_skip::syncSubmissions.load(),
            submissionBarriers.load(),deferredStoreFailures.load(),unsigned(tileRecordingSeen.load()),diagnostic_skip::irqNoDrainSubmissions.load());
        { // v211 (cumulative)
            const auto v=[](const std::atomic<unsigned long long>& a){return a.load(std::memory_order_relaxed);};
            const auto delivered=v(deferredIrqStats.delivered);
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_V211 irq_deferred_on=%u deferred=%llu delivered=%llu at_import=%llu at_must=%llu drained=%llu drain_ms=%.2f "
                "not_open=%llu waited_open=%llu refused=%llu failed=%llu skipped=%llu latency_ms=%.3f latency_max_us=%llu retire_deferred=%llu",
                unsigned(irqDeferredOn()),v(deferredIrqStats.deferred),delivered,v(deferredIrqStats.atImport),v(deferredIrqStats.atMust),
                v(deferredIrqStats.drained),v(deferredIrqStats.drainNs)/1e6,v(deferredIrqStats.notOpen),v(deferredIrqStats.waitedOpen),
                v(deferredIrqStats.refused),v(deferredIrqStats.failed),v(deferredIrqStats.skipped),
                delivered?double(v(deferredIrqStats.latencyNs))/1e6/double(delivered):0.0,v(deferredIrqStats.latencyMaxNs)/1000ULL,
                diagnostic_skip::deferredIrqSubmissions.load());
        }
        { // v183: which admission points drained (site numbers in the v183 sources)
            std::string sites;
            for(unsigned s=0;s<32;++s)if(const auto n=recomp::bootstrap::nativeSubmissionCommitSites[s].load(std::memory_order_relaxed)) {
                char b[32];std::snprintf(b,sizeof b," %u:%llu",s,n);sites+=b;}
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DEFERRED_RETIRE_SITES%s v217_quiet_returns=%llu",
                                sites.empty()?" none":sites.c_str(),recomp::bootstrap::v217QuietReturnCount());
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer", // v218: exact totals of the sampled per-event lines
                "RENDER_DIAG_TILE_IRQ_TOTALS ran=%llu refused=%llu failed=%llu submissions=%llu v224_unpinned=%llu v224_baseline=%u "
                "v240_pooled=%llu v240_hosts=%llu v240_fallbacks=%llu v241_stop_yields=%llu",tileIrqTotals.ran.load(),
                tileIrqTotals.refused.load(),tileIrqTotals.failed.load(),submitLines.load(),v224Unpinned.load(),unsigned(recomp_v224_baseline.load()),
                recomp::bootstrap::pooledHostStats().pooled,recomp::bootstrap::pooledHostStats().created,recomp::bootstrap::pooledHostStats().fallbacks,
                recomp::bootstrap::pooledHostStats().stopYields);
        }
        submitTiming={};submitTimingSince=now;
    };
    // v211 (opt-in files/private-trace/diag-irq-deferred, read at most once a second): the render thread's ring
    // submissions carrying one tile-command interrupt no longer drain the renderer and run the interrupt inline (v208 paced
    // drive: ~1 such submission per frame, ~8 ms each, ~6.5 of them the site-20 drain). The completion stores before the
    // interrupt are queued behind the draws they follow (as v183 does for plain submissions); the interrupt, the stores
    // after it and the stream-final bin words are kept here, and the read pointer is published at once. The interrupt runs
    // once the renderer has executed that queue item, on a guest thread that is outside any scope and holds the
    // submission exclusion (so no other submission runs in between): the render thread at its next import call (the
    // bridge's entry hook, non-blocking), or whoever must first (any submission's start, or the render thread before it
    // parks or sleeps: they wait for the queue item). As on the console, the render thread goes on while the interrupt is
    // pending; the tile thread that waits for the interrupt's event is woken at about the time the drain used to end.
    const auto applyStores=[&](const std::vector<diagnostic_skip::detail::Store>& stores,std::size_t from,std::size_t to) {
        unsigned failed=0; // the same stores diagnostic_skip::retire's apply() writes
        for(std::size_t i=from;i<to && i<stores.size();++i) {
            const auto& st=stores[i];
            if(st.virt){auto* q=addressSpace.translate(st.address);if(!q){++failed;continue;}diagnostic_skip::put(q,st.value,2);continue;}
            const auto physical=st.address&~3u;
            auto* q=physical<0x20000000u?addressSpace.translate(0xA0000000u+physical):nullptr;
            if(!q){++failed;continue;}
            diagnostic_skip::ocqBefore(st); // occlusion package
            diagnostic_skip::put(q,st.value,st.address&3);
            diagnostic_skip::ocqAfter(st);
        }
        if(failed)deferredStoreFailures.fetch_add(failed,std::memory_order_relaxed);
    };
    // The same handoff the synchronous path runs for a tile-command interrupt (the retire interrupt callback below).
    const auto commandIrq=[&](std::uint32_t physical,const std::vector<std::uint8_t>& bytes)->std::optional<recomp::bootstrap::NativeTileSceneHandoffResult> {
        const auto request=recomp::gpu::Gta5TileCommandInterrupt::parse(physical,bytes);
        if(!request || request->predicated() || !nativeGraphics || !diagnostic_skip::executor)return std::nullopt;
        // v282: the render thread's deferred scene IRQ with no world stop: executeNativeTileSceneConcurrent under the renderer
        // gate (no exclusive scope; the ISR's effect in one registry-lock section). A refused gate (a scope already open)
        // or a declined admission runs nothing, and the exclusive paths below take the interrupt. diag-v282-baseline: v281.
        if(!recomp_v282_baseline.load(std::memory_order_relaxed) && !recomp_v279_worker_isr.load(std::memory_order_relaxed) &&
           recomp::bootstrap::nativeSceneInlineEligible(addressSpace.base(),videoGlobals.device())) {
            prefenceNativeQueue(1);
            std::optional<recomp::bootstrap::NativeTileSceneHandoffResult> concurrent;unsigned busy=0;
            const bool gated=recomp::bootstrap::runNativeLockedGate(*diagnostic_skip::executor,[&]{
                concurrent=recomp::bootstrap::executeNativeTileSceneConcurrent(registry,videoGlobals.device(),*request,
                    [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileCommandInterrupt(*request,memory,guest);},
                    recomp::bootstrap::currentGuestThreadToken(),busy);});
            if(busy)v282Busy.fetch_add(busy,std::memory_order_relaxed);
            if(gated && concurrent && concurrent->admitted){v282Concurrent.fetch_add(1,std::memory_order_relaxed);return concurrent;}
            v282Declined.fetch_add(1,std::memory_order_relaxed);
        }
        const auto result=tileSceneIrq(*diagnostic_skip::executor,"deferred-tile-irq",[&]{
          return sceneHandoff(registry,*diagnostic_skip::executor,videoGlobals.device(),*request,
            [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileCommandInterrupt(*request,memory,guest);},
            [&](const auto&,const auto& entry) {
                PPCContext cpu{};cpu.r1.u64=entry.stackPointer;cpu.r13.u64=entry.pcrAddress;
                cpu.r3.u64=entry.argument0;cpu.r4.u64=entry.argument1;cpu.lr=0;
                recomp_host_guest_call_indirect(entry.address,cpu,addressSpace.base());
                return cpu.r1.u64==entry.stackPointer && cpu.r13.u64==entry.pcrAddress && cpu.lr==0;
            });});
        if(tileIrqNoted(result))::recomp::diag::quietLogSampled(!(result.admitted & result.returned),ANDROID_LOG_INFO,"RecompRenderer",
            "RENDER_DIAG_TILE_IRQ admitted=%u returned=%u worker=%08x writes=%u waits=%u guest_returns=%u deferred=1",
            unsigned(result.admitted),unsigned(result.returned),result.worker,result.scratchWrites,result.scratchWaits,result.guestReturns);
        return result;
    };
    // Read-only: the scene interrupt entry's device conditions (GuestImportRegistry::runNativeTileSceneInterruptScope) from
    // the device words: ack lock (+11032) free, CPU2 slot event (+11576+216+60) unsignalled, slot root and CPU, slot queue
    // empty. `ordinaryFree` also asks for the ordinary tile lock (+11156) free: the non-blocking path waits for it; the
    // must-deliver path leaves it to the entry's own (foreign-holder) check, as the synchronous interrupt does.
    const auto sceneEntryOpen=[&](bool ordinaryFree)->bool {
        const std::uint32_t device=videoGlobals.device();constexpr std::uint32_t Slot=11576+2*108;
        const auto word=[&](std::uint32_t a,std::uint32_t& v)->bool{const auto* p=addressSpace.translate(a);if(!p)return false;
            v=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];return true;};
        std::uint32_t ack=1,ordinary=1,header=0,signal=1,root=0,cpu=0,q=0,read=1;
        if(!device || !word(device+11032,ack) || ack)return false;
        if(ordinaryFree && (!word(device+11156,ordinary) || ordinary))return false;
        if(!word(device+Slot+60,header) || (header>>24)!=0 || !word(device+Slot+64,signal) || signal)return false;
        if(!word(device+Slot,root) || root!=device+11080 || !word(device+Slot+4,cpu) || cpu!=2)return false;
        return word(device+Slot+84,q) && word(device+Slot+88,read) && q==read;
    };
    const auto replayKickBins=[&](const recomp::gpu::DrawBinState& kick) { // as the retire interrupt callback's bytes.empty() case
        if(!nativeGraphics || !kick.knownWords)return;
        for(unsigned w=0;w<4;++w)if(kick.knownWords&(1u<<w)) {
            const std::uint32_t header=0xC0006000u+(w<<8),value=kick.words[w];
            const std::uint8_t packet[8]{std::uint8_t(header>>24),std::uint8_t(header>>16),std::uint8_t(header>>8),std::uint8_t(header),
                std::uint8_t(value>>24),std::uint8_t(value>>16),std::uint8_t(value>>8),std::uint8_t(value)};
            if(const auto command=recomp::gpu::NativeTileBinCommand::parse(packet))(void)nativeGraphics->executeLogicalBinCommand(*command);
        }
    };
    // mode 0: the owner at an import call (only when the queue item has run and the scene interrupt's entry is open);
    // mode 1: must deliver (a submission's start, the owner before it parks or sleeps): waits for the queue item, and up
    // to 50 ms for the entry to open (the tile thread consuming the previous slot), then delivers anyway.
    const auto deliverDeferredIrq=[&](int mode)->bool {
        if(!deferredIrqPending.load(std::memory_order_acquire))return true;
        const auto token=recomp::bootstrap::currentGuestThreadToken();
        // v233: an import-entry poll (mode 0; its callers ignore the result) returns before the mutex when the locked checks
        // below could only refuse it: another thread than the owner (deferIrqs always defers the render thread's), or the
        // queue item not reached yet. v227 outdoor profile: this mutex ~0.4% of the render thread, taken at every import of
        // every thread while an interrupt is pending. diag-v233-baseline: always the locked checks.
        if(mode==0 && !(&recomp_v233_baseline && recomp_v233_baseline.load(std::memory_order_relaxed)) &&
           (token!=0x6408e000u || !recomp::gpu::generic::guestQueueReached(deferredIrqMarker.load(std::memory_order_acquire))))return false;
        static thread_local bool busy=false;if(busy)return false;
        std::unique_lock lock(deferredIrqMutex);
        if(!deferredIrq.pending)return true;
        if(mode==0) {
            if(token!=deferredIrq.owner || !recomp::gpu::generic::guestQueueReached(deferredIrq.marker))return false;
            if(!sceneEntryOpen(true)){deferredIrqStats.notOpen.fetch_add(1,std::memory_order_relaxed);return false;}
        }
        const bool inSubmission=recomp::bootstrap::insideGuestGraphicsSubmission();
        if(!inSubmission && !recomp::bootstrap::tryEnterGuestGraphicsSubmissionExclusion()){deferredIrqStats.skipped.fetch_add(1,std::memory_order_relaxed);return false;}
        struct Leave {bool on;~Leave(){if(on)recomp::bootstrap::leaveGuestGraphicsSubmissionExclusion();}} leave{!inSubmission};
        DeferredIrq taken=std::move(deferredIrq);deferredIrq=DeferredIrq{};
        deferredIrqPending.store(false,std::memory_order_release);recomp::bootstrap::setGuestImportEntryHook(nullptr);
        lock.unlock();
        busy=true;struct Busy {~Busy(){busy=false;}} busyGuard;
        const auto putBack=[&]{ // a retryable refusal: the interrupt stays pending (nothing of it ran)
            std::lock_guard again(deferredIrqMutex);taken.pending=true;deferredIrq=std::move(taken);
            deferredIrqPending.store(true,std::memory_order_release);recomp::bootstrap::setGuestImportEntryHook(&deferredIrqImportHook);
        };
        if(!recomp::gpu::generic::guestQueueReached(taken.marker)) {
            const auto t0=SubmitClock::now();recomp::gpu::generic::guestDrainTo(taken.marker);
            deferredIrqStats.drained.fetch_add(1,std::memory_order_relaxed);deferredIrqStats.drainNs.fetch_add(submitNs(t0,SubmitClock::now()),std::memory_order_relaxed);
        }
        if(mode==1 && !sceneEntryOpen(false)) {
            deferredIrqStats.waitedOpen.fetch_add(1,std::memory_order_relaxed);
            const auto until=SubmitClock::now()+std::chrono::milliseconds(50);
            while(SubmitClock::now()<until && !sceneEntryOpen(false))
                if(!diagnostic_skip::executor->delayUntil(std::chrono::steady_clock::now()+std::chrono::microseconds(200)))break;
        }
        recomp::gpu::generic::noteCompletionPublishing(true); // v210: the interrupt and the stores publish completion
        struct EndPublish {~EndPublish(){recomp::gpu::generic::noteCompletionPublishing(false);}} endPublish;
        const auto fencesBefore=recomp::gpu::nativeQueueFenceCount();
        auto& d=taken.d;
        const std::size_t at=d.irqStoreAt.empty()?0:std::min(d.irqStoreAt[0],d.stores.size());
        applyStores(d.stores,taken.applied,at); // (none: every store before the interrupt was queued with the marker)
        diagnostic_skip::currentBins=d.irqBins.empty()?recomp::gpu::DrawBinState{}:d.irqBins[0];diagnostic_skip::tailSkipped=false;
        const auto result=commandIrq(d.irqs[0].first,d.irqs[0].second);
        if(!result || !result->admitted) {
            deferredIrqStats.refused.fetch_add(1,std::memory_order_relaxed);taken.applied=at;putBack();return false;
        }
        if(!result->returned)deferredIrqStats.failed.fetch_add(1,std::memory_order_relaxed);
        applyStores(d.stores,at,d.stores.size());
        replayKickBins(d.kickBins);
        if(recomp::gpu::nativeQueueFenceCount()!=fencesBefore)recomp::gpu::generic::submitPending(); // (as the handler after a native fence)
        const auto latency=submitNs(taken.at,SubmitClock::now());
        deferredIrqStats.delivered.fetch_add(1,std::memory_order_relaxed);
        (mode==0?deferredIrqStats.atImport:deferredIrqStats.atMust).fetch_add(1,std::memory_order_relaxed);
        deferredIrqStats.latencyNs.fetch_add(latency,std::memory_order_relaxed);
        auto seen=deferredIrqStats.latencyMaxNs.load(std::memory_order_relaxed);
        while(latency>seen && !deferredIrqStats.latencyMaxNs.compare_exchange_weak(seen,latency,std::memory_order_relaxed)){}
        return true;
    };
    deferredIrqDeliver=deliverDeferredIrq;
    // retire()'s deferIrqs: only the render thread's submissions with exactly one unpredicated tile-command interrupt.
    const auto deferIrqs=[&](diagnostic_skip::DeferredInterrupts& d)->bool {
        if(!irqDeferredOn() || d.irqs.size()!=1 || recomp::bootstrap::currentGuestThreadToken()!=0x6408e000u)return false;
        const auto request=recomp::gpu::Gta5TileCommandInterrupt::parse(d.irqs[0].first,d.irqs[0].second);
        if(!request || request->predicated() || !nativeGraphics || !diagnostic_skip::executor)return false;
        std::lock_guard lock(deferredIrqMutex);
        if(deferredIrq.pending)return false; // (delivered at the handler's start; kept synchronous if not)
        const std::size_t before=d.irqStoreAt.empty()?0:std::min(d.irqStoreAt[0],d.stores.size());
        std::vector<diagnostic_skip::detail::Store> head(d.stores.begin(),d.stores.begin()+std::ptrdiff_t(before));
        const auto marker=recomp::gpu::generic::queueGuestWorkMarked([&applyStores,head=std::move(head)]{applyStores(head,0,head.size());});
        deferredIrq=DeferredIrq{true,0x6408e000u,marker,SubmitClock::now(),std::move(d),before};
        deferredIrqMarker.store(marker,std::memory_order_relaxed); // v233 (published by the release store below)
        deferredIrqPending.store(true,std::memory_order_release);
        recomp::bootstrap::setGuestImportEntryHook(&deferredIrqImportHook);
        deferredIrqStats.deferred.fetch_add(1,std::memory_order_relaxed);
        return true;
    };
    if (nativeInitialCoherency && (!cooperative || !registry.bindNativeInitialCoherencySubmission() ||
        (experimentalNativeInitialization && !registry.bindExperimentalNativeInitialization()) ||
        (nativeEmptyCacheCoherency && !registry.bindEmptyGuestCacheCoherency()) ||
        (nativeStateBatches && !registry.bindNativeStateBatches()) ||
        (experimentalIdleCachePair && !registry.bindExperimentalIdleCachePair()) ||
        (experimentalOwnedColor && !registry.bindExperimentalOwnedColorBatch()) ||
        (nativeNoSystemOverlay && !registry.bindNativeNoSystemOverlay()) ||
        (nativeSwapCommands && !registry.bindExperimentalNativeSwapCommands([&](const auto& image){
          // v43: after every queued frame, serialised with the presenter thread.
          return framePresenter.runSync([&]()->bool {
#if (defined(__ANDROID__) || defined(__APPLE__))
            if (nativeGraphics && nativeGraphics->nativeScalerConfigured()) {
                std::vector<std::uint8_t> pixels;
                if (!nativeGraphics->mapNativeScanout(image.rgba.data(),image.rgba.size(),image.request.width,image.request.height,pixels)) return false;
                return liveCallbacks->orderedRgba(liveCallbacks->user,pixels.data(),static_cast<std::uint32_t>(pixels.size()),
                    image.request.width,image.request.height)==1;
            }
#endif
            return liveCallbacks->orderedRgba(liveCallbacks->user,image.rgba.data(),
                static_cast<std::uint32_t>(image.rgba.size()),image.request.width,image.request.height)==1;
          });
        })) ||
        (nativeScalerTiming && !registry.bindExperimentalNativeScalerTiming(static_cast<std::uint32_t>(scalerTimingValues[0]),
            static_cast<std::uint32_t>(scalerTimingValues[1]))) ||
        (nativeIdentityScaler && !registry.bindExperimentalNativeIdentityScaler()) ||
        (nativeIdentityScaler && (!hasVideoGlobals || !registry.bindNativeInterruptDeviceRoot(
            [&](std::uint32_t device) {return videoGlobals.matchesDevice(device);}))) ||
        !recomp::bootstrap::bindGuestGraphicsSubmission([&](auto word) {
            // Attempt v111: the ring walk below may retire the fences behind the draws queued so far, after which the
            // game may rewrite the vertex buffers and textures they read: the pipelined renderer reads them first.
            recomp::diag::timeline::mark(recomp::diag::timeline::SubmitBegin,0,std::uint32_t(word)); // v159
            const HostActivityScope submissionActivity("submission"); // v226
            (void)deliverDeferredIrq(1); // v211: a pending deferred interrupt precedes this submission's walk and stores
            // v183 (goal step 2): no unconditional drain. The ring walk hands a plain submission's completion stores to the
            // renderer queue (behind the draws that read the memory those fences release); every path that may publish
            // completion itself (tile interrupts, native submission handlers, tile recording) drains first through the
            // hook. files/private-trace/diag-generic-read-barrier-always restores v182 (drain before every submission).
            static const bool barrierAlways=::access(RECOMP_APP_PATH("/private-trace/diag-generic-read-barrier-always"),F_OK)==0;
            submissionBarrierDone=false;
            if(barrierAlways || tileRecordingSeen.load(std::memory_order_relaxed))submissionBarrier();
            else recomp::bootstrap::setNativeSubmissionCommitHook(&submissionBarrier);
            struct ClearHook {~ClearHook(){recomp::bootstrap::setNativeSubmissionCommitHook(nullptr);
                if(completionSectionOpen){completionSectionOpen=false;recomp::gpu::generic::noteCompletionPublishing(false);}}} clearHook; // v210
            submissionDiagnostic.begin(word);
            const auto submitStart=SubmitClock::now(); // Attempt v44 timing.
            const auto fencesBefore=recomp::gpu::nativeQueueFenceCount(); // Attempt v78
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS != 0
            const auto submitter=recomp::bootstrap::currentGuestThreadToken();
#else
            const std::uint32_t submitter=0;
#endif
            auto& who=submitTiming[submitter==0x6408e000u?0:submitter==0x640b1000u?1:2];
            // v44: the accepting handler, recorded directly (v36 serialised the whole step list to JSON
            // and searched it for the first "returned-true" step; the chain short-circuits at that step).
            const char* acceptedBy="none";
            const auto traced=[&](const char* name,auto&& operation)->bool {
                const bool result=submissionDiagnostic.attempt(name,operation);
                if(result)acceptedBy=name;
                return result;
            };
            diagnostic_skip::RetireTiming retireTiming;bool retireCalled=false;
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE) && (defined(__ANDROID__) || defined(__APPLE__))
            if(const auto captured=submissionDiagnostic.attempt("captureRendererTileSubmission",
                    [&]{return captureRendererTileSubmission(word);})) {
                // v183: a native recording completes submissions outside this handler: nothing may stay queued.
                submissionBarrier();
                if(!tileRecordingSeen.exchange(true))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_DIAG_DEFERRED_RETIRE disabled reason=tile-recording write=%u",word);
                { // Attempt v2: the per-submission line also covers the early tile-capture path.
                    const auto be=[&](std::uint32_t a)->std::uint32_t{const auto* p=addressSpace.translate(a);
                        return p?(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3]:0xFFFFFFFFu;};
                    const auto block=be(0x1004cd00u+11024);
                    if(!*captured || recomp::diag::sampled(submitLines))::recomp::diag::quietLogSampled(!*captured,ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SUBMIT write=%u accepted=%u handler=captureRendererTileSubmission fence=%08x issued=%08x",
                        word,unsigned(*captured),block!=0xFFFFFFFFu?be(block):0xFFFFFFFFu,be(0x1004cd00u+11036));
                }
                submissionDiagnostic.finish(*captured);return *captured;
            }
#endif
            // Ordinary paths remain synchronous without guest reentry. The
            // explicit scaler-interrupt path below hands off only to its fresh
            // owned CPU2 worker and directly back; every other writer is parked.
#define TRACE_SUBMISSION(method) traced(#method,[&]{return registry.method(word);})
            const bool accepted=TRACE_SUBMISSION(submitNativeInitialCoherency) ||
                (experimentalNativeInitialization && TRACE_SUBMISSION(submitExperimentalNativeInitialization)) ||
                (nativeEmptyCacheCoherency && TRACE_SUBMISSION(submitEmptyGuestCacheCoherency)) ||
                (nativeStateBatches && TRACE_SUBMISSION(submitNativeStateBatch)) ||
                (nativeStateBatches && experimentalIdleCachePair && TRACE_SUBMISSION(submitNativeMaintenancePoints)) ||
                // Disabled per independent audit (docs/claude/Audit.md): the
                // verifier accepts real guest-visible effects as side-effect-
                // free (scratch-register writeback, persistent register-state
                // changes with no draw, a malformed DRAW_INDX missing its
                // initiator token) and a prior candidate shipped with an
                // ABI-inconsistent GuestImportRegistry layout across
                // translation units. Do not re-enable until both are fixed
                // and re-verified; the investigation code/tests are kept.
                (false && nativeStateBatches && TRACE_SUBMISSION(submitVerifiedSideEffectFreeBatch)) ||
                traced("submitOwnedColor",[&]{return submitOwnedColor(word);}) ||
                (nativeIdentityScaler && TRACE_SUBMISSION(submitNativeScalerStorageCoherency)) ||
                (nativeIdentityScaler && TRACE_SUBMISSION(submitNativePostScalerStorageCoherency)) ||
                (nativeIdentityScaler && TRACE_SUBMISSION(submitNativePostDisplayStorageCoherency)) ||
                (nativeIdentityScaler && TRACE_SUBMISSION(submitNativeDisplayGamma)) ||
                (nativeDisplayOutput && TRACE_SUBMISSION(submitExperimentalNativeDisplayOutput)) ||
                (nativeDisplayOutput && TRACE_SUBMISSION(submitNativePostOutputStorageCoherency)) ||
                (livePostOutputColor && TRACE_SUBMISSION(submitNativePostOutputSwapStorageCoherency)) ||
                (livePostOutputColor && TRACE_SUBMISSION(submitNativeDeferredSwapStorageCoherency)) ||
                (submitPostOutputSwap && traced("submitPostOutputSwap",[&]{return submitPostOutputSwap(word);})) ||
                (nativeSwapCommands && (TRACE_SUBMISSION(submitNativeCommandStorageCoherency) ||
                    (experimentalGpuClock && TRACE_SUBMISSION(submitNativeWrappedSwapCommand)) ||
                    TRACE_SUBMISSION(submitExperimentalNativeSwapCommand))) ||
                (submitScalerInterrupt && traced("submitScalerInterrupt",[&]{return submitScalerInterrupt(word);})) ||
                // Last fallback: exact runtime command-storage coherency only.
                // It owns a fresh completion and never relaxes startup cursors.
                (nativeDisplayOutput && traced("submitNativeRuntimeCommandStorageCoherency",[&]{
                    // v44 timing: this accepted path holds R across VulkanEngine::fenceNativeQueue.
                    const auto started=SubmitClock::now();
                    // v110: the GPU wait first, with R free (see prefenceNativeQueue); counted in rcs_ms as before.
                    const bool prefenced=rcsPrefence && rcsCandidate(word);
                    if(prefenced)prefenceNativeQueue(0);
                    const bool r=registry.submitNativeRuntimeCommandStorageCoherency(word);
                    if(prefenced && !r)rcsPrefenceMissed.fetch_add(1,std::memory_order_relaxed);
                    if(r){const auto t=submitNs(started,SubmitClock::now());++who.rcsN;who.rcsNs+=t;who.rcsMaxNs=std::max(who.rcsMaxNs,t);}
                    return r;
                }))
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE) && (defined(__ANDROID__) || defined(__APPLE__))
                || (nativeDisplayOutput && nativeStateBatches && experimentalIdleCachePair &&
                    traced("submitRendererRuntimeMaintenance",[&]{
                        // v44: open the registry's budget-256 exclusive gate only for a submission that can
                        // complete it (diagnostic_retire.h, runtimeMaintenanceCandidate); the step still
                        // records returned-false, as the gated attempt would have.
                        const auto started=SubmitClock::now();
                        if(!diagnostic_skip::runtimeMaintenanceCandidate(registry,addressSpace,word)) {
                            ++who.maintSkipped;who.maintNs+=submitNs(started,SubmitClock::now());return false;
                        }
                        ++who.maintCalled;
                        recomp::bootstrap::noteNativeSubmissionMayCommitAt(22); // v183 site 22
                        const bool r=submitRendererRuntimeMaintenance(word);
                        who.maintNs+=submitNs(started,SubmitClock::now());return r;
                    }))
#endif
                || traced("diagnosticRetireSubmission",[&]{
                    retireCalled=true;
                    if(nativeGraphics) { // Attempt v1: adopt a newer engine-side SCRATCH_ADDR/UMSK write.
                        const auto addr=nativeGraphics->retainedRegister(0x1DD),mask=nativeGraphics->retainedRegister(0x1DC);
                        if(addr && *addr!=diagnostic_skip::retainedScratchAddr){diagnostic_skip::retainedScratchAddr=*addr;diagnostic_skip::scratchAddr=*addr;}
                        if(mask && *mask!=diagnostic_skip::retainedScratchMask){diagnostic_skip::retainedScratchMask=*mask;diagnostic_skip::scratchMask=*mask;}
                    }
                    return diagnostic_skip::retire(registry,addressSpace,word,
                    [&](std::uint32_t physical,const std::vector<std::uint8_t>& bytes) {
                        if(bytes.empty()) { // DIAGNOSTIC v689: replay a retired kick's final bin words.
                            if(!nativeGraphics)return false;
                            const auto& kick=diagnostic_skip::currentBins;unsigned replayed=0,refused=0;
                            for(unsigned w=0;w<4;++w)if(kick.knownWords&(1u<<w)) {
                                const std::uint32_t header=0xC0006000u+(w<<8),value=kick.words[w];
                                const std::uint8_t packet[8]{std::uint8_t(header>>24),std::uint8_t(header>>16),std::uint8_t(header>>8),std::uint8_t(header),
                                    std::uint8_t(value>>24),std::uint8_t(value>>16),std::uint8_t(value>>8),std::uint8_t(value)};
                                const auto command=recomp::gpu::NativeTileBinCommand::parse(packet);
                                (command && nativeGraphics->executeLogicalBinCommand(*command))?++replayed:++refused;
                            }
                            static unsigned logged=0;
                            if(logged++<64)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                                "RENDER_DIAG_KICK_BINS known=%x words=%08x/%08x/%08x/%08x replayed=%u refused=%u",
                                kick.knownWords,kick.words[0],kick.words[1],kick.words[2],kick.words[3],replayed,refused);
                            return refused==0;
                        }
                        if(bytes.size()==recomp::gpu::Gta5TileSceneContinuation::Bytes) { // DIAGNOSTIC v688
                            const auto tail=recomp::gpu::Gta5TileSceneContinuation::parse(physical,bytes);
                            if(!tail || !nativeGraphics || !diagnostic_skip::executor)return false;
                            const auto& kick=diagnostic_skip::currentBins;unsigned replayed=0,refused=0;
                            for(unsigned w=0;w<4;++w)if(kick.knownWords&(1u<<w)) {
                                const std::uint32_t header=0xC0006000u+(w<<8),value=kick.words[w];
                                const std::uint8_t packet[8]{std::uint8_t(header>>24),std::uint8_t(header>>16),std::uint8_t(header>>8),std::uint8_t(header),
                                    std::uint8_t(value>>24),std::uint8_t(value>>16),std::uint8_t(value>>8),std::uint8_t(value)};
                                const auto command=recomp::gpu::NativeTileBinCommand::parse(packet);
                                (command && nativeGraphics->executeLogicalBinCommand(*command))?++replayed:++refused;
                            }
                            const auto& state=nativeGraphics->stateBatchSnapshot();
                            const recomp::gpu::DrawBinState bins{{unsigned(state.binMask),unsigned(state.binMask>>32),
                                unsigned(state.binSelect),unsigned(state.binSelect>>32)},state.knownBinWords};
                            const auto selected=tail->select(bins);
                            static std::atomic<unsigned long long> selectLines{0}; // v218: sampled
                            if(!selected || recomp::diag::sampled(selectLines))::recomp::diag::quietLogSampled(!selected,ANDROID_LOG_INFO,"RecompRenderer",
                                "RENDER_DIAG_TILE_TAIL_SELECT kick_known=%x replayed=%u refused=%u known=%x mask=%016llx select=%016llx selected=%u interrupt=%u",
                                kick.knownWords,replayed,refused,state.knownBinWords,(unsigned long long)state.binMask,(unsigned long long)state.binSelect,
                                unsigned(selected.has_value()),unsigned(selected && selected->interruptRequired));
                            if(selected && !selected->interruptRequired) {
                                diagnostic_skip::tailSkipped=true;diagnostic_skip::skippedTailContext=tail->request().callbackContext();
                                diagnostic_skip::skippedTailScratch=tail->request().scratchPhysical();return false;
                            }
                            if(!selected)return false;
                            // v284: the tile thread's tail scene IRQ with no world stop, as v282's deferred one (the renderer gate,
                            // the ISR's effect in one registry-lock section); a refused gate or declined admission runs nothing
                            // and the exclusive path below takes it. diag-v284-baseline: v282 (the exclusive inline path).
                            if(!recomp_v284_baseline.load(std::memory_order_relaxed) && !recomp_v282_baseline.load(std::memory_order_relaxed) &&
                               !recomp_v279_worker_isr.load(std::memory_order_relaxed) &&
                               recomp::bootstrap::nativeSceneInlineEligible(addressSpace.base(),videoGlobals.device())) {
                                prefenceNativeQueue(1);
                                std::optional<recomp::bootstrap::NativeTileSceneHandoffResult> concurrent;unsigned busy=0;
                                const bool gated=recomp::bootstrap::runNativeLockedGate(*diagnostic_skip::executor,[&]{
                                    concurrent=recomp::bootstrap::executeNativeTileSceneConcurrent(registry,videoGlobals.device(),tail->request(),
                                        [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileSceneContinuation(*tail,memory,guest);},
                                        recomp::bootstrap::currentGuestThreadToken(),busy);});
                                if(busy)v282Busy.fetch_add(busy,std::memory_order_relaxed);
                                if(gated && concurrent && concurrent->admitted){v284Concurrent.fetch_add(1,std::memory_order_relaxed);
                                    return concurrent->returned;}
                                (gated?v284Declined:v284GateRefused).fetch_add(1,std::memory_order_relaxed);
                            }
                            const auto result=tileSceneIrq(*diagnostic_skip::executor,"retire-tile-tail",[&]{
                              return sceneHandoff(registry,*diagnostic_skip::executor,videoGlobals.device(),tail->request(),
                                [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileSceneContinuation(*tail,memory,guest);},
                                [&](const auto&,const auto& entry) {
                                    PPCContext cpu{};cpu.r1.u64=entry.stackPointer;cpu.r13.u64=entry.pcrAddress;
                                    cpu.r3.u64=entry.argument0;cpu.r4.u64=entry.argument1;cpu.lr=0;
                                    recomp_host_guest_call_indirect(entry.address,cpu,addressSpace.base());
                                    return cpu.r1.u64==entry.stackPointer && cpu.r13.u64==entry.pcrAddress && cpu.lr==0;
                                });});
                            if(tileIrqNoted(result))::recomp::diag::quietLogSampled(!(result.admitted & result.returned),ANDROID_LOG_INFO,"RecompRenderer",
                                "RENDER_DIAG_TILE_IRQ kind=tail admitted=%u returned=%u worker=%08x writes=%u waits=%u guest_returns=%u",
                                unsigned(result.admitted),unsigned(result.returned),result.worker,result.scratchWrites,result.scratchWaits,result.guestReturns);
                            return result.admitted && result.returned;
                        }
                        const auto request=recomp::gpu::Gta5TileCommandInterrupt::parse(physical,bytes);
                        if(!request || request->predicated() || !nativeGraphics || !diagnostic_skip::executor)return false;
                        const auto result=tileSceneIrq(*diagnostic_skip::executor,"retire-tile-irq",[&]{
                          return sceneHandoff(registry,*diagnostic_skip::executor,videoGlobals.device(),*request,
                            [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileCommandInterrupt(*request,memory,guest);},
                            [&](const auto&,const auto& entry) {
                                PPCContext cpu{};cpu.r1.u64=entry.stackPointer;cpu.r13.u64=entry.pcrAddress;
                                cpu.r3.u64=entry.argument0;cpu.r4.u64=entry.argument1;cpu.lr=0;
                                recomp_host_guest_call_indirect(entry.address,cpu,addressSpace.base());
                                return cpu.r1.u64==entry.stackPointer && cpu.r13.u64==entry.pcrAddress && cpu.lr==0;
                            });});
                        if(tileIrqNoted(result))::recomp::diag::quietLogSampled(!(result.admitted & result.returned),ANDROID_LOG_INFO,"RecompRenderer",
                            "RENDER_DIAG_TILE_IRQ admitted=%u returned=%u worker=%08x writes=%u waits=%u guest_returns=%u",
                            unsigned(result.admitted),unsigned(result.returned),result.worker,result.scratchWrites,result.scratchWaits,result.guestReturns);
                        return result.admitted && result.returned;
                    },&retireTiming,[&](std::vector<diagnostic_skip::detail::Store>&& stores)->bool { // v183
                        return recomp::gpu::generic::queueGuestWork([space=&addressSpace,stores=std::move(stores)] {
                            unsigned failed=0;
                            for(const auto& st:stores) { // the same stores diagnostic_skip::retire's apply() writes
                                if(st.virt){auto* q=space->translate(st.address);if(!q){++failed;continue;}diagnostic_skip::put(q,st.value,2);continue;}
                                const auto physical=st.address&~3u;
                                auto* q=physical<0x20000000u?space->translate(0xA0000000u+physical):nullptr;
                                if(!q){++failed;continue;}
                                diagnostic_skip::ocqBefore(st); // occlusion package
                                diagnostic_skip::put(q,st.value,st.address&3);
                                diagnostic_skip::ocqAfter(st);
                            }
                            if(failed)deferredStoreFailures.fetch_add(failed,std::memory_order_relaxed);
                        });
                    },deferIrqs);}) // DIAGNOSTIC LAST RESORT (v211: deferIrqs)
                ;
#undef TRACE_SUBMISSION
            const auto logStart=SubmitClock::now();
            { // Attempt v2: every submission's accepting handler and the D3D fence right after it.
                const auto be=[&](std::uint32_t a)->std::uint32_t{const auto* p=addressSpace.translate(a);
                    return p?(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3]:0xFFFFFFFFu;};
                const auto block=be(0x1004cd00u+11024);
                if(!accepted || recomp::diag::sampled(submitLines))::recomp::diag::quietLogSampled(!accepted,ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SUBMIT write=%u accepted=%u handler=%s fence=%08x issued=%08x",
                    word,unsigned(accepted),acceptedBy,block!=0xFFFFFFFFu?be(block):0xFFFFFFFFu,be(0x1004cd00u+11036));
            }
            { // Attempt v44: accumulate this submission's timing; one RENDER_DIAG_SUBMIT_TIMING line set per 10 s.
                const auto end=SubmitClock::now();const auto total=submitNs(submitStart,end);
                ++who.n;who.handlerNs+=total;who.handlerMaxNs=std::max(who.handlerMaxNs,total);who.logNs+=submitNs(logStart,end);
                if(retireCalled) {
                    ++who.retireN;
                    who.retireNs+=retireTiming.walkNs+retireTiming.irqNs+retireTiming.lockHoldNs+retireTiming.lockWaitNs;
                    who.walkNs+=retireTiming.walkNs;who.walkMaxNs=std::max<unsigned long long>(who.walkMaxNs,retireTiming.walkNs);
                    who.words+=retireTiming.words;who.rWaitNs+=retireTiming.lockWaitNs;who.rHoldNs+=retireTiming.lockHoldNs;
                    who.rHoldMaxNs=std::max<unsigned long long>(who.rHoldMaxNs,retireTiming.lockHoldMaxNs);
                    who.irqs+=retireTiming.irqs;who.irqNs+=retireTiming.irqNs;
                }
                if(end-submitTimingSince>=std::chrono::seconds(10))logSubmitTiming(end);
            }
            // Attempt v78: a native queue fence in the handlers above (runtime command-storage coherency, tile
            // interrupts) waited for everything on the queue the generic renderer shares; right after it, the generic
            // renderer's draws recorded since its last submission go to the GPU, so they have until the next fence to run
            // instead of all arriving at present (v71: rcs_ms ~2.1 s per 10 s on the render thread). After a handler
            // without a fence nothing is submitted (a submission then would only lengthen the next fence's wait).
            // Attempt v127 (opt-in files/private-trace/diag-generic-submit-every-kick): submit after every ring submission,
            // not only after a native fence. The GPU is ~40% busy (gpu_ms ~22 of ~53 ms frames) and the tile IRQ pre-fences
            // wait for everything submitted; work handed over at each kick (the renderer thread was just drained by
            // guestReadBarrier) starts earlier and is more likely finished when the next fence waits for it.
            static const bool submitEveryKick=::access(RECOMP_APP_PATH("/private-trace/diag-generic-submit-every-kick"),F_OK)==0;
            static std::atomic<unsigned long long> extraSubmits{0};
            if(recomp::gpu::nativeQueueFenceCount()!=fencesBefore)recomp::gpu::generic::submitPending();
            else if(submitEveryKick && accepted) {
                recomp::gpu::generic::submitPending();
                const auto n=++extraSubmits;
                if((n&(n-1))==0 && n>=256)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SUBMIT_EVERY_KICK extra_submits=%llu",n);
            }
            submissionDiagnostic.finish(accepted);
            recomp::diag::timeline::mark(recomp::diag::timeline::SubmitEnd,accepted?1:0,std::uint32_t(word)); // v159
            return accepted;
        }))) return 2;
    if (experimentalNativeInitialization && hasHsioLock &&
        !registry.bindNativeEdramTrainingPolicy(static_cast<std::uint32_t>(hsioLockValues[1]))) return 2;
    // Attempt v2: labels are views of static __func__ strings (recomp_guest_execution_checkpoint_named),
    // so the ring stores views instead of copying ~20 bytes on every guest function entry.
    // Multicore plan WP4: atomic sequence and slots, safe for concurrent sinks; same contents and order.
    diagnostic_skip::FunctionLabelRing functionLabels(functionLabelLimit);
    if (disconnectedInput && !registry.bindDisconnectedInput()) return 2;
    if(nativeInput && !registry.bindNativeGamepad(recomp::input::delivery::embedding::selectNativeGamepadReader(inputCallbacks,[&](std::uint32_t user,recomp::input::GamepadState& state){
        const int connected=inputCallbacks->gamepadState(inputCallbacks->inputUser,user,&state);
        // Attempt v13: synthetic sticks for harness movement tests (diagnostic builds only). The
        // harness writes files/private-trace/diag-stick = "LX LY RX RY millis"; the values replace the
        // physical sticks of user 0 until they expire, with a fresh packet number so the title sees
        // new input. Physical buttons and triggers are untouched.
        // v153 (play bot): the file is polled every 16 ms (v136: 200 ms) and may hold a SEQUENCE of up to 256 records, one
        // per line, "BUTTONS LT RT LX LY RX RY ms" (v136 form) or "LX LY RX RY ms" (v13 form); they play back to back on the
        // device clock from the moment the file is read, so timed combinations do not depend on adb latency. A new file
        // replaces the sequence in progress. Buttons are OR'd with the physical pad, triggers max'd, sticks replaced.
        struct SyntheticStep {int buttons,lt,rt,lx,ly,rx,ry;long long ms;};
        static std::mutex syntheticMutex;static std::vector<SyntheticStep> syntheticSteps;static long long syntheticStart=0,syntheticTotal=0;
        static std::atomic<std::uint32_t> syntheticPacket{0x40000000u};
        static std::atomic<long long> nextPoll{0};
        // ios-duo (2026-11-05, main: the Thor's autopilot on iOS): the owner's own input (the physical pad or the app's touch controls,
        // read here before the bot's synthetic steps are laid over it) makes the autopilot let go at once, as on the Thor.
        if(iosbot::physicalActivity(state))recomp_autopilot_note_human();
        if(iosbot::stopForPhysical(state)) {
            iosrendererqos::experiment().close();
            std::lock_guard lock(syntheticMutex);syntheticSteps.clear();syntheticTotal=0;
            return connected;
        }
        const long long nowMs=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if(nowMs>=nextPoll.load(std::memory_order_relaxed)) {
            nextPoll.store(nowMs+16,std::memory_order_relaxed);
            constexpr const char* StickPath=RECOMP_APP_PATH("/private-trace/diag-stick");
            if(FILE* f=std::fopen(StickPath,"r")) {
                std::vector<SyntheticStep> steps;long long total=0;bool valid=true;char line[256];
                while(steps.size()<256 && std::fgets(line,sizeof line,f)) {
                    long long v[8]{};
                    const int got=std::sscanf(line,"%lld %lld %lld %lld %lld %lld %lld %lld",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7]);
                    if(got<=0)continue;
                    const bool pad=got==8;
                    if(!(got==5 || (pad && v[0]>=0 && v[0]<=0xFFFF && v[1]>=0 && v[1]<=255 && v[2]>=0 && v[2]<=255))){valid=false;break;}
                    const long long ms=pad?v[7]:v[4];
                    if(ms<=0 || ms>60000){valid=false;break;}
                    const auto clamp=[](long long x){return int(std::max<long long>(-32768,std::min<long long>(32767,x)));};
                    steps.push_back(pad?SyntheticStep{int(v[0]),int(v[1]),int(v[2]),clamp(v[3]),clamp(v[4]),clamp(v[5]),clamp(v[6]),ms}
                                       :SyntheticStep{0,0,0,clamp(v[0]),clamp(v[1]),clamp(v[2]),clamp(v[3]),ms});
                    total+=ms;
                }
                std::fclose(f);::unlink(StickPath);
                if(valid && !steps.empty() && total<=600000) {
                    const auto first=steps.front();const auto count=steps.size();
                    {std::lock_guard lock(syntheticMutex);syntheticSteps=std::move(steps);syntheticStart=nowMs;syntheticTotal=total;}
                    __android_log_print(ANDROID_LOG_INFO,"RecompNativeInput","SYNTHETIC_PAD steps=%zu total_ms=%lld first: buttons=%04x lt=%d rt=%d lx=%d ly=%d rx=%d ry=%d ms=%lld (not physical)",
                        count,total,first.buttons,first.lt,first.rt,first.lx,first.ly,first.rx,first.ry,first.ms);
                }
            }
        }
        // v318: the latency probe (only while the timeline is armed): the first read that returns a changed input marks
        // InputPoll; a change made by a synthetic step also marks InputEdge at the moment that step took effect (the step's
        // start, or the sequence's end when it expired), on the same steady clock as the timeline.
        long long probeStepAbsMs=-1,probeEndAbsMs=-1;
        const auto probeInput=[&](const recomp::input::GamepadState& out) {
            if(user!=0 || !recomp::diag::timeline::armed.load(std::memory_order_relaxed))return;
            static std::mutex probeMutex;static recomp::input::GamepadState last;static bool have=false;
            static long long lastStepAbsMs=-1;static bool lastSynthetic=false;
            std::lock_guard lock(probeMutex);
            const bool changed=have && !v318::sameInput(last,out);
            const bool stepMoved=probeStepAbsMs>=0 && probeStepAbsMs!=lastStepAbsMs;
            long long edgeMs=-1;
            if(changed) {
                if(stepMoved)edgeMs=probeStepAbsMs;
                else if(probeStepAbsMs<0 && lastSynthetic && probeEndAbsMs>=0)edgeMs=probeEndAbsMs;
                if(edgeMs>=0)recomp::diag::timeline::markAt(std::uint64_t(edgeMs)*1000000u,recomp::diag::timeline::InputEdge,1);
                recomp::diag::timeline::mark(recomp::diag::timeline::InputPoll,edgeMs>=0?1:2);
            }
            // latency-path (lat2): EVERY read, with whether it changed for THIS thread (the audio engine's frequent reads take
            // v318's InputPoll; the game's own read of a change is only visible per reading thread). Measurement only.
            lat2NoteRead(out,probeStepAbsMs>=0,changed); // (out of line: this lambda's control flow and PGO record unchanged)
            last=out;have=true;lastStepAbsMs=probeStepAbsMs;lastSynthetic=probeStepAbsMs>=0;
        };
#if defined(RECOMP_TOUCH_MAPTAP)
        // thor-maptap (v658, map_tap.h) / qol-touch (v700) / iPad 231: user 0's pad: the app's request bit (0x0800) removed, the macro's
        // START / A OR'd with a packet number of their own; a request first reads files/private-trace/diag-maptap-tune (at most once per 200 ms).
        const auto mapTapPad=[](recomp::input::GamepadState& st,long long now) {
            namespace mt=recomp::gpu::maptap;
            if((st.buttons&mt::Request) && !mt::machine().watching()) {
                static std::atomic<long long> lastTune{-1000000};
                if(now-lastTune.load(std::memory_order_relaxed)>=200) {
                    lastTune.store(now,std::memory_order_relaxed);
                    mt::Params p;char text[256]{};
                    if(FILE* f=std::fopen(RECOMP_APP_PATH("/private-trace/diag-maptap-tune"),"r")){const auto n=std::fread(text,1,sizeof text-1,f);text[n]=0;std::fclose(f);}
                    mt::parseTune(text,p);mt::machine().tune(p);
                }
            }
            bool fresh=false;std::uint32_t serial=0;
            st.buttons=mt::machine().read(st.buttons,now,&fresh,&serial,mt::Sticks{&st.leftX,&st.leftY,&st.leftTrigger,&st.rightTrigger,&st.rightX,&st.rightY});
            if(fresh)st.packet=0x80000000u|((serial&0x7Fu)<<24)|(st.packet&0x00FFFFFFu);
        };
#endif /* RECOMP_TOUCH_MAPTAP */
        if(user==0) {
            SyntheticStep step{};bool active=false;
            {
                std::lock_guard lock(syntheticMutex);
                const long long at=nowMs-syntheticStart;
                if(!syntheticSteps.empty() && at>=0 && at<syntheticTotal) {
                    long long edge=0;
                    for(const auto& s:syntheticSteps){edge+=s.ms;if(at<edge){step=s;active=true;break;}}
                    if(active){const auto start=v318::stepStartMs(syntheticSteps,at);if(start>=0)probeStepAbsMs=syntheticStart+start;}
                }
                if(!syntheticSteps.empty())probeEndAbsMs=syntheticStart+syntheticTotal;
            }
            if(active) {
                state.leftX=static_cast<std::int16_t>(step.lx);state.leftY=static_cast<std::int16_t>(step.ly);
                state.rightX=static_cast<std::int16_t>(step.rx);state.rightY=static_cast<std::int16_t>(step.ry);
                state.buttons=static_cast<std::uint16_t>(state.buttons|step.buttons);
                state.leftTrigger=static_cast<std::uint8_t>(std::max<int>(state.leftTrigger,step.lt));
                state.rightTrigger=static_cast<std::uint8_t>(std::max<int>(state.rightTrigger,step.rt));
                state.packet=syntheticPacket.fetch_add(1)+1;
#if defined(RECOMP_TOUCH_MAPTAP)
                mapTapPad(state,nowMs); // thor-maptap (v658)
#endif
                probeInput(state);
#ifdef RECOMP_THOR_EDITION
                recomp::gpu::wheelrep::notePad(state.buttons,nowMs); // thor-radio (v651): the radio wheel's button (D-pad LEFT) for the renderer
#endif /* RECOMP_THOR_EDITION */
                return connected?connected:1;
            }
        }
#if defined(RECOMP_TOUCH_MAPTAP)
        if(user==0)mapTapPad(state,nowMs); // thor-maptap (v658)
#endif
        if(connected==1)probeInput(state);
#ifdef RECOMP_THOR_EDITION
        if(user==0 && connected==1)recomp::gpu::wheelrep::notePad(state.buttons,nowMs); // thor-radio (v651): the radio wheel's button (D-pad LEFT)
#endif /* RECOMP_THOR_EDITION */
        if(user==0) { // v338 (diagnostic causal spin): main's first pad read after each present
            static std::atomic<unsigned> seen{0};const auto p=::recomp_v338_presents.load(std::memory_order_relaxed);
            if(seen.exchange(p,std::memory_order_relaxed)!=p)
                if(const auto us=::recomp_v338_spin_main_us.load(std::memory_order_relaxed)) {
                    const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(us);while(std::chrono::steady_clock::now()<until){}
                }
        }
        return connected;
    }),[&](std::uint32_t user,recomp::input::GamepadState& state){
        return inputCallbacks->gamepadCapabilities(inputCallbacks->inputUser,user,&state);
    }))return 2;
    recomp_bind_generated_guest_import_registry(&registry, addressSpace.base());
    recomp::bootstrap::GuestExecutionGuard guard;
    const auto traceSink = functionLabelLimit == 0
                    ? recomp::bootstrap::GuestExecutionGuard::FunctionTraceSink{}
                    : recomp::bootstrap::GuestExecutionGuard::FunctionTraceSink(
                              [&](std::string_view label) {
#if defined(RECOMP_PRIVATE_CHECKPOINT_OBSERVER)
                                  RECOMP_PRIVATE_CHECKPOINT_OBSERVER(addressSpace.base(), label);
#endif
                                  diagnostic_skip::recordThreadLabel(label); // DIAGNOSTIC v692
                                  if (const auto dropped = functionLabels.record(label)) { // DIAGNOSTIC: where the guest is.
                                      const auto recent = functionLabels.recentAtDrop(dropped, 16);
                                      __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_LABELS entries=%llu recent=%s",
                                          static_cast<unsigned long long>(dropped),recent.c_str());
                                      diagnostic_skip::logScratch(registry,addressSpace); // DIAGNOSTIC v689
                                      diagnostic_skip::logThreads(); // DIAGNOSTIC v692
                                  }
                              });
    recomp::bootstrap::GuestExecutionResult execution;
    recomp::bootstrap::GuestCooperativeResult cooperativeResult;
    if (cooperative) {
        // Attempt v22 (lanes): the audio engine thread (token 64141000, 64-72% of all guest entries in
        // the v5-v13 profiles) and the native XAudio callback worker (64184000) run in permit domain 1,
        // in parallel with everything else in domain 0 (parallel-design/plan.md Phase B). Tokens have
        // been stable across runs; the policy must be pure and set before any worker starts.
        // Attempt v78 (opt-in experiment, default: none): files/private-trace/diag-domain-own lists up to four guest
        // thread tokens (hex, whitespace separated) that each get their own permit domain (4-7, in listed order). Read
        // once, before any worker starts (the policy must be pure). The tokens placed below keep their domains; list only
        // threads that are not renderer/Vulkan callers (the domain-2 invariant), for example a domain-0 thread that
        // RENDER_DIAG_EXEC_D0 shows holding or waiting for the shared permit on the main thread's frame path.
        static std::array<std::uint32_t,4> ownDomainTokens{};static unsigned ownDomainCount=0;
        if(std::FILE* f=std::fopen(RECOMP_APP_PATH("/private-trace/diag-domain-own"),"r")) {
            unsigned token=0;std::string listed;
            while(ownDomainCount<ownDomainTokens.size() && std::fscanf(f,"%x",&token)==1) {
                const bool placed=token==0x64141000u || token==0x64184000u || token==0x6408e000u || token==0x640b1000u ||
                    token==0x641c8000u || token==0x63001000u || !token;
                if(placed || std::find(ownDomainTokens.begin(),ownDomainTokens.begin()+ownDomainCount,token)!=ownDomainTokens.begin()+ownDomainCount)continue;
                ownDomainTokens[ownDomainCount++]=token;char b[16];std::snprintf(b,sizeof b,"%s%08x",listed.empty()?"":",",token);listed+=b;
            }
            std::fclose(f);
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DOMAIN_OWN tokens=%s count=%u first_domain=4",
                listed.empty()?"-":listed.c_str(),ownDomainCount);
        }
        // Attempt v119 (opt-in, default off): files/private-trace/diag-domain-split-render gives the CPU2 tile worker
        // 640b1000 permit domain 6 and 641c8000 domain 7, so they run beside the render thread 6408e000 (domain 2)
        // instead of taking turns with it (v114/v116 device: the render thread waited ~190 ms/s on domain-2 rotation).
        // What made domain 2 single-permit is covered: every VkQueue call holds recomp::gpu::sharedQueueMutex()
        // (vulkan_queue_lock.h), the generic renderer's entry points hold rendererMutex, the native engine runs under
        // the registry lock, and a WPTR store that finds the submission handler busy parks through the executor
        // (bindGuestGraphicsSubmissionWaiter) instead of being rejected. Read once, before any worker starts.
        static const bool splitRenderDomains=::access(RECOMP_APP_PATH("/private-trace/diag-domain-split-render"),F_OK)==0;
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_DOMAIN_SPLIT enabled=%u tile_worker_domain=%u hook_domain=%u",
            unsigned(splitRenderDomains),splitRenderDomains?6u:2u,splitRenderDomains?7u:2u);
        recomp::bootstrap::setGuestThreadDomainPolicy([](std::uint32_t token,std::uint32_t,std::uint32_t)->unsigned {
            for(unsigned i=0;i<ownDomainCount;++i)if(token==ownDomainTokens[i])return 4u+i; // v78 (listed tokens only)
            if(splitRenderDomains && token==0x640b1000u)return 6u; // v119
            if(splitRenderDomains && token==0x641c8000u)return 7u; // v119
            if(token==0x64141000u || token==0x64184000u)return 1u;
            // Attempt v30: every observed renderer/Vulkan caller in its own domain, so renderer state
            // stays single-permit (the lanes-audit S6 invariant, moved from domain 0 to domain 2)
            // while the main and other game threads run beside it: the D3D render thread 6408e000,
            // the CPU2 tile worker 640b1000 (second ring submitter, Vulkan runtime maintenance) and
            // 641c8000 (the resource-wait hook sub_836B1800 in v29's census). IRQ workers run inside
            // exclusive handoffs, so their domain does not matter.
            if(token==0x6408e000u || token==0x640b1000u || token==0x641c8000u)return 2u;
            // Attempt v57: the main game thread 63001000 in its own domain. v55 off-CPU profile: it waited
            // 71% of its wall time in acquirePermitLocked behind ~37 other domain-0 tasks and ran 22%.
            // It is not a renderer caller (v29 census), so the domain-2 invariant is unchanged.
            if(token==0x63001000u)return 3u;
            // v171 (goal step 3, H3): files/private-trace/diag-domain-each: every other guest thread gets its own permit
            // domain (8..63, first come first served, memoized per token so the policy stays a pure function of the
            // token), instead of all ~36 of them taking turns on domain 0 (v170: its busiest workers waited 4-5x their
            // running time; four own domains via diag-domain-own took the house scene from 69.4 to 51.1 ms). Renderer
            // callers stay in domain 2, audio in 1, main in 3; tokens past domain 63 fall back to domain 0.
            static const bool eachOwn=::access(RECOMP_APP_PATH("/private-trace/diag-domain-each"),F_OK)==0;
            if(eachOwn) {
                static std::mutex assignedMutex;static std::map<std::uint32_t,unsigned> assigned;static unsigned nextDomain=8;
                std::lock_guard guard(assignedMutex);
                if(const auto found=assigned.find(token);found!=assigned.end())return found->second;
                if(nextDomain<64u){assigned.emplace(token,nextDomain);return nextDomain++;}
            }
            return 0u;
        });
        v280GuestBase=addressSpace.base(); // v280 (the frame-limiter word)
#ifdef RECOMP_THOR_EDITION
        recomp::gpu::hud::guestBase().store(addressSpace.base(),std::memory_order_release); // hud3 (v585): the second screen reads the player's state
#endif /* RECOMP_THOR_EDITION */
        if(guestFrameLimitPolicy()!=iosguestlimit::Policy::Inherit)
            recomp_v280_apply_limiter(guestFrameLimitPolicy()==iosguestlimit::Policy::Sixty);
        recomp::bootstrap::GuestCooperativeExecutor executor;
        // v119: in split mode a busy submission handler parks the submitter (at most 5 s, then v118's rejection).
        if(splitRenderDomains)(void)recomp::bootstrap::bindGuestGraphicsSubmissionWaiter([&executor](const std::function<bool()>& ready){
            return executor.waitUntil(ready,std::chrono::steady_clock::now()+std::chrono::seconds(5));});
        struct UnbindSubmissionWaiter{~UnbindSubmissionWaiter(){(void)recomp::bootstrap::bindGuestGraphicsSubmissionWaiter({});}} unbindSubmissionWaiter;
        diagnostic_skip::executor=&executor; // DIAGNOSTIC: published for the retirement IRQ handoff.
        struct DiagnosticExecutorScope {~DiagnosticExecutorScope(){diagnostic_skip::executor=nullptr;}} diagnosticExecutorScope;
        if(livePostOutputColor) submitPostOutputSwap=[&](std::uint32_t word){
            // Admission, CPU storage and the action slot exist before native
            // presentation. Only the exclusive handoff may enter this worker;
            // an ordinary failed submission traps without scheduling peers.
            if(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(word))return false;
            recomp::bootstrap::noteNativeSubmissionMayCommitAt(23); // v183 site 23: this path publishes completion
            using Action=recomp::gpu::NativePostOutputSwapInterruptSequence::Action;
            auto actionSlot=std::make_shared<const Action*>(nullptr);
            const auto worker=registry.createNativePostOutputSwapInterruptWorker(word,[&](const auto& thread,const auto& initial){
                auto cpu=std::make_shared<PPCContext>();const auto token=thread.layout.threadAddress;
                cpu->r1.u64=initial.stackPointer;cpu->r13.u64=initial.pcrAddress;
                cpu->r3.u64=initial.argument0;cpu->r4.u64=initial.argument1;cpu->lr=0;
                return executor.enqueue(token,[&,cpu,initial,token,word,actionSlot]{
                    if(!*actionSlot)throw std::runtime_error("Second-swap IRQ entered without exclusive native action lease");
                    const auto& action=**actionSlot;
                    if(!registry.runNativePostOutputSwapInterruptScope(word,[&]{
                        if(!registry.beginNativePostOutputSwapInterrupt(word,action))
                            throw std::runtime_error("Second-swap IRQ prefix/ownership rejected");
                        if(!executor.runNativeNonYielding([&]{
                            recomp_host_guest_call_indirect(initial.address,*cpu,addressSpace.base());
                            if(cpu->r1.u64!=initial.stackPointer || cpu->r13.u64!=initial.pcrAddress)
                                throw std::runtime_error("Second-swap IRQ did not preserve owned stack/PCR");
                        },4096))throw std::runtime_error("Second-swap IRQ non-yielding scope rejected");
                    }))throw std::runtime_error("Second-swap IRQ scope/queued result rejected");
                    if(!registry.acknowledgeNativePostOutputSwapInterrupt(word,action))
                        throw std::runtime_error("Second-swap IRQ acknowledgement rejected");
                    if(!registry.markGuestThreadReturned(token,0))
                        throw std::runtime_error("Second-swap IRQ worker return rejected");
                });
            });
            if(!worker)return false;
            return registry.submitNativePostOutputWrappedSwapCommand(word,[&,actionSlot,token=worker->layout.threadAddress](const Action& action){
                if(*actionSlot)return false;
                *actionSlot=&action;
                struct Clear {const Action*& slot;~Clear(){slot=nullptr;}} clear{*actionSlot};
                // Parent remains parked, every peer remains parked, and the
                // native action reference stays alive until this worker returns.
                ExclusiveSiteLabel label("post-swap-irq"); // v110
                return executor.runNativeWorkerNext(token,4096);
            });
        };
        if (nativeIdentityScaler) submitScalerInterrupt=[&](std::uint32_t word) {
            const auto worker=registry.createNativeCommandInterruptWorker(word,[&](const auto& thread,const auto& initial) {
                auto cpu=std::make_shared<PPCContext>();const auto token=thread.layout.threadAddress;
                cpu->r1.u64=initial.stackPointer;cpu->r13.u64=initial.pcrAddress;
                cpu->r3.u64=initial.argument0;cpu->r4.u64=initial.argument1;cpu->lr=0;
                return executor.enqueue(token,[&,cpu,initial,token,word] {
                    if (!registry.runNativeCommandInterruptScope(word,[&] {
                        if (!registry.beginNativeScalerInterrupt(word))
                            throw std::runtime_error("Native scaler interrupt prefix/ownership preflight failed");
                        if (!executor.runNativeNonYielding([&] {
                            recomp_host_guest_call_indirect(initial.address,*cpu,addressSpace.base());
                            if (cpu->r1.u64!=initial.stackPointer || cpu->r13.u64!=initial.pcrAddress)
                                throw std::runtime_error("Native graphics interrupt did not preserve owned stack/PCR");
                        },4096)) throw std::runtime_error("Native graphics non-yielding scope rejected");
                    })) throw std::runtime_error("Native graphics interrupt scope/lock balance rejected");
                    if (!registry.acknowledgeNativeScalerInterrupt(word))
                        throw std::runtime_error("Native graphics interrupt did not produce its required acknowledgements");
                    if (!registry.markGuestThreadReturned(token,0))
                        throw std::runtime_error("Native graphics interrupt worker return rejected");
                });
            });
            if (!worker) return false;
            {ExclusiveSiteLabel label("scaler-irq");(void)executor.runNativeWorkerNext(worker->layout.threadAddress,4096);} // v110
            // Resumed original issuer; no peer ran during the handoff. Only
            // full native configuration/event completion may retire this root.
            return registry.completeNativeScalerWrapper(word);
        };
        if (!registry.bindThreadResumeHandler([&](const recomp::bootstrap::GuestPreparedThread& thread, std::int32_t priority) {
            const auto initial = recomp::bootstrap::makeGuestThreadEntry(thread);
            if (!initial || !recomp::bootstrap::hasExactGuestFunction(initial->address)) { return false; }
            try {
                auto cpu = std::make_shared<PPCContext>();
                cpu->r1.u64 = initial->stackPointer;
                cpu->r13.u64 = initial->pcrAddress;
                cpu->r3.u64 = initial->argument0;
                cpu->r4.u64 = initial->argument1;
                const auto token = thread.layout.threadAddress;
                // Lanes audit L5: describe the task so RENDER_DIAG_TASK_CREATED and the domain policy
                // see its start routine/context (v22 logged entry=00000000 for every task).
                recomp::bootstrap::noteGuestThreadStart(token, thread.startRoutine, thread.startContext,
                                                        thread.xapiStartup, addressSpace.base());
                return executor.enqueue(token, [&, cpu, initial = *initial, token] {
                    auto rendererQosBinding=iosrendererqos::experiment().bind(token);
                    recomp_host_guest_call_indirect(initial.address, *cpu, addressSpace.base());
                    const auto exitCode = initial.directEntry ? cpu->r3.u32 : 0U;
                    if (!registry.markGuestThreadReturned(token, exitCode)) {
                        throw std::runtime_error("Guest thread return state rejected");
                    }
                }, priority);
            } catch (const std::bad_alloc&) {
                return false;
            }
        })) {
            std::cerr << "trace setup failed: thread resume handler rejected\n";
            return 2;
        }
        if (experimentalNativeInitialization && (!hasTitleEnvironment
                || !registry.bindMountedDisc(static_cast<std::uint32_t>(titleValues[2])))) {
            std::cerr << "trace setup failed: mounted executable disc identity rejected\n"; return 2;
        }
        if (!registry.bindThreadExitHandler([&](auto token,auto code) {
                return executor.terminateCurrentThread(token,[&] {
                    if (!registry.markGuestThreadReturned(token,code)) return false;
#if defined(RECOMP_NATIVE_XMA)
                    __android_log_print(ANDROID_LOG_INFO,"RecompNativeThread","EXIT token=%08x code=%08x",token,code);
#endif
                    return true;
                });
            }) || !registry.bindVirtualCpuAffinity() || !registry.bindThreadDebugNaming() || !registry.bindNotificationService() || !registry.bindSignedOutUsers()
                || !registry.bindNoRawStorageDevices()
                || !registry.bindNoLaunchData()
                || !registry.bindUserLanguage(1) // Native test application's English preference.
                || !registry.bindUserCountry(103) // Test locale US; not host/account geography.
                || !registry.bindThreadPriorityHandler([&](auto token, auto priority) { return executor.setPriority(token, priority); })
                || !registry.bindNetworkLifecycle()
                // v211: a render thread with a deferred interrupt pending delivers it before it parks or sleeps (the tile
                // thread may be waiting for its event; nothing else delivers while the render thread is parked).
                || !registry.bindThreadWaitHandler([&](const auto& ready) {
                       if(deferredIrqPending.load(std::memory_order_acquire) && recomp::bootstrap::currentGuestThreadToken()==0x6408e000u)(void)deliverDeferredIrq(1);
                       return executor.waitUntil(ready); })
                || !registry.bindThreadTimedWaitHandler([&](const auto& ready, auto deadline) {
                       if(deferredIrqPending.load(std::memory_order_acquire) && recomp::bootstrap::currentGuestThreadToken()==0x6408e000u)(void)deliverDeferredIrq(1);
                       return executor.waitUntil(ready, deadline); })
                || !registry.bindThreadDelayHandler([&](auto deadline) {
                       if(deferredIrqPending.load(std::memory_order_acquire) && recomp::bootstrap::currentGuestThreadToken()==0x6408e000u)(void)deliverDeferredIrq(1);
                       return executor.delayUntil(deadline); })) {
            std::cerr << "trace setup failed: virtual CPU, debug naming, or wait binding rejected\n";
            return 2;
        }
        if (nativeEventWakeBoost && !registry.bindNativeEventWakeBoostHandler(
            [&](const auto* tokens,auto count){return executor.boostEventWaiters(tokens,count);})) {
            std::cerr << "trace setup failed: native event wake boost binding rejected\n";return 2;
        }
        if (nativeCriticalSectionWaits && !registry.bindNativeCriticalSectionWaits()) {
            std::cerr << "trace setup failed: native critical-section wait binding rejected\n";return 2;
        }
        if (nativeHighPriorityTiers && !registry.bindNativeHighPriorityTiers()) {
            std::cerr << "trace setup failed: native +3/+4 priority tier rejected\n";return 2;
        }
#if (defined(__ANDROID__) || defined(__APPLE__))
        if (nativeAudioRenderer && !registry.bindExperimentalAudioRenderer(nativeAudioRenderer,
            [&](const recomp::bootstrap::GuestPreparedThread& thread) {
                const auto initial=recomp::bootstrap::makeGuestThreadEntry(thread);
                if (!initial || !initial->directEntry || !recomp::bootstrap::hasExactGuestFunction(initial->address)) return false;
                auto cpu=std::make_shared<PPCContext>();const auto token=thread.layout.threadAddress;
                recomp::bootstrap::noteGuestThreadStart(token,thread.startRoutine,thread.startContext,
                                                        thread.xapiStartup,addressSpace.base()); // Lanes audit L5.
                // audio-ahead (audio_ahead.h): v431's demand rule = frames in flight < the REAL capacity rounded to packets;
                // the client behind AheadBackend admits 4 packets more, which only the gate's Early decision (main parked on
                // an engine-signalled handle, diag-au-ahead*) uses. The worker's token and v431's target, before it runs.
                recomp_au_ahead::workerStart(token,auAheadBackend.get());
                return executor.enqueue(token,[&,cpu,initial=*initial,token] {
                    using Demand=recomp::audio::NativeAudioRenderClient::Demand;
                    for(;;) { // (audio-ahead: out-of-line calls only; this lambda's PGO record is lost all the same: low-CPU loop)
                        recomp::bootstrap::GuestExecutionGuard::checkpoint("native_audio_callback_demand");
                        const auto demand=recomp_au_ahead::gateDemand(nativeAudioRenderer->poll(token),*nativeAudioRenderer);
                        if (demand==Demand::Failed) throw std::runtime_error("Native audio demand/counter/backend failure");
                        if (demand==Demand::Wait) {
                            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1);
                            // audio-ahead: v431's 1 ms backpressure wait, also ended by a new hint
                            if (!executor.waitUntil([deadline]{return recomp_au_ahead::holdOver(deadline);},deadline))
                                throw std::runtime_error("Native audio backpressure wait rejected");
                            continue;
                        }
                        { // v250 audio health: frames AAudio holds when the renderer is asked for the next packet (<= 0: starved)
                            const auto w=nativeAudio->framesWritten(),r=nativeAudio->framesRead();
                            if (w && r) {
                                const auto h=static_cast<long long>(*w)-static_cast<long long>(*r);
                                long long m=recomp_v250_audio_min.load(std::memory_order_relaxed);
                                while (h<m && !recomp_v250_audio_min.compare_exchange_weak(m,h,std::memory_order_relaxed)) {}
                                if (h<=0) recomp_v250_audio_starved.fetch_add(1,std::memory_order_relaxed);
                                recomp_v250_audio_packets.fetch_add(1,std::memory_order_relaxed);
                            }
                        }
                        if (!nativeAudioRenderer->beginCallback(token)) throw std::runtime_error("Native audio callback credit rejected");
                        cpu->r1.u64=initial.stackPointer;cpu->r13.u64=initial.pcrAddress;
                        cpu->r3.u64=initial.argument0;cpu->r4.u64=0;cpu->lr=0;
                        recomp_host_guest_call_indirect(initial.address,*cpu,addressSpace.base());
                        if (cpu->r1.u64!=initial.stackPointer || cpu->r13.u64!=initial.pcrAddress ||
                            !nativeAudioRenderer->finishCallback(token))
                            throw std::runtime_error("Native audio callback returned without one valid PCM packet or preserved stack/PCR");
                        recomp_au_ahead::workerProduced(*nativeAudioRenderer); // audio-ahead: production -> read latency
                    }
                });
            })) {std::cerr << "trace setup failed: native PCM renderer binding rejected\n";return 2;}
        if (nativeCategoryOneUnityGain && !registry.bindNativeCategoryOneUnityGain()) {
            std::cerr << "trace setup failed: native category1 unity-gain policy rejected\n";return 2;
        }
#endif
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE) && (defined(__ANDROID__) || defined(__APPLE__))
        bindRendererInlineCapture(&registry,inspectColorProgram?&*inspectColorProgram:nullptr,
            [&](const auto& f){
                // v212: the D3D hooks' gate entry is also a non-blocking delivery point for a deferred interrupt (v211: only
                // imports; the render thread builds command buffers for ms without an import: mean delivery latency 9 ms).
                if(deferredIrqPending.load(std::memory_order_acquire))(void)deliverDeferredIrq(0);
                return recomp::bootstrap::runNativeLockedGate(executor,f);},nativeGraphics.get(),
            [&](const auto& rgba,unsigned width,unsigned height) {
                reportFrameToPerformanceHint(); // attempt v36
                // v43: the v35 body (displayLive), after every queued frame, serialised with the presenter.
                return framePresenter.runSync([&]{return displayLive(rgba,width,height);});
            },[&](const auto& f){ExclusiveSiteLabel label("hook-256");return executor.runNativeNonYielding(f,256);}, // v110 labels
            [&](const auto& f){ExclusiveSiteLabel label("hook-4096");return executor.runNativeNonYielding(f,4096);},
            [&](unsigned device,const recomp::gpu::Gta5TileCommandInterrupt& request) {
                if(!nativeGraphics || request.predicated())return false;
                const auto result=tileSceneIrq(executor,"hook-tile-irq",[&]{
                  return recomp::bootstrap::executeNativeTileSceneHandoff(registry,executor,device,request,
                    [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileCommandInterrupt(request,memory,guest);},
                    [&](const auto&,const auto& entry) {
                        PPCContext cpu{};cpu.r1.u64=entry.stackPointer;cpu.r13.u64=entry.pcrAddress;
                        cpu.r3.u64=entry.argument0;cpu.r4.u64=entry.argument1;cpu.lr=0;
                        recomp_host_guest_call_indirect(entry.address,cpu,addressSpace.base());
                        return cpu.r1.u64==entry.stackPointer && cpu.r13.u64==entry.pcrAddress && cpu.lr==0;
                    });});
                __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_INITIAL_SCENE_IRQ admitted=%u returned=%u worker=%08x writes=%u waits=%u guest_returns=%u scene_completed=0 ring_retired=0",
                    unsigned(result.admitted),unsigned(result.returned),result.worker,result.scratchWrites,result.scratchWaits,result.guestReturns);
                return result.admitted && result.returned;
            },[&](unsigned device,const recomp::gpu::Gta5TileSceneContinuation& tail) {
                if(!nativeGraphics)return false;
                const auto& state=nativeGraphics->stateBatchSnapshot();
                const recomp::gpu::DrawBinState bins{{unsigned(state.binMask),unsigned(state.binMask>>32),
                    unsigned(state.binSelect),unsigned(state.binSelect>>32)},state.knownBinWords};
                const auto selected=tail.select(bins);
                if(!selected || !selected->interruptRequired)return false;
                const auto result=tileSceneIrq(executor,"hook-tile-tail",[&]{
                  return recomp::bootstrap::executeNativeTileSceneHandoff(registry,executor,device,tail.request(),
                    [&](const auto& memory,const auto& guest){return nativeGraphics->executeTileSceneContinuation(tail,memory,guest);},
                    [&](const auto&,const auto& entry) {
                        PPCContext cpu{};cpu.r1.u64=entry.stackPointer;cpu.r13.u64=entry.pcrAddress;
                        cpu.r3.u64=entry.argument0;cpu.r4.u64=entry.argument1;cpu.lr=0;
                        recomp_host_guest_call_indirect(entry.address,cpu,addressSpace.base());
                        return cpu.r1.u64==entry.stackPointer && cpu.r13.u64==entry.pcrAddress && cpu.lr==0;
                    });});
                __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_LIVE_SCENE_IRQ admitted=%u returned=%u worker=%08x writes=%u waits=%u guest_returns=%u ring_retired=0",
                    unsigned(result.admitted),unsigned(result.returned),result.worker,result.scratchWrites,result.scratchWaits,result.guestReturns);
                return result.admitted && result.returned;
            },{[&]{return executor.currentTaskToken();},[&](unsigned token){return executor.parkedTask(token);},
               [&](const auto& ready){return executor.waitUntil(ready);}});
        struct ClearRendererCapture {~ClearRendererCapture(){unbindRendererInlineCapture();}} clearRendererCapture;
        {
            // v43: generic frames go to the presenter thread once the scan-out state can no longer change
            // (identity scaler configured and the one-shot native gamma completed: after both, nothing
            // writes what mapNativeScanout reads, so the presenter thread may read it). Until then, and
            // after a failed queued display, they are displayed synchronously as before.
            auto scanoutFinal=std::make_shared<bool>(false);
            auto lutSent=std::make_shared<bool>(false); // v255
            bindRendererDiagnosticV43([&,scanoutFinal,lutSent](const std::vector<std::uint8_t>& rgba,unsigned width,unsigned height) {
                // v255: an empty frame is one the renderer already presented to the window itself (direct_present.h).
                if(rgba.empty()){reportFrameToPerformanceHint();return true;}
                if(!liveCallbacks || !liveCallbacks->orderedRgba || !nativeGraphics)return false;
                if(!*scanoutFinal)*scanoutFinal=nativeGraphics->nativeScalerConfigured() &&
                    registry.completedNativeDisplayGamma().has_value();
                if(iosSharedDisplay) {
                    if(*scanoutFinal)iosScanoutReady.store(true,std::memory_order_release);
                    return false; // first CPU frame establishes immutable map outside producer gate
                }
                // v255: once the scan-out can no longer change, its mapping as three tables for the direct present (the
                // mapping of a ramp of every value, checked per value by lutFromMapped; not sent if it is not per value).
                if(*scanoutFinal && !*lutSent && recomp::gpu::direct::presenter().gpuWanted()) {
                    *lutSent=true;
                    const auto ramp=recomp::gpu::direct::rampSource();std::vector<std::uint8_t> mapped;recomp::gpu::direct::Lut lut{};
                    const bool ok=nativeGraphics->mapNativeScanout(ramp.data(),ramp.size(),1280,720,mapped) &&
                        recomp::gpu::direct::lutFromMapped(mapped.data(),mapped.size(),lut);
                    if(ok)recomp::gpu::direct::presenter().setLut(lut);
                    else __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIRECT_PRESENT lut=0 why=mapping");
                }
                if(!*scanoutFinal || !framePresenter.submit(rgba.data(),rgba.size(),width,height))return false;
                const auto c=framePresenter.counters();
                if(!(c.submitted&255))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_DIAG_PRESENTER submitted=%llu delivered=%llu replaced=%llu failed=%llu sync_calls=%llu refused=%llu",
                    (unsigned long long)c.submitted,(unsigned long long)c.delivered,(unsigned long long)c.replaced,
                    (unsigned long long)c.failed,(unsigned long long)c.syncCalls,(unsigned long long)c.refused);
                reportFrameToPerformanceHint(); // attempt v36 (queued frames; the sync path reports its own)
                return true;
            },[&]{return recomp::bootstrap::nativeLockedGateAdmits(executor);});
            // v158: the interpolated + real frame pair (only once frames are queued at all: same scan-out condition).
            bindRendererDiagnosticV158Pair([&,scanoutFinal](const std::vector<std::uint8_t>& mid,const std::vector<std::uint8_t>& rgba,unsigned width,
                    unsigned height,std::chrono::microseconds delay) {
                if(iosSharedDisplay)return false; // preserve synchronous sequence for excluded interpolation
                if(!liveCallbacks || !liveCallbacks->orderedRgba || !nativeGraphics || !*scanoutFinal)return false;
                if(!framePresenter.submitPair(mid.data(),rgba.data(),rgba.size(),width,height,delay))return false;
                const auto c=framePresenter.counters();
                if(!(c.pairs&255))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                    "RENDER_DIAG_PRESENTER_PAIRS pairs=%llu delivered=%llu replaced=%llu mids_dropped=%llu delay_us=%lld",
                    (unsigned long long)c.pairs,(unsigned long long)c.delivered,(unsigned long long)c.replaced,(unsigned long long)c.midsDropped,
                    (long long)delay.count());
                reportFrameToPerformanceHint();
                return true;
            });
        }
#endif
        NativeDiscBridgeScope nativeDiscScope(experimentalNativeInitialization ? &registry : nullptr,
                                               addressSpace.base());
        cooperativeResult = executor.run(guestThreadToken,
                [&] { _xstart(context, addressSpace.base()); },
                {functionEntryBudget, static_cast<std::uint32_t>(quantum), 65, nativeHighPriorityTiers},
                [&](std::uint32_t token) {
                    if (!registry.bindGuestThreadToken(token)) {
                        throw std::runtime_error("Cooperative thread binding rejected");
                    }
                }, traceSink);
        execution = cooperativeResult.execution;
        submitScalerInterrupt={}; // Captures executor; clear before its destruction.
        submitPostOutputSwap={};
    } else {
        execution = guard.run(functionEntryBudget,
                [&] { _xstart(context, addressSpace.base()); }, traceSink);
    }
    // All executor workers have joined. Capture mapping-dependent diagnostic
    // values now, before resolver teardown invalidates hasExactGuestFunction.
    // Reporting below uses this owned snapshot, not a re-enabled guest resolver.
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE)
    if(!saveStoppedRendererDraw((std::filesystem::path(argv[1]).parent_path()/"renderer-first-unsupported").string()))
        std::cerr<<"trace evidence failed: unsupported renderer draw save failed\n";
#if (defined(__ANDROID__) || defined(__APPLE__))
    dumpStoppedRendererTextureDescriptions(registry,nativeGraphics.get(),inspectColorProgram?&*inspectColorProgram:nullptr);
#else
    dumpStoppedRendererTextureDescriptions(registry);
#endif
#endif
    const auto submission = execution.scalarAccess
        ? recomp::bootstrap::inspectStoppedGpuSubmission(*execution.scalarAccess, registry)
        : recomp::bootstrap::StoppedGpuSubmission{};
    std::optional<recomp::bootstrap::GuestImportRegistry::StoppedPostOutputSwapInterrupt> stoppedPostOutputSwapInterrupt;
    std::uint32_t stoppedPostOutputSwapInterruptRejection=0;
    if(livePostOutputColor && submission.status==recomp::bootstrap::StoppedGpuSubmissionStatus::Decoded)
        stoppedPostOutputSwapInterrupt=registry.inspectStoppedPostOutputSwapInterrupt(submission.writeWord,&stoppedPostOutputSwapInterruptRejection);
    std::optional<recomp::bootstrap::GuestImportRegistry::StoppedDeferredSwapInterrupt> stoppedDeferredSwapInterrupt;
    std::uint32_t stoppedDeferredSwapInterruptRejection=0;
    if(liveDeferredColor && submission.status==recomp::bootstrap::StoppedGpuSubmissionStatus::Decoded)
        stoppedDeferredSwapInterrupt=registry.inspectStoppedDeferredSwapInterrupt(submission.writeWord,&stoppedDeferredSwapInterruptRejection);
    recomp_unbind_generated_guest_import_registry();
    const auto experimentalClockReads = recomp::bootstrap::experimentalGuestGpuClockReads();
    recomp::bootstrap::resetExperimentalGuestGpuClock();
    recomp::bootstrap::resetGuestGraphicsSubmission();
    recomp::bootstrap::resetGuestXmaRegisters();
#if defined(RECOMP_NATIVE_XMA)
    if (nativeXma) __android_log_print(ANDROID_LOG_INFO,"RecompNativeXma","RESULT frames=%llu pcm_bytes=%llu boundary=%s",
        static_cast<unsigned long long>(nativeXma->decodedFrames()),static_cast<unsigned long long>(nativeXma->writtenPcmBytes()),
        nativeXma->failure().c_str());
    if (nativeXma) std::cerr<<"native XMA result: frames="<<nativeXma->decodedFrames()
        <<" pcm_bytes="<<nativeXma->writtenPcmBytes()<<" boundary="<<nativeXma->failure()<<"\n";
#endif
    recomp::bootstrap::resetGuestScalarDataImportGuard();
    recomp::bootstrap::resetGuestScalarUptimeClock();
    const auto rejectedIndirect = recomp::bootstrap::stoppedGuestIndirectCall();
    recomp::bootstrap::resetGuestIndirectCallResolver();

    const auto fileIo = registry.fileIoStatistics();
    recomp::gpu::Pm4IndirectGraph indirectGraph;
    recomp::gpu::Pm4GraphError indirectError;
    const bool inspectIndirect = submission.status == recomp::bootstrap::StoppedGpuSubmissionStatus::Decoded;
    if (inspectIndirect) {
        (void)recomp::gpu::inspectPm4IndirectGraph(submission.snapshot,
            [&](auto address, auto bytes) { return registry.snapshotStoppedGraphicsPhysical(address, bytes); },
            indirectGraph, indirectError);
    }
    recomp::gpu::StoppedGpuWork stoppedWork;
    recomp::gpu::StoppedGpuWorkError stoppedWorkError;
    const bool inspectWork = inspectIndirect && indirectError.failure == recomp::gpu::Pm4GraphFailure::None;
    if(captureStoppedPm4 && inspectIndirect) {
        // One bounded post-join report; no per-draw logging or change to admission.
        std::cerr<<"STOPPED_SUBMISSION_DISPATCH ";submissionDiagnostic.json(std::cerr);std::cerr<<'\n';
        if(inspectWork) {
            const auto saved=recomp::trace::saveStoppedSubmission(std::filesystem::path(argv[1]).parent_path(),
                submission.snapshot,indirectGraph,submission.readWord,submission.writeWord,submissionDiagnostic);
            std::cerr<<"STOPPED_SUBMISSION_BINARY_SAVE status="<<static_cast<unsigned>(saved)
                <<" words="<<indirectGraph.totalWords<<" executed=0 ring_advanced=0\n";
        }
    }
    if(captureStoppedPm4 && inspectWork) {
        // Workers are joined; this extra input evidence cannot resume a guest,
        // issue a draw, evaluate a predicate or advance the command queue.
        recomp::gpu::StoppedDmaInputs inputs;
        // A uint16 triangle index count may be odd (18 observed draws are).
        // Use the allocation-checked byte reader, not the PM4 dword reader.
        const bool captured=recomp::gpu::captureStoppedDmaInputs(submission.snapshot,indirectGraph,
                [&](auto a,auto n){return registry.copySerializedGraphicsData(a,n);},inputs);
        if(!captured)std::cerr<<"trace evidence failed: stopped DMA input collection failed\n";
        if(captured && !inputs.inputs.empty()) {
            const auto directory=std::filesystem::path(argv[1]).parent_path()/"stopped-dma-inputs";
            std::error_code error;
            bool ok=std::filesystem::create_directory(directory,error) && !error;
            if(ok) {
                std::ofstream meta(directory/"inputs.json");
                meta<<"{\"version\":1,\"requested_bytes\":"<<inputs.requestedBytes<<",\"unavailable\":"<<inputs.unavailable
                    <<",\"execution_order\":false,\"predicates_evaluated\":false,\"guest_writes\":false,\"inputs\":[";
                for(std::size_t i=0;i<inputs.inputs.size();++i) {
                    const auto& input=inputs.inputs[i];
                    if(i)meta<<',';
                    meta<<"{\"node\":"<<input.node<<",\"offset_words\":"<<input.offsetWords
                        <<",\"failure\":"<<unsigned(input.failure)<<",\"physical_address\":"<<input.layout.physicalAddress
                        <<",\"index_count\":"<<input.layout.indexCount<<",\"predicated\":"<<(input.layout.predicate.predicated?"true":"false");
                    if(!input.bytes.empty()) {
                        const auto name="indices-"+std::to_string(i)+".bin";
                        std::ofstream file(directory/name,std::ios::binary);
                        file.write(reinterpret_cast<const char*>(input.bytes.data()),input.bytes.size());
                        file.close();ok=bool(file) && ok;
                        meta<<",\"path\":\""<<name<<"\",\"byte_count\":"<<input.bytes.size();
                    }
                    meta<<'}';
                }
                meta<<"]}";meta.close();ok=bool(meta) && ok;
            }
            if(!ok)std::cerr<<"trace evidence failed: stopped DMA input save failed\n";
        }
    }
    // Native-only envelope is deliberately invalid retail PM4. Read-only
    // diagnostics may inspect its owned wrapper without making the graph valid
    // or granting execution permission. All executor writers are joined here.
    std::optional<recomp::bootstrap::GuestImportRegistry::NativePendingSubmission> nativePendingSubmission;
    std::optional<recomp::bootstrap::GuestImportRegistry::NativePendingSubmission> deferredPendingSubmission;
    std::optional<recomp::gpu::NativeScalerWrapper> nativeScalerWrapper;
    if(nativeSwapCommands && inspectIndirect && indirectError.failure==recomp::gpu::Pm4GraphFailure::DecodeRejected &&
        indirectError.parent==0 && indirectError.offsetWords==0 &&
        indirectError.decode.failure==recomp::gpu::Pm4Failure::TruncatedPacket && indirectError.decode.opcode==0x53){
        auto pending=registry.inspectStoppedNativePendingSubmission(submission.writeWord);
        if(!pending && nativeIdentityScaler)pending=registry.inspectStoppedNativeScalerSubmission(submission.writeWord);
        if(!pending && livePostOutputColor){
            deferredPendingSubmission=registry.inspectStoppedDeferredSwapSubmission(submission.writeWord);
            pending=deferredPendingSubmission;
        }
        if(pending && pending->commandOffsetWords==indirectError.decode.offsetWords)nativePendingSubmission=pending;
        else deferredPendingSubmission.reset();
    }
    if (nativeIdentityScaler && nativePendingSubmission)
        nativeScalerWrapper=registry.inspectStoppedNativeScalerWrapper(submission.writeWord);
    if (inspectWork) (void)recomp::gpu::inspectStoppedGpuWork(submission.snapshot, indirectGraph, stoppedWork, stoppedWorkError);
    recomp::gpu::StoppedMemoryEffects stoppedMemory;
    recomp::gpu::StoppedMemoryError stoppedMemoryError;
    if (inspectWork) (void)recomp::gpu::inspectStoppedMemoryEffects(submission.snapshot, indirectGraph, stoppedMemory, stoppedMemoryError);
    std::cout << ProtocolHeader << '\n'
              << "stop_reason="
              << recomp::bootstrap::guestExecutionStopReasonName(execution.reason)
              << '\n'
              << "completed_function_entries=" << execution.completedFunctionEntries << '\n'
              << "debug_message_count=" << debugMessageCount << '\n'
              << "debug_message_bytes=" << debugMessageBytes << '\n'
              << "dropped_function_labels=" << functionLabels.dropped() << '\n'
              << "validated_mapping_count=" << validatedMappingCount << '\n'
              << "file_opens=" << fileIo.opens << '\n'
              << "file_reads=" << fileIo.reads << '\n'
              << "file_bytes=" << fileIo.bytes << '\n'
              << "file_seeks=" << fileIo.seeks << '\n';
    const auto discStats = registry.nativeDiscStatistics();
    if (hasKernelVersion) std::cout << "native_kernel_version=" << kernelVersionValues[0]
        << ',' << kernelVersionValues[1] << ',' << kernelVersionValues[2] << '\n';
    if (discStats[0]) std::cout << "native_mounted_disc=" << discStats[0] << ','
        << discStats[1] << ',' << discStats[2] << '\n';
    if (hasGraphics) std::cout << "graphics_initialized=" << registry.graphicsInitialized() << '\n';
    if (imagePageProtections) std::cout << "xex_image_protections=" << imageBase << ',' << titleValues[1] << '\n';
    if (disconnectedInput) std::cout << "disconnected_input=1\n";
    if(nativeInput)std::cout << "native_input=1\n";
    if (nativeEventWakeBoost) {
        const auto stats=registry.nativeEventWakeBoostStatistics();
        std::cout << "native_event_wake_boost=" << stats.signalRequests << ',' << stats.threadsBoosted << ',' << stats.threadsSuppressed << '\n';
    }
    if (nativeCategoryOneUnityGain)
        std::cout << "native_category1_gain=1065353216," << registry.nativeCategoryOneGainQueries() << '\n';
    if (nativeCriticalSectionWaits) {
        const auto s=registry.nativeCriticalSectionWaitStatistics();
        std::cout << "native_critical_section_waits=" << s[0] << ',' << s[1] << ',' << s[2] << ',' << s[3] << '\n';
    }
    if (nativeHighPriorityTiers)std::cout << "native_high_priority_tiers=" << registry.nativeHighPriorityTiersRequests() << '\n';
    if (experimentalIdleCachePair) std::cout << "experimental_idle_cache_pairs=" << registry.experimentalIdleCachePairs() << '\n';
    if(nativeScalerTiming){using Timing=recomp::gpu::NativeScalerTiming;
        std::cout<<"native_scaler_timing=1,"<<Timing::NominalGpuMHz<<','<<Timing::PixelClockWholeMHz<<','<<Timing::PixelClockHz<<','
            <<Timing::TotalWidth<<','<<Timing::TotalHeight<<','<<Timing::RefreshHz<<','<<registry.nativeScalerTimingQueries()<<'\n';
    }
    if(nativeIdentityScaler){const auto s=registry.nativeScalerCommandStatus();
        std::cout<<"native_scaler_command="<<s.issued<<','<<s.previousReadWord<<','<<s.words<<','<<s.issuerThread<<'\n';
        const auto storage=registry.completedNativeScalerStorageCoherency();
        std::cout<<"native_scaler_storage_coherency=";
        if(storage)std::cout<<1<<','<<storage->previousWord<<','<<storage->readWord<<','<<storage->commandPhysical<<','
            <<storage->request.physicalBase<<','<<storage->request.byteCount<<','<<storage->request.cacheFlags;
        else std::cout<<"0,0,0,0,0,0,0";
        std::cout<<'\n';
    }
    if(experimentalOwnedColor){const auto completed=registry.completedNativeOwnedColorBatch();
        std::cout<<"native_owned_color_batch=";
        if(completed)std::cout<<1<<','<<completed->previousWord<<','<<completed->readWord<<','<<completed->draws<<','
            <<completed->eventWords<<','<<completed->copyBytes<<','<<completed->edramBytes<<','<<completed->nativeFences<<','<<completed->policy;
        else std::cout<<"0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
    }
    if(nativeSwapCommands){const auto swap=registry.nativeSwapCommandStatus();
        std::cout<<"native_swap_command="<<swap.issued<<','<<swap.pending<<','<<swap.presentQueued<<','
            <<swap.commandPhysical<<','<<swap.previousWord<<','<<swap.readWord<<','<<swap.issuedReadWord<<'\n';
        const auto mode=registry.nativeDisplayModeStatus();
        std::cout<<"native_display_mode="<<mode.selected<<','<<mode.flags<<','<<mode.queries<<'\n';
        const auto ordered=registry.completedNativeCommandStorageCoherency();
        std::cout<<"native_command_storage_coherency=";
        if(ordered)std::cout<<1<<','<<ordered->previousWord<<','<<ordered->readWord<<','<<ordered->request.physicalBase<<','
            <<ordered->request.byteCount<<','<<ordered->request.cacheFlags;
        else std::cout<<"0,0,0,0,0,0";
        std::cout<<'\n';
        const auto wrapped=registry.completedNativeWrappedSwap();
        std::cout<<"native_wrapped_swap=";
        if(wrapped){const auto& w=*wrapped;
            std::cout<<1<<','<<w.previousWord<<','<<w.readWord<<','<<w.childPhysical<<','<<w.gpu.nativeFences<<','
                <<w.gpu.beforeCompletionNs<<','<<w.gpu.afterCompletionNs;
            for(unsigned i=0;i<4;++i){std::uint32_t value=0;
                for(unsigned b=0;b<4;++b)value|=std::uint32_t{w.gpu.eventBytes[i*4+b]}<<(i<2?8*b:24-8*b);
                std::cout<<','<<w.eventPhysical[i]<<','<<value;
            }
        }else std::cout<<"0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        std::cout<<"native_pending_submission=";
        if(nativePendingSubmission){const auto& p=*nativePendingSubmission;
            std::cout<<1<<','<<p.rootPhysical<<','<<p.childPhysical<<','<<p.childWords<<','<<p.commandOffsetWords<<','<<p.readWord<<','<<p.writeWord;
        }else std::cout<<"0,0,0,0,0,0,0";
        std::cout<<'\n';
    }
    if (nativeIdentityScaler) {
        const auto scope=registry.nativeCommandInterruptScopeStatus();
        const auto interrupt=registry.nativeScalerInterruptStatus();
        std::cout<<"native_scaler_interrupt="<<scope.attempted<<','<<scope.returned<<','<<scope.balanced<<','<<scope.worker<<','
            <<scope.heldLocks<<','<<scope.lockPairs[0]<<','<<scope.lockPairs[1]<<','<<interrupt.prefixAttempted<<','
            <<interrupt.prefixReady<<','<<interrupt.ackAttempted<<','<<interrupt.acknowledged<<','<<interrupt.rejection<<'\n';
        std::cout<<"native_scaler_pacing=";
        for (unsigned i=0;i<interrupt.pacing.size();++i) std::cout<<(i?",":"")<<interrupt.pacing[i];
        std::cout<<'\n';
        const auto completion=registry.completedNativeScalerWrapper();
        std::cout<<"native_scaler_completion=";
        if (completion) {
            std::cout<<1<<','<<completion->previousWord<<','<<completion->readWord<<','<<completion->gpu.nativeFences;
            for (unsigned i=0;i<2;++i) {unsigned value=0;
                for (unsigned b=0;b<4;++b) value=(value<<8)|completion->gpu.eventBytes[i*4+b];
                std::cout<<','<<completion->eventPhysical[i]<<','<<value;
            }
        } else std::cout<<"0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        const auto postStorage=registry.completedNativePostScalerStorageCoherency();
        std::cout<<"native_post_scaler_storage=";
        if (postStorage) {
            const auto& s=*postStorage;
            std::cout<<s.batches<<','<<s.firstWord<<','<<s.previousWord<<','<<s.readWord<<','<<
                s.request.physicalBase<<','<<s.request.byteCount<<','<<s.request.cacheFlags<<','<<s.batches;
        } else std::cout<<"0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        std::cout<<"native_scaler_wrapper_plan=";
        if (nativeScalerWrapper) {
            const auto registration=registry.graphicsInterruptRegistration();
            std::optional<std::uint32_t> mask,base;
#if (defined(__ANDROID__) || defined(__APPLE__))
            if (nativeGraphics) { mask=nativeGraphics->retainedRegister(0x1DC);base=nativeGraphics->retainedRegister(0x1DD); }
#endif
            std::cout<<1<<','<<nativeScalerWrapper->scratchPhysical()<<','<<nativeScalerWrapper->callbackAddress()<<','
                <<nativeScalerWrapper->callbackContext()<<','<<(registration?registration->routine:0)<<','
                <<(registration?registration->userData:0)<<','<<mask.has_value()<<','<<mask.value_or(0)<<','
                <<base.has_value()<<','<<base.value_or(0);
        } else std::cout<<"0,0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        const auto gamma=registry.completedNativeDisplayGamma();
        std::cout<<"native_display_gamma=";
        if(gamma) {
            std::cout<<1<<','<<gamma->previousWord<<','<<gamma->readWord<<','<<gamma->childPhysical<<','<<gamma->frontbufferPhysical<<','<<
                gamma->gpu.nativeFences<<','<<gamma->gpu.presentQueued;
            for(unsigned i=0;i<2;++i){unsigned value=0;for(unsigned b=0;b<4;++b)value=(value<<8)|gamma->gpu.eventBytes[i*4+b];
                std::cout<<','<<gamma->eventPhysical[i]<<','<<value;}
        }else std::cout<<"0,0,0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        const auto displayStorage=registry.completedNativePostDisplayStorageCoherency();
        std::cout<<"native_post_display_storage=";
        if(displayStorage){const auto& s=*displayStorage;
            std::cout<<s.batches<<','<<s.firstWord<<','<<s.previousWord<<','<<s.readWord<<','<<
                s.request.physicalBase<<','<<s.request.byteCount<<','<<s.request.cacheFlags<<','<<s.batches;
        }else std::cout<<"0,0,0,0,0,0,0,0";
        std::cout<<'\n';
    }
    if(nativeDisplayOutput){
        const auto output=registry.completedNativeDisplayOutput();
        std::cout<<"native_display_output=";
        if(output){std::cout<<1<<','<<output->previousWord<<','<<output->readWord<<','<<output->childPhysical<<','<<
                output->gpu.nativeFences<<','<<output->gpu.presentQueued;
            for(unsigned i=0;i<2;++i){unsigned value=0;for(unsigned b=0;b<4;++b)value=(value<<8)|output->gpu.eventBytes[i*4+b];
                std::cout<<','<<output->eventPhysical[i]<<','<<value;}
        }else std::cout<<"0,0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        const auto outputStorage=registry.completedNativePostOutputStorageCoherency();
        std::cout<<"native_post_output_storage=";
        if(outputStorage){const auto& s=*outputStorage;
            std::cout<<s.batches<<','<<s.firstWord<<','<<s.previousWord<<','<<s.readWord<<','<<
                s.request.physicalBase<<','<<s.request.byteCount<<','<<s.request.cacheFlags<<','<<s.batches;
        }else std::cout<<"0,0,0,0,0,0,0,0";
        std::cout<<'\n';
    }
    if(liveDeferredColor){
        const auto runtimeMaintenance=registry.completedNativeRuntimeMaintenancePoints();
        std::cout<<"native_runtime_maintenance=";
        if(runtimeMaintenance){const auto& s=*runtimeMaintenance;
            std::cout<<s.batches<<','<<s.previousWord<<','<<s.readWord<<','<<s.draws<<','<<s.shaderLoads<<','<<
                s.invalidations<<','<<s.fencePhysical<<','<<s.fenceValue<<','<<s.commandCursor;
        }else std::cout<<"0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        // Reporting only: attempts,rejection,reachedExecute,failedLatch then the
        // actual allocation owners, so a live rejection is explainable.
        const auto maintenanceReject=registry.nativeRuntimeMaintenanceDiagnostic();
        std::cout<<"native_runtime_maintenance_rejection="<<maintenanceReject.attempts<<','<<maintenanceReject.rejection<<','
            <<maintenanceReject.reachedExecute<<','<<maintenanceReject.failedLatch<<','
            <<maintenanceReject.ringAllocation<<','<<maintenanceReject.writeBackAllocation<<','
            <<maintenanceReject.fencePhysical<<','<<maintenanceReject.fenceAllocation<<','
            <<maintenanceReject.cursorPhysical<<','<<maintenanceReject.cursorAllocation<<','<<maintenanceReject.leaseCount;
        for(unsigned i=0;i<maintenanceReject.leaseCount && i<maintenanceReject.leasePhysical.size();++i)
            std::cout<<','<<maintenanceReject.leasePhysical[i]<<','<<maintenanceReject.leaseAllocation[i];
        std::cout<<'\n';
        const auto runtimeStorage=registry.completedNativeRuntimeCommandStorageCoherency();
        std::cout<<"native_runtime_command_storage=";
        if(runtimeStorage){const auto& s=*runtimeStorage;
            std::cout<<s.batches<<','<<s.firstWord<<','<<s.previousWord<<','<<s.readWord<<','<<
                s.request.physicalBase<<','<<s.request.byteCount<<','<<s.request.cacheFlags;
        }else std::cout<<"0,0,0,0,0,0,0";
        std::cout<<'\n';
        const auto status=registry.nativeDeferredColorPrefixStatus();
        const auto completed=registry.completedNativeDeferredColorPrefix();
        const auto ring=registry.graphicsRingConfiguration();bool repeatRejected=false;
#if (defined(__ANDROID__) || defined(__APPLE__))
        if(status[0] && inspectColorProgram)repeatRejected=
            !registry.executeExperimentalDeferredColorPrefix(*inspectColorProgram,{},submission.writeWord);
#endif
        std::cout<<"native_deferred_color_prefix="<<status[0]<<','<<status[1]<<','<<status[2]<<','<<bool(completed);
        if(completed){const auto& c=*completed;
            std::cout<<','<<c.previousWord<<','<<c.pendingWriteWord<<','<<c.prefixWords<<','<<c.inputCount<<','<<c.copyBytes<<','
                <<c.nativeFences<<','<<c.leadingSetupFences;
        }else std::cout<<",0,0,0,0,0,0,0";
        std::cout<<','<<repeatRejected<<','<<(ring?ring->readWord:0)<<'\n';
        std::cout<<"stopped_deferred_swap_interrupt=[";
        if(stoppedDeferredSwapInterrupt){const auto& s=*stoppedDeferredSwapInterrupt;
            std::cout<<1<<','<<s.submission.readWord<<','<<s.submission.writeWord<<','<<(s.submission.childWords-129)<<','
                <<s.wrapper.scratchPhysical()<<','<<s.wrapper.callbackAddress()<<','<<s.wrapper.callbackContext()<<','
                <<s.device<<','<<s.handler<<','<<s.frequency;
            for(auto word:s.deviceValues)std::cout<<','<<word;
            for(auto word:s.queuedSurfaces)std::cout<<','<<word;
            for(auto word:s.scratch)std::cout<<','<<word;
            for(auto word:s.pacingRecord)std::cout<<','<<word;
        }else std::cout<<0<<','<<stoppedDeferredSwapInterruptRejection;
        std::cout<<"]\n";
    }
    if(livePostOutputColor){
        const auto status=registry.nativePostOutputColorStatus();
        std::cout<<"native_post_output_color_status="<<status[0]<<','<<status[1]<<','<<status[2]<<'\n';
        const auto completed=registry.completedNativePostOutputColorBatch();
        std::cout<<"native_post_output_color=";
        if(completed){const auto& c=completed->color;
            std::cout<<1<<','<<c.previousWord<<','<<c.readWord<<','<<c.draws<<','<<c.eventWords<<','<<c.copyBytes<<','<<c.edramBytes<<','
                <<c.nativeFences<<','<<completed->leadingSetupFences<<','<<c.policy;
        }else std::cout<<"0,0,0,0,0,0,0,0,0,0";
        std::cout<<'\n';
        const auto context=registry.nativePostOutputSystemContext();
        std::cout<<"native_post_output_system_context=";
        if(context)std::cout<<1<<','<<context->stackPointer<<','<<context->buffer<<','<<context->identifierOutput<<','<<context->readWord<<','<<context->policy;
        else std::cout<<"0,0,0,0,0,0";
        std::cout<<'\n';
    }
    if(livePostOutputColor){
        const auto storage=registry.completedNativePostOutputSwapStorageCoherency();
        const auto nextContext=registry.nativePostSwapSystemContext();
        std::cout<<"native_post_swap_system_context=";
        if(nextContext)std::cout<<1<<','<<nextContext->stackPointer<<','<<nextContext->buffer<<','<<nextContext->identifierOutput<<','<<nextContext->readWord<<','<<nextContext->policy;
        else std::cout<<"0,0,0,0,0,0";
        std::cout<<'\n';
        std::cout<<"native_deferred_swap_request=[";
        if(const auto deferred=registry.nativeDeferredSwapRequest()){
            const auto& r=deferred->request;const auto& owner=deferred->imageOwner;
            std::cout<<1<<','<<3<<','<<r.commandVirtual<<','<<r.commandPhysical<<','<<r.allocationBase<<','<<r.allocationBytes<<','
                <<r.protection<<','<<r.frontbufferVirtual<<','<<r.format<<','<<r.colorSpace<<','<<r.width<<','<<r.height<<','<<r.identifierWord;
            for(auto word:r.fetch)std::cout<<','<<word;
            std::cout<<','<<deferred->imagePhysical<<','<<owner.virtualBase<<','<<owner.byteCount<<','<<owner.protection<<','
                <<owner.writable<<','<<deferred->issuedReadWord;
        }else std::cout<<0;
        std::cout<<"]\n";
        std::cout<<"native_deferred_swap_storage=";
        if(const auto storage=registry.completedNativeDeferredSwapStorageCoherency())
            std::cout<<1<<','<<storage->previousWord<<','<<storage->readWord<<','<<storage->request.physicalBase<<','
                <<storage->request.byteCount<<','<<storage->request.cacheFlags;
        else std::cout<<"0,0,0,0,0,0";
        std::cout<<'\n';
        std::cout<<"native_post_output_swap_storage=";
        if(storage)std::cout<<1<<','<<storage->previousWord<<','<<storage->readWord<<','<<storage->request.physicalBase<<','
            <<storage->request.byteCount<<','<<storage->request.cacheFlags;
        else std::cout<<"0,0,0,0,0,0";
        std::cout<<'\n';
        const auto swap=registry.nativePostOutputSwapStatus();
        std::cout<<"native_post_output_swap="<<swap.issued<<','<<swap.pending<<','<<swap.presentQueued<<','<<swap.commandPhysical<<','
            <<swap.previousWord<<','<<swap.readWord<<','<<swap.issuedReadWord<<'\n';
        const auto wrapped=registry.completedNativePostOutputWrappedSwap();
        std::cout<<"native_post_output_wrapped_swap=";
        if(wrapped){const auto& c=*wrapped;const auto irq=registry.nativePostOutputSwapInterruptScopeStatus();const auto& s=irq.scope;
            const auto imagePhysical=c.imageOwner.physicalBase+(c.imageRequest.frontbufferVirtual-c.imageOwner.virtualBase);
            std::cout<<1<<','<<c.previousWord<<','<<c.readWord<<','<<c.childPhysical<<','<<imagePhysical<<','
                <<c.imageOwner.physicalBase<<','<<c.imageOwner.virtualBase<<','<<c.imageOwner.byteCount<<','
                <<c.imageRequest.width<<','<<c.imageRequest.height<<','<<c.gpu.nativeFences<<','<<c.gpu.interruptIdleFences<<','
                <<c.gpu.interruptAcknowledged<<','<<c.gpu.beforeCompletionNs<<','<<c.gpu.afterCompletionNs<<','
                <<s.worker<<','<<s.attempted<<','<<s.returned<<','<<s.balanced<<','<<s.heldLocks<<','<<s.lockPairs[0]<<','<<s.lockPairs[1]<<','
                <<irq.queuedCallbackValidated<<','<<irq.rejection<<','
                <<(registry.nativePostOutputSwapInterruptPhase()==recomp::gpu::NativePostOutputSwapInterruptSequence::Phase::Acknowledged);
            for(unsigned i=0;i<4;++i){unsigned value=0;
                for(unsigned b=0;b<4;++b)value|=unsigned{c.gpu.eventBytes[i*4+b]}<<(i<2?8*b:24-8*b);
                std::cout<<','<<c.eventPhysical[i]<<','<<value;}
        }else {std::cout<<0;for(unsigned i=1;i<33;++i)std::cout<<",0";}
        std::cout<<'\n';
        if(inspectIndirect){
            const auto pending=registry.inspectStoppedPostOutputSwapSubmission(submission.writeWord);
            std::cout<<"stopped_post_output_swap_submission=[";
            if(pending){const auto& p=*pending;std::cout<<1<<','<<p.rootPhysical<<','<<p.childPhysical<<','<<p.childWords<<','
                <<p.commandOffsetWords<<','<<p.readWord<<','<<p.writeWord;
            }else std::cout<<0;
            std::cout<<"]\n";
            const auto& irq=stoppedPostOutputSwapInterrupt;
            std::cout<<"stopped_post_output_swap_interrupt=[";
            if(irq){std::cout<<1<<','<<irq->wrapper.scratchPhysical()<<','<<irq->wrapper.callbackAddress()<<','
                <<irq->wrapper.callbackContext()<<','<<irq->device<<','<<irq->handler<<','<<irq->frequency;
                for(auto value:irq->deviceValues)std::cout<<','<<value;
                for(auto value:irq->queuedSurfaces)std::cout<<','<<value;
                for(auto value:irq->scratch)std::cout<<','<<value;
                std::optional<unsigned> base,mask;
#if (defined(__ANDROID__) || defined(__APPLE__))
                if(nativeGraphics){base=nativeGraphics->retainedRegister(0x1DD);mask=nativeGraphics->retainedRegister(0x1DC);}
#endif
                std::cout<<','<<base.has_value()<<','<<base.value_or(0)<<','<<mask.has_value()<<','<<mask.value_or(0);
            }else std::cout<<0<<','<<stoppedPostOutputSwapInterruptRejection;
            std::cout<<"]\n";
            if(pending){
                // Stopped writers, still-pinned owned ticket. Raw bounded
                // wrapper words remain private; this does not execute them.
                const auto bytes=registry.snapshotStoppedGraphicsPhysical(pending->childPhysical,pending->childWords*4);
                std::cout<<"stopped_post_output_swap_words=[";
                if(bytes)for(unsigned i=0;i<pending->childWords;++i){std::uint32_t value=0;
                    for(unsigned b=0;b<4;++b)value=(value<<8)|(*bytes)[i*4+b];std::cout<<(i?",":"")<<value;
                }
                std::cout<<"]\n";
            }
        }
    }
    std::optional<recomp::gpu::StoppedSwapRequestScope> postSwapCaptureScope;
    if(livePostOutputColor && execution.reason==recomp::bootstrap::GuestExecutionStopReason::RejectedImport &&
        execution.detail=="__imp__VdSwap" && execution.importArguments){
        recomp::bootstrap::GuestImportCall call;
        call.stackPointer=static_cast<std::uint32_t>(execution.importArguments->stackPointer);
        call.arguments=execution.importArguments->registers;
        const auto nextRequest=registry.inspectStoppedPostSwapRequest(call);
        std::cout<<"stopped_post_swap_request=[";
        if(nextRequest){const auto& s=*nextRequest;
            const auto translation=std::uint64_t{s.commandVirtual}-s.commandPhysical;
            std::optional<std::uint32_t> imagePhysical;
            if(s.frontbufferVirtual>=translation && recomp::gpu::NativeSwapImagePlan::make(s.fetch,s.frontbufferVirtual,
                    static_cast<std::uint32_t>(s.frontbufferVirtual-translation),s.format,s.colorSpace,s.width,s.height))
                imagePhysical=static_cast<std::uint32_t>(s.frontbufferVirtual-translation);
            postSwapCaptureScope=recomp::gpu::StoppedSwapRequestScope::make(s.commandVirtual,s.commandPhysical,
                s.allocationBase,s.allocationBytes,imagePhysical);
            std::cout<<1<<','<<s.commandVirtual<<','<<s.commandPhysical<<','<<s.allocationBase<<','<<s.allocationBytes<<','
                <<s.protection<<','<<s.frontbufferVirtual<<','<<s.format<<','<<s.colorSpace<<','<<s.width<<','<<s.height<<','<<s.identifierWord;
            for(auto word:s.fetch)std::cout<<','<<word;
        }else std::cout<<0;
        std::cout<<"]\n";
        const auto swap=registry.inspectStoppedPostOutputSwapRequest(call);
        std::cout<<"stopped_post_output_swap_request=[";
        if(swap){std::cout<<1<<','<<swap->commandVirtual<<','<<swap->commandPhysical<<','<<swap->allocationBase<<','
            <<swap->allocationBytes<<','<<swap->protection<<','<<swap->frontbufferVirtual<<','<<swap->format<<','
            <<swap->colorSpace<<','<<swap->width<<','<<swap->height<<','<<swap->identifierWord;
            for(auto word:swap->fetch)std::cout<<','<<word;
        }else std::cout<<0;
        std::cout<<"]\n";
        // Writers have joined. Retain the lock through validation and decode;
        // DO NOT invoke any image/presentation callback for this diagnostic.
        std::array<std::uint32_t,10> metadata{};
        const auto context=registry.nativePostOutputSystemContext();
        const bool continuous=registry.visitStoppedPostOutputSwapImage(call,[&](const auto& image){
            std::uint32_t black=0,opaque=0;
            for(std::size_t i=0;i<image.rgba.size();i+=4){
                black+=!(image.rgba[i]|image.rgba[i+1]|image.rgba[i+2]);opaque+=image.rgba[i+3]==255;
            }
            metadata={image.physicalAddress,image.owner.physicalBase,image.owner.virtualBase,image.owner.byteCount,
                image.owner.protection,recomp::gpu::NativeSwapImagePlan::Capacity,static_cast<std::uint32_t>(image.rgba.size()),
                black,opaque,context->readWord};
            return true;
        });
        std::cout<<"stopped_post_output_swap_image=["<<continuous;
        if(continuous)for(auto value:metadata)std::cout<<','<<value;
        std::cout<<"]\n";
    }
    if(nativeNoSystemOverlay){const auto context=registry.nativeSystemContext();
        std::cout<<"native_no_system_overlay=";
        if(context)std::cout<<1<<','<<context->stackPointer<<','<<context->buffer<<','<<context->identifierOutput<<','<<context->readWord<<','<<context->policy;
        else std::cout<<"0,0,0,0,0,0";
        std::cout<<'\n';
        if(execution.reason==recomp::bootstrap::GuestExecutionStopReason::UnimplementedImport &&
            execution.detail=="__imp__VdSwap" && execution.importArguments){
            recomp::bootstrap::GuestImportCall call;
            call.stackPointer=static_cast<std::uint32_t>(execution.importArguments->stackPointer);
            call.arguments=execution.importArguments->registers;
            const auto swap=registry.inspectStoppedNativeSwapRequest(call);
            std::cout<<"stopped_native_swap_request=[";
            if(swap){std::cout<<1<<','<<swap->commandVirtual<<','<<swap->commandPhysical<<','<<swap->allocationBase<<','
                <<swap->allocationBytes<<','<<swap->protection<<','<<swap->frontbufferVirtual<<','<<swap->format<<','
                <<swap->colorSpace<<','<<swap->width<<','<<swap->height<<','<<swap->identifierWord;
                for(auto word:swap->fetch)std::cout<<','<<word;
            }else std::cout<<0;
            std::cout<<"]\n";
            if(captureNativeSwapImage || callbacks){
                // All guest writers have joined. Metadata and snapshot remain
                // read-only; this creates neither a future lease nor GPU work.
                using Plan=recomp::gpu::NativeSwapImagePlan;
                const auto ring=registry.graphicsRingConfiguration();
                std::optional<recomp::bootstrap::GuestImportRegistry::StoppedPhysicalOwnership> owner;
                std::uint32_t physical=0,black=0,opaque=0;
                std::vector<std::uint8_t> rgba;
                bool saved=false;
                bool decoded=false,continuity=false;int accepted=0;
                if(callbacks){
                    // The actual lifecycle lock spans copy-identity validation,
                    // decode AND the consumer. No separately acquired snapshots
                    // stand in for current ownership; no VdSwap return follows.
                    continuity=registry.visitNativeOwnedSwapImage(call,[&](const auto& image){
                        owner=image.owner;physical=image.physicalAddress;rgba=image.rgba;decoded=true;
                        for(std::size_t i=0;i<rgba.size();i+=4){
                            black+=!(rgba[i]|rgba[i+1]|rgba[i+2]);opaque+=rgba[i+3]==255;
                        }
                        accepted=callbacks->stoppedRgba(callbacks->user,rgba.data(),
                            static_cast<std::uint32_t>(rgba.size()),image.request.width,image.request.height);
                        return true; // Continuity validated even if the consumer rejects.
                    });
                }else if(swap && ring && ring->virtualAddress>=ring->physicalAddress &&
                    swap->frontbufferVirtual>=ring->virtualAddress-ring->physicalAddress){
                    physical=swap->frontbufferVirtual-(ring->virtualAddress-ring->physicalAddress);
                    const auto plan=Plan::make(swap->fetch,swap->frontbufferVirtual,physical,swap->format,
                        swap->colorSpace,swap->width,swap->height);
                    if(plan)owner=registry.stoppedGraphicsPhysicalOwnership(physical,Plan::Capacity);
                    if(owner && owner->writable && owner->physicalBase<=physical &&
                        std::uint64_t{physical}+Plan::Capacity<=std::uint64_t{owner->physicalBase}+owner->byteCount &&
                        std::uint64_t{owner->virtualBase}+physical-owner->physicalBase==swap->frontbufferVirtual){
                        const auto tiled=registry.snapshotStoppedGraphicsPhysical(physical,Plan::Capacity);
                        if(tiled && plan->decode(tiled->data(),tiled->size(),rgba)){
                            decoded=true;
                            for(std::size_t i=0;i<rgba.size();i+=4){
                                black+=!(rgba[i]|rgba[i+1]|rgba[i+2]);opaque+=rgba[i+3]==255;
                            }
                            if(captureNativeSwapImage)saved=recomp::trace::saveNativeSwapImage(argv[0],rgba);
                        }
                    }
                }
                std::cout<<(callbacks?"stopped_native_swap_consumer=[":"stopped_native_swap_image=[");
                if(callbacks?decoded:saved)std::cout<<1<<','<<physical<<','<<owner->physicalBase<<','<<owner->virtualBase<<','<<owner->byteCount<<','
                    <<owner->protection<<','<<Plan::Capacity<<','<<rgba.size()<<','<<black<<','<<opaque;
                else std::cout<<0;
                if(callbacks && decoded)std::cout<<','<<(accepted==1);
                std::cout<<"]\n";
                if(callbacks){
                    std::cout<<"stopped_native_swap_continuity=[";
                    if(continuity)std::cout<<1<<','<<ring->readWord<<','<<Plan::Capacity<<','<<Plan::PixelBytes;
                    else std::cout<<0;
                    std::cout<<"]\n";
                }
            }
        }
    }
    if (cooperative) {
        const auto s = registry.threadDelayStatistics();
        std::cout << "thread_delays=" << s[0] << ',' << s[1] << ',' << s[2] << ',' << s[3]
                  << ',' << s[4] << ',' << s[5] << ',' << s[6] << '\n';
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
    if (experimentalNativeAudio) {
        const auto c=nativeAudio->configuration();
        if (!c || (!nativeAudioRenderer && (nativeAudio->started() || nativeAudio->framesWritten()!=0 || nativeAudio->framesRead()!=0))) {
            std::cerr << "trace evidence failed: configuration-only audio evidence no longer applies\n"; return 2;
        }
        std::cout << "experimental_native_audio=" << c->sampleRate << ',' << c->channels << ','
            << c->bufferCapacityFrames << ',' << c->framesPerBurst << ',' << registry.audioSpeakerQueries() << '\n';
        if (nativeAudioRenderer) {
            const auto stats=nativeAudioRenderer->statistics();const auto read=nativeAudio->framesRead();
            if (!read || nativeAudio->framesWritten()!=stats.writtenFrames) {
                std::cerr << "trace evidence failed: native renderer stream counter mismatch\n";return 2;
            }
            std::cout << "native_audio_renderer=" << stats.workerToken << ',' << stats.callbacksEntered << ','
                << stats.callbacksCompleted << ',' << stats.acceptedPackets << ',' << stats.writtenFrames << ','
                << stats.serviceReadFrames << ',' << stats.pendingPackets << ',' << stats.clippedSamples << ','
                << stats.startRequested << ',' << stats.callbackActive << ',' << stats.failed << ','
                << *read << ',' << nativeAudio->started() << '\n';
        }
    }
#endif
    if (const auto memory = registry.lastMemoryStatistics()) {
        std::cout << "native_memory_statistics=" << (*memory)[1] << ',' << (*memory)[3] << ','
                  << (*memory)[4] << ',' << (*memory)[5] << ',' << (*memory)[6] << ','
                  << (*memory)[11] << ',' << (*memory)[25] << '\n';
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::InvalidIndirectTarget && rejectedIndirect) {
        std::cout << "invalid_indirect_call=" << rejectedIndirect->target << ',' << rejectedIndirect->linkRegister
                  << ',' << rejectedIndirect->stackPointer << '\n';
        std::cout << "invalid_indirect_registers=";
        for (auto value : rejectedIndirect->registers) std::cout << value << ',';
        std::cout << '\n';
        // Writers are joined. Inspect only bounded live heap/graphics storage or
        // the verified loaded image, never an arbitrary flat-address-space read.
        for (auto index : {0U, 2U, 5U, 6U, 7U}) {
            const auto address = rejectedIndirect->registers[index];
            constexpr std::uint32_t bytes = 160;
            auto snapshot = registry.snapshotStoppedGuestHeap(address, bytes);
            if (!snapshot) snapshot = registry.snapshotStoppedGraphicsVirtual(address, bytes);
            if (!snapshot && address >= imageBase && std::uint64_t{address} + bytes <= std::uint64_t{imageBase} + titleValues[1]) {
                const auto* begin = addressSpace.translate(address);
                snapshot = std::vector<std::uint8_t>(begin, begin + bytes);
            }
            std::cout << "invalid_indirect_object=" << index << ',' << address << ',' << bool(snapshot);
            if (snapshot) for (std::size_t offset = 0; offset < snapshot->size(); offset += 4) {
                const auto* p = snapshot->data() + offset;
                std::cout << ',' << ((std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16)
                                    | (std::uint32_t{p[2]} << 8) | p[3]);
            }
            std::cout << '\n';
        }
        // (Claude 2026-10-18, c207) the game's job worker sub_83197450 calls job->func(job) at 83197564 (LR 83197568) on a job
        // taken from the manager in r26 by sub_83196290: 4 bounded rings of 8200 bytes (head +0, tail +4, seq[1024] +8,
        // data[1024] +4104). Search every ring's data for the job pointer (r3): found = the ring still holds what it handed
        // out (a job freed or reused while queued); not found = a newer job overwrote that slot (the worker read it stale).
        if (rejectedIndirect->linkRegister == 0x83197568u) {
            const std::uint32_t job = rejectedIndirect->registers[0], manager = rejectedIndirect->registers[6];
            constexpr std::uint32_t ringBytes = 8200, rings = 4;
            std::optional<std::vector<std::uint8_t>> m = registry.snapshotStoppedGuestHeap(manager, ringBytes * rings);
            if (!m && manager >= imageBase && std::uint64_t{manager} + ringBytes * rings <= std::uint64_t{imageBase} + titleValues[1]) {
                const auto* begin = addressSpace.translate(manager);
                m = std::vector<std::uint8_t>(begin, begin + ringBytes * rings);
            }
            const auto word = [&](std::size_t off) {const auto* q = m->data() + off;
                return (std::uint32_t{q[0]} << 24) | (std::uint32_t{q[1]} << 16) | (std::uint32_t{q[2]} << 8) | q[3];};
            std::cout << "job_ring_check manager=" << manager << " job=" << job << " snapshot=" << bool(m);
            if (m) {
                unsigned found = 0;
                for (std::uint32_t r = 0; r < rings; ++r) {
                    const std::size_t b = std::size_t(r) * ringBytes;
                    std::cout << " ring" << r << "=" << word(b) << '/' << word(b + 4);
                    for (std::uint32_t i = 0; i < 1024; ++i) if (word(b + 4104 + i * 4) == job) {
                        ++found; std::cout << " found=" << r << ':' << i << ":seq=" << word(b + 8 + i * 4);
                    }
                }
                std::cout << " found_count=" << found;
            }
            std::cout << '\n';
        }
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
    if (nativeGraphics && nativeGraphics->displayConfiguration()) {
        const auto& dimensions = nativeGraphics->displayConfiguration()->dimensions;
        std::cout << "native_display_dimensions=" << dimensions[0] << ',' << dimensions[1] << ','
                  << dimensions[2] << ',' << dimensions[3] << '\n';
    }
#endif
    if (experimentalGpuClock) std::cout << "experimental_gpu_clock_500mhz_reads=" << experimentalClockReads << '\n';
    std::cout << "cpu_timebase_frequency=" << clockProbe.returnValue << '\n';
    std::cout << "cpu_timebase_reads=" << recomp::bootstrap::guestTimebaseReadCount() << '\n';
    if (nativeInitialCoherency) {
        if (const auto completed = registry.completedNativeInitialCoherency())
            std::cout << "native_initial_coherency=1," << completed->physicalBase << ',' << completed->byteCount << ',' << completed->cacheFlags << '\n';
        else std::cout << "native_initial_coherency=0,0,0,0\n";
    }
    if (experimentalNativeInitialization) {
        const auto completed = registry.completedExperimentalNativeInitialization();
        if (completed) std::cout << "experimental_native_initialization=1," << completed->readWord << ','
            << completed->draws << ',' << completed->words << ',' << completed->registers << '\n';
        else std::cout << "experimental_native_initialization=0,0,0,0,0\n";
    }
    if (experimentalNativeInitialization && hasHsioLock) {
        const auto state = registry.nativeEdramTrainingState();
        std::cout << "native_edram_training_policy=" << state.workerChecks << ',' << state.noCommandQueries << ',' << state.statusQueries << ',' << state.directNoCommandQueries << '\n';
    }
    if (nativeEmptyCacheCoherency) {
        if (const auto state=registry.completedWarmEmptyCacheCoherency())
            std::cout << "native_warm_empty_cache_coherency=" << state->batches << ',' << state->readWord << ','
                << state->request.physicalBase << ',' << state->request.byteCount << ',' << state->request.cacheFlags << '\n';
        if (const auto state=registry.completedEmptyGuestCacheCoherency())
            std::cout << "native_empty_cache_coherency=" << state->batches << ',' << state->readWord << ','
                << state->request.physicalBase << ',' << state->request.byteCount << ',' << state->request.cacheFlags << '\n';
        else std::cout << "native_empty_cache_coherency=0,0,0,0,0\n";
    }
    if (nativeStateBatches) {
        for(bool warm:{false,true}) {
            const auto state=warm?registry.completedWarmStateBatches():registry.completedNativeStateBatches();
            if(state && state->maintenanceBatches)
                std::cout << "native_maintenance_points=" << unsigned(warm) << ',' << state->maintenanceBatches << ','
                    << state->maintenanceDraws << ',' << state->readWord << '\n';
        }
        if (const auto state=registry.completedWarmStateBatches())
            std::cout << "native_warm_state_batches=" << state->batches << ',' << state->readWord << ',' << state->words << ','
                << state->stateWrites << ',' << state->invalidations << '\n';
        if (const auto warm=registry.completedWarmRingCoherency())
            std::cout << "native_ring_warm_coherency=1," << warm->physicalBase << ',' << warm->byteCount << ',' << warm->cacheFlags << '\n';
        if (const auto state=registry.completedNativeStateBatches())
            std::cout << "native_state_batches=" << state->batches << ',' << state->readWord << ',' << state->words << ','
                << state->stateWrites << ',' << state->invalidations << '\n';
        else std::cout << "native_state_batches=0,0,0,0,0\n";
#if (defined(__ANDROID__) || defined(__APPLE__))
        if (nativeGraphics) {
            const auto& state=nativeGraphics->stateBatchSnapshot();
            std::cout << "native_bin_state=" << static_cast<std::uint32_t>(state.binMask) << ','
                << static_cast<std::uint32_t>(state.binMask>>32) << ','
                << static_cast<std::uint32_t>(state.binSelect) << ','
                << static_cast<std::uint32_t>(state.binSelect>>32) << ',' << state.knownBinWords << '\n';
        }
#endif
    }
    if (submission.status != recomp::bootstrap::StoppedGpuSubmissionStatus::NotSubmission) {
        std::cout << "gpu_submission=" << static_cast<unsigned>(submission.status) << ','
                  << static_cast<unsigned>(submission.error.failure) << ',' << submission.error.offsetWords << ','
                  << submission.error.opcode << ',' << submission.readWord << ',' << submission.writeWord << ','
                  << submission.snapshot.words().size() << ',' << submission.snapshot.packets().size() << '\n';
        const auto& packets = submission.snapshot.packets();
        for (std::size_t i = 0; i < std::min<std::size_t>(packets.size(), 64); ++i) {
            const auto& p = packets[i];
            std::cout << "gpu_packet=" << p.offsetWords << ',' << static_cast<unsigned>(p.kind) << ','
                      << p.payloadWords << ',' << p.opcode << ',' << p.firstRegister << ',' << p.secondRegister << ','
                      << p.predicated << ',' << p.repeatRegister << '\n';
        }
    }
    if (inspectIndirect) {
        // JSON arrays of bounded metadata only: no raw command words or paths.
        std::cout << "gpu_indirect_graph=[" << static_cast<unsigned>(indirectError.failure) << ','
                  << indirectError.parent << ',' << indirectError.offsetWords << ','
                  << static_cast<unsigned>(indirectError.decode.failure) << ',' << indirectError.decode.offsetWords << ','
                  << indirectError.decode.opcode << ',' << indirectGraph.totalWords << ',' << indirectGraph.totalPackets << ",[";
        std::size_t remainingPackets = 64;
        for (std::size_t i = 0; i < indirectGraph.nodes.size(); ++i) {
            const auto& n = indirectGraph.nodes[i];
            if (i) std::cout << ',';
            std::cout << '[' << n.parent << ',' << n.parentOffsetWords << ',' << n.physicalAddress << ',' << n.depth << ','
                      << n.snapshot.words().size() << ',' << n.snapshot.packets().size() << ",[";
            const auto count = std::min(remainingPackets, n.snapshot.packets().size());
            for (std::size_t j = 0; j < count; ++j) {
                const auto& p = n.snapshot.packets()[j];
                if (j) std::cout << ',';
                std::cout << '[' << p.offsetWords << ',' << static_cast<unsigned>(p.kind) << ',' << p.payloadWords << ','
                          << p.opcode << ',' << p.firstRegister << ',' << p.secondRegister << ',' << p.predicated << ',' << p.repeatRegister << ']';
            }
            remainingPackets -= count;
            std::cout << "]]";
        }
        std::cout << "]]\n";
    }
    if (inspectWork) {
        std::cout << "gpu_stopped_work=[" << stoppedWorkError.kind << ',' << stoppedWorkError.node << ','
                  << static_cast<unsigned>(stoppedWorkError.error.failure) << ',' << stoppedWorkError.error.offsetWords << ','
                  << stoppedWorkError.error.opcode << ",[";
        for (std::size_t i = 0; i < stoppedWork.shaders.size(); ++i) {
            const auto& s = stoppedWork.shaders[i];
            if (i) std::cout << ',';
            std::cout << '[' << s.node << ',' << s.load.packetOffsetWords << ',' << s.load.stage << ','
                      << s.load.instructionWords << ',' << s.load.immediate << ',' << s.load.physicalAddress << ']';
        }
        std::cout << "],[";
        for (std::size_t i = 0; i < stoppedWork.draws.size(); ++i) {
            const auto& d = stoppedWork.draws[i];
            if (i) std::cout << ',';
            std::cout << '[' << d.node << ',' << d.draw.packetOffsetWords << ',' << d.draw.opcode << ','
                      << d.draw.indexCount << ',' << d.draw.primitive << ',' << d.draw.source << ',' << d.draw.majorMode << ','
                      << d.draw.index32 << ',' << d.draw.notEndOfPacket << ',' << d.draw.conditionalVizQuery << ',' << d.draw.predicated << ']';
        }
        std::cout << "]]\n";
    }
    if (inspectWork && !stoppedWorkError.kind) {
        std::cout << "gpu_stopped_shader_proofs=[";
        for (std::size_t i = 0; i < stoppedWork.shaders.size(); ++i) {
            const auto& s = stoppedWork.shaders[i];
            recomp::gpu::CopyShaderProof proof; recomp::gpu::CopyShaderError error;
            const auto& bytes = s.load.inlineBigEndianBytes;
            (void)recomp::gpu::analyzeCopyShader(s.load.immediate ? bytes.data() : nullptr,bytes.size(),
                static_cast<recomp::gpu::CopyShaderStage>(s.load.stage),proof,error);
            if (i) std::cout << ',';
            std::cout << '[' << s.node << ',' << s.load.packetOffsetWords << ',' << s.load.stage << ','
                << static_cast<unsigned>(error.failure) << ',' << error.index << ',' << proof.controlCount << ','
                << proof.executedCount << ',' << proof.exported.count() << ',' << proof.initialComponentsRead.count() << ']';
        }
        std::cout << "]\n";
    }
    if (inspectWork) {
        std::cout << "gpu_stopped_memory=[" << static_cast<unsigned>(stoppedMemoryError.failure) << ','
                  << stoppedMemoryError.node << ',' << stoppedMemoryError.offsetWords << ','
                  << stoppedMemory.unmodeledScratchMaskBits << ',' << stoppedMemory.otherRegisterWrites << ','
                  << stoppedMemory.drawRequests << ",[";
        const auto ring = registry.graphicsRingConfiguration();
        for (std::size_t i = 0; i < stoppedMemory.writes.size(); ++i) {
            const auto& w = stoppedMemory.writes[i];
            const auto overlap = [&](std::uint32_t address, std::size_t bytes) {
                return w.physicalAddress < std::uint64_t{address} + bytes && address < std::uint64_t{w.physicalAddress} + 4;
            };
            bool aliasesCommands = ring && (overlap(ring->physicalAddress,ring->bytes) ||
                (ring->writeBack && overlap(ring->writeBack->physicalAddress,4)));
            for (const auto& n : indirectGraph.nodes) aliasesCommands |= overlap(n.physicalAddress,n.snapshot.words().size()*4);
            aliasesCommands |= registry.stoppedGraphicsWordAliasesIdentifier(w.physicalAddress);
            if (i) std::cout << ',';
            std::cout << '[' << w.node << ',' << w.offsetWords << ',' << w.kind << ',' << w.physicalAddress << ','
                      << w.value << ',' << w.priorDrawRequests << ',' << registry.stoppedGraphicsWordIsWritable(w.physicalAddress)
                      << ',' << aliasesCommands << ',' << w.bigEndianProfile << ']';
        }
        std::cout << "]]\n";
        recomp::gpu::StoppedWordStaging staging;
        if (recomp::gpu::planStoppedWordStaging(stoppedMemory,staging)) {
            std::size_t before = 0;
            for (std::size_t i = 0; i < staging.updates.count; ++i)
                before += staging.updates.words[i].phase == recomp::gpu::NativeWordPhase::BeforeGraphics;
            std::cout << "gpu_stopped_word_staging=" << before << ',' << staging.updates.count - before
                      << ',' << staging.destinationCount << ',' << staging.updates.bufferBytes << '\n';
        }
    }
    if (inspectWork) {
        recomp::gpu::StoppedConstantInitialization plan; recomp::gpu::StoppedConstantError error;
        (void)recomp::gpu::inspectStoppedConstantInitialization(submission.snapshot,indirectGraph,plan,error);
        std::size_t incomplete = 0;
        for (const auto& item : plan.registers) incomplete += item.second.known != 0xFFFFFFFF;
        std::cout << "gpu_stopped_constant_initialization=[" << static_cast<unsigned>(error.failure) << ','
            << error.node << ',' << error.offsetWords << ',' << plan.draws.size() << ',' << plan.shaderLoads << ','
            << plan.invalidations << ',' << plan.registers.size() << ',' << plan.otherRegisterWrites << ',' << incomplete << ",[";
        std::size_t retained = 0;
        for (const auto& item : plan.registers) if (item.second.known != 0xFFFFFFFF && retained < 16) {
            if (retained++) std::cout << ',';
            std::cout << '[' << item.first << ',' << item.second.value << ',' << item.second.known << ']';
        }
        std::cout << "],[";
        for (std::size_t i = 0; i < plan.draws.size(); ++i) {
            const auto& d = plan.draws[i];
            unsigned position = 0, parameter = 0;
            for (unsigned c = 0; c < 4; ++c) { position |= d.pair.position[c] << c; parameter |= d.pair.parameter[c] << c; }
            if (i) std::cout << ',';
            std::cout << '[' << d.node << ',' << d.offsetWords << ',' << position << ',' << parameter << ']';
        }
        std::cout << "]]\n";
    }
    if (captureStoppedShaders) {
        std::vector<std::vector<std::uint8_t>> captured;
        const unsigned status = !inspectWork || stoppedWorkError.kind ? 0 :
            (recomp::gpu::captureStoppedShaderBytes(stoppedWork,
                [&](auto address, auto bytes) { return registry.snapshotStoppedGraphicsPhysical(address, bytes); },
                captured) ? 1 : 2);
        std::cout << "stopped_shader_capture=" << status << '\n';
        if (status == 1) for (std::size_t i = 0; i < stoppedWork.shaders.size(); ++i) {
            const auto& s = stoppedWork.shaders[i];
            std::cout << "stopped_shader_blob=" << s.node << ',' << s.load.packetOffsetWords << ',' << s.load.stage << ',';
            // Explicit physical provenance; legacy three-field records mean
            // inline only. Never pass external bytes off as inline proof.
            if (!s.load.immediate) std::cout << "0," << s.load.physicalAddress << ',';
            constexpr char digits[] = "0123456789abcdef";
            for (auto byte : captured[i]) std::cout << digits[byte >> 4] << digits[byte & 15];
            std::cout << '\n';
        }
    }
    if (captureStoppedPm4) {
        const unsigned status = !inspectWork ? 0 :
            (indirectGraph.nodes.size() > 16 || indirectGraph.totalWords > 4096 ? 2 : 1);
        std::cout << "stopped_pm4_capture=" << status << '\n';
        if (status == 1) for (std::size_t id = 0; id <= indirectGraph.nodes.size(); ++id) {
            const auto& snapshot = id ? indirectGraph.nodes[id - 1].snapshot : submission.snapshot;
            std::cout << "stopped_pm4_blob=" << id << ',';
            constexpr char digits[] = "0123456789abcdef";
            for (auto word : snapshot.words()) for (unsigned i = 0; i < 8; ++i)
                std::cout << digits[(word >> (28 - i * 4)) & 15];
            std::cout << '\n';
        }
        // The native envelope is NOT retail PM4. An explicit command capture
        // can snapshot its CURRENT owned root/child separately, without
        // guessing addresses across runs or making the failed graph valid.
        unsigned nativeStatus=0;
        recomp::gpu::StoppedPhysicalRanges nativeRanges;
        std::vector<std::vector<std::uint8_t>> nativeBytes;
        if(deferredPendingSubmission && capturePhysicalRanges.empty()){
            const auto& p=*deferredPendingSubmission;
            nativeStatus=2;
            if(p.childWords<=4093){
                nativeRanges={{{p.rootPhysical,12}},{{p.childPhysical,p.childWords*4}}};
                if(recomp::gpu::captureStoppedPhysicalRanges(nativeRanges,
                    [&](auto address,auto count){return registry.snapshotStoppedGraphicsPhysical(address,count);},nativeBytes,16384))nativeStatus=1;
            }
        }
        std::cout<<"stopped_native_pending_capture="<<nativeStatus<<"\nstopped_native_pending_ownership=[";
        if(nativeStatus==1)for(unsigned i=0;i<2;++i){
            if(i)std::cout<<',';
            const auto owner=registry.stoppedGraphicsPhysicalOwnership(nativeRanges[i][0],nativeRanges[i][1]);
            if(!owner)std::cout<<"null";
            else std::cout<<'['<<owner->physicalBase<<','<<owner->virtualBase<<','<<owner->byteCount<<','<<owner->protection<<','<<owner->writable<<']';
        }
        std::cout<<"]\n";
        if(nativeStatus==1)for(unsigned i=0;i<2;++i){
            std::cout<<"stopped_native_pending_blob="<<nativeRanges[i][0]<<','<<nativeRanges[i][1]<<',';
            constexpr char digits[]="0123456789abcdef";
            for(auto b:nativeBytes[i])std::cout<<digits[b>>4]<<digits[b&15];
            std::cout<<'\n';
        }
        if(captureStoppedShaders){
            // Inspect the SAME owned child just captured, never reinterpret
            // the failed retail graph as successful work. No image pixel read,
            // execution, event publication or ring retirement is authorized.
            unsigned shaderStatus=0;recomp::gpu::NativeDeferredShaderCapture shaders;
#if (defined(__ANDROID__) || defined(__APPLE__))
            std::array<std::uint32_t,10> colorInspection{};
            const auto ring=registry.graphicsRingConfiguration();colorInspection[9]=ring?ring->readWord:0;
            std::array<std::uint32_t,4> memoryInspection{{0,0,0,colorInspection[9]}};
            std::array<std::uint32_t,9> gpuInspection{{0,0,0,0,0,0,0,0,colorInspection[9]}};
            std::array<std::uint32_t,9> prefixTimeline{{0,0,0,0,0,0,0,0,colorInspection[9]}};
            std::array<std::uint32_t,9> pendingStateInspection{{0,0,0,0,0,0,0,0,colorInspection[9]}};
            std::array<std::uint32_t,4> writeGuardInspection{{0,0,0,colorInspection[9]}};
#endif
            if(nativeStatus==1){
                shaderStatus=2;
                try{
                    if(const auto request=registry.nativeDeferredSwapRequest()){
                        const auto command=recomp::gpu::NativeSwapCommand::make({3,request->request.commandPhysical,
                            request->imagePhysical,request->issuedReadWord});
                        if(command){
                            const auto frame=recomp::gpu::NativeDeferredSwapSubmission::parse(*command,
                                nativeRanges[1][0],nativeBytes[1].data(),nativeBytes[1].size());
                            if(frame && recomp::gpu::captureNativeDeferredPrefixShaders(*frame,
                                [&](auto address,auto count){return registry.snapshotStoppedGraphicsPhysical(address,count);},shaders)){
                                shaderStatus=1;
#if (defined(__ANDROID__) || defined(__APPLE__))
                                if(inspectColorProgram && experimentalOwnedColor && nativeDisplayOutput && nativeGraphics){
                                    const auto state=nativeGraphics->stateBatchSnapshot();
                                    const recomp::gpu::DrawBinState bins{{{static_cast<std::uint32_t>(state.binMask),static_cast<std::uint32_t>(state.binMask>>32),
                                        static_cast<std::uint32_t>(state.binSelect),static_cast<std::uint32_t>(state.binSelect>>32)}},state.knownBinWords};
                                    colorInspection[0]=2;
                                    const auto color=recomp::gpu::NativeDeferredColorCapture::create(*frame,bins,*inspectColorProgram,
                                        [&](auto address,auto count){return registry.snapshotStoppedGraphicsPhysical(address,count);},&colorInspection[1]);
                                    if(color){colorInspection={{1,0,frame->swapOffsetWords(),static_cast<unsigned>(color->commands().draws.size()),
                                        static_cast<unsigned>(color->commands().shaders.size()),static_cast<unsigned>(color->commands().constants.size()),
                                        color->sourceBytes(),color->copy().copy.destinationBase,color->color().sourceRanges().constantZeroTextures,
                                        colorInspection[9]}};
#if defined(RECOMP_RENDERER_BOUNDARY_TRACE)
                compareRendererInlineInputs(color->color(),nativeGraphics.get(),inspectColorProgram?&*inspectColorProgram:nullptr);
#endif
                                        memoryInspection[0]=2;
                                        // All guest workers are JOINED. Resolve current allocation
                                        // metadata only; no future pin or write permission escapes.
                                        const auto identifier=registry.graphicsSystemIdentifierAddress();
                                        const auto previous=registry.completedNativePostOutputWrappedSwap();
                                        if(ring && ring->writeBack && identifier && previous && ring->virtualAddress>=ring->physicalAddress){
                                            const auto translation=ring->virtualAddress-ring->physicalAddress;
                                            if(*identifier>=translation){
                                                const auto fullWords=frame->swapOffsetWords()+recomp::gpu::NativeDeferredSwapWrapper::Words;
                                                const recomp::gpu::NativeColorMemoryControls controls{{ring->physicalAddress,ring->bytes},
                                                    {frame->physicalAddress(),fullWords*4},{ring->writeBack->physicalAddress,4},{*identifier-translation,4},
                                                    ring->readWord,submission.writeWord,{{0xC0013F00,frame->physicalAddress(),fullWords}}};
                                                const auto old=previous->imageOwner.physicalBase+
                                                    (previous->imageRequest.frontbufferVirtual-previous->imageOwner.virtualBase);
                                                const auto memoryPlan=recomp::gpu::NativeDeferredColorMemoryPlan::create(*color,controls,
                                                    {old,recomp::gpu::NativeSwapImagePlan::Capacity});
                                                if(memoryPlan){
                                                    recomp::gpu::NativeDeferredColorResolvedMemory memory;bool owned=true;
                                                    const auto resolve=[&](recomp::gpu::NativePhysicalRange range,recomp::gpu::NativeColorMappedRange& mapped){
                                                        const auto owner=registry.stoppedGraphicsPhysicalOwnership(range.address,range.bytes);
                                                        if(!owner){owned=false;return;}
                                                        mapped={range,{owner->physicalBase,owner->byteCount},addressSpace.translate(owner->virtualBase),owner->writable};
                                                    };
                                                    for(unsigned i=0;i<memoryPlan->inputCount();++i)resolve(memoryPlan->inputs()[i],memory.inputs[i]);
                                                    for(unsigned i=0;i<5;++i)resolve(memoryPlan->protectedRanges()[i],memory.protectedRanges[i]);
                                                    for(unsigned i=0;i<7;++i)resolve(memoryPlan->writes()[i],memory.writes[i]);
                                                    if(owned && recomp::gpu::matchesNativeDeferredColorMemory(*color,*memoryPlan,memory)){
                                                        memoryInspection={{1,memoryPlan->inputCount(),7,ring->readWord}};
                                                        if(diagnoseDeferredColor){
                                                            // Snapshot all CURRENT before-images with guest
                                                            // workers joined. Inspect only: never call the
                                                            // publication method on this stopped diagnostic.
                                                            auto writeGuard=recomp::gpu::NativeDeferredColorWriteGuard::create(*color,*memoryPlan,memory);
                                                            writeGuardInspection[0]=2;
                                                            recomp::gpu::NativeDeferredColorDiagnosticResult rendered;
                                                            const bool complete=nativeGraphics->executeStoppedDeferredColorDiagnostic(*inspectColorProgram,*color,rendered);
                                                            const auto after=nativeGraphics->stateBatchSnapshot();
                                                            const bool counters=state.binMask==after.binMask && state.binSelect==after.binSelect &&
                                                                state.invalidationEpoch==after.invalidationEpoch && state.experimentalIdleCachePairEpoch==after.experimentalIdleCachePairEpoch &&
                                                                state.knownBinWords==after.knownBinWords && state.batches==after.batches &&
                                                                state.experimentalColorBatches==after.experimentalColorBatches;
                                                            const bool same=recomp::gpu::matchesNativeDeferredColorMemory(*color,*memoryPlan,memory);
                                                            recomp::gpu::NativeDeferredColorDiagnosticResult repeated;repeated.nativeFences=99;
                                                            unsigned forbiddenPublications=0;
                                                            const recomp::gpu::NativeDeferredColorPublisher forbidden{&forbiddenPublications,[](void* p,const auto&) noexcept{
                                                                ++*static_cast<unsigned*>(p);return false;}};
                                                            const bool once=complete && !nativeGraphics->executeStoppedDeferredColorDiagnostic(*inspectColorProgram,*color,repeated) &&
                                                                repeated.nativeFences==99 && !repeated.copy && !repeated.edram && !repeated.auditedPrefixTimeline &&
                                                                !repeated.pendingRegisters && !repeated.pendingState &&
                                                                !nativeGraphics->executeExperimentalDeferredColorPrefix(*inspectColorProgram,*color,forbidden) && !forbiddenPublications;
                                                            gpuInspection={{2,0,0,0,0,unsigned(same),unsigned(counters),unsigned(once),ring->readWord}};
                                                            if(complete && same && counters && once && rendered.copy && rendered.edram && rendered.auditedPrefixTimeline &&
                                                                rendered.pendingRegisters && rendered.pendingState &&
                                                                rendered.copy->matchesPlan(color->copy().copy))gpuInspection={{1,rendered.leadingSetupFences,rendered.nativeFences,
                                                                    static_cast<unsigned>(rendered.edram->knownBytes().size()),rendered.copy->pixelBytes(),1,1,1,ring->readWord}};
                                                            if(gpuInspection[0]==1){const auto& t=*rendered.auditedPrefixTimeline;
                                                                prefixTimeline={{1,t.leadingFences(),t.completedStages(),t.invalidations(),t.copyIdleCleans(),
                                                                    t.depthWaits(),t.cacheFlushes(),t.literalEvents(),ring->readWord}};
                                                                const auto& p=*rendered.pendingState;unsigned unknown=0;
                                                                for(unsigned reg:{0x5C8u,0xF01u,0xA2Fu,0xA30u,0xA31u,0x21F9u})unknown+=!rendered.pendingRegisters->count(reg);
                                                                pendingStateInspection={{1,static_cast<unsigned>(rendered.pendingRegisters->size()),unknown,
                                                                    static_cast<unsigned>(p.invalidationEpoch-state.invalidationEpoch),p.batches-state.batches,
                                                                    p.experimentalColorBatches-state.experimentalColorBatches,
                                                                    unsigned(p.experimentalIdleCachePairEpoch==state.experimentalIdleCachePairEpoch),
                                                                    unsigned(color->initialBins().words==bins.words && color->initialBins().knownWords==bins.knownWords),ring->readWord}};
                                                                if(writeGuard && !writeGuard->published() && writeGuard->matches(*color,memory))
                                                                    writeGuardInspection={{1,static_cast<unsigned>(writeGuard->snapshotBytes()),memoryPlan->inputCount()+12,ring->readWord}};
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
#endif
                            }
                        }
                    }
                }catch(...){shaderStatus=2;}
            }
            std::cout<<"stopped_deferred_shader_capture="<<shaderStatus<<'\n';
            if(shaderStatus==1)for(std::size_t i=0;i<shaders.loads.size();++i){
                const auto& s=shaders.loads[i];
                std::cout<<"stopped_deferred_shader_blob="<<s.node<<','<<s.load.packetOffsetWords<<','<<s.load.stage<<',';
                if(!s.load.immediate)std::cout<<"0,"<<s.load.physicalAddress<<',';
                constexpr char digits[]="0123456789abcdef";
                for(auto b:shaders.bytes[i])std::cout<<digits[b>>4]<<digits[b&15];
                std::cout<<'\n';
            }
#if (defined(__ANDROID__) || defined(__APPLE__))
            if(inspectColorProgram && experimentalOwnedColor && nativeDisplayOutput){
                std::cout<<"stopped_deferred_color_inspection=[";
                for(unsigned i=0;i<colorInspection.size();++i)std::cout<<(i?",":"")<<colorInspection[i];
                std::cout<<"]\n";
                std::cout<<"stopped_deferred_memory_inspection=[";
                for(unsigned i=0;i<memoryInspection.size();++i)std::cout<<(i?",":"")<<memoryInspection[i];
                std::cout<<"]\n";
                if(diagnoseDeferredColor){
                    std::cout<<"stopped_deferred_color_gpu=[";
                    for(unsigned i=0;i<gpuInspection.size();++i)std::cout<<(i?",":"")<<gpuInspection[i];
                    std::cout<<"]\n";
                    std::cout<<"stopped_deferred_color_timeline=[";
                    for(unsigned i=0;i<prefixTimeline.size();++i)std::cout<<(i?",":"")<<prefixTimeline[i];
                    std::cout<<"]\n";
                    std::cout<<"stopped_deferred_color_pending_state=[";
                    for(unsigned i=0;i<pendingStateInspection.size();++i)std::cout<<(i?",":"")<<pendingStateInspection[i];
                    std::cout<<"]\n";
                    std::cout<<"stopped_deferred_color_write_guard=[";
                    for(unsigned i=0;i<writeGuardInspection.size();++i)std::cout<<(i?",":"")<<writeGuardInspection[i];
                    std::cout<<"]\n";
                }
            }
#endif
        }
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(inspectColorProgram && experimentalOwnedColor && nativeDisplayOutput){
        // Stopped, read-only diagnostic of the next batch. This does NOT use
        // the one-shot live publisher, advance the queue, or export textures.
        unsigned status=0;std::array<std::uint32_t,6> counts{};std::array<std::uint32_t,3> nonzero{};unsigned literals=0;
        if(inspectWork && nativeGraphics){
            const auto state=nativeGraphics->stateBatchSnapshot();
            const recomp::gpu::DrawBinState bins{{{static_cast<std::uint32_t>(state.binMask),static_cast<std::uint32_t>(state.binMask>>32),
                static_cast<std::uint32_t>(state.binSelect),static_cast<std::uint32_t>(state.binSelect>>32)}},state.knownBinWords};
            status=registry.inspectStoppedPostOutputColorBatch(*inspectColorProgram,bins,submission.writeWord,[&](const auto& capture){
                recomp::gpu::NativeColorChainBindings bindings;if(!capture.color().bindings(bindings))return false;
                std::array<std::uint32_t,3> observed{};
                for(const auto& texture:bindings.pairs[0].textures){
                    if(texture.slot>=3 || !texture.source.bytes)return false;
                    observed[texture.slot]=static_cast<std::uint32_t>(std::count_if(texture.source.bytes,
                        texture.source.bytes+texture.source.size,[](auto byte){return byte!=0;}));
                }
                counts={{static_cast<std::uint32_t>(capture.commands().commands.words().size()),static_cast<std::uint32_t>(capture.commands().shaders.size()),
                    static_cast<std::uint32_t>(capture.commands().draws.size()),capture.memory().inputCount,
                    capture.controls().previousWord,capture.controls().nextWord}};
                nonzero=observed;literals=capture.color().sourceRanges().constantZeroTextures;return true;
            })?1:2;
        }
        const auto ring=registry.graphicsRingConfiguration();
        std::cout<<"stopped_post_output_color_inspection="<<status;
        for(auto count:counts)std::cout<<','<<count;
        std::cout<<','<<(ring?ring->readWord:0)<<','<<literals;
        for(auto count:nonzero)std::cout<<','<<count;
        std::cout<<'\n';
        if(diagnosePostOutputColor){
            recomp::gpu::NativeColorChainExecutionResult rendered;bool complete=false,unchanged=true;
            if(status==1 && nativeGraphics){
                const auto before=nativeGraphics->stateBatchSnapshot();
                const recomp::gpu::DrawBinState bins{{{static_cast<std::uint32_t>(before.binMask),static_cast<std::uint32_t>(before.binMask>>32),
                    static_cast<std::uint32_t>(before.binSelect),static_cast<std::uint32_t>(before.binSelect>>32)}},before.knownBinWords};
                complete=registry.runStoppedPostOutputColorDiagnostic(*inspectColorProgram,bins,submission.writeWord,
                    [&](const auto& capture){return nativeGraphics->executeStoppedPostOutputColorDiagnostic(*inspectColorProgram,capture,rendered);});
                const auto after=nativeGraphics->stateBatchSnapshot();const auto finalRing=registry.graphicsRingConfiguration();
                unchanged=after.binMask==before.binMask && after.binSelect==before.binSelect && after.knownBinWords==before.knownBinWords &&
                    after.batches==before.batches && after.invalidationEpoch==before.invalidationEpoch &&
                    after.experimentalColorBatches==before.experimentalColorBatches && after.experimentalIdleCachePairEpoch==before.experimentalIdleCachePairEpoch &&
                    finalRing && ring && finalRing->readWord==ring->readWord;
                if(!unchanged)std::_Exit(4);
            }
            std::cout<<"stopped_post_output_color_gpu="<<unsigned(complete)<<','<<rendered.leadingSetupFences<<','<<rendered.nativeFences<<','
                <<(rendered.edram?rendered.edram->knownBytes().size():0)<<','<<(rendered.copy?rendered.copy->pixelBytes():0)<<','
                <<unsigned(unchanged)<<','<<(ring?ring->readWord:0)<<'\n';
        }
    }
    if(inspectColorProgram && !experimentalOwnedColor){
        unsigned status=0;std::array<std::uint32_t,6> counts{};
        std::unique_ptr<recomp::gpu::NativeColorBatchCapture> owned;
        recomp::gpu::DrawBinState initialColorBins;
        if(inspectWork && nativeGraphics){
            const auto state=nativeGraphics->stateBatchSnapshot();
            const recomp::gpu::DrawBinState bins{{{static_cast<std::uint32_t>(state.binMask),static_cast<std::uint32_t>(state.binMask>>32),
                static_cast<std::uint32_t>(state.binSelect),static_cast<std::uint32_t>(state.binSelect>>32)}},state.knownBinWords};
            initialColorBins=bins;
            const auto inspect=[&](const auto& capture){
                counts={{static_cast<std::uint32_t>(capture.commands().commands.words().size()),static_cast<std::uint32_t>(capture.commands().shaders.size()),
                    static_cast<std::uint32_t>(capture.commands().draws.size()),capture.memory().inputCount,
                    capture.controls().previousWord,capture.controls().nextWord}};return true;
            };
            if(renderStoppedColor || commitStoppedColor){owned=registry.snapshotStoppedNativeColorBatch(*inspectColorProgram,bins,submission.writeWord);
                status=owned && inspect(*owned)?1:2;
            }else status=registry.inspectStoppedNativeColorBatch(*inspectColorProgram,bins,submission.writeWord,inspect)?1:2;
        }
        const auto ring=registry.graphicsRingConfiguration();
        // Private metadata only; the reproduction harness validates this extra
        // record separately. No game payload, host pointer or native work count.
        std::cout<<"stopped_native_color_inspection="<<status;
        for(auto count:counts)std::cout<<','<<count;
        std::cout<<','<<(ring?ring->readWord:0)<<'\n';
        if(commitStoppedColor){
            using namespace recomp::gpu;
            // ALL guest workers have joined. This separate experiment commits
            // only native backend state and reconstructed SCRATCH outputs.
            // It does not reuse the read-only/lifecycle diagnostic as a live
            // publisher, expose guest pointers, or permit guest code to resume.
            unsigned committed=0,fences=0,scratchRptr=0,retainedWords=0,unknownRegs=0;
            bool unchangedMemory=false,stateMatches=false,oneShot=false,cacheRejected=false;
            if(owned && nativeGraphics){
                auto scratch=NativeColorScratchMemory::create(*owned);
                std::vector<NativePhysicalRange> ranges;
                const auto& request=owned->memory();
                ranges.insert(ranges.end(),request.controls.begin(),request.controls.end());
                ranges.insert(ranges.end(),request.outputs.begin(),request.outputs.end());
                std::vector<std::vector<std::uint8_t>> before;
                for(const auto& range:ranges){auto bytes=registry.snapshotStoppedGraphicsPhysical(range.address,range.bytes);
                    if(!bytes)break;before.push_back(std::move(*bytes));}
                const auto prior=nativeGraphics->stateBatchSnapshot();
                std::vector<std::optional<std::uint32_t>> priorRegisters;
                for(unsigned index=0;index<0x8000;++index)priorRegisters.push_back(nativeGraphics->retainedRegister(index));
                if(scratch && before.size()==ranges.size()){
                    std::vector<std::uint8_t> expectedEdram(NativeColorEdramSnapshot::Bytes);
                    struct Scope {const NativeColorBatchCapture& capture;const NativeColorResolvedMemory& memory;
                        std::vector<std::uint8_t>& expected;unsigned mode,calls=0,fences=0;};
                    Scope scope{*owned,scratch->memory(),expectedEdram,commitStoppedColor};
                    const NativeColorPublisher publisher{&scope,[](void* context,const NativeColorChainExecutionResult& result) noexcept{
                        auto& s=*static_cast<Scope*>(context);++s.calls;s.fences=result.nativeFences;
                        if(s.calls!=1 || !result.edram || !result.copy || result.edram->knownBytes().size()!=s.expected.size())return false;
                        std::memcpy(s.expected.data(),result.edram->knownBytes().data(),s.expected.size());
                        if(s.mode==2)return false;
                        // Fault belongs to scratch only; original guest RAM is
                        // never modified. Fresh input matching must catch this.
                        if(s.mode==3)s.memory.inputs[0].bytes()[0]^=1;
                        const bool accepted=publishCapturedNativeColorOutputs(s.capture,s.memory,*result.copy,result.eventBytes.data(),result.eventBytes.size());
                        if(s.mode==3)s.memory.inputs[0].bytes()[0]^=1;
                        return accepted;
                    }};
                    committed=nativeGraphics->executeExperimentalOwnedColorBatch(*inspectColorProgram,*owned,publisher);
                    fences=scope.fences;
                    const auto* rp=scratch->memory().outputs[3].bytes();
                    scratchRptr=(unsigned{rp[0]}<<24)|(unsigned{rp[1]}<<16)|(unsigned{rp[2]}<<8)|rp[3];
                    const auto state=nativeGraphics->stateBatchSnapshot();
                    stateMatches=state.binMask==prior.binMask && state.binSelect==prior.binSelect && state.knownBinWords==prior.knownBinWords &&
                        state.batches==prior.batches+committed && state.invalidationEpoch==prior.invalidationEpoch+committed*3 &&
                        state.experimentalColorBatches==prior.experimentalColorBatches+committed &&
                        state.experimentalIdleCachePairEpoch==prior.experimentalIdleCachePairEpoch;
                    for(unsigned index=0;index<0x8000;++index){
                        const auto value=nativeGraphics->retainedRegister(index);
                        auto expected=priorRegisters[index];
                        if(committed){const auto found=owned->commands().finalRegisters.find(index);
                            if(found!=owned->commands().finalRegisters.end() && index>=0x2000)expected=found->second.bits.value;
                            if(index==0x5C8 || index==0xA2F || index==0xA30 || index==0xA31 || index==0x21F9)expected=std::nullopt;
                        }
                        if(value!=expected)stateMatches=false;
                    }
                    for(unsigned index:{0x5C8u,0xA2Fu,0xA30u,0xA31u,0x21F9u})unknownRegs+=!nativeGraphics->retainedRegister(index);
                    for(unsigned offset=0;offset<expectedEdram.size();offset+=4){unsigned value=73;
                        const bool known=nativeGraphics->retainedColorEdramWord(NativeColorEdramSnapshot::Begin+offset,value);
                        const auto* p=expectedEdram.data()+offset;
                        const auto expected=p[0]|(unsigned{p[1]}<<8)|(unsigned{p[2]}<<16)|(unsigned{p[3]}<<24);
                        if(known && value==expected)++retainedWords;
                        if(known!=bool(committed) || (known && value!=expected) || (!known && value!=73))stateMatches=false;
                    }
                    for(unsigned at:{0u,NativeColorEdramSnapshot::Begin-4,NativeColorEdramSnapshot::Begin+NativeColorEdramSnapshot::Bytes,AliasEdramBytes-4}){
                        unsigned value=73;if(nativeGraphics->retainedColorEdramWord(at,value) || value!=73)stateMatches=false;
                    }
                    const auto calls=scope.calls;
                    oneShot=!nativeGraphics->executeExperimentalOwnedColorBatch(*inspectColorProgram,*owned,publisher) && scope.calls==calls;
                    NativeInitializationReadback unused;
                    cacheRejected=!nativeGraphics->completeEmptyGuestCacheCoherency({0,0x1000,0x01000000}) &&
                        !nativeGraphics->completeNativeStateBatch({0xC0006000,7},unused) &&
                        !nativeGraphics->completeExperimentalIdleCachePair({0x5C8,0x20000,0xE00,1,0xE40,1},unused);
                    unchangedMemory=true;
                    for(unsigned i=0;i<ranges.size();++i){auto bytes=registry.snapshotStoppedGraphicsPhysical(ranges[i].address,ranges[i].bytes);
                        if(!bytes || *bytes!=before[i])unchangedMemory=false;}
                    const auto after=registry.graphicsRingConfiguration();
                    unchangedMemory=unchangedMemory && after && ring && after->readWord==ring->readWord;
                }
            }
            std::cout<<"stopped_native_color_scratch_commit="<<commitStoppedColor<<','<<committed<<','<<fences<<','<<scratchRptr<<','
                <<retainedWords<<','<<unsigned(stateMatches)<<','<<unsigned(unchangedMemory)<<','<<unsigned(oneShot)<<','<<unsigned(cacheRejected)<<','<<unknownRegs<<'\n';
        }
        if(renderStoppedColor){
            // All generated workers have stopped. Keep independent before/after
            // copies of every output and control range; never pass these guest
            // addresses to the diagnostic renderer or publish its returned bytes.
            std::vector<recomp::gpu::NativePhysicalRange> protectedRanges;
            std::vector<std::vector<std::uint8_t>> beforeBytes;
            std::vector<std::optional<std::uint32_t>> beforeRegisters;
            recomp::gpu::VulkanEngine::StateBatchSnapshot beforeState;
            bool unchangedMemory=bool(owned && nativeGraphics);
            if(unchangedMemory){
                const auto& memory=owned->memory();
                protectedRanges.insert(protectedRanges.end(),memory.controls.begin(),memory.controls.end());
                protectedRanges.insert(protectedRanges.end(),memory.outputs.begin(),memory.outputs.end());
                for(const auto& range:protectedRanges){
                    auto bytes=registry.snapshotStoppedGraphicsPhysical(range.address,range.bytes);
                    if(!bytes){unchangedMemory=false;break;}
                    beforeBytes.push_back(std::move(*bytes));
                }
                beforeState=nativeGraphics->stateBatchSnapshot();
                beforeRegisters.reserve(0x8000);
                for(unsigned index=0;index<0x8000;++index)beforeRegisters.push_back(nativeGraphics->retainedRegister(index));
            }
            recomp::gpu::NativeColorChainExecutionResult rendered;
            const auto execute=[&](const auto& capture){
                return nativeGraphics->executeStoppedColorBatchDiagnostic(*inspectColorProgram,capture,rendered);
            };
            const bool completed=unchangedMemory && (lockedStoppedColor?
                registry.runStoppedNativeColorDiagnostic(*inspectColorProgram,initialColorBins,submission.writeWord,execute):execute(*owned));
            if(lockedStoppedColor)std::cout<<"stopped_native_color_lifecycle="<<unsigned(completed)<<'\n';
            const auto after=registry.graphicsRingConfiguration();
            const bool unchanged=after && ring && after->readWord==ring->readWord;
            if(unchangedMemory)for(std::size_t index=0;index<protectedRanges.size();++index){
                const auto& range=protectedRanges[index];
                const auto bytes=registry.snapshotStoppedGraphicsPhysical(range.address,range.bytes);
                if(!bytes || *bytes!=beforeBytes[index])unchangedMemory=false;
            }
            bool unchangedState=completed;
            if(completed){
                const auto state=nativeGraphics->stateBatchSnapshot();
                unchangedState=state.binMask==beforeState.binMask && state.binSelect==beforeState.binSelect &&
                    state.knownBinWords==beforeState.knownBinWords && state.batches==beforeState.batches &&
                    state.invalidationEpoch==beforeState.invalidationEpoch &&
                    state.experimentalColorBatches==beforeState.experimentalColorBatches &&
                    state.experimentalIdleCachePairEpoch==beforeState.experimentalIdleCachePairEpoch;
                for(unsigned index=0;index<beforeRegisters.size();++index)
                    if(beforeRegisters[index]!=nativeGraphics->retainedRegister(index))unchangedState=false;
            }
            recomp::gpu::NativeColorChainExecutionResult repeated;repeated.nativeFences=99;repeated.eventBytes.fill(0xA5);
            const bool oneShot=completed && !nativeGraphics->executeStoppedColorBatchDiagnostic(*inspectColorProgram,*owned,repeated) &&
                repeated.nativeFences==99 && !repeated.copy && !repeated.edram && !repeated.pendingRegisters && !repeated.pendingState && !repeated.auditedTimeline &&
                std::all_of(repeated.eventBytes.begin(),repeated.eventBytes.end(),[](auto byte){return byte==0xA5;});
            std::cout<<"stopped_native_color_render="<<unsigned(completed)<<','<<rendered.nativeFences<<','
                <<(rendered.edram?rendered.edram->knownBytes().size()/4:0)<<','<<unsigned(unchanged)<<'\n';
            std::cout<<"stopped_native_color_isolation="<<unsigned(unchangedMemory)<<','<<unsigned(unchangedState)<<','<<unsigned(oneShot)<<'\n';
            unsigned pendingCount=0,unknownEffects=0,highWrites=0;bool pendingMatches=completed && rendered.pendingRegisters.has_value();
            if(pendingMatches){
                const auto& journal=*rendered.pendingRegisters;pendingCount=static_cast<unsigned>(journal.size());
                for(unsigned index:{0x5C8u,0xA2Fu,0xA30u,0xA31u,0x21F9u})unknownEffects+=!journal.count(index);
                for(const auto& item:owned->commands().finalRegisters)if(item.first>=0x2000 && item.first!=0x21F9){
                    const auto found=journal.find(item.first);++highWrites;
                    if(found==journal.end() || found->second!=std::make_pair(item.second.bits.value,item.second.bits.known))pendingMatches=false;
                }
            }
            std::cout<<"stopped_native_color_registers="<<pendingCount<<','<<unknownEffects<<','<<highWrites<<','<<unsigned(pendingMatches)<<'\n';
            bool stateMatches=completed && rendered.pendingState.has_value();
            if(stateMatches){const auto& state=*rendered.pendingState;const auto& bins=owned->commands().finalBins;
                stateMatches=state.binMask==(std::uint64_t{bins.words[0]}|(std::uint64_t{bins.words[1]}<<32)) &&
                    state.binSelect==(std::uint64_t{bins.words[2]}|(std::uint64_t{bins.words[3]}<<32)) &&
                    state.knownBinWords==bins.knownWords && state.batches==beforeState.batches+1 &&
                    state.experimentalColorBatches==beforeState.experimentalColorBatches+1 &&
                    state.invalidationEpoch==beforeState.invalidationEpoch+3 &&
                    state.experimentalIdleCachePairEpoch==beforeState.experimentalIdleCachePairEpoch;
            }
            std::cout<<"stopped_native_color_pending_state="<<unsigned(stateMatches)<<'\n';
            if(rendered.auditedTimeline){const auto& timeline=*rendered.auditedTimeline;
                std::cout<<"stopped_native_color_timeline="<<timeline.completedStages()<<','<<timeline.depthWaits()<<','
                    <<timeline.invalidations()<<','<<timeline.idleCleans()<<','<<timeline.cacheFlushes()<<','<<timeline.literalEvents()<<'\n';
            }
        }
    }
#endif
    if(!inspectOwnershipRanges.empty()){
        const bool inspectRequest=postSwapCaptureScope && std::all_of(inspectOwnershipRanges.begin(),inspectOwnershipRanges.end(),
            [&](const auto& r){return postSwapCaptureScope->permitsOwnershipQuery(r[0],r[1]);});
        const bool inspectOwners=inspectWork || inspectRequest;
        std::cout << "stopped_ownership_status=" << unsigned(inspectOwners) << "\nstopped_ownership_requests=[";
        for(std::size_t i=0;i<inspectOwnershipRanges.size();++i){
            if(i)std::cout << ',';
            std::cout << '[' << inspectOwnershipRanges[i][0] << ',' << inspectOwnershipRanges[i][1] << ']';
        }
        std::cout << "]\nstopped_ownership_results=[";
        if(inspectOwners)for(std::size_t i=0;i<inspectOwnershipRanges.size();++i){
            if(i)std::cout << ',';
            const auto owner=registry.stoppedGraphicsPhysicalOwnership(inspectOwnershipRanges[i][0],inspectOwnershipRanges[i][1]);
            if(!owner)std::cout << "null";
            else std::cout << '[' << owner->physicalBase << ',' << owner->virtualBase << ',' << owner->byteCount
                << ',' << owner->protection << ',' << unsigned(owner->writable) << ']';
        }
        std::cout << "]\n";
    }
    if (!capturePhysicalRanges.empty()) {
        std::vector<std::vector<std::uint8_t>> bytes;
        const auto nativeCaptureBudget=deferredPendingSubmission?16384u:4096u;
        const bool nativeCapture=nativePendingSubmission && captureByteBudget<=nativeCaptureBudget &&
            std::all_of(capturePhysicalRanges.begin(),capturePhysicalRanges.end(),[&](const auto& r){
                const auto& p=*nativePendingSubmission;
                return (r[0]>=p.rootPhysical && std::uint64_t{r[0]}+r[1]<=std::uint64_t{p.rootPhysical}+12) ||
                    (r[0]>=p.childPhysical && std::uint64_t{r[0]}+r[1]<=std::uint64_t{p.childPhysical}+p.childWords*4) ||
                    (nativeScalerWrapper && r[0]>=nativeScalerWrapper->scratchPhysical() &&
                        std::uint64_t{r[0]}+r[1]<=std::uint64_t{nativeScalerWrapper->scratchPhysical()}+32);
            });
        const bool requestCapture=postSwapCaptureScope && std::all_of(capturePhysicalRanges.begin(),capturePhysicalRanges.end(),
            [&](const auto& r){return postSwapCaptureScope->permitsCommandPayload(r[0],r[1]);});
        const unsigned status = !inspectWork && !nativeCapture && !requestCapture ? 0 :
            (recomp::gpu::captureStoppedPhysicalRanges(capturePhysicalRanges,
                [&](auto address, auto count) { return registry.snapshotStoppedGraphicsPhysical(address,count); }, bytes,captureByteBudget) ? 1 : 2);
        std::cout << "stopped_physical_byte_budget=" << captureByteBudget << "\nstopped_physical_capture=" << status << "\nstopped_physical_requests=[";
        for (std::size_t i = 0; i < capturePhysicalRanges.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << '[' << capturePhysicalRanges[i][0] << ',' << capturePhysicalRanges[i][1] << ']';
        }
        std::cout << "]\n";
        std::cout << "stopped_physical_ownership=[";
        if(status==1)for(std::size_t i=0;i<capturePhysicalRanges.size();++i){
            if(i)std::cout << ',';
            const auto owner=registry.stoppedGraphicsPhysicalOwnership(capturePhysicalRanges[i][0],capturePhysicalRanges[i][1]);
            if(!owner)std::cout << "null";
            else std::cout << '[' << owner->physicalBase << ',' << owner->virtualBase << ',' << owner->byteCount
                << ',' << owner->protection << ',' << unsigned(owner->writable) << ']';
        }
        std::cout << "]\n";
        if (status == 1) for (std::size_t i = 0; i < bytes.size(); ++i) {
            std::cout << "stopped_physical_blob=" << capturePhysicalRanges[i][0] << ',' << capturePhysicalRanges[i][1] << ',';
            constexpr char digits[] = "0123456789abcdef";
            for (auto b : bytes[i]) std::cout << digits[b >> 4] << digits[b & 15];
            std::cout << '\n';
        }
    }
    if (const auto ring = registry.graphicsRingConfiguration()) {
        if (const auto retired=registry.retiredGraphicsRingReadWord())
            std::cout << "native_ring_restart=1," << *retired << '\n';
        std::cout << "graphics_ring_physical=" << ring->physicalAddress << '\n'
                  << "graphics_ring_virtual=" << ring->virtualAddress << '\n'
                  << "graphics_ring_bytes=" << ring->bytes << '\n'
                  << "graphics_ring_read_word=" << ring->readWord << '\n';
        if (ring->writeBack) {
            std::cout << "graphics_wb_physical=" << ring->writeBack->physicalAddress << '\n'
                      << "graphics_wb_virtual=" << ring->writeBack->virtualAddress << '\n'
                      << "graphics_wb_exponent=" << ring->writeBack->blockSizeLog2 << '\n';
        }
        if (const auto identifier = registry.graphicsSystemIdentifierAddress()) {
            std::cout << "graphics_identifier_virtual=" << *identifier << '\n';
        }
    }
    std::cout << "title_termination_registrations=" << registry.titleTerminationRegistrations().size() << '\n';
    if (execution.scalarAccess) {
        const auto& access = *execution.scalarAccess;
        std::cout << "scalar_address=" << access.address << '\n'
                  << "scalar_width=" << access.width << '\n'
                  << "scalar_value=" << access.value << '\n'
                  << "scalar_write=" << access.write << '\n'
                  << "scalar_region=" << static_cast<std::uint32_t>(access.region) << '\n';
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::HostException) {
        // The guard already bounded and sanitised this to one printable line.
        std::cout << "stop_exception=" << execution.detail << '\n';
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::UnimplementedImport
            || execution.reason == recomp::bootstrap::GuestExecutionStopReason::RejectedImport) {
        std::cout << "stop_import=" << execution.detail << '\n';
        if (execution.importArguments) {
            const auto& arguments = *execution.importArguments;
            std::cout << "import_stack_pointer=" << arguments.stackPointer << '\n';
            for (std::size_t i = 0; i < arguments.registers.size(); ++i) {
                std::cout << "import_r" << i + 3 << '=' << arguments.registers[i] << '\n';
            }
        }
    } else {
        std::cout << "stop_import=none\n";
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::RejectedImport
            && execution.detail == "__imp__KeSetBasePriorityThread") {
        if (const auto request = registry.lastThreadPriorityRequest()) {
            std::cout << "priority_increment=" << request->increment << '\n'
                      << "priority_previous=" << request->previousIncrement << '\n'
                      << "priority_initial=" << request->initial << '\n'
                      << "priority_current=" << request->current << '\n'
                      << "priority_suspended=" << request->suspended << '\n'
                      << "priority_returned=" << request->returned << '\n';
        }
    }
    for (std::size_t i = 0; i < functionLabels.size(); ++i) {
        std::cout << "function_label=" << functionLabels.at(i) << '\n';
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::RejectedImport &&
        execution.detail == "__imp__RtlEnterCriticalSection") {
        if (const auto state=registry.lastCriticalSectionRejection()) {
            std::cout << "critical_section_rejection=";
            for(std::size_t i=0;i<state->size();++i) {if(i)std::cout<<',';std::cout<<(*state)[i];}
            std::cout << '\n';
        }
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::RejectedImport &&
        execution.detail == "__imp__KeWaitForMultipleObjects" && execution.importArguments) {
        const auto count=static_cast<std::uint32_t>(execution.importArguments->registers[0]);
        const auto array=static_cast<std::uint32_t>(execution.importArguments->registers[1]);
        if (count && count<=64 && array && !(array&3U) &&
            std::uint64_t{array}+count*4<=recomp::bootstrap::GuestAddressSpace::Size &&
            !(array<0x7FC90000 && std::uint64_t{array}+count*4>0x7FC80000)) {
            std::cout << "dispatcher_wait_objects=[";
            for (std::uint32_t i=0;i<count;++i) {
                const auto* p=addressSpace.translate(array+i*4);
                const auto address=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
                const auto snapshot=registry.inspectDispatcherEvent(address);
                if (i) std::cout << ',';
                std::cout << '[' << address << ',' << bool(snapshot) << ','
                          << (snapshot?(*snapshot)[0]:0) << ',' << (snapshot?(*snapshot)[1]:0) << ','
                          << (snapshot?(*snapshot)[2]:0) << ']';
            }
            std::cout << "]\n";
        }
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::RejectedImport
            && execution.detail == "__imp__NtCreateFile") {
        if (const auto request = registry.lastFileCreateRequest()) {
            std::cout << "file_create_access=" << request->access << '\n'
                      << "file_create_attributes=" << request->attributes << '\n'
                      << "file_create_share=" << request->share << '\n'
                      << "file_create_disposition=" << request->disposition << '\n'
                      << "file_create_options=" << request->options << '\n'
                      << "file_create_object_root=" << request->objectRoot << '\n'
                      << "file_create_object_flags=" << request->objectFlags << '\n'
                      << "file_create_lookup=" << request->lookup << '\n'
                      << "file_create_record=" << (request->lookup == 1 && !request->mountedContent ? lastFileMetadataIndex : 0) << '\n';
            std::cout << "file_create_path=";
            for(unsigned char c:request->path)std::cout << (c>=32 && c<127?char(c):'?');
            std::cout << '\n';
        }
    }
    if (execution.reason == recomp::bootstrap::GuestExecutionStopReason::RejectedImport
            && (execution.detail == "__imp__XamContentCreateEx" || execution.detail == "__imp__XamContentClose")) {
        if (const auto mask = registry.contentMountRejection()) { std::cout << "content_mount_rejection=" << mask << '\n'; }
    }
    if (cooperative) {
        std::cout << "workers_created=" << cooperativeResult.workersCreated << '\n'
                  << "workers_entered=" << cooperativeResult.workersEntered << '\n'
                  << "context_switches=" << cooperativeResult.contextSwitches << '\n'
                  << "wait_operations=" << cooperativeResult.waitOperations << '\n';
    }
    return 0;
}

#if defined(RECOMP_PRIVATE_TRACE_LIBRARY)
namespace {
[[gnu::noinline]] int runTraceWithInputObserver(int argc,char** argv,const RecompPrivateTraceCallbacksV2* callbacks,
        const RecompPrivateTraceCallbacksV3* liveCallbacks,const RecompPrivateTraceCallbacksV5* inputCallbacks) {
    if(!recomp::input::delivery::embedding::beginRun())return 3;
    // Only normal core completion admits release. Exceptional unwind does not
    // prove all workers joined; keep registration frozen and its owner alive.
    const int result=runTraceMain(argc,argv,callbacks,liveCallbacks,inputCallbacks);
    recomp::input::delivery::embedding::finishAfterJoin();
    return result;
}
std::atomic_flag privateTraceEntered=ATOMIC_FLAG_INIT;
int privateTraceEntry(int argc,char** argv,const RecompPrivateTraceCallbacksV2* callbacks,
        const RecompPrivateTraceCallbacksV3* liveCallbacks=nullptr,
        const RecompPrivateTraceCallbacksV5* inputCallbacks=nullptr,IosSharedDisplay* shared=nullptr) noexcept {
    if(argc<1 || argc>1024 || !argv)return 2;
    for(int i=0;i<argc;++i)if(!argv[i] || strnlen(argv[i],4097)>4096)return 2;
    if(privateTraceEntered.test_and_set(std::memory_order_acq_rel))return 3;
    if(shared){iosSharedDisplay=shared;recomp::ios_shared::enabled=true;}
    if(const int refused=::recomp::appdir::fromLoader(argv[0]))return refused; // data-paths: the files directory, before any thread
    try {return runTraceWithInputObserver(argc,argv,callbacks,liveCallbacks,inputCallbacks);}
    catch(...) {return 2;}
}
}
extern "C" int recomp_run_private_trace_v1(int argc,char** argv) noexcept {
    return privateTraceEntry(argc,argv,nullptr);
}
extern "C" int recomp_run_private_trace_v2(int argc,char** argv,const RecompPrivateTraceCallbacksV2* callbacks) noexcept {
    if(!callbacks || callbacks->structureBytes!=sizeof(RecompPrivateTraceCallbacksV2) || callbacks->version!=2 ||
        callbacks->reserved0 || callbacks->reserved1 || !callbacks->stoppedRgba)return 2;
    const auto owned=*callbacks;
    return privateTraceEntry(argc,argv,&owned);
}
extern "C" int recomp_run_private_trace_v3(int argc,char** argv,const RecompPrivateTraceCallbacksV3* callbacks) noexcept {
    if(!callbacks || callbacks->structureBytes!=sizeof(RecompPrivateTraceCallbacksV3) || callbacks->version!=3 ||
        callbacks->reserved0 || callbacks->reserved1 || !callbacks->orderedRgba)return 2;
    const auto owned=*callbacks;
    return privateTraceEntry(argc,argv,nullptr,&owned);
}
extern "C" int recomp_run_private_trace_v4(int argc,char** argv,const RecompPrivateTraceCallbacksV4* callbacks) noexcept {
    if(!callbacks || callbacks->structureBytes!=sizeof(RecompPrivateTraceCallbacksV4) || callbacks->version!=4 ||
        callbacks->reserved0 || callbacks->reserved1 || !callbacks->orderedRgba)return 2;
    recomp::private_trace::DisplayCallbacksV4 owned(*callbacks);
    const RecompPrivateTraceCallbacksV3 bridge{sizeof(RecompPrivateTraceCallbacksV3),3,0,0,&owned,
        [](void* user,const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
            return static_cast<recomp::private_trace::DisplayCallbacksV4*>(user)->consume(rgba,bytes,width,height);}};
    return privateTraceEntry(argc,argv,nullptr,&bridge);
}
extern "C" int recomp_run_private_trace_v5(int argc,char** argv,const RecompPrivateTraceCallbacksV5* callbacks) noexcept {
    if(!callbacks || callbacks->structureBytes!=sizeof(*callbacks) || callbacks->version!=5 ||
       callbacks->reserved0 || callbacks->reserved1 || !callbacks->gamepadState || !callbacks->gamepadCapabilities)return 2;
    const auto owned=*callbacks;
    const auto& d=owned.display;
    if(d.structureBytes!=sizeof(d) || d.version!=4 || d.reserved0 || d.reserved1 || !d.orderedRgba)return 2;
    recomp::private_trace::DisplayCallbacksV4 display(d);
    const RecompPrivateTraceCallbacksV3 bridge{sizeof(RecompPrivateTraceCallbacksV3),3,0,0,&display,
        [](void* user,const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
            return static_cast<recomp::private_trace::DisplayCallbacksV4*>(user)->consume(rgba,bytes,width,height);}};
    return privateTraceEntry(argc,argv,nullptr,&bridge,&owned);
}
extern "C" int recomp_run_private_trace_gpu_v1(int argc,char** argv,const RecompPrivateTraceCallbacksV5* callbacks,
        const RecompIosGpuCallbacksV1* gpu) noexcept {
    bool enabled=false;try{enabled=recomp::ios_shared::requested();}catch(...){return 2;}
    if(!enabled)return recomp_run_private_trace_v5(argc,argv,callbacks);
    if(!callbacks || callbacks->structureBytes!=sizeof(*callbacks) || callbacks->version!=5 ||
       callbacks->reserved0 || callbacks->reserved1 || !callbacks->gamepadState || !callbacks->gamepadCapabilities ||
       !gpu || gpu->structureBytes!=sizeof(*gpu) || gpu->version!=1 || gpu->reserved0 || gpu->reserved1 || !gpu->frame || !gpu->close)return 2;
    const auto& d=callbacks->display;
    if(d.structureBytes!=sizeof(d) || d.version!=4 || d.reserved0 || d.reserved1 || !d.orderedRgba)return 2;
    // Permanent owner: exceptional worker unwind cannot leave callback storage dangling.
    auto* owner=new IosSharedDisplay(d,*gpu);
    const auto input=*callbacks;
    const RecompPrivateTraceCallbacksV3 bridge{sizeof(RecompPrivateTraceCallbacksV3),3,0,0,owner,
        [](void* user,const uint8_t* rgba,uint32_t bytes,uint32_t width,uint32_t height) noexcept {
            return static_cast<IosSharedDisplay*>(user)->cpu(rgba,bytes,width,height);}};
    const int result=privateTraceEntry(argc,argv,nullptr,&bridge,&input,owner);
    if(result==3)return result; // old running entry owns all globals
    if(result!=0)owner->fail();
    owner->close(false);return result;
}
#if (defined(__ANDROID__) || defined(__APPLE__))
namespace {
// v255: V6's display bridge. The first frame decides for the run: the library's presenter claims the window (direct
// present) or the app's own renderer gets every frame as with V5. A presenter that fails or gives the window back hands
// the rest of the run to the app's renderer (the V4 sequence continues from its next number).
class DirectDisplayBridge final {
public:
    explicit DirectDisplayBridge(const RecompPrivateTraceCallbacksV4& callbacks):app_(callbacks){}
    int consume(const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
        try {
            std::lock_guard lock(mutex_);
            auto& presenter=recomp::gpu::direct::presenter();
#if defined(__APPLE__)
            // ios-direct-present: while the app's layer is not on screen yet the claim stays open and the frame is not shown
            // (it would otherwise go to the app's own presenter, which then owns the layer for the run).
            if(mode_==Mode::Undecided) {
                if(presenter.claim())mode_=Mode::Direct;
                else if(presenter.claimPending())return 1;
                else mode_=Mode::App;
            }
#else
            if(mode_==Mode::Undecided)mode_=presenter.claim()?Mode::Direct:Mode::App;
#endif
            if(mode_==Mode::Direct) {
                if(presenter.presentCpu(rgba,bytes,width,height))return 1;
                if(presenter.usable())return 1; // (dropped: an acquire timeout or an out-of-date swapchain, counted there)
                presenter.giveBack();mode_=Mode::App;
                __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIRECT_PRESENT handed_to_app=1");
            }
            return app_.consume(rgba,bytes,width,height);
        } catch(...) {return 0;}
    }
private:
    enum class Mode {Undecided,Direct,App};
    recomp::private_trace::DisplayCallbacksV4 app_;
    std::mutex mutex_;
    Mode mode_=Mode::Undecided;
};
}
#ifdef RECOMP_THOR_EDITION
// hud2 (v575): the app's second window (private_trace_hud_api.h), set once before the V6 entry.
extern "C" int recomp_set_hud_window_v1(const RecompHudWindowV1* w) noexcept {
    if(!w || w->structureBytes!=sizeof(*w) || w->version!=1 || w->reserved0 || w->reserved1 || !w->acquireWindow || !w->releaseWindow ||
       !w->windowGeneration)return 2;
    recomp::gpu::hud2::screen().setWindow({w->windowUser,w->acquireWindow,w->releaseWindow,w->windowGeneration});
    return 0;
}
#if defined(RECOMP_DUO_EDITION)
// ios-duo (2026-10-24): the app's split (1) / fullscreen (0) state; any thread, any time (hud_screen.h Screen::setShown).
extern "C" void recomp_duo_set_lower_screen_v1(int on) noexcept {recomp::gpu::hud2::screen().setShown(on!=0);}
// ios-duo: the lower screen's map rectangle (x0, y0, x1, y1 in its pixels); 0 = not laid out yet.
extern "C" int recomp_duo_lower_map_rect_v1(int* r) noexcept {
    auto& s=recomp::gpu::hud2::screen();if(!r)return 0;
    for(int i=0;i<4;++i)r[i]=s.duoMapRect[i].load(std::memory_order_relaxed);
    return r[2]>r[0] && r[3]>r[1];
}
#endif
#endif /* RECOMP_THOR_EDITION */
extern "C" int recomp_run_private_trace_v6(int argc,char** argv,const RecompPrivateTraceCallbacksV6* callbacks) noexcept {
    if(!callbacks || callbacks->structureBytes!=sizeof(*callbacks) || callbacks->version!=6 || callbacks->reserved0 ||
       callbacks->reserved1 || !callbacks->acquireWindow || !callbacks->releaseWindow || !callbacks->windowGeneration)return 2;
    const auto owned=*callbacks;
    const auto& input=owned.input;
    if(input.structureBytes!=sizeof(input) || input.version!=5 || input.reserved0 || input.reserved1 || !input.gamepadState ||
       !input.gamepadCapabilities)return 2;
    const auto& d=input.display;
    if(d.structureBytes!=sizeof(d) || d.version!=4 || d.reserved0 || d.reserved1 || !d.orderedRgba)return 2;
    try {
        recomp::gpu::direct::presenter().setWindow({owned.windowUser,owned.acquireWindow,owned.releaseWindow,owned.windowGeneration});
        DirectDisplayBridge display(d);
        const RecompPrivateTraceCallbacksV3 bridge{sizeof(RecompPrivateTraceCallbacksV3),3,0,0,&display,
            [](void* user,const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
                return static_cast<DirectDisplayBridge*>(user)->consume(rgba,bytes,width,height);}};
        return privateTraceEntry(argc,argv,nullptr,&bridge,&input);
    } catch(...) {return 2;}
}
#endif
#else
int main(int argc,char** argv) {return runTraceMain(argc,argv);}
#endif
