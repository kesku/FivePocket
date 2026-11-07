#pragma once
#include "gta5_recorded_depth_resolve.h"
#include "gta5_logical_tile_depth_resolve.h"
#include "gta5_native_scene_inputs.h"

namespace recomp::gpu {
// Borrowed ORIGINAL emitter inputs/outputs. Neither a returned CPU emitter nor
// its command bytes prove that the native image has been rendered/resolved.
struct Gta5NativeSceneResolveSource {
    const Gta5TileResolveCapture* before=nullptr;
    const RecordedDepthResolveOutput* output=nullptr;
    unsigned commandAddress=0,tile=0;
    bool originalReturned=false;
};
struct Gta5NativeSceneResolves {
    struct Resolve {
        Gta5ClearCapture surfaces;
        NativeOffscreenResolveRequest request;
        NativeOffscreenResolveArguments arguments;
        XenosTextureDescription destination;
        Gta5LogicalTileResolve geometry;
        unsigned object=0,tile=0,beginBytes=0,endBytes=0,drawBytes=0,destinationIndex=0;
        bool depth=false;
    };
    struct Destination {
        unsigned object=0,address=0,bytes=0;
        std::vector<std::uint8_t> finalHeader;
    };
    std::vector<Resolve> resolves;
    std::vector<Destination> destinations;
};
// Bind each API resolve to its exact ORIGINAL command range and single copy
// draw. Planning FencedNativeImages requires the eventual target's fenced copy
// implementation; it does not discharge other PM4 effects or guest waits.
inline std::optional<Gta5NativeSceneResolves> prepareGta5NativeSceneResolves(
        const Gta5ClearCapture& seed,unsigned pendingAddress,std::span<const std::uint8_t> pending,
        std::span<const Gta5NativeSceneResolveSource> sources,
        std::span<const Gta5NativeSceneDrawSource> draws) {
    const auto layout=describeGta5LogicalTileSurfaces(seed);
    if(!layout || sources.empty() || sources.size()>10 || !pendingAddress || pendingAddress%4 ||
       pending.empty() || pending.size()>65536 || std::uint64_t{pendingAddress}+pending.size()>0xC0000000 ||
       pendingAddress<0xA0000000 || draws.empty())return std::nullopt;
    Pm4Snapshot stream;Pm4Error error;
    if(!decodePm4(pending.data(),pending.size(),{},stream,error))return std::nullopt;
    const auto word=[](const auto& b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    };
    const auto boundary=[&](unsigned at) {
        return at==pending.size() || std::any_of(stream.packets().begin(),stream.packets().end(),
            [&](const auto& p){return p.offsetWords*4==at;});
    };
    Gta5NativeSceneResolves result;unsigned previousEnd=0;std::uint64_t totalBytes=0;
    for(const auto& source:sources) {
        if(!source.before || !source.output || !source.originalReturned)return std::nullopt;
        const auto& c=*source.before;const auto& out=*source.output;const auto& a=c.arguments;
        if(a[0]!=seed.call.device || !c.state || c.state->size()!=13860 || !c.surfaces ||
           !c.destinationHeader || c.destinationHeader->size()!=52 || out.destinationHeader.size()!=52 ||
           out.state.size()!=15344 || out.commands.empty() || out.commands.size()>RecordedDepthResolveAdmission::CommandBytes ||
           source.commandAddress!=std::uint64_t{word(*c.state,48)}+4 || source.commandAddress<pendingAddress ||
           std::uint64_t{source.commandAddress}+out.commands.size()>std::uint64_t{pendingAddress}+pending.size() ||
           std::uint64_t{word(out.state,48)}+4!=std::uint64_t{source.commandAddress}+out.commands.size())return std::nullopt;
        Gta5NativeSceneResolves::Resolve r;
        r.beginBytes=source.commandAddress-pendingAddress;r.endBytes=r.beginBytes+out.commands.size();
        if(r.beginBytes<previousEnd || !boundary(r.beginBytes) || !boundary(r.endBytes) ||
           !std::equal(out.commands.begin(),out.commands.end(),pending.begin()+r.beginBytes))return std::nullopt;
        previousEnd=r.endBytes;
        unsigned copies=0;
        for(const auto& p:stream.packets())if(p.offsetWords*4>=r.beginBytes && p.offsetWords*4<r.endBytes &&
                p.kind==Pm4Kind::Command && (p.opcode==0x22 || p.opcode==0x36)) {
            // Original823C2738's predicated rectangle-list copy, not indexed
            // game geometry or an arbitrary draw silently treated as a copy.
            if(stream.words()[p.offsetWords]!=0xC0003601 || p.payloadWords!=1 ||
               stream.words()[p.offsetWords+1]!=0x30088 || ++copies!=1)return std::nullopt;
            r.drawBytes=p.offsetWords*4;
        }
        if(copies!=1 || r.drawBytes==r.beginBytes)return std::nullopt;
        r.surfaces.call.device=a[0];r.surfaces.state=c.state;
        for(unsigned slot=0;slot<5;++slot) {
            const auto& b=c.surfaces->surfaces[slot];const auto& expected=seed.surfaces[slot];
            if(b.object!=expected.object || b.tileCacheObject!=expected.tileCache || b.header!=expected.header)return std::nullopt;
            r.surfaces.surfaces[slot]={b.object,b.tileCacheObject,b.header};
        }
        r.request={c.stack,a[1],a[2],a[4],a[5],a[6],a[7]};r.arguments=c.pointed;
        r.object=a[3];r.tile=source.tile;r.depth=a[1]==0x14;
        const auto destination=describeGta5TextureHeader(*c.destinationHeader);
        const auto after=describeGta5TextureHeader(out.destinationHeader);
        if(!destination || !after || destination->fetch!=after->fetch)return std::nullopt;
        r.destination=*destination;
        if(r.depth) {
            const auto p=planGta5LogicalTileDepthResolve(r.surfaces,r.request,r.arguments,r.destination,Gta5ResolveSynchronization::FencedNativeImages);
            if(!p)return std::nullopt;r.geometry=p->geometry;
        } else {
            const auto p=planGta5LogicalTileResolve(r.surfaces,r.request,r.arguments,r.destination,Gta5ResolveSynchronization::FencedNativeImages);
            if(!p)return std::nullopt;r.geometry=*p;
        }
        if(r.geometry.regions.size()!=1 || r.geometry.regions[0].tile!=r.tile || r.tile>=layout->layout.tiles.size())return std::nullopt;
        const auto address=r.destination.baseAddress;const auto bytes=r.geometry.storage.sourceBytes;
        if(r.object<0xA0000000 || std::uint64_t{r.object}+52>0xC0000000 || address<0xA0000000 ||
           !bytes || bytes>64u*1024*1024 || std::uint64_t{address}+bytes>0xC0000000)return std::nullopt;
        auto found=std::find_if(result.destinations.begin(),result.destinations.end(),[&](const auto& d){return d.object==r.object;});
        if(found==result.destinations.end()) {
            totalBytes+=bytes;if(totalBytes>64u*1024*1024)return std::nullopt;
            r.destinationIndex=result.destinations.size();
            result.destinations.push_back({r.object,address,static_cast<unsigned>(bytes),out.destinationHeader});
        } else {
            if(found->address!=address || found->bytes!=bytes ||
               describeGta5TextureHeader(found->finalHeader)->fetch!=destination->fetch)return std::nullopt;
            r.destinationIndex=found-result.destinations.begin();found->finalHeader=out.destinationHeader;
        }
        result.resolves.push_back(std::move(r));
    }
    const auto overlaps=[](std::uint64_t a,std::uint64_t n,std::uint64_t b,std::uint64_t m){return n && m && a<b+m && b<a+n;};
    for(unsigned i=0;i<result.destinations.size();++i) {
        const auto& d=result.destinations[i];
        if(overlaps(d.address,d.bytes,pendingAddress,pending.size()))return std::nullopt;
        for(unsigned j=0;j<result.destinations.size();++j) {
            const auto& other=result.destinations[j];
            if(overlaps(d.address,d.bytes,other.object,52) ||
               (i!=j && overlaps(d.address,d.bytes,other.address,other.bytes)))return std::nullopt;
        }
        for(const auto& draw:draws) {
            if(!draw.textures)return std::nullopt;
            for(const auto& [slot,t]:*draw.textures) {
                (void)slot;const auto desc=XenosTextureDescription::decode(t.fetch);
                if(!desc || desc->baseAddress>=0x20000000 || desc->mipAddress>=0x20000000 ||
                   overlaps(d.address-0xA0000000,d.bytes,desc->baseAddress,t.base.size()) ||
                   overlaps(d.address-0xA0000000,d.bytes,desc->mipAddress,t.mips.size()))return std::nullopt;
            }
        }
    }
    return result;
}
// Snapshot CURRENT owned destination storage under the caller's exclusive gate.
// Missing bytes are not zeros/test sentinels. Keep original bytes for the later
// compare-before-publish transaction. This is not a lease across a released gate.
struct Gta5NativeSceneResolveStorage {
    std::vector<std::vector<std::uint8_t>> original,current;
};
inline std::optional<Gta5NativeSceneResolveStorage> captureGta5NativeSceneResolveStorage(
        const Gta5NativeSceneResolves& plan,const Gta5ResourceReader& ownedGraphics) {
    if(!ownedGraphics || plan.destinations.empty())return std::nullopt;
    Gta5NativeSceneResolveStorage result;
    for(const auto& d:plan.destinations) {
        const auto header=ownedGraphics(d.object,52);
        if(!header || *header!=d.finalHeader)return std::nullopt;
        const auto bytes=ownedGraphics(d.address,d.bytes);
        if(!bytes || bytes->size()!=d.bytes)return std::nullopt;
        result.original.push_back(*bytes);result.current.push_back(*bytes);
    }
    return result;
}
// The real target's methods fence prior native work and preserve all untouched
// destination bytes. Only private staging changes here. Original command side
// effects, ordering between calls and all guest publication remain caller duties.
template<class NativeTarget>
bool executeGta5NativeSceneResolve(const Gta5NativeSceneResolves& plan,unsigned index,unsigned tile,
        NativeTarget& target,Gta5NativeSceneResolveStorage& storage) {
    if(index>=plan.resolves.size() || storage.original.size()!=plan.destinations.size() ||
       storage.current.size()!=plan.destinations.size())return false;
    const auto& r=plan.resolves[index];
    if(tile!=r.tile || r.destinationIndex>=storage.current.size())return false;
    auto& previous=storage.current[r.destinationIndex];
    if(previous.size()!=plan.destinations[r.destinationIndex].bytes)return false;
    auto output=r.depth?target.prepareCapturedDepthResolve(r.surfaces,r.request,r.arguments,r.destination,previous):
        target.prepareCapturedColorResolve(r.surfaces,r.request,r.arguments,r.destination,previous);
    if(!output || output->size()!=previous.size())return false;
    previous=std::move(*output);return true;
}
}
