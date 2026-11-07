#pragma once
#include "native_edram32_bands.h"

namespace recomp::gpu {
// Diagnostic snapshots of already-known physical content at the refusal point.
// Not an execution receipt or proof of when a failed attempt modified anything;
// never committed back to the live owner. The caller trace must establish the
// producer/consumer boundary. Fixed extent is the original bound1280x720 pair.
struct NativeStoppedPhysical32Capture {
    std::uint64_t serial=0;
    std::vector<std::uint32_t> color,depth;
};
inline std::optional<NativeStoppedPhysical32Capture> captureNativeStoppedPhysical32(
        const NativeEdram32Bands& owner,const std::vector<std::uint8_t>& state) {
    if(state.size()!=13860)return std::nullopt;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{state[at]}<<24)|(std::uint32_t{state[at+1]}<<16)|
            (std::uint32_t{state[at+2]}<<8)|state[at+3];
    };
    // Current source view: RGB7e3 at tile720, D24FS8 at0, pitch1280, single sample.
    if(word(10368)!=0x14000500 || word(10372)!=0x302D0 || word(10376)!=0x10000)
        return std::nullopt;
    const auto serial=owner.nextSerial();if(serial<=1)return std::nullopt;
    auto color=owner.readColorWords(720,1280,1280,720);
    auto depth=owner.readDepthWords(0,1280,1280,720);
    if(!color || !depth)return std::nullopt;
    return NativeStoppedPhysical32Capture{serial-1,std::move(*color),std::move(*depth)};
}
}
