#pragma once
#include "native_screen_strip_plan.h"
#include "native_edram32_bands.h"
#include "xenos_color_7e3.h"

namespace recomp::gpu {
// A grid proof, not a generic triangle overlap heuristic. On the observed
// five-band, eight-cell strip, every nondegenerate triangle is exactly one
// half of a cell. Strictly ordered axes and identical seams make interiors
// disjoint. The repeated endpoints keep the remaining18 triangles degenerate.
// Thus a single-sample pixel blends at most once (native top-left rule), and
// final7e3 encoding is not postponed past a second fragment at the same pixel.
// Call with CURRENT fenced outputs of the same VS/inputs used by the draw.
inline bool nativeScreenStripSingleCoverage(const std::vector<std::array<float,4>>& p) {
    if(p.size()!=100)return false;
    for(const auto& v:p)if(std::any_of(v.begin(),v.end(),[](float f){return !std::isfinite(f);}) ||
        v[3]!=1.f || v[2]<0.f || v[2]>1.f)return false;
    std::array<float,9> x{};std::array<float,6> y{};
    for(unsigned c=0;c<9;++c)x[c]=p[1+2*c][0];
    for(unsigned r=0;r<5;++r)y[r]=p[20*r+1][1];
    y[5]=p[98][1];
    if(x.front()!=-1.f || x.back()!=1.f || y.front()!=1.f || y.back()!=-1.f)return false;
    for(unsigned c=1;c<9;++c)if(!(x[c]>x[c-1]))return false;
    for(unsigned r=1;r<6;++r)if(!(y[r]<y[r-1]))return false;
    for(unsigned r=0;r<5;++r) {
        const unsigned base=20*r;
        if(p[base]!=p[base+1] || p[base+18]!=p[base+19])return false;
        for(unsigned c=0;c<9;++c) {
            const auto& top=p[base+1+2*c];const auto& bottom=p[base+2+2*c];
            if(top[0]!=x[c] || bottom[0]!=x[c] || top[1]!=y[r] || bottom[1]!=y[r+1])return false;
            if(r && top!=p[base-20+2+2*c])return false;
        }
    }
    return true;
}
struct NativeScreenStripReadback {
    std::vector<float> rgba;
    std::vector<std::uint32_t> depthStencil;
};
// Current physical owner -> decode -> separately fenced GPU consumer -> pack
// -> SAME physical owner. No guest texture/display/ring publication. The
// caller holds its exclusive gate and invalidates overlapping native views.
// Any refusal/throw leaves live ownership unknown via the existing transaction.
template<class Execute>
bool executeNativeScreenStripPhysical(NativeEdram32Mutation& mutation,const NativeScreenStripPlan& p,
        const std::vector<std::array<float,4>>& positions,Execute&& execute,const char** why=nullptr) {
    const auto no=[&](const char* text){if(why)*why=text;return false;};
    if(p.width!=1280 || p.height!=720 || p.pitch!=1280 || p.colorBase!=720 || p.depthBase!=0 ||
       p.expandedVertices!=294 || !p.colorObject || !p.depthObject || p.colorObject==p.depthObject)
        return no("physical-layout");
    if(!nativeScreenStripSingleCoverage(positions))return no("single-coverage-grid");
    auto& owner=mutation.candidate();
    const auto color=owner.readColorWords(p.colorBase,p.pitch,p.width,p.height);
    const auto depth=owner.readDepthWords(p.depthBase,p.pitch,p.width,p.height);
    if(!color || !depth)return no("physical-source-unknown");
    const auto pixels=color->size();std::vector<float> input(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        const auto components=unpackColor7e3((*color)[i]);
        for(unsigned c=0;c<4;++c)input[i*4+c]=colorFloat(components[c]);
    }
    if(!owner.invalidate(p.colorBase,720))return no("physical-invalidate");
    const auto output=execute(input,*depth);
    if(!output || output->rgba.size()!=input.size() || output->depthStencil!=*depth)
        return no("fenced-readback-or-depth-preservation");
    std::vector<std::uint8_t> bytes(pixels*4);
    for(std::size_t i=0;i<pixels;++i) {
        // Alpha writes are disabled. Preserve the original encoded two bits,
        // never re-quantize a fraction such as1/3 after an unrelated RGB draw.
        if(colorBits(output->rgba[4*i+3])!=colorBits(input[4*i+3]))return no("alpha-preservation");
        std::uint32_t raw=(*color)[i]&0xC0000000u;
        for(unsigned c=0;c<3;++c) {
            std::uint32_t code=0;
            if(!pack7e3Checked(colorBits(output->rgba[4*i+c]),code))return no("7e3-finite-rounding");
            raw|=code<<(10*c);
        }
        for(unsigned c=0;c<4;++c)bytes[4*i+c]=raw>>(8*c);
    }
    if(!owner.recordCompletedRawColorRows(p.colorBase,p.pitch,p.width,p.height,bytes,owner.nextSerial()))
        return no("physical-publication");
    mutation.commit();if(why)*why="fenced-rgb7e3-depth-stencil-alpha-preserved";return true;
}
}
