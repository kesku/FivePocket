#include "vulkan_queue_lock_all.h" // v177: every VkQueue call in this file holds the shared queue lock
#include "recomp_app_dir.h" // data-paths: the app's files directory. v179: vulkan_engine.h, native_tile_command_interrupt.h and native_tile_scene_handoff.h are attempt copies (the journal fix).
#include "vulkan_engine.h"
#include "apple/ios_gpu_lifecycle.h"
#include "vulkan_queue_lock.h" // v119
#include <unistd.h> // v170: access()
#include "native_display_information.h"
#include "vulkan_constant_points.h"
#include "vulkan_maintenance_points.h"
#include "xenos_native_initialization.h"
#include "xenos_native_state_batch.h"
#include "native_constant_shaders.h"
#include "native_color_chain_execution.h"
#include "native_deferred_color_capture.h"
#include "native_deferred_color_result.h"
#include "native_swap_gpu_execution.h"
#include "native_post_output_swap_gpu_execution.h"
#include "native_post_output_swap_native_actions.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <atomic>
#include <array>   // v110
#include <chrono>  // v110
#if defined(__ANDROID__)
#include <android/log.h>
#endif
// v110 (guest_cooperative_executor.cpp): whether an exclusive scope owns the executor (the world is stopped).
namespace recomp::bootstrap { bool guestExclusiveScopeActive() noexcept __attribute__((weak)); }

namespace recomp::gpu {
// Attempt v78: every native queue fence (fenceNativeQueue) counted, so the ring submission handler can tell whether it
// fenced the queue (trace_runner.cpp submits the generic renderer's recorded work right after such a handler).
namespace {std::atomic<std::uint64_t> nativeQueueFences{0};}
std::uint64_t nativeQueueFenceCount() noexcept {return nativeQueueFences.load(std::memory_order_relaxed);}
namespace {
// v110: every native queue fence (command buffer, submit, wait), by context: inside a tile interrupt or
// not, and with the world stopped (an executor exclusive scope active) or running. Cumulative,
// RENDER_DIAG_NATIVE_FENCE at most every ~10 s from the fencing thread.
struct NativeFenceTiming {std::atomic<std::uint64_t> n{0},ns{0},maxNs{0};};
std::array<NativeFenceTiming,4> nativeFenceTiming; // [irq?2:0 + stopped?1:0]
std::atomic<std::int64_t> nativeFenceNextLog{0};
void noteNativeFence(bool irq,bool stopped,std::chrono::steady_clock::time_point started) noexcept {
    const auto now=std::chrono::steady_clock::now();
    const auto ns=static_cast<std::uint64_t>(std::max<std::int64_t>(0,std::chrono::duration_cast<std::chrono::nanoseconds>(now-started).count()));
    auto& e=nativeFenceTiming[(irq?2:0)+(stopped?1:0)];
    e.n.fetch_add(1,std::memory_order_relaxed);e.ns.fetch_add(ns,std::memory_order_relaxed);
    auto seen=e.maxNs.load(std::memory_order_relaxed);
    while(ns>seen && !e.maxNs.compare_exchange_weak(seen,ns,std::memory_order_relaxed)){}
    const auto nowNs=std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    auto next=nativeFenceNextLog.load(std::memory_order_relaxed);
    if(nowNs<next || !nativeFenceNextLog.compare_exchange_strong(next,nowNs+10'000'000'000LL,std::memory_order_relaxed))return;
    if(!next)return; // the first fence arms the timer
    const auto v=[](const std::atomic<std::uint64_t>& a){return static_cast<unsigned long long>(a.load(std::memory_order_relaxed));};
    const auto& a=nativeFenceTiming;
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
#else
    std::fprintf(stderr,
#endif
        "RENDER_DIAG_NATIVE_FENCE other_n=%llu other_ms=%llu other_max_us=%llu other_stopped_n=%llu other_stopped_ms=%llu "
        "other_stopped_max_us=%llu irq_n=%llu irq_ms=%llu irq_max_us=%llu irq_stopped_n=%llu irq_stopped_ms=%llu irq_stopped_max_us=%llu\n",
        v(a[0].n),v(a[0].ns)/1000000,v(a[0].maxNs)/1000,v(a[1].n),v(a[1].ns)/1000000,v(a[1].maxNs)/1000,
        v(a[2].n),v(a[2].ns)/1000000,v(a[2].maxNs)/1000,v(a[3].n),v(a[3].ns)/1000000,v(a[3].maxNs)/1000);
}
} // namespace
bool VulkanEngine::bindNativeSwapCompletionClock(NativeCompletionClock clock) {
    if(attempted_ || nativeSwapClock_ || !clock)return false;
    nativeSwapClock_=std::move(clock);return true;
}
bool VulkanEngine::executeNativeSwapWrapper(const NativeSwapWrapper& plan,
        const std::function<bool()>& present,NativeSwapGpuResult& output) {
    if(!ready() || !constantInitializationCompleted_ || !experimentalColorAttempted_ || !colorEdram_ ||
        !warmRingInitialized_ || ringRestartPending_ || !nativeDisplayModeSelected_ ||
        !commandStorageCoherencyCompleted_ || nativeSwapWrapperCompleted_ || !nativeSwapClock_ || !present ||
        stateBatchSnapshot_.experimentalIdleCachePairEpoch==UINT64_MAX ||
        stateBatchSnapshot_.invalidationEpoch==UINT64_MAX)return false;
    // No GPU-backed guest bindings survive the owned color batch: only CPU
    // EDRAM/raw state. Full native ordering implements the explicit conservative
    // idle/cache-pair policy. Do not fabricate readable 0E00/0E40 request bits.
    // Prepare all allocating journal work before any GPU/presentation operation.
    auto journal=initializationRegisterJournal_;auto state=stateBatchSnapshot_;
    journal[0x21F9]={3,0xFFFFFFFF};
    ++state.experimentalIdleCachePairEpoch;++state.invalidationEpoch;
    NativeSwapGpuResult candidate;
    if(!executeNativeSwapGpuWrapper(context_,plan,nativeSwapClock_,present,candidate))return false;
    if(!validNativeSwapCompletion(plan,candidate))std::_Exit(4);
    static_assert(std::is_nothrow_move_assignable_v<decltype(journal)>);
    initializationRegisterJournal_=std::move(journal);stateBatchSnapshot_=state;
    nativeSwapWrapperCompleted_=true;output=candidate;return true;
}
bool VulkanEngine::executeNativePostOutputSwapWrapper(const NativePostOutputSwapWrapper& plan,
        const std::function<bool()>& present,const NativePostOutputSwapInterrupt& interrupt,NativePostOutputSwapGpuResult& output){
    unsigned base=0,mask=0;
    if(!ready() || !nativeSwapWrapperCompleted_ || !postOutputSwapStorageCompleted_ || !nativeOutputCompleted_ ||
        !nativePostOutputStorageCompleted_ || !postOutputColorExecutionAttempted_ || stateBatchSnapshot_.experimentalColorBatches!=2 ||
        !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ || !nativeDisplayModeSelected_ ||
        !nativeScalerConfigured() || !nativeGammaRamp_ || postOutputSwapAttempted_ || !nativeSwapClock_ || !present || !interrupt ||
        !queryNativeScratchWriteback(base,mask) || !plan.scratchStateMatches(base,mask) ||
        stateBatchSnapshot_.experimentalIdleCachePairEpoch==UINT64_MAX || stateBatchSnapshot_.invalidationEpoch==UINT64_MAX)return false;
    // No persistent GPU-backed guest cache survives this native resource model.
    // Preallocate the FULL journal before presentation or guest memory actions.
    auto journal=initializationRegisterJournal_;auto state=stateBatchSnapshot_;
    NativePostOutputSwapNativeActions actions(plan,journal,state);
    bool actionFailed=false;
    // This callable exists before any accepted work. waitIdle itself is owned
    // by the six-fence helper, not a recycled first-frame fence.
    const std::function<bool()>* activeWaitIdle=nullptr;
    const NativePostOutputSwapInterruptSequence::Action action=[&](const auto& op){
        if(!activeWaitIdle || actionFailed || !actions.apply(op,*activeWaitIdle)){actionFailed=true;return false;}
        return true;
    };
    const NativePostOutputSwapInterruptDispatch dispatch=[&](const std::function<bool()>& waitIdle){
        activeWaitIdle=&waitIdle;const auto completed=interrupt(action);activeWaitIdle=nullptr;
        if(!completed || actionFailed || !actions.complete())return false;
        unsigned currentBase=0,currentMask=0;
        return queryNativeScratchWriteback(currentBase,currentMask) && currentBase==base && currentMask==mask;
    };
    postOutputSwapAttempted_=true;
    NativePostOutputSwapGpuResult candidate;
    if(!executeNativePostOutputSwapGpuWrapper(context_,plan,nativeSwapClock_,present,dispatch,candidate))return false;
    if(!validNativePostOutputSwapCompletion(plan,candidate) || actionFailed || !actions.complete())std::_Exit(4);
    ++state.experimentalIdleCachePairEpoch;++state.invalidationEpoch;
    journal.find(0x21F9)->second={3,0xFFFFFFFF};
    static_assert(std::is_nothrow_move_assignable_v<decltype(journal)>);
    initializationRegisterJournal_=std::move(journal);stateBatchSnapshot_=state;
    postOutputSwapCompleted_=true;output=candidate;return true;
}
bool VulkanEngine::executeNativeDeferredSwapWrapper(const NativeDeferredSwapSubmission& submission,
        const std::function<bool()>& present,const NativeDeferredSwapInterrupt& interrupt,NativeDeferredSwapGpuResult& output){
    unsigned base=0,mask=0;const auto& plan=submission.swap();
    if(!ready() || !postOutputSwapCompleted_ || !deferredSwapStorageCompleted_ || !nativeOutputCompleted_ ||
        !deferredColorExecutionAttempted_ || !committedDeferredColorSubmission_ ||
        !committedDeferredColorSubmission_->sameRequest(submission) || stateBatchSnapshot_.experimentalColorBatches!=3 ||
        !colorEdram_ || colorEdram_->knownBytes().size()!=NativeColorEdramSnapshot::Bytes ||
        !warmRingInitialized_ || ringRestartPending_ || !nativeDisplayModeSelected_ ||
        !nativeScalerConfigured() || !nativeGammaRamp_ || deferredSwapAttempted_ || !nativeSwapClock_ || !present || !interrupt ||
        !queryNativeScratchWriteback(base,mask) || !plan.scratchStateMatches(base,mask) ||
        stateBatchSnapshot_.experimentalIdleCachePairEpoch==UINT64_MAX || stateBatchSnapshot_.invalidationEpoch==UINT64_MAX)return false;
    // The stored request was moved from a freshly COMMITTED prefix result,
    // never from the diagnostic path. Allocate every journal/callable before
    // attempting presentation or guest IRQ actions. No prefix rerendering.
    auto journal=initializationRegisterJournal_;auto state=stateBatchSnapshot_;
    NativeDeferredSwapNativeActions actions(plan,journal,state);
    bool actionFailed=false;
    const std::function<bool()>* activeWaitIdle=nullptr;
    const NativeDeferredSwapInterruptSequence::Action action=[&](const auto& op){
        if(!activeWaitIdle || actionFailed || !actions.apply(op,*activeWaitIdle)){actionFailed=true;return false;}
        return true;
    };
    const NativePostOutputSwapInterruptDispatch dispatch=[&](const std::function<bool()>& waitIdle){
        activeWaitIdle=&waitIdle;const auto completed=interrupt(action);activeWaitIdle=nullptr;
        if(!completed || actionFailed || !actions.complete())return false;
        unsigned currentBase=0,currentMask=0;
        return queryNativeScratchWriteback(currentBase,currentMask) && currentBase==base && currentMask==mask;
    };
    deferredSwapAttempted_=true;
    NativeDeferredSwapGpuResult candidate;
    if(!executeNativeDeferredSwapGpuWrapper(context_,plan,nativeSwapClock_,present,dispatch,candidate))return false;
    if(!validNativeDeferredSwapCompletion(plan,candidate) || actionFailed || !actions.complete())std::_Exit(4);
    ++state.experimentalIdleCachePairEpoch;++state.invalidationEpoch;
    journal.find(0x21F9)->second={3,0xFFFFFFFF};
    static_assert(std::is_nothrow_move_assignable_v<decltype(journal)>);
    initializationRegisterJournal_=std::move(journal);stateBatchSnapshot_=state;
    deferredSwapCompleted_=true;output=candidate;return true;
}
bool VulkanEngine::executeExperimentalOwnedColorBatch(const NativeColorChainProgram& program,
        const NativeColorBatchCapture& capture,const NativeColorPublisher& publisher) {
    if(!ready() || !constantInitializationCompleted_ || !warmRingInitialized_ || ringRestartPending_ ||
        stoppedColorDiagnosticAttempted_ || experimentalColorAttempted_ || colorEdram_ ||
        !publisher.context || !publisher.publish)return false;
    NativeRegisterJournal registers;NativeGraphicsState state;
    if(!prepareNativeColorState(capture,initializationRegisterJournal_,stateBatchSnapshot_,registers,state))return false;
    experimentalColorAttempted_=true;
    // ConservativeOwnedResourcesV1: command-selected immutable CPU snapshots
    // create fresh native bindings, with no cross-batch shader/texture cache.
    // All prior startup resources completed and were destroyed. Real recorded
    // barriers and four audited fences cover depth wait, idleclean, copy and
    // literal-event visibility. A zero guest range NEVER skips native work.
    // This is an explicit native policy, not a claim of Xbox cache equivalence.
    NativeColorChainExecutionResult completed;
    if(!executeCapturedNativeColorBatch(context_,program,capture,completed))return false;
    return publishAndCommitNativeColorState(capture,completed,registers,state,publisher,
        initializationRegisterJournal_,stateBatchSnapshot_,colorEdram_);
}
bool VulkanEngine::executeStoppedColorBatchDiagnostic(const NativeColorChainProgram& program,
        const NativeColorBatchCapture& capture,NativeColorChainExecutionResult& output) {
    if(!ready() || !constantInitializationCompleted_ || !warmRingInitialized_ || ringRestartPending_ ||
        stoppedColorDiagnosticAttempted_ || experimentalColorAttempted_)return false;
    NativeRegisterJournal registers;NativeGraphicsState state;
    if(!prepareNativeColorState(capture,initializationRegisterJournal_,stateBatchSnapshot_,registers,state))return false;
    stoppedColorDiagnosticAttempted_=true;
    // Independent temporary GPU resources; no live cache/register completion or
    // resident guest resource is fabricated. The returned EDRAM remains a
    // diagnostic snapshot, not committed persistent live engine state.
    NativeColorChainExecutionResult candidate;
    if(!executeCapturedNativeColorBatch(context_,program,capture,candidate))return false;
    candidate.pendingRegisters=std::move(registers);
    candidate.pendingState=state;
    output=std::move(candidate);return true;
}
bool VulkanEngine::executeStoppedPostOutputColorDiagnostic(const NativeColorChainProgram& program,
        const NativeColorBatchCapture& capture,NativeColorChainExecutionResult& output) {
    return executeAuditedPostOutputColor(program,capture,output);
}
bool VulkanEngine::executeStoppedDeferredColorDiagnostic(const NativeColorChainProgram& program,
        const NativeDeferredColorCapture& capture,NativeDeferredColorDiagnosticResult& output) {
    NativeDeferredColorExecutionResult fresh;
    if(!executeFreshDeferredColor(program,capture,fresh))return false;
    NativeDeferredColorDiagnosticResult candidate;
    candidate.copy=std::move(fresh.copy);candidate.edram=std::move(fresh.edram);
    candidate.auditedPrefixTimeline=fresh.auditedPrefixTimeline;
    candidate.pendingRegisters=std::move(fresh.pendingRegisters);candidate.pendingState=fresh.pendingState;
    candidate.nativeFences=fresh.nativeFences;candidate.leadingSetupFences=fresh.leadingSetupFences;
    output=std::move(candidate);return true; // Never publish or change resident state.
}
bool VulkanEngine::executeExperimentalDeferredColorPrefix(const NativeColorChainProgram& program,
        const NativeDeferredColorCapture& capture,const NativeDeferredColorPublisher& publisher) {
    if(!publisher.context || !publisher.publishCopy)return false;
    NativeDeferredColorExecutionResult fresh;
    if(!executeFreshDeferredColor(program,capture,fresh))return false;
    if(!publishAndCommitNativeDeferredColorState(capture,fresh,publisher,
        initializationRegisterJournal_,stateBatchSnapshot_,colorEdram_))return false;
    // No allocation/check/throw after guest copy and native state publication.
    // The owning identity was allocated before the leading GPU fence.
    static_assert(std::is_nothrow_move_assignable_v<decltype(committedDeferredColorSubmission_)>);
    committedDeferredColorSubmission_=std::move(fresh.submission);return true;
}
bool VulkanEngine::executeFreshDeferredColor(const NativeColorChainProgram& program,
        const NativeDeferredColorCapture& capture,NativeDeferredColorExecutionResult& output) {
    if(!ready() || !displayConfiguration_ || !postOutputSwapCompleted_ || !nativeOutputCompleted_ ||
        !deferredSwapStorageCompleted_ || deferredColorExecutionAttempted_ || !colorEdram_ ||
        colorEdram_->knownBytes().size()!=NativeColorEdramSnapshot::Bytes ||
        !warmRingInitialized_ || ringRestartPending_ || !nativeDisplayModeSelected_ ||
        !nativeScalerConfigured() || !nativeGammaRamp_ || stateBatchSnapshot_.experimentalColorBatches!=2 ||
        program.coordinates()!=NativeColorCoordinateProfile::Padded1280W1 ||
        capture.color().coordinates()!=program.coordinates() || !program.matches(capture.color().active()))return false;
    NativeDeferredColorSyncRequests sync;
    if(!planNativeDeferredColorSync(capture.submission(),capture.commands(),sync))return false;
    NativeRegisterJournal registers;NativeGraphicsState state;
    if(!prepareNativeDeferredColorState(capture,initializationRegisterJournal_,stateBatchSnapshot_,registers,state))return false;
    NativeDeferredColorExecutionResult candidate;
    try{candidate.submission=capture.submission();}catch(...){return false;} // Allocate before GPU.
    deferredColorExecutionAttempted_=true; // Both paths share the terminal attempt.
    if(!fenceNativeQueue())return false;
    NativeDeferredColorSyncProgress prefix;
    if(!prefix.finishAuditedLeadingFence())return false;
    NativeWordUpdatePlan noEvents;noEvents.bufferBytes=40;
    NativeColorChainExecutionResult result;
    // Fresh private resources, same current owned inputs, NO event words.
    // Four real fences/readback audits execute depth/color/color/copy. Do not
    // request the old completed-batch timeline: its final events are absent.
    if(!executeCapturedNativeColorChain(context_,program,capture.color(),capture.copy().copy.destinationBase,
        noEvents,result,false,&prefix,&apiDrawInitialHalf_))return false;
    unsigned busy=99;
    if(!prefix.complete() || prefix.completedStages()!=4 || prefix.leadingFences()!=1 ||
        prefix.invalidations()!=2 || prefix.copyIdleCleans()!=1 || prefix.depthWaits()!=1 ||
        prefix.cacheFlushes()!=1 || prefix.literalEvents() || !prefix.busyBit(busy) || busy)return false;
    if(result.nativeFences!=4 || !result.copy || !result.edram || result.auditedTimeline ||
        result.pendingRegisters || result.pendingState || result.leadingSetupFences ||
        !std::all_of(result.eventBytes.begin(),result.eventBytes.end(),[](auto b){return b==0x5A;}))return false;
    candidate.copy=std::move(result.copy);candidate.edram=std::move(result.edram);
    candidate.auditedPrefixTimeline=prefix;
    candidate.pendingRegisters=std::move(registers);candidate.pendingState=state;
    candidate.nativeFences=4;candidate.leadingSetupFences=1;
    output=std::move(candidate);return true; // Caller chooses diagnostic OR scoped publisher.
}
bool VulkanEngine::executeExperimentalPostOutputColorBatch(const NativeColorChainProgram& program,
        const NativeColorBatchCapture& capture,const NativeColorPublisher& publisher) {
    if(!publisher.context || !publisher.publish)return false;
    NativeColorChainExecutionResult completed;
    if(!executeAuditedPostOutputColor(program,capture,completed))return false;
    if(!completed.pendingRegisters || !completed.pendingState)return false;
    return publishAndCommitNativeColorState(capture,completed,*completed.pendingRegisters,*completed.pendingState,publisher,
        initializationRegisterJournal_,stateBatchSnapshot_,colorEdram_,NativeColorEdramCommit::ReplaceFullKnownRegion);
}
bool VulkanEngine::executeAuditedPostOutputColor(const NativeColorChainProgram& program,
        const NativeColorBatchCapture& capture,NativeColorChainExecutionResult& output) {
    if(!ready() || !nativeOutputCompleted_ || !nativePostOutputStorageCompleted_ ||
        !nativeDisplayModeSelected_ || !nativeScalerConfigured() || !nativeGammaRamp_ ||
        !experimentalColorAttempted_ || !warmRingInitialized_ || ringRestartPending_ ||
        stoppedColorDiagnosticAttempted_ || postOutputColorExecutionAttempted_ ||
        program.coordinates()!=NativeColorCoordinateProfile::Padded1280W1 ||
        capture.color().coordinates()!=program.coordinates() || !program.matches(capture.color().active()) ||
        capture.controls().rootWords[2]!=capture.commands().commands.words().size() ||
        !validNativeColorEdramTransition(capture,stateBatchSnapshot_,colorEdram_,NativeColorEdramCommit::ReplaceFullKnownRegion))return false;
    NativeRegisterJournal registers;NativeGraphicsState state;
    if(!prepareNativeColorState(capture,initializationRegisterJournal_,stateBatchSnapshot_,registers,state))return false;
    postOutputColorExecutionAttempted_=true;
    // Leading WAIT_UNTIL followed by the same RB_BC_CONTROL legacy native
    // setup contract as initialization. No persistent GPU guest cache survives;
    // fresh bindings use driver-managed storage. Order ALL prior native work
    // through a newly submitted fence before initializing the fresh resources.
    if(!fenceNativeQueue())return false;
    NativeColorChainExecutionResult candidate;
    if(!executeCapturedNativeColorChain(context_,program,capture.color(),capture.copy().copy.destinationBase,
        capture.events().staging,candidate,true))return false;
    candidate.leadingSetupFences=1;candidate.pendingRegisters=std::move(registers);candidate.pendingState=state;
    output=std::move(candidate);return true;
}

bool VulkanEngine::drainForRingReconfiguration() {
    if (!ready() || !constantInitializationCompleted_ || !stateBatchSnapshot_.batches || ringRestartPending_ || warmRingInitialized_) return false;
    if (recomp::gpu::lockedQueueWaitIdle(context_.device,context_.queue)!=VK_SUCCESS) { queueVerified_=false; return false; }
    ringRestartPending_=true; return true;
}

bool VulkanEngine::completeWarmRingInitialization(const std::vector<std::uint32_t>& root,
        const std::vector<std::uint32_t>& child) {
    recomp::bootstrap::NativeInitialCoherency request;
    if (!ringRestartPending_ || warmRingInitialized_ || stateBatchSnapshot_.invalidationEpoch==UINT64_MAX ||
        !recomp::bootstrap::parseNativeInitialCoherency(root,child,request) || !fenceEmptyGuestCache(request)) return false;
    // Rebind the validated native ME configuration, not Xenos microcode. Raw
    // bin/register state survives; derived command-state caches get a new epoch.
    std::copy_n(root.begin(),19,warmMeInitProfile_.begin());
    ++stateBatchSnapshot_.invalidationEpoch;
    ringRestartPending_=false; warmRingInitialized_=true; return true;
}

bool VulkanEngine::completeNativeStateBatch(const std::vector<std::uint32_t>& words,
        NativeInitializationReadback& output) {
    return completeStateBatch(words,output,false);
}
bool VulkanEngine::completeExperimentalIdleCachePair(const std::vector<std::uint32_t>& words,
        NativeInitializationReadback& output) {
    return completeStateBatch(words,output,true);
}
bool VulkanEngine::completeStateBatch(const std::vector<std::uint32_t>& words,
        NativeInitializationReadback& output, bool experimentalIdleCachePair) {
    // The idle-cache-pair pattern alone is allowed to run after real color
    // output has started: planNativeStateBatch only recognizes it as the exact
    // six-word {WAIT_UNTIL=0x20000, 0xE00=1, 0xE40=1} sequence, it performs
    // zero resident-texture word updates, and its only durable effect here is
    // bumping experimentalIdleCachePairEpoch. publishAndCommitNativeColorState
    // (native_color_state_commit.h) and the deferred variant already compare
    // that epoch against the value captured when a color batch was prepared
    // and safely reject the commit on a mismatch, so this is a real staleness
    // signal, not fabricated completion. Every other state batch remains
    // blocked post-color; this does not claim general resident-texture cache
    // invalidation is implemented.
    if (!ready() || !displayConfiguration_ || !constantInitializationCompleted_ || ringRestartPending_ ||
        (experimentalColorAttempted_ && !experimentalIdleCachePair) ||
        stateBatchSnapshot_.batches==UINT32_MAX) return false;
    NativeStateBatch plan; Pm4Error error;
    if (!planNativeStateBatch(words,plan,error,experimentalIdleCachePair) ||
        plan.experimentalIdleCachePair!=experimentalIdleCachePair ||
        (experimentalIdleCachePair && stateBatchSnapshot_.experimentalIdleCachePairEpoch==UINT64_MAX) ||
        UINT64_MAX-stateBatchSnapshot_.invalidationEpoch<plan.invalidations) return false;
    // Prepare all potentially allocating state before recording any GPU work.
    auto journal=initializationRegisterJournal_;
    for (const auto& item : plan.registers) journal[item.first]={item.second,0xFFFFFFFF};
    auto state=stateBatchSnapshot_;
    for (unsigned i=0;i<4;++i) if (plan.bins[i]) {
        auto& target=i<2?state.binMask:state.binSelect;
        const auto shift=(i&1)*32;
        target=(target&~(std::uint64_t{0xFFFFFFFF}<<shift)) | (std::uint64_t{*plan.bins[i]}<<shift);
        state.knownBinWords|=1u<<i;
    }
    // No derived shader/pipeline pointers survive initialization's temporary
    // resources. Retain an epoch so future persistent caches MUST invalidate
    // their derived entries on this command; raw guest register bits survive.
    state.invalidationEpoch+=plan.invalidations; ++state.batches;
    // This backend owns no resident guest textures/vertices (initialization's
    // temporary resources have been destroyed). Future residency support MUST
    // reject this experimental path until actual cache invalidation is wired.
    // A separate epoch records this hypothesis; raw register readback/status
    // is deliberately not fabricated. Publish only after the real fence/audit.
    if (experimentalIdleCachePair) ++state.experimentalIdleCachePairEpoch;
    std::array<std::uint8_t,4096> expected{}; expected.fill(0x5A);
    for (std::size_t i=0;i<plan.updates.count;++i) {
        const auto& w=plan.updates.words[i];
        std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),expected.begin()+w.byteOffset);
    }
    VkCommandBufferAllocateInfo allocate{}; allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool=commands_; allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(context_.device,&allocate,&command)!=VK_SUCCESS) return false;
    const auto release=[&] { vkFreeCommandBuffers(context_.device,commands_,1,&command); };
    VkCommandBufferBeginInfo begin{}; begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS) { release(); return false; }
    const auto barrier=[&](VkPipelineStageFlags from,VkPipelineStageFlags to,VkAccessFlags src,VkAccessFlags dst) {
        VkMemoryBarrier b{}; b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER; b.srcAccessMask=src; b.dstAccessMask=dst;
        vkCmdPipelineBarrier(command,from,to,0,1,&b,0,nullptr,0,nullptr);
    };
    // The private bootstrap staging buffer is never guest resource residency.
    // All earlier backend commands completed synchronously before this call.
    std::memset(mapped_,0x5A,4096);
    barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    for (std::size_t i=0;i<plan.updates.count;++i) {
        const auto& w=plan.updates.words[i]; alignas(4) const auto bytes=w.bigEndianBytes;
        vkCmdUpdateBuffer(command,bootstrapBuffer_,w.byteOffset,4,bytes.data());
    }
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    if (vkEndCommandBuffer(command)!=VK_SUCCESS || vkResetFences(context_.device,1,&fence_)!=VK_SUCCESS) {
        release(); return false;
    }
    VkSubmitInfo submit{}; submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
    if (recomp::gpu::lockedQueueSubmit(context_.queue,1,&submit,fence_)!=VK_SUCCESS ||
        vkWaitForFences(context_.device,1,&fence_,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS) {
        std::fflush(stdout); std::_Exit(4);
    }
    release();
    AuditedNativeWords audited; NativeWordReadbackFailure auditError;
    if (std::memcmp(mapped_,expected.data(),4096)!=0 ||
        !auditNativeWordReadback(plan.updates,static_cast<const std::uint8_t*>(mapped_),plan.updates.bufferBytes,audited,auditError)) {
        queueVerified_=false; return false;
    }
    NativeInitializationReadback candidate;
    candidate.byteCount=plan.updates.bufferBytes;
    std::memcpy(candidate.bytes.data(),mapped_,candidate.byteCount);
    initializationRegisterJournal_.swap(journal); stateBatchSnapshot_=state;
    output=candidate; return true;
}

bool VulkanEngine::executeExperimentalMaintenancePoints(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
        const std::function<bool(const NativeInitializationReadback&)>& publish) {
    if(!ready() || !displayConfiguration_ || !constantInitializationCompleted_ || ringRestartPending_ || !publish ||
       stateBatchSnapshot_.batches==UINT32_MAX || stateBatchSnapshot_.experimentalIdleCachePairEpoch==UINT64_MAX)return false;
    MaintenancePointPlan plan;MaintenancePointError error;
    if(!planMaintenancePoints(root,graph,plan,error,true) ||
       UINT64_MAX-stateBatchSnapshot_.invalidationEpoch<plan.invalidations)return false;
    // Prepare all allocating state before GPU work/publication. INVALIDATE_STATE
    // does not erase the loaded microcode or already-rendered guest images.
    auto journal=initializationRegisterJournal_;
    for(const auto& [reg,value]:plan.registers)journal[reg]={value,UINT32_MAX};
    auto programs=plan.finalPrograms;
    auto state=stateBatchSnapshot_;++state.batches;++state.experimentalIdleCachePairEpoch;
    state.invalidationEpoch+=plan.invalidations;
    NativeMaintenancePointResult executed;
    if(!executeNativeMaintenancePoints(context_,root,graph,executed) ||
       executed.readback.draws!=plan.points.size() || executed.readback.byteCount!=plan.updates.bufferBytes)return false;
    if(!publish(executed.readback))return false;
    // All previous backend work is synchronous and the new fence completed.
    // Evict actual derived position pipelines/buffers; preserve guest target
    // images and raw API driver state. Texture uploads are per-draw temporaries,
    // not retained guest-memory caches. Epoch changes reject old deferred plans.
    clippedFan_.reset();gammaFanPositions_.reset();gammaQuadPositions_.reset();logicalQueryPositions_.reset();
    cubeRectanglePositions_.reset();lodRectanglePositions_.reset();
    initializationRegisterJournal_.swap(journal);retainedMaintenancePrograms_.swap(programs);
    stateBatchSnapshot_=state;return true;
}

bool VulkanEngine::executeRuntimeMaintenancePoints(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
        const NativeLogicalTileTarget& target,const std::function<bool(const NativeInitializationReadback&)>& publish) {
    if(tileInterruptActive_){tileInterruptFailed_=true;return false;}
    if(!ready() || !displayConfiguration_ || !constantInitializationCompleted_ || !warmRingInitialized_ ||
       ringRestartPending_ || !nativeLogicalGeometryEnabled_ || tileInterruptFailed_ || !visibilityQueries_.idle() ||
       !publish || stateBatchSnapshot_.batches==UINT32_MAX ||
       stateBatchSnapshot_.experimentalIdleCachePairEpoch==UINT64_MAX)return false;
    MaintenancePointPlan plan;MaintenancePointError error;
    if(!planRuntimeMaintenancePoints(root,graph,plan,error) || !plan.runtimeTail ||
       UINT64_MAX-stateBatchSnapshot_.invalidationEpoch<plan.invalidations ||
       !target.invalidateDerivedState(context_,0x7FFF))return false;
    // The live logical target destroys successful per-draw bindings before
    // returning. Audit same-owner/no-failed-inflight state instead of assuming
    // that an epoch increment alone invalidates a resident native target.
    auto journal=initializationRegisterJournal_;
    for(const auto&[reg,value]:plan.registers)journal[reg]={value,UINT32_MAX};
    auto programs=plan.finalPrograms;auto state=stateBatchSnapshot_;
    ++state.batches;++state.experimentalIdleCachePairEpoch;state.invalidationEpoch+=plan.invalidations;
    tileInterruptActive_=true;
    const auto finish=[&](bool ok){tileInterruptActive_=false;tileInterruptFailed_|=!ok;return ok && !tileInterruptFailed_;};
    try {
        NativeMaintenancePointResult executed;
        if(!executeNativeMaintenancePoints(context_,root,graph,executed,MaintenancePointProfile::RuntimeProgramUploads) ||
           executed.readback.draws!=plan.points.size() || executed.readback.byteCount!=plan.updates.bufferBytes ||
           !target.invalidateDerivedState(context_,0x7FFF) || tileInterruptFailed_)return finish(false);
        // Actual24 native masked points, all101 ordered dependencies and both
        // private event words have completed and passed the GPU canary audit.
        // Evict every retained derived position executor; keep guest images,
        // shader microcode and constants. No CPU completion is invented here.
        clippedFan_.reset();gammaFanPositions_.reset();gammaQuadPositions_.reset();logicalQueryPositions_.reset();
        cubeRectanglePositions_.reset();lodRectanglePositions_.reset();
        if(!publish(executed.readback) || tileInterruptFailed_)return finish(false);
        // Caller holds the exclusive producer/consumer gate throughout. These
        // no-throw swaps follow successful atomic guest publication before any
        // guest writer can resume. A failed publisher commits no raw state.
        initializationRegisterJournal_.swap(journal);retainedMaintenancePrograms_.swap(programs);
        stateBatchSnapshot_=state;return finish(true);
    } catch(...) {finish(false);throw;}
}

bool VulkanEngine::completeExperimentalConstantInitialization(const StoppedConstantInitialization& plan,
        NativeInitializationReadback& output) {
    using namespace constant_points;
    if (!ready() || !displayConfiguration_ || !initialCoherencyCompleted_ || constantInitializationAttempted_ ||
        plan.draws.empty() || plan.draws.size() > 64 || !plan.memory.updates.bufferBytes ||
        plan.memory.updates.bufferBytes > 256) return false;
    std::array<NativeConstantPointInputs,64> inputs;
    for (std::size_t i = 0; i < plan.draws.size(); ++i)
        if (!packNativeConstantPointInputs(plan.draws[i].pair,inputs[i])) return false;
    // Validate the mutable native word plan again before any GPU commands.
    std::array<std::uint8_t,256> expected{}; expected.fill(0x5A);
    if (plan.memory.updates.count > NativeWordUpdatePlan::Limit) return false;
    for (std::size_t i = 0; i < plan.memory.updates.count; ++i) {
        const auto& w = plan.memory.updates.words[i];
        if (std::uint64_t{w.byteOffset}+4 > plan.memory.updates.bufferBytes) return false;
        std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),expected.begin()+w.byteOffset);
    }
    AuditedNativeWords audited; NativeWordReadbackFailure auditError;
    if (!auditNativeWordReadback(plan.memory.updates,expected.data(),plan.memory.updates.bufferBytes,audited,auditError)) return false;
    std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>> journal;
    for (const auto& item : plan.registers) journal.emplace(item.first,std::make_pair(item.second.value,item.second.known));
    constantInitializationAttempted_ = true;
    Resources r(context_.device,256);
    if (!setup(context_,r,genericConstantShaderCode(),true) || !pipeline(r,0)) return false;
    std::memset(r.mapped,0x5A,Bytes+256);
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(r.command,&begin) != VK_SUCCESS) return false;
    const auto barrier = [&](VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags src, VkAccessFlags dst) {
        VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; b.srcAccessMask = src; b.dstAccessMask = dst;
        vkCmdPipelineBarrier(r.command,from,to,0,1,&b,0,nullptr,0,nullptr);
    };
    const auto updates = [&](NativeWordPhase phase) {
        for (std::size_t i = 0; i < plan.memory.updates.count; ++i) {
            const auto& w = plan.memory.updates.words[i];
            if (w.phase != phase) continue;
            alignas(4) const auto bytes = w.bigEndianBytes;
            vkCmdUpdateBuffer(r.command,r.buffer,Bytes+w.byteOffset,4,bytes.data());
        }
    };
    barrier(VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    updates(NativeWordPhase::BeforeGraphics);
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    // This initialization profile disables color/depth/query side effects and
    // proves no shader memory reads/writes. Use an internal throwaway target;
    // no guest EDRAM/color surface is cleared or marked resident.
    VkClearValue clear{}; for (auto& c : clear.color.float32) c = 1;
    VkRenderPassBeginInfo pass{}; pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = r.pass; pass.framebuffer = r.framebuffer; pass.renderArea = {{0,0},{Side,Side}};
    pass.clearValueCount = 1; pass.pClearValues = &clear;
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipeline);
    for (std::size_t i = 0; i < plan.draws.size(); ++i) {
        vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(inputs[i]),&inputs[i]);
        vkCmdDraw(r.command,1,1,0,0);
    }
    vkCmdEndRenderPass(r.command);
    barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    updates(NativeWordPhase::AfterGraphics);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {Side,Side,1};
    vkCmdCopyImageToBuffer(r.command,r.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffer,1,&copy);
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    if (vkEndCommandBuffer(r.command) != VK_SUCCESS) return false;
    if (vkGetFenceStatus(r.device,r.fence) != VK_NOT_READY) return false;
    for (std::size_t i = 0; i < Bytes+256; ++i)
        if (static_cast<const std::uint8_t*>(r.mapped)[i] != 0x5A) return false;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &r.command;
    if (recomp::gpu::lockedQueueSubmit(context_.queue,1,&submit,r.fence) != VK_SUCCESS ||
        vkWaitForFences(r.device,1,&r.fence,VK_TRUE,5'000'000'000ULL) != VK_SUCCESS) {
        std::fflush(stdout); std::_Exit(4); // Never release possibly in-flight work.
    }
    const auto* bytes = static_cast<const std::uint8_t*>(r.mapped);
    for (std::size_t i = 0; i < Bytes; ++i) if (bytes[i] != 255) return false;
    if (std::memcmp(bytes+Bytes,expected.data(),256) != 0 ||
        !auditNativeWordReadback(plan.memory.updates,bytes+Bytes,plan.memory.updates.bufferBytes,audited,auditError)) return false;
    NativeInitializationReadback candidate;
    std::copy_n(bytes+Bytes,plan.memory.updates.bufferBytes,candidate.bytes.begin());
    candidate.byteCount = plan.memory.updates.bufferBytes;
    candidate.draws = static_cast<std::uint32_t>(plan.draws.size());
    initializationRegisterJournal_ = std::move(journal);
    output = candidate; constantInitializationCompleted_ = true; return true;
}

VulkanEngine::EdramTrainingPolicy VulkanEngine::queryEdramTrainingPolicy() {
    if (!ready() || !displayConfiguration_ || !initialCoherencyCompleted_ || !constantInitializationCompleted_)
        return EdramTrainingPolicy::Unavailable;
    // Vulkan owns physical memory/link management. There is no Xenos HSIO bus
    // for guest PM4 training commands to calibrate. Check the live driver queue
    // before reporting this native capability; do not create dummy EDRAM work.
    // This says nothing about unimplemented guest render targets or resolves.
    if (recomp::gpu::lockedQueueWaitIdle(context_.device,context_.queue) != VK_SUCCESS) {
        queueVerified_ = false;
        return EdramTrainingPolicy::Unavailable;
    }
    return EdramTrainingPolicy::DriverManagedNoXenosLink;
}

bool VulkanEngine::queryDisplayGamma(std::uint32_t& type, std::uint32_t& powerBits) const noexcept {
    if (!ready() || !displayConfiguration_) return false;
    // Explicit native SDR sRGB output profile. The power parameter is neutral
    // and unused for type 1; only type 3 selects a custom power curve.
    type = 1; powerBits = 0x3F800000;
    return true;
}

bool VulkanEngine::selectNativeDisplayMode(std::uint32_t flags) noexcept {
    std::array<std::uint8_t,88> information{};
    if(flags!=0x40000000 || !ready() || !displayConfiguration_ || nativeDisplayModeSelected_ ||
        !recomp::bootstrap::makeNativeDisplayInformation(*displayConfiguration_,information))return false;
    // This exact RGBA8 extent was capability-checked by configureDisplay and
    // is the native owned-window consumer's logical input. No physical resize.
    nativeDisplayModeSelected_=true;return true;
}
bool VulkanEngine::querySelectedDisplayConfiguration(recomp::bootstrap::NativeDisplayConfiguration& output) const noexcept {
    if(!ready() || !nativeDisplayModeSelected_ || !displayConfiguration_)return false;
    output=*displayConfiguration_;return true;
}
bool VulkanEngine::configureDisplay(const recomp::bootstrap::NativeDisplayConfiguration& configuration) {
    if (!ready() || !recomp::bootstrap::validNativeDisplayConfiguration(configuration)) return false;
    // Plan a native SDR RGBA8 headless extent against actual device capabilities.
    // A notification does not create, resize, draw or present an image.
    VkImageFormatProperties supported{};
    if (vkGetPhysicalDeviceImageFormatProperties(context_.physical, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            0, &supported) != VK_SUCCESS ||
        supported.maxExtent.width < configuration.dimensions[2] ||
        supported.maxExtent.height < configuration.dimensions[3]) return false;
    displayConfiguration_ = configuration;
    nativeDisplayModeSelected_=false;
    return true;
}

bool VulkanEngine::completeInitialEmptyCacheCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if (!ready() || !displayConfiguration_ || initialCoherencyAttempted_ ||
        !recomp::bootstrap::validNativeInitialCoherency(request)) return false;
    // This backend has no resident guest textures/vertices or guest memory
    // mirror yet: the first invalidation's affected resource set is empty.
    // Retire this cold-only path when introducing resource residency. A fenced
    // Vulkan ordering operation completes before the command processor may
    // acknowledge the wait. No range is uploaded, drawn or marked rendered.
    initialCoherencyAttempted_ = true;
    if (!fenceEmptyGuestCache(request)) return false;
    initialCoherencyCompleted_ = true; return true;
}

bool VulkanEngine::completeEmptyGuestCacheCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if (!initialCoherencyCompleted_ || !constantInitializationCompleted_ || ringRestartPending_) return false;
    // This engine currently has no guest image/texture/vertex residency API.
    // Initialization's temporary point resources were fenced and destroyed.
    // Retire/integrate this empty-set path BEFORE adding any guest residency.
    return fenceEmptyGuestCache(request);
}

bool VulkanEngine::fenceEmptyGuestCache(const recomp::bootstrap::NativeInitialCoherency& request) {
    if(experimentalColorAttempted_)return false; // Preserve the old empty-set admission guard.
    return fenceNativeOrdering(request);
}
bool VulkanEngine::completeNativeCommandStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if(!experimentalColorAttempted_ || !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ ||
        !nativeDisplayModeSelected_ || commandStorageCoherencyCompleted_ || request.cacheFlags!=0x01000000)return false;
    // ConservativeOwnedResourcesV1 has fenced/destroyed all temporary draw
    // bindings. Only owned CPU EDRAM/raw register snapshots survive; commands
    // and shaders are re-read from synchronized CPU snapshots, not a retained
    // GPU command/texture cache. Registry validates the exact pinned command
    // allocation. Future persistent GPU guest bindings MUST revise this path.
    if(!fenceNativeOrdering(request))return false;
    commandStorageCoherencyCompleted_=true;return true;
}
bool VulkanEngine::completeNativeScalerStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if (!experimentalColorAttempted_ || !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ ||
        !nativeDisplayModeSelected_ || !nativeSwapWrapperCompleted_ || nativeScalerStorageCompleted_ ||
        request.cacheFlags!=0x01000000) return false;
    // Previous native commands completed synchronously; only CPU EDRAM/raw
    // state survives. This is a NEW ordering operation, not the old swap fence.
    if (!fenceNativeOrdering(request)) return false;
    nativeScalerStorageCompleted_=true;return true;
}
bool VulkanEngine::completeNativePostOutputSwapStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if(!ready() || !displayConfiguration_ || !nativeOutputCompleted_ || !nativePostOutputStorageCompleted_ ||
        !postOutputColorExecutionAttempted_ || stateBatchSnapshot_.experimentalColorBatches!=2 ||
        !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ || !nativeDisplayModeSelected_ ||
        !nativeScalerConfigured() || !nativeGammaRamp_ || !nativeSwapWrapperCompleted_ ||
        postOutputSwapStorageAttempted_ || request.cacheFlags!=0x01000000 ||
        !recomp::bootstrap::validNativeInitialCoherency(request))return false;
    postOutputSwapStorageAttempted_=true;
    if(!fenceNativeOrdering(request))return false;
    postOutputSwapStorageCompleted_=true;return true;
}
bool VulkanEngine::completeNativeDeferredSwapStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if(!ready() || !displayConfiguration_ || !postOutputSwapCompleted_ || !nativeOutputCompleted_ ||
        !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ || !nativeDisplayModeSelected_ ||
        !nativeScalerConfigured() || !nativeGammaRamp_ || stateBatchSnapshot_.experimentalColorBatches!=2 ||
        deferredSwapStorageAttempted_ || request.cacheFlags!=0x01000000 ||
        !recomp::bootstrap::validNativeInitialCoherency(request))return false;
    // Only synchronized CPU command/EDRAM snapshots survive the previous
    // completed work. A NEW native queue fence is required, never an old
    // receipt. Persistent guest GPU residency would require revising this policy.
    deferredSwapStorageAttempted_=true;
    if(!fenceNativeOrdering(request))return false;
    deferredSwapStorageCompleted_=true;return true;
}
bool VulkanEngine::fenceNativeOrdering(const recomp::bootstrap::NativeInitialCoherency& request) {
    if (!ready() || !displayConfiguration_ || !recomp::bootstrap::validNativeInitialCoherency(request)) return false;
    return fenceNativeQueue();
}
bool VulkanEngine::completeNativeRuntimeCommandStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
       ringRestartPending_ || !visibilityQueries_.idle() || request.cacheFlags!=0x01000000 ||
       !recomp::bootstrap::validNativeInitialCoherency(request))return false;
    // Registry authority is restricted to the pinned CPU command allocation,
    // not arbitrary textures or render targets. Native draw inputs are copied
    // from current CPU snapshots and their temporary bindings are destroyed
    // after a fence. No persistent GPU alias of command memory is retained.
    // Existing GPU targets stay intact; this is fresh ordering, not a clear,
    // draw, cached completion, or permission to skip command-buffer contents.
    return fenceNativeOrdering(request);
}
bool VulkanEngine::completeNativePostScalerStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    if (!nativeScalerConfigured() || !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ ||
        !nativeDisplayModeSelected_ || request.cacheFlags!=0x01000000) return false;
    // Same conservative owned-resource policy: retained EDRAM/registers are
    // CPU snapshots, no persistent GPU guest-resource cache. Registry proves
    // the exact command allocation. Each request submits a NEW native fence;
    // neither the scaler receipt nor an earlier coherency is reused.
    return fenceNativeOrdering(request);
}
bool VulkanEngine::completeNativePostDisplayStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    // Gamma presentation/events completed; order this NEW command-memory
    // request through a fresh fence under the same owned-resource policy.
    return nativeGammaRamp_.has_value() && completeNativePostScalerStorageCoherency(request);
}
bool VulkanEngine::completeNativePostOutputStorageCoherency(const recomp::bootstrap::NativeInitialCoherency& request) {
    // A completed output transaction, not merely an attempted presentation.
    // Each command-memory request gets a fresh ordering fence; no new image.
    if(!nativeOutputCompleted_ || !completeNativePostDisplayStorageCoherency(request))return false;
    nativePostOutputStorageCompleted_=true;return true;
}
bool VulkanEngine::fenceNativeQueue() {
    if (!ready() || !displayConfiguration_) return false;
    // v170 (H2b, diag-native-fence-only): no native submission since the last completed native fence: nothing native
    // is left to order, so the WAIT_UNTIL/fence is satisfied without waiting for the generic renderer's frame.
    static const bool fenceOnly = ::access(RECOMP_APP_PATH("/private-trace/diag-native-fence-only"), F_OK) == 0;
    if (recomp::gpu::nativeFenceUnneeded(fenceOnly)) return true;
    const auto ticketsBefore = recomp::gpu::nativeSubmissionTickets().load(std::memory_order_acquire); // v170
    nativeQueueFences.fetch_add(1, std::memory_order_relaxed); // v78
    const auto fenceStarted = std::chrono::steady_clock::now(); // v110
    const bool fenceStopped = recomp::bootstrap::guestExclusiveScopeActive && recomp::bootstrap::guestExclusiveScopeActive();
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = commands_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(context_.device, &allocate, &command) != VK_SUCCESS) return false;
    const auto release = [&] { vkFreeCommandBuffers(context_.device, commands_, 1, &command); };
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) { release(); return false; }
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(command) != VK_SUCCESS || vkResetFences(context_.device, 1, &fence_) != VK_SUCCESS) {
        release(); return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (recomp::gpu::lockedQueueSubmit(context_.queue, 1, &submit, fence_) != VK_SUCCESS ||
        vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, 5'000'000'000ULL) != VK_SUCCESS) {
        // No acknowledgment and no destruction of potentially in-flight work.
        std::fflush(stdout); std::_Exit(4);
    }
    release();
    recomp::gpu::noteNativeTicketsFenced(ticketsBefore + 1); // v170: this fence's own submission was ticket before+1 at best
    noteNativeFence(tileInterruptActive_, fenceStopped, fenceStarted); // v110
    return true;
}

bool VulkanEngine::queryNativeScratchWriteback(std::uint32_t& base,std::uint32_t& mask) const noexcept {
    if(!ready())return false;
    const auto retainedBase=retainedRegister(0x1DD),retainedMask=retainedRegister(0x1DC);
    if(!retainedBase || !retainedMask)return false;
    base=*retainedBase;mask=*retainedMask;return true;
}
bool VulkanEngine::executeTileInterrupt(NativeTileCommandInterrupt execution,
        const NativeTileCommandInterrupt::Memory& memory,const NativeTileCommandInterrupt::Guest& guest) {
    if(tileInterruptActive_){tileInterruptFailed_=true;return false;}
    unsigned base=0,mask=0;
    if(tileInterruptFailed_ || visibilityQueries_.failed() || !ready() || !nativeLogicalGeometryEnabled_ || ringRestartPending_ ||
       !queryNativeScratchWriteback(base,mask))return false;
    tileInterruptActive_=true;
    const auto finish=[&](bool ok){tileInterruptActive_=false;tileInterruptFailed_|=!ok;return ok && !tileInterruptFailed_;};
    try {
        const NativeTileCommandInterrupt::Memory action=memory?NativeTileCommandInterrupt::Memory(
            [&](const auto& op){return !tileInterruptFailed_ && memory(op) && !tileInterruptFailed_;}):NativeTileCommandInterrupt::Memory{};
        const NativeTileCommandInterrupt::Guest handoff=guest?NativeTileCommandInterrupt::Guest(
            [&](const auto& plan){return !tileInterruptFailed_ && guest(plan) && !tileInterruptFailed_;}):NativeTileCommandInterrupt::Guest{};
        return finish(execution.execute(base,mask,stateBatchSnapshot_,initializationRegisterJournal_,
            [&]{return !tileInterruptFailed_ && fenceNativeQueue();},action,handoff));
    } catch(...) {finish(false);throw;}
}
bool VulkanEngine::executeTileCommandInterrupt(const Gta5TileCommandInterrupt& plan,
        const NativeTileCommandInterrupt::Memory& memory,const NativeTileCommandInterrupt::Guest& guest) {
    return executeTileInterrupt(NativeTileCommandInterrupt(plan),memory,guest);
}
bool VulkanEngine::publishTileExtentBounds(Gta5TileExtentPublication& plan,
        const Gta5TileExtentPublication::Read& read,const Gta5TileExtentPublication::Publish& publish) {
    if(tileInterruptActive_){tileInterruptFailed_=true;return false;}
    if(tileInterruptFailed_ || !visibilityQueries_.idle() || !ready() ||
       !nativeLogicalGeometryEnabled_ || ringRestartPending_ || !read || !publish)return false;
    tileInterruptActive_=true;
    const auto finish=[&](bool ok){tileInterruptActive_=false;tileInterruptFailed_|=!ok;return ok && !tileInterruptFailed_;};
    try {
        return finish(plan.publish([&](auto address,auto bytes)->std::optional<std::vector<std::uint8_t>> {
            if(tileInterruptFailed_)return std::nullopt;
            auto result=read(address,bytes);return tileInterruptFailed_?std::nullopt:std::move(result);
        },[&]{return !tileInterruptFailed_ && fenceNativeQueue();},[&](auto address,const auto& expected,const auto& completed) {
            return !tileInterruptFailed_ && publish(address,expected,completed) && !tileInterruptFailed_;
        }));
    } catch(...) {finish(false);throw;}
}
bool VulkanEngine::executeTileSceneContinuation(const Gta5TileSceneContinuation& tail,
        const NativeTileCommandInterrupt::Memory& memory,const NativeTileCommandInterrupt::Guest& guest) {
    return executeTileInterrupt(NativeTileCommandInterrupt(tail),memory,guest);
}
bool VulkanEngine::executeTileDeferredTail(const Gta5TileDeferredTail& plan,NativeLogicalTileTarget& target,
        const NativeTileDeferredTail::Publish& publish) {
    if(tileInterruptActive_){tileInterruptFailed_=true;return false;}
    if(tileInterruptFailed_ || !visibilityQueries_.idle() || !ready() || !nativeLogicalGeometryEnabled_ || ringRestartPending_ || !publish)return false;
    tileInterruptActive_=true;
    const auto finish=[&](bool ok){tileInterruptActive_=false;tileInterruptFailed_|=!ok;return ok && !tileInterruptFailed_;};
    try {
        NativeTileDeferredTail execution(plan);
        return finish(execution.execute(stateBatchSnapshot_,initializationRegisterJournal_,
            [&]{return !tileInterruptFailed_ && fenceNativeQueue();},[&](auto mask) {
                if(tileInterruptFailed_ || !target.invalidateDerivedState(context_,mask))return false;
                // These are derived shader executors. Native image contents and
                // raw guest constants/registers are deliberately preserved.
                clippedFan_.reset();gammaFanPositions_.reset();gammaQuadPositions_.reset();logicalQueryPositions_.reset();
                cubeRectanglePositions_.reset();lodRectanglePositions_.reset();
                return true;
            },[&](const auto& updates){return !tileInterruptFailed_ && publish(updates) && !tileInterruptFailed_;}));
    } catch(...) {finish(false);throw;}
}
bool VulkanEngine::executeLogicalStateInvalidation(const NativeTileStateInvalidation& plan,NativeLogicalTileTarget& target,unsigned* evictedExecutors) {
    if(tileInterruptActive_){tileInterruptFailed_=true;return false;}
    if(tileInterruptFailed_ || !visibilityQueries_.idle() || !ready() || !nativeLogicalGeometryEnabled_ || ringRestartPending_)return false;
    tileInterruptActive_=true;
    const auto finish=[&](bool ok){tileInterruptActive_=false;tileInterruptFailed_|=!ok;return ok && !tileInterruptFailed_;};
    try {
        auto execution=plan;unsigned evicted=0;
        const bool ok=finish(execution.execute(stateBatchSnapshot_,
            [&]{return !tileInterruptFailed_ && fenceNativeQueue();},[&] {
                // Full derived-state eviction is conservative for the original
                //0x100 subset. The target proves actual same-owner idle state;
                // do not replace this audit with an epoch-only acknowledgement.
                if(tileInterruptFailed_ || !target.invalidateDerivedState(context_,0x7FFF))return false;
                evicted=unsigned(bool(clippedFan_))+unsigned(bool(gammaFanPositions_))+unsigned(bool(gammaQuadPositions_))+
                    unsigned(bool(logicalQueryPositions_))+unsigned(bool(cubeRectanglePositions_))+unsigned(bool(lodRectanglePositions_));
                clippedFan_.reset();gammaFanPositions_.reset();gammaQuadPositions_.reset();logicalQueryPositions_.reset();
                cubeRectanglePositions_.reset();lodRectanglePositions_.reset();
                return true;
            }));
        if(ok && evictedExecutors)*evictedExecutors=evicted;
        return ok;
    } catch(...) {finish(false);throw;}
}
bool VulkanEngine::executeLogicalCacheBarrier(const NativeTileCacheBarrier& plan,NativeLogicalTileTarget& target,unsigned* evictedExecutors) {
    if(tileInterruptActive_){tileInterruptFailed_=true;return false;}
    if(tileInterruptFailed_ || !visibilityQueries_.idle() || !ready() || !nativeLogicalGeometryEnabled_ || ringRestartPending_ ||
       !target.invalidateDerivedState(context_,0x7FFF))return false;
    tileInterruptActive_=true;
    const auto finish=[&](bool ok){tileInterruptActive_=false;tileInterruptFailed_|=!ok;return ok && !tileInterruptFailed_;};
    try {
        auto execution=plan;unsigned evicted=0;
        const bool ok=finish(execution.execute(stateBatchSnapshot_,initializationRegisterJournal_,
            [&]{return !tileInterruptFailed_ && fenceNativeQueue();},[&] {
                // Logical draws upload fresh owned texture/vertex bytes and
                // destroy derived resources after their actual fence. Reject
                // any failed/in-flight target; do not discard native images.
                if(tileInterruptFailed_ || !target.invalidateDerivedState(context_,0x7FFF))return false;
                evicted=unsigned(bool(clippedFan_))+unsigned(bool(gammaFanPositions_))+unsigned(bool(gammaQuadPositions_))+
                    unsigned(bool(logicalQueryPositions_))+unsigned(bool(cubeRectanglePositions_))+unsigned(bool(lodRectanglePositions_));
                clippedFan_.reset();gammaFanPositions_.reset();gammaQuadPositions_.reset();logicalQueryPositions_.reset();
                cubeRectanglePositions_.reset();lodRectanglePositions_.reset();return true;
            }));
        if(ok && evictedExecutors)*evictedExecutors=evicted;
        return ok;
    } catch(...) {finish(false);throw;}
}
bool VulkanEngine::beginNativeScalerInterrupt(const NativeScalerWrapper& plan,const NativeScalerInterruptSequence::Action& memory) {
    if (!ready() || !colorEdram_ || !nativeSwapWrapperCompleted_ || !nativeScalerStorageCompleted_ ||
        !nativeDisplayModeSelected_ || ringRestartPending_ || nativeScalerInterrupt_ || !memory) return false;
    const auto base=retainedRegister(0x1DD),mask=retainedRegister(0x1DC);
    if (!base || !mask || !plan.scratchStateMatches(*base,*mask)) return false;
    // Allocate every journal slot before the first externally visible action.
    auto journal=initializationRegisterJournal_;
    for (auto reg:{0x578,0x579,0x57C,0x57D,0x5C8,0x45E}) journal.try_emplace(reg,std::make_pair(0u,0u));
    initializationRegisterJournal_=std::move(journal);
    nativeScalerInterrupt_.emplace(plan); // Single attempt, including partial failure.
    const auto action=[&](const NativeScalerWrapper::Operation& op) {
        using Kind=NativeScalerWrapper::Kind;
        switch (op.kind) {
        case Kind::BinMaskLow:
            stateBatchSnapshot_.binMask=(stateBatchSnapshot_.binMask&0xFFFFFFFF00000000ULL)|op.value;
            stateBatchSnapshot_.knownBinWords|=1;return true;
        case Kind::ScratchWrite:
            if (!memory(op)) return false;
            initializationRegisterJournal_.find(0x578+op.target)->second={op.value,0xFFFFFFFF};return true;
        case Kind::WaitScratch:return memory(op);
        case Kind::WaitIdle:
            if (!fenceNativeQueue()) return false;
            initializationRegisterJournal_.find(0x5C8)->second={op.value,0xFFFFFFFF};return true;
        default:return false;
        }
    };
    if (!nativeScalerInterrupt_->prefix(*base,*mask,action)) return false;
    return nativeScalerInterrupt_->enterGuest(); // Caller dispatches; not evidence it ran.
}
bool VulkanEngine::acknowledgeNativeScalerInterrupt(const NativeScalerInterruptSequence::Action& memory) {
    if (!ready() || !nativeScalerInterrupt_ || !memory ||
        nativeScalerInterrupt_->phase()!=NativeScalerInterruptSequence::Phase::GuestRunning) return false;
    const auto base=retainedRegister(0x1DD),mask=retainedRegister(0x1DC);
    if (!base || !mask || !nativeScalerInterrupt_->plan().scratchStateMatches(*base,*mask)) {
        nativeScalerInterrupt_->fail();return false;
    }
    if (!nativeScalerInterrupt_->guestReturned()) return false;
    return nativeScalerInterrupt_->acknowledge([&](const NativeScalerWrapper::Operation& op) {
        using Kind=NativeScalerWrapper::Kind;
        if (op.kind==Kind::CallbackAcknowledge) {
            initializationRegisterJournal_.find(op.target)->second={op.value,0xFFFFFFFF};return true;
        }
        if (op.kind==Kind::WaitScratch) return memory(op);
        if (op.kind==Kind::ScratchWrite && memory(op)) {
            initializationRegisterJournal_.find(0x578+op.target)->second={op.value,0xFFFFFFFF};return true;
        }
        return false;
    });
}

bool VulkanEngine::writeNativeDisplayEvents(const std::array<std::uint32_t,2>& values,std::array<std::uint8_t,8>& output) {
    // Private native staging, not guest residency. Two updates in packet order,
    // no intervening guest reads. Audit all4096 bytes, including untouched data.
    std::array<std::uint8_t,4096> expected{};expected.fill(0x5A);
    for (unsigned i=0;i<2;++i) for (unsigned b=0;b<4;++b)
        expected[16+i*4+b]=values[i]>>(24-b*8);
    VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool=commands_;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE;
    if(vkAllocateCommandBuffers(context_.device,&allocate,&command)!=VK_SUCCESS)return false;
    const auto release=[&]{vkFreeCommandBuffers(context_.device,commands_,1,&command);};
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS){release();return false;}
    std::memset(mapped_,0x5A,4096);
    VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,1,&barrier,0,nullptr,0,nullptr);
    for (unsigned i=0;i<2;++i) {
        alignas(4) std::array<std::uint8_t,4> bytes{};std::copy_n(expected.data()+16+i*4,4,bytes.data());
        vkCmdUpdateBuffer(command,bootstrapBuffer_,16+i*4,4,bytes.data());
    }
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    if(vkEndCommandBuffer(command)!=VK_SUCCESS || vkResetFences(context_.device,1,&fence_)!=VK_SUCCESS){release();return false;}
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    if(recomp::gpu::lockedQueueSubmit(context_.queue,1,&submit,fence_)!=VK_SUCCESS ||
        vkWaitForFences(context_.device,1,&fence_,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){std::fflush(stdout);std::_Exit(4);}
    release();
    if(std::memcmp(mapped_,expected.data(),4096)){queueVerified_=false;return false;}
    std::memcpy(output.data(),static_cast<const std::uint8_t*>(mapped_)+16,8);return true;
}
bool VulkanEngine::completeNativeScalerTail(NativeScalerTailResult& output) {
    if (!ready() || !nativeScalerInterrupt_ || nativeScalerInterrupt_->phase()!=NativeScalerInterruptSequence::Phase::Acknowledged ||
        nativeScalerDisplay_.nextOperation()!=15 || stateBatchSnapshot_.invalidationEpoch==UINT64_MAX) return false;
    const auto& plan=nativeScalerInterrupt_->plan();
    const auto base=retainedRegister(0x1DD),mask=retainedRegister(0x1DC);
    if (!base || !mask || !plan.scratchStateMatches(*base,*mask)) return false;
    auto journal=initializationRegisterJournal_;
    // Retain explicit writes; do NOT invent a hardware pending-bit readback.
    journal[0x1973]={0,0x10000};journal[0x1964]={1,0xFFFFFFFF};
    journal[0x1852]={0,0xFFFFFFFF};journal[0x1838]={1,0xFFFFFFFF};
    NativeScalerTailResult result;
    const NativeScalerDisplayState::Fence fence=[&]{return fenceNativeQueue();};
    const bool completed=nativeScalerInterrupt_->completeTail([&](const NativeScalerWrapper::Operation& op) {
        using Kind=NativeScalerWrapper::Kind;
        if (op.wordOffset<=69) return nativeScalerDisplay_.apply(op,plan.command(),fence);
        if (op.kind==Kind::InvalidateState) {++stateBatchSnapshot_.invalidationEpoch;return true;}
        if (op.kind==Kind::DiagnosticNop) return true;
        if (op.kind==Kind::LiteralEvent && op.wordOffset==137) return true; // Batched with the next adjacent event.
        if (op.kind==Kind::LiteralEvent && op.wordOffset==141) {
            result.configured=nativeScalerDisplay_.configured();result.nativeFences=nativeScalerDisplay_.nativeFences();
            if (!result.configured || !writeNativeDisplayEvents({plan.operations()[24].value,plan.operations()[25].value},result.eventBytes)) return false;
            ++result.nativeFences;return true;
        }
        return false;
    });
    if (!completed || !validNativeScalerTailResult(plan,result)) return false;
    initializationRegisterJournal_.swap(journal);output=result;return true;
}
bool VulkanEngine::executeNativeDisplayGamma(const NativeDisplayGammaWrapper& plan,
        const std::function<bool()>& present,NativeDisplayGammaResult& output) {
    if (!ready() || !nativeScalerConfigured() || !present || nativeGammaAttempted_ || !colorEdram_ ||
        !warmRingInitialized_ || ringRestartPending_ || stateBatchSnapshot_.invalidationEpoch==UINT64_MAX) return false;
    auto journal=initializationRegisterJournal_;
    // Native RGB8 selection only controls format bits; preserve unknown other
    // D1GRPH_CONTROL bits. No invented 1973 pending/readback bit.
    auto& format=journal[0x1841];format.first&=0xFFFFF8FF;format.second|=0x700;
    for (const auto& write:std::array<std::pair<unsigned,unsigned>,8>{{
            {0x1930,0},{0x1844,plan.frontbuffer()},{0x1838,0},{0x1852,1},
            {0x1921,0},{0x1922,256},{0x1927,7},{0x1925,plan.lastPackedSample()}}})
        journal[write.first]={write.second,0xFFFFFFFF};
    nativeGammaAttempted_=true;
    // The native unlock/wait orders earlier work before the logical display
    // change. Whole-table upload is atomic in this policy; no physical vblank.
    if (!fenceNativeQueue()) return false;
    if (!present()) return false; // Caller already owns and mapped the RGB8 image.
    NativeDisplayGammaResult result{true,1,{}};
    try {
        if (!writeNativeDisplayEvents({plan.eventValue(0),plan.eventValue(1)},result.eventBytes)) std::_Exit(4);
    } catch (...) {std::_Exit(4);} // Presentation accepted: no resumable failure.
    ++result.nativeFences;
    initializationRegisterJournal_.swap(journal);++stateBatchSnapshot_.invalidationEpoch;
    nativeGammaRamp_=plan.ramp();output=result;return true;
}

bool VulkanEngine::executeExperimentalNativeDisplayOutput(const NativeDisplayOutputWrapper& plan,
        const std::function<bool()>& present,NativeDisplayOutputResult& output) {
    if (!ready() || !nativeGammaRamp_ || !nativeScalerConfigured() || !present || nativeOutputAttempted_ ||
        !colorEdram_ || !warmRingInitialized_ || ringRestartPending_ || stateBatchSnapshot_.invalidationEpoch==UINT64_MAX) return false;
    auto journal=initializationRegisterJournal_;
    auto& control=journal[NativeDisplayOutputWrapper::Register];
    control=NativeDisplayOutputWrapper::clearForceBit(control);
    nativeOutputAttempted_=true;
    // Explicit compositor-owned normal-video policy: re-present the actual
    // current image with installed scaler/gamma, never manufacture force-data
    // pixels or claim an electrical XDVO transition/vblank. Separate opt-in.
    if (!fenceNativeQueue() || !present()) return false;
    NativeDisplayOutputResult result{true,1,{}};
    try {
        if (!writeNativeDisplayEvents({plan.eventValue(0),plan.eventValue(1)},result.eventBytes)) std::_Exit(4);
    } catch (...) {std::_Exit(4);}
    ++result.nativeFences;
    initializationRegisterJournal_.swap(journal);++stateBatchSnapshot_.invalidationEpoch;
    nativeOutputCompleted_=true;output=result;return true;
}

VulkanEngine::~VulkanEngine() {
    if (context_.device && ::recomp::apple::iosGpuLifecycle().snapshot().phase !=
            ::recomp::apple::IosGpuLifecycle::Phase::Disabled) {
        if (!::recomp::apple::iosGpuLifecycle().retireDevice(context_.device) ||
                vkDeviceWaitIdle(context_.device) != VK_SUCCESS) {
            std::fprintf(stderr,"APPLE_IOS_LIFECYCLE_TEARDOWN_FAILED owner=VulkanEngine\n");
            std::_Exit(4);
        }
    }
    if (mapped_) vkUnmapMemory(context_.device, bootstrapMemory_);
    if (fence_) vkDestroyFence(context_.device, fence_, nullptr);
    if (commands_) vkDestroyCommandPool(context_.device, commands_, nullptr);
    if (bootstrapBuffer_) vkDestroyBuffer(context_.device, bootstrapBuffer_, nullptr);
    if (bootstrapMemory_) vkFreeMemory(context_.device, bootstrapMemory_, nullptr);
}

bool VulkanEngine::initialize() {
    if (attempted_) return false;
    attempted_ = true;
    ShaderRequirements requirements;
    requirements.runtimeDescriptorArray = true;
    requirements.clipDistance = true;
    requirements.fragmentStoresAndAtomics = true;
    requirements.vertexStoresAndAtomics = nativeGeometryEnabled_;
    if (!context_.inspect() || context_.properties.limits.maxBoundDescriptorSets < 5 ||
        !context_.create(requirements,nativeGeometryEnabled_,nativeGeometryEnabled_,nativeLogicalGeometryEnabled_)) return false;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = context_.queueFamily;
    if (!checkVulkan(vkCreateCommandPool(context_.device, &pool, nullptr, &commands_), "engine-command-pool")) return false;
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = 4096;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!checkVulkan(vkCreateBuffer(context_.device, &buffer, nullptr, &bootstrapBuffer_), "engine-buffer")) return false;
    VkMemoryRequirements required{};
    vkGetBufferMemoryRequirements(context_.device, bootstrapBuffer_, &required);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(context_.physical, &memory);
    uint32_t memoryType = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((required.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            memoryType = i; break;
        }
    }
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = required.size;
    allocate.memoryTypeIndex = memoryType;
    if (!checkVulkan(vkAllocateMemory(context_.device, &allocate, nullptr, &bootstrapMemory_), "engine-memory") ||
        !checkVulkan(vkBindBufferMemory(context_.device, bootstrapBuffer_, bootstrapMemory_, 0), "engine-bind") ||
        !checkVulkan(vkMapMemory(context_.device, bootstrapMemory_, 0, required.size, 0, &mapped_), "engine-map")) return false;
    std::memset(mapped_, 0, 4096);
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commands_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (!checkVulkan(vkAllocateCommandBuffers(context_.device, &commandInfo, &command), "engine-command")) return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!checkVulkan(vkBeginCommandBuffer(command, &begin), "engine-begin")) return false;
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(command, bootstrapBuffer_, 0, 4096, 0x51A7C0DE);
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
    if (!checkVulkan(vkEndCommandBuffer(command), "engine-end")) return false;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!checkVulkan(vkCreateFence(context_.device, &fence, nullptr, &fence_), "engine-fence")) return false;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (!checkVulkan(recomp::gpu::lockedQueueSubmit(context_.queue, 1, &submit, fence_), "engine-submit")) return false;
    const auto waited = vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, 5'000'000'000ULL);
    if (waited != VK_SUCCESS) {
        checkVulkan(waited, "engine-wait");
        std::fflush(stdout);
        // Never destroy potentially in-flight resources or claim initialization.
        std::_Exit(4);
    }
    const auto* words = static_cast<const uint32_t*>(mapped_);
    for (size_t i = 0; i < 1024; ++i) if (words[i] != 0x51A7C0DE) return false;
    queueVerified_ = true;
    return true;
}
}
