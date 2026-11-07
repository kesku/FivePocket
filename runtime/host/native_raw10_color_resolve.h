#pragma once
#include "gta5_tile_resolve_capture.h"
#include "native_edram32_bands.h"
#include "xenos_color_copy.h"

namespace recomp::gpu {
// Planner/packer for captured ordinary2 or expanded10 -> texture54 aliases. It
// grants no GPU execution, destination ownership or source initialization.
// The live adapter separately holds the exclusive gate, validates a writable
// destination owner and atomically publishes against its unchanged old bytes.
//
// Original82388A38->823C2738 setup prefix, exact call/headers with scratch
// command bookkeeping: CONTROL00100000, INFO01000382, PITCH02D00500.
// Both captured ordinary2/flag0 and expanded10/flag1 setup prefixes produce
// these same copy controls. Expanded blending precision is not a copy effect.
// Thus convert, colour0/sample0, no clear/bias, storage format7, swapRB and
// 8-in-32 endian. Xenia95a5c3ee IsColorResolveFormatBitwiseEquivalent maps
// RT2/10 -> texture7/54. Expanded blending precision is NOT used by this copy.
struct NativeRaw10ColorResolvePlan {
    unsigned sourceObject=0,destinationObject=0;
    XenosTextureDescription destination;
    static constexpr unsigned Width=1280,Height=720,Pitch=1280,Base=720,Bytes=3768320;
};
inline std::optional<NativeRaw10ColorResolvePlan> planNativeRaw10ColorResolve(
        const Gta5TileResolveCapture& c,const char** why=nullptr) {
    const auto no=[&](const char* stage){if(why)*why=stage;return std::optional<NativeRaw10ColorResolvePlan>{};};
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto zero=[](const auto& b){return b && b->size()==4 && std::all_of(b->begin(),b->end(),[](auto x){return !x;});};
    const auto& a=c.arguments;
    if(!a[0] || a[0]%4 || a[0]>UINT32_MAX-13859 || !c.state || c.state->size()!=13860 || !c.surfaces)
        return no("source-state");
    if(a[1] || a[4] || a[5] || a[6])return no("copy-flags-subresource");
    if(!c.stack || c.stack%4 || c.stack>UINT32_MAX-103 || !zero(c.pointed.stack92) || !zero(c.pointed.stack100))
        return no("stack-arguments");
    // f1 and colour pointer are unused without their clear flags. No default
    // clear value is fabricated, and supplied bytes do not request a clear.
    if(!a[2] || a[2]%4 || a[2]>UINT32_MAX-15 || !c.pointed.rectangle || c.pointed.rectangle->size()!=16)
        return no("rectangle");
    for(unsigned i=0;i<4;++i)if(word(*c.pointed.rectangle,i*4)!=std::array<unsigned,4>{0,0,1280,720}[i])
        return no("rectangle");
    const auto& state=*c.state;const auto& source=c.surfaces->surfaces[0];
    for(unsigned i=0;i<5;++i)if(c.surfaces->surfaces[i].object!=word(state,12816+i*4))return no("binding");
    for(unsigned i=1;i<4;++i)if(c.surfaces->surfaces[i].object)return no("extra-colour");
    if(!source.object || source.object%4 || source.object>UINT32_MAX-39 || !source.header || source.header->size()!=40)
        return no("source-header");
    const auto sourceView=word(state,10372),expanded=word(state,12408);
    // Two coherent original profiles, not a mask that accepts arbitrary view
    // flags. RT3 (7e3 float) and mismatched expanded state require other work.
    const bool raw10View=(sourceView==0x202D0 && expanded==0) ||
        (sourceView==0xA02D0 && expanded==1);
    if(word(*source.header,24)!=0x14000500 || word(*source.header,28)!=0x202D0 ||
       word(*source.header,36)!=0x13FC1678 || word(state,10368)!=0x14000500 ||
       !raw10View || word(state,11068)!=0x2C00)
        return no("source-format-view");
    if(!a[3] || a[3]%4 || a[3]>UINT32_MAX-51 || !c.destinationHeader || c.destinationHeader->size()!=52)
        return no("destination-header");
    const auto d=describeGta5TextureHeader(*c.destinationHeader);if(!d)return no("destination-decode");
    const auto& f=d->fetch;
    // The destination's TEXTURE sampling exponent is not RB_COPY_DEST_INFO's
    // copy exponent. Original82388A38->823C2738 emits INFO01000382 (copy bias0)
    // for both captured fetch3=C14 and6C14 (sampling bias0 and3). In particular
    // its copy exponent comes from flags r21, not texture fetch3 bits13..18.
    // Preserve the descriptor for the later sampler; don't scale copied words.
    // Other descriptor variants remain unproven, not silently masked away.
    const bool capturedSamplingProfile=f[3]==0xC14 || f[3]==0x6C14;
    if(f[0]!=0x8A000002 || (f[1]&0xFFFu)!=0xB6 || f[2]!=0x59E4FF || !capturedSamplingProfile || f[4] || f[5]!=0x200)
        return no("destination-storage-profile");
    if(d->baseAddress>=0x20000000u && (d->baseAddress<0xA0000000u || d->baseAddress>=0xC0000000u))
        return no("destination-namespace");
    const auto physical=d->baseAddress<0x20000000u?d->baseAddress:d->baseAddress-0xA0000000u;
    if(!physical || std::uint64_t{physical}+NativeRaw10ColorResolvePlan::Bytes>0x20000000u)
        return no("destination-extent");
    if(why)*why="raw10-layout-planned-source-unproven";
    return NativeRaw10ColorResolvePlan{source.object,a[3],*d};
}
inline std::optional<std::vector<std::uint8_t>> packNativeRaw10ColorResolve(
        const Gta5TileResolveCapture& capture,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& before,const char** why=nullptr) {
    const auto p=planNativeRaw10ColorResolve(capture,why);if(!p)return std::nullopt;
    const auto no=[&](const char* stage){if(why)*why=stage;return std::optional<std::vector<std::uint8_t>>{};};
    if(before.size()!=p->Bytes)return no("destination-bytes");
    const auto source=owner.readColorWords(p->Base,p->Pitch,p->Width,p->Height);
    if(!source)return no("physical-colour-unknown");
    auto after=before;
    for(unsigned y=0;y<p->Height;++y)for(unsigned x=0;x<p->Width;++x) {
        const auto raw=(*source)[std::size_t{y}*p->Width+x];
        const auto swapped=(raw&0xC00FFC00u)|((raw&0x3FFu)<<20)|((raw>>20)&0x3FFu);
        unsigned to=0;if(!colorCopyOffset(p->Width,736,x,y,to))return no("destination-address");
        for(unsigned c=0;c<4;++c)after[to+c]=swapped>>(24-8*c); // 8-in-32 endian.
    }
    if(why)*why="raw10-packed-no-publication";
    return after;
}
// Compare only captured inputs this no-clear copy consumes. Unused f1/clear
// colour values and the original command allocator are not pixel operands.
inline bool sameNativeRaw10ColorInputs(const Gta5TileResolveCapture& before,
        const Gta5TileResolveCapture& now) {
    if(!planNativeRaw10ColorResolve(before) || !planNativeRaw10ColorResolve(now) ||
       before.stack!=now.stack || before.arguments!=now.arguments || before.state!=now.state ||
       before.destinationHeader!=now.destinationHeader || before.pointed.rectangle!=now.pointed.rectangle ||
       before.pointed.stack92!=now.pointed.stack92 || before.pointed.stack100!=now.pointed.stack100)return false;
    return before.surfaces->surfaces[0].header==now.surfaces->surfaces[0].header;
}
template<class Current,class Publish>
bool publishNativeRaw10ColorResolve(const Gta5TileResolveCapture& capture,const NativeEdram32Bands& owner,
        const std::vector<std::uint8_t>& before,Current&& current,Publish&& publish,const char** why=nullptr) {
    const auto completed=packNativeRaw10ColorResolve(capture,owner,before,why);
    if(!completed)return false;
    if(!current()){if(why)*why="changed-inputs";return false;}
    if(!publish(*completed)){if(why)*why="publication";return false;}
    if(why)*why="raw10-published";
    return true;
}
}
