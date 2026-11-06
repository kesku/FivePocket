#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace recomp::gpu {

// Describes resource-level texture inputs without imposing a frame-specific
// command profile. Layout reference: Xenia's xe_gpu_texture_fetch_t, also in
// rexglue-sdk c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 graphics/xenos.h.
// This is metadata, NOT validation that a texture is resident or GPU-rendered.
// Addresses retain their original domain: GTA resource headers contain CPU
// virtual addresses, while submitted fetch constants may contain physical ones.
struct XenosTextureDescription {
    enum class Dimension : unsigned { OneD, TwoDOrStacked, ThreeD, Cube };
    std::array<std::uint32_t,6> fetch{};
    Dimension dimension{};
    std::uint32_t width=0, height=0, depthOrLayers=0;
    std::uint32_t pitchTexels=0, baseAddress=0, mipAddress=0;
    unsigned format=0, endian=0, mipMin=0, mipMax=0;
    std::array<unsigned,4> sign{}, swizzle{};
    std::array<unsigned,3> clamp{};
    int exponentBias=0;
    bool tiled=false, stacked=false, packedMips=false, integerComponents=false;

    static std::optional<XenosTextureDescription> decode(
            const std::array<std::uint32_t,6>& words) noexcept {
        if ((words[0]&3u)!=2u) return std::nullopt; // Not a texture fetch constant.
        XenosTextureDescription d;
        d.fetch=words;
        d.dimension=static_cast<Dimension>((words[5]>>9)&3u);
        d.pitchTexels=((words[0]>>22)&511u)*32u;
        d.tiled=(words[0]>>31)!=0;
        d.format=words[1]&63u;
        d.endian=(words[1]>>6)&3u;
        d.stacked=(words[1]&1024u)!=0;
        d.baseAddress=words[1]&0xfffff000u;
        d.mipAddress=words[5]&0xfffff000u;
        d.packedMips=(words[5]&2048u)!=0;
        d.integerComponents=(words[3]&1u)!=0;
        d.mipMin=(words[4]>>2)&15u;
        d.mipMax=(words[4]>>6)&15u;
        const auto bias=(words[3]>>13)&63u;
        d.exponentBias=bias<32u?static_cast<int>(bias):static_cast<int>(bias)-64;
        for (unsigned i=0;i<4;++i) {
            d.sign[i]=(words[0]>>(2+2*i))&3u;
            d.swizzle[i]=(words[3]>>(1+3*i))&7u;
        }
        for (unsigned i=0;i<3;++i) d.clamp[i]=(words[0]>>(10+3*i))&7u;
        switch (d.dimension) {
        case Dimension::OneD:
            d.width=(words[2]&0xffffffu)+1; d.height=1; d.depthOrLayers=1; break;
        case Dimension::ThreeD:
            d.width=(words[2]&2047u)+1; d.height=((words[2]>>11)&2047u)+1;
            d.depthOrLayers=(words[2]>>22)+1; break;
        case Dimension::TwoDOrStacked:
        case Dimension::Cube:
            d.width=(words[2]&8191u)+1; d.height=((words[2]>>13)&8191u)+1;
            d.depthOrLayers=d.dimension==Dimension::Cube?6u:(d.stacked?(words[2]>>26)+1:1u);
            break;
        }
        return d;
    }
};

// GTA V's original texture object stores its six fetch words at byte 28.
// Derived from the original resolve implementation at 823C2738. A bounded,
// owned byte snapshot is required; this helper never dereferences guest RAM.
// Do not cast that object to SanRecomp's host-backed GuestTexture type.
inline std::optional<XenosTextureDescription> describeGta5TextureHeader(
        const std::uint8_t* header, std::size_t size) noexcept {
    if (!header || size<52) return std::nullopt;
    std::array<std::uint32_t,6> words{};
    for (unsigned i=0;i<6;++i) {
        const auto* p=header+28+4*i;
        words[i]=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
            (std::uint32_t{p[2]}<<8)|p[3];
    }
    return XenosTextureDescription::decode(words);
}
template<class Bytes> inline std::optional<XenosTextureDescription> describeGta5TextureHeader(
        const Bytes& header) noexcept {
    return describeGta5TextureHeader(header.data(),header.size());
}

}
