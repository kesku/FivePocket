#pragma once
#include "xenos_float32_texture.h"

namespace recomp::gpu {
// Explicit format29 EXPAND -> binary16 storage view. The pinned Xenia Vulkan
// texture table maps both signed and unsigned EXPAND to RGBA16_SFLOAT. In the
// captured GTA copy, original82388A38->823C2738 emits COPY_DEST_INFO0000F001:
// storage format32, FLOAT, no bias/swap, 8-in-16 endian. Sign bits in a fetch
// descriptor do NOT request a signed-integer resolve encoder.
// This helper grants byte addressing only, not sampling/shader admission. Keep
// the original descriptor for those decisions. General texture admission is
// unchanged; gamma/biased signs, mip chains and exponent bias still refuse.
inline std::optional<XenosTexture2DLayout> planXenosExpanded16FloatStorage(
        const XenosTextureDescription& description) {
    const auto original=XenosTextureDescription::decode(description.fetch);
    if(!original || original->format!=29)return std::nullopt;
    auto words=original->fetch;words[1]=(words[1]&~63u)|32u;
    const auto view=XenosTextureDescription::decode(words);
    return view?planXenosFloatStorage(*view):std::nullopt;
}
inline std::optional<std::vector<std::uint8_t>> linearizeXenosExpanded16FloatStorage(
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& bytes) {
    const auto view=planXenosExpanded16FloatStorage(description);
    return view?linearizeXenosTexture2D(view->description,bytes):std::nullopt;
}
}
