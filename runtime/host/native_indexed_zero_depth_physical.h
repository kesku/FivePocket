#pragma once
#include "native_edram32_bands.h"
#include "xenos_color_7e3.h"

namespace recomp::gpu {
struct NativeIndexedZeroDepthReadback {
    std::vector<float> rgba;
    std::vector<std::uint32_t> depthStencil;
};
// Real fenced rendering is required even if the original depth rejects every
// sample. No draw completion may be inferred from authored/captured depth.
// Constant zero GEQUAL with no depth/stencil writes permits colour changes
// ONLY over CURRENT depth zero. Replacement allows packing after rasterization.
template<class Execute>
bool executeNativeIndexedZeroDepthPhysical(NativeEdram32Mutation& mutation,Execute&& execute,
        const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    auto& owner=mutation.candidate();
    const auto color=owner.readColorWords(720,1280,1280,720),depth=owner.readDepthWords(0,1280,1280,720);
    if(!color || !depth)return no("indexed-zero-depth-current-owner-unknown");
    const auto pixels=color->size();std::vector<float> rgba(pixels*4);
    for(std::size_t i=0;i<pixels;++i){const auto value=unpackColor7e3((*color)[i]);
        for(unsigned c=0;c<4;++c)rgba[4*i+c]=colorFloat(value[c]);}
    // Only colour may be written. Preserve the untouched depth allocation and
    // every unrelated physical tile, including unknown regions.
    if(!owner.invalidate(720,720))return no("indexed-zero-depth-invalidate");
    const auto result=execute(rgba,*depth);
    if(!result || result->rgba.size()!=rgba.size() || result->depthStencil!=*depth)
        return no("indexed-zero-depth-fenced-depth-stencil-preservation");
    std::vector<std::uint8_t> bytes(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        std::array<unsigned,4> value{};for(unsigned c=0;c<4;++c)value[c]=colorBits(result->rgba[4*i+c]);
        unsigned packed=0;if(!packColor7e3(value,packed))return no("indexed-zero-depth-finite-color");
        if(packed!=(*color)[i] && ((*depth)[i]>>8)!=0)return no("indexed-zero-depth-color-test-connection");
        for(unsigned c=0;c<4;++c)bytes[4*i+c]=packed>>(8*c);
    }
    if(!owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,owner.nextSerial()))
        return no("indexed-zero-depth-publication");
    mutation.commit();if(why)*why="fenced-indexed-rgb7e3-zero-depth-preserved-stencil";return true;
}
}
