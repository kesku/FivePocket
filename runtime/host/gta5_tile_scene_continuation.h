#pragma once
#include "gta5_tile_command_interrupt.h"
#include "xenos_draw_bin_predicate.h"

namespace recomp::gpu {
// Original EndTiling appends this 43-word tail to the scene indirect buffer:
// mask reset, first-pass mask, WAIT_UNTIL, predicated command interrupt, reset.
// Only the type-3 waits and interrupt are predicated. Scratch register writes,
// including acknowledgement/poison, STILL occur on later passes. Do not skip
// the whole tail when its interrupt is skipped. This is a read-only plan, not
// proof of rendering, bounds writeback, IRQ delivery, or ring retirement.
class Gta5TileSceneContinuation final {
public:
    static constexpr unsigned Words=43,Bytes=Words*4,RequestOffsetBytes=24;
    struct Selection {
        bool interruptRequired=false;
        std::array<unsigned,10> operationIndices{};
        unsigned operationCount=0;
        DrawBinState interruptBins{},finalBins{};
    };
    static std::optional<Gta5TileSceneContinuation> parse(unsigned physical,
            std::span<const std::uint8_t> bytes) noexcept {
        if(bytes.size()!=Bytes || (physical&3) || std::uint64_t{physical}+Bytes>0x20000000)
            return std::nullopt;
        const auto word=[&](unsigned i) {
            const auto*p=bytes.data()+i*4;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        };
        if(word(0)!=0xC0006000 || word(1)!=UINT32_MAX || word(2)!=0xC0006000 ||
           word(3)!=0x80000000 || word(4)!=0x5C8 || word(5)!=0x20000 ||
           word(41)!=0xC0006000 || word(42)!=UINT32_MAX)return std::nullopt;
        const auto request=Gta5TileCommandInterrupt::parseProfile(physical+RequestOffsetBytes,
            bytes.subspan(RequestOffsetBytes,Gta5TileCommandInterrupt::Bytes),true);
        if(!request)return std::nullopt;
        const auto overlaps=[&](unsigned address,unsigned size) {
            return std::uint64_t{physical}<std::uint64_t{address}+size &&
                std::uint64_t{address}<std::uint64_t{physical}+Bytes;
        };
        if(overlaps(request->scratchPhysical(),32) || overlaps(request->callbackContext()-0xC0000000,4))
            return std::nullopt;
        return Gta5TileSceneContinuation(*request);
    }
    const Gta5TileCommandInterrupt& request() const noexcept {return request_;}
    std::optional<Selection> select(const DrawBinState& initial) const noexcept {
        if(evaluateDrawBinPredicate(initial,false)==DrawBinDecision::InvalidState)return std::nullopt;
        Selection result;result.interruptBins=initial;
        result.interruptBins.words[0]=0x80000000;result.interruptBins.knownWords|=1;
        const auto decision=evaluateDrawBinPredicate(result.interruptBins,true);
        if(decision==DrawBinDecision::InvalidState || decision==DrawBinDecision::Unknown)return std::nullopt;
        result.interruptRequired=decision==DrawBinDecision::Pass;
        for(unsigned i=0;i<request_.operations().size();++i) {
            const auto kind=request_.operations()[i].kind;
            const bool conditional=kind==Gta5TileCommandInterrupt::Kind::WaitScratch ||
                kind==Gta5TileCommandInterrupt::Kind::Interrupt;
            if(!conditional || result.interruptRequired)result.operationIndices[result.operationCount++]=i;
        }
        result.finalBins=result.interruptBins;result.finalBins.words[0]=UINT32_MAX;
        return result;
    }
private:
    explicit Gta5TileSceneContinuation(const Gta5TileCommandInterrupt& request):request_(request) {}
    Gta5TileCommandInterrupt request_;
};
}
