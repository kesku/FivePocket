#pragma once
#include "xenos_copy_shader.h"
#include "xenos_stopped_word_staging.h"
#include "xenos_register_knowledge.h"

namespace recomp::gpu {
enum class StoppedConstantFailure : std::uint32_t {
    None, MemoryInspection, WordStaging, RegisterLimit, ShaderLayout, ShaderProof,
    DrawLayout, MissingDrawState, DrawState, MissingShader, ShaderLinkage, UnsupportedControl,
    DrawCount, AllocationFailure
};
struct StoppedConstantError {
    StoppedConstantFailure failure = StoppedConstantFailure::None;
    std::uint32_t node = 0, offsetWords = 0;
};
struct StoppedConstantDraw {
    std::uint32_t node = 0, offsetWords = 0;
    ConstantShaderPair pair;
};
struct StoppedConstantInitialization {
    StoppedWordStaging memory;
    std::vector<StoppedConstantDraw> draws;
    std::map<std::uint32_t,StoppedRegisterKnowledge> registers;
    std::uint32_t shaderLoads = 0, invalidations = 0, otherRegisterWrites = 0;
};

// Join the ordered shader/state and memory subplans. This is stopped inspection
// ONLY: retained hardware registers are not implemented effects, a synthetic
// location-0 linkage is not title raster replay, and snapshots are not future
// guest write authority. No native or guest command is recorded here.
// Memory inspection prevalidates complete graph framing/control/child mapping;
// the second walk preserves actual parent/IB/return order for draw dependencies.
inline bool inspectStoppedConstantInitialization(const Pm4Snapshot& root, const Pm4IndirectGraph& graph,
        StoppedConstantInitialization& output, StoppedConstantError& error) {
    const auto fail = [&](StoppedConstantFailure why, std::uint32_t node = 0, std::uint32_t at = 0) {
        error = {why,node,at}; return false;
    };
    try {
        StoppedMemoryEffects effects; StoppedMemoryError memoryError;
        if (!inspectStoppedMemoryEffects(root,graph,effects,memoryError))
            return fail(StoppedConstantFailure::MemoryInspection,memoryError.node,memoryError.offsetWords);
        StoppedConstantInitialization candidate;
        if (!planStoppedWordStaging(effects,candidate.memory)) return fail(StoppedConstantFailure::WordStaging);
        candidate.otherRegisterWrites = effects.otherRegisterWrites;
        std::array<std::optional<CopyShaderProof>,2> shaders;
        std::size_t shaderBytes = 0;
        constexpr std::array<std::array<std::uint32_t,2>,7> required{{
            {{0x2180,0x1000000E}},{{0x2181,0}},{{0x2104,0}},{{0x2200,0}},
            {{0x2208,4}},{{0x2293,0}},{{0x2280,0x80008}}}};
        unsigned freshDrawRegisters = 0;
        const auto visit = [&](auto&& self, const Pm4Snapshot& snapshot, std::uint32_t id) -> bool {
            const auto reg = [&](std::uint32_t index) -> StoppedRegisterKnowledge* {
                const auto found = candidate.registers.find(index);
                if (found != candidate.registers.end()) return &found->second;
                if (candidate.registers.size() == 1024) return nullptr;
                return &candidate.registers[index];
            };
            for (const auto& packet : snapshot.packets()) {
                const auto at = packet.offsetWords;
                const auto* payload = snapshot.words().data()+at+1;
                if (packet.kind == Pm4Kind::RegisterSequence || packet.kind == Pm4Kind::RegisterPair) {
                    for (std::uint32_t i = 0; i < packet.payloadWords; ++i) {
                        const auto index = packet.kind == Pm4Kind::RegisterPair ? (i ? packet.secondRegister : packet.firstRegister) :
                            packet.firstRegister + (packet.repeatRegister ? 0 : i);
                        auto* state = reg(index);
                        if (!state) return fail(StoppedConstantFailure::RegisterLimit,id,at);
                        state->assign(payload[i]);
                        for (unsigned field = 0; field < required.size(); ++field)
                            if (index == required[field][0]) freshDrawRegisters |= 1u << field;
                    }
                    continue;
                }
                if (packet.kind == Pm4Kind::Filler) continue;
                if (packet.kind != Pm4Kind::Command || packet.predicated)
                    return fail(StoppedConstantFailure::UnsupportedControl,id,at);
                switch (packet.opcode) {
                case 0x37: case 0x3F: {
                    std::uint32_t child = 0;
                    while (child < graph.nodes.size() && (graph.nodes[child].parent != id || graph.nodes[child].parentOffsetWords != at)) ++child;
                    if (child == graph.nodes.size()) return fail(StoppedConstantFailure::MemoryInspection,id,at);
                    if (!self(self,graph.nodes[child].snapshot,child+1)) return false;
                    break;
                }
                case 0x21: {
                    // The memory pass already requires immediate 13-bit RMW.
                    auto* state = reg(payload[0]);
                    if (!state) return fail(StoppedConstantFailure::RegisterLimit,id,at);
                    state->modify(payload[1],payload[2]); break;
                }
                case 0x2B: {
                    if (packet.payloadWords < 2 || payload[0] > 1 || !payload[1] || payload[1] % 3 ||
                        payload[1] != packet.payloadWords-2 || payload[1] > 1024 || candidate.shaderLoads == 8 ||
                        payload[1]*4 > 4096-shaderBytes) return fail(StoppedConstantFailure::ShaderLayout,id,at);
                    std::array<std::uint8_t,4096> bytes{};
                    for (std::uint32_t i = 0; i < payload[1]; ++i) for (unsigned lane = 0; lane < 4; ++lane)
                        bytes[i*4+lane] = payload[i+2] >> (24-lane*8);
                    CopyShaderProof proof; CopyShaderError proofError;
                    if (!analyzeCopyShader(bytes.data(),payload[1]*4,static_cast<CopyShaderStage>(payload[0]),proof,proofError))
                        return fail(StoppedConstantFailure::ShaderProof,id,at);
                    shaders[payload[0]] = proof; shaderBytes += payload[1]*4; ++candidate.shaderLoads;
                    break;
                }
                case 0x36: {
                    if (packet.payloadWords != 1 || payload[0] != 0x10081 || candidate.draws.size() == 64)
                        return fail(StoppedConstantFailure::DrawLayout,id,at);
                    // Only the already tested masked, depth/query-disabled
                    // one-point profile. These are required full values, not
                    // default-zero reads from a sparse register file.
                    if (freshDrawRegisters != 127) return fail(StoppedConstantFailure::MissingDrawState,id,at);
                    for (const auto& item : required) {
                        const auto found = candidate.registers.find(item[0]);
                        if (found == candidate.registers.end() || found->second.known != 0xFFFFFFFF)
                            return fail(StoppedConstantFailure::MissingDrawState,id,at);
                        if (!found->second.equals(item[1])) return fail(StoppedConstantFailure::DrawState,id,at);
                    }
                    if (!shaders[0] || !shaders[1]) return fail(StoppedConstantFailure::MissingShader,id,at);
                    StoppedConstantDraw draw{id,at,{}};
                    if (!linkConstantShaderPair(*shaders[0],*shaders[1],0x1000000E,0,draw.pair))
                        return fail(StoppedConstantFailure::ShaderLinkage,id,at);
                    candidate.draws.push_back(draw); break;
                }
                case 0x3B:
                    if (packet.payloadWords != 1 || (payload[0] != 0x300 && payload[0] != 0x7FFF))
                        return fail(StoppedConstantFailure::UnsupportedControl,id,at);
                    ++candidate.invalidations;
                    // Conservative invalidation of our analysis cache: subsequent
                    // draws must reload both programs. The broader captured mask
                    // also requires seven fresh draw-state writes before another
                    // proof. Retain the register-write journal, not guessed reset
                    // values. This does NOT implement either GPU invalidation.
                    if (payload[0] == 0x7FFF) freshDrawRegisters = 0;
                    shaders = {}; break;
                case 0x10: case 0x58: break; // NOP or already inspected literal memory request.
                default: return fail(StoppedConstantFailure::UnsupportedControl,id,at);
                }
            }
            return true;
        };
        if (!visit(visit,root,0)) return false;
        if (candidate.draws.size() != effects.drawRequests) return fail(StoppedConstantFailure::DrawCount);
        output = std::move(candidate); error = {}; return true;
    } catch (const std::bad_alloc&) { return fail(StoppedConstantFailure::AllocationFailure); }
}
}
