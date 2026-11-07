#pragma once
#include "native_translated_draw.h"
#include <span>
namespace recomp::gpu {
struct NativeRectangleListCoverage {
    unsigned left=0,top=0,right=0,bottom=0;
    std::uint64_t area=0;
};
// Position-only geometry proof, retaining bounds AND nonoverlapping area.
// Integer window edges and common positive W per primitive. Partial coverage
// never implies that untouched target pixels are initialized or dispensable.
inline std::optional<NativeRectangleListCoverage> nativeRectangleListCoverage(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    if(draw.primitive!=8 || draw.vertexCount<3 || draw.vertexCount>384 || draw.vertexCount%3 ||
       positions.size()!=draw.vertexCount)return std::nullopt;
    const auto scissor=nativeTranslatedScissor(draw);
    if(!scissor || (*scissor)[0] || (*scissor)[1] || !(*scissor)[2] || !(*scissor)[3] ||
       !nativeTranslatedViewport(draw))return std::nullopt;
    std::array<float,4> viewport{};
    for(unsigned i=0;i<4;++i){const auto bits=gta5DrawRegister(draw,0x210F+i);if(!bits)return std::nullopt;std::memcpy(&viewport[i],&*bits,4);}
    struct Rect{unsigned left,top,right,bottom;};std::vector<Rect> rectangles;std::uint64_t area=0;
    for(unsigned first=0;first<positions.size();first+=3) {
        std::array<std::array<double,2>,3> xy{};
        const auto w=positions[first][3],z=positions[first][2];
        if(!std::isfinite(w) || w<=0 || z<0 || z>w)return std::nullopt;
        for(unsigned i=0;i<3;++i) {
            const auto& p=positions[first+i];
            for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e10f)return std::nullopt;
            if(p[2]!=z || p[3]!=w)return std::nullopt;
            xy[i]={double(p[0])/w*viewport[0]+viewport[1],double(p[1])/w*viewport[2]+viewport[3]};
            for(unsigned c=0;c<2;++c)if(xy[i][c]<0 || xy[i][c]>(*scissor)[c+2] || std::floor(xy[i][c])!=xy[i][c])return std::nullopt;
        }
        unsigned corner=3;
        for(unsigned a=0;a<3;++a) {
            const auto b=(a+1)%3,c=(a+2)%3;
            if((xy[a][0]==xy[b][0] && xy[a][1]!=xy[b][1] && xy[a][1]==xy[c][1] && xy[a][0]!=xy[c][0]) ||
               (xy[a][1]==xy[b][1] && xy[a][0]!=xy[b][0] && xy[a][0]==xy[c][0] && xy[a][1]!=xy[c][1]))corner=a;
        }
        if(corner==3)return std::nullopt;
        const auto b=(corner+1)%3,c=(corner+2)%3;
        Rect r{unsigned(std::min(xy[b][0],xy[c][0])),unsigned(std::min(xy[b][1],xy[c][1])),
               unsigned(std::max(xy[b][0],xy[c][0])),unsigned(std::max(xy[b][1],xy[c][1]))};
        if(r.left==r.right || r.top==r.bottom)return std::nullopt;
        for(const auto& previous:rectangles)if(r.left<previous.right && previous.left<r.right &&
            r.top<previous.bottom && previous.top<r.bottom)return std::nullopt;
        area+=std::uint64_t{r.right-r.left}*(r.bottom-r.top);rectangles.push_back(r);
    }
    NativeRectangleListCoverage coverage{(*scissor)[2],(*scissor)[3],0,0,area};
    for(const auto& r:rectangles) {
        coverage.left=std::min(coverage.left,r.left);coverage.top=std::min(coverage.top,r.top);
        coverage.right=std::max(coverage.right,r.right);coverage.bottom=std::max(coverage.bottom,r.bottom);
    }
    return coverage;
}
// Separate explicit partial-region proof; original full-replacement callers
// below retain their exact whole-scissor requirement.
inline bool nativeTopLeftRectangleList(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions,unsigned width,unsigned height) {
    if(!width || !height)return false;
    const auto coverage=nativeRectangleListCoverage(draw,positions);
    return coverage && !coverage->left && !coverage->top && coverage->right==width && coverage->bottom==height &&
        coverage->area==std::uint64_t{width}*height;
}
inline bool nativeFullRectangleList(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    const auto scissor=nativeTranslatedScissor(draw);
    return scissor && nativeTopLeftRectangleList(draw,positions,(*scissor)[2],(*scissor)[3]);
}
}
