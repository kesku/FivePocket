#pragma once
#include "gta5_clear_capture.h"
#include "gta5_color8_clear.h"
#include "gta5_driver_tiling.h"
#include "xenos_float24_clear.h"
#include <cstring>

namespace recomp::gpu {
// API-level effect of original 823D89A8 -> 8237B378, for logical tiled storage.
// Header bytes must come from current owned readers. This describes clear
// effects, not recording completion, guest ownership, query or draw support.
struct Gta5LogicalTileClear {
    Gta5DriverTiling layout;
    unsigned aspects=0,tiles=0;
    std::array<unsigned,4> rectangle{};
    std::array<std::array<std::uint8_t,4>,4> colors{};
    float depth=0;
    std::uint8_t stencil=0;
    bool empty()const{return !aspects || !tiles;}
};
// Physical surface identity only. Shader preparation in original823BAAC8
// updates11071 bit1; that does not change image layout. Clear-specific effects
// must still be admitted by planGta5LogicalTileClear, never by this description.
struct Gta5LogicalTileSurfaces {
    Gta5DriverTiling layout;
    unsigned boundAspects=0;
    std::array<unsigned,5> formats{};
};
inline std::optional<Gta5LogicalTileSurfaces> describeGta5LogicalTileSurfaces(const Gta5ClearCapture& c) {
    if(!c.state || c.state->size()<13860)return std::nullopt;
    const auto& state=*c.state;
    auto layout=describeGta5DriverTiling(state);
    if(!layout || !layout->boundTargetsUseTiles || !layout->disjointFullCoverage)return std::nullopt;
    const auto word=[](const auto& b,unsigned at) {return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto surfaceInfo=word(state,10368);
    if(surfaceInfo&0x3C000u)return std::nullopt; // Single sample, no reserved pitch bits.
    std::array<unsigned,5> bases{},counts{},formats{};
    unsigned localWidth=0,localHeight=0;
    for(const auto& t:layout->tiles) {
        localWidth=std::max(localWidth,t.local[2]);localHeight=std::max(localHeight,t.local[3]);
    }
    unsigned boundAspects=0;
    for(unsigned i=0;i<5;++i) {
        const auto& s=c.surfaces[i];const auto object=layout->boundTargets[i];
        if(s.object!=object || s.tileCache!=layout->targets[i])return std::nullopt;
        // A changed binding needs explicit image/alias transitions, not reuse.
        if(object!=layout->targets[i])return std::nullopt;
        if(!object)continue;
        if(object%4 || object>UINT32_MAX-39 || !s.header || s.header->size()!=40)return std::nullopt;
        const auto info=word(*s.header,24),target=word(*s.header,28),dim=word(*s.header,36);
        const auto width=(dim>>18)+1,height=((dim>>3)&0x7FFF)+1,pitch=info&0x3FFF;
        const unsigned registerOffset=i==4?10376:i==0?10372:10376+i*4;
        formats[i]=(target>>16)&15;bases[i]=target&0x7FF;
        if(info!=surfaceInfo || target!=word(state,registerOffset) || (target&~0x107FFu) ||
           (i==4?formats[i]!=1:formats[i]>1) || !pitch || pitch%80 || pitch<width ||
           width<localWidth || height<localHeight)return std::nullopt;
        counts[i]=((pitch+79)/80)*((height+15)/16);
        if(!counts[i] || counts[i]>2048-bases[i])return std::nullopt;
        for(unsigned j=0;j<i;++j)if(layout->targets[j] &&
                (object==layout->targets[j] ||
                 (bases[i]<bases[j]+counts[j] && bases[j]<bases[i]+counts[i])))return std::nullopt;
        boundAspects|=i==4?0x30:1u<<i;
    }
    return Gta5LogicalTileSurfaces{*layout,boundAspects,formats};
}
inline std::optional<Gta5LogicalTileClear> planGta5LogicalTileClear(const Gta5ClearCapture& c) {
    if(!c.state || c.state->size()<13860 || (c.call.flags&~0x3Fu))return std::nullopt;
    const auto& state=*c.state;
    // Preserve ALL clear-specific execution restrictions, including zero-aspect
    // calls. A surface description alone is not proof these effects are safe.
    if((state[11068]&0xC0) || (state[11071]&0x22) || (state[11072]&0xC0) ||
       (state[11068]&0x10))return std::nullopt;
    const auto surfaces=describeGta5LogicalTileSurfaces(c);if(!surfaces)return std::nullopt;
    const auto* layout=&surfaces->layout;
    const auto& formats=surfaces->formats;
    const auto word=[](const auto& b,unsigned at) {return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto real=[](std::uint32_t bits){float f;std::memcpy(&f,&bits,4);return f;};
    const auto signedWord=[](std::uint32_t v)->std::int64_t {return std::int64_t{v}-((v&0x80000000u)?0x100000000LL:0LL);};
    Gta5LogicalTileClear p;p.layout=*layout;
    p.aspects=c.call.flags&surfaces->boundAspects;
    // The original color loop performs depth/stencil only on its first actual
    // color clear. If color bits are requested but ALL those bindings are null,
    // it performs no depth clear either (823D8F78..823D8FC4).
    if((c.call.flags&15) && !(p.aspects&15))p.aspects=0;
    if(!p.aspects)return p;
    if(!c.rectangle || c.rectangle->size()!=16)return std::nullopt;
    std::array<std::int64_t,4> clip{},viewport{};
    for(unsigned i=0;i<4;++i) {
        const auto f=real(word(state,13024+i*4));
        if(!std::isfinite(f) || f<-32768 || f>32768)return std::nullopt;
        viewport[i]=static_cast<std::int64_t>(f);clip[i]=signedWord(word(*c.rectangle,i*4));
    }
    viewport[2]+=viewport[0];viewport[3]+=viewport[1];
    for(unsigned i=0;i<4;++i) {
        clip[i]=i<2?std::max(clip[i],viewport[i]):std::min(clip[i],viewport[i]);
        if(word(state,12288)) {
            const auto scissor=signedWord(word(state,13052+i*4));
            clip[i]=i<2?std::max(clip[i],scissor):std::min(clip[i],scissor);
        }
    }
    // 8237B378 intersects each bin before subtracting its local origin. Native
    // full-size images keep logical coordinates; never add row384 a second time.
    clip[0]=std::max<std::int64_t>(clip[0],0);clip[1]=std::max<std::int64_t>(clip[1],0);
    clip[2]=std::min<std::int64_t>(clip[2],layout->width);clip[3]=std::min<std::int64_t>(clip[3],layout->height);
    if(clip[0]>=clip[2] || clip[1]>=clip[3])return p;
    for(unsigned i=0;i<4;++i)p.rectangle[i]=static_cast<unsigned>(clip[i]);
    if(word(state,13088))return std::nullopt; // High-bin selection is not represented here.
    const auto selection=word(state,13084);
    for(unsigned i=0;i<layout->tiles.size();++i) {
        const auto& t=layout->tiles[i];
        if(!(selection&t.selectLow))continue;
        if(clip[0]<t.logical[2] && t.logical[0]<clip[2] && clip[1]<t.logical[3] && t.logical[1]<clip[3])p.tiles|=1u<<i;
    }
    if(!p.tiles)return p;
    if(p.aspects&15) {
        // A null color argument causes the original to read an image constant.
        // Without that captured value, don't substitute a presumed zero.
        if(!c.color || c.color->size()!=16)return std::nullopt;
        std::array<float,4> rgba{};for(unsigned i=0;i<4;++i)rgba[i]=real(word(*c.color,i*4));
        for(unsigned i=0;i<4;++i)if(p.aspects&(1u<<i)) {
            auto packed=packGta5Color8Clear(rgba,formats[i]);if(!packed)return std::nullopt;p.colors[i]=*packed;
        }
    }
    if(p.aspects&0x20) {if(c.call.stencil>255)return std::nullopt;p.stencil=c.call.stencil;}
    if(p.aspects&0x10) {
        double requested;std::memcpy(&requested,&c.call.depthBits,8);
        const auto value=planExactXenosFloat24Clear(requested,0);if(!value)return std::nullopt;p.depth=value->depth;
    }
    return p;
}
// A recorded clear can select several bins, but the original worker executes
// a single bin's pass at a time. Do not initialize a future logical tile before
// the intervening physical carry and that tile's own replay. The caller proves
// the worker phase/predicate admits this operation; this only narrows its tile.
inline std::optional<Gta5LogicalTileClear> planGta5LogicalTileClearForReplay(
        const Gta5ClearCapture& capture,unsigned tile) {
    auto plan=planGta5LogicalTileClear(capture);
    if(!plan || tile>=plan->layout.tiles.size())return std::nullopt;
    plan->tiles&=1u<<tile;
    return plan;
}
}
