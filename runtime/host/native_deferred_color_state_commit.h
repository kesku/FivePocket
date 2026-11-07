#pragma once
#include "native_deferred_color_execution_result.h"

namespace recomp::gpu {
// Synchronous non-reentrant COPY-only publisher. It must hold real allocation
// lifecycle ownership and parked writers, recheck all before-images and leave
// every byte unchanged on false. No event, IRQ, presentation or RPTR writes.
struct NativeDeferredColorPublisher {
    void* context=nullptr;
    bool (*publishCopy)(void*,const NativeDeferredColorExecutionResult&) noexcept=nullptr;
};
// Complete a PREFIX subtransaction only. The full IB must stay pending for its
// separately typed ticket3 tail. This helper does not admit a live submission,
// run a GPU, obtain a lease or make a diagnostic usable as a live receipt.
inline bool publishAndCommitNativeDeferredColorState(const NativeDeferredColorCapture& capture,
        NativeDeferredColorExecutionResult& completed,const NativeDeferredColorPublisher& publisher,
        NativeRegisterJournal& liveRegisters,NativeGraphicsState& liveState,
        std::optional<NativeColorEdramSnapshot>& liveEdram) noexcept {
    static_assert(noexcept(liveRegisters.swap(*completed.pendingRegisters)));
    static_assert(noexcept(liveEdram.swap(completed.edram)));
    static_assert(std::is_nothrow_copy_assignable_v<NativeGraphicsState>);
    if(!publisher.context || !publisher.publishCopy || completed.committed || !completed.submission ||
        !completed.submission->sameRequest(capture.submission()) || !completed.copy || !completed.edram ||
        !completed.pendingRegisters || !completed.pendingState || !completed.auditedPrefixTimeline ||
        &liveRegisters==&*completed.pendingRegisters || &liveState==&*completed.pendingState || &liveEdram==&completed.edram ||
        !liveEdram || liveEdram->knownBytes().size()!=NativeColorEdramSnapshot::Bytes ||
        completed.edram->knownBytes().size()!=NativeColorEdramSnapshot::Bytes ||
        completed.nativeFences!=4 || completed.leadingSetupFences!=1 || !completed.copy->matchesPlan(capture.copy().copy))return false;
    const auto& timeline=*completed.auditedPrefixTimeline;unsigned busy=99;
    if(!timeline.complete() || timeline.leadingFences()!=1 || timeline.completedStages()!=4 ||
        timeline.invalidations()!=2 || timeline.copyIdleCleans()!=1 || timeline.depthWaits()!=1 ||
        timeline.cacheFlushes()!=1 || timeline.literalEvents() || !timeline.busyBit(busy) || busy)return false;
    // Rebuild the entire expected candidate BEFORE the first copy store.
    NativeRegisterJournal checked;NativeGraphicsState state;
    if(!prepareNativeDeferredColorState(capture,liveRegisters,liveState,checked,state) || checked!=*completed.pendingRegisters)return false;
    const auto& pending=*completed.pendingState;
    if(state.binMask!=pending.binMask || state.binSelect!=pending.binSelect || state.knownBinWords!=pending.knownBinWords ||
        state.invalidationEpoch!=pending.invalidationEpoch || state.batches!=pending.batches ||
        state.experimentalIdleCachePairEpoch!=pending.experimentalIdleCachePairEpoch ||
        state.experimentalColorBatches!=pending.experimentalColorBatches)return false;
    if(!publisher.publishCopy(publisher.context,completed))return false;
    // No allocation, callback, fallible check or early return after publication.
    // Retain only the same fully overwritten known EDRAM region; prior storage
    // moves into completed for destruction after the transaction, never early.
    liveRegisters.swap(*completed.pendingRegisters);liveEdram.swap(completed.edram);
    liveState=state;completed.committed=true;return true;
}
}
