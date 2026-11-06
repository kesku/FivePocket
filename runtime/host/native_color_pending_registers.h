#pragma once
#include "native_color_batch_capture.h"

namespace recomp::gpu {
using NativeRegisterJournal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
// Preallocated RAW register candidate, not a completed backend transaction.
// Only the value/known-bit transfer of high register writes and constant loads
// is modeled here. The admitted profile has no RMW commands: the generic parser
// limits RMW to low registers and this batch's sync gate excludes low RMW.
// Unassigned prior registers retain exactly their original value/knowledge.
// Four low request registers and EVENT_INITIATOR are deliberately removed:
// their values depend on uncompleted synchronization/event effects. A future
// backend must supply those effects before this can become committed state.
// No shader residency, cache epoch, EDRAM state, fence or publication is implied.
inline bool prepareNativeColorRegisters(const NativeColorBatchCapture& capture,
        const NativeRegisterJournal& prior,NativeRegisterJournal& output) noexcept {
    if(prior.size()>0x8000 || capture.controls().rootWords[2]!=capture.commands().commands.words().size())return false;
    NativeColorChainBindings bindings;
    if(!capture.color().bindings(bindings))return false; // moved-from owner
    for(const auto& entry:prior)
        if(entry.first>=0x8000 || (entry.second.first&~entry.second.second))return false;
    try {
        auto candidate=prior;
        for(const auto& entry:capture.commands().finalRegisters){
            const auto index=entry.first;const auto bits=entry.second.bits;
            if(index>=0x8000 || (bits.value&~bits.known))return false;
            if(index<0x2000 || index==0x21F9)continue;
            if(bits.known!=0xFFFFFFFF)return false;
            candidate[index]={bits.value,bits.known};
        }
        for(unsigned index:{0x5C8u,0xA2Fu,0xA30u,0xA31u,0x21F9u})candidate.erase(index);
        output.swap(candidate);return true;
    }catch(...){return false;}
}
}
