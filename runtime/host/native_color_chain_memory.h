#pragma once
#include "native_color_chain_inputs.h"
#include "native_color_chain_events.h"
#include "native_color_copy_request.h"

namespace recomp::gpu {
struct NativePhysicalRange {
    std::uint32_t address=0,bytes=0;
};
inline bool validNativePhysicalRange(NativePhysicalRange r) noexcept {
    return r.bytes && !(r.address&3) && !(r.bytes&3) && std::uint64_t{r.address}+r.bytes<=0x20000000;
}
inline bool nativePhysicalOverlap(NativePhysicalRange a,NativePhysicalRange b) noexcept {
    return a.address<std::uint64_t{b.address}+b.bytes && b.address<std::uint64_t{a.address}+a.bytes;
}
// Every non-immediate shader load and constant load remains protected, even if
// a later command replaces it. Raw captured vertex/texture sources are separate
// ranges. Read/read overlaps are permitted. This is an address request, NOT an
// allocation lease, GPU-time snapshot or proof of host-pointer uniqueness.
struct NativeColorSourceRanges {
    std::array<NativePhysicalRange,47> ranges{};
    unsigned count=0,shaderRanges=0,constantRanges=0,constantZeroTextures=0;
};
inline bool collectNativeColorSourceRanges(const DrawCommandPlan& plan,const NativeColorChainBindings& inputs,
        NativeColorSourceRanges& output) noexcept {
    if(plan.shaders.size()>8 || plan.constants.size()>32 || inputs.pairs[0].textures.size()!=3 || inputs.pairs[1].textures.size()!=1)return false;
    NativeColorSourceRanges candidate;std::uint64_t total=0;
    const auto append=[&](std::uint32_t address,std::size_t bytes){
        if(!bytes || bytes>2*1024*1024 || total+bytes>2*1024*1024 || candidate.count==candidate.ranges.size())return false;
        const NativePhysicalRange range{address,static_cast<std::uint32_t>(bytes)};
        if(!validNativePhysicalRange(range))return false;
        candidate.ranges[candidate.count++]=range;total+=bytes;return true;
    };
    unsigned shaderBytes=0,constantBytes=0;
    for(const auto& shader:plan.shaders){
        const auto size=shader.bigEndianCode.size();
        if(shader.stage>1 || !size || size%12 || size>4096-shaderBytes)return false;
        shaderBytes+=static_cast<unsigned>(size);
        if(shader.immediate){if(shader.physicalAddress)return false;}
        else {if(!append(shader.physicalAddress,size))return false;++candidate.shaderRanges;}
    }
    for(const auto& constants:plan.constants){
        const auto size=constants.bigEndianWords.size();
        if(!size || size%4 || size>8192-constantBytes || !append(constants.physicalAddress,size))return false;
        constantBytes+=static_cast<unsigned>(size);++candidate.constantRanges;
    }
    for(const auto* vertex:{&inputs.depth,&inputs.pairs[0].vertices,&inputs.pairs[1].vertices})
        if(!append(vertex->source.physicalAddress,vertex->source.size))return false;
    for(unsigned i=0;i<2;++i)for(const auto& texture:inputs.pairs[i].textures) {
        if(i==1 && color_chain_inputs::constantZeroTexture(texture)) {++candidate.constantZeroTextures;continue;}
        if(texture.sourceKind!=NativeColorTextureBinding::SourceKind::GuestMemory ||
            !append(texture.source.physicalAddress,texture.source.size))return false;
    }
    output=candidate;return true;
}
struct NativeColorMemoryControls {
    NativePhysicalRange ring,child,readPointer,identifier;
    unsigned previousWord=0,nextWord=0;
    std::array<std::uint32_t,3> rootWords{};
};
inline bool validNativeColorMemoryControls(const NativeColorMemoryControls& controls,std::size_t childWordCount) noexcept {
    if(!childWordCount || childWordCount>4096 || !validNativePhysicalRange(controls.ring) || controls.ring.bytes>16*1024*1024 ||
        !validNativePhysicalRange(controls.child) || controls.child.bytes!=childWordCount*4 ||
        !validNativePhysicalRange(controls.readPointer) || controls.readPointer.bytes!=4 ||
        !validNativePhysicalRange(controls.identifier) || controls.identifier.bytes!=4 ||
        std::uint64_t{controls.previousWord}+3!=controls.nextWord || controls.nextWord>=controls.ring.bytes/4 ||
        controls.rootWords!=std::array<std::uint32_t,3>{0xC0013F00,controls.child.address,static_cast<std::uint32_t>(childWordCount)})return false;
    const std::array<NativePhysicalRange,4> ranges{{controls.ring,controls.child,controls.readPointer,controls.identifier}};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)if(nativePhysicalOverlap(ranges[i],ranges[j]))return false;
    return true;
}
struct NativeColorMemoryRequest {
    // Caller resolves all inputs and four controls while lifecycle-locked and
    // all generated writers parked, retaining that ownership through publish.
    std::array<NativePhysicalRange,48> inputs{};
    unsigned inputCount=0;
    std::array<NativePhysicalRange,4> controls{};
    // Tiled copy, two literal events, RPTR (publication order).
    std::array<NativePhysicalRange,4> outputs{};
};
// Physical exclusion proof only. Does not read root words/RPTR from RAM, resolve
// owners, pin storage, compare host aliases, fence, publish or retire commands.
// All original source ranges, including copy vertices and shader loads, exclude
// the entire ring/child, RPTR and identifier. Output capacity includes untouched
// tiled padding, conservatively protected even though publication preserves it.
inline bool planNativeColorMemory(const NativeColorSourceRanges& sources,std::size_t childWordCount,
        const NativeColorCopyRequest& copy,const NativeColorChainEventRequests& events,
        const NativeColorMemoryControls& controls,NativeColorMemoryRequest& output) noexcept {
    if(sources.constantZeroTextures>1 || sources.count<6 || sources.count>47 || sources.shaderRanges>8 || sources.constantRanges>32 ||
        sources.count!=7-sources.constantZeroTextures+sources.shaderRanges+sources.constantRanges ||
        !validNativeColorMemoryControls(controls,childWordCount))return false;
    // Rebuild the destination capacity from the known copy profile, not mutable
    // plan fields. Literal events must still describe exactly the two BE words.
    ColorCopyPlan expected;
    if(copy.copy.source.surface!=0x14000500 || copy.copy.source.info!=0xC02D0 ||
        copy.copy.source.width!=1280 || copy.copy.source.height!=720 || copy.copy.source.depth || !copy.copy.swapRedBlue ||
        !planColorCopy(copy.copy.source,0x100000,copy.copy.destinationBase,0x2D00500,0x1000300,expected) ||
        copy.copy.capacity!=expected.capacity || copy.copy.touchedSpan!=expected.touchedSpan || copy.copy.pixelBytes!=expected.pixelBytes ||
        !events.cacheFlushOffset || !events.packetOffsets[0] || events.packetOffsets[0]>=events.packetOffsets[1] ||
        events.cacheFlushOffset>=events.packetOffsets[0] || events.packetOffsets[1]>=childWordCount ||
        events.staging.count!=2 || events.staging.bufferBytes!=40)return false;
    for(unsigned i=0;i<2;++i)if(events.staging.words[i].byteOffset!=16+i*4 ||
        events.staging.words[i].phase!=NativeWordPhase::AfterGraphics)return false;
    NativeColorMemoryRequest candidate;
    candidate.controls={{controls.ring,controls.child,controls.readPointer,controls.identifier}};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)
        if(nativePhysicalOverlap(candidate.controls[i],candidate.controls[j]))return false;
    candidate.outputs={{{copy.copy.destinationBase,expected.capacity},
        {events.physicalAddresses[0],4},{events.physicalAddresses[1],4},controls.readPointer}};
    for(unsigned i=0;i<4;++i){
        if(!validNativePhysicalRange(candidate.outputs[i]))return false;
        for(unsigned j=0;j<i;++j)if(nativePhysicalOverlap(candidate.outputs[i],candidate.outputs[j]))return false;
        for(unsigned j=0;j<4;++j){
            if(i==3 && j==2)continue; // RPTR is the only deliberate control write.
            if(nativePhysicalOverlap(candidate.outputs[i],candidate.controls[j]))return false;
        }
    }
    std::uint64_t total=0;
    for(unsigned i=0;i<=sources.count;++i){
        const NativePhysicalRange range=i<sources.count?sources.ranges[i]:NativePhysicalRange{copy.vertexPhysicalAddress,24};
        if(!validNativePhysicalRange(range) || (total+=range.bytes)>2*1024*1024)return false;
        for(const auto& control:candidate.controls)if(nativePhysicalOverlap(range,control))return false;
        for(const auto& target:candidate.outputs)if(nativePhysicalOverlap(range,target))return false;
        candidate.inputs[candidate.inputCount++]=range;
    }
    output=candidate;return true;
}
}
