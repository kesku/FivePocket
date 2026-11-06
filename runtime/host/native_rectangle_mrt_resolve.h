#pragma once
#include "native_rectangle_mrt_plan.h"
#include "native_offscreen_resolve_request.h"
#include "xenos_float32_texture.h"

namespace recomp::gpu {
// Resolve out of a completed ABI22 rectangle MRT target.
//
// The original 82388A38 is a shim onto 823C2738, which selects the resolve
// SOURCE with the low three bits of the flags word:
//
//   clrlwi r11,r21,29        ; r11 = flags & 7
//   cmplwi cr6,r11,4         ; ==4 selects depth
//   lwz    r11,12832(r31)    ;   -> device[12832]
//   addi   r11,r11,3204      ; else r11 = (flags&7) + 3204
//   rlwinm r11,r11,2,0,29    ;      *= 4
//   lwzx   r11,r11,r31       ;      -> device[12816 + (flags&7)*4]
//
// 3204*4 is 12816, the colour target register block, so 0-3 name colour0-3 and
// 4 names depth. That is also why tryResolveFixed16Target opens with
// `if(ctx.r4.u32&7u)return std::nullopt;` — it handles source index 0 only.
//
// This band admits colour sources 0 and 1, the two the ABI22 pass writes. Depth
// is not admitted: this pass binds no depth. Bit 8 (region clear) is refused
// rather than ignored, because clearing the source is a separate authority.
struct NativeRectangleMrtResolvePlan {
    XenosTexture2DLayout storage;
    unsigned width=0,height=0,sourceSlot=0;
};
// Each admitted source format maps to exactly one destination texture format,
// so the copy is byte-for-byte after tiling and never a silent conversion:
//   colour k_32_FLOAT (14)          -> texture k_32_FLOAT (36),          4 B/texel
//   colour k_16_16_16_16_FLOAT (7)  -> texture k_16_16_16_16_FLOAT (32),  8 B/texel
// This is the reference emulator's own colour-RT-to-ColorFormat mapping
// (rex/graphics/xenos.h: ColorRenderTargetFormat 7/14 -> ColorFormat 32/36),
// and neither pair needs a conversion: only tiling and the endian permutation.
// Colour RT format to the texture format its resolve destination carries. This
// is a property of Xenos, not of a band: k_32_FLOAT(14) -> 36,
// k_16_16_16_16_FLOAT(7) -> 32 and k_16_16(4) -> 25 (reference xenos.h:584).
// Only the first two are BITWISE equivalent; k_16_16 needs the signed-to-
// unsigned bias its own route applies, which is why this map alone never
// authorises a byte copy.
inline constexpr unsigned nativeRectangleMrtResolveDestinationFormat(unsigned colourFormat) {
    return colourFormat==14?36u:colourFormat==7?32u:colourFormat==4?25u:0u;
}
inline std::optional<NativeRectangleMrtResolvePlan> planNativeRectangleMrtResolve(
        const NativeRectangleMrtPlan& source,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription& destination,
        const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeRectangleMrtResolvePlan>{};};
    // Only the source-index field may be set. Every other bit, the region clear
    // included, names behaviour this band does not implement.
    if(r.flags&~7u)return no("resolve-flags");
    const auto slot=r.flags&7u;
    if(slot>=2)return no("resolve-source-slot");
    const auto expected=nativeRectangleMrtResolveDestinationFormat(source.formats[slot]);
    if(!expected)return no("source-colour-format");
    const auto decoded=XenosTextureDescription::decode(destination.fetch);
    if(!decoded || decoded->format!=expected)return no("destination-format");
    const auto storage=planXenosFloatStorage(destination);
    if(!storage)return no("destination-storage");
    const auto& target=source.targets[slot];
    if(!target.object || target.object%4 || !target.width || !target.height ||
       target.width>1280 || target.height>720)return no("source-extent");
    if(storage->description.width!=target.width || storage->description.height!=target.height)
        return no("destination-extent");
    if(r.mip || r.slice)return no("resolve-subresource");
    const auto zero=[](const auto& b,unsigned size) {
        return b && b->size()==size && std::all_of(b->begin(),b->end(),[](auto v){return !v;});
    };
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    if(!r.stack || (r.stack&3) || r.stack>UINT32_MAX-103 || !zero(a.stack92,4) || !zero(a.stack100,4))
        return no("resolve-stack");
    if(r.destinationPoint && ((r.destinationPoint&3) || !zero(a.point,8)))return no("resolve-destination-point");
    // An explicit rectangle must be the whole surface at the origin; a partial
    // region would leave the rest of the destination stale.
    if(r.rectangle && ((r.rectangle&3) || !a.rectangle || a.rectangle->size()!=16 ||
       word(*a.rectangle,0) || word(*a.rectangle,4) ||
       word(*a.rectangle,8)!=target.width || word(*a.rectangle,12)!=target.height))
        return no("resolve-rectangle");
    if(why)*why="planned";
    return NativeRectangleMrtResolvePlan{*storage,target.width,target.height,slot};
}
// The attachment bytes are the real fenced readback of the completed target:
// one IEEE binary32 per texel, little endian, exactly the destination's own
// element layout. Only tiling and the guest endian permutation are applied, and
// every padding byte in the owned previous allocation is preserved.
inline std::optional<std::vector<std::uint8_t>> packNativeRectangleMrtResolve(
        const NativeRectangleMrtResolvePlan& p,const std::vector<std::uint8_t>& attachment,
        const std::vector<std::uint8_t>& previous) {
    const auto texel=p.storage.bytesPerTexel;
    if((texel!=4 && texel!=8) ||
       attachment.size()!=std::uint64_t{p.width}*p.height*texel ||
       previous.size()!=p.storage.sourceBytes ||
       p.storage.linearBytes!=attachment.size())return std::nullopt;
    return storeXenosTexture2D(p.storage.description,attachment,previous);
}
}
