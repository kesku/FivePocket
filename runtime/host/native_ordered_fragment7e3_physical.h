#pragma once
#include "native_edram32_bands.h"
#include "native_fragment7e3_depth.h"

namespace recomp::gpu {
struct NativeOrderedFragment7e3 {
    std::uint32_t primitive,pixel;
    NativeFragment7e3Depth sample;
};
struct NativeOrderedFragment7e3Batch {
    std::uint32_t primitiveCount;
    std::vector<NativeOrderedFragment7e3> fragments;
};
// Exact target-layout component (1280x720, single sample, color7e3 at tile720,
// depth20e4 at tile0), not an admitted guest shader/state or completion receipt.
// Execute MUST return all covered fragments from the CURRENT original shader,
// textures, indices and validated culling/scissor, AFTER a successful fence.
// Entries are sorted by primitive then pixel, with at most one per primitive
// per pixel. Different primitives may overlap: quantize and update BOTH owners
// after EACH passing fragment. Unknown owners, duplicate entries, incomplete
// execution or numeric refusal leave the live mutation unknown, never partial.
// No counts, query fences, guest ring or presentation ownership are published.
template<class Execute>
bool executeNativeOrderedFragment7e3Physical(NativeEdram32Mutation& mutation,
        Execute&& execute,const char** why=nullptr,
        Color7e3Rounding rounding=Color7e3Rounding::RequireNearestEvenAgreement) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(!validColor7e3Rounding(rounding))return no("ordered-fragment7e3-rounding-profile");
    auto& owner=mutation.candidate();
    auto color=owner.readColorWords(720,1280,1280,720);
    auto depth=owner.readDepthWords(0,1280,1280,720);
    if(!color || !depth)return no("ordered-fragment7e3-current-owner-unknown");
    if(!owner.invalidate(720,720) || !owner.invalidate(0,720))
        return no("ordered-fragment7e3-invalidate");
    const auto batch=execute();
    constexpr std::uint32_t pixels=1280*720;
    if(!batch || !batch->primitiveCount || batch->primitiveCount>65535 ||
       batch->fragments.size()>16*std::size_t{pixels})
        return no("ordered-fragment7e3-fenced-batch-extent");
    std::uint64_t previous=0;bool first=true;
    for(const auto& f:batch->fragments) {
        if(f.pixel>=pixels || f.primitive>=batch->primitiveCount)
            return no("ordered-fragment7e3-fragment-range");
        const auto key=std::uint64_t{f.primitive}*pixels+f.pixel;
        if(!first && key<=previous)return no("ordered-fragment7e3-order-or-duplicate");
        first=false;previous=key;
        const auto result=mergeNativeFragment7e3Depth(f.sample,(*color)[f.pixel],(*depth)[f.pixel],rounding);
        if(!result)return no("ordered-fragment7e3-numeric-policy");
        (*color)[f.pixel]=result->color;(*depth)[f.pixel]=result->depthStencil;
    }
    std::vector<std::uint8_t> bytes(std::size_t{pixels}*4);
    for(unsigned i=0;i<pixels;++i)for(unsigned c=0;c<4;++c)bytes[4*i+c]=(*color)[i]>>(8*c);
    if(!owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,owner.nextSerial()) ||
       !owner.recordCompletedDepthWords(0,1280,1280,720,*depth,owner.nextSerial()))
        return no("ordered-fragment7e3-paired-publication");
    mutation.commit();if(why)*why="fenced-ordered-fragment7e3-color-depth-complete";return true;
}
}
