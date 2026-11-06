#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_offscreen_resolve_request.h"

namespace recomp::gpu {
// Read-only terminal capture at the ORIGINAL 82388A38 ABI, before the adapter
// rearranges registers for823C2738. Missing ownership stays unavailable. This
// does not plan/execute/complete a resolve or advance an original recording.
struct Gta5TileResolveCapture {
    static constexpr unsigned DevicePrefixBytes=13860,AllocatorTailEnd=15344;
    static constexpr unsigned AllocatorHeaderBytes=176;
    static constexpr std::uint32_t ResolveGlobalsAddress=0x83CD2AA4;
    std::uint32_t stack=0;
    std::uint64_t f1=0;
    std::array<std::uint32_t,9> arguments{}; // r3..r10, LR.
    NativeOffscreenResolveArguments pointed;
    std::optional<std::vector<std::uint8_t>> state,destinationHeader;
    // Original823C2738's allocator helper823BBD00 reads device+13920 and
    // counters15308..15340 beyond the normal draw-state snapshot. Keep this
    // bounded optional tail separate: unavailable bytes are NEVER zero-filled.
    std::optional<std::vector<std::uint8_t>> allocatorTail,allocatorHeader;
    // Original823BC018 reads device[11024]+4. Full823C2738 also reads three
    // mutable title globals, not immutable constants from the decoded image.
    std::optional<std::vector<std::uint8_t>> allocatorFence,resolveGlobals;
    std::optional<Gta5BoundSurfaceCapture> surfaces;
};
inline Gta5TileResolveCapture captureGta5TileResolveBoundary(std::uint32_t stack,
        const std::array<std::uint32_t,9>& arguments,std::uint64_t f1,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics,const Gta5ResourceReader& argumentReader,
        const Gta5ResourceReader& titleImage={}) {
    Gta5TileResolveCapture c;c.stack=stack;c.arguments=arguments;c.f1=f1;
    const NativeOffscreenResolveRequest r{stack,arguments[1],arguments[2],arguments[4],arguments[5],arguments[6],arguments[7]};
    c.pointed=captureNativeOffscreenResolveArguments(r,argumentReader);
    c.destinationHeader=captureNativeResolveTextureHeader(arguments[3],heap,graphics);
    const auto device=arguments[0];
    if(heap && device && !(device&3) && device<=UINT32_MAX-13859) {
        auto bytes=heap(device,13860);
        if(bytes && bytes->size()==13860)c.state=std::move(bytes);
    }
    if(c.state)c.surfaces=captureGta5BoundSurfaces(*c.state,heap,graphics,40);
    if(c.state) {
        const auto& b=*c.state;
        const auto fenceBase=(std::uint32_t{b[11024]}<<24)|(std::uint32_t{b[11025]}<<16)|
            (std::uint32_t{b[11026]}<<8)|b[11027];
        if(graphics && fenceBase && !(fenceBase&3) && fenceBase<=UINT32_MAX-7) {
            auto bytes=graphics(fenceBase+4,4);
            if(bytes && bytes->size()==4)c.allocatorFence=std::move(bytes);
        }
        if(titleImage) {
            auto bytes=titleImage(Gta5TileResolveCapture::ResolveGlobalsAddress,12);
            if(bytes && bytes->size()==12)c.resolveGlobals=std::move(bytes);
        }
    }
    if(c.state && device<=UINT32_MAX-(Gta5TileResolveCapture::AllocatorTailEnd-1)) {
        constexpr auto size=Gta5TileResolveCapture::AllocatorTailEnd-Gta5TileResolveCapture::DevicePrefixBytes;
        auto bytes=heap(device+Gta5TileResolveCapture::DevicePrefixBytes,size);
        if(bytes && bytes->size()==size) {
            c.allocatorTail=std::move(bytes);
            // 823BBD00 selects on word152; 836B7E50 also reads word156,
            // and 836B80C0 reads callback context164 and function172. Capture
            // this full direct header, without following its callback pointers.
            const auto& b=*c.allocatorTail;
            const auto object=(std::uint32_t{b[60]}<<24)|(std::uint32_t{b[61]}<<16)|
                (std::uint32_t{b[62]}<<8)|b[63];
            if(object && !(object&3) && object<=UINT32_MAX-(Gta5TileResolveCapture::AllocatorHeaderBytes-1)) {
                auto header=heap(object,Gta5TileResolveCapture::AllocatorHeaderBytes);
                if(header && header->size()==Gta5TileResolveCapture::AllocatorHeaderBytes)c.allocatorHeader=std::move(header);
            }
        }
    }
    return c;
}
}
