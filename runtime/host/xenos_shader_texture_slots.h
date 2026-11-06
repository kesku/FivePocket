#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace recomp::gpu {
// Diagnostic resource selection from bounded straight-line pixel microcode.
// This does NOT analyze ALU semantics, validate shader linkage, execute fetches,
// or authorize any sampler/command. Unknown control/fetch forms reject rather
// than guessing a resource set. Fields match Xenia ucode.h EXEC/ALLOC/TEX_FETCH.
inline std::optional<std::uint32_t> inspectStraightLineTextureSlots(
        const std::uint8_t* bytes,std::size_t size,bool captureFetchAll=false,
        bool captureSamplingVariants=false) noexcept {
    if(!bytes || !size || size%12 || size>4096)return std::nullopt;
    const auto word=[&](unsigned i) {
        const auto* p=bytes+4*i;return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
            (std::uint32_t{p[2]}<<8)|p[3];
    };
    struct Control {unsigned address=0,count=0,sequence=0;};
    std::array<Control,128> controls{};unsigned controlCount=0,executed=0;
    const auto extent=static_cast<unsigned>(size/12);auto body=extent;bool ended=false;
    for(unsigned pair=0;pair<body && !ended;++pair) {
        const auto a=word(pair*3),b=word(pair*3+1),c=word(pair*3+2);
        for(auto cf:std::array<std::uint64_t,2>{a|(std::uint64_t{b&65535}<<32),(b>>16)|(std::uint64_t{c}<<16)}) {
            if(controlCount==controls.size())return std::nullopt;
            auto& out=controls[controlCount++];const auto op=cf>>44;
            if(op==0){if(cf)return std::nullopt;}
            else if(op==12) {
                if(cf&~((std::uint64_t{15}<<44)|(std::uint64_t{7}<<40)|7) || ((cf>>41)&3)!=2)return std::nullopt;
            }else if(op==1 || op==2) {
                if(cf&~((std::uint64_t{15}<<44)|(std::uint64_t{1}<<43)|(std::uint64_t{1}<<41)|((std::uint64_t{1}<<34)-1)))return std::nullopt;
                out={static_cast<unsigned>(cf&4095),static_cast<unsigned>((cf>>12)&7),static_cast<unsigned>((cf>>16)&4095)};
                if(!out.count || out.count>6 || out.address+out.count>extent || out.sequence>>(2*out.count))return std::nullopt;
                if(out.address<body)body=out.address;
                ended=op==2;
            }else return std::nullopt;
            if(ended)break;
        }
    }
    if(!ended || (controlCount+1)/2>body)return std::nullopt;
    std::uint32_t slots=0;unsigned fetches=0,gradients=0,lodSets=0;
    for(unsigned i=0;i<controlCount;++i)for(unsigned j=0;j<controls[i].count;++j) {
        if(++executed>256)return std::nullopt;
        if(!(controls[i].sequence&(1u<<(2*j))))continue;
        const auto at=(controls[i].address+j)*3;
        const auto a=word(at),b=word(at+1),c=word(at+2);
        // Canonical setTexLOD names no texture. The following register-LOD
        // fetch still names its own constant slot. Capture-only recognition:
        // no expression, sampler or GPU execution authority is granted here.
        if((a&31)==24 && captureSamplingVariants) {
            // Xenia ParseTextureFetchInstruction marks setTexLOD has_dest=false.
            // Its encoded destination index is unused (observed nonzero).
            constexpr std::uint32_t variable=(63u<<5)|(63u<<12)|(63u<<26);
            if((a&~variable)!=(24u|(1u<<19)) || (a>>26)!=((a>>26)&3)*21 ||
               b!=0x0F1FFFFF || c || ++lodSets>16)return std::nullopt;
            continue;
        }
        // Canonical getGradients reads interpolators, not a texture slot.
        // Recognize its exact captured modifiers so a mixed TEX_FETCH/gradient
        // shader can retain its REAL texture inputs. This grants no derivative
        // execution semantics; malformed/unknown fetch forms still reject.
        if((a&31)==18) {
            constexpr std::uint32_t variable=(63u<<5)|(63u<<12)|(15u<<26);
            if((a&~variable)!=(18u|(1u<<19)) || (b&~4095u)!=0x1F1FF000 || c || ++gradients>16)return std::nullopt;
            for(unsigned lane=0;lane<4;++lane)if(((b>>(3*lane))&7)==6)return std::nullopt;
            continue;
        }
        // Resource-slot identity is unchanged by the valid-only/fetch-all bit.
        // Explicit diagnostic capture may retain either; this grants no native
        // helper-invocation or sampler execution semantics.
        const std::uint32_t variable=(63u<<5)|(63u<<12)|(31u<<20)|(63u<<26)|(captureFetchAll?1u<<19:0u);
        // Xenia ucode.h: signed XY offsets occupy word2[16:25]; they do not
        // alter slot identity. Keep Z, bias, gradients, predication and filter
        // overrides rejected. The old inspection contract is unchanged.
        const bool implicit=(b&~4095u)==0x1F1FF000 &&
            (c&~(captureSamplingVariants?0x03FF0000u:0u))==0x4000;
        const bool explicitLod=captureSamplingVariants && lodSets &&
            (b&~4095u)==0x2F1FF000 && c==0x4000;
        if((a&~variable)!=(captureFetchAll?1u:1u|(1u<<19)) ||
           (!implicit && !explicitLod) || ++fetches>32)return std::nullopt;
        for(unsigned lane=0;lane<4;++lane)if(((b>>(3*lane))&7)==6)return std::nullopt;
        slots|=1u<<((a>>20)&31);
    }
    return slots;
}
// Capture policy is independent of whether the original title is recording a
// tiled pass or executing a live draw. Both fetch forms name the same resource;
// retaining its owned bytes must not confer helper-lane/sampling permission.
inline std::optional<std::uint32_t> inspectCapturedPixelTextureSlots(
        const std::uint8_t* bytes,std::size_t size) noexcept {
    return inspectStraightLineTextureSlots(bytes,size,true,true);
}
}
