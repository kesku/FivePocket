#pragma once
#include <array>
#include <cstdint>

namespace recomp::gpu {
enum class DrawBinDecision { InvalidState, Unknown, Skip, Pass };
struct DrawBinState {
    // Mask low/high, select low/high. Unknown words must have no fabricated bits.
    std::array<std::uint32_t,4> words{};
    std::uint32_t knownWords=0;
};
struct DrawBinPredicate {
    DrawBinState state;
    bool predicated=false;
};
// Native counterpart of bin_predicate.py's conservative reference policy.
// Xenia95a5c3ee ExecutePacketType3 uses nonzero(mask & select). This helper
// neither assumes reset values nor executes any draw, query, swap or command.
// A Pass is one admission prerequisite, never permission to retire a batch.
inline DrawBinDecision evaluateDrawBinPredicate(const DrawBinState& state,bool predicated) noexcept {
    if(state.knownWords&~15u)return DrawBinDecision::InvalidState;
    for(unsigned i=0;i<4;++i)
        if(!(state.knownWords&(1u<<i)) && state.words[i])return DrawBinDecision::InvalidState;
    if(!predicated)return DrawBinDecision::Pass;
    bool uncertain=false;
    for(unsigned i=0;i<2;++i){
        const bool maskKnown=(state.knownWords&(1u<<i))!=0,selectKnown=(state.knownWords&(1u<<(i+2)))!=0;
        if(maskKnown && selectKnown){
            if(state.words[i]&state.words[i+2])return DrawBinDecision::Pass;
        }else if((!maskKnown || state.words[i]) && (!selectKnown || state.words[i+2]))uncertain=true;
    }
    return uncertain?DrawBinDecision::Unknown:DrawBinDecision::Skip;
}
}
