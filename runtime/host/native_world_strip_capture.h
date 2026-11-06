#pragma once
#include "native_world_strip_plan.h"

namespace recomp::gpu {
// Current submitted descriptors and owned bytes under the caller's exclusive
// execution gate. No stale texture cache, saved fixture or address-mask alias.
// Slot1 has two independent planes; slot6 is a raw addressing view only.
inline std::optional<NativeWorldStripTextures> captureNativeWorldStripTextures(
        const Gta5TextureFetchReader& submitted,const Gta5ResourceReader& graphics) {
    if(!submitted || !graphics)return std::nullopt;
    constexpr unsigned slots[]{2,6,1,3};NativeWorldStripTextures result;
    for(unsigned i=0;i<4;++i) {
        const auto fetch=submitted(slots[i]);if(!fetch)return std::nullopt;
        if(slots[i]==6) {
            const auto desc=XenosTextureDescription::decode(*fetch);
            const auto storage=desc?planXenosFloatStorage(*desc):std::nullopt;
            if(!storage || storage->sourceBytes!=16384)return std::nullopt;
            const auto bytes=graphics(desc->baseAddress,storage->sourceBytes);
            if(!bytes || bytes->size()!=storage->sourceBytes)return std::nullopt;
            result[i]={*fetch,*bytes,{}};
        } else {
            auto texture=captureNativeMipTextureStorage(*fetch,graphics,1024*1024);
            if(!texture)return std::nullopt;result[i]=std::move(*texture);
        }
        if(!planNativeWorldStripTexture(slots[i],result[i]))return std::nullopt;
    }
    return result;
}
}
