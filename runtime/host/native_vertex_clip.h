#pragma once
#include "native_translated_draw.h"
#include <span>

namespace recomp::gpu {
// Common homogeneous XY outside-plane test. This is deliberately stricter than
// general primitive clipping: all vertices must be outside ONE plane, with
// positive W and a 1/256 normalized guard band. Boundary / nonfinite / near-zero
// W inputs reject. No Z culling (viewport/depth clipping have other semantics).
// The margin is an engineering precaution, not an Xbox transcendental oracle.
// Caller must supply every ORIGINAL, pre-viewport position from the validated
// side-effect-free shader and separately validate original clipping controls.
inline std::optional<unsigned> nativeCommonOutsideXYPlane(std::span<const std::array<float,4>> positions) {
    if(positions.size()<3 || positions.size()>65536)return std::nullopt;
    unsigned common=15;
    for(const auto& p:positions) {
        for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e20f)return std::nullopt;
        if(p[3]<1e-6f)return std::nullopt;
        const double boundary=double(p[3])*(1.0+1.0/256.0);
        unsigned outside=0;
        if(double(p[0])<-boundary)outside|=1;
        if(double(p[0])>boundary)outside|=2;
        if(double(p[1])<-boundary)outside|=4;
        if(double(p[1])>boundary)outside|=8;
        common&=outside;
    }
    if(!common)return std::nullopt;
    for(unsigned i=0;i<4;++i)if(common&(1u<<i))return i;
    return std::nullopt;
}
// Unlike a perspective divide, sign*coordinate-W remains a linear plane
// expression when W crosses zero. If it is strictly positive at EVERY vertex,
// every convex combination is outside that clip plane as well. No conclusion
// is drawn from divided negative-W coordinates. Retain a conservative relative
// separation and reject nonfinite/oversized inputs, including unused lanes.
inline std::optional<unsigned> nativeCommonOutsideHomogeneousXYPlane(std::span<const std::array<float,4>> positions) {
    if(positions.size()<3 || positions.size()>65536)return std::nullopt;
    unsigned common=15;
    for(const auto& p:positions) {
        for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e20f)return std::nullopt;
        unsigned outside=0;
        for(unsigned plane=0;plane<4;++plane) {
            const double coordinate=p[plane/2],w=p[3];
            const double margin=std::max(1e-6,std::max(std::abs(coordinate),std::abs(w))/256.0);
            if((plane%2?coordinate:-coordinate)-w>margin)outside|=1u<<plane;
        }
        common&=outside;
    }
    for(unsigned plane=0;plane<4;++plane)if(common&(1u<<plane))return plane;
    return std::nullopt;
}
// Every triangle is a convex combination of its original homogeneous
// vertices. If all W are strictly negative, W cannot reach the positive half
// space required by -W<=X<=W (even before the Z planes). Do NOT divide by W
// and interpret a behind-camera NDC coordinate as an on-screen point.
// The relative/absolute separation rejects near-zero or numerically fragile
// inputs; it is an engineering margin, not an Xbox floating-point oracle.
inline bool nativeAllVerticesBehindW(std::span<const std::array<float,4>> positions) {
    if(positions.size()<3 || positions.size()>65536)return false;
    for(const auto& p:positions) {
        double magnitude=0;
        for(float f:p) {
            if(!std::isfinite(f) || std::abs(f)>1e20f)return false;
            magnitude=std::max(magnitude,std::abs(double(f)));
        }
        if(double(p[3])>=-std::max(1e-6,magnitude/256.0))return false;
    }
    return true;
}
// No target is read or written by a proven fully clipped primitive. Only the
// no-query, no-user-plane, standard homogeneous filled-fan subset is admitted.
// Shader identity, owned data, complete compute execution and common-plane
// proof remain independent requirements. Pixel shader memory exports MUST have
// been rejected by the compiler; otherwise clipping doesn't license elision.
inline bool nativeClippedFanState(const Gta5InlineDraw& draw,unsigned stride) {
    if(draw.primitive!=5 || draw.vertexCount<3 || draw.vertexCount>65536 ||
       !stride || stride%4 || stride>256 || draw.stride!=stride ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*stride ||
       draw.vertices.size()>256*1024 || !nativeTranslatedViewport(draw) || !nativeTranslatedScissor(draw))return false;
    constexpr unsigned state[][2]{{0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2208,4},{0x2284,0},{0x2292,0},{0x2293,0},{0x2183,0},{0x2184,0}};
    for(const auto& r:state)if(gta5DrawRegister(draw,r[0])!=std::optional<std::uint32_t>{r[1]})return false;
    return true;
}
// Separate position-only profile for filled, nonindexed quad lists. This does
// not grant rendering or elision: complete positions, resource/shader identity,
// and a separate coverage/state proof are required by the eventual consumer.
inline bool nativeQuadPositionState(const Gta5InlineDraw& draw,unsigned stride) {
    if(draw.primitive!=13 || draw.vertexCount<4 || draw.vertexCount>65536 || draw.vertexCount%4 ||
       !stride || stride%4 || stride>256 || draw.stride!=stride ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*stride ||
       draw.vertices.size()>256*1024 || !nativeTranslatedViewport(draw) || !nativeTranslatedScissor(draw))return false;
    constexpr unsigned state[][2]{{0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},
        {0x2208,4},{0x2284,0},{0x2292,0},{0x2293,0},{0x2183,0},{0x2184,0}};
    for(const auto& r:state)if(gta5DrawRegister(draw,r[0])!=std::optional<std::uint32_t>{r[1]})return false;
    return true;
}
// A linear homogeneous screen-half-plane expression is valid across W=0:
// clipping retains only convex combinations with positive W, and division by
// that W preserves the sign. The extra2px margin is relative to the scissor,
// not a claim that a negative-W projected point is visible. No Z inference.
inline std::optional<unsigned> nativeCommonOutsideScissorHalfPlane(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    if(positions.size()<3 || positions.size()>65536 || !nativeTranslatedViewport(draw))return std::nullopt;
    const auto scissor=nativeTranslatedScissor(draw);if(!scissor)return std::nullopt;
    std::array<float,4> viewport{};
    for(unsigned i=0;i<4;++i){const auto bits=*gta5DrawRegister(draw,0x210F+i);std::memcpy(&viewport[i],&bits,4);}
    const double correction=*gta5DrawRegister(draw,0x2302)==4?.5:0;
    for(const auto& p:positions)for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e20f)return std::nullopt;
    for(unsigned plane=0;plane<4;++plane) {
        const auto axis=plane/2;const bool high=plane%2;
        const double edge=double((*scissor)[axis])+(high?(*scissor)[axis+2]+2.0:-2.0);
        bool common=true;
        for(const auto& p:positions) {
            const double a=double(p[axis])*viewport[axis*2];
            const double b=(viewport[axis*2+1]+correction-edge)*p[3];
            if((high?1:-1)*(a+b)<=1e-6*std::max(1.0,std::abs(a)+std::abs(b))){common=false;break;}
        }
        if(common)return plane;
    }
    return std::nullopt;
}
inline std::optional<unsigned> nativeClippedFanOutside(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    if(positions.size()!=draw.vertexCount || !nativeClippedFanState(draw,draw.stride))return std::nullopt;
    // Classification4 is negative-W rejection, not an XY plane. Otherwise
    // require BOTH homogeneous clip-volume and >=2px scissor separation.
    // Both proofs cover every original vertex and remain valid across W=0.
    // No target ownership, query, clipping-control or shader guard is relaxed.
    if(nativeAllVerticesBehindW(positions))return 4;
    const auto plane=nativeCommonOutsideHomogeneousXYPlane(positions);
    if(!plane || !nativeCommonOutsideScissorHalfPlane(draw,positions))return std::nullopt;
    return plane;
}
struct NativeClippedQuadList {
    unsigned behind=0,scissorExcluded=0,degenerate=0;
};
// Both triangles formed from each four-vertex quad must be absent. Checking
// ALL four vertices against one half-plane proves either diagonal, and avoids
// depending on a particular tessellator. Filled point-degenerates have zero
// area; line/point modes, queries and shader side effects remain unadmitted.
inline std::optional<NativeClippedQuadList> nativeClippedQuadListOutside(const Gta5InlineDraw& draw,
        std::span<const std::array<float,4>> positions) {
    if(!nativeQuadPositionState(draw,draw.stride) || positions.size()!=draw.vertexCount)return std::nullopt;
    for(const auto& p:positions)for(float f:p)if(!std::isfinite(f) || std::abs(f)>1e20f)return std::nullopt;
    NativeClippedQuadList result;
    for(unsigned first=0;first<positions.size();first+=4) {
        const auto q=positions.subspan(first,4);
        if(nativeAllVerticesBehindW(q)){++result.behind;continue;}
        if(nativeCommonOutsideScissorHalfPlane(draw,q)){++result.scissorExcluded;continue;}
        if(q[0]==q[1] && q[0]==q[2] && q[0]==q[3]){++result.degenerate;continue;}
        return std::nullopt;
    }
    return result;
}
}
