#pragma once
#include "native_vertex_clip.h"

namespace recomp::gpu {
struct NativeAxisQuadCoverage {
    unsigned candidateQuad=0,scissorExcluded=0,degenerate=0,candidateQuads=0,behind=0,constantAxis=0;
};
// Bounded single-coverage proof, NOT a general quad tessellator. Every quad is
// expanded as (0,1,2),(0,2,3). Potentially visible quads share one constant
// nonzero X (or Y) and have rectangular (Y,W) (or X,W) coordinates; Z is affine
// in W. On the W>0 part the constant coordinate divided by W determines W,
// then the other projected coordinate determines Y (or X). Both actual
// triangles partition that rectangle and keep their common edge, for either
// cyclic corner order and either diagonal. Bow-tie orders are not admitted.
// Intersecting with homogeneous clip planes cannot introduce overlap. W=0
// cannot survive XY clipping because the constant axis is nonzero. This works across the camera
// plane, unlike a positive-W-only projected-convexity check.
// Rectangles must be pairwise interior-disjoint in (otherAxis,W), so the same inverse
// map also proves no coverage overlap BETWEEN quads. Exact shared edges are
// permitted; overlapping rectangles, different constants or mixed constant axes
// are not. Other quads must be exactly degenerate, behind W or scissor-excluded.
// A frontoparallel plane (constantAxis=3) instead has the SAME positive W and Z
// for all candidates and rectangular (X,Y) coordinates. Division by this fixed
// W is affine and injective, so disjoint XY rectangles prove single coverage.
// All original vertices are checked; no quad is removed from the actual draw.
// Native fill/sample rules and translated shader arithmetic are not Xbox oracles.
inline std::optional<NativeAxisQuadCoverage> nativeSingleCoverageAxisQuadList(
        const Gta5InlineDraw& draw,std::span<const std::array<float,4>> positions) {
    if(!nativeQuadPositionState(draw,draw.stride) || positions.size()!=draw.vertexCount || positions.size()>512)return std::nullopt;
    const auto scissor=*nativeTranslatedScissor(draw);
    if(scissor[2]<=4 || scissor[3]<=4)return std::nullopt;
    for(const auto& p:positions)for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e20f)return std::nullopt;
    NativeAxisQuadCoverage result;std::vector<std::array<double,4>> rectangles;float commonCoordinate=0,commonDepth=0;
    for(unsigned first=0;first<positions.size();first+=4) {
        const auto original=positions.subspan(first,4);
        if(original[0]==original[1] && original[0]==original[2] && original[0]==original[3]){++result.degenerate;continue;}
        if(nativeAllVerticesBehindW(original)){++result.behind;continue;}
        if(nativeCommonOutsideScissorHalfPlane(draw,original)){++result.scissorExcluded;continue;}
        const auto constant=[&](unsigned axis){return std::abs(original[0][axis])>=1e-6f &&
            original[0][axis]==original[1][axis] && original[0][axis]==original[2][axis] && original[0][axis]==original[3][axis];};
        if(!result.candidateQuads) {
            result.candidateQuad=first/4;result.constantAxis=constant(3) && original[0][3]>0?3:(constant(0)?0:1);
            commonCoordinate=original[0][result.constantAxis];
            commonDepth=original[0][2];
        }
        const unsigned axis=result.constantAxis,u=axis==3?0:1-axis,v=axis==3?1:3;
        if(!constant(axis) || original[0][axis]!=commonCoordinate)return std::nullopt;
        if(axis==3)for(const auto& p:original)if(p[2]!=commonDepth)return std::nullopt;
        // Normalize only the proof's corner labels, never the actual vertices.
        const unsigned rotation=original[0][u]==original[1][u]?0:1;
        std::array<std::array<float,4>,4> q;
        for(unsigned i=0;i<4;++i)q[i]=original[(i+rotation)%4];
        if(q[0][u]!=q[1][u] || q[2][u]!=q[3][u] || q[0][2]!=q[3][2] || q[1][2]!=q[2][2] ||
           q[0][v]!=q[3][v] || q[1][v]!=q[2][v] || std::max(q[0][3],q[1][3])<1e-6f)return std::nullopt;
        for(unsigned varying:{u,v}) {
            const double a=q[0][varying],b=q[varying==u?2:1][varying];
            if(std::abs(a-b)<=1e-6*std::max({1.0,std::abs(a),std::abs(b)}))return std::nullopt;
        }
        const float lowV=std::min(q[0][v],q[1][v]);
        const std::array<double,4> rectangle{std::min(q[0][u],q[2][u]),std::max(q[0][u],q[2][u]),
            axis==3?lowV:std::max(0.f,lowV),std::max(q[0][v],q[1][v])};
        for(const auto& prior:rectangles)if(!(rectangle[1]<=prior[0] || prior[1]<=rectangle[0] ||
                rectangle[3]<=prior[2] || prior[3]<=rectangle[2]))return std::nullopt;
        rectangles.push_back(rectangle);++result.candidateQuads;
    }
    // Fully absent geometry is not admitted as a depth writer by this profile.
    return result.candidateQuads?std::optional{result}:std::nullopt;
}
}
