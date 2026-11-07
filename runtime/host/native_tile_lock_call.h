#pragma once
#include "guest_import_registry.h"

namespace recomp::bootstrap {
enum class NativeTileLockCall { PendingProducer, OrdinaryWorker };
enum class NativeTileLockCallResult { NotRequired, Returned, Refused };

// Shared original-function adapter. The caller owns the active guest task and
// supplies its real executor gate. Original CPU code is never replaced by a
// fabricated return; Refused after entry is terminal, never a retry signal.
// This handles locking only: no GPU/IRQ/event/fence or ring retirement.
inline NativeTileLockCallResult invokeNativeTileLockCall(GuestImportRegistry& registry,
        const GuestImportRegistry::NativeSerializedGate& gate,std::uint32_t device,
        NativeTileLockCall kind,const std::function<void()>& original) {
    if(!device || !gate || !original)return NativeTileLockCallResult::Refused;
    const auto state=registry.copySerializedGuestHeap(device,21944);
    if(!state || state->size()!=21944)return NativeTileLockCallResult::Refused;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{(*state)[at]}<<24)|(std::uint32_t{(*state)[at+1]}<<16)|
            (std::uint32_t{(*state)[at+2]}<<8)|(*state)[at+3];
    };
    if(kind==NativeTileLockCall::PendingProducer) {
        // Original8232E278 branches before taking the ordinary lock. Preserve
        // recording/direct submissions outside this adapter's lock scope.
        if(word(13600) || !word(11152))return NativeTileLockCallResult::NotRequired;
        // Optional post-pair event requires separate dispatcher integration.
        if((*state)[11072]&8)return NativeTileLockCallResult::Refused;
    } else if(kind==NativeTileLockCall::OrdinaryWorker) {
        // Current captured worker has no per-CPU command buffers or L2 unlock.
        // Do not start mutating worker state before an unimplemented drain.
        if(word(11080+112))return NativeTileLockCallResult::Refused;
        for(unsigned cpu=0;cpu<6;++cpu)
            if(word(11592+cpu*108))return NativeTileLockCallResult::Refused;
    } else return NativeTileLockCallResult::Refused;
    return registry.runNativeTileLockScope(device,gate,original)?
        NativeTileLockCallResult::Returned:NativeTileLockCallResult::Refused;
}
}
