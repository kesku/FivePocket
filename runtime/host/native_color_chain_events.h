#pragma once
#include "native_gpu_word_updates.h"
#include "xenos_draw_command_plan.h"

namespace recomp::gpu {
struct NativeColorChainEventRequests {
    NativeWordUpdatePlan staging;
    std::array<std::uint32_t,2> physicalAddresses{},packetOffsets{};
    std::uint32_t cacheFlushOffset=0;
};
// Select ONLY the current final-event request profile from an already inspected
// child: four draw requests, exact CACHE_FLUSH, then two literal BE PS_DONE_TS.
// Address/value extraction agrees with event_write.py's pinned Xenia reference.
// These are packet literals, not sampled hardware counters or wall-clock time.
//
// This is not permission to submit, flush or publish: every preceding draw,
// register side effect (including EVENT_INITIATOR), WAIT, invalidation, cache
// flush and live ownership/alias constraint remains the executor's obligation.
// Native offsets 16/20 refer only to the private 40-byte staging buffer.
inline bool planNativeColorChainFinalEvents(const DrawCommandPlan& plan,
        NativeColorChainEventRequests& output) noexcept {
    if (plan.draws.size()!=4 || plan.commands.words().size()>4096 ||
        plan.commands.packets().size()>512 || plan.actions.size()!=plan.commands.packets().size()) return false;
    NativeColorChainEventRequests candidate;
    std::array<NativeWordRequest,2> requests{};
    unsigned draws=0,events=0,flushes=0;
    for (std::size_t i=0;i<plan.commands.packets().size();++i) {
        const auto& p=plan.commands.packets()[i];
        const auto at=p.offsetWords;
        if (plan.actions[i].offsetWords!=at || plan.actions[i].priorDraws!=draws) return false;
        if (p.kind!=Pm4Kind::Command) continue;
        const auto* v=plan.commands.words().data()+at+1;
        if (p.opcode==0x22 || p.opcode==0x36) {
            if (draws==4 || flushes || events || plan.draws[draws].offsetWords!=at) return false;
            ++draws;
        } else if (p.opcode==0x46) {
            if (p.predicated || p.payloadWords!=1 || plan.commands.words()[at]!=0xC0004600 ||
                v[0]!=6 || draws!=4 || flushes || events) return false;
            candidate.cacheFlushOffset=at; ++flushes;
        } else if (p.opcode==0x58) {
            if (p.predicated || p.payloadWords!=3 || plan.commands.words()[at]!=0xC0025800 ||
                v[0]!=3 || (v[1]&3)!=2 || (v[1]&~3u)>0x1FFFFFFC || draws!=4 || flushes!=1 || events==2) return false;
            const auto address=v[1]&~3u;
            if (events && address==candidate.physicalAddresses[0]) return false;
            candidate.physicalAddresses[events]=address; candidate.packetOffsets[events]=at;
            requests[events]={16+events*4,v[2],NativeWordPhase::AfterGraphics}; ++events;
        }
    }
    if (draws!=4 || flushes!=1 || events!=2 || !planNativeWordUpdates(requests.data(),2,40,candidate.staging)) return false;
    output=candidate; return true;
}
}
