#pragma once
#include "native_world_strip_plan.h"
#include "native_edram32_bands.h"
#include "xenos_color_7e3.h"

namespace recomp::gpu {
struct NativeWorldStripReadback {
    std::vector<float> rgba;
    std::vector<std::uint32_t> depthStencil;
};
// No source/destination blending: only the last surviving fragment contributes
// colour. Depth is encoded to20e4 BEFORE every fragment test by ABI29. Thus
// final7e3 encoding does not move rounding past a destination-dependent blend.
// The callback must execute the exact admitted original pair against these
// imported current contents and return FENCED results. Not a coverage proof.
template<class Execute>
bool executeNativeWorldStripPhysical(NativeEdram32Mutation& mutation,const NativeWorldStripPlan& p,
        Execute&& execute,const char** why=nullptr) {
    const auto no=[&](const char* text){if(why)*why=text;return false;};
    if(!p.colorObject || !p.depthObject || p.colorObject==p.depthObject ||
       p.depthStencil!=*decodeXenosDepthStencilState(0x708767,0xFFFF00,0))return no("physical-profile");
    auto& owner=mutation.candidate();
    const auto color=owner.readColorWords(720,1280,1280,720);
    const auto depth=owner.readDepthWords(0,1280,1280,720);
    if(!color || !depth)return no("physical-source-unknown");
    const auto pixels=color->size();std::vector<float> input(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        const auto rgba=unpackColor7e3((*color)[i]);
        for(unsigned c=0;c<4;++c)input[4*i+c]=colorFloat(rgba[c]);
    }
    if(!owner.invalidate(0,1440))return no("physical-invalidate");
    const auto result=execute(input,*depth);
    if(!result || result->rgba.size()!=input.size() || result->depthStencil.size()!=pixels)
        return no("fenced-readback");
    std::vector<std::uint8_t> bytes(pixels*4);
    // 2^-24 ->20e4 code0x400, stencil reference0. Each output is unchanged
    // (clip/discard/depth failure), or this EXACT permitted depth+stencil write.
    // A colour change must be accompanied by a legal passing depth operation.
    constexpr unsigned written=0x40000;
    for(std::size_t i=0;i<pixels;++i) {
        const auto before=(*depth)[i],after=result->depthStencil[i];
        std::array<unsigned,4> rgba{};for(unsigned c=0;c<4;++c)rgba[c]=colorBits(result->rgba[4*i+c]);
        unsigned packed=0;if(!packColor7e3(rgba,packed))return no("7e3-finite-rounding");
        const bool passing=(before>>8)<=0x400u;
        if(after!=before && (!passing || after!=written))return no("depth-stencil-effect");
        if(packed!=(*color)[i] && (!passing || after!=written))return no("color-depth-connection");
        for(unsigned c=0;c<4;++c)bytes[4*i+c]=packed>>(8*c);
    }
    if(!owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,owner.nextSerial()) ||
       !owner.recordCompletedDepthWords(0,1280,1280,720,result->depthStencil,owner.nextSerial()))
        return no("physical-publication");
    mutation.commit();if(why)*why="fenced-rgb7e3-depth20e4-stencil";return true;
}
}
