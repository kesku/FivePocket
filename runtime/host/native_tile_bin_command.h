#pragma once
#include "native_graphics_state.h"
#include "xenos_draw_bin_predicate.h"
#include <optional>
#include <span>

namespace recomp::gpu {
// One canonical, unpredicated SET_BIN_{MASK,SELECT}_{LO,HI} packet.
// Original worker 836B1570 enqueues SELECT_LO; scene packets update MASK.
// The caller supplies the current owned packet at its real execution position.
// This is a state effect only: no omitted command, scratch, IRQ, GPU submission,
// memory publication, ring counter or whole-scene completion is acknowledged.
class NativeTileBinCommand final {
public:
    static std::optional<NativeTileBinCommand> parse(std::span<const std::uint8_t> bytes) noexcept {
        if(bytes.size()!=8)return std::nullopt;
        const auto word=[&](unsigned at) {
            return (std::uint32_t{bytes[at]}<<24)|(std::uint32_t{bytes[at+1]}<<16)|
                (std::uint32_t{bytes[at+2]}<<8)|bytes[at+3];
        };
        const auto header=word(0);
        for(unsigned index=0;index<4;++index)
            if(header==0xC0006000u+(index<<8))return NativeTileBinCommand(index,word(4));
        return std::nullopt;
    }
    bool apply(NativeGraphicsState& state) const noexcept {
        DrawBinState current{{static_cast<unsigned>(state.binMask),static_cast<unsigned>(state.binMask>>32),
            static_cast<unsigned>(state.binSelect),static_cast<unsigned>(state.binSelect>>32)},state.knownBinWords};
        if(evaluateDrawBinPredicate(current,false)==DrawBinDecision::InvalidState)return false;
        auto& target=index_<2?state.binMask:state.binSelect;
        const unsigned shift=(index_&1)*32;
        target=(target&~(std::uint64_t{UINT32_MAX}<<shift))|(std::uint64_t{value_}<<shift);
        state.knownBinWords|=1u<<index_;
        return true;
    }
    unsigned index() const noexcept{return index_;}
    std::uint32_t value() const noexcept{return value_;}
private:
    NativeTileBinCommand(unsigned index,std::uint32_t value):index_(index),value_(value) {}
    unsigned index_;
    std::uint32_t value_;
};
}
