#pragma once
#include "gta5_tile_resolve_capture.h"
#include "native_edram32_bands.h"
#include "xenos_expanded16_float_texture.h"

namespace recomp::gpu {
// Every RGB7e3 code is finite and exactly representable as binary16. Direct
// integer normalization avoids imposing a host floating-point rounding policy.
// Alpha UNORM2 has only four values; RNE and RTZ agree for all four float32
// values used by Xenia's unpack -> packHalf operation (exhaustively tested).
inline std::uint16_t rgb7e3ToBinary16(unsigned code) {
    const unsigned exponent=code>>7,mantissa=code&127;
    if(exponent)return static_cast<std::uint16_t>(((exponent+12)<<10)|(mantissa<<3));
    if(!mantissa)return 0;
    unsigned leading=0;for(unsigned n=mantissa;n>1;n>>=1)++leading;
    return static_cast<std::uint16_t>(((leading+6)<<10)|((mantissa-(1u<<leading))<<(10-leading)));
}
inline std::array<std::uint16_t,4> color7e3ToBinary16(std::uint32_t word) {
    constexpr std::array<std::uint16_t,4> alpha{0,0x3555,0x3955,0x3C00};
    return {rgb7e3ToBinary16(word&1023),rgb7e3ToBinary16((word>>10)&1023),
            rgb7e3ToBinary16((word>>20)&1023),alpha[word>>30]};
}
// Full-rectangle ordinaryRT3 copies, one admitted (width,height,base,flags)
// per row. Every checked word below is DERIVED from its row and must match
// exactly; the row list is the only admission, not a size range.
//
//   640x360 base720 flags0      OBSERVED captured call (filter-resolve v414).
//   320x180 base720 flags0      OBSERVED captured call (max-copy v439) and live
//                               RENDER_API_RGB7E3_FLOAT_RESOLVE object10271110.
//   160x90  base768 flags0x100  OBSERVED captured call (hg739jnq, LR823889E8,
//        after live ABI41): surface0 only, RB_SURFACE_INFO028000A0,
//        RB_COLOR_INFO00030300, dims027C02C8, 11068=0C00, 12408=0, destination
//        fetch81400156/A650A05D/000B209F, clear vector {0,0,0,1.0f}. Its
//        original prefix (t1-resolve v579, including CRT ldexp) replays
//        control00100100 base0650A000 pitch005A00A0 info0000F001 and
//        RB_COLOR_CLEAR C0000000. Flag origin: 8237C8E8 passes r7=1 to
//        82916520 (options+20); 823AAC90 copies options to RT vtable+164 =
//        82388720, which ORs 0x100 onto table[0]=0 (pass65 r7=0 is flags0).
//   80x45   base780 flags0x100  OBSERVED captured call (1jamjqig, after live
//        second pass71): RB_SURFACE_INFO01800050 (pitch80, hi-Z pitch96),
//        RB_COLOR_INFO0003030C, dims013C0160, destination fetch80C00156 (tiled
//        pitch96)/A654605D/0005804F, clear vector {0,0,0,1.0f}.
//   160x90  base768 flags0      OBSERVED captured call (99jco_dd, after live
//        pass83's additive upsample): the T1 call's words exactly, flags0 (no
//        clear); original prefix (t1b-resolve v635) replays control00100000
//        base0650A000 pitch005A00A0 and emits no RB_COLOR_CLEAR.
//
// All observed rows use hi-Z pitch == tiled destination pitch == width
// rounded up to 32 (640,320,160; 80->96); only the 80x45 row is padded.
// Original setup replays control00100000 (00100100 with flag0x100),
// info0000F001, pitch=(height<<16)|width: colour0/sample0, no copy bias, no
// R/B swap. Does NOT turn a stale display image into a valid source.
struct NativeRgb7e3FloatResolveProfile {unsigned width,height,base,flags;};
inline constexpr NativeRgb7e3FloatResolveProfile NativeRgb7e3FloatResolveProfiles[]{
    {640,360,720,0},{320,180,720,0},{160,90,768,0x100},{80,45,780,0x100},{160,90,768,0}};
// EDRAM pitch equals width (80-sample tiles); hi-Z and tiled destination
// pitches are the width padded to 32 texels (only 80x45 pads, to 96).
constexpr unsigned nativeRgb7e3FloatResolvePaddedPitch(unsigned width){return (width+31)/32*32;}
constexpr bool nativeRgb7e3FloatResolveProfilesExact() {
    for(const auto& p:NativeRgb7e3FloatResolveProfiles)
        if(!p.width || !p.height || p.width%80 || p.width>1280 || p.height>720 ||
           p.base>=2048 || (p.flags!=0 && p.flags!=0x100))return false;
    return nativeRgb7e3FloatResolvePaddedPitch(640)==640 && nativeRgb7e3FloatResolvePaddedPitch(80)==96;
}
static_assert(nativeRgb7e3FloatResolveProfilesExact());
struct NativeRgb7e3FloatResolvePlan {
    unsigned sourceObject=0,destinationObject=0;
    XenosTextureDescription destination;
    XenosTexture2DLayout storage;
    unsigned width=0,height=0,pitch=0,base=0;
    // 823C2738 flag0x100 (RB_COPY_CONTROL color_clear_enable): after the copy
    // the GPU clears the source rectangle, rounded up to 8 rows (823C2C68), to
    // RB_COLOR_CLEAR. The caller records that clear after publication with
    // recordNativeRgb7e3FloatResolveClear (below); every admitted clearing row
    // ends on a tile boundary, so the footprint becomes fully KNOWN.
    bool clearsSource=false;
    unsigned clearTiles=0;
};
inline std::optional<NativeRgb7e3FloatResolvePlan> planNativeRgb7e3FloatResolve(
        const Gta5TileResolveCapture& c,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeRgb7e3FloatResolvePlan>{};};
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto zero=[](const auto& b){return b && b->size()==4 && std::all_of(b->begin(),b->end(),[](auto v){return !v;});};
    const auto& a=c.arguments;
    if(!a[0] || a[0]%4 || a[0]>UINT32_MAX-13859 || !c.state || c.state->size()!=13860 || !c.surfaces)
        return no("source-state");
    if(a[4] || a[5] || a[6])return no("copy-flags-subresource");
    if(!c.stack || c.stack%4 || c.stack>UINT32_MAX-103 || !zero(c.pointed.stack92) || !zero(c.pointed.stack100))
        return no("stack-arguments");
    if(!a[2] || a[2]%4 || a[2]>UINT32_MAX-15 || !c.pointed.rectangle || c.pointed.rectangle->size()!=16)
        return no("rectangle");
    const unsigned width=word(*c.pointed.rectangle,8),height=word(*c.pointed.rectangle,12);
    // Several rows may share a size (160x90: flags0x100 for the first T1 resolve,
    // flags0 for its re-resolve after pass83). The row is the one whose flags
    // match; otherwise the first row of that size, whose flags then refuse.
    const auto sameSize=[&](const auto& p){return p.width==width && p.height==height;};
    const auto exact=std::find_if(std::begin(NativeRgb7e3FloatResolveProfiles),std::end(NativeRgb7e3FloatResolveProfiles),
        [&](const auto& p){return sameSize(p) && p.flags==a[1];});
    const auto profile=exact!=std::end(NativeRgb7e3FloatResolveProfiles)?exact:
        std::find_if(std::begin(NativeRgb7e3FloatResolveProfiles),std::end(NativeRgb7e3FloatResolveProfiles),sameSize);
    if(word(*c.pointed.rectangle,0) || word(*c.pointed.rectangle,4) || profile==std::end(NativeRgb7e3FloatResolveProfiles))
        return no("rectangle");
    // Flags are exact per row: 0x100 is colour clear only (823C2738 bit8 ->
    // 823482A8); depth clear0x200, sample select, controls and bias refuse.
    if(a[1]!=profile->flags)return no("copy-flags-subresource");
    if(profile->flags) {
        // 82388720 builds the vector at its own frame+128 from options+8
        // (82916520 stores FF000000): {R,G,B,A}={bits16-23,8-15,0-7,24-31}
        // times the 3744(0x82000000) scale (1/255). The captured hg739jnq
        // vector is exactly {0,0,0,1.0f}; any other captured vector refuses.
        constexpr std::uint32_t expected[4]{0,0,0,0x3F800000};
        if(!a[7] || a[7]%16 || a[7]>UINT32_MAX-15 || !c.pointed.color || c.pointed.color->size()!=16)
            return no("clear-color");
        for(unsigned i=0;i<4;++i)if(word(*c.pointed.color,4*i)!=expected[i])return no("clear-color");
    }
    // RB_SURFACE_INFO: pitch=width, hiz pitch=padded width, 1x. RB_COLOR_INFO:
    // base, format3 (k_2_10_10_10_FLOAT), no exponent bias (also the clear
    // packer's input). Header dims word: (width-1)<<18 | (height-1)<<3.
    const unsigned padded=nativeRgb7e3FloatResolvePaddedPitch(width);
    const unsigned sourceInfo=(padded<<18)|width,colorInfo=0x30000u|profile->base;
    const unsigned sourceExtent=((width-1)<<18)|((height-1)<<3);
    const auto& state=*c.state;const auto& source=c.surfaces->surfaces[0];
    for(unsigned i=0;i<5;++i)if(c.surfaces->surfaces[i].object!=word(state,12816+4*i))return no("binding");
    for(unsigned i=1;i<5;++i)if(c.surfaces->surfaces[i].object)return no("extra-attachment");
    if(!source.object || source.object%4 || source.object>UINT32_MAX-39 || !source.header || source.header->size()!=40)
        return no("source-header");
    if(word(*source.header,24)!=sourceInfo || word(*source.header,28)!=colorInfo ||
       word(*source.header,36)!=sourceExtent || word(state,10368)!=sourceInfo ||
       word(state,10372)!=colorInfo || word(state,12408) || word(state,11068)!=0xC00)
        return no("source-format-view");
    if(!a[3] || a[3]%4 || a[3]>UINT32_MAX-51 || !c.destinationHeader || c.destinationHeader->size()!=52)
        return no("destination-header");
    const auto d=describeGta5TextureHeader(*c.destinationHeader);if(!d)return no("destination-decode");
    const auto& f=d->fetch;
    // Tiled format29 EXPAND, pitch padded/32 at bits22-30, extent (w-1)|(h-1)<<13:
    // reproduces all four observed headers (85000156/2CE27F, 82800156/16613F,
    // 81400156/0B209F, 80C00156/05804F).
    const unsigned destinationPitch=0x80000156u|((padded/32)<<22);
    const unsigned destinationExtent=(width-1)|((height-1)<<13);
    if(f[0]!=destinationPitch || (f[1]&0xFFF)!=0x5D || f[2]!=destinationExtent || f[3]!=0xD11 || f[4] || f[5]!=0x200)
        return no("destination-storage-profile");
    const auto storage=planXenosExpanded16FloatStorage(*d);
    if(!storage)return no("destination-layout");
    if(d->baseAddress>=0x20000000u && (d->baseAddress<0xA0000000u || d->baseAddress>=0xC0000000u))return no("destination-namespace");
    const auto physical=d->baseAddress<0x20000000u?d->baseAddress:d->baseAddress-0xA0000000u;
    if(!physical || std::uint64_t{physical}+storage->sourceBytes>0x20000000u)return no("destination-extent");
    if(why)*why="rgb7e3-float-layout-source-unproven";
    return NativeRgb7e3FloatResolvePlan{source.object,a[3],*d,*storage,width,height,width,profile->base,
        profile->flags!=0,(width/80)*((height+15)/16)};
}
// RB_COLOR_CLEAR for the one admitted clear vector {0,0,0,1.0f}: the original
// CPU prefix emits C0000000 for both clearing calls (t1-resolve v579 and
// t2-resolve v598, PM4 0x231E), equal to the XeniaShiftRound 7e3 packing of
// that vector (host test). The prefix's copy window rounds the height to 8
// (006000A0 for 160x90, 00300050 for 80x45).
inline constexpr std::uint32_t NativeRgb7e3FloatResolveClearWord=0xC0000000u;
constexpr unsigned nativeRgb7e3FloatResolveClearRows(unsigned height){return (height+7)/8*8;}
constexpr bool nativeRgb7e3FloatResolveClearsTileAligned() {
    for(const auto& p:NativeRgb7e3FloatResolveProfiles)
        if(p.flags && nativeRgb7e3FloatResolveClearRows(p.height)!=(p.height+15)/16*16)return false;
    return true;
}
static_assert(nativeRgb7e3FloatResolveClearsTileAligned());
// After publication only, under the same physical mutation as the copy.
inline bool recordNativeRgb7e3FloatResolveClear(NativeEdram32Bands& physical,const NativeRgb7e3FloatResolvePlan& plan) {
    if(!plan.clearsSource)return false;
    const unsigned rows=nativeRgb7e3FloatResolveClearRows(plan.height);
    if(plan.pitch!=plan.width || (plan.width/80)*(rows/16)!=plan.clearTiles)return false;
    std::vector<std::uint8_t> bytes(std::size_t{plan.width}*rows*4);
    for(std::size_t i=0;i<bytes.size();i+=4)bytes[i+3]=NativeRgb7e3FloatResolveClearWord>>24; // Little-endian word.
    return physical.recordCompletedRawColorRows(plan.base,plan.pitch,plan.width,rows,bytes,physical.nextSerial());
}
inline std::optional<std::vector<std::uint8_t>> packNativeRgb7e3FloatResolve(
        const Gta5TileResolveCapture& capture,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& previous,const char** why=nullptr) {
    const auto p=planNativeRgb7e3FloatResolve(capture,why);if(!p)return std::nullopt;
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<std::uint8_t>>{};};
    if(previous.size()!=p->storage.sourceBytes)return no("destination-bytes");
    const auto source=owner.readKnownColorRows(p->base,p->pitch,p->width,p->height);
    if(!source)return no("physical-colour-unknown");
    std::vector<std::uint8_t> linear(p->storage.linearBytes);
    for(std::size_t i=0;i<source->size();++i) {
        const auto pixel=color7e3ToBinary16((*source)[i]);
        for(unsigned c=0;c<4;++c){linear[i*8+c*2]=pixel[c]&255;linear[i*8+c*2+1]=pixel[c]>>8;}
    }
    auto packed=storeXenosTexture2D(p->storage.description,linear,previous);
    if(why)*why=packed?"rgb7e3-float-packed-no-publication":"destination-address";
    return packed;
}
inline bool sameNativeRgb7e3FloatInputs(const Gta5TileResolveCapture& before,const Gta5TileResolveCapture& now) {
    if(!planNativeRgb7e3FloatResolve(before) || !planNativeRgb7e3FloatResolve(now) || before.stack!=now.stack ||
       before.arguments!=now.arguments || before.state!=now.state || before.destinationHeader!=now.destinationHeader ||
       before.pointed.rectangle!=now.pointed.rectangle || before.pointed.stack92!=now.pointed.stack92 ||
       before.pointed.stack100!=now.pointed.stack100 || before.pointed.color!=now.pointed.color)return false;
    return before.surfaces->surfaces[0].header==now.surfaces->surfaces[0].header;
}
template<class Current,class Publish>
bool publishNativeRgb7e3FloatResolve(const Gta5TileResolveCapture& capture,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& previous,Current&& current,Publish&& publish,const char** why=nullptr) {
    const auto completed=packNativeRgb7e3FloatResolve(capture,owner,previous,why);if(!completed)return false;
    if(!current()){if(why)*why="changed-inputs";return false;}
    if(!publish(*completed)){if(why)*why="publication";return false;}
    if(why)*why="rgb7e3-float-published";return true;
}
}
