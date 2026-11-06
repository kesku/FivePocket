#pragma once
#include "xenos_draw_bin_predicate.h"
#include <functional>
#include <optional>
#include <span>

namespace recomp::gpu {
// The original resolve's predicated REG_TO_MEM of RB_COPY_DEST_BASE.
// Read the CURRENT register journal; never substitute the command parser's
// predicted value. This writes private/owned storage through the caller, not
// a fence, resource completion or command-ring receipt.
class NativeTileRegisterWriteback final {
public:
    using Read=std::function<std::optional<unsigned>(unsigned)>;
    using Publish=std::function<bool(unsigned,const std::array<std::uint8_t,4>&)>;
    static std::optional<NativeTileRegisterWriteback> parse(std::span<const std::uint8_t> bytes) noexcept {
        if(bytes.size()!=12)return std::nullopt;
        const auto word=[&](unsigned at) {
            return (unsigned(bytes[at])<<24)|(unsigned(bytes[at+1])<<16)|
                (unsigned(bytes[at+2])<<8)|bytes[at+3];
        };
        if(word(0)!=0xC0015501 || word(4)!=0x2319 || word(8)%4 || word(8)>=0x20000000)return std::nullopt;
        return NativeTileRegisterWriteback(word(8));
    }
    unsigned targetPhysical()const noexcept{return target_;}
    bool execute(const DrawBinState& bins,const Read& read,const Publish& publish)const {
        if(!read || !publish)return false;
        const auto selected=evaluateDrawBinPredicate(bins,true);
        if(selected==DrawBinDecision::Skip)return true;
        if(selected!=DrawBinDecision::Pass)return false;
        const auto value=read(0x2319);if(!value)return false;
        // Endian selector0 in this exact packet: little-endian output bytes,
        // independent of the host's endian and the big-endian command stream.
        return publish(target_,{std::uint8_t(*value),std::uint8_t(*value>>8),
            std::uint8_t(*value>>16),std::uint8_t(*value>>24)});
    }
private:
    explicit NativeTileRegisterWriteback(unsigned target):target_(target){}
    unsigned target_;
};
}
