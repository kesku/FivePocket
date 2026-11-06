#pragma once
#include "native_color_batch_capture.h"
#include <limits>

namespace recomp::gpu {
// Borrowed mapping metadata supplied by the lifecycle-locked allocation owner.
// This does NOT acquire a lock, park guest writers, pin an allocation or prove
// that a caller-provided pointer is mapped. The provider must retain those
// guarantees across capture, validation, GPU completion and guest publication.
struct NativeColorMappedRange {
    NativePhysicalRange range,allocation;
    std::uint8_t* allocationBytes=nullptr;
    bool writable=false;
    // Only call after validating the complete mapping set below.
    std::uint8_t* bytes() const noexcept {return allocationBytes+(range.address-allocation.address);}
};
struct NativeColorResolvedMemory {
    std::array<NativeColorMappedRange,48> inputs{};
    std::array<NativeColorMappedRange,4> controls{},outputs{};
};
namespace color_memory_detail {
inline bool same(NativePhysicalRange a,NativePhysicalRange b) noexcept {return a.address==b.address && a.bytes==b.bytes;}
inline bool hostOverlap(const std::uint8_t* a,std::size_t n,const std::uint8_t* b,std::size_t m) noexcept {
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    return x<y+m && y<x+n; // All endpoints have already been checked.
}
inline bool mapping(const NativeColorMappedRange& m,NativePhysicalRange requested,bool write) noexcept {
    return same(m.range,requested) && validNativePhysicalRange(m.range) && validNativePhysicalRange(m.allocation) &&
        m.range.address>=m.allocation.address &&
        std::uint64_t{m.range.address}+m.range.bytes<=std::uint64_t{m.allocation.address}+m.allocation.bytes &&
        m.allocationBytes && reinterpret_cast<std::uintptr_t>(m.allocationBytes)<=
            std::numeric_limits<std::uintptr_t>::max()-m.allocation.bytes && (!write || m.writable);
}
}
// No dereferences, callbacks or stores. Check every requested extent and the
// consistency of complete allocation mappings, not merely the touched slices.
// One physical allocation must have one host base/extent/protection; distinct
// physical allocations must not share host storage. Read/read slices may alias.
inline bool validateNativeColorResolvedMemory(const NativeColorMemoryRequest& request,
        const NativeColorResolvedMemory& memory) noexcept {
    using namespace color_memory_detail;
    if(request.inputCount<7 || request.inputCount>48 || request.controls[0].bytes>16*1024*1024 ||
        request.controls[1].bytes>16384 || request.controls[2].bytes!=4 || request.controls[3].bytes!=4 ||
        request.outputs[0].bytes!=3768320 || request.outputs[1].bytes!=4 || request.outputs[2].bytes!=4 ||
        !same(request.outputs[3],request.controls[2]))return false;
    std::array<const NativeColorMappedRange*,56> all{};unsigned count=0;std::uint64_t total=0;
    for(unsigned i=0;i<request.inputCount;++i){
        if((total+=request.inputs[i].bytes)>2*1024*1024 || !mapping(memory.inputs[i],request.inputs[i],false))return false;
        all[count++]=&memory.inputs[i];
    }
    for(unsigned i=0;i<4;++i){
        if(!mapping(memory.controls[i],request.controls[i],i==2) || !mapping(memory.outputs[i],request.outputs[i],true))return false;
        all[count++]=&memory.controls[i];all[count++]=&memory.outputs[i];
    }
    for(unsigned i=0;i<count;++i)for(unsigned j=0;j<i;++j){
        const auto& a=*all[i];const auto& b=*all[j];
        if(nativePhysicalOverlap(a.allocation,b.allocation)){
            if(!same(a.allocation,b.allocation) || a.allocationBytes!=b.allocationBytes || a.writable!=b.writable)return false;
        }else if(hostOverlap(a.allocationBytes,a.allocation.bytes,b.allocationBytes,b.allocation.bytes))return false;
    }
    const auto overlaps=[](const auto& a,const auto& b){return nativePhysicalOverlap(a.range,b.range) ||
        hostOverlap(a.bytes(),a.range.bytes,b.bytes(),b.range.bytes);};
    for(unsigned i=0;i<4;++i){
        for(unsigned j=0;j<i;++j)if(overlaps(memory.controls[i],memory.controls[j]) || overlaps(memory.outputs[i],memory.outputs[j]))return false;
        for(unsigned j=0;j<4;++j)if(!(i==3 && j==2) && overlaps(memory.outputs[i],memory.controls[j]))return false;
        for(unsigned j=0;j<request.inputCount;++j)if(overlaps(memory.inputs[j],memory.controls[i]) || overlaps(memory.inputs[j],memory.outputs[i]))return false;
    }
    return true;
}
// Check CURRENT provider-owned bytes against the complete captured snapshot.
// This is read-only and is not a replacement for writer exclusion: a compare
// followed by an unlocked GPU call is still a race. The identifier payload and
// destination contents are deliberately not read. No guest register is changed.
inline bool matchesNativeColorBatchMemory(const NativeColorBatchCapture& capture,
        const NativeColorResolvedMemory& memory) noexcept {
    const auto& controls=capture.controls();const auto& plan=capture.commands();
    if(!validNativeColorMemoryControls(controls,plan.commands.words().size()) ||
        !validateNativeColorResolvedMemory(capture.memory(),memory))return false;
    const auto word=[](const std::uint8_t* p){return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|
        (std::uint32_t{p[2]}<<8)|p[3];};
    if(word(memory.controls[2].bytes())!=controls.previousWord)return false;
    for(unsigned i=0;i<3;++i)if(word(memory.controls[0].bytes()+(controls.previousWord+i)*4)!=controls.rootWords[i])return false;
    for(unsigned i=0;i<plan.commands.words().size();++i)
        if(word(memory.controls[1].bytes()+i*4)!=plan.commands.words()[i])return false;
    try {
        unsigned input=0;
        const auto equal=[&](std::uint32_t address,const std::uint8_t* bytes,std::size_t size){
            if(input>=capture.memory().inputCount || !bytes)return false;
            const auto& m=memory.inputs[input++];
            return m.range.address==address && m.range.bytes==size && !std::memcmp(m.bytes(),bytes,size);
        };
        for(const auto& shader:plan.shaders)if(!shader.immediate && !equal(shader.physicalAddress,
            shader.bigEndianCode.data(),shader.bigEndianCode.size()))return false;
        for(const auto& constants:plan.constants)if(!equal(constants.physicalAddress,
            constants.bigEndianWords.data(),constants.bigEndianWords.size()))return false;
        NativeColorChainBindings raw;if(!capture.color().bindings(raw))return false;
        for(const auto* vertex:{&raw.depth,&raw.pairs[0].vertices,&raw.pairs[1].vertices})
            if(!equal(vertex->source.physicalAddress,vertex->source.bytes,vertex->source.size))return false;
        for(const auto& pair:raw.pairs)for(const auto& texture:pair.textures) {
            if(color_chain_inputs::constantZeroTexture(texture))continue;
            if(texture.sourceKind!=NativeColorTextureBinding::SourceKind::GuestMemory ||
                !equal(texture.source.physicalAddress,texture.source.bytes,texture.source.size))return false;
        }
        return equal(capture.copy().vertexPhysicalAddress,capture.copy().vertexBytes.data(),24) && input==capture.memory().inputCount;
    }catch(...){return false;}
}
}
