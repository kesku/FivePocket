#pragma once
#include "xenos_command_stream.h"
#include "native_gpu_word_updates.h"
#include "xenos_scratch_writeback.h"
#include <algorithm>
#include <map>
#include <optional>

namespace recomp::gpu {
// A bounded no-draw state transaction, not a general command processor. State
// survives for later rendering; events require real native writes and a fence.
// RB_BC_CONTROL keeps the explicit experimental initialization contract.
struct NativeStateBatch {
    std::map<std::uint32_t,std::uint32_t> registers;
    // mask low/high, selector low/high. Absent words preserve preceding state.
    std::array<std::optional<std::uint32_t>,4> bins{};
    std::array<std::uint32_t,64> destinations{};
    NativeWordUpdatePlan updates;
    std::uint32_t stateWrites = 0, waits = 0, invalidations = 0;
    bool experimentalIdleCachePair = false;
};
inline bool planNativeStateBatch(const std::vector<std::uint32_t>& words,
        NativeStateBatch& output, Pm4Error& error, bool allowExperimentalIdleCachePair = false) {
    const auto fail=[&](std::uint32_t offset, std::uint32_t opcode=0) {
        error={Pm4Failure::UnsupportedOpcode,offset,opcode}; return false;
    };
    if (words.empty() || words.size()>256) return fail(0);
    std::array<std::uint8_t,1024> bytes{};
    for (std::size_t i=0;i<words.size();++i)
        for (unsigned b=0;b<4;++b) bytes[i*4+b]=words[i]>>(24-b*8);
    Pm4Snapshot snapshot;
    if (!decodePm4(bytes.data(),words.size()*4,{256,128,false},snapshot,error)) return false;
    NativeStateBatch candidate;
    // An explicit hypothesis, NOT verified Xenos register semantics. Related
    // Yamato TC_CNTL_STATUS(0E00).bit0 is L2_INVALIDATE, but 0E40 is unknown.
    // Only this complete observed idle/pair sequence gets the experimental
    // empty-resident-cache treatment; isolated/mixed/reordered writes reject.
    constexpr std::array<std::uint32_t,6> IdleCachePair{0x5C8,0x20000,0xE00,1,0xE40,1};
    if (allowExperimentalIdleCachePair && words.size()==IdleCachePair.size() &&
            std::equal(words.begin(),words.end(),IdleCachePair.begin())) {
        candidate.stateWrites=3; candidate.waits=1; candidate.experimentalIdleCachePair=true;
        // Request bits aren't retained as readable hardware status. No guest
        // payload upload, event or draw belongs to this transaction.
        if (!planNativeWordUpdates(nullptr,0,4,candidate.updates)) return fail(0);
        output=std::move(candidate); error={}; return true;
    }
    std::array<NativeWordRequest,64> requests{};
    std::size_t count=0;
    for (const auto& p : snapshot.packets()) {
        const auto at=p.offsetWords;
        const auto* data=words.data()+at+1;
        if (p.predicated) return fail(at,p.opcode);
        // The diagnostic decoder intentionally exposes framing without judging
        // reserved bits. An executable profile must also require canonical headers.
        if ((p.kind==Pm4Kind::Command && words[at]!=(0xC0000000u|((p.payloadWords-1)<<16)|(p.opcode<<8))) ||
            (p.kind==Pm4Kind::RegisterPair && words[at]!=(0x40000000u|p.firstRegister|(p.secondRegister<<11))) ||
            (p.kind==Pm4Kind::Filler && words[at]!=0x80000000)) return fail(at,p.opcode);
        if (p.kind==Pm4Kind::RegisterSequence || p.kind==Pm4Kind::RegisterPair) {
            for (std::uint32_t i=0;i<p.payloadWords;++i) {
                const auto index=p.kind==Pm4Kind::RegisterPair ? (i?p.secondRegister:p.firstRegister) :
                    p.firstRegister+(p.repeatRegister?0:i);
                if (index>=0x2080 && index<=0x2082) {
                    // Raw window/scissor bits are retained, not rasterized here.
                } else if (index==0x5C8 && data[i]==0x20000) {
                    ++candidate.waits; // native all-prior-work fence, no vblank.
                } else if ((index==0xF01 && data[i]==0x200E) ||
                    (index==0xD02 && data[i]==0x10800) || (index==0xD04 && data[i]==0) ||
                    (index==0xA02 && data[i]==0xC0100000) || (index==0xA03 && data[i]==0x07F00000) ||
                    (index==0xA04 && data[i]==0xC0000000) || (index==0xA05 && data[i]==0x00100000)) {
                    // Same explicit native legacy-setup contract as cold init.
                } else if ((index==0x1DD && !(data[i]&3) && data[i]<0x20000000) ||
                           (index==0x1DC && scratchWritebackBigEndianProfile(data[i]))) {
                    // Retain base/mask only; neither write itself requests a
                    // scratch writeback. Actual SCRATCH_REG writes still reject.
                } else return fail(at);
                candidate.registers[index]=data[i]; ++candidate.stateWrites;
            }
            continue;
        }
        if (p.kind==Pm4Kind::Filler) continue;
        if (p.kind!=Pm4Kind::Command) return fail(at);
        if (p.opcode>=0x60 && p.opcode<=0x63 && p.payloadWords==1) {
            candidate.bins[p.opcode-0x60]=data[0]; ++candidate.stateWrites;
        } else if (p.opcode==0x10) {
            // NOP payload has no command semantics; framing was checked above.
        } else if (p.opcode==0x3B && p.payloadWords==1 && data[0]==0x7FFF) {
            ++candidate.invalidations;
        } else if (p.opcode==0x58 && p.payloadWords==3 && data[0]==3 &&
                (data[1]&3)==2 && (data[1]&~3u)<0x20000000 && count<requests.size()) {
            const auto address=data[1]&~3u;
            for (std::size_t i=0;i<count;++i) if (candidate.destinations[i]==address) return fail(at,p.opcode);
            candidate.destinations[count]=address;
            requests[count]={static_cast<std::uint32_t>(count*4),data[2],NativeWordPhase::AfterGraphics};
            ++count;
            candidate.registers[0x21F9]=3; ++candidate.stateWrites;
        } else return fail(at,p.opcode); // draws, shaders, IBs, reads, IRQs, resolves.
    }
    if (!planNativeWordUpdates(requests.data(),count,static_cast<std::uint32_t>(std::max<std::size_t>(4,count*4)),candidate.updates))
        return fail(0);
    output=std::move(candidate); error={}; return true;
}
}
