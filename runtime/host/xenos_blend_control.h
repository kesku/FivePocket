#pragma once
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace recomp::gpu {
// RB_BLENDCONTROL field/factor definitions adapted from Xenia95a5c3ee:
// src/xenia/gpu/{registers.h,xenos.h,vulkan/vulkan_pipeline_cache.cc}.
// Copyright 2022 Ben Vanik; BSD-3-Clause, docs/licenses/XENIA.txt.
// Unlike Xenia's defensive unknown->ZERO table, unsupported encodings here
// refuse. This decodes pipeline state; it does not admit a shader/target/draw.
struct XenosBlendControl {
    unsigned colorSource,colorDestination,colorOperation;
    unsigned alphaSource,alphaDestination,alphaOperation;
    bool enabled;
    bool readsDestination(unsigned mask)const {
        const auto channel=[](unsigned source,unsigned destination,unsigned operation) {
            return operation==2 || operation==3 || destination!=0 ||
                source==8 || source==9 || source==10 || source==11 || source==16;
        };
        return ((mask&7) && channel(colorSource,colorDestination,colorOperation)) ||
            ((mask&8) && channel(alphaSource,alphaDestination,alphaOperation));
    }
};
inline bool knownXenosBlendFactor(unsigned factor) {
    return factor<=1 || (factor>=4 && factor<=16);
}
inline std::optional<XenosBlendControl> decodeXenosBlendControl(std::uint32_t word) {
    if(word&0xE000E000u)return std::nullopt;
    XenosBlendControl b{word&31,(word>>8)&31,(word>>5)&7,
        (word>>16)&31,(word>>24)&31,(word>>21)&7,true};
    if(!knownXenosBlendFactor(b.colorSource) || !knownXenosBlendFactor(b.colorDestination) ||
       !knownXenosBlendFactor(b.alphaSource) || !knownXenosBlendFactor(b.alphaDestination) ||
       b.colorOperation>4 || b.alphaOperation>4)return std::nullopt;
    b.enabled=!(b.colorSource==1 && b.colorDestination==0 && b.colorOperation==0 &&
        b.alphaSource==1 && b.alphaDestination==0 && b.alphaOperation==0);
    return b;
}
// Alpha COLOR factors select the alpha component, not an RGB vector. The
// COLOR normalization follows Xenia's D3D12 path. Additionally normalize
// saturate to ONE for alpha (its defined alpha component), instead of relying
// on API-specific COLOR-factor alpha behavior.
inline unsigned xenosAlphaBlendFactor(unsigned factor) {
    switch(factor) {
    case 4:return 6;case 5:return 7;case 8:return 10;case 9:return 11;
    case 12:return 14;case 13:return 15;case 16:return 1;default:return factor;
    }
}
inline std::optional<XenosBlendControl> planNativeRegisterBlend(std::uint32_t word) {
    auto b=decodeXenosBlendControl(word);if(!b)return std::nullopt;
    // No owned blend-constant path is supplied yet. Never silently use zero
    // constants or ignore a portability-subset constant-alpha limitation.
    for(auto f:{b->colorSource,b->colorDestination,b->alphaSource,b->alphaDestination})
        if(f>=12 && f<=15)return std::nullopt;
    b->alphaSource=xenosAlphaBlendFactor(b->alphaSource);
    b->alphaDestination=xenosAlphaBlendFactor(b->alphaDestination);
    return b;
}
}
