#pragma once
#include "gta5_bound_surface_capture.h"
#include "native_geometry_sampler_plan.h"
#include "native_mip_texture_capture.h"
#include "native_translated_draw.h"
#include <span>

namespace recomp::gpu {
using NativeIndexedLodTextures=std::array<NativeMipTextureCapture,5>;
enum class NativeIndexedLodProfile { Indexed600, Indexed601 };
enum class NativeIndexedLodRaster { LegacyBatches, ClippedWindows };
inline constexpr unsigned NativeIndexedLodWindowedMaxTriangles=4096;
inline bool validNativeIndexedLodProfile(NativeIndexedLodProfile p) {
    return p==NativeIndexedLodProfile::Indexed600 || p==NativeIndexedLodProfile::Indexed601;
}
inline std::array<unsigned,5> nativeIndexedLodSlots(NativeIndexedLodProfile p) {
    return p==NativeIndexedLodProfile::Indexed601?std::array<unsigned,5>{2,4,15,3,0}:
        std::array<unsigned,5>{2,15,3,1,0};
}
struct NativeIndexedLodPlan {
    std::array<float,4> viewport;
    std::array<XenosTextureDescription,5> textures;
};
inline bool validateNativeIndexedLodPositions(std::span<const std::array<float,4>> positions,
        const std::array<float,4>& viewport,NativeIndexedLodRaster raster,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return false;};
    if(raster!=NativeIndexedLodRaster::LegacyBatches && raster!=NativeIndexedLodRaster::ClippedWindows)
        return no("indexed-lod-raster-execution-profile");
    if(positions.empty())return no("indexed-lod-position-receipt");
    for(auto v:viewport)if(!std::isfinite(v))return no("indexed-lod-position-viewport");
    for(const auto& p:positions) {
        for(float v:p)if(!std::isfinite(v))return no("indexed-lod-position-finite");
        // Current original outputs are checked before rasterization: zero W
        // invalidates the ABI33 NDC varying even if clipping hides the vertex.
        if(p[3]==0 || (raster==NativeIndexedLodRaster::LegacyBatches && p[3]<0) ||
           !std::isfinite(p[2]/p[3]))return no("indexed-lod-position-w");
        for(unsigned c=0;c<2;++c) {
            volatile float scaled=p[c]*viewport[c],offset=p[3]*viewport[c+2];
            volatile float sum=scaled+offset;
            if(!std::isfinite(scaled) || !std::isfinite(offset) || !std::isfinite(sum))
                return no("indexed-lod-position-viewport");
        }
    }
    return true;
}
inline std::optional<XenosTextureDescription> planNativeIndexedLodTexture(unsigned index,
        const NativeMipTextureCapture& t,NativeIndexedLodProfile profile=NativeIndexedLodProfile::Indexed600) {
    // First-use order2,15,3,1,0. Address pages may relocate; all descriptor
    // format/dimension/swizzle/filter/mip/offset fields remain exact.
    constexpr unsigned original[][6]{
        {0x81000002,0x52,0x7E03F,0xA81690,0x43,0xA10},
        {0x81024802,0x52,0x6003,0xD10,0,0x210},
        {0x81000002,0x7B,0x7E03F,0xA815B6,0x43,0xA10},
        {0x84024802,0x86,0x1FE1FF,0xA80C14,0xC3,0x210},
        {0x81000002,0x54,0xFE07F,0xA80D10,0x83,0xA10}};
    constexpr unsigned next[][6]{
        {0x81000002,0x52,0xFE07F,0xA81690,0x83,0xA10},
        {0x81000002,0x52,0x7E03F,0xA81510,0x43,0xA10},
        {0x81024802,0x52,0x6003,0xD10,0,0x210},
        {0x81000002,0x7B,0xFE07F,0xA815B6,0x83,0xA10},
        {0x81000002,0x54,0xFE07F,0xA80D10,0x83,0xA10}};
    if(index>=5 || !validNativeIndexedLodProfile(profile))return std::nullopt;
    const auto& required=profile==NativeIndexedLodProfile::Indexed601?next:original;
    const auto slots=nativeIndexedLodSlots(profile);
    for(unsigned i=0;i<6;++i) {
        const auto mask=(i==1 || (i==5 && slots[index]!=15))?0xFFFu:UINT32_MAX;
        if((t.fetch[i]&mask)!=required[index][i])return std::nullopt;
    }
    const auto chain=planXenosUnpackedMipChain(t.fetch);
    const auto sampler=planNativeGeometrySampler2D(t.fetch);
    if(!chain || !sampler || sampler->anisotropy || t.base.size()!=chain->baseBytes ||
       t.mips.size()!=chain->mipBytes)return std::nullopt;
    return XenosTextureDescription::decode(t.fetch);
}
inline std::optional<NativeIndexedLodTextures> captureNativeIndexedLodTextures(
        const Gta5TextureFetchReader& submitted,const Gta5ResourceReader& graphics,
        NativeIndexedLodProfile profile=NativeIndexedLodProfile::Indexed600) {
    if(!submitted || !graphics || !validNativeIndexedLodProfile(profile))return std::nullopt;
    NativeIndexedLodTextures result;const auto slots=nativeIndexedLodSlots(profile);
    for(unsigned i=0;i<5;++i) {
        const auto f=submitted(slots[i]);if(!f)return std::nullopt;
        auto t=captureNativeMipTextureStorage(*f,graphics,2*1024*1024);
        if(!t || !planNativeIndexedLodTexture(i,*t,profile))return std::nullopt;
        result[i]=std::move(*t);
    }
    return result;
}
inline std::optional<NativeIndexedLodPlan> planNativeIndexedLod(
        const Gta5InlineDraw& draw,std::span<const unsigned> indices,
        const Gta5BoundSurfaceCapture& bound,const NativeIndexedLodTextures& textures,
        bool queriesIdle,const char** why=nullptr,
        NativeIndexedLodProfile profile=NativeIndexedLodProfile::Indexed600,
        NativeIndexedLodRaster raster=NativeIndexedLodRaster::LegacyBatches) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeIndexedLodPlan>{};};
    if(!queriesIdle)return no("indexed-lod-query-owner");
    if(!validNativeIndexedLodProfile(profile))return no("indexed-lod-profile");
    if((raster!=NativeIndexedLodRaster::LegacyBatches && raster!=NativeIndexedLodRaster::ClippedWindows) ||
       (raster==NativeIndexedLodRaster::ClippedWindows && profile!=NativeIndexedLodProfile::Indexed601))
        return no("indexed-lod-raster-execution-profile");
    const unsigned maxIndices=raster==NativeIndexedLodRaster::ClippedWindows?3*NativeIndexedLodWindowedMaxTriangles:
        profile==NativeIndexedLodProfile::Indexed601?36:12;
    if(draw.primitive!=4 || draw.stride!=28 || draw.vertexCount<3 || draw.vertexCount>65536 ||
       draw.vertices.size()!=std::uint64_t{draw.vertexCount}*28 || draw.vertices.size()>256*1024 ||
       indices.empty() || indices.size()>maxIndices || indices.size()%3)return no("indexed-lod-extent");
    for(auto i:indices)if(i>=draw.vertexCount)return no("indexed-lod-index-range");
    if(draw.deviceState.size()!=13860)return no("indexed-lod-state-extent");
    const auto word=[](const auto& b,unsigned at){return(std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    if(!draw.vertexShader || !draw.pixelShader || word(draw.deviceState,13072)!=draw.vertexShader ||
       word(draw.deviceState,13068)!=draw.pixelShader)return no("indexed-lod-shader-binding");
    constexpr unsigned required[][2]{
        {0x2000,0x14000500},{0x2001,0xC02D0},{0x2002,0x10000},
        {0x2104,15},{0x210D,0xFFFFFF},{0x210E,0},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},{0x2112,0x43B40000},{0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10611E07},{0x2181,8},{0x2182,0x3F007F},{0x2183,0},{0x2184,0},
        {0x2200,0x700766},{0x2201,0x07060706},{0x2202,0xAA00000C},{0x2203,0x13},
        {0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},{0x2207,0},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0x14},{0x2300,0x400},{0x2301,0},{0x2302,4},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& r:required)if(gta5DrawRegister(draw,r[0])!=r[1])return no("indexed-lod-raster-profile");
    if(word(draw.deviceState,12408)!=1 || draw.deviceState[11068] || draw.deviceState[12498] ||
       draw.deviceState[12563])return no("indexed-lod-recording-query-mode");
    const auto viewport=nativeTranslatedViewport(draw);const auto scissor=nativeTranslatedScissor(draw);
    if(!viewport || !scissor || *scissor!=std::array<unsigned,4>{0,0,1280,720})return no("indexed-lod-viewport-scissor");
    for(unsigned i=0;i<5;++i) {
        const auto object=word(draw.deviceState,12816+4*i);const auto& s=bound.surfaces[i];
        if(s.object!=object)return no("indexed-lod-surface-binding");
        if(i!=0 && i!=4){if(object)return no("indexed-lod-extra-surface");continue;}
        if(!object || object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40 ||
           word(*s.header,24)!=0x14000500 || word(*s.header,28)!=(i?0x10000u:0x302D0u) ||
           word(*s.header,36)!=0x13FC1678u)return no("indexed-lod-surface-profile");
    }
    if(bound.surfaces[0].object==bound.surfaces[4].object)return no("indexed-lod-surface-alias");
    NativeIndexedLodPlan result;result.viewport=*viewport;
    for(unsigned i=0;i<5;++i) {
        const auto d=planNativeIndexedLodTexture(i,textures[i],profile);if(!d)return no("indexed-lod-texture-profile");result.textures[i]=*d;
    }
    if(why)*why="indexed-lod-plan-only";return result;
}
}
