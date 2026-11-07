#pragma once
#include "gta5_inline_draw.h"
#include "native_color_vertex_geometry.h"
#include <algorithm>
#include <cstring>

namespace recomp::gpu {
inline bool validateGta5StartupGeometry(const Gta5InlineDraw& draw,
        const NativeColorShaderIdentity& shader,unsigned pair) {
    if(pair>1 || draw.vertices.size()!=144)return false;
    VertexArithmeticInputs input;input.vertices=draw.vertices.data();input.vertexBytes=144;input.fetchSlot=95;
    for(unsigned c=8;c<=36;++c)if(c<=11 || (!pair && c==36)) {
        input.constantKnown[c]=15;
        for(unsigned lane=0;lane<4;++lane)input.constantBits[c][lane]=draw.vertexConstants[c*4+lane];
    }
    NativeColorVertexGeometry geometry;
    return planNativeColorVertexGeometryInputs(shader,input,pair,geometry);
}
// Adapts owned API-level inputs to the EXISTING validated shader ABI v1.
// Pair0 is the R8 video shader; pair1 the small texture shader. These modules
// have bounded four-vertex fetches. Other shader ABIs need their own reflection /
// translation, not silently reusing this layout. Fixed-function state, shader
// identity and completed rendering remain obligations of the draw consumer.
inline std::optional<std::vector<std::uint8_t>> packGta5StartupDraw(
        const Gta5InlineDraw& draw,unsigned pair) {
    if(pair>1 || draw.primitive!=6 || draw.vertexCount!=4 || draw.stride!=36 || draw.vertices.size()!=144 ||
        draw.textures.size()!=(pair?1u:3u))return std::nullopt;
    std::vector<std::uint8_t> packed(pair?4352:1382720);
    const auto texture=[&](unsigned slot)->const Gta5InlineDraw::Texture* {
        const Gta5InlineDraw::Texture* found=nullptr;
        for(const auto& t:draw.textures)if(t.slot==slot){if(found)return nullptr;found=&t;}
        return found;
    };
    if(!pair) {
        std::size_t at=0;
        for(unsigned slot=0;slot<3;++slot) {
            const auto* t=texture(slot);if(!t || t->literalZero)return std::nullopt;
            LinearR8Plane profile;
            if(!decodeLinearR8Plane(t->fetch,profile))return std::nullopt;
            const auto d=XenosTextureDescription::decode(t->fetch);
            if(!d || d->format!=2 || d->width!=(slot?640u:1280u) || d->height!=(slot?360u:720u))return std::nullopt;
            const auto pixels=linearizeXenosTexture2D(*d,t->source);
            if(!pixels)return std::nullopt;
            std::memcpy(packed.data()+at,pixels->data(),pixels->size());at+=pixels->size();
        }
    } else {
        const auto* t=texture(2);if(!t)return std::nullopt;
        if(t->literalZero) {
            if(!isNativeConstantZeroTexture(t->fetch) || !t->source.empty())return std::nullopt;
        } else {
            Tiled32Texture profile;
            if(!decodeTiled32Texture(t->fetch,profile))return std::nullopt;
            const auto d=XenosTextureDescription::decode(t->fetch);
            if(!d || d->format!=6 || d->width!=32 || d->height!=32)return std::nullopt;
            const auto pixels=linearizeXenosTexture2D(*d,t->source);if(!pixels)return std::nullopt;
            for(unsigned p=0;p<1024;++p)for(unsigned c=0;c<4;++c) {
                const auto s=d->swizzle[c];
                packed[p*4+c]=s<4?(*pixels)[p*4+s]:s==4?0:255;
            }
        }
    }
    const auto constants=[&](unsigned offset,const auto& values,std::initializer_list<unsigned> indices) {
        for(const auto index:indices)for(unsigned lane=0;lane<4;++lane) {
            const auto value=values[index*4+lane];
            for(unsigned b=0;b<4;++b)packed[offset++]=static_cast<std::uint8_t>(value>>(24-8*b));
        }
    };
    std::memcpy(packed.data()+(pair?4144:1382496),draw.vertices.data(),144);
    if(pair) {
        constants(4112,draw.pixelConstants,{16,62});
        constants(4288,draw.vertexConstants,{8,9,10,11});
    } else {
        constants(1382416,draw.pixelConstants,{32,33,34,35,255});
        constants(1382640,draw.vertexConstants,{8,9,10,11,36});
    }
    return packed;
}
}
