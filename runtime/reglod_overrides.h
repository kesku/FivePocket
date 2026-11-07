#pragma once
// dusk-speckle (2026-10-06, local-analysis/agents/ship-week/dusk-speckle/RESULT.md): register-LOD texture fetches.
//
// A Xenos pixel shader can fetch a texture at an explicit mip level: setTexLOD (fetch opcode 24) loads the LOD register
// and a tfetch whose UseRegisterLOD bit is set samples that level instead of the one the GPU computes from screen
// derivatives. The v41 XenosRecomp dropped the setTexLOD and never read the bit, so the device pack's modules sample those
// textures with the implicit level. In GTA V that is 108 pixel shaders, and one texture above all, the reflection map
// (512x256, four levels, rebuilt every frame):
//   - im#65p / im#67p / im#69p write its levels 1, 2, 3 from "the level above" (LOD = level - 1) with 1, 8 and 23 taps.
//     The area they draw is 1/2, 1/4, 1/8 of the texture, so the implicit level is about the level being written, i.e.
//     last frame's own output, not the level above;
//   - tiled_lighting#28p (the deferred ambient + reflection pass, the only light of a road pixel when the sun is off)
//     fetches the reflection at LOD = 4 * (1 - gloss)^2: level 3.8 for a road. The implicit level instead follows the
//     screen derivatives of a coordinate computed from the per-pixel (normal-mapped) normal.
//
// The fix is in the translation (the lane's xenosrecomp-reglod.patch: "texLod = rN.c" for setTexLOD, SampleLevel(...,
// texLod) for a register-LOD fetch); the 108 shaders re-translated with it are generic_reglod_overrides.inc
// (tools/make_reglod_overrides.py: same keys, same containers, new SPIR-V). apply() appends each module after the pack's
// bytes and points the pack's OWN entry at it, before initialize() takes any entry pointer or runs any module pass: every
// later reader (constant masks, v294's bank rewrite, the gc fold, the prewarm, the recipes) sees one module per key.
// All or nothing: one refused override leaves the pack exactly as it was read (why names the first refusal).
// files/private-trace/diag-generic-no-reglod-fix (read ONCE, at initialize()) keeps the pack's modules: today's behaviour.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace recomp::gpu::generic::reglod {

// A GSP2 entry (generic_renderer.cpp's PackEntry; written by tools/iter/generic_shader_pack.py).
struct Entry {
    std::uint64_t hash;std::uint32_t stage,spirvOffset,spirvWords,elementCount;
    std::uint32_t elements[24];std::uint32_t outputs,samplers;
    std::uint8_t elementDst[24];
};
static_assert(sizeof(Entry)==152);

struct Result {bool enabled=false;unsigned overrides=0,applied=0,words=0;const char* why="-";};

// Points every pack entry the table names at the table's module (appended to `pack`). `Override` is the renderer's
// DenormOverride {hash, words, code}; the table is generated (distinct keys, validated SPIR-V), so only what depends on
// the pack read at run time is checked here. `taken`/`takenCount`: the keys the renderer's other compiled-in tables
// replace (denorm, gradient), which would silently win over an entry changed here: such a key is refused.
template<class Override>
[[gnu::noinline]] Result apply(std::vector<std::uint8_t>& pack,const Override* table,std::size_t count,
                               const std::uint64_t* taken,std::size_t takenCount,std::uint32_t stageMask=1u<<1) noexcept {
    Result r;r.enabled=true;r.overrides=unsigned(count);
    std::uint32_t packCount=0;
    if(pack.size()<16 || std::memcmp(pack.data(),"GSP2",4)){r.why="pack-header";return r;}
    std::memcpy(&packCount,pack.data()+8,4);
    if(packCount>(pack.size()-16)/sizeof(Entry)){r.why="pack-header";return r;}
    if(count==0){r.why="no-overrides";return r;}
    std::vector<std::uint32_t> target; // override -> pack entry index
    std::size_t total=0;
    try {target.resize(count);} catch(...) {r.why="alloc";return r;}
    for(std::size_t i=0;i<count;++i) {
        const Override& o=table[i];
        for(std::size_t k=0;k<takenCount;++k)if(taken[k]==o.hash){r.why="override-taken";return r;}
        std::uint32_t found=packCount;
        for(std::uint32_t j=0;j<packCount;++j) {
            std::uint64_t hash;std::memcpy(&hash,pack.data()+16+std::size_t{j}*sizeof(Entry),8);
            if(hash==o.hash){found=j;break;}
        }
        if(found==packCount){r.why="override-key-missing";return r;}
        std::uint32_t stage;std::memcpy(&stage,pack.data()+16+std::size_t{found}*sizeof(Entry)+offsetof(Entry,stage),4);
        if(stage>31 || !((stageMask>>stage)&1u)){r.why="override-stage";return r;} // (register-LOD fetches exist in pixel shaders only:
        // the default mask is stage 1; iPad 2026-10-22: the co-issue table holds 133 vertex (stage 0) and 26 pixel modules and passes 0x3)
        target[i]=found;total+=std::size_t{o.words}*4+3;
    }
    if(pack.size()+total>std::size_t{0xFFFFFFFFu}){r.why="pack-size";return r;}
    try {pack.reserve(pack.size()+total);} catch(...) {r.why="alloc";return r;} // (no reallocation, so no throw, after this)
    for(std::size_t i=0;i<count;++i) {
        const Override& o=table[i];
        while(pack.size()%4)pack.push_back(0);
        const std::uint32_t offset=std::uint32_t(pack.size()),words=o.words;
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(o.code);
        pack.insert(pack.end(),bytes,bytes+std::size_t{words}*4);
        std::uint8_t* entry=pack.data()+16+std::size_t{target[i]}*sizeof(Entry);
        std::memcpy(entry+offsetof(Entry,spirvOffset),&offset,4);
        std::memcpy(entry+offsetof(Entry,spirvWords),&words,4);
        ++r.applied;r.words+=words;
    }
    return r;
}

// The switch file: present = nothing read, nothing changed (enabled stays false).
template<class Override>
[[gnu::noinline]] Result applyUnlessSwitch(std::vector<std::uint8_t>& pack,const Override* table,std::size_t count,
                                           const std::uint64_t* taken,std::size_t takenCount,const char* switchPath,
                                           std::uint32_t stageMask=1u<<1) noexcept {
    if(access(switchPath,F_OK)==0)return {};
    return apply(pack,table,count,taken,takenCount,stageMask);
}

} // namespace recomp::gpu::generic::reglod
