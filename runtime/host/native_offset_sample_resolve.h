#pragma once
#include "native_offscreen_draw_plan.h"
#include "native_offscreen_resolve_request.h"
#include "native_rectangle_mrt_resolve.h"
#include "xenos_float32_texture.h"

namespace recomp::gpu {
// Resolve out of a completed ABI23/ABI24 offset sample target.
//
// THE FLAGS WORD. sub_82388A38 forwards r3/r4/r5 to sub_823C2738 unchanged, and
// that function reads exactly these fields of r4 and nothing else:
//
//   bits 4-6   copy_sample_select     bits 8,9  colour/depth clear enable
//   bits 13,14,15  three single-bit controls
//   bits 26-31 read at +367, then assembled at +476..502 into the word the
//              guest writes as RB_COPY_DEST_INFO:
//
//     r28 = (field << 3) | number        r10 = ((r28 << 6) | destFormat) << 7
//
//   which lands destFormat at bit 7, copy_dest_number at 13 and the field at
//   bit 16. Bit 16 of RB_COPY_DEST_INFO is copy_dest_exp_bias, a SIGNED six-bit
//   exponent (registers.h:877). So bits 26-31 of the flags word are that bias.
//
// Bits 0-3, 7, 10-12 and 16-25 are never read there, so copy_src_select is NOT
// taken from this word on this path. The ABI22 route below reads it as if it
// were, and is correct only because its plan independently cross-checks the
// destination's format against the source's.
//
// The device recorded 0xec000000 for the ABI24 tonemap resolve: every consumed
// field zero except the bias, which is 59 -- signed, -5, a scale of 2^-5. That
// is exactly the [-32,32] to [-1,1] conversion k_16_16 needs, and it comes from
// the guest rather than from an assumption here. It also explains why that
// resolve cannot be byte-identical: the reference takes its fast byte-copy path
// only when copy_dest_exp_bias is zero AND the formats are bitwise equivalent
// (draw.cpp:1122), and k_16_16 satisfies neither.
//
// The destination the device reported alongside it: format 25 (k_16_16), extent
// 640x360, sign selects 0,0,0,0 -- TextureSign::kUnsigned -- and
// integerComponents 0. Only that normalized unsigned destination is admitted
// for this conversion; signed/integer sampling requires separate evidence.
//
// THE EXPONENT BIAS IS ALREADY PAID. The reference divides a k_16_16 target's
// shader output by 32 to fit it into a host [-1,1] attachment -- "Remap from
// -32...32 to -1...1 by dividing the output values by 32, losing blending
// correctness, but getting the full range" (command_processor.cpp:6241) -- which
// is exactly what the ABI24 pixel stage does and why. Its resolve then ADDS 5
// back for a target truncated that way (draw.cpp:1076), because the stored
// halves no longer mean what the guest's bias assumes.
//
// So for this band the guest's -5 and the attachment's own +5 cancel: the net
// exponent bias is zero. Any OTHER bias would mean a
// scale this route does not implement, so only -5 is admitted.
//
// Numeric conversion remains: SNORM16 is decoded with denominator 32767,
// negative values clamp to zero, then UNORM16 uses 65535 and nearest rounding.
// This is NOT the clear-path sign-bit XOR: zero must remain zero, not 0x8000.
// Independent reference: Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
// resolve.xesli (unpack/exp_bias), resolve_full_32bpp.xesli (pack), and
// pixel_formats.xesli: XePackR16G16UNorm. Tests use that floating-point oracle,
// independently of the integer-rational implementation below.
//
// The colour-RT to texture format mapping is the reference emulator's own
// (rex/graphics/xenos.h): k_16_16_16_16_FLOAT(7) -> ColorFormat 32 and
// k_32_FLOAT(14) -> 36. The captured pass writes format 7, so its destination is
// eight bytes per texel and the copy is byte-for-byte after tiling.
// How a completed target's bytes become the destination's.
enum class NativeOffsetSampleResolveCopy {
    ByteIdentical,           // The two float pairs the reference calls equivalent.
    SnormToUnorm16,          // k_16_16(4) -> unsigned normalized texture 25.
    RedChannelToR8,          // k_8_8_8_8(0) -> texture 2, one component kept.
};
struct NativeOffsetSampleResolvePlan {
    XenosTexture2DLayout storage;
    unsigned width=0,height=0;
    NativeOffsetSampleResolveCopy copy=NativeOffsetSampleResolveCopy::ByteIdentical;
};
// Colour RT format 4 is k_16_16 and its destination is texture format 25; every
// other format this band writes copies byte for byte.
inline constexpr bool nativeOffsetSampleResolveConvertsFixed16(unsigned sourceFormat) {
    return sourceFormat==4;
}
// k_8_8_8_8 is bitwise equivalent to texture formats 6, 14 and 50 but NOT to 2,
// the single-byte k_8 (reference xenos.h:569). A k_8 destination therefore keeps
// one component. This route takes component 0, and for the pass that reaches it
// the choice cannot be observed at all: its pixel stage exports the same value
// to all four, `color=vec4(n,n,n,n)`. A pass whose components differ would need
// evidence for which one the hardware keeps, and is refused below until it has
// some.
inline constexpr bool nativeOffsetSampleResolveKeepsRed(unsigned sourceFormat,unsigned destinationFormat) {
    return sourceFormat==0 && destinationFormat==2;
}
inline std::optional<NativeOffsetSampleResolvePlan> planNativeOffsetSampleResolve(
        const NativeOffscreenDrawPlan& source,const NativeOffscreenResolveRequest& r,
        const NativeOffscreenResolveArguments& a,const XenosTextureDescription& destination,
        const char** why=nullptr) {
    const auto no=[&](const char* reason){if(why)*why=reason;return std::optional<NativeOffsetSampleResolvePlan>{};};
    // Only the fields sub_823C2738 actually reads carry meaning; the rest must
    // be clear rather than ignored, so an unfamiliar word refuses instead of
    // being silently treated as one of these.
    constexpr std::uint32_t unread=0x03ff1c8fu;   // bits 0-3, 7, 10-12, 16-25
    if(r.flags&unread)return no("resolve-flags");
    if((r.flags>>4)&7u)return no("resolve-sample-select");
    if((r.flags>>8)&3u)return no("resolve-clear");           // colour/depth clear
    if((r.flags>>13)&7u)return no("resolve-controls");        // bits 13,14,15
    const auto bias=int((r.flags>>26)&63u)-((r.flags>>31)&1u?64:0);
    const bool fixed16=nativeOffsetSampleResolveConvertsFixed16(source.format);
    // copy_dest_exp_bias. The float targets store what the guest expects, so
    // they take none; the fixed-point one is already divided by 32 in the pixel
    // stage, which is what the guest's -5 asks for, leaving a net of zero.
    if(bias!=(fixed16?-5:0))return no("resolve-exponent-bias");
    const auto decoded=XenosTextureDescription::decode(destination.fetch);
    if(!decoded)return no("destination-format");
    const auto keepsRed=nativeOffsetSampleResolveKeepsRed(source.format,decoded->format);
    const auto expected=nativeRectangleMrtResolveDestinationFormat(source.format);
    if(!expected && !keepsRed)return no("source-colour-format");
    if(!keepsRed && decoded->format!=expected)return no("destination-format");
    if(fixed16 && (decoded->integerComponents ||
       std::any_of(decoded->sign.begin(),decoded->sign.end(),[](auto sign){return sign!=0;})))
        return no("destination-numeric");
    // The single-byte destination is ordinary addressable colour, so it takes
    // the ordinary plan; only the float formats need the raw view.
    const auto storage=keepsRed?planXenosTexture2D(*decoded):planXenosFloatStorage(destination);
    if(!storage)return no("destination-storage");
    if(keepsRed && storage->bytesPerTexel!=1)return no("destination-storage");
    if(!source.object || source.object%4 || !source.width || !source.height ||
       source.width>1280 || source.height>720)return no("source-extent");
    if(storage->description.width!=source.width || storage->description.height!=source.height)
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
       word(*a.rectangle,8)!=source.width || word(*a.rectangle,12)!=source.height))
        return no("resolve-rectangle");
    if(why)*why="planned";
    return NativeOffsetSampleResolvePlan{*storage,source.width,source.height,
        keepsRed?NativeOffsetSampleResolveCopy::RedChannelToR8:
        fixed16?NativeOffsetSampleResolveCopy::SnormToUnorm16:
               NativeOffsetSampleResolveCopy::ByteIdentical};
}
// The attachment bytes are the real fenced readback of the completed target, in
// their native element layout. Apply the selected numeric/component conversion,
// tiling and guest endian permutation; preserve the previous allocation's padding.
inline std::optional<std::vector<std::uint8_t>> packNativeOffsetSampleResolve(
        const NativeOffsetSampleResolvePlan& p,const std::vector<std::uint8_t>& attachment,
        const std::vector<std::uint8_t>& previous) {
    const auto texel=p.storage.bytesPerTexel;
    const auto pixels=std::uint64_t{p.width}*p.height;
    if(p.copy==NativeOffsetSampleResolveCopy::RedChannelToR8) {
        // The attachment is RGBA8; the destination keeps one byte per pixel.
        if(texel!=1 || attachment.size()!=pixels*4 || previous.size()!=p.storage.sourceBytes ||
           p.storage.linearBytes!=pixels)return std::nullopt;
        std::vector<std::uint8_t> kept(static_cast<std::size_t>(pixels));
        for(std::size_t at=0;at<kept.size();++at)kept[at]=attachment[at*4];
        return storeXenosTexture2D(p.storage.description,kept,previous);
    }
    if((texel!=4 && texel!=8) ||
       attachment.size()!=pixels*texel ||
       previous.size()!=p.storage.sourceBytes ||
       p.storage.linearBytes!=attachment.size())return std::nullopt;
    if(p.copy==NativeOffsetSampleResolveCopy::ByteIdentical)
        return storeXenosTexture2D(p.storage.description,attachment,previous);
    if(p.copy!=NativeOffsetSampleResolveCopy::SnormToUnorm16 || texel!=4)return std::nullopt;
    auto converted=attachment;
    for(std::size_t at=0;at<converted.size();at+=2) {
        const auto bits=std::uint32_t{attachment[at]}|(std::uint32_t{attachment[at+1]}<<8);
        // Nearest UNORM16 quantization of max(snorm16,0). Denominator is odd,
        // so no exact halfway case exists; all products fit in uint32_t.
        const auto value=(bits&0x8000u)?0u:(bits*65535u+16383u)/32767u;
        converted[at]=static_cast<std::uint8_t>(value);
        converted[at+1]=static_cast<std::uint8_t>(value>>8);
    }
    return storeXenosTexture2D(p.storage.description,converted,previous);
}
}
