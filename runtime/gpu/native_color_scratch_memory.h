#pragma once
#include "native_color_resolved_memory.h"
#include <memory>
namespace recomp::gpu {
// Independent diagnostic memory ONLY, reconstructed from owned capture bytes.
// Never a real registry allocator, guest-pointer resolver or lifecycle lease.
// Fixed owned storage outlives all borrowed mappings; object cannot be copied
// or moved. Used by CPU fixtures and GPU scratch-publication integration tests.
class NativeColorScratchMemory {
public:
    NativeColorScratchMemory(const NativeColorScratchMemory&)=delete;
    NativeColorScratchMemory& operator=(const NativeColorScratchMemory&)=delete;
    static std::unique_ptr<NativeColorScratchMemory> create(const NativeColorBatchCapture& capture) noexcept {
        try {
            const auto& request=capture.memory();const auto& controls=capture.controls();
            NativeColorChainBindings bindings;
            if(request.inputCount>48 || !capture.color().bindings(bindings) ||
                !validNativeColorMemoryControls(controls,capture.commands().commands.words().size()))return nullptr;
            auto result=std::unique_ptr<NativeColorScratchMemory>(new NativeColorScratchMemory);
            auto& blocks=result->blocks_;auto& memory=result->memory_;
            std::vector<NativePhysicalRange> spans(request.inputs.begin(),request.inputs.begin()+request.inputCount);
            spans.insert(spans.end(),request.controls.begin(),request.controls.end());
            spans.insert(spans.end(),request.outputs.begin(),request.outputs.end());
            std::sort(spans.begin(),spans.end(),[](auto a,auto b){return a.address<b.address;});
            for(auto span:spans){
                if(!validNativePhysicalRange(span))return nullptr;
                if(!blocks.empty() && nativePhysicalOverlap(blocks.back().range,span)){
                    auto& range=blocks.back().range;range.bytes=static_cast<std::uint32_t>(
                        std::max(std::uint64_t{range.address}+range.bytes,std::uint64_t{span.address}+span.bytes)-range.address);
                }else blocks.push_back({span,{}});
            }
            for(auto& block:blocks)block.bytes.assign(block.range.bytes,0x5A);
            const auto map=[&](auto range){for(auto& block:blocks)if(range.address>=block.range.address &&
                std::uint64_t{range.address}+range.bytes<=std::uint64_t{block.range.address}+block.range.bytes)
                    return NativeColorMappedRange{range,block.range,block.bytes.data(),true};
                return NativeColorMappedRange{};};
            for(unsigned i=0;i<request.inputCount;++i)memory.inputs[i]=map(request.inputs[i]);
            for(unsigned i=0;i<4;++i){memory.controls[i]=map(request.controls[i]);memory.outputs[i]=map(request.outputs[i]);}
            if(!validateNativeColorResolvedMemory(request,memory))return nullptr;
            unsigned input=0;
            const auto put=[&](auto address,const auto* bytes,std::size_t size){
                if(input>=request.inputCount || !bytes)return false;const auto& m=memory.inputs[input++];
                if(m.range.address!=address || m.range.bytes!=size)return false;
                std::memcpy(m.bytes(),bytes,size);return true;};
            for(const auto& shader:capture.commands().shaders)if(!shader.immediate &&
                !put(shader.physicalAddress,shader.bigEndianCode.data(),shader.bigEndianCode.size()))return nullptr;
            for(const auto& c:capture.commands().constants)
                if(!put(c.physicalAddress,c.bigEndianWords.data(),c.bigEndianWords.size()))return nullptr;
            for(const auto* v:{&bindings.depth,&bindings.pairs[0].vertices,&bindings.pairs[1].vertices})
                if(!put(v->source.physicalAddress,v->source.bytes,v->source.size))return nullptr;
            for(const auto& pair:bindings.pairs)for(const auto& t:pair.textures){
                if(color_chain_inputs::constantZeroTexture(t))continue;
                if(t.sourceKind!=NativeColorTextureBinding::SourceKind::GuestMemory ||
                    !put(t.source.physicalAddress,t.source.bytes,t.source.size))return nullptr;
            }
            if(!put(capture.copy().vertexPhysicalAddress,capture.copy().vertexBytes.data(),24) || input!=request.inputCount)return nullptr;
            const auto write=[](std::uint8_t* p,std::uint32_t word){for(unsigned b=0;b<4;++b)p[b]=word>>(24-b*8);};
            for(unsigned i=0;i<3;++i)write(memory.controls[0].bytes()+(controls.previousWord+i)*4,controls.rootWords[i]);
            for(unsigned i=0;i<capture.commands().commands.words().size();++i)
                write(memory.controls[1].bytes()+i*4,capture.commands().commands.words()[i]);
            write(memory.controls[2].bytes(),controls.previousWord);
            if(!matchesNativeColorBatchMemory(capture,memory))return nullptr;
            return result;
        }catch(...){return nullptr;}
    }
    const NativeColorResolvedMemory& memory() const noexcept{return memory_;}
private:
    NativeColorScratchMemory()=default;
    struct Block{NativePhysicalRange range;std::vector<std::uint8_t> bytes;};
    std::vector<Block> blocks_;
    NativeColorResolvedMemory memory_;
};
}
