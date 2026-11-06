#pragma once
#include "xenos_linear_r8_plane.h"
#include "xenos_tiled32_texture.h"
#include "native_constant_texture.h"
#include <limits>
#include <utility>

namespace recomp::gpu {
struct NativeInputWord {
    std::uint32_t bits=0,knownMask=0;
};
// Borrowed exact source slice, never an ownership certificate. The caller must
// hold allocation lifetimes and park all writers for the whole packing call.
struct NativeColorSource {
    std::uint32_t physicalAddress=0;
    const std::uint8_t* bytes=nullptr;
    std::size_t size=0;
};
struct NativeColorVertexBinding {
    std::uint32_t slot=0;
    std::array<NativeInputWord,2> descriptor{};
    NativeColorSource source;
};
struct NativeColorTextureBinding {
    enum class SourceKind { GuestMemory, ConstantZeroSwizzle };
    std::uint32_t slot=0;
    std::array<NativeInputWord,6> descriptor{};
    NativeColorSource source;
    SourceKind sourceKind=SourceKind::GuestMemory;
};
struct NativeColorConstant {
    std::uint32_t index=0;
    std::array<NativeInputWord,4> words{};
};
struct NativeColorPairBindings {
    NativeColorVertexBinding vertices;
    std::vector<NativeColorTextureBinding> textures;
    std::vector<NativeColorConstant> vertexConstants,pixelConstants;
};
struct NativeColorChainBindings {
    NativeColorVertexBinding depth;
    std::array<NativeColorPairBindings,2> pairs;
};
struct PackedNativeColorChainInputs {
    std::vector<std::uint8_t> depth;
    std::array<std::vector<std::uint8_t>,2> pairs;
};

namespace color_chain_inputs {
inline bool validSource(const NativeColorSource& source,std::uint32_t address,std::size_t size) noexcept {
    if(!source.bytes || !size || source.size!=size || source.physicalAddress!=address || (address&3) ||
        address>=0x20000000 || size>std::uint64_t{0x20000000}-address)return false;
    const auto host=reinterpret_cast<std::uintptr_t>(source.bytes);
    return size<=std::numeric_limits<std::uintptr_t>::max()-host;
}
template<std::size_t N>
bool knownWords(const std::array<NativeInputWord,N>& source,std::array<std::uint32_t,N>& words) noexcept {
    for(std::size_t i=0;i<N;++i){if(source[i].knownMask!=0xFFFFFFFF)return false;words[i]=source[i].bits;}
    return true;
}
inline bool validVertex(const NativeColorVertexBinding& binding,unsigned slot,unsigned size) noexcept {
    std::array<std::uint32_t,2> words{};
    return binding.slot==slot && knownWords(binding.descriptor,words) && (words[0]&3)==3 &&
        words[1]==(0x10000000u|size|2u) && validSource(binding.source,words[0]&~3u,size);
}
inline bool constantZeroTexture(const NativeColorTextureBinding& binding) noexcept {
    std::array<std::uint32_t,6> words{};
    return binding.slot==2 && binding.sourceKind==NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle &&
        knownWords(binding.descriptor,words) && isNativeConstantZeroTexture(words) &&
        !binding.source.physicalAddress && !binding.source.bytes && !binding.source.size;
}
template<std::size_t N>
bool constants(const std::vector<NativeColorConstant>& source,const std::array<unsigned,N>& indices,
        std::array<std::uint8_t,N*16>& output) noexcept {
    if(source.size()!=N)return false;
    for(std::size_t i=0;i<N;++i){
        const NativeColorConstant* found=nullptr;
        for(const auto& c:source)if(c.index==indices[i]){if(found)return false;found=&c;}
        if(!found)return false;
        for(unsigned lane=0;lane<4;++lane){
            const auto& word=found->words[lane];if(word.knownMask!=0xFFFFFFFF)return false;
            for(unsigned byte=0;byte<4;++byte)output[i*16+lane*4+byte]=static_cast<std::uint8_t>(word.bits>>(24-byte*8));
        }
    }
    return true;
}
inline const NativeColorTextureBinding* texture(const NativeColorPairBindings& pair,unsigned slot) noexcept {
    const NativeColorTextureBinding* found=nullptr;
    for(const auto& t:pair.textures)if(t.slot==slot){if(found)return nullptr;found=&t;}
    return found;
}
}

// Exact current ABI: depth 84 bytes; R8 pair 1,382,720; small pair 4,352.
// The known register sets identify byte layout ONLY, not shader identity or
// fixed-function/predicate admission. No shader is executed here. Raw vertex
// and constant bits, including subnormals/nonfinite values, remain unchanged
// for the already guarded shader adapter to handle. No reference pixels enter.
// All output buffers are replaced together only after every source is packed.
inline bool packNativeColorChainInputs(const NativeColorChainBindings& input,
        PackedNativeColorChainInputs& output) noexcept {
    using namespace color_chain_inputs;
    if(!validVertex(input.depth,0,84) || !validVertex(input.pairs[0].vertices,95,144) ||
       !validVertex(input.pairs[1].vertices,95,144) || input.pairs[0].textures.size()!=3 ||
       input.pairs[1].textures.size()!=1)return false;
    std::array<std::uint8_t,80> r8Vertex{},r8Pixel{};
    std::array<std::uint8_t,64> smallVertex{};std::array<std::uint8_t,32> smallPixel{};
    if(!constants(input.pairs[0].vertexConstants,std::array<unsigned,5>{8,9,10,11,36},r8Vertex) ||
       !constants(input.pairs[0].pixelConstants,std::array<unsigned,5>{32,33,34,35,255},r8Pixel) ||
       !constants(input.pairs[1].vertexConstants,std::array<unsigned,4>{8,9,10,11},smallVertex) ||
       !constants(input.pairs[1].pixelConstants,std::array<unsigned,2>{16,62},smallPixel))return false;
    std::array<LinearR8Plane,3> planes{};
    std::array<const NativeColorTextureBinding*,3> textures{};
    for(unsigned i=0;i<3;++i){
        textures[i]=texture(input.pairs[0],i);std::array<std::uint32_t,6> words{};
        if(!textures[i] || textures[i]->sourceKind!=NativeColorTextureBinding::SourceKind::GuestMemory ||
            !knownWords(textures[i]->descriptor,words) || !decodeLinearR8Plane(words,planes[i]))return false;
        const auto& p=planes[i];
        if(p.width!=(i?640u:1280u) || !validSource(textures[i]->source,p.physicalAddress,p.pitch*p.height))return false;
    }
    const auto* small=texture(input.pairs[1],2);Tiled32Texture tile;std::array<std::uint32_t,6> words{};
    if(!small)return false;
    const bool literalZero=constantZeroTexture(*small);
    if(!literalZero && (small->sourceKind!=NativeColorTextureBinding::SourceKind::GuestMemory ||
       !knownWords(small->descriptor,words) || !decodeTiled32Texture(words,tile) ||
       !validSource(small->source,tile.physicalAddress,4096)))return false;
    try {
        PackedNativeColorChainInputs candidate;
        candidate.depth.assign(input.depth.source.bytes,input.depth.source.bytes+84);
        candidate.pairs[0].resize(1382720);candidate.pairs[1].resize(4352);
        auto* r8=candidate.pairs[0].data();std::size_t at=0;
        for(unsigned i=0;i<3;++i){const auto& p=planes[i];
            for(unsigned y=0;y<p.height;++y){std::memcpy(r8+at,textures[i]->source.bytes+y*p.pitch,p.width);at+=p.width;}}
        // The 16-byte unused headers remain zero from value initialization.
        std::memcpy(r8+1382416,r8Pixel.data(),r8Pixel.size());
        std::memcpy(r8+1382496,input.pairs[0].vertices.source.bytes,144);
        std::memcpy(r8+1382640,r8Vertex.data(),r8Vertex.size());
        std::array<std::uint8_t,4096> rgba{};
        // This is a native representation of four descriptor literals, NOT
        // an upload from unowned/null guest memory or a replacement game asset.
        if(!literalZero && !decodeTiled32Pixels(tile,small->source.bytes,small->source.size,rgba))return false;
        auto* smallBytes=candidate.pairs[1].data();
        std::memcpy(smallBytes,rgba.data(),rgba.size());
        std::memcpy(smallBytes+4112,smallPixel.data(),smallPixel.size());
        std::memcpy(smallBytes+4144,input.pairs[1].vertices.source.bytes,144);
        std::memcpy(smallBytes+4288,smallVertex.data(),smallVertex.size());
        output=std::move(candidate);return true;
    } catch (...) {return false;}
}
}
