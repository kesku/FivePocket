#pragma once
#include "native_color_chain_events.h"

namespace recomp::gpu {
enum class NativeColorBatchPhase {Startup,PostOutput};
struct NativeColorChainSyncRequests {
    unsigned depthStatusOffset=0,depthRangeOffset=0,depthWaitOffset=0,cacheFlushOffset=0;
    std::array<unsigned,3> invalidationOffsets{},idleCleanOffsets{};
    NativeColorBatchPhase phase=NativeColorBatchPhase::Startup;
    unsigned leadingIdleOffset=0,legacySetupOffset=0;
};
// Exact ordering of synchronization REQUESTS in the bounded four-draw chain.
// Includes WAIT_UNTIL writes (bit17, 3D idleclean), not just PM4 wait opcodes.
// It does NOT clear a busy bit, clean a cache, invalidate resident resources,
// update EVENT_INITIATOR, signal events or certify any fence. In particular the
// zero COHER_SIZE request is NOT interpreted as no work. A live implementation
// must discharge these obligations with its real resource model and fences.
inline bool planNativeColorChainSync(const DrawCommandPlan& plan,NativeColorChainSyncRequests& output,
        NativeColorBatchPhase phase=NativeColorBatchPhase::Startup) noexcept {
    if(phase!=NativeColorBatchPhase::Startup && phase!=NativeColorBatchPhase::PostOutput)return false;
    NativeColorChainEventRequests events;
    if(!planNativeColorChainFinalEvents(plan,events) || plan.waits.size()!=1 ||
        plan.waits[0].priorDraws!=1)return false;
    NativeColorChainSyncRequests candidate;candidate.cacheFlushOffset=events.cacheFlushOffset;candidate.phase=phase;
    unsigned invalidations=0,idles=0,statusWrites=0,rangeWrites=0,waits=0;
    bool leadingIdle=false,legacySetup=false;
    for(unsigned i=0;i<plan.commands.packets().size();++i){
        const auto& p=plan.commands.packets()[i];const auto& action=plan.actions[i];
        const auto at=p.offsetWords;const auto* v=plan.commands.words().data()+at+1;
        if(p.kind==Pm4Kind::RegisterSequence || p.kind==Pm4Kind::RegisterPair){
            bool low=false;
            for(unsigned n=0;n<p.payloadWords;++n){
                const auto reg=p.kind==Pm4Kind::RegisterPair?(n?p.secondRegister:p.firstRegister):p.firstRegister+(p.repeatRegister?0:n);
                low|=reg<0x2000;
            }
            if(!low)continue;
            if(p.kind!=Pm4Kind::RegisterSequence || p.repeatRegister || action.kind!=DrawCommandKind::RegisterWrite)return false;
            if(p.firstRegister==0xA31){
                if(statusWrites++ || rangeWrites || waits || action.priorDraws!=1 || p.payloadWords!=1 ||
                    plan.commands.words()[at]!=0xA31 || v[0]!=0x10000)return false;
                candidate.depthStatusOffset=at;
            } else if(p.firstRegister==0xA2F){
                if(rangeWrites++ || statusWrites!=1 || waits || action.priorDraws!=1 || p.payloadWords!=2 ||
                    plan.commands.words()[at]!=0x10A2F || v[0] || v[1]!=0x1000 || at!=candidate.depthStatusOffset+2)return false;
                candidate.depthRangeOffset=at;
            } else if(p.firstRegister==0x5C8){
                if(action.priorDraws==0){
                    if(phase!=NativeColorBatchPhase::PostOutput || leadingIdle || legacySetup || invalidations ||
                        p.payloadWords!=1 || plan.commands.words()[at]!=0x5C8 || v[0]!=0x20000)return false;
                    candidate.leadingIdleOffset=at;leadingIdle=true;continue;
                }
                if(idles==3 || action.priorDraws!=(idles?4u:3u) || p.payloadWords!=1 ||
                    plan.commands.words()[at]!=0x5C8 || v[0]!=0x20000)return false;
                candidate.idleCleanOffsets[idles++]=at;
            } else if(p.firstRegister==0xF01){
                // Same explicit legacy RB_BC_CONTROL=0x200E native setup
                // policy as cold/warm initialization, after a NEW leading
                // all-prior-work wait. Metadata only; caller must implement it.
                if(phase!=NativeColorBatchPhase::PostOutput || !leadingIdle || legacySetup || invalidations ||
                    action.priorDraws || p.payloadWords!=1 || plan.commands.words()[at]!=0xF01 || v[0]!=0x200E ||
                    at!=candidate.leadingIdleOffset+2)return false;
                candidate.legacySetupOffset=at;legacySetup=true;
            } else return false;
        } else if(p.kind==Pm4Kind::Command && p.opcode==0x21){
            // No RMW can hide a low-register side effect from the explicit list.
            if(!p.payloadWords || v[0]<0x2000)return false;
        } else if(p.kind==Pm4Kind::Command && p.opcode==0x3B){
            constexpr std::array<unsigned,3> masks{{0x300,0x100,0x7FFF}},priorDraws{{0,3,4}};
            if(invalidations==3 || p.payloadWords!=1 || p.predicated || plan.commands.words()[at]!=0xC0003B00 ||
                v[0]!=masks[invalidations] || action.priorDraws!=priorDraws[invalidations] || action.kind!=DrawCommandKind::Invalidate)return false;
            candidate.invalidationOffsets[invalidations++]=at;
        } else if(p.kind==Pm4Kind::Command && p.opcode==0x3C){
            if(waits++ || statusWrites!=1 || rangeWrites!=1 || action.kind!=DrawCommandKind::Wait || action.detailIndex ||
                action.priorDraws!=1 || p.payloadWords!=5 || p.predicated || plan.commands.words()[at]!=0xC0043C00 ||
                v[0]!=3 || v[1]!=0xA31 || v[2] || v[3]!=0x80000000 || v[4]!=8 || at!=candidate.depthRangeOffset+3 ||
                plan.waits[0].offsetWords!=at)return false;
            const auto& regs=plan.waits[0].registers;
            for(const auto& pair:std::array<std::array<unsigned,2>,4>{{{{0xA31,0x10000}},{{0xA2F,0}},{{0xA30,0x1000}},{{0x200D,0}}}}){
                const auto found=regs.find(pair[0]);
                if(found==regs.end() || !found->second.bits.equals(pair[1]) || found->second.lastAssignmentOffset>=at)return false;
            }
            candidate.depthWaitOffset=at;
        }
    }
    if(leadingIdle!=(phase==NativeColorBatchPhase::PostOutput) || legacySetup!=leadingIdle ||
        (leadingIdle && candidate.legacySetupOffset>=candidate.invalidationOffsets[0]) ||
        statusWrites!=1 || rangeWrites!=1 || waits!=1 || invalidations!=3 || idles!=3 ||
        !(candidate.invalidationOffsets[0]<plan.draws[0].offsetWords &&
          plan.draws[0].offsetWords<candidate.depthStatusOffset && candidate.depthWaitOffset<plan.draws[1].offsetWords &&
          plan.draws[2].offsetWords<candidate.invalidationOffsets[1] && candidate.invalidationOffsets[1]<candidate.idleCleanOffsets[0] &&
          candidate.idleCleanOffsets[0]<plan.draws[3].offsetWords && plan.draws[3].offsetWords<candidate.cacheFlushOffset &&
          candidate.cacheFlushOffset<candidate.idleCleanOffsets[1] && candidate.idleCleanOffsets[1]<candidate.invalidationOffsets[2] &&
          candidate.invalidationOffsets[2]<candidate.idleCleanOffsets[2] && candidate.idleCleanOffsets[2]<events.packetOffsets[0]))return false;
    output=candidate;return true;
}
}
