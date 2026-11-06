#pragma once
#include "native_display_gamma.h"
#include "native_display_output.h"
#include "guest_native_display.h"
#include "guest_initial_coherency.h"
#include "native_swap_completion.h"
#include "native_scaler_interrupt_sequence.h"
#include "native_scaler_display_state.h"
#include "native_post_output_swap_completion.h"
#include "native_post_output_swap_interrupt_sequence.h"

namespace recomp::gpu { struct StoppedConstantInitialization; struct NativeInitializationReadback;
class Pm4Snapshot;struct Pm4IndirectGraph;
class NativeColorChainProgram;class NativeColorBatchCapture;struct NativeColorPublisher;
class NativeDeferredColorCapture;class NativeDeferredSwapSubmission;struct NativeDeferredColorPublisher; }

namespace recomp::bootstrap {
// Host graphics lifecycle. Implementations own real backend resources and
// verify queue operation before readiness. This does not imply support for
// every Xenos command or execution of guest GPU firmware.
// Called under the registry lifecycle lock: implementations must not call
// guest code or reenter the registry from initialize()/ready().
class GuestGraphicsBackend {
public:
    virtual bool executeExperimentalNativeDisplayOutput(const recomp::gpu::NativeDisplayOutputWrapper&,
        const std::function<bool()>&,recomp::gpu::NativeDisplayOutputResult&) {return false;}
    enum class EdramTrainingPolicy { Unavailable, DriverManagedNoXenosLink };
    virtual ~GuestGraphicsBackend() = default;
    virtual bool initialize() = 0;
    virtual bool ready() const noexcept = 0;
    // Explicit native metadata consumer. Acceptance is not a draw, scaler
    // execution, resize completion or system-overlay callback. Fail closed.
    virtual bool configureDisplay(const NativeDisplayConfiguration&) { return false; }
    // Configured transfer function, not a physical-monitor gamma measurement.
    virtual bool queryDisplayGamma(std::uint32_t&, std::uint32_t&) const noexcept { return false; }
    // Explicit native fixed-mode selection/readback. Applies to the logical
    // output contract, never physical monitor timing or vblank. Fail closed.
    virtual bool selectNativeDisplayMode(std::uint32_t) noexcept { return false; }
    virtual bool querySelectedDisplayConfiguration(NativeDisplayConfiguration&) const noexcept { return false; }
    // Native command storage is read directly from synchronized owned CPU
    // memory. Requires no retained GPU-backed guest bindings in this range.
    // Issue a fresh real native queue ordering/fence boundary, preserving CPU
    // EDRAM/register state. Not Xbox cache/status-bit emulation or an upload.
    virtual bool completeNativeCommandStorageCoherency(const NativeInitialCoherency&) { return false; }
    // Separate fresh command-allocation fence after the first ordered swap and
    // before the native scaler command. Same conservative CPU-owned resource
    // model; no upload, scaler execution, guest wait-bit write or presentation.
    virtual bool completeNativeScalerStorageCoherency(const NativeInitialCoherency&) { return false; }
    // Ordered prefix through the command-interrupt boundary. Action handles
    // ONLY scoped scratch writes/actual reads under the caller's owner lease.
    // Backend supplies a fresh native idle fence and retains register state.
    // False/throw after an action is terminal, never permission to retry.
    // No guest callback, scaler, event tail, presentation or RPTR retirement.
    virtual bool beginNativeScalerInterrupt(const recomp::gpu::NativeScalerWrapper&,
        const recomp::gpu::NativeScalerInterruptSequence::Action&) { return false; }
    // Caller must independently verify the registered guest callback returned
    // with balanced native locks and intact stack/PCR. Reads the two real zero
    // waits; no host-created zero acknowledgement. Stops before scaler update.
    virtual bool acknowledgeNativeScalerInterrupt(const recomp::gpu::NativeScalerInterruptSequence::Action&) { return false; }
    // Only after the owned IRQ sequence has acknowledged. Applies native
    // logical scanout, invalidates derived state and GPU-writes/audits two
    // literal events. Three new fences; no guest writes or presentation.
    virtual bool completeNativeScalerTail(recomp::gpu::NativeScalerTailResult&) { return false; }
    // Fresh command-allocation ordering after completed native scanout setup.
    // No cached completion reuse or arbitrary guest resource coherency.
    virtual bool completeNativePostScalerStorageCoherency(const NativeInitialCoherency&) { return false; }
    virtual bool completeNativePostDisplayStorageCoherency(const NativeInitialCoherency&) { return false; }
    virtual bool completeNativePostOutputStorageCoherency(const NativeInitialCoherency&) { return false; }
    virtual bool executeNativeDisplayGamma(const recomp::gpu::NativeDisplayGammaWrapper&,
        const std::function<bool()>&,recomp::gpu::NativeDisplayGammaResult&) { return false; }
    // Complete the entire native wrapper under the explicit conservative
    // owned-resource/idle-cache policy. All writers are parked; no registry
    // reentry or guest memory access. Callback may be invoked at most once.
    // False/throw is legal ONLY before callback acceptance. True requires
    // acceptance, four audited native fences/events and no-throw state commit.
    virtual bool executeNativeSwapWrapper(const recomp::gpu::NativeSwapWrapper&,
        const std::function<bool()>&,recomp::gpu::NativeSwapGpuResult&) { return false; }
    // Fresh ordering of the second swap's owned command allocation, only after
    // live post-output color publication. No reuse of the first swap fence.
    virtual bool completeNativePostOutputSwapStorageCoherency(const NativeInitialCoherency&) { return false; }
    // Separate fresh command-owner fence for a deferred request after swap2.
    // No image is uploaded, decoded or marked rendered by this operation.
    virtual bool completeNativeDeferredSwapStorageCoherency(const NativeInitialCoherency&) { return false; }
    // Runtime command-allocation ordering, after startup and between native
    // submissions. The registry proves the CURRENT pinned command/ring owners
    // and the exact coherency-only packet. Every call needs a fresh fence;
    // retained render targets and unrelated guest completion remain untouched.
    virtual bool completeNativeRuntimeCommandStorageCoherency(const NativeInitialCoherency&) { return false; }
    // Read-only fully known retained scratch base/mask, not an IRQ receipt.
    // Leave outputs untouched on refusal. Required by the second-swap owner.
    virtual bool queryNativeScratchWriteback(std::uint32_t&,std::uint32_t&) const noexcept { return false; }
    using NativePostOutputSwapInterrupt=std::function<bool(const recomp::gpu::NativePostOutputSwapInterruptSequence::Action&)>;
    // Explicit reentrant IRQ seam: caller releases the registry mutex and holds
    // the exclusive executor gate. Its callback must run the fresh scoped guest
    // handler and real acknowledgement. Native action receives all11 non-IRQ
    // operations in order AFTER owner scratch writes/reads succeed. No retained
    // callback or borrowed views. Accepted presentation makes failure terminal.
    virtual bool executeNativePostOutputSwapWrapper(const recomp::gpu::NativePostOutputSwapWrapper&,
        const std::function<bool()>&,const NativePostOutputSwapInterrupt&,recomp::gpu::NativePostOutputSwapGpuResult&) { return false; }
    using NativeDeferredSwapInterrupt=std::function<bool(const recomp::gpu::NativeDeferredSwapInterruptSequence::Action&)>;
    // Third tail ONLY after this backend committed the exact whole prefix
    // request. Diagnostic results never qualify. Same exclusive executor/owner
    // contract as above, but independently typed actions/result and fresh work.
    virtual bool executeNativeDeferredSwapWrapper(const recomp::gpu::NativeDeferredSwapSubmission&,
        const std::function<bool()>&,const NativeDeferredSwapInterrupt&,recomp::gpu::NativeDeferredSwapGpuResult&) { return false; }
    // One cold-start invalidation only, before any guest resources are resident.
    // Acceptance requires a real backend ordering/completion boundary. It must
    // not clear arbitrary GPU waits or claim resource upload/draw completion.
    virtual bool completeInitialEmptyCacheCoherency(const NativeInitialCoherency&) { return false; }
    // Subsequent exact invalidations, only while there are no resident guest
    // resources. Must order/fence each call independently; no cached success.
    virtual bool completeEmptyGuestCacheCoherency(const NativeInitialCoherency&) { return false; }
    // Opt-in experimental initialization contract; all input is an independent
    // snapshot. Return only after GPU completion and stable host-visible data.
    // No guest pointer, callback, registry reentry or guest publication is allowed.
    virtual bool completeExperimentalConstantInitialization(const recomp::gpu::StoppedConstantInitialization&,
        recomp::gpu::NativeInitializationReadback&) { return false; }
    // Validate the independent command snapshot again. Preserve native state;
    // return audited event bytes only after real GPU execution/fence completion.
    virtual bool completeNativeStateBatch(const std::vector<std::uint32_t>&,
        recomp::gpu::NativeInitializationReadback&) { return false; }
    // Unverified 0E00/0E40 cache-pair hypothesis, enabled separately. Accept
    // only the exact idle/pair batch with NO resident guest resources, and
    // perform a fresh native ordering/fence boundary. No status-bit emulation.
    virtual bool completeExperimentalIdleCachePair(const std::vector<std::uint32_t>&,
        recomp::gpu::NativeInitializationReadback&) { return false; }
    // Raw owned snapshots, separately proved and executed as maintenance points.
    // Publisher runs at most once AFTER fresh fenced/audited native work. False
    // means no guest publication; true is followed only by no-throw native state
    // commit. Caller pins allocation owners and parks every guest writer.
    virtual bool executeExperimentalMaintenancePoints(const recomp::gpu::Pm4Snapshot&,
        const recomp::gpu::Pm4IndirectGraph&,
        const std::function<bool(const recomp::gpu::NativeInitializationReadback&)>&) { return false; }
    // Explicit bounded native resource model. Return false before publication
    // on failure; true only after synchronous audited GPU work, publisher true,
    // and a nonthrowing native state commit. No reentry/retained borrowed views.
    // A backend must never throw/return false after the publisher succeeds.
    virtual bool executeExperimentalOwnedColorBatch(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::NativeColorBatchCapture&,const recomp::gpu::NativeColorPublisher&) { return false; }
    // Separate single post-output full-region replacement. Requires one NEW
    // leading setup fence plus four audited stages; same publisher contract.
    // Updating owned memory is NOT presenting a new scanout image.
    virtual bool executeExperimentalPostOutputColorBatch(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::NativeColorBatchCapture&,const recomp::gpu::NativeColorPublisher&) { return false; }
    // Prefix-only fresh execution and copy/state commit. Caller must retain
    // WHOLE combined-submission ownership and parked writers. True does NOT
    // complete the ticket3 surface/IRQ tail or authorize RPTR retirement.
    virtual bool executeExperimentalDeferredColorPrefix(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::NativeDeferredColorCapture&,const recomp::gpu::NativeDeferredColorPublisher&) { return false; }
    // Drain all native work before retiring a command-ring generation. This
    // does not execute new guest commands or write a guest completion value.
    virtual bool drainForRingReconfiguration() { return false; }
    virtual bool completeWarmRingInitialization(const std::vector<std::uint32_t>&,
        const std::vector<std::uint32_t>&) { return false; }
    // Native capability, NOT successful electrical training or EDRAM rendering.
    // Return the no-link policy only after real initialization has completed;
    // unsupported backends must not silently opt in. No guest work or reentry.
    virtual EdramTrainingPolicy queryEdramTrainingPolicy() { return EdramTrainingPolicy::Unavailable; }
};
}
