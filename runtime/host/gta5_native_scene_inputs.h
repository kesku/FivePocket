#pragma once
#include "gta5_bound_indexed_draw.h"
#include "gta5_bound_surface_capture.h"
#include "gta5_tile_query_commands.h"
#include "native_logical_geometry_state.h"
#include "native_geometry_query_program.h"

namespace recomp::gpu {
// Draw-time native upload state, not a later PM4 register approximation.
// Views are borrowed only for this preparation call. The live caller holds its
// execution gate, preserves the recording generation, and supplies EVERY draw.
struct Gta5NativeSceneDrawSource {
    unsigned sequence=0;
    bool orderedConstants=false,originalEmissionReturned=false;
    const Gta5BoundIndexedStorage* geometry=nullptr;
    const Gta5BoundSurfaceCapture* surfaces=nullptr;
    const std::vector<std::uint8_t>* vertex=nullptr;
    const std::vector<std::uint8_t>* pixel=nullptr;
    const std::array<unsigned,4>* scissor=nullptr;
    const std::vector<std::pair<unsigned,NativeMipTextureCapture>>* textures=nullptr;
};
struct Gta5NativeSceneInputs {
    struct Draw {
        unsigned sequence=0,programIndex=0;
        Gta5ClearCapture surfaces;
        NativeLogicalGeometryInput input;
        unsigned queryProgramIndex=UINT32_MAX;
    };
    std::vector<Draw> draws;
    Gta5TileQueryCommands queryCommands;
    bool anisotropy=false;
};
// Bridge the real recording's draw sequence, effective shaders, submitted
// constants/textures and original DMA packets BEFORE creating a native target.
// All-or-nothing, with no GPU work or guest mutation. This does not grant a
// resource lease, handle other packet effects, or complete the EndTiling call.
inline std::optional<Gta5NativeSceneInputs> prepareGta5NativeSceneInputs(
        unsigned device,const Gta5ClearCapture& seed,
        std::span<const unsigned> recordedSequences,
        std::span<const Gta5NativeSceneDrawSource> sources,
        const std::vector<NativeTranslatedDrawProgram>& programs,
        const Pm4Snapshot& commands,const std::vector<NativeClippedFanProgram>& queryPrograms={}) {
    if(!device || seed.call.device!=device || !describeGta5LogicalTileSurfaces(seed) ||
       sources.empty() || sources.size()>128 || sources.size()!=recordedSequences.size() || programs.empty())return std::nullopt;
    Gta5NativeSceneInputs result;
    std::vector<Gta5TileQueryDrawBinding> bindings;bindings.reserve(sources.size());
    unsigned previous=0;
    for(unsigned i=0;i<sources.size();++i) {
        const auto& s=sources[i];
        if(!s.sequence || s.sequence<=previous || s.sequence!=recordedSequences[i] ||
           !s.orderedConstants || !s.originalEmissionReturned || !s.geometry || !s.surfaces ||
           !s.vertex || !s.pixel || !s.scissor || !s.textures)return std::nullopt;
        previous=s.sequence;
        const auto& g=*s.geometry;const auto& draw=g.draw;
        if(draw.device!=device || g.request.primitive!=draw.primitive ||
           g.request.indexCount!=g.effectiveIndices.size() || g.rawIndices.size()!=g.effectiveIndices.size())return std::nullopt;
        Gta5ClearCapture surfaces;surfaces.call.device=device;surfaces.state=draw.deviceState;
        for(unsigned slot=0;slot<5;++slot) {
            const auto& b=s.surfaces->surfaces[slot];const auto& expected=seed.surfaces[slot];
            if(b.object!=expected.object || b.tileCacheObject!=expected.tileCache || b.header!=expected.header)return std::nullopt;
            surfaces.surfaces[slot]={b.object,b.tileCacheObject,b.header};
        }
        const auto* program=selectNativeLogicalGeometryProgram(programs,*s.vertex,*s.pixel,draw,*s.scissor);
        if(!program)return std::nullopt;
        auto input=prepareNativeLogicalGeometry(*program,draw,g.effectiveIndices,surfaces,*s.scissor,*s.textures);
        if(!input)return std::nullopt;
        bindings.push_back({g.indexPhysicalAddress,g.request.indexCount,input->state.visibilityControl});
        result.anisotropy|=input->anisotropy;
        unsigned queryIndex=UINT32_MAX;
        if(input->state.visibilityControl&1) {
            const auto* query=selectNativeGeometryQueryProgram(*program,queryPrograms);
            if(!query)return std::nullopt;
            queryIndex=static_cast<unsigned>(query-queryPrograms.data());
        }
        result.draws.push_back({s.sequence,static_cast<unsigned>(program-programs.data()),std::move(surfaces),std::move(*input),queryIndex});
    }
    if(!matchGta5TileQueryCommands(commands,bindings,result.queryCommands))return std::nullopt;
    return result;
}
}
