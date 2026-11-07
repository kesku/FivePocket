#pragma once
#include "native_color_execution_result.h"

namespace recomp::gpu {
// Non-owning synchronous publisher. It may write only its prevalidated owned
// outputs, must keep every failure before the first store, and must never
// reenter guest execution, registry or backend. Return true only on complete
// copy/events/RPTR-last publication. A false result must leave outputs intact.
struct NativeColorPublisher {
    void* context=nullptr;
    bool (*publish)(void*,const NativeColorChainExecutionResult&) noexcept=nullptr;
};
enum class NativeColorEdramCommit {Initialize,ReplaceFullKnownRegion};
// The only resident representation currently has ONE fixed fully known region.
// The admitted full depth-clear/alias/color/copy chain overwrites that entire
// same region. It does not consume old pixels; replacing it is not a general
// partial-surface merge or permission to drop knowledge elsewhere in EDRAM.
inline bool validNativeColorEdramTransition(const NativeColorBatchCapture& capture,
        const NativeGraphicsState& state,const std::optional<NativeColorEdramSnapshot>& retained,
        NativeColorEdramCommit mode) noexcept {
    if(mode==NativeColorEdramCommit::Initialize)
        return capture.synchronization().phase==NativeColorBatchPhase::Startup && !retained && !state.experimentalColorBatches;
    if(mode!=NativeColorEdramCommit::ReplaceFullKnownRegion)return false;
    return capture.synchronization().phase==NativeColorBatchPhase::PostOutput && retained &&
        retained->knownBytes().size()==NativeColorEdramSnapshot::Bytes &&
        state.experimentalColorBatches>0 && state.experimentalColorBatches<256;
}

// Last part of the ConservativeOwnedResourcesV1 transaction. Caller has run
// the actual native executor and audited its fences/visibility; the counters
// below are sanity checks, NOT proof that those prerequisites happened. Guest
// publishers also require continuously held lifecycle ownership/parked writers.
//
// Staging is disjoint from live state. No allocations, user callbacks, fallible
// checks or early returns follow successful publication. Only the defined EDRAM
// region survives; no diagnostic canaries become known memory. Replacement is
// explicitly opt-in and restricted to the identical fully overwritten region.
inline bool publishAndCommitNativeColorState(const NativeColorBatchCapture& capture,
        NativeColorChainExecutionResult& completed,NativeRegisterJournal& preparedRegisters,
        const NativeGraphicsState& prepared,NativeColorPublisher publisher,
        NativeRegisterJournal& liveRegisters,NativeGraphicsState& liveState,
        std::optional<NativeColorEdramSnapshot>& liveEdram,
        NativeColorEdramCommit mode=NativeColorEdramCommit::Initialize) noexcept {
    static_assert(noexcept(liveRegisters.swap(preparedRegisters)));
    static_assert(noexcept(liveEdram.swap(completed.edram)));
    static_assert(std::is_nothrow_copy_assignable_v<NativeGraphicsState>);
    if(!publisher.context || !publisher.publish || &liveRegisters==&preparedRegisters || &liveState==&prepared ||
        &liveEdram==&completed.edram || !validNativeColorEdramTransition(capture,liveState,liveEdram,mode) ||
        completed.leadingSetupFences!=(mode==NativeColorEdramCommit::ReplaceFullKnownRegion?1u:0u) ||
        !completed.copy || !completed.edram ||
        completed.edram->knownBytes().size()!=NativeColorEdramSnapshot::Bytes || !completed.auditedTimeline ||
        !completed.auditedTimeline->complete() || completed.nativeFences!=4 ||
        !completed.copy->matchesPlan(capture.copy().copy))return false;
    // Check the complete prepared raw state again before the publication
    // callback. Any map allocation is still before the first guest store.
    NativeRegisterJournal checked;NativeGraphicsState state;
    if(!prepareNativeColorState(capture,liveRegisters,liveState,checked,state) || checked!=preparedRegisters ||
        state.binMask!=prepared.binMask || state.binSelect!=prepared.binSelect || state.knownBinWords!=prepared.knownBinWords ||
        state.invalidationEpoch!=prepared.invalidationEpoch || state.batches!=prepared.batches ||
        state.experimentalIdleCachePairEpoch!=prepared.experimentalIdleCachePairEpoch ||
        state.experimentalColorBatches!=prepared.experimentalColorBatches)return false;
    if(!publisher.publish(publisher.context,completed))return false;
    liveRegisters.swap(preparedRegisters);
    liveEdram.swap(completed.edram);
    liveState=prepared;
    return true;
}
}
