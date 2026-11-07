#pragma once
#include "native_deferred_color_capture.h"
#include "native_color_pending_registers.h"
#include "native_graphics_state.h"

namespace recomp::gpu {
// Preallocated PREFIX candidate only; not committed state or a GPU receipt.
// The initial bins must match the snapshot from BEFORE this prefix, not a later
// draw's predicate. Exactly two invalidations belong here; the third is in the
// separately typed surface tail. No final color events or tail state borrowed.
// Pending sync/event values remain unknown, including the low legacy setup
// register. The fresh leading fence does not prove hardware register readback.
inline bool prepareNativeDeferredColorState(const NativeDeferredColorCapture& capture,
        const NativeRegisterJournal& priorRegisters,const NativeGraphicsState& prior,
        NativeRegisterJournal& outputRegisters,NativeGraphicsState& output) noexcept {
    const auto& initial=capture.initialBins();const auto& final=capture.commands().finalBins;
    const auto mask=[](const DrawBinState& b){return std::uint64_t{b.words[0]}|(std::uint64_t{b.words[1]}<<32);};
    const auto select=[](const DrawBinState& b){return std::uint64_t{b.words[2]}|(std::uint64_t{b.words[3]}<<32);};
    NativeDeferredColorSyncRequests requests;NativeColorChainBindings bindings;
    if(prior.experimentalColorBatches!=2 || prior.batches<2 || prior.batches==UINT32_MAX ||
        prior.invalidationEpoch>UINT64_MAX-2 || priorRegisters.size()>0x8000 ||
        mask(initial)!=prior.binMask || select(initial)!=prior.binSelect || initial.knownWords!=prior.knownBinWords ||
        evaluateDrawBinPredicate(initial,false)==DrawBinDecision::InvalidState ||
        evaluateDrawBinPredicate(final,false)==DrawBinDecision::InvalidState ||
        !planNativeDeferredColorSync(capture.submission(),capture.commands(),requests) ||
        !capture.color().bindings(bindings))return false;
    for(const auto& entry:priorRegisters)
        if(entry.first>=0x8000 || (entry.second.first&~entry.second.second))return false;
    try{
        auto registers=priorRegisters;auto state=prior;
        for(const auto& entry:capture.commands().finalRegisters){
            const auto index=entry.first;const auto bits=entry.second.bits;
            if(index>=0x8000 || (bits.value&~bits.known))return false;
            if(index<0x2000 || index==0x21F9)continue;
            if(bits.known!=0xFFFFFFFF)return false;
            registers[index]={bits.value,bits.known};
        }
        for(unsigned index:{0x5C8u,0xF01u,0xA2Fu,0xA30u,0xA31u,0x21F9u})registers.erase(index);
        state.binMask=mask(final);state.binSelect=select(final);state.knownBinWords=final.knownWords;
        state.invalidationEpoch+=2;++state.batches;++state.experimentalColorBatches;
        // experimentalIdleCachePairEpoch belongs to separately dispatched
        // whole idle/cache-pair transactions, not individual draw-stage waits.
        outputRegisters.swap(registers);output=state;return true;
    }catch(...){return false;}
}
}
