#pragma once
#include "xenos_texture_2d.h"
#include "xenos_depth24_texture.h"
#include "xenos_float32_texture.h"
#include "native_constant_texture.h"
#include <functional>

namespace recomp::gpu {
// Owned inputs at the original EndVertices boundary. This is deliberately not
// a ticket/frame-specific GPU batch. A renderer must additionally interpret
// the captured render state, bind the effective shader programs, and resolve.
struct Gta5InlineDraw {
    struct Texture {
        unsigned slot=0;
        std::array<std::uint32_t,6> fetch{};
        bool literalZero=false;
        std::vector<std::uint8_t> source;
    };
    unsigned primitive=0,vertexCount=0,stride=0;
    std::uint32_t device=0,vertexAddress=0,vertexShader=0,pixelShader=0;
    std::vector<std::uint8_t> vertices;
    std::array<std::uint32_t,1024> vertexConstants{},pixelConstants{};
    // Original driver shadow, not a flat Xenos register file. Fields that do
    // not have a verified register mapping must not be interpreted as one.
    std::vector<std::uint8_t> deviceState;
    std::optional<std::array<std::uint32_t,3>> scissorRegisters;
    std::vector<Texture> textures;
};
using Gta5ResourceReader=std::function<std::optional<std::vector<std::uint8_t>>(
    std::uint32_t,std::uint32_t)>;
using Gta5TextureFetchReader=std::function<std::optional<std::array<std::uint32_t,6>>(unsigned)>;

// Both readers run while the caller holds the executor's exclusive gate.
// Heap reader validates CPU objects. Data reader resolves the address namespace
// through actual allocation ownership; never guess a physical alias by masking.
inline std::optional<Gta5InlineDraw> captureGta5InlineDraw(
        std::uint32_t device,unsigned primitive,unsigned vertexCount,unsigned stride,
        const std::vector<unsigned>& textureSlots,const Gta5ResourceReader& heap,
        const Gta5ResourceReader& data,const Gta5TextureFetchReader& submittedFetch={},
        // ABI25's draw genuinely has no pixel stage, and its pixel shader
        // object is zero for that reason. Every other band requires one, so
        // this is opt-in: silence still means a draw must have both stages.
        bool allowAbsentPixelStage=false) {
    constexpr unsigned DeviceBytes=13860;
    const auto size=std::uint64_t{vertexCount}*stride;
    if(!heap || !data || !device || !vertexCount || !stride || (size&3) ||
        size>256*1024 || textureSlots.size()>32)return std::nullopt;
    auto snapshot=heap(device,DeviceBytes);
    if(!snapshot || snapshot->size()!=DeviceBytes)return std::nullopt;
    const auto word=[&](unsigned offset) {
        const auto* p=snapshot->data()+offset;
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
    };
    if(std::uint64_t{word(13856)}*4!=size)return std::nullopt;
    Gta5InlineDraw draw;
    draw.device=device;draw.primitive=primitive;draw.vertexCount=vertexCount;draw.stride=stride;
    draw.vertexAddress=word(13848);draw.vertexShader=word(13072);draw.pixelShader=word(13068);
    auto vertices=data(draw.vertexAddress,static_cast<std::uint32_t>(size));
    if(!vertices || vertices->size()!=size || !draw.vertexShader ||
       (!draw.pixelShader && !allowAbsentPixelStage))return std::nullopt;
    draw.vertices=std::move(*vertices);
    for(unsigned i=0;i<1024;++i) {
        draw.vertexConstants[i]=word(1920+i*4);draw.pixelConstants[i]=word(6016+i*4);
    }
    std::uint32_t slots=0;
    std::size_t total=draw.vertices.size();
    for(const auto slot:textureSlots) {
        if(slot>=32 || (slots&(1u<<slot)))return std::nullopt;
        slots|=1u<<slot;
        Gta5InlineDraw::Texture texture;texture.slot=slot;
        if(submittedFetch) {
            const auto fetch=submittedFetch(slot);
            if(!fetch)return std::nullopt;
            texture.fetch=*fetch;
        } else for(unsigned i=0;i<6;++i)texture.fetch[i]=word(1152+24*slot+4*i);
        texture.literalZero=isNativeConstantZeroTexture(texture.fetch);
        if(!texture.literalZero) {
            const auto description=XenosTextureDescription::decode(texture.fetch);
            // D24FS8 (format23) is addressed through its raw 32-bit storage view;
            // planXenosTexture2D deliberately refuses it as a colour import. This
            // only establishes how many OWNED bytes to copy, exactly as for a
            // colour texture; expansion to float stays with the image owner.
            // k_16_16_16_16_FLOAT (32) carries sign and integer-component
            // selects that planXenosTexture2D refuses for a colour import, so
            // it is addressed through its own raw storage view -- the same
            // treatment D24FS8 already gets. This only establishes how many
            // OWNED bytes to copy; the image owner decides the sampled format.
            const auto layout=!description?std::nullopt:
                description->format==23?planXenosDepth24FloatStorage(*description):
                description->format==32 || description->format==36 ||
                    description->format==25?planXenosFloatStorage(*description):
                planXenosTexture2D(*description);
            if(!layout || total+layout->sourceBytes>64u*1024*1024)return std::nullopt;
            auto pixels=data(description->baseAddress,layout->sourceBytes);
            if(!pixels || pixels->size()!=layout->sourceBytes)return std::nullopt;
            total+=pixels->size();texture.source=std::move(*pixels);
        }
        draw.textures.push_back(std::move(texture));
    }
    draw.deviceState=std::move(*snapshot);
    return draw;
}
}
