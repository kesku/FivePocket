#pragma once
#include "gta5_inline_draw.h"

namespace recomp::gpu {
// Owned snapshot: four color bindings and the depth binding, plus independent
// tile-cache identities. Planners must separately validate current registers,
// dimensions and execution state. Default32-byte extent preserves older users;
// 40 bytes also captures original dimensions.
// Caller holds the exclusive guest gate.
// Unknown ownership is recorded as unavailable, never replaced with zeros.
struct Gta5BoundSurfaceCapture {
    struct Surface {
        std::uint32_t object=0,tileCacheObject=0;
        // `header` is read at `object`. `tileCacheHeader` is the independent
        // read at `tileCacheObject`. It is NEVER a replacement for an unavailable
        // current header: notably, the current object can be stack-backed.
        // tileCacheHeader carries a default member initializer so existing
        // aggregate initialisers that list the first three fields keep
        // compiling under -Wmissing-field-initializers -Werror.
        std::optional<std::vector<std::uint8_t>> header;
        std::optional<std::vector<std::uint8_t>> tileCacheHeader{};
    };
    std::array<Surface,5> surfaces{};
};
inline std::optional<Gta5BoundSurfaceCapture> captureGta5BoundSurfaces(
        const std::vector<std::uint8_t>& state,const Gta5ResourceReader& heap,
        const Gta5ResourceReader& graphics={},unsigned headerBytes=32) {
    if(state.size()<13124 || !heap || (headerBytes!=32 && headerBytes!=40))return std::nullopt;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{state[at]}<<24)|(std::uint32_t{state[at+1]}<<16)|
            (std::uint32_t{state[at+2]}<<8)|state[at+3];
    };
    Gta5BoundSurfaceCapture result;
    for(unsigned i=0;i<result.surfaces.size();++i) {
        auto& s=result.surfaces[i];s.object=word(12816+4*i);s.tileCacheObject=word(13104+4*i);
        const auto read=[&](std::uint32_t object)->std::optional<std::vector<std::uint8_t>> {
            if(!object || object%4 || object>UINT32_MAX-(headerBytes-1))return std::nullopt;
            const auto& reader=object>=0x20000000?graphics:heap;
            if(!reader)return std::nullopt;
            auto bytes=reader(object,headerBytes);
            if(bytes && bytes->size()==headerBytes)return bytes;
            return std::nullopt;
        };
        s.header=read(s.object);
        s.tileCacheHeader=read(s.tileCacheObject);
    }
    return result;
}
// Caller holds the same exclusive gate and supplies the registry's current-
// worker argument reader. This extends HEADER capture only, not arbitrary
// resource/shader reads. Neither a saved diagnostic nor raw RAM is a reader.
inline std::optional<Gta5BoundSurfaceCapture> captureGta5ArgumentBoundSurfaces(
        const std::vector<std::uint8_t>& state,const Gta5ResourceReader& heap,
        const Gta5ResourceReader& graphics,const Gta5ResourceReader& arguments) {
    return captureGta5BoundSurfaces(state,heap,[&](auto address,auto bytes) {
        auto owned=arguments?arguments(address,bytes):std::nullopt;
        return owned?owned:(graphics?graphics(address,bytes):std::nullopt);
    },40);
}
}
