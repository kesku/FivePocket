#pragma once
#include "gta5_driver_scissor.h"
#include "gta5_logical_tile_clear.h"
#include "native_translated_draw.h"
#include "native_geometry_sampler_plan.h"
#include "native_mip_texture_capture.h"
#include "xenos_depth_stencil_state.h"
#include "xenos_mrt_blend.h"

namespace recomp::gpu {
struct NativeLogicalGeometryState {
    std::array<float,4> viewport{};
    // Native x,y,width,height, NOT the original left,top,right,bottom.
    std::array<unsigned,4> scissor{};
    XenosDepthStencilState depth;
    std::array<unsigned,4> colorWriteMasks{};
    std::uint32_t visibilityControl=0;
};
// Single-sample scene geometry. Extracted from the connected 66-draw replay,
// not an extension of ABI13's earlier two-sample loading-target contract.
// A session must separately own the effective shader, target, input snapshots,
// selected-bin/phase and command order. This function grants no completion.
inline std::optional<NativeLogicalGeometryState> planNativeLogicalGeometryState(
        const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
        const std::array<unsigned,4>& recordedScissor) {
    if(program.abiVersion<14 || program.abiVersion>16 || !program.indexed ||
       !program.geometryDepth20e4 || program.geometrySampleDepth || program.geometryCentroidMask ||
       program.geometryColor8Format!=UINT32_MAX ||
       program.geometryMrtFormats!=std::array<unsigned,4>{0,0,0,0} ||
       !program.geometryMrtOutputs || (program.geometryMrtOutputs&~15u) ||
       program.geometryPixelEffects!=(program.abiVersion>=15) ||
       program.geometryDepthColorWitness!=(program.abiVersion==16) ||
       program.directTriangles || program.affinePositionXY || program.color8SourceClamped ||
       program.cubeRectanglePwl || program.explicitLodRectangle ||
       draw.primitive!=4 || (draw.stride!=24 && draw.stride!=28) || draw.stride!=program.stride ||
       draw.deviceState.size()!=13860 || draw.deviceState[11068]!=0x21 ||
       (draw.deviceState[11071]&0x20) || (draw.deviceState[11072]&0xC0))return std::nullopt;
    const auto r=[&](unsigned reg){return gta5DrawRegister(draw,reg);};
    const auto viewport=nativeTranslatedViewport(draw);
    const auto scissor=planGta5DriverScissor(draw.deviceState,recordedScissor);
    if(!viewport || !scissor || !scissor->binned || scissor->auxiliaryRectangle ||
       recordedScissor[0]>recordedScissor[2] || recordedScissor[1]>recordedScissor[3] ||
       recordedScissor[2]>1280 || recordedScissor[3]>720)return std::nullopt;
    constexpr unsigned required[][2]{
        {0x200E,0},{0x200F,0x20002000},{0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},
        {0x2183,0},{0x2184,0},{0x2202,0xAA000007},{0x2203,0x33},
        {0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},{0x2207,0},{0x2208,4},
        {0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x2113,0x3F800000},{0x2114,0}};
    for(const auto& v:required)if(r(v[0])!=v[1])return std::nullopt;
    constexpr std::array<std::array<unsigned,2>,11> interfaces{{
        {0x40210505,0x30007},{0x40210705,0x30007},{0x40410906,0xF001F},
        {0x40510806,0x1F003F},{0x40410806,0xF001F},{0x40410706,0xF001F},{0x40210405,0x30007},
        {0x10410506,0xF001F},{0x40450706,0xF001F},{0x40410606,0xF001F},{0x40410506,0xF001F}}};
    const auto control=r(0x2180),context=r(0x2182),surface=r(0x2000),mask=r(0x2104);
    if(!control || !context || !surface || !mask || (*surface&0x30000) || (*mask&0xFFFF0000u) ||
       std::find(interfaces.begin(),interfaces.end(),std::array<unsigned,2>{*control,*context})==interfaces.end())return std::nullopt;
    if(*control&(1u<<18)) {
        if(!program.geometryPixelEffects || r(0x2181)!=0x50A || program.geometryParameterRegister!=5)return std::nullopt;
    } else if(r(0x2181)!=8 || program.geometryParameterRegister!=UINT32_MAX)return std::nullopt;
    const auto visibility=r(0x2293);
    if(!visibility || (*visibility&~0x7Fu))return std::nullopt; // Query kill modes not implemented.
    const auto depthControl=r(0x2200),stencilReference=r(0x210D),stencilMask=r(0x210C);
    if(!depthControl || !stencilReference || !stencilMask)return std::nullopt;
    const auto depth=decodeXenosDepthStencilState(*depthControl,*stencilReference,*stencilMask);
    if(!depth)return std::nullopt;
    NativeLogicalGeometryState result;result.viewport=*viewport;result.depth=*depth;
    result.visibilityControl=*visibility;
    result.scissor={recordedScissor[0],recordedScissor[1],
        recordedScissor[2]-recordedScissor[0],recordedScissor[3]-recordedScissor[1]};
    for(unsigned slot=0;slot<4;++slot) {
        const auto blend=r(slot?0x2208+slot:0x2201);
        if(!blend || *blend!=program.geometryMrtBlendControls[slot] || !decodeXenosMrtBlend(*blend) ||
           (!program.geometryPixelEffects && *blend!=0x10001))return std::nullopt;
        result.colorWriteMasks[slot]=(*mask>>(slot*4))&15;
    }
    return result;
}

// Select the EFFECTIVE shader bytes observed after the original selector and
// command-copy patch, not a resource pointer or the unpatched archive shader.
// Program metadata also specializes emitted code (blend/parameter/stride).
// Reject an ambiguous pack instead of making its vector order semantic.
inline const NativeTranslatedDrawProgram* selectNativeLogicalGeometryProgram(
        const std::vector<NativeTranslatedDrawProgram>& programs,
        const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
        const Gta5InlineDraw& draw,const std::array<unsigned,4>& recordedScissor) {
    if(vertex.empty() || pixel.empty())return nullptr;
    const NativeTranslatedDrawProgram* selected=nullptr;
    for(const auto& program:programs) {
        if(program.vertexIdentity!=vertex || program.pixelIdentity!=pixel ||
           program.vertexCode.empty() || program.pixelCode.empty() ||
           !planNativeLogicalGeometryState(draw,program,recordedScissor))continue;
        if(selected)return nullptr;
        selected=&program;
    }
    return selected;
}

struct NativeLogicalGeometryInput {
    NativeLogicalGeometryState state;
    std::vector<std::uint8_t> payload;
    std::vector<XenosTextureDescription> descriptions;
    // Shader descriptor order -> submitted capture index. Capture order is
    // not shader binding order (the live capture enumerates numeric slots).
    std::vector<unsigned> textureSourceIndices;
    bool anisotropy=false;
};
// Validate the whole producer input before creating images or issuing a GPU
// write. Constants are the ordered submitted snapshots, not stale shadow data.
// Shader identity and current guest allocation leases remain caller duties.
inline std::optional<NativeLogicalGeometryInput> prepareNativeLogicalGeometry(
        const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
        const std::vector<std::uint32_t>& indices,const Gta5ClearCapture& surfaces,
        const std::array<unsigned,4>& recordedScissor,
        const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
    const auto state=planNativeLogicalGeometryState(draw,program,recordedScissor);
    const auto layout=describeGta5LogicalTileSurfaces(surfaces);
    const auto slots=nativeTranslatedTextureSlots(program);
    if(!state || !draw.device || surfaces.call.device!=draw.device || !surfaces.state ||
       *surfaces.state!=draw.deviceState || !layout || layout->layout.width!=1280 || layout->layout.height!=720 ||
       !draw.vertexCount || draw.vertexCount>65536 || draw.vertices.size()>256*1024 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       indices.empty() || indices.size()>65535 || indices.size()%3 ||
       std::any_of(indices.begin(),indices.end(),[&](auto i){return i>=draw.vertexCount;}) ||
       slots.size()>4 || textures.size()!=slots.size())return std::nullopt;
    NativeLogicalGeometryInput result;result.state=*state;
    unsigned seen=0;std::size_t bytes=0;
    for(unsigned i=0;i<slots.size();++i) {
        const auto slot=slots[i];
        if(slot>=32 || (seen&(1u<<slot)))return std::nullopt;
        const auto found=std::find_if(textures.begin(),textures.end(),[&](const auto& t){return t.first==slot;});
        if(found==textures.end() || std::count_if(textures.begin(),textures.end(),[&](const auto& t){return t.first==slot;})!=1)return std::nullopt;
        result.textureSourceIndices.push_back(static_cast<unsigned>(found-textures.begin()));
        const auto& texture=found->second;
        seen|=1u<<slot;
        const auto desc=XenosTextureDescription::decode(texture.fetch);
        const auto chain=planXenosUnpackedMipChain(texture.fetch);
        const auto sampler=planNativeGeometrySampler2D(texture.fetch);
        if(!desc || !chain || !sampler || texture.base.size()!=chain->baseBytes || texture.mips.size()!=chain->mipBytes ||
           texture.base.size()>64u*1024*1024-bytes)return std::nullopt;
        bytes+=texture.base.size();
        if(texture.mips.size()>64u*1024*1024-bytes)return std::nullopt;
        bytes+=texture.mips.size();result.descriptions.push_back(*desc);result.anisotropy|=sampler->anisotropy!=0;
    }
    result.payload.resize(8208+draw.vertices.size());
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned c=0;c<4;++c)result.payload[at+c]=v>>(24-8*c);};
    put(0,draw.vertexCount);put(4,static_cast<unsigned>(slots.size()));
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::copy(draw.vertices.begin(),draw.vertices.end(),result.payload.begin()+8208);
    return result;
}
}
