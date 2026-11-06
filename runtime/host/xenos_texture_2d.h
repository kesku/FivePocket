#pragma once
#include "xenos_texture_description.h"
#include <vector>

namespace recomp::gpu {
// Resource-level import, independent of command-ring epochs or frame numbers.
// Unsigned R8/RGBA8, raw R16_FLOAT, BC1/BC3/DXN expanded to RGBA8, DXT5A to R8;
// base level, one 2D layer. DXN expands its second component into G/B/A.
// Unsupported formats/mips fail explicitly; they are never replaced by blanks.
struct XenosTexture2DLayout {
    XenosTextureDescription description;
    unsigned bytesPerTexel=0; // Linear output, including decompressed formats.
    std::uint32_t sourceBytes=0, linearBytes=0;
};
std::optional<XenosTexture2DLayout> planXenosTexture2D(
    const XenosTextureDescription& description) noexcept;

// Source is an independently owned copy beginning at description.baseAddress.
// Byte order and tiling are normalized; channel swizzle remains in description
// for the native image view. No guest pointer dereference or GPU work here.
std::optional<std::vector<std::uint8_t>> linearizeXenosTexture2D(
    const XenosTextureDescription& description, const std::uint8_t* source, std::size_t size);
inline std::optional<std::vector<std::uint8_t>> linearizeXenosTexture2D(
        const XenosTextureDescription& description, const std::vector<std::uint8_t>& source) {
    return linearizeXenosTexture2D(description,source.data(),source.size());
}
// Inverse storage permutation for uncompressed R8/RGBA8/R16_FLOAT. Linear component
// order is raw storage order (no sampler swizzle/conversion). Preserve every
// padding byte in the independently owned previous allocation. This is not a
// resolve request, allocation owner check, GPU completion or guest publication.
std::optional<std::vector<std::uint8_t>> storeXenosTexture2D(
    const XenosTextureDescription&,const std::vector<std::uint8_t>& linear,
    const std::vector<std::uint8_t>& previous);
// Raw-storage subrectangle [left,top,right,bottom], with tightly packed source
// rows. Preserves all other texels and padding, including endian-shared words.
// No shader swizzle/gamma conversion or guest publication is performed.
std::optional<std::vector<std::uint8_t>> storeXenosTexture2DRegion(
    const XenosTextureDescription&,const std::array<unsigned,4>& rectangle,
    const std::vector<std::uint8_t>& linear,const std::vector<std::uint8_t>& previous);
}
