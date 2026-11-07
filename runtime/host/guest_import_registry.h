#pragma once

#include "guest_image_protections.h"
#include "native_gamepad_state.h"
#include "guest_graphics_backend.h"
#include "guest_audio_backend.h"
#include "guest_xma_backend.h"
#include "native_audio_render_client.h"
#include "native_tiled_copy_publication.h"
#include "native_swap_command.h"
#include "native_scaler_command.h"
#include "native_scaler_wrapper.h"
#include "native_post_output_swap_wrapper.h"
#include "native_post_output_swap_interrupt_sequence.h"

#include "guest_address_space.h"
#include "guest_file_metadata.h"
#include "guest_symbolic_links.h"
#include "guest_content_catalog.h"
#include "guest_read_only_file.h"
#include "guest_xex_header.h"
#include "guest_thread_storage_pool.h"
#include "guest_thread_entry.h"
#include "guest_waitable_timer.h"

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace recomp::gpu { class NativeColorChainProgram; class NativeColorBatchCapture; class NativeDeferredSwapSubmission; class Gta5NativeTileInitialQueue; class Gta5NativeTileFirstScene; class Gta5NativeTileFinalScene; class Gta5NativeTileSubmissionCapture; class Gta5TileCommandInterrupt; class Gta5TileDeferredTail; struct DrawBinState; struct NativeColorResolvedMemory; }

namespace recomp::bootstrap {

struct GuestRegistryValidationSnapshot; // Diagnostic attempt v42+ (defined only in attempt registries).

enum class GuestImportCategory {
    Memory,
    String,
    Heap,
    Logging,
    Process,
    Title,
    Synchronization,
    ThreadLocal,
};

enum class GuestImportDispatchState {
    Handled,
    NotRegistered,
    RegistryUnavailable,
    GuestMemoryFault,
    Rejected,
};

struct GuestImportDescriptor {
    std::string_view name;
    GuestImportCategory category;
    std::uint32_t minimumDirectCallDistanceFromEntry;
    bool writesReturnValue;
};

// Snapshot of the Xenon integer argument registers r3-r10. A future generated
// PPCContext adapter can copy into/out of this type without making the portable
// shim layer depend on a private generated header.
struct GuestImportCall {
    std::array<std::uint64_t, 8> arguments{};
    std::uint64_t returnValue{0};
    std::uint32_t stackPointer{0}; // Guest r1, never a host pointer.

    [[nodiscard]] std::uint32_t argument32(std::size_t index) const noexcept {
        return index < arguments.size()
                ? static_cast<std::uint32_t>(arguments[index])
                : 0;
    }
    [[nodiscard]] bool readArgument32(const GuestAddressSpace& space, std::size_t index,
            std::uint32_t& value) const noexcept {
        if (index < arguments.size()) { value = argument32(index); return true; }
        // Xbox integer stack slots: argument 9 is at r1+0x54, stride 8.
        // This bounded seam deliberately does not implement varargs or FP args.
        if (index >= 16 || !space.isReserved() || stackPointer == 0 || (stackPointer & 15U) != 0) { return false; }
        const auto address = std::uint64_t{stackPointer} + 0x54 + (index - 8) * 8;
        if (address + 4 > GuestAddressSpace::Size) { return false; }
        const auto* bytes = space.translate(static_cast<std::uint32_t>(address));
        value = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16)
                | (std::uint32_t{bytes[2]} << 8) | bytes[3];
        return true;
    }
};

struct GuestImportDispatchResult {
    GuestImportDispatchState state{GuestImportDispatchState::NotRegistered};
    std::string importName;
    std::string diagnostic;

    [[nodiscard]] bool handled() const noexcept {
        return state == GuestImportDispatchState::Handled;
    }
};

using GuestLogSink = std::function<void(std::string_view)>;

class GuestImportRegistry final {
public:
    static constexpr std::uint32_t PageSize = 0x1000U;
    static constexpr std::size_t MaximumKernelHandles = 4096;
    static constexpr std::uint32_t MaximumMutantRecursion = 65535;

    GuestImportRegistry() = default;

    GuestImportRegistry(const GuestImportRegistry&) = delete;
    GuestImportRegistry& operator=(const GuestImportRegistry&) = delete;

    // The caller chooses an explicit page-aligned arena that does not overlap
    // the decoded image, stack, or runtime tables. The address space must
    // outlive this registry (or reset must be called first). No allocation
    // occurs until a registered heap import is explicitly dispatched. The log
    // sink is called synchronously and must not re-enter this registry.
    [[nodiscard]] bool initialize(
            GuestAddressSpace& addressSpace,
            std::uint32_t heapBase,
            std::uint32_t heapSize,
            GuestLogSink logSink = {});
    void reset();
    // Explicit headless configuration: zero guest input devices. This does not
    // expose host controllers, generate state/events, or claim usable controls.
    [[nodiscard]] bool bindDisconnectedInput();
    // 1=connected snapshot, 0=disconnected, other=backend failure. Must not
    // reenter the registry. Controller presence does not create a user profile.
    using NativeGamepadReader=std::function<int(std::uint32_t,recomp::input::GamepadState&)>;
    [[nodiscard]] bool bindNativeGamepad(NativeGamepadReader reader,NativeGamepadReader capabilities);
    // Experimental reference speaker word 0x00010001, gated on an actual open
    // native output stream. Xbox bitfield semantics/hardware equivalence are
    // unverified. Registration, frames, callbacks and volume imports stay guarded.
    [[nodiscard]] bool bindExperimentalAudioOutput(std::shared_ptr<GuestAudioBackend> backend);
    [[nodiscard]] std::uint32_t audioSpeakerQueries() const;

    // The portable synchronization slice never invents a current guest
    // thread. Its caller must bind a nonzero, guest-visible identity before
    // dispatching critical-section enter/leave calls. Rebinding is explicit so
    // tests and the cooperative scheduler can expose contention. Blocking is
    // guarded unless bindNativeCriticalSectionWaits explicitly opts into it.
    [[nodiscard]] bool bindGuestThreadToken(std::uint32_t token);
    void unbindGuestThreadToken();
    // Call after binding token at least once. dynamicSlotBase is the first
    // dynamic slot, after any caller-staged static TLS template. Only this
    // slotCount * 4 byte range is written. Existing tracked values are restored
    // in big endian order; repeated identical bindings preserve guest writes.
    // Process allocations are capped by the smallest bound storage (2048
    // without backing); binding fails if any live slot exceeds that capacity.
    // The caller excludes image, static TLS, PCR, and stack ranges; this class
    // additionally rejects overlaps with its arenas and tracked TLS/locks.
    [[nodiscard]] bool bindGuestThreadTlsStorage(
            std::uint32_t token,
            std::uint32_t dynamicSlotBase,
            std::uint32_t slotCount);
    // Preserve live values in token-only storage, then scrub the dynamic slots.
    void unbindGuestThreadTlsStorage(std::uint32_t token);
    // Scrub and discard one thread's TLS, leaving process slot allocations
    // intact. Refuse removal while that token owns a tracked critical section.
    [[nodiscard]] bool teardownGuestThread(std::uint32_t token);
    [[nodiscard]] bool bindExecutableXexHeader(std::uint32_t address);
    void unbindExecutableXexHeader();
    // One immutable page-metadata binding until reset; no host permission changes.
    [[nodiscard]] bool bindImagePageProtections(std::uint32_t header, std::uint32_t headerBytes,
                                              std::uint32_t imageBase, std::uint32_t imageBytes);
    // physicalAddressOffset is the physical address corresponding to
    // arenaBase. The arena is independent from, and must not overlap, the
    // virtual heap passed to initialize().
    [[nodiscard]] bool bindPhysicalMemoryArena(
            std::uint32_t arenaBase,
            std::uint32_t arenaSize,
            std::uint32_t physicalAddressOffset);
    void unbindPhysicalMemoryArena();
    [[nodiscard]] bool bindNativeXma(std::shared_ptr<GuestXmaBackend> backend);
    // Only for synchronous native audio under the serialized guest gate. The
    // returned span is borrowed until that operation returns, never retained.
    [[nodiscard]] std::uint8_t* mapNativeAudioPhysical(std::uint32_t physical,std::uint32_t bytes,bool writable);

    // Opt-in, owned storage for flags 0/1 creation. The caller excludes all
    // title/header/initial-thread scratch ranges; known registry arenas and TLS
    // are checked again here. Validator must be a pure exact-function lookup
    // and must not re-enter this registry. reset tears down every created thread.
    [[nodiscard]] bool bindThreadStorageArena(
            const GuestThreadStorageConfig& config,
            const std::vector<GuestThreadMemoryRange>& excludedRanges,
            std::function<bool(std::uint32_t)> exactFunctionValidator);
    [[nodiscard]] std::optional<GuestPreparedThread> lookupSuspendedThread(
            std::uint32_t handle) const;
    [[nodiscard]] std::size_t createdThreadCount() const;
    // Native service worker, not an ExCreateThread call or an NT handle. Owns
    // a separate wrapped-context page, PCR, static/dynamic TLS and title-sized
    // stack. Enqueue is the last fallible step and MUST NOT run inline/reenter
    // this registry; false/throw MUST leave no queued task. Keep storage until
    // executor shutdown/reset, including after markGuestThreadReturned().
    using NativeCallbackEnqueue = std::function<bool(const GuestPreparedThread&)>;
    [[nodiscard]] std::optional<GuestPreparedThread> createNativeCallbackWorker(
        std::uint32_t callback,std::uint32_t context,const NativeCallbackEnqueue& enqueue);
    // Prepare ONE registered command-interrupt worker for the complete pending
    // native scaler wrapper. Caller holds the serialized execution gate; all
    // other guest writers are parked. No inline dispatch/reentry from enqueue.
    // Requires CPU2 identity and direct(source1,userData) ABI, not audio wrapping.
    // Allocation/enqueue alone does not publish scratch, execute the interrupt,
    // change IRQL, acknowledge a wait, execute the scaler or advance RPTR.
    using NativeCommandInterruptEnqueue=std::function<bool(const GuestPreparedThread&,const GuestThreadEntryState&)>;
    [[nodiscard]] std::optional<GuestPreparedThread> createNativeCommandInterruptWorker(
        std::uint32_t writeWord,const NativeCommandInterruptEnqueue& enqueue);
    // Separate CPU2 worker for ticket2; never resets/borrows the completed
    // scaler worker. Enqueue alone does not admit or execute guest interrupts.
    [[nodiscard]] std::optional<GuestPreparedThread> createNativePostOutputSwapInterruptWorker(
        std::uint32_t writeWord,const NativeCommandInterruptEnqueue& enqueue);
    [[nodiscard]] std::optional<std::uint32_t> nativePostOutputSwapInterruptWorker() const;
    // Fresh ticket3 CPU2 worker; preserves both earlier workers and receipts.
    [[nodiscard]] std::optional<GuestPreparedThread> createNativeDeferredSwapInterruptWorker(
        std::uint32_t writeWord,const NativeCommandInterruptEnqueue& enqueue);
    [[nodiscard]] std::optional<std::uint32_t> nativeDeferredSwapInterruptWorker() const;
    // Explicit native level2 scope for this one CPU2 worker, called under the
    // executor's exclusive worker handoff. Actual guest dispatch must additionally
    // use its bounded non-yielding scope; prefix GPU fences remain host-bounded.
    // The registry mutex is released for
    // invoke; only the four observed lock imports and two read-only process/
    // clock queries are admitted. No host spinning, APC delivery or retail IRQL
    // equivalence. A failed invocation is not retried and guest writes are not
    // rolled back. Return means balanced locks, NOT scratch/GPU completion.
    [[nodiscard]] bool runNativeCommandInterruptScope(std::uint32_t writeWord,const std::function<void()>& invoke);
    struct NativeCommandInterruptScopeStatus {
        bool attempted=false,returned=false,balanced=false;
        std::uint32_t worker=0,heldLocks=0;
        std::array<std::uint32_t,2> lockPairs{};
    };
    [[nodiscard]] NativeCommandInterruptScopeStatus nativeCommandInterruptScopeStatus() const;
    // Original8232E278 (pending-list producer) and836BE058 (ordinary worker)
    // pair Kf locks on device+11156. They are NOT the source1 IRQ profile.
    // The supplied gate must be this executor's runNativeNonYielding scope;
    // all peer writers stay parked through invoke and final validation.
    // One nonrecursive pair, native logical level0->2->0, no spinning/APCs or
    // retail IRQL claim. No guest CPU/PCR byte is rewritten. Only Kf lock
    // imports and two read-only kernel queries are admitted. Never clears a
    // held guest word on failure/unwind. Return proves locking, NOT GPU work.
    using NativeSerializedGate=std::function<bool(const std::function<void()>&)>;
    [[nodiscard]] bool runNativeTileLockScope(std::uint32_t device,
        const NativeSerializedGate& gate,const std::function<void()>& invoke);
    struct NativeTileLockScopeStatus {
        bool attempted=false,returned=false,balanced=false,failed=false;
        std::uint32_t worker=0,device=0,pairs=0;bool held=false;
    };
    [[nodiscard]] NativeTileLockScopeStatus nativeTileLockScopeStatus() const;
    // Scene-only source1 CPU handler scope. The caller must already own a
    // real CPU2 task and freshly completed native prefix/command leases.
    // Validates registered836B1768 -> scratch callback836BE1C8, empty CPU2
    // queue, owned notification event, exact raised ack-lock pair and original
    // scratch acknowledgement. No new task, GPU operation or RPTR retirement.
    // Unlike the swap IRQ scope, admits exactly one real scene event/boost.
    [[nodiscard]] bool runNativeTileSceneInterruptScope(std::uint32_t device,
        std::uint32_t callbackContext,const NativeSerializedGate&,const std::function<void()>&);
    // Allocate/enqueue a fresh direct source1/CPU2 entry for the registered
    // scene handler. No wrapped audio context, affinity rewrite of an existing
    // task, scratch write, event signal, GPU completion or ring retirement.
    // Caller holds its executor gate; enqueue must not execute/reenter inline.
    // Execution still requires fresh command/scratch leases and the IRQ scope.
    [[nodiscard]] std::optional<GuestPreparedThread> createNativeTileSceneInterruptWorker(
        std::uint32_t device,std::uint32_t callbackContext,const NativeCommandInterruptEnqueue&);
    struct NativeTileSceneInterruptStatus {
        bool attempted=false,returned=false,balanced=false,failed=false;
        std::uint32_t worker=0,device=0,lockPairs=0,eventSignals=0;bool held=false;
    };
    [[nodiscard]] NativeTileSceneInterruptStatus nativeTileSceneInterruptStatus() const;
    // Read-only registration evidence for the ORIGINAL CPU2 scene event.
    // Excludes signaled/settled/expired/group waits; does not consume a signal
    // or prove an executor task is parked. The scheduler must check that too.
    [[nodiscard]] std::optional<std::uint32_t> nativeTileSceneWaitingWorker(std::uint32_t device) const;
    // Retire ONLY the completed initial cache/IRQ queue, under the caller's
    // actual non-yielding executor gate. Requires its current sealed bytes,
    // pinned configured ring/writeback, returned registered original IRQ and
    // final scratch ACK/poison. Publishes RPTR and ring.readWord together; no
    // state-batch receipt, scene draw, scene fence or presentation is invented.
    [[nodiscard]] bool retireNativeTileInitialQueue(const recomp::gpu::Gta5NativeTileInitialQueue&,
        const NativeSerializedGate&);
    // Exact executed first scene + bounds + returned selected IRQ, before the
    // ordinary consumer rewrites its IB. Does not retire later submissions.
    [[nodiscard]] bool retireNativeTileFirstScene(const recomp::gpu::Gta5NativeTileFirstScene&,
        const NativeSerializedGate&);
    [[nodiscard]] bool retireNativeTileFinalScene(const recomp::gpu::Gta5NativeTileFinalScene&,
        const NativeSerializedGate&);
    struct NativePostOutputSwapInterruptScopeStatus {
        NativeCommandInterruptScopeStatus scope{};
        std::uint32_t rejection=0;
        bool queuedCallbackValidated=false;
    };
    // Fresh source1/CPU2 scope for the observed empty-queue second-swap profile.
    // invoke must perform the ordered prefix and ORIGINAL guest handler, not
    // the later ACK/poison. The owner still owes GPU work, acknowledgement and
    // RPTR-last publication. Requires the executor's exclusive non-yielding gate.
    [[nodiscard]] bool runNativePostOutputSwapInterruptScope(std::uint32_t writeWord,const std::function<void()>& invoke);
    [[nodiscard]] NativePostOutputSwapInterruptScopeStatus nativePostOutputSwapInterruptScopeStatus() const;
    [[nodiscard]] bool beginNativePostOutputSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativePostOutputSwapInterruptSequence::Action& nativeCommand);
    [[nodiscard]] bool acknowledgeNativePostOutputSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativePostOutputSwapInterruptSequence::Action& nativeCommand);
    [[nodiscard]] std::optional<recomp::gpu::NativePostOutputSwapInterruptSequence::Phase> nativePostOutputSwapInterruptPhase() const;
    struct NativeDeferredSwapInterruptScopeStatus {
        NativeCommandInterruptScopeStatus scope{};
        std::uint32_t rejection=0;
        bool queuedCallbackValidated=false;
    };
    // Separate nonempty-queue profile after committed prefix. No vblank/source0,
    // second-worker reuse or artificial acknowledgement/queue consumption.
    [[nodiscard]] bool runNativeDeferredSwapInterruptScope(std::uint32_t writeWord,const std::function<void()>& invoke);
    [[nodiscard]] NativeDeferredSwapInterruptScopeStatus nativeDeferredSwapInterruptScopeStatus() const;
    [[nodiscard]] bool beginNativeDeferredSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativeDeferredSwapInterruptSequence::Action& nativeCommand);
    [[nodiscard]] bool acknowledgeNativeDeferredSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativeDeferredSwapInterruptSequence::Action& nativeCommand);
    [[nodiscard]] std::optional<recomp::gpu::NativeDeferredSwapInterruptSequence::Phase> nativeDeferredSwapInterruptPhase() const;
    // Pure validator owned by the existing video-data-variable binding. Bind
    // before graphics init; verifies slot -> owned storage -> expected device
    // under mutex without guest execution/reentry.
    [[nodiscard]] bool bindNativeInterruptDeviceRoot(std::function<bool(std::uint32_t)> validator);
    // Read-only stopped-boundary proof from the exact bound video-global
    // validator. Null means unavailable; false is an observed mismatch.
    [[nodiscard]] std::optional<bool> snapshotNativeInterruptDeviceRoot(std::uint32_t device) const;
    [[nodiscard]] bool beginNativeScalerInterrupt(std::uint32_t writeWord);
    [[nodiscard]] bool acknowledgeNativeScalerInterrupt(std::uint32_t writeWord);
    struct NativeScalerInterruptStatus {
        bool prefixAttempted=false,prefixReady=false,ackAttempted=false,acknowledged=false;
        std::uint32_t rejection=0; // Scope1,plan2,scratch3,process-root4,pacing5,backend6,continuity7; preflight8..17.
        // Observed preflight only: valid,refresh,count,optional callback,context,
        // frequency. Neither these values nor exact mapping prove call safety.
        std::array<std::uint64_t,6> pacing{};
    };
    [[nodiscard]] NativeScalerInterruptStatus nativeScalerInterruptStatus() const;
    struct NativeScalerCompletion {
        std::uint32_t previousWord=0,readWord=0;
        std::array<std::uint32_t,2> eventPhysical{};
        recomp::gpu::NativeScalerTailResult gpu;
    };
    [[nodiscard]] bool completeNativeScalerWrapper(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativeScalerCompletion> completedNativeScalerWrapper() const;
    struct NativePostScalerStorageCompletion {
        std::uint32_t batches=0,firstWord=0,previousWord=0,readWord=0;
        NativeInitialCoherency request{};
    };
    bool submitNativePostScalerStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativePostScalerStorageCompletion> completedNativePostScalerStorageCoherency() const;
    bool submitNativePostDisplayStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativePostScalerStorageCompletion> completedNativePostDisplayStorageCoherency() const;
    bool submitNativePostOutputStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativePostScalerStorageCompletion> completedNativePostOutputStorageCoherency() const;
    // Separate from immutable startup phase history. Accept only an exact
    // command-allocation coherency transaction at the current owned RPTR.
    bool submitNativeRuntimeCommandStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativePostScalerStorageCompletion> completedNativeRuntimeCommandStorageCoherency() const;
    struct NativeRuntimeMaintenanceCompletion {
        std::uint32_t batches=0,previousWord=0,readWord=0,draws=0,shaderLoads=0,invalidations=0;
        std::uint32_t fencePhysical=0,fenceValue=0,commandCursor=0;
    };
    using NativeRuntimeMaintenanceExecutor=std::function<bool(const recomp::gpu::Pm4Snapshot&,
        const recomp::gpu::Pm4IndirectGraph&,
        const std::function<bool(const recomp::gpu::NativeInitializationReadback&)>&)>;
    // Separate runtime transaction after the real returned scene. The gate
    // parks all guest writers; execute must fence native work and call the
    // failure-atomic publisher synchronously, exactly once, without reentry.
    [[nodiscard]] bool submitNativeRuntimeMaintenancePoints(std::uint32_t device,std::uint32_t writeWord,
        const NativeSerializedGate&,const NativeRuntimeMaintenanceExecutor&);
    [[nodiscard]] std::optional<NativeRuntimeMaintenanceCompletion> completedNativeRuntimeMaintenancePoints() const;
    // Reporting only. Records which admission condition stopped the last runtime
    // maintenance attempt and the actual allocation owners it resolved, so a live
    // rejection is explainable without relaxing any condition. `reachedExecute`
    // distinguishes a registry rejection from an engine callback that ran.
    struct NativeRuntimeMaintenanceDiagnostic {
        std::uint32_t attempts=0,rejection=0,reachedExecute=0,failedLatch=0;
        std::uint32_t ringAllocation=0,writeBackAllocation=0;
        std::uint32_t fencePhysical=0,fenceAllocation=0;
        std::uint32_t cursorPhysical=0,cursorAllocation=0,leaseCount=0;
        std::array<std::uint32_t,4> leasePhysical{};
        std::array<std::uint32_t,4> leaseAllocation{};
    };
    [[nodiscard]] NativeRuntimeMaintenanceDiagnostic nativeRuntimeMaintenanceDiagnostic() const;
    struct NativeDisplayGammaCompletion {
        std::uint32_t previousWord=0,readWord=0,childPhysical=0,frontbufferPhysical=0;
        std::array<std::uint32_t,2> eventPhysical{};
        recomp::gpu::NativeDisplayGammaResult gpu;
    };
    [[nodiscard]] bool submitNativeDisplayGamma(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativeDisplayGammaCompletion> completedNativeDisplayGamma() const;
    struct NativeDisplayOutputCompletion {
        std::uint32_t previousWord=0,readWord=0,childPhysical=0;
        std::array<std::uint32_t,2> eventPhysical{};
        recomp::gpu::NativeDisplayOutputResult gpu;
    };
    // Separate opt-in normal-video policy for captured XDVO bit0-clear only.
    // Electrical/retail force-data behavior is not implemented or implied.
    [[nodiscard]] bool submitExperimentalNativeDisplayOutput(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativeDisplayOutputCompletion> completedNativeDisplayOutput() const;
    // Explicit single-client PCM rendering. Enqueue has the same last-fallible,
    // no-inline/no-reentry contract as native worker creation. No unregister or
    // multi-client mixing is implied; reset only after executor shutdown.
    [[nodiscard]] bool bindExperimentalAudioRenderer(
        std::shared_ptr<audio::NativeAudioRenderClient> client,NativeCallbackEnqueue enqueue);
    // Explicit immutable native category1 policy: no category-level attenuation.
    // The title consumes this gain; do not multiply the mixed PCM again. Not an
    // Android system-volume query, ducking policy, or Xbox dashboard setting.
    [[nodiscard]] bool bindNativeCategoryOneUnityGain();
    [[nodiscard]] std::uint64_t nativeCategoryOneGainQueries() const;
    // The title loader owns the opaque type identity storage. This does not
    // synthesize a complete OBJECT_TYPE or adopt an emulator's magic token.
    [[nodiscard]] bool bindThreadObjectType(std::uint32_t identityAddress);
    [[nodiscard]] bool bindInitialThreadObject(std::uint32_t token, std::uint32_t threadId);
    [[nodiscard]] std::uint32_t threadObjectReferenceCount(std::uint32_t token) const;
    static constexpr std::uint32_t MaximumThreadObjectReferences = 65535;
    struct ThreadPriorityRequest {
        std::int32_t increment;
        std::int32_t previousIncrement;
        bool initial;
        bool current;
        bool suspended;
        bool returned;
    };
    // Last known-object request only. No guest pointers, names, or memory retained.
    [[nodiscard]] std::optional<ThreadPriorityRequest> lastThreadPriorityRequest() const;
    using ThreadResumeHandler = std::function<bool(const GuestPreparedThread&, std::int32_t)>;
    // Handler must enqueue without running guest code inline. False or an
    // exception must leave no runnable task behind. Bind before resume or
    // unsuspended creation; reset only after every
    // executor worker has joined.
    [[nodiscard]] bool bindThreadResumeHandler(ThreadResumeHandler handler);
    using ThreadExitHandler=std::function<bool(std::uint32_t,std::uint32_t)>;
    // Called WITHOUT the registry mutex. Must validate executor ownership,
    // commit markGuestThreadReturned and unwind the current task on success.
    // False rejects without mutation; no success return to original game code.
    [[nodiscard]] bool bindThreadExitHandler(ThreadExitHandler handler);
    [[nodiscard]] std::optional<std::uint32_t> guestThreadExitCode(std::uint32_t token) const;
    using ThreadPriorityHandler = std::function<bool(std::uint32_t, std::int32_t)>;
    // Called under the registry mutex: no reentry, guest execution or switching.
    // False leaves scheduler state unchanged. Normal -2..2 or saturation +/-16.
    [[nodiscard]] bool bindThreadPriorityHandler(ThreadPriorityHandler handler);
    // Immutable opt-in after the priority setter is bound. Caller must enable
    // the same +3/+4 tiers in its executor, including subsequently resumed workers.
    [[nodiscard]] bool bindNativeHighPriorityTiers();
    [[nodiscard]] std::uint64_t nativeHighPriorityTiersRequests() const;
    using NativeEventWakeBoostHandler=std::function<bool(const std::uint32_t*,std::size_t)>;
    // Explicit native increment1 policy, not console priority/IRQL equivalence.
    // Handler atomically boosts every supplied parked thread or changes none;
    // no allocation/reentry/guest execution/switching. DisableBoost suppresses
    // future boosts before calling the handler. Increment>1 and Wait!=0 reject.
    [[nodiscard]] bool bindNativeEventWakeBoostHandler(NativeEventWakeBoostHandler handler);
    struct NativeEventWakeBoostStatistics {std::uint64_t signalRequests{0},threadsBoosted{0},threadsSuppressed{0};};
    [[nodiscard]] NativeEventWakeBoostStatistics nativeEventWakeBoostStatistics() const;
    [[nodiscard]] bool markGuestThreadReturned(std::uint32_t token, std::uint32_t exitCode);
    // Explicit diagnostic policy: six virtual CPU identities, singleton masks,
    // serialized execution, no physical host affinity or Xbox timing claim.
    [[nodiscard]] bool bindVirtualCpuAffinity();
    // Consume only the well-formed debugger thread-name notification. No SEH,
    // C++ unwinding, host debugger, or logging of guest names is provided.
    [[nodiscard]] bool bindThreadDebugNaming();
    [[nodiscard]] std::optional<std::string> guestThreadDebugName(std::uint32_t token) const;
    [[nodiscard]] std::optional<std::uint32_t> guestThreadCriticalRegionDepth(std::uint32_t token) const;
    using ThreadWaitReady = std::function<bool()>;
    using ThreadWaitHandler = std::function<bool(const ThreadWaitReady&)>;
    // Invoked WITHOUT the registry mutex. May park under the serialized executor.
    // Readiness may be checked by another gated task and consumes a real signal.
    // The handler must not retain readiness after return/unwind. Reset after join.
    [[nodiscard]] bool bindThreadWaitHandler(ThreadWaitHandler handler);
    // Explicit serialized native FIFO policy. Waits park without spinning or
    // changing guest dispatcher headers/wait lists. The guest lock-count word
    // continues to describe recursion only, not host-side queued waiters.
    [[nodiscard]] bool bindNativeCriticalSectionWaits();
    // Registered waits, successful acquisitions, cancelled/rejected waits, pending.
    [[nodiscard]] std::array<std::uint64_t,4> nativeCriticalSectionWaitStatistics() const;
    using ThreadTimedWaitHandler = std::function<bool(const ThreadWaitReady&, std::chrono::steady_clock::time_point)>;
    [[nodiscard]] bool bindThreadTimedWaitHandler(ThreadTimedWaitHandler handler);
    using ThreadDelayHandler = std::function<bool(std::chrono::steady_clock::time_point)>;
    // Called without the registry mutex. Must yield the current slice even at
    // an elapsed deadline, retain no callbacks, and return only after expiry.
    [[nodiscard]] bool bindThreadDelayHandler(ThreadDelayHandler handler);
    // Requests, completed, cancelled, zero requests, total ticks, max ticks, pending.
    [[nodiscard]] std::array<std::uint64_t,7> threadDelayStatistics() const;
    static constexpr std::size_t MaximumNotificationListeners = 128;
    static constexpr std::size_t MaximumQueuedNotifications = 64;
    // Explicit event ingress. No fabricated startup/sign-in/controller events.
    // Post from the serialized host adapter; this does not wake an asynchronous reactor.
    [[nodiscard]] bool bindNotificationService();
    [[nodiscard]] bool postNotification(std::uint32_t id, std::uint32_t parameter);
    // Native overlay preference only; setting it does not create an overlay
    // or synthesize a notification. Empty until the title explicitly sets it.
    [[nodiscard]] std::optional<std::uint32_t> notificationUiPosition() const;
    // Explicit native policy until a real local profile service is installed.
    // All slots are signed out; this does not fabricate a user or Live login.
    [[nodiscard]] bool bindSignedOutUsers();
    // Standalone native launch: no payload from another Xbox title/dashboard.
    [[nodiscard]] bool bindNoLaunchData();
    // Identity comes from the already-mounted executable's verified XEX header.
    [[nodiscard]] bool bindMountedDisc(std::uint32_t xexHeader);
    // Native replacement for the SDK's handle-to-event conversion. Returned
    // address is an opaque owned reference, not an Xbox kernel structure.
    [[nodiscard]] std::optional<std::uint32_t> referenceNativeEvent(std::uint32_t handle);
    [[nodiscard]] std::array<std::uint32_t, 3> nativeDiscStatistics() const;
    // Explicit native namespace policy: Xbox raw HDD partitions are absent.
    // Never opens a host device or creates a synthetic disk handle.
    [[nodiscard]] bool bindNoRawStorageDevices();
    // Xbox language ID for the native application's language preference.
    // Independent of a signed-in user and of the host's geographic region.
    [[nodiscard]] bool bindUserLanguage(std::uint32_t language);
    [[nodiscard]] bool bindUserCountry(std::uint32_t country);
    using SystemTimeSource = std::function<std::optional<std::uint64_t>()>;
    // Source returns signed-range FILETIME ticks; never called under a guest scheduler wait.
    // Frequency must describe the same timebase used by translated mftb.
    [[nodiscard]] bool bindClockSources(SystemTimeSource source, std::uint32_t performanceFrequency);
    // Explicit software display configuration only; no monitor/GPU readiness claim.
    [[nodiscard]] bool bindVirtualVideoMode720p60();
    // Opt-in native scaler bandwidth hypothesis. Caller first establishes a
    // reversible parser-validated0x1C0 data binding to a dedicated500MHz word.
    [[nodiscard]] bool bindExperimentalNativeScalerTiming(std::uint32_t slot,std::uint32_t storage);
    [[nodiscard]] std::uint64_t nativeScalerTimingQueries() const;
    // Explicit one-shot native identity command producer. It returns generated
    // DWORD count only; submission remains guarded until an ordered consumer
    // accounts for the complete surrounding stream. Bind before graphics init.
    [[nodiscard]] bool bindExperimentalNativeIdentityScaler();
    struct NativeScalerCommandStatus {
        bool issued;
        std::uint32_t previousReadWord, words, issuerThread;
    };
    [[nodiscard]] NativeScalerCommandStatus nativeScalerCommandStatus() const;
    // Initialization only: the 28-byte big-endian DPC record, not a queue,
    // interrupt source, IRQL model or callback dispatch. Bind before workers.
    [[nodiscard]] bool bindDpcInitialization(std::function<bool(std::uint32_t)> callbackValidator);
    // Explicit native backend, with bounded image-pointer metadata and an exact
    // guest callback validator. Guest firmware and callbacks are not executed.
    [[nodiscard]] bool bindGraphicsBackend(std::shared_ptr<GuestGraphicsBackend> backend,
        std::uint32_t imageBase, std::uint32_t imageBytes,
        std::function<bool(std::uint32_t)> callbackValidator);
    [[nodiscard]] bool graphicsInitialized() const;
    struct GraphicsRingWriteBack { std::uint32_t physicalAddress, virtualAddress, blockSizeLog2; };
    struct GraphicsRingConfiguration {
        std::uint32_t physicalAddress, virtualAddress, bytes;
        std::uint32_t readWord{0};
        std::optional<GraphicsRingWriteBack> writeBack;
    };
    // Configuration only until a command processor is bound. No read-pointer
    // advance, guest-memory writeback or fake interrupt occurs during setup.
    [[nodiscard]] std::optional<GraphicsRingConfiguration> graphicsRingConfiguration() const;
    // Diagnostic read only. All guest writers must be stopped/joined before
    // calling: the registry mutex does not serialize generated direct stores.
    // Returns independent bytes from the still-owned/pinned ring, never a view.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> snapshotStoppedGraphicsRing() const;
    // Same stopped-writers precondition. Exact physical namespace, no masking
    // aliases or unowned flat-memory fallback. At most 4 MiB per snapshot.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> snapshotStoppedGraphicsPhysical(
        std::uint32_t physicalAddress, std::uint32_t byteCount) const;
    // All guest writers must be joined. Virtual heap namespace, not GPU
    // physical memory: one live allocation with every covered page committed.
    // Bounded owned copy only; does not pin the resource for future execution.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> snapshotStoppedGuestHeap(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const;
    // Stopped writers, CPU virtual address of one owned physical allocation.
    // Used by native resources whose object header contains a CPU pointer.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> snapshotStoppedGraphicsVirtual(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const;
    // Resource reads inside the current executor's exclusive, non-yielding
    // gate. Other guest writers must be parked. Same ownership validation as
    // stopped snapshots; copies do not grant a future lease on guest storage.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> copySerializedGuestHeap(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const;
    // API argument in a live heap allocation or the currently executing
    // worker's own stack. Same exclusive executor-gate requirement.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> copySerializedGuestArgument(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const;
    // Current bytes from the complete, validated title-image page map. Used for
    // mutable driver globals; initial decoded-image bytes are not live evidence.
    // Same exclusive executor-gate requirement; no arbitrary mapped-RAM fallback.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> copySerializedTitleImage(
        std::uint32_t address, std::uint32_t byteCount) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> copySerializedGraphicsData(
        std::uint32_t address, std::uint32_t byteCount) const;
    // Diagnostic attempt v32+ (defined only in local-analysis attempt registries): the two copies'
    // validation, returning the guest bytes in place (nullptr where the copy would refuse). The guest
    // mappings are permanent; the bytes may change like any guest read.
    [[nodiscard]] const std::uint8_t* viewSerializedGuestHeap(
        std::uint32_t virtualAddress, std::uint32_t byteCount) const;
    [[nodiscard]] const std::uint8_t* viewSerializedGraphicsData(
        std::uint32_t address, std::uint32_t byteCount) const;
    // Diagnostic attempt v42+ (defined only in attempt registries): the registry mutex, for the
    // attempt's typed registry sections (guest_registry_concurrency.h). Not for other callers.
    [[nodiscard]] std::mutex& registryMutex() const noexcept;
    // Publish completed native resource work into one still-owned writable
    // graphics allocation, only if its complete expected bytes are unchanged.
    // Caller holds the executor gate through capture, GPU work and publication.
    [[nodiscard]] bool publishSerializedGraphicsData(std::uint32_t virtualAddress,
        const std::vector<std::uint8_t>& expected,const std::vector<std::uint8_t>& completed);
    struct SerializedGraphicsUpdate {
        std::uint32_t virtualAddress;
        const std::vector<std::uint8_t>* expected;
        const std::vector<std::uint8_t>* completed;
    };
    // Same executor-gate/GPU-completion preconditions as the single update.
    // Revalidate every CURRENT allocation and expected byte before any write.
    // Ordered publication supports pixels -> cursor -> resource fence without
    // partially publishing when a later destination is stale/read-only. This
    // does not update the command processor's ring state or discharge PM4.
    [[nodiscard]] bool publishSerializedGraphicsBatch(const SerializedGraphicsUpdate* updates,std::size_t count);
    template<std::size_t N>[[nodiscard]] bool publishSerializedGraphicsBatch(const std::array<SerializedGraphicsUpdate,N>& updates) {
        return publishSerializedGraphicsBatch(updates.data(),updates.size());
    }
    struct StoppedPhysicalOwnership {
        std::uint32_t physicalBase,virtualBase,byteCount,protection;
        bool writable;
    };
    // Metadata only, while ALL guest writers are stopped. This neither reads
    // payload bytes nor pins/reserves ownership for a later GPU transaction.
    // Canonical physical namespace only; no masking a noncanonical address.
    [[nodiscard]] std::optional<StoppedPhysicalOwnership> stoppedGraphicsPhysicalOwnership(
        std::uint32_t physicalAddress,std::uint32_t byteCount) const;
    // Read-only diagnostic of one canonical physical writable word. Does not
    // pin the allocation, validate command aliases or authorize a later write.
    // All guest writers must be stopped; future execution must revalidate.
    [[nodiscard]] bool stoppedGraphicsWordIsWritable(std::uint32_t physicalAddress) const;
    [[nodiscard]] bool stoppedGraphicsWordAliasesIdentifier(std::uint32_t physicalAddress) const;
    // Read-only diagnostic, with ALL guest writers stopped/joined or parked by
    // the serialized execution token for the entire call. Resolves current
    // owners and compares root/child/RPTR/source bytes under one lifecycle lock.
    // Inspector receives only the owned frontend capture, never guest pointers;
    // it must not reenter the registry, run guest/GPU work, publish or retain
    // borrowed views. Acceptance does NOT execute/retire a command or create a
    // lease usable after this method returns. Initial bins are caller evidence.
    [[nodiscard]] bool inspectStoppedNativeColorBatch(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& inspector) const;
    // Read-only inspection AFTER completed display output and its fresh command
    // coherency. All workers must be stopped; retains the real allocation lock.
    // Does not execute, publish, replace the scanout lease or advance RPTR.
    [[nodiscard]] bool inspectStoppedPostOutputColorBatch(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& inspector) const;
    // Separate post-stop GPU diagnostic callback while holding real owners.
    // Callback may submit independent native resources, NEVER guest writes or
    // live state publication. All guest workers must have joined first.
    [[nodiscard]] bool runStoppedPostOutputColorDiagnostic(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& backend) const;
    // Same stopped-writer precondition, but transfers independent owned bytes
    // after the lifecycle-held inspection. No guest pointers or allocation lease
    // escape. The result may drive post-stop diagnostics, NEVER later guest
    // publication without an entirely new live ownership transaction.
    [[nodiscard]] std::unique_ptr<recomp::gpu::NativeColorBatchCapture> snapshotStoppedNativeColorBatch(
        const recomp::gpu::NativeColorChainProgram&,const recomp::gpu::DrawBinState&,std::uint32_t writeWord) const;
    // Explicit post-stop backend seam, separate from the read-only inspector.
    // Caller has joined every guest writer. Hold the actual allocation lifecycle
    // lock through the synchronous native diagnostic/fence/audit callback.
    // Callback MUST NOT reenter this registry/guest execution, write guest RAM,
    // or retain borrowed capture views. No output mapping/lease escapes; no
    // guest status, journal or RPTR is published even on success.
    [[nodiscard]] bool runStoppedNativeColorDiagnostic(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& backend) const;
    // Opaque registration: no identifier payload is read or written here.
    [[nodiscard]] std::optional<std::uint32_t> graphicsSystemIdentifierAddress() const;
    [[nodiscard]] bool graphicsMeInitVerified() const;
    [[nodiscard]] bool bindNativeInitialCoherencySubmission();
    // Called only while the invoking thread owns the serialized guest-execution
    // token and all other guest writers are parked. Holds the lifecycle lock;
    // backend must not reenter guest execution or this registry. One exact cold
    // submission and, after explicit retirement/reconfiguration, one separately
    // fenced warm submission; historical completions never authorize new work.
    [[nodiscard]] bool submitNativeInitialCoherency(std::uint32_t writeWord);
    [[nodiscard]] std::optional<NativeInitialCoherency> completedNativeInitialCoherency() const;
    [[nodiscard]] bool bindExperimentalNativeInitialization();
    // Same serialized-writers and non-reentry preconditions as the cold path.
    // This method snapshots/validates ALL input and destination ownership before
    // native work, audits ALL returned bytes, then publishes the whole result.
    [[nodiscard]] bool submitExperimentalNativeInitialization(std::uint32_t writeWord);
    struct NativeInitializationCompletion { std::uint32_t readWord, draws, words, registers; };
    [[nodiscard]] std::optional<NativeInitializationCompletion> completedExperimentalNativeInitialization() const;
    [[nodiscard]] bool bindEmptyGuestCacheCoherency();
    [[nodiscard]] bool submitEmptyGuestCacheCoherency(std::uint32_t writeWord);
    struct EmptyCacheCoherencyCompletion { std::uint32_t batches, readWord; NativeInitialCoherency request; };
    [[nodiscard]] std::optional<EmptyCacheCoherencyCompletion> completedEmptyGuestCacheCoherency() const;
    [[nodiscard]] std::optional<EmptyCacheCoherencyCompletion> completedWarmEmptyCacheCoherency() const;
    bool bindNativeStateBatches();
    [[nodiscard]] bool bindExperimentalIdleCachePair();
    [[nodiscard]] std::uint64_t experimentalIdleCachePairs() const;
    bool submitNativeStateBatch(std::uint32_t writeWord);
    // Dedicated proved maintenance executor, not side-effect-free acknowledgement.
    // Successful batches join the shared state-batch RPTR chain and separately
    // count their real point work. Requires the explicit idle/cache experiment.
    bool submitNativeMaintenancePoints(std::uint32_t writeWord);
    // Fallback for the same ring position/ownership chain as
    // submitNativeStateBatch, for a batch too large or too opcode-varied to be
    // a state batch (real draws/shader loads/nested indirect buffers). Verifies
    // - using real decoded register/packet content, never guessed - that the
    // whole batch cannot produce any persistent, CPU-visible effect (see
    // xenos_side_effect_free_batch.h), then advances the ring without
    // executing a single draw. Fails closed on anything it cannot prove safe.
    [[nodiscard]] bool submitVerifiedSideEffectFreeBatch(std::uint32_t writeWord);
    struct SideEffectFreeBatchCompletion { std::uint32_t batches, readWord, draws, indirectBuffers; };
    [[nodiscard]] std::optional<SideEffectFreeBatchCompletion> completedSideEffectFreeBatches() const;
    [[nodiscard]] std::optional<SideEffectFreeBatchCompletion> completedWarmSideEffectFreeBatches() const;
    [[nodiscard]] bool bindExperimentalOwnedColorBatch();
    // One exact admitted four-draw profile on the warm ring. Caller owns the
    // serialized guest-execution token and parks all other writers throughout.
    // Real allocation ownership spans capture, GPU work, fresh publication
    // checks and nonthrowing bookkeeping; no borrowed mapping escapes.
    [[nodiscard]] bool submitExperimentalOwnedColorBatch(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord);
    struct NativeOwnedColorCompletion {
        std::uint32_t previousWord,readWord,draws,eventWords,copyBytes,edramBytes,nativeFences,policy;
    };
    [[nodiscard]] std::optional<NativeOwnedColorCompletion> completedNativeOwnedColorBatch() const;
    // Single bounded post-output batch. Keeps the original displayed snapshot
    // and receipts, installs a separate pending copy, does NOT present it.
    [[nodiscard]] bool submitExperimentalPostOutputColorBatch(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord);
    struct NativePostOutputColorCompletion { NativeOwnedColorCompletion color;std::uint32_t leadingSetupFences; };
    [[nodiscard]] std::optional<NativePostOutputColorCompletion> completedNativePostOutputColorBatch() const;
    // Attempted, exact admission/publication refusal, physical range (if any).
    [[nodiscard]] std::array<std::uint32_t,3> nativePostOutputColorStatus() const;
    // PREFIX ONLY: true does not retire the combined IB or present an image.
    // Caller must hold the serialized guest execution token with every other
    // writer parked; no guest reentry is permitted during this synchronous call.
    [[nodiscard]] bool executeExperimentalDeferredColorPrefix(const recomp::gpu::NativeColorChainProgram&,
        const recomp::gpu::DrawBinState&,std::uint32_t writeWord);
    struct NativeDeferredColorPrefixCompletion {
        std::uint32_t previousWord,pendingWriteWord,prefixWords,inputCount,copyBytes,nativeFences,leadingSetupFences;
    };
    [[nodiscard]] std::optional<NativeDeferredColorPrefixCompletion> completedNativeDeferredColorPrefix() const;
    [[nodiscard]] std::array<std::uint32_t,3> nativeDeferredColorPrefixStatus() const;
    // Explicit native platform policy: no console dashboard/system overlay is
    // scheduled. Returns one empty system context after the first color batch;
    // does NOT complete a swap, write the GPU identifier or retire ring words.
    [[nodiscard]] bool bindNativeNoSystemOverlay();
    struct NativeSystemContext {
        std::uint32_t stackPointer, buffer, identifierOutput, readWord, policy;
    };
    [[nodiscard]] std::optional<NativeSystemContext> nativeSystemContext() const;
    [[nodiscard]] std::optional<NativeSystemContext> nativePostOutputSystemContext() const;
    [[nodiscard]] std::optional<NativeSystemContext> nativePostSwapSystemContext() const;
    struct StoppedNativeSwapRequest {
        std::uint32_t commandVirtual, commandPhysical, allocationBase, allocationBytes, protection;
        std::uint32_t frontbufferVirtual, format, colorSpace, width, height, identifierWord;
        std::array<std::uint32_t,6> fetch;
    };
    // Read-only metadata after ALL guest writers join. Requires the previously
    // issued no-overlay context and still-owned caller stack/command storage.
    // Does not decode/display pixels, publish commands, or create a future lease.
    [[nodiscard]] std::optional<StoppedNativeSwapRequest> inspectStoppedNativeSwapRequest(const GuestImportCall&) const;
    // Separate post-output phase; original context/image receipts stay intact.
    // These diagnostics neither issue a command nor grant a new scanout lease.
    [[nodiscard]] std::optional<StoppedNativeSwapRequest> inspectStoppedPostOutputSwapRequest(const GuestImportCall&) const;
    // Raw request metadata after the separately completed second swap. Does
    // not validate or decode the next requested frontbuffer, nor issue ticket3.
    [[nodiscard]] std::optional<StoppedNativeSwapRequest> inspectStoppedPostSwapRequest(const GuestImportCall&) const;
    // A separate deferred request after the second swap. Owns metadata and
    // pins allocations, but has NO decoded pixels or rendering completion.
    // Existing first/second-swap consumers cannot execute ticket3.
    struct NativeDeferredSwapRequest {
        StoppedNativeSwapRequest request;
        StoppedPhysicalOwnership imageOwner;
        std::uint32_t imagePhysical,issuedReadWord;
    };
    [[nodiscard]] std::optional<NativeDeferredSwapRequest> nativeDeferredSwapRequest() const;
    struct NativeOwnedSwapImage {
        StoppedNativeSwapRequest request;
        StoppedPhysicalOwnership owner;
        std::uint32_t physicalAddress;
        std::vector<std::uint8_t> rgba;
    };
    // Read-only bridge toward a native swap consumer, NOT a VdSwap handler.
    // Caller parks ALL guest writers (or joins them after stopping). Holds the
    // allocation lock through validation/decode/visitor. Requires the exact
    // still-owned image and active bytes published by the completed color batch.
    // Visitor must not reenter the registry or resume guests. Owned RGBA may be
    // copied; no borrowed guest view/future lease/command completion escapes.
    [[nodiscard]] bool visitNativeOwnedSwapImage(const GuestImportCall&,
        const std::function<bool(const NativeOwnedSwapImage&)>&) const;
    [[nodiscard]] bool visitStoppedPostOutputSwapImage(const GuestImportCall&,
        const std::function<bool(const NativeOwnedSwapImage&)>&) const;
    // Native-only experimental producer/consumer. VdSwap stays NotRegistered
    // unless this explicit consumer is bound before graphics initialization.
    // Bind before graphics initialization. Consumer runs under the lifecycle
    // lock with all guest writers parked. True means real audited native upload
    // and QueuePresent acceptance, not vblank; false/throw must not acknowledge
    // work. No reentry or borrowed views retained. One isolated64-word command
    // in one exact root IB only; mixed streams remain guarded.
    [[nodiscard]] bool bindExperimentalNativeSwapCommands(std::function<bool(const NativeOwnedSwapImage&)>);
    // After the explicit live post-output color phase, may issue ONE separate
    // ticket2. Original executors do not consume it; no new completion implied.
    [[nodiscard]] bool issueExperimentalNativeSwapCommand(const GuestImportCall&);
    [[nodiscard]] bool submitExperimentalNativeSwapCommand(std::uint32_t writeWord);
    // One exact command-allocation visibility batch before the pending swap.
    // No guest command upload/status-bit write; native ordering must complete.
    [[nodiscard]] bool submitNativeCommandStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] bool submitNativePostOutputSwapStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] bool submitNativeDeferredSwapStorageCoherency(std::uint32_t writeWord);
    [[nodiscard]] bool submitNativeScalerStorageCoherency(std::uint32_t writeWord);
    struct NativeScalerStorageCompletion {
        std::uint32_t previousWord,readWord,commandPhysical;
        NativeInitialCoherency request;
    };
    [[nodiscard]] std::optional<NativeScalerStorageCompletion> completedNativeScalerStorageCoherency() const;
    // Exact90word ordered wrapper only, after the command-storage fence.
    // Entire command/events/owners preflighted before GPU work. RPTR last.
    [[nodiscard]] bool submitNativeWrappedSwapCommand(std::uint32_t writeWord);
    struct NativeWrappedSwapCompletion {
        std::uint32_t previousWord,readWord,childPhysical;
        std::array<std::uint32_t,4> eventPhysical;
        recomp::gpu::NativeSwapGpuResult gpu;
    };
    [[nodiscard]] std::optional<NativeWrappedSwapCompletion> completedNativeWrappedSwap() const;
    struct NativePostOutputWrappedSwapCompletion {
        std::uint32_t previousWord,readWord,childPhysical;
        std::array<std::uint32_t,4> eventPhysical;
        recomp::gpu::NativePostOutputSwapGpuResult gpu;
        StoppedNativeSwapRequest imageRequest;
        StoppedPhysicalOwnership imageOwner;
    };
    // Exact129word transaction. Caller has preallocated a fresh IRQ worker and
    // holds the exclusive executor gate. Backend IRQ callback runs WITHOUT the
    // registry mutex; callback must run scope/begin/original guest/ack, mark the
    // worker returned, then resume the original issuer. Never run peer writers.
    [[nodiscard]] bool submitNativePostOutputWrappedSwapCommand(std::uint32_t writeWord,
        const GuestGraphicsBackend::NativePostOutputSwapInterrupt& interrupt);
    [[nodiscard]] std::optional<NativePostOutputWrappedSwapCompletion> completedNativePostOutputWrappedSwap() const;
    struct NativeDeferredWrappedSwapCompletion {
        std::uint32_t previousWord,readWord,childPhysical,childWords;
        std::array<std::uint32_t,4> eventPhysical;
        recomp::gpu::NativeDeferredSwapGpuResult gpu;
        NativeDeferredSwapRequest issuedRequest;
    };
    // Completes only the remaining tail of the retained whole prefix request.
    // Exclusive executor gate and freshly prepared third worker required.
    // Present once, original IRQ, audited events, then RPTR last. No prefix retry.
    [[nodiscard]] bool submitNativeDeferredWrappedSwapCommand(std::uint32_t writeWord,
        const GuestGraphicsBackend::NativeDeferredSwapInterrupt& interrupt);
    [[nodiscard]] std::optional<NativeDeferredWrappedSwapCompletion> completedNativeDeferredWrappedSwap() const;
    struct NativeCommandStorageCompletion {
        std::uint32_t previousWord,readWord;
        NativeInitialCoherency request;
    };
    [[nodiscard]] std::optional<NativeCommandStorageCompletion> completedNativeCommandStorageCoherency() const;
    [[nodiscard]] std::optional<NativeCommandStorageCompletion> completedNativePostOutputSwapStorageCoherency() const;
    [[nodiscard]] std::optional<NativeCommandStorageCompletion> completedNativeDeferredSwapStorageCoherency() const;
    struct NativePendingSubmission {
        std::uint32_t rootPhysical,childPhysical,childWords,commandOffsetWords,readWord,writeWord;
    };
    // Stopped-only metadata, caller must join all guest writers. An owned child
    // containing the immutable native ticket is NOT executable PM4 evidence.
    [[nodiscard]] std::optional<NativePendingSubmission> inspectStoppedNativePendingSubmission(std::uint32_t writeWord) const;
    [[nodiscard]] std::optional<NativePendingSubmission> inspectStoppedPostOutputSwapSubmission(std::uint32_t writeWord) const;
    [[nodiscard]] std::optional<NativePendingSubmission> inspectStoppedDeferredSwapSubmission(std::uint32_t writeWord) const;
    template<class Wrapper> struct StoppedSurfaceSwapInterrupt {
        Wrapper wrapper;
        NativePendingSubmission submission;
        std::uint32_t device,handler;
        std::uint64_t frequency;
        // Only reviewed callback/IRQ fields. The ring contains16 page/counter
        // pairs; these are diagnostic values, not a lease or completed IRQ.
        static constexpr std::array<unsigned,14> DeviceOffsets{
            11028,11032,16944,16952,16956,16960,16972,16976,17112,17120,21964,23788,17108,16948};
        std::array<std::uint32_t,14> deviceValues{};
        std::array<std::uint32_t,32> queuedSurfaces{};
        std::array<std::uint32_t,8> scratch{};
    };
    using StoppedPostOutputSwapInterrupt=StoppedSurfaceSwapInterrupt<recomp::gpu::NativePostOutputSwapWrapper>;
    struct StoppedDeferredSwapInterrupt : StoppedSurfaceSwapInterrupt<recomp::gpu::NativeDeferredSwapWrapper> {
        std::array<std::uint32_t,6> pacingRecord{};
    };
    // Current ticket3 state AFTER the live prefix only. Owned read-only values,
    // not old ticket2 admission, a future IRQ lease or permission to dispatch.
    [[nodiscard]] std::optional<StoppedDeferredSwapInterrupt> inspectStoppedDeferredSwapInterrupt(
        std::uint32_t writeWord,std::uint32_t* rejection=nullptr) const;
    [[nodiscard]] std::optional<StoppedPostOutputSwapInterrupt> inspectStoppedPostOutputSwapInterrupt(
        std::uint32_t writeWord,std::uint32_t* rejection=nullptr) const;
    // Stronger stopped-only candidate for BEFORE native presentation: adds the
    // reviewed queued callback profile and retained scratch configuration. Still
    // no lease, allocation, guest execution or permission to retire a wrapper.
    // Reasons1..21 are the raw inspector;22 profile,23 leaf,24 native scratch.
    [[nodiscard]] std::optional<StoppedPostOutputSwapInterrupt> inspectStoppedPostOutputSwapQueuedInterrupt(
        std::uint32_t writeWord,std::uint32_t* rejection=nullptr) const;
    // Same stopped-only read-only metadata, for the copied scaler ticket after
    // its separate command-storage fence. Unknown wrapper words NOT executed.
    [[nodiscard]] std::optional<NativePendingSubmission> inspectStoppedNativeScalerSubmission(std::uint32_t writeWord) const;
    // Full owned request plan only. All writers must be joined; no callback,
    // memory write, GPU operation or retirement is authorized by inspection.
    [[nodiscard]] std::optional<recomp::gpu::NativeScalerWrapper> inspectStoppedNativeScalerWrapper(std::uint32_t writeWord) const;
    struct NativeSwapCommandStatus {
        bool issued,pending,presentQueued;
        std::uint32_t commandPhysical,previousWord,readWord,issuedReadWord;
    };
    [[nodiscard]] NativeSwapCommandStatus nativeSwapCommandStatus() const;
    [[nodiscard]] NativeSwapCommandStatus nativePostOutputSwapStatus() const;
    struct NativeDisplayModeStatus { bool selected; std::uint32_t flags,queries; };
    [[nodiscard]] NativeDisplayModeStatus nativeDisplayModeStatus() const;
    struct NativeStateBatchCompletion {
        std::uint32_t batches, readWord, words, stateWrites, invalidations;
        std::uint32_t maintenanceBatches=0, maintenanceDraws=0;
    };
    [[nodiscard]] std::optional<NativeStateBatchCompletion> completedNativeStateBatches() const;
    [[nodiscard]] std::optional<NativeStateBatchCompletion> completedWarmStateBatches() const;
    // One retired startup ring; warm ME_INIT completion is separate from its
    // historical draw/cache/state records. Later-generation batches still guard.
    [[nodiscard]] std::optional<std::uint32_t> retiredGraphicsRingReadWord() const;
    [[nodiscard]] std::optional<NativeInitialCoherency> completedWarmRingCoherency() const;
    // Explicit native no-Xenos-link policy, bound before execution to the actual
    // initialized exported lock. It does not implement guest EDRAM resources.
    [[nodiscard]] bool bindNativeEdramTrainingPolicy(std::uint32_t calibrationLock);
    struct NativeEdramTrainingState { std::uint32_t workerChecks, noCommandQueries, statusQueries, directNoCommandQueries; };
    [[nodiscard]] NativeEdramTrainingState nativeEdramTrainingState() const;
    struct GraphicsInterruptRegistration { std::uint32_t routine, userData; };
    [[nodiscard]] std::optional<GraphicsInterruptRegistration> graphicsInterruptRegistration() const;
    // Registration metadata only. Title exit remains guarded: diagnostic
    // cancellation/reset must never invoke these guest callbacks.
    struct TitleTerminationRegistration { std::uint32_t record, routine, priority; };
    static constexpr std::size_t MaximumTitleTerminationRegistrations = 64;
    [[nodiscard]] bool bindTitleTerminationNotifications(std::uint32_t imageBase,
        std::uint32_t imageBytes, std::function<bool(std::uint32_t)> callbackValidator);
    [[nodiscard]] std::vector<TitleTerminationRegistration> titleTerminationRegistrations() const;
    static constexpr std::uint32_t NativeModuleRecordSize = 0x64;
    [[nodiscard]] bool bindNativeXamModule(std::uint32_t address, std::uint32_t titleModule,
            const std::vector<GuestThreadMemoryRange>& excluded, std::uint32_t compatibilityVersion = 0);
    // Configuration/version negotiation only: no sockets, DNS, or traffic.
    [[nodiscard]] bool bindNetworkLifecycle();
    using FileMetadataQuery = std::function<GuestFileQueryResult(std::string_view)>;
    // Pure metadata lookup under the registry mutex. No host I/O, reentry,
    // guest execution, retained path views, or scheduler switching.
    [[nodiscard]] bool bindFileMetadataQuery(FileMetadataQuery query);
    using ReadOnlyFileOpen = std::function<std::shared_ptr<const GuestReadOnlyFile>(std::string_view)>;
    // Return an already-open explicit source only; no path-based host opens,
    // guest execution or registry reentry inside this callback.
    [[nodiscard]] bool bindReadOnlyFiles(ReadOnlyFileOpen open);
    // Bounded transactional request staging; providers retain the 1MiB call cap.
    static constexpr std::uint32_t MaximumFileReadBytes = 16 * 1024 * 1024;
    static constexpr std::uint32_t FileReadChunkBytes = 1024 * 1024;
    struct FileIoStatistics { std::uint64_t opens{0}, reads{0}, bytes{0}, seeks{0}; };
    [[nodiscard]] FileIoStatistics fileIoStatistics() const;
    struct FileCreateRequest {
        std::uint32_t access, attributes, share, disposition, options;
        // 0 unchecked, 1 found, 2 absent in mounted snapshot, 3 unmounted.
        std::uint32_t lookup{0};
        bool mountedContent{false};
        std::string path{}; // Bounded guest path (raw if normalization fails).
        std::uint32_t objectRoot{0}, objectFlags{0};
    };
    [[nodiscard]] std::optional<FileCreateRequest> lastFileCreateRequest() const;
    // Copies a validated snapshot once; title must match the loader's verified
    // executable metadata. Reset only after all guest workers have joined.
    [[nodiscard]] bool bindInstalledContent(const GuestContentCatalog& catalog, std::uint32_t title);
    using ContentFiles = std::map<std::string, std::shared_ptr<const GuestReadOnlyFile>>;
    // All regular files in an explicit content:\ snapshot must already be open.
    // Index is one-based in the installed catalog. This does not mount a root.
    [[nodiscard]] bool bindContentPackage(std::uint32_t index, const GuestFileMetadataCatalog& metadata, const ContentFiles& files);
    // Redacted policy failures only: root=1,user=2,flags=4,license=8,cache=16,
    // content size=32,overlapped=64,unbound catalog=128,unbound package=256.
    // Root features: starts with digit=512, trailing colon=1024, slash=2048, empty=4096.
    [[nodiscard]] std::uint32_t contentMountRejection() const;

    [[nodiscard]] bool isReady() const;
    [[nodiscard]] GuestImportDispatchResult dispatch(
            std::string_view importName,
            GuestImportCall& call);

    [[nodiscard]] static const std::array<GuestImportDescriptor, 137>&
    implementedBootImports() noexcept;
    [[nodiscard]] static bool isImplemented(std::string_view importName) noexcept;
    // Snapshot of the last successfully published 104-byte native-arena query.
    [[nodiscard]] std::optional<std::array<std::uint32_t, 26>> lastMemoryStatistics() const;
    // Stop-only diagnostic: header/signal/validated, never a wait or signal.
    // Missing means the owned bounded header could not be read, not zero state.
    [[nodiscard]] std::optional<std::array<std::uint32_t, 3>> inspectDispatcherEvent(std::uint32_t address) const;
    // Stop-only snapshot at the exact Enter rejection, before executor unwind.
    // reason,address,currentToken,registered,registryOwner,registryRecursion,
    // liveWordsRead,seven live words,rangeAvailable,storageMatches. No writes.
    [[nodiscard]] std::optional<std::array<std::uint32_t,16>> lastCriticalSectionRejection() const;

private:
    bool visitStoppedNativeColorBatch(const recomp::gpu::NativeColorChainProgram&,const recomp::gpu::DrawBinState&,
        std::uint32_t,const std::function<bool(recomp::gpu::NativeColorBatchCapture&&)>&) const;
    // Private common capture/owner resolver. Every caller already holds mutex_
    // and parks/joins all guest writers. Memory views last only through visitor.
    bool visitNativeColorBatchLocked(const recomp::gpu::NativeColorChainProgram&,const recomp::gpu::DrawBinState&,
        std::uint32_t,const std::function<bool(recomp::gpu::NativeColorBatchCapture&&,const recomp::gpu::NativeColorResolvedMemory&)>&,
        bool postOutput=false) const;
    [[nodiscard]] std::optional<GuestPreparedThread> createNativeCallbackWorkerLocked(
        std::uint32_t callback,std::uint32_t context,const NativeCallbackEnqueue& enqueue);
    [[nodiscard]] std::optional<GuestPreparedThread> createNativeWorkerLocked(
        std::uint32_t callback,std::uint32_t context,bool wrapped,std::uint32_t cpu,const NativeCallbackEnqueue& enqueue);
    [[nodiscard]] GuestImportDispatchResult dispatchNativeInterruptSpinLock(std::string_view name,GuestImportCall& call);
    [[nodiscard]] bool validateNativeInterruptScopeLocked();
    struct NativeInterruptScope {
        std::uint32_t worker,pcr,device,allocationBase,allocationBytes;
        std::array<std::uint32_t,2> locks;
        std::uint32_t held=0; // At most one observed lock held; no recursive/nested spins.
        std::array<std::uint32_t,2> pairs{};
        bool postOutputSwap=false;
        bool deferredSwap=false; // Mutually exclusive with postOutputSwap.
    };
    std::optional<NativeInterruptScope> nativeInterruptScope_;
    struct NativeTileLockScope {
        std::uint32_t worker,device,allocationBase,allocationBytes;
        bool held=false,failed=false;std::uint32_t pairs=0;
    };
    std::optional<NativeTileLockScope> nativeTileLockScope_;
    NativeTileLockScopeStatus nativeTileLockScopeStatus_{};
    [[nodiscard]] bool validateNativeTileLockScopeLocked();
    [[nodiscard]] GuestImportDispatchResult dispatchNativeTileSpinLock(std::string_view,GuestImportCall&);
    struct NativeTileSceneInterruptScope {
        std::uint32_t worker=0,pcr=0,device=0,allocationBase=0,allocationBytes=0;
        std::uint32_t scratch=0,scratchPhysical=0,scratchOwner=0,contextPhysical=0,contextOwner=0;
        std::uint32_t event=0,pairs=0,events=0;bool held=false,failed=false;
        std::vector<std::uint8_t> expectedDevice,pcrBefore;
        std::array<std::uint8_t,32> scratchBefore{};
    };
    std::optional<NativeTileSceneInterruptScope> nativeTileSceneInterruptScope_;
    NativeTileSceneInterruptStatus nativeTileSceneInterruptStatus_{};
    struct NativeTileSceneWorker {std::uint32_t token,device,context;};
    std::optional<NativeTileSceneWorker> nativeTileSceneWorker_;
    std::uint32_t retiredNativeTileInitialWorker_{0};
    std::uint32_t retiredNativeTileFirstWorker_{0};
    std::uint32_t retiredNativeTileFinalWorker_{0};
    [[nodiscard]] bool retireNativeTileReturnedQueue(const recomp::gpu::Gta5NativeTileSubmissionCapture&,
        const recomp::gpu::Gta5TileCommandInterrupt&,const NativeSerializedGate&,
        const recomp::gpu::Gta5TileDeferredTail* =nullptr);
    [[nodiscard]] bool validateNativeTileSceneInterruptScopeLocked();
    [[nodiscard]] GuestImportDispatchResult dispatchNativeTileSceneInterrupt(std::string_view,GuestImportCall&);
    NativeCommandInterruptScopeStatus nativeInterruptScopeStatus_{};
    NativePostOutputSwapInterruptScopeStatus postOutputSwapInterruptScopeStatus_{};
    std::optional<StoppedPostOutputSwapInterrupt> postOutputSwapInterruptSnapshot_;
    std::optional<recomp::gpu::NativePostOutputSwapInterruptSequence> postOutputSwapInterruptSequence_;
    std::optional<std::array<std::uint32_t,6>> postOutputSwapPacingRecord_;
    NativeDeferredSwapInterruptScopeStatus deferredSwapInterruptScopeStatus_{};
    std::optional<StoppedDeferredSwapInterrupt> deferredSwapInterruptSnapshot_;
    std::optional<recomp::gpu::NativeDeferredSwapInterruptSequence> deferredSwapInterruptSequence_;
    std::optional<std::array<std::uint32_t,6>> deferredSwapPacingRecord_;
    [[nodiscard]] std::uint32_t deferredSwapQueuedInterruptRejectionLocked(const StoppedDeferredSwapInterrupt&) const;
    [[nodiscard]] bool validateNativeDeferredSwapInterruptPlanLocked(std::uint32_t writeWord) const;
    [[nodiscard]] bool validateNativeDeferredSwapGuestResultLocked(std::uint32_t writeWord,bool poisoned) const;
    [[nodiscard]] bool nativeDeferredSwapInterruptActionLocked(std::uint32_t writeWord,
        const recomp::gpu::NativeDeferredSwapWrapper::Operation& operation,
        const recomp::gpu::NativeDeferredSwapInterruptSequence::Action& nativeCommand);
    [[nodiscard]] bool validateNativePostOutputSwapInterruptPlanLocked(std::uint32_t writeWord) const;
    [[nodiscard]] bool validateNativePostOutputSwapGuestResultLocked(std::uint32_t writeWord,bool poisoned) const;
    [[nodiscard]] bool nativePostOutputSwapInterruptActionLocked(std::uint32_t writeWord,
        const recomp::gpu::NativePostOutputSwapWrapper::Operation& operation,
        const recomp::gpu::NativePostOutputSwapInterruptSequence::Action& nativeCommand);
    NativeScalerInterruptStatus nativeScalerInterruptStatus_{};
    bool nativeScalerTailAttempted_=false;
    std::optional<NativeScalerCompletion> completedNativeScaler_;
    bool nativePostScalerStorageFailed_=false;
    std::optional<NativePostScalerStorageCompletion> completedPostScalerStorage_;
    bool nativePostDisplayStorageFailed_=false;
    bool nativeDisplayOutputAttempted_=false;
    std::optional<NativeDisplayOutputCompletion> completedDisplayOutput_;
    std::optional<NativePostScalerStorageCompletion> completedPostDisplayStorage_;
    bool nativePostOutputStorageFailed_=false;
    std::optional<NativePostScalerStorageCompletion> completedPostOutputStorage_;
    bool nativeRuntimeCommandStorageFailed_=false;
    std::optional<NativePostScalerStorageCompletion> completedRuntimeCommandStorage_;
    bool nativeRuntimeMaintenanceFailed_=false;
    std::optional<NativeRuntimeMaintenanceCompletion> completedRuntimeMaintenance_;
    NativeRuntimeMaintenanceDiagnostic runtimeMaintenanceDiagnostic_;
    enum class NativeStoragePhase { Scaler, Display, Output, Runtime };
    bool submitNativeStorageCoherencyLocked(std::uint32_t writeWord,NativeStoragePhase phase);
    bool nativeDisplayGammaAttempted_=false;
    std::optional<NativeDisplayGammaCompletion> completedDisplayGamma_;
    std::function<bool(std::uint32_t)> nativeInterruptDeviceRoot_;
    std::optional<recomp::gpu::NativeScalerWrapper> nativeScalerInterruptPlan_;
    std::optional<StoppedPhysicalOwnership> nativeScalerInterruptScratchOwner_;
    std::array<std::uint32_t,8> nativeScalerInterruptScratchBefore_{};
    [[nodiscard]] bool nativeScalerInterruptMemoryActionLocked(const recomp::gpu::NativeScalerWrapper::Operation&);
    [[nodiscard]] bool validateNativeScalerInterruptPlanLocked(std::uint32_t writeWord) const;
    [[nodiscard]] bool validGuestDataBuffer(std::uint32_t address,std::uint32_t bytes,bool writable,
                                          std::uint32_t alignment=4) const;
    [[nodiscard]] GuestImportDispatchResult dispatchAudioRegister(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchAudioSubmit(GuestImportCall& call);
    std::shared_ptr<audio::NativeAudioRenderClient> audioRenderClient_;
    NativeCallbackEnqueue audioCallbackEnqueue_;
    std::optional<std::uint32_t> nativeCategoryOneGainBits_;
    std::uint64_t nativeCategoryOneGainQueries_{0};
    [[nodiscard]] GuestImportDispatchResult dispatchKeInitializeDpc(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchGraphicsDisplayNotification(GuestImportCall& call);
    [[nodiscard]] bool readDispatcherEvent(std::uint32_t address, std::uint32_t& header,
            std::uint32_t& signal, bool* observed = nullptr) const;
    [[nodiscard]] bool hasDispatcherWaitInRange(std::uint32_t address, std::uint64_t size) const;
    [[nodiscard]] GuestImportDispatchResult dispatchKeEvent(GuestImportCall& call, bool set);
    [[nodiscard]] GuestImportDispatchResult dispatchKeWaitForSingleObject(
            GuestImportCall& call, std::unique_lock<std::mutex>& lock);
    [[nodiscard]] GuestImportDispatchResult dispatchKeDelayExecutionThread(
            GuestImportCall& call, std::unique_lock<std::mutex>& lock);
    [[nodiscard]] GuestImportDispatchResult dispatchKeWaitForMultipleObjects(
            GuestImportCall& call, std::unique_lock<std::mutex>& lock);
    [[nodiscard]] GuestImportDispatchResult dispatchGraphicsRing(GuestImportCall& call, bool writeBack);
    struct PhysicalRange { std::uint32_t virtualAddress, allocationBase; };
    const std::uint8_t* viewSerializedGraphicsDataLocked(std::uint32_t address, std::uint32_t byteCount) const; // attempt v32+
    // Attempt v42+: copy of the read-validation state (caller holds R), and the snapshot fast path
    // for the heap (graphics=false) and graphics (graphics=true) views; nullptr means "use R".
    void captureValidationSnapshotLocked(GuestRegistryValidationSnapshot& snapshot) const;
    [[nodiscard]] const std::uint8_t* fastValidatedView(std::uint32_t address, std::uint32_t byteCount, bool graphics) const;
    // Attempt v42+: dispatch() minus its pre-R diagnostics and post-R result-text materialization.
    [[nodiscard]] GuestImportDispatchResult dispatchUnderRegistryLock(std::string_view importName, GuestImportCall& call);
    [[nodiscard]] std::optional<PhysicalRange> resolvePhysicalRange(
        std::uint32_t physical, std::uint32_t bytes, bool writable) const;
    [[nodiscard]] GuestImportDispatchResult dispatchTitleTerminationRegistration(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlTimeFieldsToTime(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchContentEnumeration(GuestImportCall& call, bool create);
    [[nodiscard]] GuestImportDispatchResult dispatchContentMount(GuestImportCall& call, bool close);
    [[nodiscard]] GuestImportDispatchResult dispatchSymbolicLink(GuestImportCall& call, bool create);
    [[nodiscard]] GuestImportDispatchResult dispatchContentDeviceData(GuestImportCall& call);
    [[nodiscard]] std::pair<std::uint32_t, std::string> contentPath(std::string_view path) const;
    [[nodiscard]] GuestFileQueryResult queryFileMetadata(std::string_view path) const;
    [[nodiscard]] std::shared_ptr<const GuestReadOnlyFile> openFileSource(std::string_view path) const;
    [[nodiscard]] GuestImportDispatchResult dispatchDbgPrint(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeGetCurrentProcessType(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeTlsAlloc(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeTlsFree(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeTlsGetValue(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeTlsSetValue(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlCompareMemoryUlong(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlFillMemoryUlong(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNativeIdentityScaler(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlInitAnsiString(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlInitUnicodeString(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlImageXexHeaderField(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlInitializeCriticalSection(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlInitializeCriticalSectionAndSpinCount(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlEnterCriticalSection(
            GuestImportCall& call, std::unique_lock<std::mutex>& lock);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlLeaveCriticalSection(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlTryEnterCriticalSection(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXexCheckExecutablePrivilege(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmAllocatePhysicalMemory(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmAllocatePhysicalMemoryEx(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmFreePhysicalMemory(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmGetPhysicalAddress(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmQueryAddressProtect(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmQueryAllocationSize(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchMmQueryStatistics(GuestImportCall& call);
    void markVirtualCommit(std::uint32_t address, std::uint32_t size, bool committed) noexcept;
    [[nodiscard]] GuestImportDispatchResult dispatchNtAllocateVirtualMemory(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtFreeVirtualMemory(
            GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtCreateSemaphore(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtReleaseSemaphore(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtCreateEvent(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchTimer(std::string_view name, GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtDuplicateObject(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtCreateMutant(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtReleaseMutant(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtSetEvent(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtClearEvent(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtWaitForSingleObjectEx(
            GuestImportCall& call, bool& pending, std::uint64_t& relativeTicks);
    [[nodiscard]] GuestImportDispatchResult dispatchNtClose(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchExCreateThread(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchObReferenceObjectByHandle(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchObDereferenceObject(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeQueryBasePriorityThread(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeSetBasePriorityThread(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNtResumeThread(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeSetAffinityThread(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlRaiseException(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXamNotifyCreateListener(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXNotifyGetNext(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeSetDisableBoostThread(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeQuerySystemTime(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchKeQueryPerformanceFrequency(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXexLoadImage(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXexGetProcedureAddress(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXexGetModuleHandle(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXexUnloadImage(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchRtlNtStatusToDosError(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchNetCleanup(GuestImportCall& call, bool winsock);
    [[nodiscard]] GuestImportDispatchResult dispatchWsaGetLastError(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXNetStartup(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXNetGetOpt(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchWsaStartup(GuestImportCall& call);
    [[nodiscard]] GuestImportDispatchResult dispatchXamGetSystemVersion(GuestImportCall& call);
    void clearNativeXamModule();

    [[nodiscard]] bool contains(std::uint32_t address, std::uint64_t size) const noexcept;
    [[nodiscard]] bool readU16(std::uint32_t address, std::uint16_t& value) const noexcept;
    [[nodiscard]] bool readU32(std::uint32_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool writeU16(std::uint32_t address, std::uint16_t value) noexcept;
    [[nodiscard]] bool writeU32(std::uint32_t address, std::uint32_t value) noexcept;
    [[nodiscard]] bool readAnsiString(
            std::uint32_t address,
            std::size_t maximumBytes,
            std::string& value) const;
    [[nodiscard]] bool findFreeBlock(
            std::uint32_t size,
            std::uint32_t alignment,
            std::uint32_t& address);
    void releaseBlock(std::uint32_t address, std::uint32_t size);

    struct CriticalSectionState {
        std::array<std::uint8_t, 16> dispatcherHeader{};
        std::uint32_t ownerToken{0};
        std::uint32_t recursionCount{0};
    };

    [[nodiscard]] bool criticalSectionStorageMatches(
            std::uint32_t address,
            const CriticalSectionState& state) const noexcept;
    [[nodiscard]] bool writeCriticalSectionState(
            std::uint32_t address,
            const CriticalSectionState& state) noexcept;
    [[nodiscard]] bool adoptPreinitializedCriticalSection(std::uint32_t address);
    [[nodiscard]] bool criticalSectionRangeAvailable(std::uint32_t address) const noexcept;
    [[nodiscard]] bool hasActiveCriticalSectionInRange(std::uint32_t address,std::uint64_t bytes) const;

    struct PhysicalAllocation {
        std::uint32_t size{0};
        std::uint32_t pageSize{0};
        std::uint32_t type{0};
        std::uint32_t protection{0};
    };

    [[nodiscard]] bool findPhysicalBlock(
            std::uint32_t size,
            std::uint32_t alignment,
            std::uint32_t minimumPhysicalAddress,
            std::uint32_t maximumPhysicalAddress,
            std::uint32_t& address);
    void releasePhysicalBlock(std::uint32_t address, std::uint32_t size);
    void clearPhysicalMemoryArena(bool scrub) noexcept;

    struct ThreadTlsStorage {
        std::uint32_t dynamicSlotBase{0};
        std::uint32_t slotCount{0};
    };

    [[nodiscard]] std::size_t tlsAllocationCapacity() const noexcept;
    [[nodiscard]] bool overlapsThreadTlsStorage(
            std::uint32_t address, std::uint64_t size) const noexcept;
    void scrubThreadTlsStorage(const ThreadTlsStorage& storage) noexcept;
    void clearThreadTlsStorage() noexcept;

    enum class KernelObjectKind { Semaphore, NotificationEvent, SynchronizationEvent, Thread, NotificationListener, File, ContentEnumerator, Mutant, NotificationTimer, SynchronizationTimer };
    struct KernelObject {
        KernelObjectKind kind;
        std::uint32_t signalCount;
        std::uint32_t maximumCount;
        std::uint32_t threadId{0};
        std::uint32_t ownerToken{0};
        std::uint32_t recursion{0};
        bool abandoned{false};
        GuestWaitableTimer timer{};
    };
    enum class ObjectAcquireState { Unavailable, Acquired, RecursionLimit };
    [[nodiscard]] ObjectAcquireState acquireKernelObject(
            KernelObject& object, std::uint32_t waiter, std::uint64_t& status);
    [[nodiscard]] std::uint32_t createKernelHandle(KernelObject object);
    std::map<std::uint32_t, KernelObject> kernelHandles_;
    struct NativeEventReference {
        std::uint32_t handle, references;
        KernelObject closedObject;
    };
    std::map<std::uint32_t, NativeEventReference> nativeEventReferences_;
    std::uint32_t nativeEventPool_{0}, nativeEventSlots_{0};
    std::uint32_t mountedDisc_{0}, mountedDiscCount_{0}, nativeDiscCompletions_{0};
    struct OpenFile {
        std::shared_ptr<const GuestReadOnlyFile> source;
        std::uint64_t position{0};
        GuestFileMetadata metadata;
        std::string contentRoot;
    };
    std::map<std::uint32_t, OpenFile> openFiles_;
    ReadOnlyFileOpen readOnlyFileOpen_;
    FileIoStatistics fileIoStatistics_;
    GuestContentCatalog installedContent_;
    struct ContentEnumerator { std::uint32_t capacity; std::size_t cursor; };
    std::map<std::uint32_t, ContentEnumerator> contentEnumerators_;
    struct ContentPackage { GuestFileMetadataCatalog metadata; ContentFiles files; };
    std::map<std::uint32_t, ContentPackage> contentPackages_;
    std::map<std::string, std::uint32_t> contentMounts_;
    std::uint32_t contentMountRejection_{0};
    // Opaque, aligned guest handles; never pointers. Do not reuse identifiers
    // after close/reset during this registry's lifetime, including after reset.
    std::uint64_t nextKernelHandle_{0x100U};
    struct Notification { std::uint32_t id{0}, parameter{0}; };
    struct NotificationListener {
        std::uint64_t mask{0};
        std::uint32_t maximumVersion{0};
        std::size_t size{0};
        std::array<Notification, MaximumQueuedNotifications> queue{};
    };
    bool notificationServiceBound_{false};
    std::optional<std::uint32_t> notificationUiPosition_;
    bool signedOutUsersBound_{false};
    bool noLaunchDataBound_{false};
    bool noRawStorageDevicesBound_{false};
    std::optional<std::uint32_t> userLanguage_;
    std::optional<std::uint32_t> userCountry_;
    std::map<std::uint32_t, NotificationListener> notificationListeners_;
    SystemTimeSource systemTimeSource_;
    std::uint32_t performanceFrequency_{0};
    std::uint32_t nativeXamAddress_{0}, nativeTitleAddress_{0};
    std::uint16_t nativeXamLoadCount_{0};
    std::uint32_t nativeSystemVersion_{0};
    bool networkLifecycleBound_{false};
    bool disconnectedInputBound_{false};
    std::shared_ptr<GuestAudioBackend> audioBackend_;
    std::shared_ptr<GuestXmaBackend> xmaBackend_;
    std::uint32_t audioSpeakerQueries_{0};
    bool xnetConfigured_{false};
    std::array<std::uint8_t, 13> xnetParameters_{};
    std::uint16_t winsockReferences_{0};

    [[nodiscard]] bool overlapsThreadArena(std::uint32_t address, std::uint64_t size) const;
    GuestThreadStoragePool threadStorage_;
    GuestThreadStorageConfig threadStorageConfig_{};
    std::function<bool(std::uint32_t)> exactFunctionValidator_;
    // Thread token -> ID. Independent from handles: close must not kill a thread.
    std::map<std::uint32_t, std::uint32_t> createdThreads_;
    struct ThreadObjectState {
        std::uint32_t threadId;
        std::uint32_t references{0};
        // NT-compatible relative increment. New title threads inherit normal
        // priority (increment zero). Normal -2..2 and saturation +/-16 are modeled.
        std::int32_t priorityIncrement{0};
        bool initial{false};
        std::uint32_t suspendCount{1};
        bool returned{false};
        std::uint32_t exitCode{0};
        std::uint32_t virtualCpuMask{1}; // Prepared PCR/thread bytes start on virtual CPU 0.
        std::optional<std::string> debugName{}; // Bounded, transient, never logged or serialized.
        bool disableBoost{false}; // Suppresses future opt-in native event wake boosts.
        std::uint32_t criticalRegionDepth{0}; // Native normal-kernel-APC deferral; not a scheduler lock.
    };
    [[nodiscard]] ThreadObjectState* findThreadObject(std::uint32_t token);
    [[nodiscard]] const ThreadObjectState* findThreadObject(std::uint32_t token) const;
    [[nodiscard]] bool overlapsThreadTypeIdentity(std::uint32_t address, std::uint64_t size) const;
    [[nodiscard]] bool overlapsProtectedThreadStorage(std::uint32_t address, std::uint64_t size) const;
    std::map<std::uint32_t, ThreadObjectState> threadObjects_;
    std::uint32_t threadObjectTypeAddress_{0};
    ThreadResumeHandler threadResumeHandler_;
    ThreadExitHandler threadExitHandler_;
    FileMetadataQuery fileMetadataQuery_;
    GuestSymbolicLinks symbolicLinks_;
    std::optional<FileCreateRequest> lastFileCreateRequest_;
    ThreadPriorityHandler threadPriorityHandler_;
    NativeEventWakeBoostHandler nativeEventWakeBoostHandler_;
    NativeEventWakeBoostStatistics nativeEventWakeBoostStatistics_{};
    std::optional<ThreadPriorityRequest> lastThreadPriorityRequest_;
    bool virtualCpuAffinityBound_{false};
    bool threadDebugNamingBound_{false};
    ThreadWaitHandler threadWaitHandler_;
    ThreadTimedWaitHandler threadTimedWaitHandler_;
    ThreadDelayHandler threadDelayHandler_;
    std::array<std::uint64_t,7> threadDelayStatistics_{};
    // Direct, guest-initialized KEVENTs are distinct from NT handles. Host-only
    // wait queues preserve guest list heads; they never stash host pointers in RAM.
    // Each parked wait pins its enclosing allocation until cleanup, including
    // cancellation. Set reserves real signals before any later Reset can lose them.
    struct DispatcherWaitAnyGroup {
        std::optional<std::uint32_t> winner;
        bool settled{false};
    };
    struct DispatcherEventWait {
        std::uint32_t header;
        bool signaled{false};
        bool settled{false};
        std::optional<std::chrono::steady_clock::time_point> deadline;
        std::shared_ptr<DispatcherWaitAnyGroup> group{};
        std::uint32_t index{0};
        std::uint32_t token{0};
    };
    std::map<std::uint32_t, std::vector<std::shared_ptr<DispatcherEventWait>>> dispatcherEventWaits_;
    bool virtualVideoModeBound_{false};
    std::optional<std::array<std::uint32_t,2>> nativeScalerTimingBinding_;
    std::uint64_t nativeScalerTimingQueries_=0;
    bool nativeIdentityScalerBound_ = false;
    std::optional<recomp::gpu::NativeScalerCommand> pendingNativeScaler_;
    // Lease starts at successful wrapped swap, before returning to guest code.
    // Otherwise freeing/reusing a command allocation could inherit a receipt.
    std::optional<StoppedPhysicalOwnership> nativeScalerStorageOwner_;
    std::optional<GraphicsRingConfiguration> nativeScalerRing_;
    std::optional<NativeScalerStorageCompletion> completedScalerStorageCoherency_;
    std::optional<std::uint32_t> nativeCommandInterruptWorker_;
    std::optional<std::uint32_t> postOutputSwapInterruptWorker_;
    std::optional<std::uint32_t> deferredSwapInterruptWorker_;
    [[nodiscard]] bool validateNativeScalerStorageLocked() const noexcept;
    [[nodiscard]] std::optional<NativePendingSubmission> inspectStoppedNativeScalerSubmissionLocked(std::uint32_t writeWord) const;
    [[nodiscard]] std::optional<NativePendingSubmission> inspectPostOutputSwapSubmissionLocked(std::uint32_t writeWord) const;
    [[nodiscard]] std::optional<StoppedPostOutputSwapInterrupt> inspectPostOutputSwapInterruptLocked(
        std::uint32_t writeWord,std::uint32_t* rejection,bool allowOwnedActiveScope=false) const;
    [[nodiscard]] std::uint32_t postOutputSwapQueuedInterruptRejectionLocked(const StoppedPostOutputSwapInterrupt&) const;
    [[nodiscard]] std::optional<recomp::gpu::NativeScalerWrapper> planNativeScalerWrapperLocked(std::uint32_t writeWord) const;
    std::shared_ptr<GuestGraphicsBackend> graphicsBackend_;
    std::function<bool(std::uint32_t)> graphicsCallbackValidator_;
    std::uint32_t graphicsImageBase_{0}, graphicsImageBytes_{0};
    std::function<bool(std::uint32_t)> dpcCallbackValidator_;
    bool graphicsInitialized_{false};
    std::array<std::uint32_t, 5> graphicsConfiguration_{};
    std::optional<GraphicsInterruptRegistration> graphicsInterruptRegistration_;
    std::optional<GraphicsRingConfiguration> graphicsRingConfiguration_;
    std::optional<std::uint32_t> graphicsRingAllocation_, graphicsWriteBackAllocation_;
    std::optional<std::uint32_t> graphicsSystemIdentifier_, graphicsIdentifierAllocation_;
    bool nativeNoSystemOverlayBound_{false};
    std::optional<NativeSystemContext> nativeSystemContext_;
    std::optional<NativeSystemContext> postOutputSystemContext_;
    std::optional<NativeSystemContext> postSwapSystemContext_;
    bool issueNativePostOutputSystemContextLocked(const GuestImportCall&,bool afterSwap=false);
    std::uint32_t nativeSystemContextThread_{0};
    bool graphicsMeInitVerified_{false};
    bool initialCoherencySubmissionBound_{false};
    std::optional<NativeInitialCoherency> completedInitialCoherency_;
    bool experimentalInitializationBound_{false};
    bool emptyCacheCoherencyBound_{false};
    std::optional<EmptyCacheCoherencyCompletion> completedEmptyCacheCoherency_;
    std::optional<EmptyCacheCoherencyCompletion> completedWarmEmptyCacheCoherency_;
    bool nativeStateBatchesBound_{false};
    bool experimentalOwnedColorBound_{false};
    std::optional<NativeOwnedColorCompletion> completedOwnedColor_;
    struct NativeOwnedColorImage {
        std::uint32_t physicalBase,virtualBase,allocationBase,allocationBytes,protection,readWord;
        recomp::gpu::NativeTiledCopyPublication copy;
    };
    // Owned readback, not a pinned guest allocation. Invalidate on allocation
    // free/reset so a same-address replacement cannot inherit this receipt.
    std::optional<NativeOwnedColorImage> completedOwnedColorImage_;
    bool postOutputColorAttempted_=false;
    std::uint32_t postOutputColorRejection_=0,postOutputColorDetail_=0;
    std::optional<NativePostOutputColorCompletion> completedPostOutputColor_;
    std::optional<NativeOwnedColorImage> pendingPostOutputColorImage_;
    [[nodiscard]] std::optional<StoppedNativeSwapRequest> inspectNativeSwapRequestLocked(const GuestImportCall&,bool postOutput=false,bool afterSwap=false) const;
    [[nodiscard]] bool issueExperimentalNativeSwapCommandLocked(const GuestImportCall&);
    [[nodiscard]] bool issuePostOutputSwapCommandLocked(const GuestImportCall&);
    [[nodiscard]] bool issueDeferredNativeSwapCommandLocked(const GuestImportCall&);
    [[nodiscard]] bool validatePendingNativeSwapLocked(bool postOutput=false) const noexcept;
    [[nodiscard]] bool visitNativeOwnedSwapImageLocked(const GuestImportCall&,
        const std::function<bool(const NativeOwnedSwapImage&)>&,bool postOutput=false) const;
    struct PendingNativeSwapCommand {
        recomp::gpu::NativeSwapCommand command;
        NativeOwnedSwapImage image;
        GraphicsRingConfiguration ring;
        std::uint32_t identifierAddress;
        std::array<std::uint32_t,5> pinnedAllocations; // command, image, ring, writeback, identifier
    };
    std::function<bool(const NativeOwnedSwapImage&)> nativeSwapConsumer_;
    std::optional<PendingNativeSwapCommand> pendingNativeSwap_;
    std::optional<PendingNativeSwapCommand> pendingPostOutputSwap_;
    struct PendingDeferredSwapCommand {
        recomp::gpu::NativeSwapCommand command;
        NativeDeferredSwapRequest metadata;
        std::array<std::uint32_t,5> pinnedAllocations;
        GraphicsRingConfiguration ring;
        std::uint32_t identifierAddress;
    };
    std::optional<PendingDeferredSwapCommand> pendingDeferredSwap_;
    bool deferredSwapStorageAttempted_=false;
    std::optional<NativeCommandStorageCompletion> completedDeferredSwapStorage_;
    bool deferredColorPrefixAttempted_=false;
    bool deferredWrappedSwapAttempted_=false;
    std::optional<NativeDeferredWrappedSwapCompletion> completedDeferredWrappedSwap_;
    std::uint32_t deferredColorPrefixRejection_=0,deferredColorPrefixDetail_=0;
    std::optional<NativeDeferredColorPrefixCompletion> completedDeferredColorPrefix_;
    std::optional<NativeOwnedColorImage> pendingDeferredColorImage_;
    std::shared_ptr<const recomp::gpu::NativeDeferredSwapSubmission> deferredColorSubmission_;
    std::array<std::uint32_t,60> deferredColorPinnedAllocations_{};
    unsigned deferredColorPinCount_=0;
    [[nodiscard]] std::optional<NativePendingSubmission> inspectDeferredSwapSubmissionLocked(std::uint32_t writeWord) const;
    [[nodiscard]] std::optional<StoppedDeferredSwapInterrupt> inspectDeferredSwapInterruptLocked(
        std::uint32_t writeWord,std::uint32_t* rejection,bool allowOwnedActiveScope=false) const;
    template<class Wrapper> [[nodiscard]] std::optional<StoppedSurfaceSwapInterrupt<Wrapper>> inspectSurfaceSwapFieldsLocked(
        const Wrapper&,const NativePendingSubmission&,std::uint32_t* rejection) const;
    [[nodiscard]] bool validatePendingDeferredSwapLocked() const noexcept;
    std::optional<StoppedNativeSwapRequest> nativeScanoutRequest_;
    [[nodiscard]] bool validateNativeScanoutLocked() const noexcept;
    // Independent image lease established by the completed second native swap.
    // Never redirects first-frame gamma/output consumers to different pixels.
    [[nodiscard]] bool validateNativePostOutputScanoutLocked(bool afterDeferredStorage=false) const noexcept;
    NativeSwapCommandStatus nativeSwapStatus_{};
    NativeSwapCommandStatus postOutputSwapStatus_{};
    bool postOutputSwapStorageAttempted_=false;
    std::optional<NativeCommandStorageCompletion> completedPostOutputSwapStorage_;
    [[nodiscard]] bool submitNativeSwapStorageLocked(std::uint32_t writeWord,bool postOutput);
    NativeDisplayModeStatus nativeDisplayModeStatus_{};
    std::optional<NativeCommandStorageCompletion> completedCommandStorageCoherency_;
    std::optional<NativeWrappedSwapCompletion> completedWrappedSwap_;
    bool postOutputWrappedSwapAttempted_=false;
    std::optional<NativePostOutputWrappedSwapCompletion> completedPostOutputWrappedSwap_;
    bool experimentalIdleCachePairBound_{false};
    std::uint64_t experimentalIdleCachePairs_{0};
    std::optional<NativeStateBatchCompletion> completedNativeStateBatches_;
    std::optional<NativeStateBatchCompletion> completedWarmStateBatches_;
    std::optional<SideEffectFreeBatchCompletion> completedSideEffectFreeBatches_;
    std::optional<SideEffectFreeBatchCompletion> completedWarmSideEffectFreeBatches_;
    std::optional<std::uint32_t> retiredGraphicsRingReadWord_;
    bool graphicsWriteBackRebindAllowed_{false};
    std::optional<NativeInitialCoherency> completedWarmRingCoherency_;
    std::optional<NativeInitializationCompletion> completedExperimentalInitialization_;
    std::uint32_t edramCalibrationLock_{0}, edramPreparedThread_{0};
    NativeEdramTrainingState edramTrainingState_{};
    [[nodiscard]] GuestImportDispatchResult dispatchNativeEdramTraining(std::string_view name, GuestImportCall& call);
    std::uint32_t terminationImageBase_{0}, terminationImageBytes_{0};
    std::function<bool(std::uint32_t)> terminationCallbackValidator_;
    std::vector<TitleTerminationRegistration> titleTerminationRegistrations_;

    mutable std::mutex mutex_;
    GuestAddressSpace* addressSpace_{nullptr};
    std::uint32_t heapBase_{0};
    std::uint32_t heapSize_{0};
    std::map<std::uint32_t, std::uint32_t> freeBlocks_;
    std::map<std::uint32_t, std::uint32_t> allocations_;
    // Guest-requested commit state only, not host RSS or page-fault residency.
    std::vector<std::uint64_t> virtualCommitBits_;
    std::uint32_t virtualCommittedPages_{0};
    std::optional<std::array<std::uint32_t, 26>> lastMemoryStatistics_;
    GuestImageProtections imageProtections_;
    std::map<std::uint32_t, CriticalSectionState> criticalSections_;
    bool nativeCriticalSectionWaitsBound_{false};
    bool nativeHighPriorityTiersBound_{false};
    std::uint64_t nativeHighPriorityTiersRequests_{0};
    std::map<std::uint32_t,std::vector<std::uint32_t>> criticalSectionWaits_;
    std::array<std::uint64_t,4> criticalSectionWaitStatistics_{};
    std::optional<std::array<std::uint32_t,16>> lastCriticalSectionRejection_;
    bool physicalMemoryArenaBound_{false};
    std::uint32_t physicalArenaBase_{0};
    std::uint32_t physicalArenaSize_{0};
    std::uint32_t physicalAddressOffset_{0};
    std::map<std::uint32_t, std::uint32_t> physicalFreeBlocks_;
    std::map<std::uint32_t, PhysicalAllocation> physicalAllocations_;
    std::uint32_t boundGuestThreadToken_{0};
    std::uint32_t boundExecutableXexHeader_{0};
    static constexpr std::size_t TlsSlotCount = 2048;
    std::bitset<TlsSlotCount> tlsSlots_;
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> threadTlsValues_;
    std::map<std::uint32_t, ThreadTlsStorage> threadTlsStorage_;
    GuestLogSink logSink_;
    NativeGamepadReader nativeGamepadReader_;
    NativeGamepadReader nativeGamepadCapabilities_;
};

[[nodiscard]] const char* guestImportCategoryName(GuestImportCategory category) noexcept;
[[nodiscard]] const char* guestImportDispatchStateName(
        GuestImportDispatchState state) noexcept;

} // namespace recomp::bootstrap
