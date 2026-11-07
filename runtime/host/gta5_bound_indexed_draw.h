#pragma once
#include "gta5_draw_state.h"
#include "xenos_draw_bin_predicate.h"
#include <algorithm>

namespace recomp::gpu {
// Original823B9DF4 compares index count against65535 (branch823B9E00) before its
// primitive-splitting path. Both native indexed target implementations check
// the same single-draw limit and allocate65536 uint32 index-buffer entries.
// Count admission is separate from owned byte extents, shader/state support,
// visibility and GPU completion; none of those checks may be bypassed.
inline constexpr unsigned Gta5SingleIndexedDrawMaxIndices=65535;
struct Gta5BoundIndexedRequest {
    // Original 823B9B58 arguments r4/r5/r6/r7; count is INDICES, not triangles.
    std::uint32_t primitive=0,baseVertex=0,startIndex=0,indexCount=0;
};
// Owned input bytes only. In particular this is NOT a native bin-pass proof.
struct Gta5BoundIndexedStorage {
    Gta5InlineDraw draw;
    Gta5BoundIndexedRequest request;
    std::uint32_t indexResource=0,indexPhysicalAddress=0,vertexResource=0;
    std::uint32_t vertexExtent=0,fetchSlot=95;
    std::vector<std::uint32_t> rawIndices,effectiveIndices;
};
struct Gta5BoundIndexedDraw : Gta5BoundIndexedStorage { DrawBinPredicate predicate; };

// CPU-write window for the bounded original binned emitter, not a GPU receipt.
// Original823B9DBC..823B9FC4 writes64 command bytes and16 list bytes for one
// <=65535-index triangle draw. Original823B9E00 branches past splitting when
// count<=65535; the diagnostic may retain this range without GPU admission.
// Reject allocation/flush cases before proceeding.
struct Gta5RecordedIndexedEmissionWindow {std::uint32_t command=0,record=0;};
inline std::optional<Gta5RecordedIndexedEmissionWindow> describeGta5RecordedIndexedEmission(
        const Gta5BoundIndexedStorage& capture,const Gta5ResourceReader& owned) {
    const auto& b=capture.draw.deviceState;const auto& r=capture.request;
    if(!owned || b.size()!=13860 || b[11068]!=0x21 || r.primitive!=4 ||
       !r.indexCount || r.indexCount>65535 || r.indexCount%3 || capture.rawIndices.size()!=r.indexCount ||
       capture.effectiveIndices.size()!=r.indexCount)return std::nullopt;
    const auto word=[&](unsigned at){return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];};
    const auto command=word(48),limit=word(52),flush=word(56),record=word(13628),end=word(13632);
    // The captured allocator uses A/B graphics virtual addresses. Do not invent
    // another namespace/alias policy or let arithmetic wrap into one.
    if(command<0xA0000000 || command>0xBFFFFFBB || record<0xA0000000 || record>0xBFFFFFEF ||
       command%4 || record%4 || command>flush || flush>limit || limit>=0xC0000000 ||
       command+64>limit || record+16>end || end>=0xC0000000 ||
       (command+4<record+16 && record<command+68))return std::nullopt;
    const auto commands=owned(command+4,64),records=owned(record,16);
    if(!commands || commands->size()!=64 || !records || records->size()!=16)return std::nullopt;
    return Gta5RecordedIndexedEmissionWindow{command,record};
}

// Rendering needs capture at 823B9B58's prepared-state boundary (823B9DBC),
// not entry or a later ring flush. Diagnostic-only callers may sample after
// successful original return, without claiming pre-emission GPU execution.
// The caller must park writers and hold allocation ownership
// throughout every read, then overlay ORDERED submitted constants/textures and
// effective shader code before rendering. This helper neither clears dirty
// bits, retires commands, nor certifies a shader, render target or GPU draw.
//
// Current geometry profile: stream0/fetch95, uint16 swap8in16 triangle lists,
// explicit bin predicate Pass, no driver query-patching mode. Other vertex
// streams, formats, queries and primitive splitting remain unsupported.
// Index mapping follows original 823B9B58. Stream binding is the prepared
// fetch95 descriptor, not 823B9768's intermediate raw address/size pair.
// Resource headers may themselves reside in allocated graphics virtual memory.
// data() receives an explicitly virtual resource header or computed PHYSICAL
// payload address and must validate its namespace/allocation. It may
// not turn an arbitrary address into an alias merely by masking it.
namespace detail {
inline std::optional<Gta5BoundIndexedStorage> captureBoundIndexedStorage(
        std::uint32_t device,const Gta5BoundIndexedRequest& request,
        std::vector<std::uint8_t> snapshot,const Gta5ResourceReader& heap,
        const Gta5ResourceReader& data,unsigned maxIndices=Gta5SingleIndexedDrawMaxIndices) {
    constexpr unsigned DeviceBytes=13860,MaxBytes=256*1024;
    if(!device || std::uint64_t{device}+DeviceBytes>0x100000000ULL || !heap || !data ||
       request.primitive!=4 || !request.indexCount || request.indexCount>maxIndices || request.indexCount%3 ||
       snapshot.size()!=DeviceBytes)return std::nullopt;
    const auto word=[](const std::uint8_t* p) {
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
    };
    const auto field=[&](unsigned offset){return word(snapshot.data()+offset);};
    // Exact visibility token calculation in 823B9B58. The observed profile
    // uses selector0/table[0]=0; do not silently lose a nonzero query token.
    const unsigned selector=snapshot[12563];
    if(selector || snapshot[12498] || (field(10568)&(1u<<21)))return std::nullopt;
    Gta5BoundIndexedStorage result;
    result.request=request;
    result.indexResource=field(12812);result.vertexResource=field(12836);
    result.draw.vertexShader=field(13072);result.draw.pixelShader=field(13068);
    result.draw.stride=unsigned(snapshot[12904])*4;
    if(!result.indexResource || !result.vertexResource || !result.draw.vertexShader ||
       !result.draw.pixelShader || !result.draw.stride)return std::nullopt;
    const auto resource=[&](std::uint32_t address) {
        return address>=0xA0000000?data(address,32):heap(address,32);
    };
    const auto indexHeader=resource(result.indexResource),vertexHeader=resource(result.vertexResource);
    if(!indexHeader || indexHeader->size()!=32 || !vertexHeader || vertexHeader->size()!=32)return std::nullopt;
    const auto flags=word(indexHeader->data());
    if((flags&0x80000000u) || ((flags>>29)&3)!=1)return std::nullopt;
    const auto indexStart=std::uint64_t{request.startIndex}*2;
    const auto indexBytes=request.indexCount*2;
    const auto indexBase=word(indexHeader->data()+24),indexExtent=word(indexHeader->data()+28);
    if(indexStart+indexBytes>indexExtent || std::uint64_t{indexBase}+indexStart+indexBytes>0x100000000ULL)
        return std::nullopt;
    // The original conversion also handles its E-view page bias. Reject wrap
    // rather than manufacturing a physical interval across the arena boundary.
    const auto physical=[](std::uint32_t address) {
        return (address&0x1FFFFFFFu)+(((address>>20)+512u)&0x1000u);
    };
    result.indexPhysicalAddress=physical(static_cast<std::uint32_t>(indexBase+indexStart));
    if((result.indexPhysicalAddress&3) || std::uint64_t{result.indexPhysicalAddress}+indexBytes>0x20000000)
        return std::nullopt;
    const auto indexData=data(result.indexPhysicalAddress,indexBytes);
    if(!indexData || indexData->size()!=indexBytes)return std::nullopt;
    const auto minIndex=field(10448)&0xFFFFFFu,maxIndex=field(10444)&0xFFFFFFu;
    if(minIndex>maxIndex)return std::nullopt;
    unsigned highest=0;
    result.rawIndices.reserve(request.indexCount);result.effectiveIndices.reserve(request.indexCount);
    for(unsigned i=0;i<indexBytes;i+=2) {
        const unsigned index=(unsigned((*indexData)[i])<<8)|(*indexData)[i+1];
        // VGT offset wraps at 24 bits BEFORE the min/max clamp (Xenia
        // src/xenia/gpu/registers.h VGT_INDX_OFFSET). Primitive restart is
        // rejected above, before applying this indexing policy. Keeping raw
        // and effective indices separate preserves r0.x identity for shaders.
        const unsigned effective=std::clamp((index+request.baseVertex)&0xFFFFFFu,minIndex,maxIndex);
        result.rawIndices.push_back(index);result.effectiveIndices.push_back(effective);
        highest=std::max(highest,effective);
    }
    const auto vertexHeader0=word(vertexHeader->data()+24),vertexHeader1=word(vertexHeader->data()+28);
    // Xenos vertex descriptor: type2 bits, address30 bits (dwords), then
    // endian2 bits and size24 bits (dwords). Upper size-word bits are retained
    // in deviceState, not treated as byte extent. The live CPU-prepared stream
    // uses type3 / swap8in32, distinct from the uint16 index buffer's endian.
    const auto fetch0=field(1912),fetch1=field(1916);
    // Original 823B9768 adds the stream offset to header24, converts its
    // physical view, and subtracts that offset from header28. It does NOT
    // unpack/repack either word. Thus the header ALSO carries type/endian/
    // size bits; treating header28 as a raw byte count invents a huge offset.
    if((fetch0&3)!=3 || (fetch1&3)!=2 || (vertexHeader0&3)!=3 ||
       (vertexHeader1&3)!=2 || (vertexHeader1&0xFC000003)!=(fetch1&0xFC000003))return std::nullopt;
    const auto vertexBase=vertexHeader0&0xFFFFFFFC,vertexBytes=vertexHeader1&0x03FFFFFC;
    result.vertexExtent=fetch1&0x03FFFFFC;
    result.draw.vertexAddress=fetch0&0xFFFFFFFC;
    if(!result.vertexExtent || result.vertexExtent>vertexBytes)return std::nullopt;
    const auto bindingOffset=vertexBytes-result.vertexExtent;
    const auto bindingAddress=std::uint64_t{vertexBase}+bindingOffset;
    const auto needed=std::uint64_t{highest+1}*result.draw.stride;
    if(bindingAddress>0xFFFFFFFF || physical(static_cast<std::uint32_t>(bindingAddress))!=result.draw.vertexAddress ||
       (result.draw.vertexAddress&3) || !needed || needed>MaxBytes || needed>result.vertexExtent ||
       std::uint64_t{result.draw.vertexAddress}+needed>0x20000000)return std::nullopt;
    auto vertices=data(result.draw.vertexAddress,static_cast<std::uint32_t>(needed));
    if(!vertices || vertices->size()!=needed)return std::nullopt;
    result.draw.vertices=std::move(*vertices);result.draw.vertexCount=highest+1;
    result.draw.device=device;result.draw.primitive=request.primitive;
    for(unsigned i=0;i<1024;++i) {
        result.draw.vertexConstants[i]=field(1920+4*i);result.draw.pixelConstants[i]=field(6016+4*i);
    }
    result.draw.deviceState=std::move(snapshot);
    return result;
}
} // namespace detail

inline std::optional<Gta5BoundIndexedDraw> captureGta5BoundIndexedDraw(
        std::uint32_t device,const Gta5BoundIndexedRequest& request,
        const DrawBinState& bins,const Gta5ResourceReader& heap,const Gta5ResourceReader& data) {
    // Execution admission is distinct from recording, including early bounds
    // rejection before payload reads. Recorded storage cannot stand in for it.
    if(!device || std::uint64_t{device}+13860>0x100000000ULL || !heap || !data ||
       request.primitive!=4 || !request.indexCount || request.indexCount>Gta5SingleIndexedDrawMaxIndices || request.indexCount%3 ||
       evaluateDrawBinPredicate(bins,true)!=DrawBinDecision::Pass)return std::nullopt;
    auto snapshot=heap(device,13860);
    if(!snapshot || snapshot->size()!=13860 || ((*snapshot)[11068]&1))return std::nullopt;
    auto storage=detail::captureBoundIndexedStorage(device,request,std::move(*snapshot),heap,data);
    if(!storage)return std::nullopt;
    Gta5BoundIndexedDraw result;static_cast<Gta5BoundIndexedStorage&>(result)=std::move(*storage);
    result.predicate={bins,true};return result;
}

inline std::optional<Gta5BoundIndexedStorage> captureGta5RecordedIndexedStorage(
        std::uint32_t device,const Gta5BoundIndexedRequest& request,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& data) {
    // Diagnostic only: do not clear/forge the binned state flag, substitute a
    // native predicate, execute a draw, or publish completion. Preserve the
    // exact state in the returned input record. Same pointer/extent/ownership,
    // primitive/index mapping and unsupported-query checks as ordinary storage.
    if(!device || std::uint64_t{device}+13860>0x100000000ULL || !heap || !data ||
       request.primitive!=4 || !request.indexCount || request.indexCount>65535 || request.indexCount%3)return std::nullopt;
    auto snapshot=heap(device,13860);
    if(!snapshot || snapshot->size()!=13860 || ((*snapshot)[11068]&0x21)!=0x21)return std::nullopt;
    return detail::captureBoundIndexedStorage(device,request,std::move(*snapshot),heap,data,65535);
}
}
