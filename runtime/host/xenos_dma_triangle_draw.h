#pragma once
#include "xenos_command_stream.h"
#include "xenos_draw_bin_predicate.h"
#include "xenos_indirect_inspection.h"
#include <type_traits>

namespace recomp::gpu {
enum class DmaTriangleFailure {
    None, Packet, Layout, Predicate, Count, Address, MissingReader,
    ReadRejected, ReaderException, AllocationFailure
};
struct DmaTriangleRequest {
    std::uint32_t packetOffset = 0, initiator = 0, dmaSize = 0;
    std::uint32_t physicalAddress = 0, indexCount = 0;
    DrawBinPredicate predicate;
};
struct DmaTriangleInput {
    DmaTriangleRequest request;
    // Owned values only, NOT an admitted render command or a guest memory lease.
    std::vector<std::uint32_t> indices;
};

// Narrow observed profile: DRAW_INDX, viz token zero, triangle-list DMA,
// uint16 indices, swap8in16, explicit major mode zero, no NOT_EOP, and exact
// declared index count. Field layout follows VGT_DRAW_INITIATOR / VGT_DMA_SIZE
// in the pinned reference; it is not permission to ignore other draw modes.
// This function does not read memory, load a shader or write any register.
inline bool inspectDmaTriangleLayout(const Pm4Snapshot& stream, std::size_t packetIndex,
        DmaTriangleRequest& output, DmaTriangleFailure& error) noexcept {
    const auto fail = [&](DmaTriangleFailure why) { error=why; return false; };
    if (packetIndex >= stream.packets().size()) return fail(DmaTriangleFailure::Packet);
    const auto& p = stream.packets()[packetIndex];
    if (p.kind != Pm4Kind::Command || p.opcode != 0x22 || p.payloadWords != 4)
        return fail(DmaTriangleFailure::Layout);
    const auto* v = stream.words().data()+p.offsetWords+1;
    if (stream.words()[p.offsetWords] != (0xC0032200u | unsigned(p.predicated)) ||
        v[0] || (v[1]&0xFFFFu) != 4 || (v[3]&0xFF000000u) != 0x40000000u)
        return fail(DmaTriangleFailure::Layout);
    const auto count=v[1]>>16;
    // Original823B9B58 compares against65535 before splitting a larger API
    // draw; VGT_DRAW_INITIATOR stores16 count bits. The earlier4096 diagnostic
    // cap rejects12 actual mission draws (largest7272), despite every field
    // and owned byte extent being representable. Keep primitive/count/DMA
    // agreement checks, not an unrelated small-draw limit.
    if (!count || count%3 || (v[3]&0xFFFFFFu)!=count)
        return fail(DmaTriangleFailure::Count);
    if ((v[2]&3) || std::uint64_t{v[2]}+count*2>0x20000000)
        return fail(DmaTriangleFailure::Address);
    // Layout inspection deliberately leaves predicate knowledge unknown. A
    // diagnostic may copy inputs of a skipped draw; it must not admit that draw.
    output={p.offsetWords,v[1],v[3],v[2],count,{{},p.predicated}};
    error=DmaTriangleFailure::None; return true;
}

inline bool planDmaTriangleRequest(const Pm4Snapshot& stream, std::size_t packetIndex,
        const DrawBinState& bins, DmaTriangleRequest& output, DmaTriangleFailure& error) noexcept {
    DmaTriangleRequest candidate;
    if(!inspectDmaTriangleLayout(stream,packetIndex,candidate,error))return false;
    if(evaluateDrawBinPredicate(bins,candidate.predicate.predicated)!=DrawBinDecision::Pass) {
        error=DmaTriangleFailure::Predicate;return false;
    }
    candidate.predicate.state=bins;output=candidate;return true;
}

struct StoppedDmaInput {
    std::uint32_t node=0,offsetWords=0;
    DmaTriangleRequest layout;
    DmaTriangleFailure failure=DmaTriangleFailure::None;
    std::vector<std::uint8_t> bytes;
};
struct StoppedDmaInputs {
    std::vector<StoppedDmaInput> inputs;
    std::uint32_t requestedBytes=0,unavailable=0;
};
// Diagnostic inventory of an already validated, owned stopped IB graph. This
// is node/packet storage order, NOT execution order or predicate evaluation.
// Unknown/non-DMA opcodes remain uninterpreted. Each range still goes through
// the caller's stopped-allocation reader. Partial unavailable reads are explicit;
// a global bound/allocation failure preserves the previous output entirely.
inline bool captureStoppedDmaInputs(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
        const Pm4PhysicalReader& reader,StoppedDmaInputs& output) noexcept {
    if(!reader || graph.nodes.size()>64)return false;
    try {
        StoppedDmaInputs result;
        for(std::uint32_t node=0;node<=graph.nodes.size();++node) {
            const auto& stream=node?graph.nodes[node-1].snapshot:root;
            for(std::size_t i=0;i<stream.packets().size();++i) {
                const auto& p=stream.packets()[i];
                if(p.kind!=Pm4Kind::Command || p.opcode!=0x22)continue;
                if(result.inputs.size()==256)return false;
                StoppedDmaInput input;input.node=node;input.offsetWords=p.offsetWords;
                if(inspectDmaTriangleLayout(stream,i,input.layout,input.failure)) {
                    const auto count=input.layout.indexCount*2;
                    if(count>2*1024*1024-result.requestedBytes)return false;
                    result.requestedBytes+=count;
                    try {
                        auto bytes=reader(input.layout.physicalAddress,count);
                        if(bytes && bytes->size()==count)input.bytes=std::move(*bytes);
                        else input.failure=DmaTriangleFailure::ReadRejected;
                    } catch(const std::bad_alloc&) {return false;}
                      catch(...) {input.failure=DmaTriangleFailure::ReaderException;}
                }
                if(input.failure!=DmaTriangleFailure::None)++result.unavailable;
                result.inputs.push_back(std::move(input));
            }
        }
        output=std::move(result);return true;
    } catch(...) {return false;}
}

// Caller must hold the serialized execution token and allocation ownership
// lock, park writers, and provide a read-only bounds-checked physical reader.
// No vertex-range claim: vertex fetch descriptors and shaders are separate
// requirements. No GPU work, DMA-register write, event store or RPTR advance.
// Failure leaves output unchanged; callback exceptions cannot escape.
inline bool captureDmaTriangleInput(const Pm4Snapshot& stream, std::size_t packetIndex,
        const DrawBinState& bins, const Pm4PhysicalReader& reader,
        DmaTriangleInput& output, DmaTriangleFailure& error) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<DmaTriangleInput>);
    DmaTriangleInput candidate;
    if (!planDmaTriangleRequest(stream,packetIndex,bins,candidate.request,error)) return false;
    if (!reader) { error=DmaTriangleFailure::MissingReader; return false; }
    try {
        const auto& r=candidate.request;
        const auto bytes=reader(r.physicalAddress,r.indexCount*2);
        if (!bytes || bytes->size()!=r.indexCount*2) {
            error=DmaTriangleFailure::ReadRejected; return false;
        }
        candidate.indices.reserve(r.indexCount);
        for (std::size_t i=0; i<bytes->size(); i+=2)
            candidate.indices.push_back((std::uint32_t{(*bytes)[i]}<<8)|(*bytes)[i+1]);
        output=std::move(candidate); error=DmaTriangleFailure::None; return true;
    } catch (const std::bad_alloc&) { error=DmaTriangleFailure::AllocationFailure; }
      catch (...) { error=DmaTriangleFailure::ReaderException; }
    return false;
}
}
