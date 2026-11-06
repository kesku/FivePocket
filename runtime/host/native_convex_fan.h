#pragma once
#include "native_vertex_clip.h"

namespace recomp::gpu {
// A strict closed convex fan with its center inside every boundary edge.
// Each sample is covered at most once under the native triangle fill rule;
// this permits linear blending followed by per-sample gamma quantization for
// one draw, without deferring quantization across overlapping fragments.
// This is NOT a sample-location, shader-math or Xbox rasterization oracle.
// Caller provides complete original GPU position outputs and validates shaders,
// state, ownership, depth/stencil read-only semantics and target lifetime.
inline bool nativeSingleCoverageConvexFan(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    if(!nativeClippedFanState(draw,draw.stride) || positions.size()!=draw.vertexCount ||
       positions.size()<5 || positions.size()>512)return false;
    const auto scissor=*nativeTranslatedScissor(draw);
    if(scissor[2]<=4 || scissor[3]<=4)return false;
    std::array<float,4> viewport{};
    for(unsigned i=0;i<4;++i){const auto bits=*gta5DrawRegister(draw,0x210F+i);std::memcpy(&viewport[i],&bits,4);}
    const double correction=*gta5DrawRegister(draw,0x2302)==4?.5:0;
    std::vector<std::array<double,2>> points;points.reserve(positions.size());
    for(const auto& p:positions) {
        for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e20f)return false;
        if(p[3]<1e-6f || p[2]<0 || p[2]>p[3] || std::abs(p[0])>=p[3] || std::abs(p[1])>=p[3])return false;
        std::array<double,2> point{};
        for(unsigned axis=0;axis<2;++axis) {
            point[axis]=double(p[axis])/p[3]*viewport[2*axis]+viewport[2*axis+1]+correction;
            if(point[axis]<=scissor[axis]+2 || point[axis]>=scissor[axis]+scissor[axis+2]-2)return false;
        }
        points.push_back(point);
    }
    if(points[1]!=points.back())return false; // Exact closure, not an invented final triangle.
    const auto cross=[](const auto& a,const auto& b,const auto& c) {
        return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
    };
    const auto first=cross(points[1],points[2],points[0]);
    constexpr double GuardArea=1e-4; // Reject near-collinear / numerically fragile edges.
    if(std::abs(first)<=GuardArea)return false;
    const double sign=first>0?1:-1;
    for(unsigned i=1;i+1<points.size();++i) {
        const auto& a=points[i];const auto& b=points[i+1];
        // Check ALL other vertices against each edge, not only consecutive
        // turn signs (which would incorrectly accept self-intersecting stars).
        for(unsigned j=0;j+1<points.size();++j) {
            if(j==i || j==i+1 || (i+2==points.size() && j==1))continue;
            if(sign*cross(a,b,points[j])<=GuardArea)return false;
        }
    }
    return true;
}
}
