#pragma once
#include "gta5_bound_surface_capture.h"
#include <span>

namespace recomp::gpu {
// Admission for ORIGINAL CPU allocation/recording, not a replacement allocator
// and never a GPU fence receipt. Caller parks writers and retains allocation
// ownership between this check and original execution. No state is changed here.
struct Gta5LocalGraphicsAllocation {
    std::uint32_t address=0,bytes=0,end=0,generation=0,counterAfter=0;
    bool wrapped=false;
};
// Exact first-read predicate in original823BC018. Only this no-wait path is
// admitted: never run its polling/import path with a made-up fence completion.
inline bool gta5LocalAllocationNeedsNoWait(std::uint32_t end,
        std::uint32_t generation,std::uint32_t fenceWord) {
    const auto delta=(generation-fenceWord)&3u;
    return delta==0 || (delta==1 && end<=(fenceWord&~3u));
}
inline std::optional<Gta5LocalGraphicsAllocation> describeGta5LocalGraphicsAllocation(
        std::span<const std::uint8_t> state,std::uint32_t words,std::uint32_t alignment,
        std::optional<std::uint32_t> fenceWord,const Gta5ResourceReader& owned) {
    // Six words/alignment4 is the observed resolve quad. Other bounded sizes
    // are checked for the same original823BBD00 ->823BBDE8 allocation-mode1.
    // Manager/callback mode, command-buffer allocation-mode0, counter overflow
    // and threshold-induced command-limit rewrites are deliberately deferred.
    if(state.size()!=15344 || !fenceWord || !owned || !words || words>4096 ||
       alignment<4 || alignment>4096 || (alignment&(alignment-1)))return std::nullopt;
    const auto word=[&](unsigned at){return (std::uint32_t{state[at]}<<24)|
        (std::uint32_t{state[at+1]}<<16)|(std::uint32_t{state[at+2]}<<8)|state[at+3];};
    if((state[11069]&0x20) || word(13920))return std::nullopt;
    const auto arenaBegin=word(15308),arenaEnd=word(15312);
    if(arenaBegin<0xA0000000 || arenaEnd>=0xC0000000 || arenaBegin>=arenaEnd ||
       ((arenaBegin|arenaEnd)&3))return std::nullopt;
    const auto aligned=[&](std::uint64_t cursor)->std::optional<std::uint32_t> {
        const auto at=(cursor+alignment-1)&~std::uint64_t{alignment-1};
        if(cursor<arenaBegin || at>=0xC0000000 || at>arenaEnd)return std::nullopt;
        return static_cast<std::uint32_t>(at);
    };
    const auto cursor=word(15320);
    auto start=aligned(cursor?cursor:std::uint64_t{word(52)}+68);
    if(!start)return std::nullopt;
    Gta5LocalGraphicsAllocation plan;
    plan.bytes=words*4;plan.generation=word(15324);
    const auto recorded=word(13600),recordedGeneration=word(13604);
    if(std::uint64_t{*start}+plan.bytes>arenaEnd) {
        start=aligned(word(15328));if(!start)return std::nullopt;
        plan.wrapped=true;++plan.generation; // Original modulo32 generation.
        if(std::uint64_t{*start}+plan.bytes>word(15332) ||
           word(15332)>arenaEnd)return std::nullopt;
    }
    const auto end=std::uint64_t{*start}+plan.bytes;
    if(end>arenaEnd || end>=0xC0000000)return std::nullopt;
    plan.address=*start;plan.end=static_cast<std::uint32_t>(end);
    if(recorded) {
        if(recorded<arenaBegin || recorded>arenaEnd || (recorded&3))return std::nullopt;
        const auto distance=recordedGeneration-plan.generation;
        if(distance==UINT32_MAX || (distance==0 && recorded<plan.end))return std::nullopt;
    }
    const auto counter=std::uint64_t{word(15336)}+plan.bytes;
    if(counter>UINT32_MAX || (counter>word(15340) && !recorded))return std::nullopt;
    plan.counterAfter=static_cast<std::uint32_t>(counter);
    if(!gta5LocalAllocationNeedsNoWait(plan.end,plan.generation,*fenceWord))return std::nullopt;
    const auto bytes=owned(plan.address,plan.bytes);
    if(!bytes || bytes->size()!=plan.bytes)return std::nullopt;
    return plan;
}
}
