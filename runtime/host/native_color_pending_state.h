#pragma once
#include "native_color_pending_registers.h"
#include "native_graphics_state.h"

namespace recomp::gpu {
// Prepare the entire raw journal and native bookkeeping BEFORE GPU work.
// No live state changes. Completion-dependent register bits remain unknown.
// The three invalidation epochs denote the conservative fresh-owned-bindings
// native policy, NOT measured Xenos cache behavior. A completed native executor
// and failure-atomic publisher are still required before any state commit.
inline bool prepareNativeColorState(const NativeColorBatchCapture& capture,
        const NativeRegisterJournal& priorRegisters,const NativeGraphicsState& prior,
        NativeRegisterJournal& outputRegisters,NativeGraphicsState& output) noexcept {
    const auto& initial=capture.initialBins();
    const auto& final=capture.commands().finalBins;
    const auto mask=[](const DrawBinState& b){return std::uint64_t{b.words[0]}|(std::uint64_t{b.words[1]}<<32);};
    const auto select=[](const DrawBinState& b){return std::uint64_t{b.words[2]}|(std::uint64_t{b.words[3]}<<32);};
    if(mask(initial)!=prior.binMask || select(initial)!=prior.binSelect || initial.knownWords!=prior.knownBinWords ||
        evaluateDrawBinPredicate(initial,false)==DrawBinDecision::InvalidState ||
        evaluateDrawBinPredicate(final,false)==DrawBinDecision::InvalidState ||
        prior.invalidationEpoch>UINT64_MAX-3 || prior.batches==UINT32_MAX ||
        prior.experimentalColorBatches==UINT32_MAX)return false;
    auto candidate=prior;
    candidate.binMask=mask(final);candidate.binSelect=select(final);candidate.knownBinWords=final.knownWords;
    candidate.invalidationEpoch+=3;++candidate.batches;++candidate.experimentalColorBatches;
    NativeRegisterJournal registers;
    if(!prepareNativeColorRegisters(capture,priorRegisters,registers))return false;
    outputRegisters.swap(registers);output=candidate;return true;
}
}
