#pragma once
#include "native_deferred_color_capture.h"
#include "native_color_resolved_memory.h"

namespace recomp::gpu {
// Immutable physical exclusion requests for the WHOLE combined IB. Neither
// the prefix nor a successful read-only capture may independently retire it.
// This does not resolve/pin allocations or authorize any listed future write.
class NativeDeferredColorMemoryPlan final {
public:
    static std::optional<NativeDeferredColorMemoryPlan> create(const NativeDeferredColorCapture& capture,
            const NativeColorMemoryControls& controls,NativePhysicalRange oldScanout) noexcept {
        const auto& frame=capture.submission();const auto& sources=capture.color().sourceRanges();
        const auto words=frame.prefix().words().size()+NativeDeferredSwapWrapper::Words;
        NativeColorChainBindings bindings;
        if(!capture.color().bindings(bindings) || !validNativeColorMemoryControls(controls,words) ||
            controls.child.address!=frame.physicalAddress() || controls.previousWord!=frame.swap().command().identity().previousReadWord+3 ||
            !validNativePhysicalRange(oldScanout) || (oldScanout.address&4095) || oldScanout.bytes!=NativeSwapImagePlan::Capacity ||
            sources.constantZeroTextures>1 || sources.count<6 || sources.count>47 || sources.shaderRanges>8 || sources.constantRanges>32 ||
            sources.count!=7-sources.constantZeroTextures+sources.shaderRanges+sources.constantRanges)return std::nullopt;
        NativeDeferredColorMemoryPlan candidate;candidate.controls_=controls;candidate.oldScanout_=oldScanout;
        candidate.protected_={{controls.ring,controls.child,controls.readPointer,controls.identifier,oldScanout}};
        candidate.writes_[0]={frame.swap().command().identity().imagePhysical,NativeSwapImagePlan::Capacity};
        for(unsigned i=0;i<4;++i)candidate.writes_[i+1]={frame.swap().events()[i].physicalAddress,4};
        candidate.writes_[5]={frame.swap().scratchPhysical(),32};candidate.writes_[6]=controls.readPointer;
        for(unsigned i=0;i<candidate.protected_.size();++i)
            for(unsigned j=0;j<i;++j)if(nativePhysicalOverlap(candidate.protected_[i],candidate.protected_[j]))return std::nullopt;
        for(unsigned i=0;i<candidate.writes_.size();++i){
            const auto target=candidate.writes_[i];if(!validNativePhysicalRange(target))return std::nullopt;
            for(unsigned j=0;j<i;++j)if(nativePhysicalOverlap(target,candidate.writes_[j]))return std::nullopt;
            for(unsigned j=0;j<candidate.protected_.size();++j)
                if(!(i==6 && j==2) && nativePhysicalOverlap(target,candidate.protected_[j]))return std::nullopt;
        }
        std::uint64_t total=0;
        for(unsigned i=0;i<=sources.count;++i){
            const auto range=i<sources.count?sources.ranges[i]:NativePhysicalRange{capture.copy().vertexPhysicalAddress,24};
            if(!validNativePhysicalRange(range) || (total+=range.bytes)>2*1024*1024)return std::nullopt;
            for(const auto& control:candidate.protected_)if(nativePhysicalOverlap(range,control))return std::nullopt;
            for(const auto& target:candidate.writes_)if(nativePhysicalOverlap(range,target))return std::nullopt;
            candidate.inputs_[candidate.inputCount_++]=range;
        }
        return candidate;
    }
    const NativeColorMemoryControls& controls() const noexcept{return controls_;}
    const std::array<NativePhysicalRange,48>& inputs() const noexcept{return inputs_;}
    unsigned inputCount() const noexcept{return inputCount_;}
    // Copy image, four surface-tail event words, scratch32, final RPTR. Array
    // order is NOT a publication algorithm; tail phases/IRQ must be discharged.
    const std::array<NativePhysicalRange,7>& writes() const noexcept{return writes_;}
    const std::array<NativePhysicalRange,5>& protectedRanges() const noexcept{return protected_;}
    NativePhysicalRange oldScanout() const noexcept{return oldScanout_;}
private:
    NativeDeferredColorMemoryPlan()=default;
    NativeColorMemoryControls controls_;
    NativePhysicalRange oldScanout_;
    std::array<NativePhysicalRange,48> inputs_{};
    std::array<NativePhysicalRange,7> writes_{};
    std::array<NativePhysicalRange,5> protected_{};
    unsigned inputCount_=0;
};
// Borrowed owner metadata only. Caller retains the lifecycle lock, writer
// exclusion and allocation pins. Image buffers are never dereferenced here.
struct NativeDeferredColorResolvedMemory {
    std::array<NativeColorMappedRange,48> inputs{};
    std::array<NativeColorMappedRange,5> protectedRanges{};
    std::array<NativeColorMappedRange,7> writes{};
};
inline bool validateNativeDeferredColorResolvedMemory(const NativeDeferredColorMemoryPlan& request,
        const NativeDeferredColorResolvedMemory& memory) noexcept {
    using namespace color_memory_detail;
    std::array<const NativeColorMappedRange*,60> all{};unsigned count=0;
    for(unsigned i=0;i<request.inputCount();++i){
        if(!mapping(memory.inputs[i],request.inputs()[i],false))return false;
        all[count++]=&memory.inputs[i];
    }
    for(unsigned i=0;i<5;++i){
        if(!mapping(memory.protectedRanges[i],request.protectedRanges()[i],i==2))return false;
        all[count++]=&memory.protectedRanges[i];
    }
    for(unsigned i=0;i<7;++i){
        if(!mapping(memory.writes[i],request.writes()[i],true))return false;
        all[count++]=&memory.writes[i];
    }
    for(unsigned i=0;i<count;++i)for(unsigned j=0;j<i;++j){
        const auto& a=*all[i];const auto& b=*all[j];
        if(nativePhysicalOverlap(a.allocation,b.allocation)){
            if(!same(a.allocation,b.allocation) || a.allocationBytes!=b.allocationBytes || a.writable!=b.writable)return false;
        }else if(hostOverlap(a.allocationBytes,a.allocation.bytes,b.allocationBytes,b.allocation.bytes))return false;
    }
    // Immutable request has already proved disjoint physical slice extents.
    // Consistent non-aliasing complete owner mappings preserve that proof on
    // host pointers, including untouched image padding and the entire tail.
    return true;
}
inline bool matchesNativeDeferredColorMemory(const NativeDeferredColorCapture& capture,
        const NativeDeferredColorMemoryPlan& request,const NativeDeferredColorResolvedMemory& memory) noexcept {
    using namespace color_memory_detail;
    const auto rebuilt=NativeDeferredColorMemoryPlan::create(capture,request.controls(),request.oldScanout());
    if(!rebuilt || rebuilt->inputCount()!=request.inputCount() || !validateNativeDeferredColorResolvedMemory(request,memory))return false;
    for(unsigned i=0;i<request.inputCount();++i)if(!same(rebuilt->inputs()[i],request.inputs()[i]))return false;
    for(unsigned i=0;i<7;++i)if(!same(rebuilt->writes()[i],request.writes()[i]))return false;
    const auto word=[](const std::uint8_t* p){return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    const auto& controls=request.controls();
    if(word(memory.protectedRanges[2].bytes())!=controls.previousWord)return false;
    for(unsigned i=0;i<3;++i)if(word(memory.protectedRanges[0].bytes()+(controls.previousWord+i)*4)!=controls.rootWords[i])return false;
    try{
        const auto current=NativeDeferredSwapSubmission::parse(capture.submission().swap().command(),controls.child.address,
            memory.protectedRanges[1].bytes(),controls.child.bytes);
        if(!current || !current->sameRequest(capture.submission()))return false;
        unsigned input=0;
        const auto equal=[&](unsigned address,const std::uint8_t* bytes,std::size_t size){
            if(input>=request.inputCount() || !bytes)return false;
            const auto& m=memory.inputs[input++];
            return m.range.address==address && m.range.bytes==size && !std::memcmp(m.bytes(),bytes,size);
        };
        for(const auto& shader:capture.commands().shaders)if(!shader.immediate && !equal(shader.physicalAddress,
            shader.bigEndianCode.data(),shader.bigEndianCode.size()))return false;
        for(const auto& constants:capture.commands().constants)if(!equal(constants.physicalAddress,
            constants.bigEndianWords.data(),constants.bigEndianWords.size()))return false;
        NativeColorChainBindings bindings;if(!capture.color().bindings(bindings))return false;
        for(const auto* vertex:{&bindings.depth,&bindings.pairs[0].vertices,&bindings.pairs[1].vertices})
            if(!equal(vertex->source.physicalAddress,vertex->source.bytes,vertex->source.size))return false;
        for(const auto& pair:bindings.pairs)for(const auto& texture:pair.textures){
            if(color_chain_inputs::constantZeroTexture(texture))continue;
            if(texture.sourceKind!=NativeColorTextureBinding::SourceKind::GuestMemory ||
                !equal(texture.source.physicalAddress,texture.source.bytes,texture.source.size))return false;
        }
        return equal(capture.copy().vertexPhysicalAddress,capture.copy().vertexBytes.data(),24) && input==request.inputCount();
    }catch(...){return false;}
}
}
