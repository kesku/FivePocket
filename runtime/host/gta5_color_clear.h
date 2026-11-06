#pragma once
#include "gta5_inline_draw.h"
#include "xenos_color_7e3.h"
#include <algorithm>

namespace recomp::gpu {
struct Gta5ColorClear {
    std::uint32_t target=0,packedColor=0;
    std::array<float,4> color{};
};
// Original 823D89A8: low four flag bits select color targets; r5 is a
// rectangle, r6 is float4 color. Viewport and enabled scissor intersect that
// rectangle. This first native target supports a full 1280x720 7e3 clear only;
// depth/stencil, additional MRTs, partial and binned clears remain unsupported.
inline std::optional<Gta5ColorClear> captureGta5ColorClear(std::uint32_t device,
        std::uint32_t flags,std::uint32_t rectangle,std::uint32_t color,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& argument) {
    if(!heap || !argument || !(flags&1) || (flags&~15u) || !rectangle || !color)return std::nullopt;
    const auto state=heap(device,13128),rect=argument(rectangle,16),rgba=argument(color,16);
    if(!state || state->size()!=13128 || !rect || rect->size()!=16 || !rgba || rgba->size()!=16)return std::nullopt;
    const auto word=[](const auto& data,unsigned at) {
        return (std::uint32_t{data[at]}<<24)|(std::uint32_t{data[at+1]}<<16)|
            (std::uint32_t{data[at+2]}<<8)|data[at+3];
    };
    // Surface pitch/sample state is a driver shadow. Read the bound surface's
    // format: Xenos storage format3 and expanded render view12 both store7e3.
    // ReXGlue c94f5eb xenos.h GetStorageColorFormat maps12 to3; the original
    // clear's 8237B3D8..8237B414 uses that same canonicalization.
    Gta5ColorClear out;out.target=word(*state,12816);
    const auto surface=out.target?heap(out.target,32):std::nullopt;
    if(word(*state,10368)!=0x14000500 || !surface || surface->size()!=32)return std::nullopt;
    const auto colorInfo=word(*surface,28);
    if(colorInfo!=0x302D0 && colorInfo!=0xC02D0)return std::nullopt;
    // 8237B644..8237B6E8: bit0x20 alone does not imply binning. It selects
    // binning only when every non-null bound surface matches the tile cache.
    const auto deviceFlags=(*state)[11068];
    bool binned=(deviceFlags&0x10)!=0;
    if(!binned && (deviceFlags&0x20)) {
        binned=true;
        for(unsigned i=0;i<5;++i)if(word(*state,12816+i*4) &&
            word(*state,12816+i*4)!=word(*state,13104+i*4))binned=false;
    }
    if(binned)return std::nullopt;
    for(unsigned i=1;i<4;++i)if((flags&(1u<<i)) && word(*state,12816+i*4))return std::nullopt;
    std::array<int,4> clipped{},viewport{};
    for(unsigned i=0;i<4;++i) {
        const float v=colorFloat(word(*state,13024+i*4));
        if(!std::isfinite(v) || v<-8192 || v>8192)return std::nullopt;
        viewport[i]=static_cast<int>(v);
        clipped[i]=static_cast<std::int32_t>(word(*rect,i*4));
    }
    viewport[2]+=viewport[0];viewport[3]+=viewport[1];
    for(unsigned i=0;i<4;++i)clipped[i]=i<2?std::max(clipped[i],viewport[i]):std::min(clipped[i],viewport[i]);
    if(word(*state,12288))for(unsigned i=0;i<4;++i) {
        const auto s=static_cast<std::int32_t>(word(*state,13052+i*4));
        clipped[i]=i<2?std::max(clipped[i],s):std::min(clipped[i],s);
    }
    if(clipped!=std::array<int,4>{0,0,1280,720})return std::nullopt;
    std::array<std::uint32_t,4> bits{};
    for(unsigned i=0;i<4;++i)bits[i]=word(*rgba,i*4);
    if(!packColor7e3(bits,out.packedColor))return std::nullopt;
    const auto rounded=unpackColor7e3(out.packedColor);
    for(unsigned i=0;i<4;++i)out.color[i]=colorFloat(rounded[i]);
    return out;
}
}
