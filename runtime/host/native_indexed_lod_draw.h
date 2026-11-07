#pragma once
#include "native_indexed_lod_plan.h"
#include "native_ordered_fragment_raster.h"
#include "native_geometry_sampler.h"
#include "native_vertex_clip_executor.h"
#include "native_ordered_triangle_batches.h"
#include "native_ordered_fragment_windows.h"

namespace recomp::gpu {
// Separate ABI33 descriptor/stream program, never passed to legacy ABI12/32
// resources (their binding5 has a different meaning).
struct NativeIndexedLodProgram {
    NativeClippedFanProgram positions;
    NativeOrderedFragmentRasterProgram fragments;
    unsigned abi=33;
    NativeIndexedLodProfile profile=NativeIndexedLodProfile::Indexed600;
    NativeIndexedLodRaster raster=NativeIndexedLodRaster::LegacyBatches;
};
inline bool executeNativeIndexedLodDraw(ProbeDevice& gpu,const NativeIndexedLodProgram& program,
        const Gta5InlineDraw& draw,const std::vector<unsigned>& indices,
        const Gta5BoundSurfaceCapture& bound,const NativeIndexedLodTextures& textures,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    const bool next=program.profile==NativeIndexedLodProfile::Indexed601;
    if(!validNativeIndexedLodProfile(program.profile) || program.abi!=33 || program.positions.stride!=28 ||
       vertex.size()!=(next?504u:516u) || pixel.size()!=(next?2928u:2952u) ||
       vertex!=program.positions.vertexIdentity || pixel!=program.positions.pixelIdentity)
        return no("indexed-lod-program-identity");
    const auto plan=planNativeIndexedLod(draw,indices,bound,textures,queriesIdle,why,program.profile,program.raster);if(!plan)return false;
    // A NaN/zeroW vertex could disappear before producing a fragment receipt.
    // Prove CURRENT original vertex outputs, never infer validity from an
    // empty fragment list or a historical capture with different constants.
    auto executor=NativeVertexClipExecutor::create(gpu,program.positions);
    if(!executor)return no("indexed-lod-position-executor");
    const auto positions=executor->indexedPositions(draw);
    if(!positions || positions->size()!=draw.vertexCount)return no("indexed-lod-position-receipt");
    if(!validateNativeIndexedLodPositions(*positions,plan->viewport,program.raster,why))return false;
    std::array<std::unique_ptr<NativeTextureImage>,5> images;
    std::array<std::unique_ptr<NativeGeometrySampler>,5> samplers;
    std::array<VkDescriptorImageInfo,5> descriptors{};
    for(unsigned i=0;i<5;++i) {
        images[i]=NativeTextureImage::createUnpackedMips(gpu,plan->textures[i],textures[i].base,textures[i].mips);
        if(!images[i])return no("indexed-lod-image-import");
        samplers[i]=NativeGeometrySampler::create(gpu,*images[i]);if(!samplers[i])return no("indexed-lod-sampler");
        descriptors[i]={samplers[i]->sampler(),images[i]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    std::vector<std::uint8_t> input(8208+draw.vertices.size());
    const auto put=[&](unsigned at,unsigned value){for(unsigned c=0;c<4;++c)input[at+c]=value>>(24-8*c);};
    put(0,draw.vertexCount);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    const char* rasterWhy=nullptr;
    const bool windows=program.raster==NativeIndexedLodRaster::ClippedWindows;
    static_assert(NativeIndexedLodWindowedMaxTriangles==OrderedMeshMaxTriangles);
    const bool complete=executeNativeOrderedFragment7e3Physical(mutation,[&] {
        if(windows) {
            auto raster=NativeOrderedFragmentRasterExecutor::create(gpu,program.fragments,input,
                std::span<const unsigned>(indices).first(3),plan->viewport,descriptors,
                OrderedFragment7e3MaxCapacity,&rasterWhy);
            if(!raster)return std::optional<NativeOrderedFragment7e3Batch>{};
            const char* batchWhy=nullptr;bool rasterFailed=false;
            auto result=executeNativeOrderedTriangleWindows(indices,[&](auto triangle,unsigned row,unsigned rows) {
                auto batch=raster->execute(triangle,{{0,int(row)},{1280,rows}},&rasterWhy);
                rasterFailed=!batch;return batch;
            },&batchWhy);
            if(!result && !rasterFailed)rasterWhy=batchWhy;
            return result;
        }
        if(next) {
            const char* batchWhy=nullptr;bool rasterFailed=false;
            auto result=executeNativeOrderedTriangleBatches(indices,[&](std::span<const unsigned> triangle) {
                auto batch=executeNativeOrderedFragmentRaster(gpu,program.fragments,input,triangle,plan->viewport,
                    descriptors,OrderedFragment7e3MaxCapacity,&rasterWhy);
                rasterFailed=!batch;return batch;
            },&batchWhy);
            // Keep the failed GPU stage, not a generic aggregation refusal.
            if(!result && !rasterFailed)rasterWhy=batchWhy;
            return result;
        }
        return executeNativeOrderedFragmentRaster(gpu,program.fragments,input,indices,plan->viewport,
            descriptors,OrderedFragment7e3MaxCapacity,&rasterWhy);
    },why,windows?Color7e3Rounding::XeniaShiftRound:Color7e3Rounding::RequireNearestEvenAgreement);
    if(!complete && rasterWhy && why)*why=rasterWhy;
    return complete;
}
}
