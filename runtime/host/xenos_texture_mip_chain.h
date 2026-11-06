#pragma once
#include "xenos_texture_2d.h"
#include <algorithm>
#include <bit>

namespace recomp::gpu {
// Independent subresource plan, not a replacement for the original fetch.
// Reference: Xenia95a5c3ee texture_util.cc GetGuestTextureLayout: mip row/slice
// strides use power-of-two base dimensions, 32x32-block tiles, and 4KiB-aligned
// subresources. Base pitch remains the submitted pitch. Packed TAILS, arrays,
// relative addressing, signed/exponent conversion and sampling are unsupported.
struct XenosTextureMipChain {
    struct Level {
        unsigned level=0;
        std::uint32_t planeOffset=0,storageStride=0;
        XenosTexture2DLayout layout;
    };
    std::array<std::uint32_t,6> submittedFetch{};
    std::vector<Level> levels;
    std::uint32_t baseBytes=0,mipBytes=0;
};

inline std::optional<XenosTextureMipChain> planXenosUnpackedMipChain(
        const std::array<std::uint32_t,6>& fetch) {
    const auto decoded=XenosTextureDescription::decode(fetch);
    if(!decoded)return std::nullopt;
    const auto& t=*decoded;
    if(t.dimension!=XenosTextureDescription::Dimension::TwoDOrStacked || t.stacked ||
        t.width>8192 || t.height>8192 || t.mipMin || !t.baseAddress || !t.pitchTexels ||
        (t.mipMax && !t.mipAddress) || (!t.mipMax && t.mipAddress) ||
        t.mipMax>=static_cast<unsigned>(std::bit_width(std::max(t.width,t.height))))return std::nullopt;
    if(t.packedMips) {
        const auto shortLog=static_cast<unsigned>(std::bit_width(std::min(t.width,t.height)-1));
        const auto packedLevel=shortLog>4?shortLog-4:0;
        if(t.mipMax>=packedLevel)return std::nullopt; // Never pretend packed tail is a full plane.
    }
    const unsigned block=(t.format==18 || t.format==20 || t.format==49 || t.format==59)?4:1;
    const unsigned blockBytes=t.format==20 || t.format==49?16:t.format==18 || t.format==59?8:t.format==2?1:t.format==30?2:4;
    const auto align=[](std::uint64_t value,unsigned alignment){return (value+alignment-1)&~std::uint64_t{alignment-1};};
    const auto powerWidth=std::bit_ceil(t.width),powerHeight=std::bit_ceil(t.height);
    XenosTextureMipChain chain;chain.submittedFetch=fetch;
    std::uint64_t offset=0;
    for(unsigned level=0;level<=t.mipMax;++level) {
        const auto width=std::max(t.width>>level,1u),height=std::max(t.height>>level,1u);
        const auto rowTexels=level?std::max(powerWidth>>level,1u):t.pitchTexels;
        auto rowBytes=align((rowTexels+block-1)/block,32)*blockBytes;
        if(level && !t.tiled)rowBytes=align(rowBytes,256);
        const auto heightForStride=level?std::max(powerHeight>>level,1u):t.height;
        const auto rows=align((heightForStride+block-1)/block,32);
        const auto stride=align(rowBytes*rows,4096);
        const auto address=level?std::uint64_t{t.mipAddress}+offset:t.baseAddress;
        const auto pitch=level?rowBytes/blockBytes*block:t.pitchTexels;
        if(stride>64u*1024*1024 || address>=0x100000000ULL || pitch>511*32 || pitch%32)
            return std::nullopt;
        auto view=fetch;
        // These are DERIVED single-level view descriptors, not fabricated
        // submitted bindings. The original fetch is retained separately above.
        view[0]=(view[0]&~(511u<<22))|(static_cast<unsigned>(pitch/32)<<22);
        view[1]=(view[1]&0xFFF)|static_cast<std::uint32_t>(address);
        view[2]=(view[2]&0xFC000000)|(width-1)|((height-1)<<13);
        view[4]&=~0x3FCu;view[5]&=0x7FFu;
        const auto description=XenosTextureDescription::decode(view);
        const auto layout=description?planXenosTexture2D(*description):std::nullopt;
        if(!layout)return std::nullopt;
        if(level) {
            const auto end=offset+layout->sourceBytes;
            if(end>64u*1024*1024 || std::uint64_t{t.mipAddress}+end>0x100000000ULL)
                return std::nullopt;
            chain.mipBytes=std::max(chain.mipBytes,static_cast<std::uint32_t>(end));
        } else chain.baseBytes=layout->sourceBytes;
        chain.levels.push_back({level,static_cast<std::uint32_t>(level?offset:0),
                               static_cast<std::uint32_t>(stride),*layout});
        if(level)offset+=stride;
    }
    return chain;
}

// Both planes are independently owned snapshots. Validate every required
// extent before publishing any decoded level; never zero-fill missing mips.
inline std::optional<std::vector<std::vector<std::uint8_t>>> linearizeXenosUnpackedMipChain(
        const std::array<std::uint32_t,6>& fetch,
        const std::vector<std::uint8_t>& base,const std::vector<std::uint8_t>& mips) {
    const auto plan=planXenosUnpackedMipChain(fetch);
    if(!plan || base.size()<plan->baseBytes || mips.size()<plan->mipBytes)return std::nullopt;
    std::uint64_t total=0;
    for(const auto& level:plan->levels)total+=level.layout.linearBytes;
    if(total>64u*1024*1024)return std::nullopt;
    try {
        std::vector<std::vector<std::uint8_t>> result;
        result.reserve(plan->levels.size());
        for(const auto& level:plan->levels) {
            const auto& plane=level.level?mips:base;
            auto decoded=linearizeXenosTexture2D(level.layout.description,
                plane.data()+level.planeOffset,level.layout.sourceBytes);
            if(!decoded)return std::nullopt;
            result.push_back(std::move(*decoded));
        }
        return result;
    } catch(const std::bad_alloc&) {return std::nullopt;}
}
}
