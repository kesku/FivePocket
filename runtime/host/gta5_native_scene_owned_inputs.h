#pragma once
#include "gta5_native_scene_resolves.h"
#include "gta5_tile_recording_diagnostic.h"

namespace recomp::gpu {
struct Gta5NativeSceneInputCheck {
    enum class Issue {None,Missing,Vertex,Index,Surface,Texture,ResourceHeader,ResourceBytes};
    Issue issue=Issue::None;unsigned index=0,address=0;
    explicit operator bool()const noexcept{return issue==Issue::None;}
};
// Read-only current-memory check shared with the live dispatcher. Readers must
// enforce actual allocation ownership while the caller excludes guest writers.
// Submitted constants/shader identities are owned draw-time snapshots, not a
// later device-register state. No shader patch or completion is implied here.
inline Gta5NativeSceneInputCheck checkGta5NativeSceneOwnedInputs(
        std::span<const Gta5NativeSceneDrawSource> sources,const Gta5NativeSceneResolves& plan,
        const Gta5NativeSceneResolveStorage& storage,const Gta5ResourceReader& heap,const Gta5ResourceReader& gfx) {
    using Issue=Gta5NativeSceneInputCheck::Issue;
    if(!heap || !gfx || sources.empty() || storage.original.size()!=plan.destinations.size())return {Issue::Missing};
    for(unsigned i=0;i<sources.size();++i) {
        const auto& s=sources[i];
        if(!s.geometry || !s.surfaces || !s.textures || !s.originalEmissionReturned)return {Issue::Missing,i};
        const auto& g=*s.geometry;
        if(g.draw.vertices.empty() || gfx(g.draw.vertexAddress,g.draw.vertices.size())!=std::optional{g.draw.vertices})
            return {Issue::Vertex,i,g.draw.vertexAddress};
        const auto indices=gfx(g.indexPhysicalAddress,g.rawIndices.size()*2);
        if(!indices || g.rawIndices.empty() || indices->size()!=g.rawIndices.size()*2)return {Issue::Index,i,g.indexPhysicalAddress};
        for(unsigned j=0;j<g.rawIndices.size();++j)
            if((unsigned((*indices)[j*2])<<8|(*indices)[j*2+1])!=g.rawIndices[j])return {Issue::Index,i,g.indexPhysicalAddress+2*j};
        for(const auto& b:s.surfaces->surfaces)if(b.object) {
            if(!b.header || (b.object>=0xA0000000?gfx(b.object,b.header->size()):heap(b.object,b.header->size()))!=b.header)
                return {Issue::Surface,i,b.object};
        }
        for(const auto& [slot,t]:*s.textures) {
            const auto d=XenosTextureDescription::decode(t.fetch);
            if(!d)return {Issue::Texture,i,slot};
            if(!t.base.empty() && gfx(d->baseAddress,t.base.size())!=std::optional{t.base})return {Issue::Texture,i,d->baseAddress};
            if(!t.mips.empty() && gfx(d->mipAddress,t.mips.size())!=std::optional{t.mips})return {Issue::Texture,i,d->mipAddress};
        }
    }
    for(unsigned i=0;i<plan.destinations.size();++i) {
        const auto& d=plan.destinations[i];
        if(gfx(d.object,d.finalHeader.size())!=std::optional{d.finalHeader})return {Issue::ResourceHeader,i,d.object};
        if(gfx(d.address,d.bytes)!=std::optional{storage.original[i]})return {Issue::ResourceBytes,i,d.address};
    }
    return {};
}
inline bool validGta5NativeSceneClearPrefix(std::span<const Gta5ClearCapture> clears,
        std::span<const Gta5TileRecordingDiagnostic::Event> events) {
    if(clears.empty())return false;
    bool raster=false;unsigned count=0;
    for(const auto& e:events) {
        using Kind=Gta5TileRecordingDiagnostic::Kind;
        if(e.kind==Kind::Indexed || e.kind==Kind::Resolve)raster=true;
        if(e.kind==Kind::Clear && (raster || e.index!=count++))return false;
    }
    return count==clears.size();
}
}
