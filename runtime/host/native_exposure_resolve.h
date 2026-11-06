#pragma once
#include "gta5_tile_resolve_capture.h"
#include "native_edram32_bands.h"
#include "xenos_expanded16_float_texture.h"

namespace recomp::gpu {
// Actual post-exposure copy: RT7 at720 -> EXPAND fetch29. Original CPU setup
// emits COPY_DEST_INFO0000F001, whose storage format is32 (RGBA16F), not29.
// Xenia's RT7/texture32 bitwise equivalence therefore applies. No float
// conversion, source clear, exponent bias or presentation ownership is needed.
struct NativeExposureResolvePlan {
    unsigned sourceObject=0,destinationObject=0;
    XenosTextureDescription destination;
    XenosTexture2DLayout storage;
    static constexpr unsigned Base=720,Pitch=80,Width=1,Height=1;
};
inline std::optional<NativeExposureResolvePlan> planNativeExposureResolve(
        const Gta5TileResolveCapture& c,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeExposureResolvePlan>{};};
    const auto word=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|
        (unsigned{b[at+2]}<<8)|b[at+3];};
    const auto zero=[](const auto& b){return b && b->size()==4 && std::all_of(b->begin(),b->end(),[](auto v){return !v;});};
    const auto& a=c.arguments;
    if(!a[0] || a[0]%4 || a[0]>UINT32_MAX-13859 || !c.state || c.state->size()!=13860 || !c.surfaces)
        return no("source-state");
    if(a[1] || a[4] || a[5] || a[6])return no("copy-flags-subresource");
    if(!c.stack || c.stack%4 || c.stack>UINT32_MAX-103 || !zero(c.pointed.stack92) || !zero(c.pointed.stack100))
        return no("stack-arguments");
    if(!a[2] || a[2]%4 || a[2]>UINT32_MAX-15 || !c.pointed.rectangle || c.pointed.rectangle->size()!=16 ||
       word(*c.pointed.rectangle,0) || word(*c.pointed.rectangle,4) ||
       word(*c.pointed.rectangle,8)!=1 || word(*c.pointed.rectangle,12)!=1)return no("rectangle");
    const auto& state=*c.state;const auto& source=c.surfaces->surfaces[0];
    for(unsigned i=0;i<5;++i)if(c.surfaces->surfaces[i].object!=word(state,12816+4*i))return no("binding");
    for(unsigned i=1;i<5;++i)if(c.surfaces->surfaces[i].object)return no("extra-attachment");
    if(!source.object || source.object%4 || source.object>UINT32_MAX-39 || !source.header || source.header->size()!=40)
        return no("source-header");
    if(word(*source.header,24)!=0x00800050 || word(*source.header,28)!=0x000702D0 ||
       word(*source.header,36) || word(state,10368)!=0x00800050 || word(state,10372)!=0x000702D0 ||
       word(state,12408) || word(state,11068)!=0x8C08)return no("source-format-view");
    if(!a[3] || a[3]%4 || a[3]>UINT32_MAX-51 || !c.destinationHeader || c.destinationHeader->size()!=52)
        return no("destination-header");
    const auto d=describeGta5TextureHeader(*c.destinationHeader);if(!d)return no("destination-decode");
    const auto& f=d->fetch;
    if(f[0]!=0x80400156 || (f[1]&0xFFF)!=0x5D || f[2] || f[3]!=0xD11 || f[4] || f[5]!=0x200)
        return no("destination-storage-profile");
    const auto storage=planXenosExpanded16FloatStorage(*d);
    if(!storage || storage->bytesPerTexel!=8 || storage->linearBytes!=8)return no("destination-layout");
    if(d->baseAddress>=0x20000000u && (d->baseAddress<0xA0000000u || d->baseAddress>=0xC0000000u))
        return no("destination-namespace");
    const auto physical=d->baseAddress<0x20000000u?d->baseAddress:d->baseAddress-0xA0000000u;
    if(!physical || std::uint64_t{physical}+storage->sourceBytes>0x20000000u)return no("destination-extent");
    if(why)*why="exposure-copy-layout-source-unproven";
    return NativeExposureResolvePlan{source.object,a[3],*d,*storage};
}
inline std::optional<std::vector<std::uint8_t>> packNativeExposureResolve(
        const Gta5TileResolveCapture& c,const NativeEdram32Bands& owner,const std::vector<std::uint8_t>& previous,
        NativeEdram64Model model,const char** why=nullptr) {
    const auto p=planNativeExposureResolve(c,why);if(!p)return std::nullopt;
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<std::uint8_t>>{};};
    if(model!=NativeEdram64Model::Xenia40x16)return no("physical64-model-unproven");
    if(previous.size()!=p->storage.sourceBytes)return no("destination-bytes");
    // Exactly two known words; requiring/promoting a whole tile would lose
    // partial-word provenance. Explicitly mirror the wide-colour writer model.
    const auto source=owner.readKnownColorRows(p->Base,p->Pitch*2,p->Width*2,p->Height);
    if(!source)return no("physical-rgba16f-unknown");
    std::vector<std::uint8_t> linear(8);
    for(unsigned w=0;w<2;++w)for(unsigned b=0;b<4;++b)linear[w*4+b]=(*source)[w]>>(8*b);
    auto packed=storeXenosTexture2D(p->storage.description,linear,previous);
    if(why)*why=packed?"exposure-copy-packed-no-publication":"destination-address";
    return packed;
}
inline bool sameNativeExposureResolveInputs(const Gta5TileResolveCapture& a,const Gta5TileResolveCapture& b) {
    if(!planNativeExposureResolve(a) || !planNativeExposureResolve(b) || a.stack!=b.stack ||
       a.arguments!=b.arguments || a.state!=b.state || a.destinationHeader!=b.destinationHeader ||
       a.pointed.rectangle!=b.pointed.rectangle || a.pointed.stack92!=b.pointed.stack92 ||
       a.pointed.stack100!=b.pointed.stack100)return false;
    return a.surfaces->surfaces[0].header==b.surfaces->surfaces[0].header;
}
template<class Current,class Publish>
bool publishNativeExposureResolve(const Gta5TileResolveCapture& c,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& previous,NativeEdram64Model model,
        Current&& current,Publish&& publish,const char** why=nullptr) {
    const auto bytes=packNativeExposureResolve(c,owner,previous,model,why);if(!bytes)return false;
    if(!current()){if(why)*why="changed-inputs";return false;}
    if(!publish(*bytes)){if(why)*why="publication";return false;}
    if(why)*why="exposure-copy-published";return true;
}
}
