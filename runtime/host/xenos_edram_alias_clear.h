#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Independent bounded 32bpp reference layout, based on pinned Xenia's xenos.h
// and shaders/edram.xesli (95a5c3ee...). No runtime caller, GPU command execution,
// clipping, predicates, compression, 64bpp, wrapping or retail-fidelity claim.
namespace recomp::gpu {
constexpr std::uint32_t AliasEdramBytes=2048u*80u*16u*4u;
struct AliasView {std::uint32_t surface=0,info=0,width=0,height=0;bool depth=false;};
struct AliasFootprint {std::uint32_t begin=0,bytes=0,pitchTiles=0,tileRows=0,samples=0;};
struct OpaqueAliasClear {std::uint32_t begin=0,bytes=0,word=0;};

inline bool fullAliasFootprint(const AliasView& v,AliasFootprint& out) {
    if(!v.width || !v.height || v.width>8192 || v.height>8192 ||
        (v.surface&0xC000u) || (v.surface&0x3FFFu)!=v.width || (v.info&~0x000F07FFu))return false;
    const unsigned mode=(v.surface>>16)&3,format=(v.info>>16)&15;
    if(mode>2 || (v.depth?format!=0:(format!=3 && format!=12)))return false;
    const unsigned sampleWidth=v.width*(mode==2?2:1),sampleHeight=v.height*(mode?2:1);
    if(sampleWidth%80 || sampleHeight%16)return false; // Whole tiles only; no edge overfill.
    const unsigned pitch=sampleWidth/80,rows=sampleHeight/16,base=v.info&2047;
    const std::uint64_t count=std::uint64_t{pitch}*rows;
    if(base+count>2048)return false; // Never implement implicit modulo/wrap.
    out={base*5120u,static_cast<std::uint32_t>(count*5120),pitch,rows,1u<<mode};return true;
}

inline bool aliasSampleOffset(const AliasView& view,std::uint32_t x,std::uint32_t y,
        std::uint32_t sample,std::uint32_t& out) {
    AliasFootprint f;if(!fullAliasFootprint(view,f) || x>=view.width || y>=view.height || sample>=f.samples)return false;
    const unsigned sx=x*(f.samples==4?2:1)+(sample>>1),sy=y*(f.samples>=2?2:1)+(sample&1);
    const unsigned tile=(view.info&2047)+(sy/16)*f.pitchTiles+sx/80;
    unsigned localX=sx%80;if(view.depth)localX=localX<40?localX+40:localX-40;
    out=tile*5120+((sy%16)*80+localX)*4;return true;
}

inline bool canonicalClearDepthBits(std::uint32_t depth24,std::uint32_t& out) {
    if(depth24>0xFFFFFF)return false;
    const float value=static_cast<float>(depth24+(depth24>=0x800000?1u:0u))*0x1p-24f;
    std::memcpy(&out,&value,4);return true;
}

// Exact inverse of canonical UNORM24->float clear values, NOT arbitrary raster
// depth quantization. In particular 0.5 is not in that canonical set. No tie
// rounding, denormal flush or clamp policy is guessed.
inline bool packCanonicalDepthStencil(std::uint32_t bits,std::uint32_t stencil,std::uint32_t& out) {
    if(bits>0x3F800000 || stencil>255)return false; // Includes -0/negative/NaN/Inf.
    float value=0;std::memcpy(&value,&bits,4);
    const double scaled=static_cast<double>(value)*16777216.0;
    if(scaled!=std::floor(scaled) || scaled==8388608.0)return false;
    const auto n=static_cast<std::uint32_t>(scaled)-(scaled>8388608.0?1u:0u);
    std::uint32_t back=0;if(!canonicalClearDepthBits(n,back) || back!=bits)return false;
    out=(n<<8)|stencil;return true;
}

inline bool planOpaqueAliasClear(const AliasView& depth,const AliasView& color,
        std::uint32_t depthBits,std::uint32_t stencil,OpaqueAliasClear& out) {
    AliasFootprint d,c;std::uint32_t word=0;
    if(!depth.depth || color.depth || !fullAliasFootprint(depth,d) || !fullAliasFootprint(color,c) ||
        d.begin!=c.begin || d.bytes!=c.bytes || !packCanonicalDepthStencil(depthBits,stencil,word) ||
        word!=0xC0000000)return false; // This clear only: RGB10 bits0, A2 bits3.
    out={d.begin,d.bytes,word};return true;
}

// Test/reference-owned little-endian dword backing only, not guest physical RAM.
// Revalidate before any write, including when called with a forged output plan.
inline bool fillOpaqueAliasClear(const AliasView& depth,const AliasView& color,
        std::uint32_t depthBits,std::uint32_t stencil,std::uint8_t* backing,std::size_t size) {
    OpaqueAliasClear plan;
    if(!backing || size!=AliasEdramBytes || !planOpaqueAliasClear(depth,color,depthBits,stencil,plan))return false;
    for(std::uint32_t at=plan.begin;at<plan.begin+plan.bytes;at+=4)
        for(unsigned i=0;i<4;++i)backing[at+i]=static_cast<std::uint8_t>(plan.word>>(i*8));
    return true;
}
}
