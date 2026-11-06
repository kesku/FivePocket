#pragma once
#include "gta5_tile_resolve_capture.h"
#include "native_edram32_bands.h"
#include "xenos_float32_texture.h"

namespace recomp::gpu {
// Xenia IsColorResolveFormatBitwiseEquivalent: RT14 -> texture36. No float
// conversion, channel swizzle, clear or exponent bias. Original CPU setup for
// the observed copies emit CONTROL00100000/INFO0000F202, with PITCH00400040
// for64x64,00100020 for16x16,00040020 for4x4 and00010020 for1x1.
// This typed physical owner is independent of presentation-image validity.
struct NativeR32FloatResolvePlan {
    unsigned sourceObject=0,destinationObject=0;
    XenosTextureDescription destination;
    XenosTexture2DLayout storage;
    unsigned Width=0,Height=0;
    static constexpr unsigned Pitch=80;
    unsigned Base=1440;
};
inline std::optional<NativeR32FloatResolvePlan> planNativeR32FloatResolve(
        const Gta5TileResolveCapture& c,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeR32FloatResolvePlan>{};};
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto zero=[](const auto& b){return b && b->size()==4 && std::all_of(b->begin(),b->end(),[](auto v){return !v;});};
    const auto& a=c.arguments;
    if(!a[0] || a[0]%4 || a[0]>UINT32_MAX-13859 || !c.state || c.state->size()!=13860 || !c.surfaces)
        return no("source-state");
    if(a[1] || a[4] || a[5] || a[6])return no("copy-flags-subresource");
    if(!c.stack || c.stack%4 || c.stack>UINT32_MAX-103 || !zero(c.pointed.stack92) || !zero(c.pointed.stack100))
        return no("stack-arguments");
    if(!a[2] || a[2]%4 || a[2]>UINT32_MAX-15 || !c.pointed.rectangle || c.pointed.rectangle->size()!=16 ||
       word(*c.pointed.rectangle,0) || word(*c.pointed.rectangle,4))return no("rectangle");
    const auto size=word(*c.pointed.rectangle,8);
    if((size!=64 && size!=16 && size!=4 && size!=1) || word(*c.pointed.rectangle,12)!=size)return no("rectangle");
    // Each complete profile is backed by its original CPU-copy prefix and live
    // operands. A matching size alone never authorizes copy flags or resources.
    const auto sourceView=size==64?0x01000050u:0x00800050u;
    const auto sourceExtent=size==64?0x00FC01F8u:size==16?0x003C0078u:size==4?0x000C0018u:0u;
    const unsigned sourceBase=size==1?720:1440,sourceColor=0x000E0000|sourceBase;
    const auto destinationPitch=size==64?0x80800156u:0x80400156u;
    const auto destinationExtent=size==64?0x7E03Fu:size==16?0x1E00Fu:size==4?0x6003u:0u;
    const auto& state=*c.state;const auto& source=c.surfaces->surfaces[0];
    for(unsigned i=0;i<5;++i)if(c.surfaces->surfaces[i].object!=word(state,12816+4*i))return no("binding");
    for(unsigned i=1;i<5;++i)if(c.surfaces->surfaces[i].object)return no("extra-attachment");
    if(!source.object || source.object%4 || source.object>UINT32_MAX-39 || !source.header || source.header->size()!=40)
        return no("source-header");
    if(word(*source.header,24)!=sourceView || word(*source.header,28)!=sourceColor ||
       word(*source.header,36)!=sourceExtent || word(state,10368)!=sourceView ||
       word(state,10372)!=sourceColor || word(state,12408) || word(state,11068)!=0xC00)
        return no("source-format-view");
    if(!a[3] || a[3]%4 || a[3]>UINT32_MAX-51 || !c.destinationHeader || c.destinationHeader->size()!=52)
        return no("destination-header");
    const auto d=describeGta5TextureHeader(*c.destinationHeader);if(!d)return no("destination-decode");
    const auto& f=d->fetch;
    if(f[0]!=destinationPitch || (f[1]&0xFFF)!=0xA4 || f[2]!=destinationExtent || f[3]!=0x16D1 || f[4] || f[5]!=0x200)
        return no("destination-storage-profile");
    const auto storage=planXenosFloat32Storage(*d);
    if(!storage || storage->bytesPerTexel!=4 || storage->linearBytes!=size*size*4)return no("destination-layout");
    if(d->baseAddress>=0x20000000u && (d->baseAddress<0xA0000000u || d->baseAddress>=0xC0000000u))return no("destination-namespace");
    const auto physical=d->baseAddress<0x20000000u?d->baseAddress:d->baseAddress-0xA0000000u;
    if(!physical || std::uint64_t{physical}+storage->sourceBytes>0x20000000u)return no("destination-extent");
    if(why)*why="r32-float-layout-source-unproven";
    return NativeR32FloatResolvePlan{source.object,a[3],*d,*storage,size,size,sourceBase};
}
inline std::optional<std::vector<std::uint8_t>> packNativeR32FloatResolve(
        const Gta5TileResolveCapture& capture,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& previous,const char** why=nullptr) {
    const auto p=planNativeR32FloatResolve(capture,why);if(!p)return std::nullopt;
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<std::uint8_t>>{};};
    if(previous.size()!=p->storage.sourceBytes)return no("destination-bytes");
    const auto source=owner.readKnownColorRows(p->Base,p->Pitch,p->Width,p->Height);
    if(!source)return no("physical-r32-unknown");
    std::vector<std::uint8_t> linear(p->storage.linearBytes);
    for(std::size_t i=0;i<source->size();++i)
        for(unsigned b=0;b<4;++b)linear[i*4+b]=(*source)[i]>>(8*b);
    auto packed=storeXenosTexture2D(p->storage.description,linear,previous);
    if(why)*why=packed?"r32-float-packed-no-publication":"destination-address";
    return packed;
}
inline bool sameNativeR32FloatInputs(const Gta5TileResolveCapture& before,const Gta5TileResolveCapture& now) {
    if(!planNativeR32FloatResolve(before) || !planNativeR32FloatResolve(now) || before.stack!=now.stack ||
       before.arguments!=now.arguments || before.state!=now.state || before.destinationHeader!=now.destinationHeader ||
       before.pointed.rectangle!=now.pointed.rectangle || before.pointed.stack92!=now.pointed.stack92 ||
       before.pointed.stack100!=now.pointed.stack100)return false;
    return before.surfaces->surfaces[0].header==now.surfaces->surfaces[0].header;
}
template<class Current,class Publish>
bool publishNativeR32FloatResolve(const Gta5TileResolveCapture& capture,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& previous,Current&& current,Publish&& publish,const char** why=nullptr) {
    const auto completed=packNativeR32FloatResolve(capture,owner,previous,why);if(!completed)return false;
    if(!current()){if(why)*why="changed-inputs";return false;}
    if(!publish(*completed)){if(why)*why="publication";return false;}
    if(why)*why="r32-float-published";return true;
}
}
