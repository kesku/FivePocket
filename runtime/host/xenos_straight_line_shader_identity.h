#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace recomp::gpu {
// Identity comparison against an ALREADY translated/validated program, not a
// shader validator or permission to execute unknown code. Preserve exact-match
// behavior; otherwise ignore only storage unreachable under a bounded sequence
// of NOP/ALLOC/unconditional EXEC ending in unconditional EXEC_END.
//
// Compare the complete control-flow pairs and every referenced instruction byte.
// Conditional end, jump, call, loop and unknown encodings have no fallback.
// An end marker is not a byte-length prefix: referenced instructions may follow
// gaps, and must all match. Full capture bytes and state/ownership checks remain
// unchanged. Word layout agrees with the offline schedule() parser and pinned
// ReX ucode ControlFlowExecInstruction; this is not GPU completion evidence.
inline bool sameXenosStraightLineShader(const std::vector<std::uint8_t>& known,
                                        const std::vector<std::uint8_t>& actual) {
    if(known.empty() || known.size()!=actual.size())return false;
    if(known==actual)return true;
    if(known.size()>4096 || known.size()%12)return false;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{known[at]}<<24)|(std::uint32_t{known[at+1]}<<16)|
            (std::uint32_t{known[at+2]}<<8)|known[at+3];
    };
    const auto equal=[&](unsigned at,unsigned count) {
        return std::equal(known.begin()+at,known.begin()+at+count,actual.begin()+at);
    };
    const unsigned extent=static_cast<unsigned>(known.size()/12);
    unsigned firstInstruction=extent,controls=0;
    for(unsigned pair=0;pair<firstInstruction;++pair) {
        const unsigned at=pair*12;
        if(!equal(at,12))return false;
        const auto a=word(at),b=word(at+4),c=word(at+8);
        const std::uint64_t cfPair[]{std::uint64_t{a}|((std::uint64_t{b}&0xFFFF)<<32),
                                    std::uint64_t{b>>16}|(std::uint64_t{c}<<16)};
        for(const auto cf:cfPair) {
            if(++controls>128)return false;
            const auto opcode=cf>>44;
            if(opcode==0) {
                if(cf)return false; // Canonical NOP only.
            } else if(opcode==12) {
                constexpr std::uint64_t fields=(15ULL<<44)|(7ULL<<40)|7ULL;
                const auto kind=(cf>>41)&3;
                if((cf&~fields) || (kind!=1 && kind!=2))return false;
            } else if(opcode==1 || opcode==2) {
                constexpr std::uint64_t fields=(15ULL<<44)|(1ULL<<43)|(1ULL<<41)|((1ULL<<34)-1);
                if(cf&~fields)return false;
                const unsigned address=cf&4095,count=(cf>>12)&7,sequence=(cf>>16)&4095;
                if(!count || count>6 || address+count>extent || (sequence>>(count*2)))return false;
                firstInstruction=std::min(firstInstruction,address);
                if(!equal(address*12,count*12))return false;
                if(opcode==2)return (controls+1)/2<=firstInstruction;
            } else return false;
        }
    }
    return false; // No unconditional end or overlapping control/instruction data.
}
}
