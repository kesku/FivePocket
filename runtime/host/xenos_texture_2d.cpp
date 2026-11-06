// The 2D address permutation below is adapted from Xenia/ReXGlue texture util.
// Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>
// Copyright (c) 2022, Ben Vanik and Xenia project contributors
// All rights reserved. BSD-3-Clause; full notice in docs/licenses/REXGLUE.txt.
#include "xenos_texture_2d.h"
#include <algorithm>

namespace recomp::gpu {
namespace {
// Positive coordinates only, bounded by the resource planner before use.
std::uint32_t tiledOffset(unsigned x, unsigned y, unsigned pitch, unsigned logBpp) noexcept {
    const auto macro=((x>>5)+(y>>5)*(pitch>>5))<<(logBpp+7);
    const auto micro=((x&7)+((y&14)<<2))<<logBpp;
    const auto offset=macro+((micro&~15u)<<1)+(micro&15)+((y&1)<<4);
    return ((offset&~511u)<<3)+((y&16)<<7)+((offset&448)<<2)+
        (((((y&8)>>2)+(x>>3))&3)<<6)+(offset&63);
}
}

std::optional<XenosTexture2DLayout> planXenosTexture2D(
        const XenosTextureDescription& d) noexcept {
    // Re-decode so a caller cannot forge derived fields that disagree with the
    // captured descriptor. The resulting layout always uses the encoded facts.
    const auto decoded=XenosTextureDescription::decode(d.fetch);
    if (!decoded) return std::nullopt;
    const auto& t=*decoded;
    if (t.dimension!=XenosTextureDescription::Dimension::TwoDOrStacked || t.stacked ||
        t.depthOrLayers!=1 || !t.width || !t.height || t.width>8192 || t.height>8192 ||
        !t.pitchTexels || t.pitchTexels<t.width || t.mipMin || t.mipMax ||
        t.mipAddress || t.packedMips || (t.integerComponents && t.format!=30) || t.exponentBias ||
        (t.format!=2 && t.format!=6 && t.format!=18 && t.format!=20 && t.format!=30 && t.format!=32 && t.format!=49 && t.format!=59) || !t.baseAddress) return std::nullopt;
    // ReX's Vulkan format table imports both signed/unsigned k_16_FLOAT as
    // raw R16_SFLOAT. Numeric fractional/integer selection does not turn its
    // floating representation into UNORM. Biased/gamma signs still need a
    // separate sampling conversion; do not relax those or any integer format.
    for (auto sign:t.sign) if (sign>(t.format==30?1u:0u)) return std::nullopt;
    for (auto swizzle:t.swizzle) if (swizzle>5) return std::nullopt;
    const bool compressed=t.format==18 || t.format==20 || t.format==49 || t.format==59;
    const bool wideBlock=t.format==20 || t.format==49;
    // k_16_16_16_16_FLOAT (32) is the only eight-byte-per-texel format here;
    // its 32x32 tile is addressed with logBpp 3, the same shift DXT1 blocks use.
    const unsigned bpp=t.format==2 || t.format==59?1:t.format==30?2:t.format==32?8:4;
    const unsigned logBpp=wideBlock?4:compressed?3:bpp==1?0:bpp==2?1:bpp==8?3:2;
    const unsigned width=compressed?(t.width+3)/4:t.width,height=compressed?(t.height+3)/4:t.height;
    // Tiled pitch is aligned in 32x32 blocks, not compressed-image texels.
    const unsigned pitch=compressed?(t.tiled?((t.pitchTexels/4+31)&~31u):t.pitchTexels/4):t.pitchTexels;
    const unsigned blockBytes=wideBlock?16:compressed?8:bpp;
    const auto linear=std::uint64_t{t.width}*t.height*bpp;
    std::uint64_t extent;
    if (t.tiled) {
        // Xenia/ReX's upper bound of the final32x32 tile. R8/R16 bank addressing
        // spans128x128/64x64 regions: tile origin+tile bytes is insufficient.
        extent=std::uint64_t{tiledOffset((width-1)&~31u,(height-1)&~31u,pitch,logBpp)}+
            (logBpp==0?0xA00u:logBpp==1?0xC00u:0x400u<<logBpp);
    } else {
        extent=std::uint64_t{height-1}*pitch*blockBytes+std::uint64_t{width}*blockBytes;
        extent=(extent+3)&~std::uint64_t{3}; // Endian permutation is within DWORDs.
    }
    constexpr std::uint64_t Limit=64u*1024*1024;
    if (extent>Limit || linear>Limit || std::uint64_t{t.baseAddress}+extent>0x100000000ULL)
        return std::nullopt;
    return XenosTexture2DLayout{t,bpp,static_cast<std::uint32_t>(extent),static_cast<std::uint32_t>(linear)};
}

std::optional<std::vector<std::uint8_t>> storeXenosTexture2D(
        const XenosTextureDescription& d,const std::vector<std::uint8_t>& linear,
        const std::vector<std::uint8_t>& previous) {
    const auto plan=planXenosTexture2D(d);
    if(!plan || (plan->description.format!=2 && plan->description.format!=6 && plan->description.format!=30 &&
        plan->description.format!=32) ||
       linear.size()!=plan->linearBytes || previous.size()!=plan->sourceBytes)return std::nullopt;
    const auto& t=plan->description;
    const auto bpp=plan->bytesPerTexel,logBpp=bpp==1?0u:bpp==2?1u:bpp==8?3u:2u;
    constexpr unsigned EndianXor[]{0,1,3,2};
    try {
        auto result=previous;
        for(unsigned y=0;y<t.height;++y)for(unsigned x=0;x<t.width;++x) {
            const auto to=t.tiled?tiledOffset(x,y,t.pitchTexels,logBpp):(y*t.pitchTexels+x)*bpp;
            const auto from=(y*t.width+x)*bpp;
            for(unsigned c=0;c<bpp;++c) {
                const auto offset=(to+c)^EndianXor[t.endian];
                if(offset>=result.size())return std::nullopt;
                result[offset]=linear[from+c];
            }
        }
        return result;
    } catch(const std::bad_alloc&) {return std::nullopt;}
}

std::optional<std::vector<std::uint8_t>> storeXenosTexture2DRegion(
        const XenosTextureDescription& d,const std::array<unsigned,4>& r,
        const std::vector<std::uint8_t>& linear,const std::vector<std::uint8_t>& previous) {
    const auto plan=planXenosTexture2D(d);
    if(!plan || (plan->description.format!=2 && plan->description.format!=6 && plan->description.format!=30) ||
       r[0]>=r[2] || r[1]>=r[3] || r[2]>plan->description.width || r[3]>plan->description.height ||
       linear.size()!=std::uint64_t{r[2]-r[0]}*(r[3]-r[1])*plan->bytesPerTexel ||
       previous.size()!=plan->sourceBytes)return std::nullopt;
    const auto& t=plan->description;const auto bpp=plan->bytesPerTexel,logBpp=bpp==1?0u:bpp==2?1u:bpp==8?3u:2u;
    constexpr unsigned EndianXor[]{0,1,3,2};
    try {
        auto result=previous;
        for(unsigned y=r[1];y<r[3];++y)for(unsigned x=r[0];x<r[2];++x) {
            const auto to=t.tiled?tiledOffset(x,y,t.pitchTexels,logBpp):(y*t.pitchTexels+x)*bpp;
            const auto from=((y-r[1])*(r[2]-r[0])+x-r[0])*bpp;
            for(unsigned c=0;c<bpp;++c) {
                const auto offset=(to+c)^EndianXor[t.endian];
                if(offset>=result.size())return std::nullopt;
                result[offset]=linear[from+c];
            }
        }
        return result;
    }catch(const std::bad_alloc&){return std::nullopt;}
}

std::optional<std::vector<std::uint8_t>> linearizeXenosTexture2D(
        const XenosTextureDescription& d, const std::uint8_t* source, std::size_t size) {
    const auto plan=planXenosTexture2D(d);
    if (!source || !plan || size<plan->sourceBytes) return std::nullopt;
    const auto& t=plan->description;
    const unsigned bpp=plan->bytesPerTexel, logBpp=bpp==1?0:bpp==2?1:bpp==8?3:2;
    constexpr unsigned EndianXor[]{0,1,3,2};
    try {
        std::vector<std::uint8_t> result(plan->linearBytes);
        if(t.format==18 || t.format==20 || t.format==49 || t.format==59) {
            const bool bc3=t.format==20;
            const bool dxn=t.format==49;
            const unsigned blockBytes=bc3 || dxn?16:8;
            const unsigned blocksX=(t.width+3)/4,blocksY=(t.height+3)/4;
            const unsigned pitch=t.tiled?((t.pitchTexels/4+31)&~31u):t.pitchTexels/4;
            // BC1: two RGB565 endpoints followed by sixteen little-bit-order
            // two-bit selectors. Apply the guest endian permutation first.
            for(unsigned by=0;by<blocksY;++by)for(unsigned bx=0;bx<blocksX;++bx) {
                const auto from=t.tiled?tiledOffset(bx,by,pitch,bc3 || dxn?4:3):(by*pitch+bx)*blockBytes;
                std::array<std::uint8_t,16> decodedBlock{};
                for(unsigned i=0;i<blockBytes;++i) {
                    const auto at=(from+i)^EndianXor[t.endian];
                    if(at>=size)return std::nullopt;
                    decodedBlock[i]=source[at];
                }
                if(dxn) {
                    // Xenos DXN (49) is BC5_UNORM: consecutive independent
                    // BC4 blocks for R and G. Xenia/ReXGlue's texture-cache
                    // format table uses RGGG before the submitted swizzle.
                    // Expand that into RGBA8 here, leaving the original fetch
                    // and NativeTextureImage's subsequent swizzle untouched.
                    // Same conventional 8-bit interpolation as DXT5A below;
                    // no signed DXN or Xbox filtering-conformance claim.
                    std::array<std::array<std::uint8_t,8>,2> palette{};
                    std::array<std::uint64_t,2> selectors{};
                    for(unsigned c=0;c<2;++c) {
                        const auto* block=decodedBlock.data()+c*8;
                        auto& p=palette[c];p[0]=block[0];p[1]=block[1];
                        const unsigned divisor=p[0]>p[1]?7:5;
                        for(unsigned i=1;i<divisor;++i)
                            p[i+1]=static_cast<std::uint8_t>(((divisor-i)*p[0]+i*p[1])/divisor);
                        if(divisor==5){p[6]=0;p[7]=255;}
                        for(unsigned i=0;i<6;++i)selectors[c]|=std::uint64_t{block[2+i]}<<(8*i);
                    }
                    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
                        const unsigned px=bx*4+x,py=by*4+y;
                        if(px>=t.width || py>=t.height)continue;
                        const auto shift=3*(y*4+x),at=(py*t.width+px)*4;
                        result[at]=palette[0][(selectors[0]>>shift)&7];
                        const auto green=palette[1][(selectors[1]>>shift)&7];
                        for(unsigned c=1;c<4;++c)result[at+c]=green;
                    }
                    continue;
                }
                if(t.format==59) {
                    // Xenos DXT5A: 4x4 texels in one 64-bit BC4_UNORM block.
                    // Decode to R8; NativeTextureImage replicates R to every
                    // source component before applying the ORIGINAL swizzle.
                    // Conventional 8-bit interpolation policy, not a claim
                    // of matching undocumented Xbox filtering precision.
                    std::array<std::uint8_t,8> alpha{decodedBlock[0],decodedBlock[1]};
                    const unsigned divisor=alpha[0]>alpha[1]?7:5;
                    for(unsigned i=1;i<divisor;++i)
                        alpha[i+1]=static_cast<std::uint8_t>(((divisor-i)*alpha[0]+i*alpha[1])/divisor);
                    if(divisor==5){alpha[6]=0;alpha[7]=255;}
                    std::uint64_t selectors=0;
                    for(unsigned i=0;i<6;++i)selectors|=std::uint64_t{decodedBlock[2+i]}<<(8*i);
                    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
                        const unsigned px=bx*4+x,py=by*4+y;
                        if(px<t.width && py<t.height)result[py*t.width+px]=alpha[(selectors>>(3*(y*4+x)))&7];
                    }
                    continue;
                }
                const auto* block=decodedBlock.data()+(bc3?8:0);
                const unsigned c0=block[0]|(unsigned(block[1])<<8),c1=block[2]|(unsigned(block[3])<<8);
                const bool fourColors=bc3 || c0>c1;
                std::array<std::array<std::uint8_t,4>,4> colors{};
                const auto unpack=[](unsigned c) {
                    const unsigned r=c>>11,g=(c>>5)&63,b=c&31;
                    return std::array<std::uint8_t,4>{static_cast<std::uint8_t>((r<<3)|(r>>2)),
                        static_cast<std::uint8_t>((g<<2)|(g>>4)),static_cast<std::uint8_t>((b<<3)|(b>>2)),255};
                };
                colors[0]=unpack(c0);colors[1]=unpack(c1);
                for(unsigned c=0;c<3;++c) {
                    colors[2][c]=static_cast<std::uint8_t>(fourColors?(2*colors[0][c]+colors[1][c])/3:(colors[0][c]+colors[1][c])/2);
                    colors[3][c]=static_cast<std::uint8_t>(fourColors?(colors[0][c]+2*colors[1][c])/3:0);
                }
                colors[2][3]=255;colors[3][3]=fourColors?255:0;
                std::array<std::uint8_t,8> alpha{};std::uint64_t alphaSelectors=0;
                if(bc3) {
                    alpha[0]=decodedBlock[0];alpha[1]=decodedBlock[1];
                    const unsigned divisor=alpha[0]>alpha[1]?7:5;
                    for(unsigned i=1;i<divisor;++i)
                        alpha[i+1]=static_cast<std::uint8_t>(((divisor-i)*alpha[0]+i*alpha[1])/divisor);
                    if(divisor==5){alpha[6]=0;alpha[7]=255;}
                    for(unsigned i=0;i<6;++i)alphaSelectors|=std::uint64_t{decodedBlock[2+i]}<<(8*i);
                }
                const auto selectors=std::uint32_t{block[4]}|(std::uint32_t{block[5]}<<8)|
                    (std::uint32_t{block[6]}<<16)|(std::uint32_t{block[7]}<<24);
                for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
                    const unsigned px=bx*4+x,py=by*4+y;
                    if(px>=t.width || py>=t.height)continue;
                    auto color=colors[(selectors>>(2*(y*4+x)))&3];
                    if(bc3)color[3]=alpha[(alphaSelectors>>(3*(y*4+x)))&7];
                    std::copy(color.begin(),color.end(),result.begin()+(py*t.width+px)*4);
                }
            }
            return result;
        }
        for (unsigned y=0;y<t.height;++y) for (unsigned x=0;x<t.width;++x) {
            const auto from=t.tiled?tiledOffset(x,y,t.pitchTexels,logBpp):(y*t.pitchTexels+x)*bpp;
            const auto to=(y*t.width+x)*bpp;
            for (unsigned c=0;c<bpp;++c) {
                const auto offset=(from+c)^EndianXor[t.endian];
                if (offset>=size) return std::nullopt;
                result[to+c]=source[offset];
            }
        }
        return result;
    } catch (const std::bad_alloc&) { return std::nullopt; }
}
}
