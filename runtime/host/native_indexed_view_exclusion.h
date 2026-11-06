#pragma once
#include "native_vertex_clip.h"

namespace recomp::gpu {
struct NativeIndexedViewExclusion {
    unsigned triangles=0,behind=0,outsideXY=0;
};
// A sufficient geometric exclusion proof, not a rasterizer or Hi-Z model.
// Every ORIGINAL indexed triangle must be wholly behind W, or outside both
// one homogeneous XY view plane and one recorded screen/scissor half-plane.
// In particular, divided negative-W coordinates cannot prove exclusion.
// Unknown, crossing, near-boundary, degenerate-only and Z-only cases reject.
// The caller must bind these positions to a completed side-effect-free VS and
// validate the full original draw/target/program state. No publication here.
inline std::optional<NativeIndexedViewExclusion> nativeIndexedViewExclusion(
        const Gta5InlineDraw& draw,std::span<const std::array<float,4>> positions,
        std::span<const std::uint32_t> indices,const std::array<unsigned,4>& rectangle) {
    if(draw.primitive!=4 || !draw.vertexCount || draw.vertexCount>65536 ||
       positions.size()!=draw.vertexCount || indices.empty() || indices.size()>65535 || indices.size()%3 ||
       rectangle[0]>=rectangle[2] || rectangle[1]>=rectangle[3] || rectangle[2]>1280 || rectangle[3]>720 ||
       !nativeTranslatedViewport(draw))return std::nullopt;
    // Ordinary homogeneous triangle-list, no user planes or query kill modes.
    // Guard bands may enlarge XY clipping but cannot make an object separated
    // from the recorded screen rectangle touch its pixels.
    constexpr unsigned required[][2]{{0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},
        {0x2208,4},{0x2183,0},{0x2184,0}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return std::nullopt;
    const auto query=gta5DrawRegister(draw,0x2293);
    if(!query || (*query&~0x7Fu))return std::nullopt;
    for(const auto& p:positions)for(float f:p)
        if(!std::isfinite(f) || std::abs(f)>1e20f)return std::nullopt;
    for(auto i:indices)if(i>=positions.size())return std::nullopt;
    std::array<float,4> viewport{};
    for(unsigned i=0;i<4;++i) {
        const auto bits=*gta5DrawRegister(draw,0x210F+i);std::memcpy(&viewport[i],&bits,4);
    }
    const double correction=*gta5DrawRegister(draw,0x2302)==4?.5:0;
    NativeIndexedViewExclusion result;
    for(std::size_t at=0;at<indices.size();at+=3) {
        const std::array triangle{positions[indices[at]],positions[indices[at+1]],positions[indices[at+2]]};
        ++result.triangles;
        if(nativeAllVerticesBehindW(triangle)){++result.behind;continue;}
        if(!nativeCommonOutsideHomogeneousXYPlane(triangle))return std::nullopt;
        bool outside=false;
        for(unsigned plane=0;plane<4 && !outside;++plane) {
            const unsigned axis=plane/2;const bool high=plane%2;
            const double edge=double(rectangle[axis+(high?2:0)])+(high?2.0:-2.0);
            outside=true;
            for(const auto& p:triangle) {
                const double a=double(p[axis])*viewport[axis*2];
                const double b=(viewport[axis*2+1]+correction-edge)*p[3];
                if((high?1:-1)*(a+b)<=1e-6*std::max(1.0,std::abs(a)+std::abs(b))) {
                    outside=false;break;
                }
            }
        }
        if(!outside)return std::nullopt;
        ++result.outsideXY;
    }
    return result;
}
}
