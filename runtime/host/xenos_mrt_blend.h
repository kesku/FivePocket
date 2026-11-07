#pragma once
#include <cstdint>
#include <optional>

namespace recomp::gpu {
// Evidenced RB_BLENDCONTROL encodings in the captured first mission batch.
// All ADD; factor numbers are original Xenos, not Vulkan enum values.
struct XenosMrtBlend {
    unsigned colorSource,colorDestination,alphaSource,alphaDestination;
    bool enabled;
    bool readsDestination(unsigned writeMask)const {
        return ((writeMask&7) && colorDestination!=0) || ((writeMask&8) && alphaDestination!=0);
    }
};
inline std::optional<XenosMrtBlend> decodeXenosMrtBlend(std::uint32_t word) {
    switch(word) {
    case 0x00010001:return XenosMrtBlend{1,0,1,0,false}; // replacement
    case 0x07010001:return XenosMrtBlend{1,0,1,7,true}; // replace RGB; accumulate alpha
    case 0x07000706:return XenosMrtBlend{6,7,0,7,true}; // source-alpha RGB; fade destination alpha
    default:return std::nullopt; // No mapping reserved/unknown factors to zero.
    }
}
}
