#pragma once
#include "gta5_local_graphics_allocation.h"
#include "gta5_tile_resolve_capture.h"
#include "gta5_logical_tile_clear.h"
#include "xenos_depth24_texture.h"

namespace recomp::gpu {
// This describes only a bounded ORIGINAL CPU
// emitter. In particular C0 cache state is retained for original823CCEB8 to
// consume: it is NOT permission for the native copy planner to ignore barriers.
struct RecordedDepthResolveAdmission {
    static constexpr unsigned CommandBytes=2048;
    unsigned command=0,tile=0;
    Gta5LocalGraphicsAllocation allocation;
};
namespace detail {
enum class RecordedResolveProfile { Depth, ColorClear };
inline std::optional<RecordedDepthResolveAdmission> describeRecordedResolveProfile(
        const Gta5TileResolveCapture& c,const Gta5ResourceReader& ownedGraphics,
        const Gta5ResourceReader& ownedArguments,RecordedResolveProfile profile) {
    const bool color=profile==RecordedResolveProfile::ColorClear;
    const auto sized=[](const auto& b,unsigned n){return b && b->size()==n;};
    if(!sized(c.state,13860) || !sized(c.allocatorTail,1484) || !sized(c.allocatorFence,4) ||
       !sized(c.resolveGlobals,12) || !sized(c.destinationHeader,52) || !c.surfaces ||
       !ownedGraphics || !ownedArguments)return std::nullopt;
    const auto word=[](const auto& b,unsigned at){return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];};
    const auto& s=*c.state;const auto& a=c.arguments;
    if(a[0]<0x10000 || a[0]>0x20000000-15344 || (a[0]&3) ||
       (color?(a[1]&~3u)!=0x110u:a[1]!=0x14u) || !a[2] || a[3]<0xA0000000 || a[3]>0xBFFFFFCB || !a[4] || a[5] || a[6] ||
       (a[2]&3) || (a[3]&3) || (a[4]&3) || c.stack<4096 || c.stack>UINT32_MAX-103 ||
       !sized(c.pointed.rectangle,16) || !sized(c.pointed.point,8) ||
       !sized(c.pointed.stack92,4) || !sized(c.pointed.stack100,4) ||
       word(*c.pointed.stack92,0) || word(*c.pointed.stack100,0))return std::nullopt;
    // Restrict control branches, not the individual register values/dirty
    // masks. Both actual-first and original-produced second-resolve states fit.
    if(s[11068]!=0x20 || s[11069] || (s[11070]!=0x24 && s[11070]!=0x2C) ||
       (s[11071]!=0 && s[11071]!=3) || (s[11072]!=0 && s[11072]!=0xC0) ||
       word(s,40) || word(s,44) || !word(s,11036) || word(s,13088) || word(s,13092)!=1)
        return std::nullopt;
    if(color) {
        // Original823C2C4C calls823482A8/82347E30 for the selected color slot's
        // format0. No depth clear, alternate source, NaN or unowned vector.
        if(!a[7] || a[7]%16 || a[7]>UINT32_MAX-15 || !sized(c.pointed.color,16))return std::nullopt;
        std::array<float,4> values{};
        for(unsigned i=0;i<4;++i){const auto bits=word(*c.pointed.color,i*4);std::memcpy(&values[i],&bits,4);}
        if(!packGta5Color8Clear(values,0))return std::nullopt;
    } else if(a[7])return std::nullopt;
    Gta5ClearCapture surfaces;surfaces.call.device=a[0];surfaces.state=c.state;
    for(unsigned i=0;i<5;++i) {
        const auto& b=c.surfaces->surfaces[i];surfaces.surfaces[i]={b.object,b.tileCacheObject,b.header};
    }
    const auto source=describeGta5LogicalTileSurfaces(surfaces);
    const auto destination=describeGta5TextureHeader(*c.destinationHeader);
    if(!source || !destination || source->layout.tiles.size()!=2)return std::nullopt;
    unsigned width=0,height=0;
    if(color) {
        const auto storage=planNativeOffscreenResolveDestination(*destination,0);
        const auto slot=a[1]&3u;
        if(!(source->boundAspects&(1u<<slot)) || source->formats[slot]!=0 || destination->format!=6 || !storage ||
           destination->sign!=std::array<unsigned,4>{0,0,0,0})return std::nullopt;
        width=storage->storage.description.width;height=storage->storage.description.height;
    } else {
        const auto storage=planXenosDepth24FloatStorage(*destination);
        if(source->formats[4]!=1 || !storage)return std::nullopt;
        width=storage->description.width;height=storage->description.height;
    }
    if(source->layout.width!=width || source->layout.height!=height)return std::nullopt;
    // No scissor replacement or alternate subresource path. Current full-size
    // scissor contains both rectangles, so823ABBE0's auxiliary path is absent.
    if(word(s,10436)!=0 || source->layout.width>=0x4000 || source->layout.height>=0x4000 ||
       word(s,10440)!=(source->layout.width|(source->layout.height<<16)))return std::nullopt;
    std::optional<unsigned> tile;
    for(unsigned i=0;i<2;++i) {
        const auto& t=source->layout.tiles[i];bool same=word(s,13084)==t.selectLow;
        for(unsigned j=0;j<4;++j)same=same && word(*c.pointed.rectangle,j*4)==t.logical[j] && !(t.logical[j]&7);
        if(same && word(*c.pointed.point,0)==t.logical[0] && word(*c.pointed.point,4)==t.logical[1])tile=i;
    }
    if(!tile)return std::nullopt;
    auto state=s;state.insert(state.end(),c.allocatorTail->begin(),c.allocatorTail->end());
    const auto allocation=describeGta5LocalGraphicsAllocation(state,6,4,word(*c.allocatorFence,0),ownedGraphics);
    if(!allocation)return std::nullopt;
    const auto command=word(s,48),flush=word(s,56),limit=word(s,52);
    constexpr auto budget=RecordedDepthResolveAdmission::CommandBytes;
    // Six dirty-mask groups contain at most113 source register bits
    // (12+5+16+21+21+38); splitting every bit needs<=226 dwords. Two-bin
    // address records and fixed resolve packets have the remaining286 dwords.
    // Strict headroom avoids BOTH823BA300 fallback and8235E218 flush paths.
    if(command<0xA0000000 || command>0xBFFFFFFF-budget-4 || command%4 ||
       command+budget>flush || flush>=limit || limit>=0xC0000000 ||
       (command+4<allocation->end && allocation->address<command+4+budget) ||
       (a[3]<command+4+budget && command+4<a[3]+52) ||
       (a[3]<allocation->end && allocation->address<a[3]+52))return std::nullopt;
    using Range=std::pair<std::uint64_t,std::uint64_t>;
    const auto overlap=[](Range x,Range y){return x.first<y.first+y.second && y.first<x.first+x.second;};
    const std::array<Range,5> writes{{{a[0],15344},{c.stack-4096,4200},
        {command+4,budget},{allocation->address,allocation->bytes},{a[3],52}}};
    for(unsigned i=0;i<writes.size();++i)for(unsigned j=0;j<i;++j)
        if(overlap(writes[i],writes[j]))return std::nullopt;
    for(const auto& b:surfaces.surfaces)if(b.object)
        for(const auto& w:writes)if(overlap(w,{b.object,40}))return std::nullopt;
    // Preserve externally read inputs. Stack arguments can alias only the
    // owned stack workspace; they cannot alias command/quad/header outputs.
    for(unsigned i:{0u,2u,3u,4u})for(const auto& input:std::array<Range,5>{{
            {a[2],16},{a[4],8},{word(s,11024)+4ULL,4},{Gta5TileResolveCapture::ResolveGlobalsAddress,12},
            {a[7],color?16u:0u}}})
        if(overlap(writes[i],input))return std::nullopt;
    const auto commands=ownedGraphics(command+4,budget),stack=ownedArguments(c.stack-4096,4200);
    if(!commands || commands->size()!=budget || !stack || stack->size()!=4200)return std::nullopt;
    return RecordedDepthResolveAdmission{command,*tile,*allocation};
}
} // namespace detail
inline std::optional<RecordedDepthResolveAdmission> describeRecordedDepthResolve(
        const Gta5TileResolveCapture& c,const Gta5ResourceReader& graphics,const Gta5ResourceReader& arguments) {
    return detail::describeRecordedResolveProfile(c,graphics,arguments,detail::RecordedResolveProfile::Depth);
}
inline std::optional<RecordedDepthResolveAdmission> describeRecordedColor1ClearResolve(
        const Gta5TileResolveCapture& c,const Gta5ResourceReader& graphics,const Gta5ResourceReader& arguments) {
    if(c.arguments[1]!=0x111)return std::nullopt;
    return detail::describeRecordedResolveProfile(c,graphics,arguments,detail::RecordedResolveProfile::ColorClear);
}
// Original829322B8 selects slot1; original82312788 selects slots0/2/3.
// Same strict copy+clear, format and owned CPU-write contract for each slot.
inline std::optional<RecordedDepthResolveAdmission> describeRecordedColorClearResolve(
        const Gta5TileResolveCapture& c,const Gta5ResourceReader& graphics,const Gta5ResourceReader& arguments) {
    return detail::describeRecordedResolveProfile(c,graphics,arguments,detail::RecordedResolveProfile::ColorClear);
}
// Captured CPU output is not an image, a submission or a completion receipt.
struct RecordedDepthResolveOutput {
    std::vector<std::uint8_t> commands,quad,state,destinationHeader;
};
inline std::optional<RecordedDepthResolveOutput> captureRecordedDepthResolveOutput(
        const Gta5TileResolveCapture& before,const RecordedDepthResolveAdmission& plan,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics) {
    if(!heap || !graphics)return std::nullopt;
    const auto state=heap(before.arguments[0],15344);
    if(!state || state->size()!=15344)return std::nullopt;
    const auto word=[&](unsigned at){return (unsigned((*state)[at])<<24)|(unsigned((*state)[at+1])<<16)|
        (unsigned((*state)[at+2])<<8)|(*state)[at+3];};
    const auto end=word(48);
    if(end<=plan.command || end-plan.command>plan.CommandBytes || (end&3) ||
       word(15320)!=plan.allocation.end || word(15324)!=plan.allocation.generation ||
       word(15336)!=plan.allocation.counterAfter)return std::nullopt;
    const auto commands=graphics(plan.command+4,end-plan.command),quad=graphics(plan.allocation.address,plan.allocation.bytes),
        header=graphics(before.arguments[3],52);
    if(!commands || commands->size()!=end-plan.command || !quad || quad->size()!=plan.allocation.bytes ||
       !header || header->size()!=52)return std::nullopt;
    return RecordedDepthResolveOutput{*commands,*quad,*state,*header};
}
}
