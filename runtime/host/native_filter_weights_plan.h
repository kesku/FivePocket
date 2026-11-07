#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_raw10_texture_capture.h"
#include "native_translated_draw.h"
#include "native_offscreen_draw_plan.h"
#include "xenos_raw10_texture.h"

namespace recomp::gpu {
// Separate, opt-in capture/state contract for the observed half-resolution
// packed10 filter-weights pass. Nothing here executes a shader or completes
// a draw. Original VS arithmetic/rectangle coverage, PS translation, fenced
// output and physical-owner transaction must all be established separately.
inline bool nativeFilterWeightsVertexLiteral(const std::array<std::uint32_t,6>& fetch) {
    // All channels select literal zero. No read of physical address0; this is
    // NOT the generic native constant-texture descriptor or a null fallback.
    return fetch==std::array<std::uint32_t,6>{0x02000002,2,0,0x01001249,0,0x200};
}
inline std::optional<Gta5InlineDraw> captureNativeFilterWeightsDraw(
        unsigned device,unsigned primitive,unsigned vertexCount,unsigned stride,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data,
        const Gta5TextureFetchReader& submittedFetch) {
    if(primitive!=8 || vertexCount!=3 || stride!=24 || !submittedFetch)return std::nullopt;
    auto draw=captureGta5InlineDraw(device,primitive,vertexCount,stride,{},heap,data);
    if(!draw)return std::nullopt;
    const auto vertex=submittedFetch(17),pixel=submittedFetch(5);
    if(!vertex || !pixel || !nativeFilterWeightsVertexLiteral(*vertex))return std::nullopt;
    auto texture=captureNativeRaw10Base(5,*pixel,data);
    if(!texture)return std::nullopt;
    draw->textures.push_back(std::move(*texture));
    draw->textures.push_back({17,*vertex,true,{}});
    return draw;
}
// Compiler certificate requires position=fetch0.xyz/1 and UV=fetch1.xy.
// These exact current vertices then prove full rectangle coverage. Neither an
// old position capture nor the target clear colour can replace this check.
inline bool nativeFilterWeightsFullRectangle(const Gta5InlineDraw& draw) {
    if(draw.primitive!=8 || draw.vertexCount!=3 || draw.stride!=24 || draw.vertices.size()!=72)return false;
    unsigned corners=0;
    for(unsigned i=0;i<3;++i) {
        std::array<float,6> v{};
        for(unsigned c:{0u,1u,2u,4u,5u}) {
            const auto at=i*24+c*4;const auto& b=draw.vertices;
            const unsigned bits=(unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];
            std::memcpy(&v[c],&bits,4);
        }
        if((v[0]!=-1 && v[0]!=1) || (v[1]!=-1 && v[1]!=1) || v[2]!=0 ||
           v[4]!=(v[0]+1)*.5f || v[5]!=(1-v[1])*.5f)return false;
        const auto corner=unsigned(v[0]>0)|(unsigned(v[1]<0)<<1);
        if(corners&(1u<<corner))return false;
        corners|=1u<<corner;
    }
    return true;
}
struct NativeFilterWeightsIdentity {std::vector<std::uint8_t> vertex,pixel;};
struct NativeFilterWeightsPlan {
    NativeOffscreenDrawPlan target;
    XenosTextureDescription texture;
    unsigned textureIndex=0;
    std::array<float,4> viewport{};
};
inline constexpr unsigned NativeFilterWeightsRegisters[][2]{
    {0x2000,0x0A000280},{0x2001,0x000302D0},{0x2104,15},{0x210D,0xFFFFFF},
    {0x210F,0x43A00000},{0x2110,0x43A00000},{0x2111,0xC3340000},{0x2112,0x43340000},
    {0x2113,0xBF800000},{0x2114,0x3F800000},
    {0x2180,0x10010E01},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
    {0x2200,0x00700760},{0x2201,0x10001},{0x2202,0xAA000003},{0x2203,0x10},
    {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2207,0},{0x2208,4},
    {0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
    // PA_SC_VIZ_QUERY enable bit0 is clear; the remaining value is a query
    // identifier, NOT evidence that an independently owned query is idle.
    {0x2293,0x14},{0x2300,0x400},{0x2301,0},{0x2302,5},
    {0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
    {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
inline std::optional<NativeFilterWeightsPlan> planNativeFilterWeightsDraw(
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const NativeFilterWeightsIdentity& identity,const std::vector<std::uint8_t>& currentVertex,
        const std::vector<std::uint8_t>& currentPixel,bool indexed,bool queriesIdle,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeFilterWeightsPlan>{};};
    if(identity.vertex.size()!=132 || identity.pixel.size()!=708 ||
       identity.vertex!=currentVertex || identity.pixel!=currentPixel)return no("shader-identity");
    if(indexed || draw.primitive!=8 || draw.vertexCount!=3 || draw.stride!=24 ||
       draw.vertices.size()!=72)return no("rectangle-extent");
    if(!nativeFilterWeightsFullRectangle(draw))return no("original-position-uv-coverage");
    if(draw.deviceState.size()!=13860)return no("state-extent");
    if(!queriesIdle)return no("query-owner-not-idle");
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("shader-binding");
    for(const auto& r:NativeFilterWeightsRegisters)
        if(gta5DrawRegister(draw,r[0])!=r[1])return no("raster-profile");
    if(draw.deviceState[11068]&0x10 || word(draw.deviceState,12408)!=0)return no("surface-mode");
    const auto scissor=nativeTranslatedScissor(draw);
    const auto viewport=nativeTranslatedViewport(draw);
    if(!scissor || *scissor!=std::array<unsigned,4>{0,0,640,360} || !viewport)return no("viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto& s=bound.surfaces[i];
        if(s.object!=word(draw.deviceState,12816+4*i))return no("surface-binding");
        if(i){if(s.object)return no("extra-surface");continue;}
        if(!s.object || s.object%4 || s.object>UINT32_MAX-39 || !s.header || s.header->size()!=40)
            return no("surface-header");
        if(word(*s.header,24)!=0x0A000280 || word(*s.header,28)!=0x302D0)return no("surface-format");
        const auto dimensions=word(*s.header,36);
        if((dimensions>>18)+1!=640 || ((dimensions>>3)&0x7FFF)+1!=360)return no("surface-dimensions");
    }
    if(draw.textures.size()!=2)return no("texture-count");
    unsigned pixel=UINT32_MAX,vertex=UINT32_MAX;
    for(unsigned i=0;i<draw.textures.size();++i) {
        if(draw.textures[i].slot==5 && pixel==UINT32_MAX)pixel=i;
        else if(draw.textures[i].slot==17 && vertex==UINT32_MAX)vertex=i;
        else return no("texture-slot");
    }
    if(pixel==UINT32_MAX || vertex==UINT32_MAX)return no("texture-slot");
    const auto& v=draw.textures[vertex];const auto& p=draw.textures[pixel];
    if(!v.literalZero || !v.source.empty() || !nativeFilterWeightsVertexLiteral(v.fetch))
        return no("vertex-literal");
    if(p.literalZero || p.fetch[0]!=0x8A004802 || (p.fetch[1]&0xFFF)!=0xB6 ||
       p.fetch[2]!=0x59E4FF || p.fetch[3]!=0xA86C14 || p.fetch[4]!=3 || p.fetch[5]!=0x210)
        return no("pixel-sampling-profile");
    const auto d=XenosTextureDescription::decode(p.fetch);
    const auto storage=d?planXenosRaw10UnsignedStorage(*d):std::nullopt;
    if(!d || !storage || !d->baseAddress || std::uint64_t{d->baseAddress}+storage->sourceBytes>0x20000000ULL ||
       p.source.size()!=storage->sourceBytes)return no("pixel-owned-storage");
    if(why)*why="state-resources-planned-not-executed";
    return NativeFilterWeightsPlan{{bound.surfaces[0].object,640,360,640,720,184,3},*d,pixel,*viewport};
}
}
