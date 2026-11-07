#pragma once
#include "native_deferred_swap_submission.h"
#include "xenos_draw_command_plan.h"

namespace recomp::gpu {
// Requests before the native surface tail, NOT the standalone color chain's
// completion. There are no literal final events, post-copy idle or 0x7FFF
// invalidation in this prefix. No old completion receipt can discharge this.
struct NativeDeferredColorSyncRequests {
    unsigned leadingIdleOffset=0,legacySetupOffset=0,depthStatusOffset=0,
        depthRangeOffset=0,depthWaitOffset=0,copyIdleOffset=0,cacheFlushOffset=0;
    std::array<unsigned,2> invalidationOffsets{};
};
inline bool planNativeDeferredColorSync(const NativeDeferredSwapSubmission& frame,
        const DrawCommandPlan& plan,NativeDeferredColorSyncRequests& output) noexcept {
    if(plan.commands.words()!=frame.prefix().words() || plan.draws.size()!=4 || plan.waits.size()!=1 ||
        plan.actions.size()!=plan.commands.packets().size() || plan.waits[0].priorDraws!=1)return false;
    NativeDeferredColorSyncRequests candidate;
    unsigned draws=0,invalidations=0,status=0,ranges=0,waits=0,copyIdles=0,flushes=0;
    bool leading=false,setup=false;
    for(unsigned i=0;i<plan.commands.packets().size();++i){
        const auto& p=plan.commands.packets()[i];const auto& action=plan.actions[i];
        const auto at=p.offsetWords;const auto* v=plan.commands.words().data()+at+1;
        if(action.offsetWords!=at || action.priorDraws!=draws)return false;
        if(p.kind==Pm4Kind::RegisterSequence || p.kind==Pm4Kind::RegisterPair){
            bool low=false;
            for(unsigned n=0;n<p.payloadWords;++n){
                const auto reg=p.kind==Pm4Kind::RegisterPair?(n?p.secondRegister:p.firstRegister):p.firstRegister+(p.repeatRegister?0:n);
                low|=reg<0x2000;
            }
            if(!low)continue;
            if(p.kind!=Pm4Kind::RegisterSequence || p.repeatRegister || action.kind!=DrawCommandKind::RegisterWrite)return false;
            if(p.firstRegister==0x5C8){
                if(p.payloadWords!=1 || plan.commands.words()[at]!=0x5C8 || v[0]!=0x20000)return false;
                if(!draws){
                    if(leading || setup || invalidations)return false;
                    leading=true;candidate.leadingIdleOffset=at;
                }else{
                    if(draws!=3 || copyIdles++ || invalidations!=2 || flushes)return false;
                    candidate.copyIdleOffset=at;
                }
            }else if(p.firstRegister==0xF01){
                if(!leading || setup || draws || invalidations || p.payloadWords!=1 ||
                    plan.commands.words()[at]!=0xF01 || v[0]!=0x200E || at!=candidate.leadingIdleOffset+2)return false;
                setup=true;candidate.legacySetupOffset=at;
            }else if(p.firstRegister==0xA31){
                if(status++ || ranges || waits || draws!=1 || p.payloadWords!=1 ||
                    plan.commands.words()[at]!=0xA31 || v[0]!=0x10000)return false;
                candidate.depthStatusOffset=at;
            }else if(p.firstRegister==0xA2F){
                if(ranges++ || status!=1 || waits || draws!=1 || p.payloadWords!=2 ||
                    plan.commands.words()[at]!=0x10A2F || v[0] || v[1]!=0x1000 || at!=candidate.depthStatusOffset+2)return false;
                candidate.depthRangeOffset=at;
            }else return false;
        }else if(p.kind==Pm4Kind::Command){
            if(p.opcode==0x22 || p.opcode==0x36){
                if(draws==4 || flushes || action.kind!=DrawCommandKind::Draw || action.detailIndex!=draws ||
                    plan.draws[draws].offsetWords!=at)return false;
                ++draws;
            }else if(p.opcode==0x21){
                if(!p.payloadWords || v[0]<0x2000)return false;
            }else if(p.opcode==0x3B){
                if(invalidations==2 || !setup || p.payloadWords!=1 || p.predicated ||
                    plan.commands.words()[at]!=0xC0003B00 || v[0]!=(invalidations?0x100u:0x300u) ||
                    draws!=(invalidations?3u:0u) || action.kind!=DrawCommandKind::Invalidate)return false;
                candidate.invalidationOffsets[invalidations++]=at;
            }else if(p.opcode==0x3C){
                if(waits++ || status!=1 || ranges!=1 || draws!=1 || action.kind!=DrawCommandKind::Wait || action.detailIndex ||
                    p.payloadWords!=5 || p.predicated || plan.commands.words()[at]!=0xC0043C00 ||
                    v[0]!=3 || v[1]!=0xA31 || v[2] || v[3]!=0x80000000 || v[4]!=8 ||
                    at!=candidate.depthRangeOffset+3 || plan.waits[0].offsetWords!=at)return false;
                const auto& regs=plan.waits[0].registers;
                for(const auto& pair:std::array<std::array<unsigned,2>,4>{{{{0xA31,0x10000}},{{0xA2F,0}},{{0xA30,0x1000}},{{0x200D,0}}}}){
                    const auto found=regs.find(pair[0]);
                    if(found==regs.end() || !found->second.bits.equals(pair[1]) || found->second.lastAssignmentOffset>=at)return false;
                }
                candidate.depthWaitOffset=at;
            }else if(p.opcode==0x46){
                if(flushes++ || draws!=4 || action.kind!=DrawCommandKind::Event || p.payloadWords!=1 || p.predicated ||
                    plan.commands.words()[at]!=0xC0004600 || v[0]!=6)return false;
                candidate.cacheFlushOffset=at;
            }else if(p.opcode==0x58)return false; // No prefix completion events.
        }
    }
    if(!leading || !setup || draws!=4 || invalidations!=2 || status!=1 || ranges!=1 || waits!=1 || copyIdles!=1 || flushes!=1 ||
        !(candidate.legacySetupOffset<candidate.invalidationOffsets[0] && candidate.invalidationOffsets[0]<plan.draws[0].offsetWords &&
          plan.draws[0].offsetWords<candidate.depthStatusOffset && candidate.depthWaitOffset<plan.draws[1].offsetWords &&
          plan.draws[2].offsetWords<candidate.invalidationOffsets[1] && candidate.invalidationOffsets[1]<candidate.copyIdleOffset &&
          candidate.copyIdleOffset<plan.draws[3].offsetWords && plan.draws[3].offsetWords<candidate.cacheFlushOffset))return false;
    output=candidate;return true;
}
}
