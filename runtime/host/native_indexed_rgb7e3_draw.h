#pragma once
#include "native_clipped_indexed_plan.h"
#include "native_vertex_clip_executor.h"
#include "native_linear_sample_target.h"
#include "native_indexed_rgb7e3_physical.h"

namespace recomp::gpu {
struct NativeIndexedRgb7e3Program {
    NativeTranslatedDrawProgram draw;
    NativeClippedFanProgram positions;
};
// Component consumer only until explicitly wired and qualified at the live
// boundary. Exact pair/state is shared with the earlier exclusion path, but
// this path executes pixels, original indices and variable depth. It never
// turns a failed exclusion test into an invented draw completion.
inline bool executeNativeIndexedRgb7e3Draw(ProbeDevice& device,const NativeIndexedRgb7e3Program& program,
        const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,
        const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& actualVertex,
        const std::vector<std::uint8_t>& actualPixel,bool queriesIdle,NativeEdram32Mutation& mutation,
        const char** why=nullptr) {
    const auto no=[&](const char* text){if(why)*why=text;return false;};
    if(!validateNativeIndexedRgb7e3Program(program.draw) ||
       program.positions.vertexIdentity!=program.draw.vertexIdentity ||
       program.positions.pixelIdentity!=program.draw.pixelIdentity)return no("indexed-rgb7e3-program");
    // This planner validates ownership/identity/state only; no view-exclusion
    // decision is used here. The actual raster path still has its own proof.
    if(!planNativeClippedIndexed(program.positions,draw,indices,bound,actualVertex,actualPixel,queriesIdle,why))return false;
    auto executor=NativeVertexClipExecutor::create(device,program.positions);
    if(!executor)return no("indexed-rgb7e3-position-executor");
    const auto positions=executor->indexedPositions(draw);
    if(!positions || positions->size()!=draw.vertexCount)return no("indexed-rgb7e3-position-readback");
    // Preserve the already-qualified no-pixel path, using THIS submission's
    // GPU positions. Crucially, do not let a failed exclusion destroy the
    // physical mutation before the visible renderer can consume it.
    if(nativeIndexedViewExclusion(draw,*positions,indices,{0,0,1280,720})) {
        mutation.commit();if(why)*why="current-indexed-view-exclusion";return true;
    }
    // Keep the first visible profile in the measured positive-W / DX depth
    // domain. W crossings and depth clipping require separate connected tests.
    for(const auto& p:*positions) {
        for(auto v:p)if(!std::isfinite(v))return no("indexed-rgb7e3-position-finite");
        if(p[3]<=0 || p[2]<0 || p[2]>p[3])return no("indexed-rgb7e3-position-domain");
    }
    NativeTranslatedPipelineOptions options;options.depthStencil=*decodeXenosDepthStencilState(0x708767,0xFFFF00,0);
    options.minDepth=0x1p-22f;options.maxDepth=0x1p-24f;
    options.colorWriteMasks={15,0,0,0};options.cullMode=VK_CULL_MODE_BACK_BIT;
    options.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned c=0;c<4;++c)input[at+c]=value>>(24-8*c);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    const char* failure=nullptr;
    const bool complete=executeNativeIndexedRgb7e3Physical(mutation,[&](const auto& rgba,const auto& depth)
            ->std::optional<NativeIndexedRgb7e3Readback> {
        auto target=NativeLinearSampleTarget::create(device,1280,720,1);
        if(!target){failure="indexed-rgb7e3-target-create";return std::nullopt;}
        if(!target->loadSingleSampleContents(rgba,depth)){failure="indexed-rgb7e3-owner-import";return std::nullopt;}
        const auto colors=target->draw(program.draw,input,draw.vertexCount,*nativeTranslatedViewport(draw),{},options,
            NativeTranslatedBlend::Replace,{0,0,0,0},0,0,true,indices,true,std::array<unsigned,4>{0,0,1280,720});
        if(!colors){failure=target->lastDrawFailure();return std::nullopt;}
        const auto depths=target->readSingleSampleDepthWords();
        if(!depths){failure="indexed-rgb7e3-depth-readback";return std::nullopt;}
        return NativeIndexedRgb7e3Readback{*colors,*depths};
    },why);
    if(!complete && failure && why)*why=failure;
    return complete;
}
}
