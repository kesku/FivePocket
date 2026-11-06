#pragma once
#include "xenos_indirect_inspection.h"
#include "xenos_scratch_writeback.h"
#include <map>

namespace recomp::gpu {
enum class StoppedMemoryFailure : std::uint32_t {
    None, InvalidGraph, Limit, UnsupportedControl, UnknownScratchState,
    InvalidAddress, UnsupportedEvent, AllocationFailure, UnsupportedScratchMutation
};
struct StoppedMemoryError {
    StoppedMemoryFailure failure = StoppedMemoryFailure::None;
    std::uint32_t node = 0, offsetWords = 0;
};
struct StoppedMemoryWrite {
    std::uint32_t node, offsetWords, kind, physicalAddress, value, priorDrawRequests;
    bool bigEndianProfile = false;
    // kind 0: enabled scratch low-bit writeback request; kind 1: literal
    // PS_DONE_TS event. Four bytes, never written here. Event endianness is
    // validated as BE; scratch supports only the source-corroborated mode-2
    // profile. Other scratch swap controls remain unmodeled.
};
struct StoppedMemoryEffects {
    std::vector<StoppedMemoryWrite> writes;
    std::uint32_t unmodeledScratchMaskBits = 0, otherRegisterWrites = 0, drawRequests = 0;
};
// Read-only request inspection, not an executable or complete memory-effect
// plan. Unsupported SCRATCH_UMSK profiles, other register side effects and
// shader memory effects remain unmodeled. Required state must occur here.
// Only exact unpredicated IBs expand; unsupported control/memory opcodes reject
// the entire inspection. Ownership/fencing/alias validation happens elsewhere.
inline bool inspectStoppedMemoryEffects(const Pm4Snapshot& root, const Pm4IndirectGraph& graph,
        StoppedMemoryEffects& output, StoppedMemoryError& error) {
    const auto fail = [&](StoppedMemoryFailure why, std::uint32_t node = 0, std::uint32_t at = 0) {
        error = {why,node,at}; return false;
    };
    if (graph.nodes.size() > 64) return fail(StoppedMemoryFailure::Limit);
    try {
        StoppedMemoryEffects candidate;
        std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> children;
        std::size_t words = root.words().size(), packets = root.packets().size();
        for (std::uint32_t i = 0; i < graph.nodes.size(); ++i) {
            const auto& n = graph.nodes[i];
            if (n.parent > i || !n.depth || n.depth > 8 || n.snapshot.words().empty() ||
                n.snapshot.words().size() > 0xFFFFF || (n.physicalAddress & 3) ||
                std::uint64_t{n.physicalAddress} + n.snapshot.words().size() * 4 > 0x20000000 ||
                !children.emplace(std::make_pair(n.parent,n.parentOffsetWords),i+1).second)
                return fail(StoppedMemoryFailure::InvalidGraph,i+1);
            const auto& parent = n.parent ? graph.nodes[n.parent-1].snapshot : root;
            const auto depth = n.parent ? graph.nodes[n.parent-1].depth+1 : 1;
            if (n.depth != depth || std::uint64_t{n.parentOffsetWords}+3 > parent.words().size())
                return fail(StoppedMemoryFailure::InvalidGraph,i+1);
            words += n.snapshot.words().size(); packets += n.snapshot.packets().size();
        }
        if (words > 1048576 || packets > 65536) return fail(StoppedMemoryFailure::Limit);
        if (words != graph.totalWords || packets != graph.totalPackets) return fail(StoppedMemoryFailure::InvalidGraph);
        std::optional<std::uint32_t> scratchBase, scratchMask;
        std::size_t visited = 0;
        const auto visit = [&](auto&& self, const Pm4Snapshot& snapshot, std::uint32_t id) -> bool {
            const auto request = [&](std::uint32_t at, std::uint32_t kind, std::uint64_t address, std::uint32_t value) {
                if ((address & 3) || address + 4 > 0x20000000) return fail(StoppedMemoryFailure::InvalidAddress,id,at);
                if (candidate.writes.size() == 64) return fail(StoppedMemoryFailure::Limit,id,at);
                candidate.writes.push_back({id,at,kind,static_cast<std::uint32_t>(address),value,candidate.drawRequests,
                    kind == 1 || (scratchMask && scratchWritebackBigEndianProfile(*scratchMask))});
                return true;
            };
            const auto writeRegister = [&](std::uint32_t at, std::uint32_t index, std::uint32_t value) {
                if (index == 0x1DD) {
                    if ((value & 3) || value >= 0x20000000) return fail(StoppedMemoryFailure::InvalidAddress,id,at);
                    scratchBase = value;
                } else if (index == 0x1DC) {
                    scratchMask = value;
                    if (!scratchWritebackBigEndianProfile(value)) candidate.unmodeledScratchMaskBits |= value & ~255u;
                } else if (index >= 0x578 && index <= 0x57F) {
                    if (!scratchMask) return fail(StoppedMemoryFailure::UnknownScratchState,id,at);
                    const auto slot = index - 0x578;
                    if (*scratchMask & (1u << slot)) {
                        if (!scratchBase) return fail(StoppedMemoryFailure::UnknownScratchState,id,at);
                        return request(at,0,std::uint64_t{*scratchBase}+slot*4,value);
                    }
                } else ++candidate.otherRegisterWrites;
                return true;
            };
            for (const auto& p : snapshot.packets()) {
                const auto at = p.offsetWords;
                if (p.predicated) return fail(StoppedMemoryFailure::UnsupportedControl,id,at);
                const auto* payload = snapshot.words().data()+at+1;
                if (p.kind == Pm4Kind::RegisterSequence || p.kind == Pm4Kind::RegisterPair) {
                    for (std::uint32_t i = 0; i < p.payloadWords; ++i) {
                        const auto index = p.kind == Pm4Kind::RegisterPair ? (i ? p.secondRegister : p.firstRegister) :
                            p.firstRegister + (p.repeatRegister ? 0 : i);
                        if (!writeRegister(at,index,payload[i])) return false;
                    }
                    continue;
                }
                if (p.kind == Pm4Kind::Filler) continue;
                if (p.kind != Pm4Kind::Command) return fail(StoppedMemoryFailure::UnsupportedControl,id,at);
                switch (p.opcode) {
                case 0x3F: case 0x37: {
                    const auto child = children.find({id,at});
                    if (p.payloadWords != 2 || child == children.end()) return fail(StoppedMemoryFailure::InvalidGraph,id,at);
                    const auto& n = graph.nodes[child->second-1];
                    if (n.physicalAddress != payload[0] || n.snapshot.words().size() != payload[1])
                        return fail(StoppedMemoryFailure::InvalidGraph,id,at);
                    ++visited;
                    if (!self(self,n.snapshot,child->second)) return false;
                    break;
                }
                case 0x21: {
                    if (p.payloadWords != 3 || (payload[0] & ~0x1FFFu)) return fail(StoppedMemoryFailure::UnsupportedControl,id,at);
                    const auto index = payload[0];
                    if (index == 0x1DC || index == 0x1DD || (index >= 0x578 && index <= 0x57F))
                        return fail(StoppedMemoryFailure::UnsupportedScratchMutation,id,at);
                    ++candidate.otherRegisterWrites; break;
                }
                case 0x58:
                    if (p.payloadWords != 3 || payload[0] != 3 || (payload[1] & 3) != 2)
                        return fail(StoppedMemoryFailure::UnsupportedEvent,id,at);
                    if (!request(at,1,payload[1]&~3u,payload[2])) return false;
                    break;
                case 0x36:
                    if (p.payloadWords != 1 || candidate.drawRequests == 64)
                        return fail(StoppedMemoryFailure::Limit,id,at);
                    ++candidate.drawRequests; break;
                case 0x10: case 0x27: case 0x2B: case 0x3B:
                    break; // These effects are deliberately outside this inspector.
                default: return fail(StoppedMemoryFailure::UnsupportedControl,id,at);
                }
            }
            return true;
        };
        if (!visit(visit,root,0)) return false;
        if (visited != graph.nodes.size()) return fail(StoppedMemoryFailure::InvalidGraph);
        output = std::move(candidate); error = {}; return true;
    } catch (const std::bad_alloc&) { return fail(StoppedMemoryFailure::AllocationFailure); }
}
}
