#pragma once
#include "xenos_command_stream.h"
#include "xenos_draw_bin_predicate.h"
#include "xenos_indirect_inspection.h"
#include "xenos_register_knowledge.h"
#include <array>
#include <map>
#include <type_traits>
#include <utility>

namespace recomp::gpu {
enum class DrawCommandFailure {
    None, Empty, Limit, Header, RegisterRange, PredicatedState, UnsupportedCommand,
    ShaderLayout, ConstantLayout, DrawLayout, MissingVertex, InitialBins,
    MissingSource, SourceSize, ReaderException, AllocationFailure
};
struct DrawCommandError {
    DrawCommandFailure failure = DrawCommandFailure::None;
    std::uint32_t offsetWords = 0;
};
enum class DrawCommandKind {
    RegisterWrite, RegisterModify, BinWrite, ShaderLoad, ConstantLoad, Draw,
    Invalidate, Wait, Event, EventWord, Nop, Filler
};
constexpr std::uint32_t NoDrawCommandDetail = 0xFFFFFFFF;
struct DrawCommandAction {
    DrawCommandKind kind;
    std::uint32_t offsetWords, priorDraws, detailIndex = NoDrawCommandDetail;
};
struct DrawRegisterCandidate {
    StoppedRegisterKnowledge bits;
    std::uint32_t lastAssignmentOffset = 0;
};
using DrawRegisterCandidates = std::map<std::uint32_t,DrawRegisterCandidate>;
struct DrawShaderCandidate {
    std::uint32_t offsetWords = 0, stage = 0, physicalAddress = 0;
    bool immediate = false;
    std::vector<std::uint8_t> bigEndianCode;
};
struct DrawConstantCandidate {
    std::uint32_t offsetWords = 0, physicalAddress = 0, firstRegister = 0;
    std::vector<std::uint8_t> bigEndianWords;
};
struct DrawCommandCandidate {
    std::uint32_t offsetWords = 0, opcode = 0, indexCount = 0, primitive = 0;
    // Index into shaders, NOT a compiled or admitted native shader.
    std::array<std::uint32_t,2> shaders{{NoDrawCommandDetail,NoDrawCommandDetail}};
    DrawBinPredicate predicate;
    DrawRegisterCandidates registers;
};
struct DrawWaitCandidate {
    std::uint32_t offsetWords = 0, priorDraws = 0;
    DrawRegisterCandidates registers;
};
struct DrawCommandPlan {
    // Own every original packet, including side effects not implemented here.
    // actions is one-to-one with commands.packets(), in unchanged command order.
    Pm4Snapshot commands;
    std::vector<DrawCommandAction> actions;
    std::vector<DrawShaderCandidate> shaders;
    std::vector<DrawConstantCandidate> constants;
    std::vector<DrawCommandCandidate> draws;
    std::vector<DrawWaitCandidate> waits;
    DrawRegisterCandidates finalRegisters;
    DrawBinState finalBins;
};

// Bounded OWNED CANDIDATES for one child stream, not an executable GPU plan.
// No IB expansion: the caller must separately admit the root/child relationship.
// Unknown state stays unknown. Invalidations, register hardware side effects,
// waits, events and even known-Skip draws are retained, NEVER executed/ignored.
// The last loaded shader remains a syntactic candidate across invalidations;
// this is deliberately not a claim that it is still resident on real hardware.
//
// A source reader must enforce allocation bounds and stable contents under the
// caller's ownership lock AND parked-writer execution token. Returned vectors
// are copied/owned here, not future leases. No GPU, guest store or RPTR update.
// All framing and local budgets are checked before the first physical read.
// Failure preserves output; physical reads may already have happened on a
// missing/invalid source. Reader must be read-only and must not reenter guests.
inline bool planDrawCommandCandidates(const Pm4Snapshot& input, const DrawBinState& initialBins,
        const Pm4PhysicalReader& reader, DrawCommandPlan& output, DrawCommandError& error) noexcept {
    constexpr std::size_t MaxWords = 4096, MaxPackets = 512, MaxRegisters = 4096;
    constexpr std::size_t MaxShaderLoads = 8, MaxShaderBytes = 4096;
    constexpr std::size_t MaxConstantLoads = 32, MaxConstantBytes = 8192, MaxDraws = 64, MaxWaits = 64;
    static_assert(std::is_nothrow_move_assignable_v<DrawCommandPlan>);
    const auto fail = [&](DrawCommandFailure why, std::uint32_t at = 0) {
        error = {why,at}; return false;
    };
    if (input.words().empty()) return fail(DrawCommandFailure::Empty);
    if (input.words().size() > MaxWords || input.packets().size() > MaxPackets)
        return fail(DrawCommandFailure::Limit);
    if (evaluateDrawBinPredicate(initialBins,false) == DrawBinDecision::InvalidState)
        return fail(DrawCommandFailure::InitialBins);
    try {
        // Syntax pass: no supplied callback executes here. Deliberately narrower
        // than decodePm4: reserved header bits and unknown control opcodes reject.
        std::array<bool,0x8000> assigned{};
        std::size_t registerCount = 0, shaderCount = 0, shaderBytes = 0;
        std::size_t constantCount = 0, constantBytes = 0, drawCount = 0, waitCount = 0;
        bool vertexLoaded = false;
        std::vector<DrawCommandAction> actions;
        actions.reserve(input.packets().size());
        const auto countRegister = [&](std::uint32_t index, std::uint32_t at) {
            if (index >= assigned.size()) return fail(DrawCommandFailure::RegisterRange,at);
            if (!assigned[index]) {
                if (registerCount == MaxRegisters) return fail(DrawCommandFailure::Limit,at);
                assigned[index] = true; ++registerCount;
            }
            return true;
        };
        for (const auto& p : input.packets()) {
            const auto at = p.offsetWords;
            const auto* v = input.words().data()+at+1;
            std::uint32_t canonical = 0;
            switch (p.kind) {
            case Pm4Kind::RegisterSequence:
                canonical = ((p.payloadWords-1) << 16) | p.firstRegister | (p.repeatRegister ? 0x8000 : 0); break;
            case Pm4Kind::RegisterPair: canonical = 0x40000000 | p.firstRegister | (p.secondRegister << 11); break;
            case Pm4Kind::Filler: canonical = 0x80000000; break;
            case Pm4Kind::Command:
                canonical = 0xC0000000 | ((p.payloadWords-1) << 16) | (p.opcode << 8) | unsigned(p.predicated); break;
            default: return fail(DrawCommandFailure::Header,at);
            }
            if (input.words()[at] != canonical) return fail(DrawCommandFailure::Header,at);
            DrawCommandAction action{DrawCommandKind::Filler,at,static_cast<std::uint32_t>(drawCount)};
            if (p.kind == Pm4Kind::RegisterSequence || p.kind == Pm4Kind::RegisterPair) {
                action.kind = DrawCommandKind::RegisterWrite;
                for (std::uint32_t i = 0; i < p.payloadWords; ++i) {
                    const auto index = p.kind == Pm4Kind::RegisterPair ? (i ? p.secondRegister : p.firstRegister) :
                        p.firstRegister + (p.repeatRegister ? 0 : i);
                    if (!countRegister(index,at)) return false;
                }
            } else if (p.kind == Pm4Kind::Command) {
                if (p.predicated && p.opcode != 0x22 && p.opcode != 0x36)
                    return fail(DrawCommandFailure::PredicatedState,at);
                switch (p.opcode) {
                case 0x10: action.kind = DrawCommandKind::Nop; break;
                case 0x60: case 0x61: case 0x62: case 0x63:
                    if (p.payloadWords != 1) return fail(DrawCommandFailure::UnsupportedCommand,at);
                    action.kind = DrawCommandKind::BinWrite; break;
                case 0x21:
                    if (p.payloadWords != 3 || (v[0]&~0x1FFFu)) return fail(DrawCommandFailure::UnsupportedCommand,at);
                    if (!countRegister(v[0],at)) return false;
                    action.kind = DrawCommandKind::RegisterModify; break;
                case 0x27: case 0x2B: {
                    if (p.payloadWords < 2) return fail(DrawCommandFailure::ShaderLayout,at);
                    const bool immediate = p.opcode == 0x2B;
                    const auto stage = immediate ? v[0] : v[0]&3;
                    if (stage > 1 || !v[1] || v[1] > 1024 || v[1]%3 ||
                        p.payloadWords != (immediate ? v[1]+2 : 2) ||
                        (!immediate && std::uint64_t{v[0]&~3u}+v[1]*4 > 0x20000000))
                        return fail(DrawCommandFailure::ShaderLayout,at);
                    if (shaderCount == MaxShaderLoads || v[1]*4 > MaxShaderBytes-shaderBytes)
                        return fail(DrawCommandFailure::Limit,at);
                    action.kind = DrawCommandKind::ShaderLoad;
                    action.detailIndex = static_cast<std::uint32_t>(shaderCount++);
                    shaderBytes += v[1]*4; vertexLoaded |= stage == 0; break;
                }
                case 0x2F:
                    if (p.payloadWords != 3 || (v[0]&3) || (v[1]&~0x7FFu) ||
                        !v[2] || v[2] > 2048 || v[1]+v[2] > 2048 ||
                        std::uint64_t{v[0]}+v[2]*4 > 0x20000000)
                        return fail(DrawCommandFailure::ConstantLayout,at);
                    if (constantCount == MaxConstantLoads || v[2]*4 > MaxConstantBytes-constantBytes)
                        return fail(DrawCommandFailure::Limit,at);
                    for (std::uint32_t i = 0; i < v[2]; ++i) if (!countRegister(0x4000+v[1]+i,at)) return false;
                    action.kind = DrawCommandKind::ConstantLoad;
                    action.detailIndex = static_cast<std::uint32_t>(constantCount++);
                    constantBytes += v[2]*4; break;
                case 0x22: case 0x36: {
                    const unsigned prefix = p.opcode == 0x22 ? 1 : 0;
                    if (p.payloadWords != prefix+1 || (prefix && v[0])) return fail(DrawCommandFailure::DrawLayout,at);
                    const auto control = v[prefix];
                    if ((control&0xFF00) || ((control>>6)&3) != 2 || !(control>>16) || (control>>16) > 4096)
                        return fail(DrawCommandFailure::DrawLayout,at);
                    if (!vertexLoaded) return fail(DrawCommandFailure::MissingVertex,at);
                    if (drawCount == MaxDraws) return fail(DrawCommandFailure::Limit,at);
                    action.kind = DrawCommandKind::Draw;
                    action.detailIndex = static_cast<std::uint32_t>(drawCount++); break;
                }
                case 0x3B: case 0x3C: case 0x46: case 0x58: {
                    const auto count = p.opcode == 0x3C ? 5u : (p.opcode == 0x58 ? 3u : 1u);
                    if (p.payloadWords != count) return fail(DrawCommandFailure::UnsupportedCommand,at);
                    action.kind = p.opcode == 0x3B ? DrawCommandKind::Invalidate :
                        (p.opcode == 0x3C ? DrawCommandKind::Wait :
                        (p.opcode == 0x46 ? DrawCommandKind::Event : DrawCommandKind::EventWord));
                    if (p.opcode == 0x3C) {
                        if (waitCount == MaxWaits) return fail(DrawCommandFailure::Limit,at);
                        action.detailIndex = static_cast<std::uint32_t>(waitCount++);
                    }
                    break;
                }
                default: return fail(DrawCommandFailure::UnsupportedCommand,at);
                }
            }
            actions.push_back(action);
        }
        if (!drawCount) return fail(DrawCommandFailure::Empty);
        DrawCommandPlan candidate;
        candidate.commands = input;
        candidate.actions = std::move(actions);
        candidate.shaders.reserve(shaderCount); candidate.constants.reserve(constantCount);
        candidate.draws.reserve(drawCount); candidate.waits.reserve(waitCount);
        candidate.finalBins = initialBins;
        std::array<std::uint32_t,2> shaders{{NoDrawCommandDetail,NoDrawCommandDetail}};
        const auto read = [&](std::uint32_t address, std::uint32_t size, std::uint32_t at,
                              std::vector<std::uint8_t>& bytes) {
            if (!reader) return fail(DrawCommandFailure::MissingSource,at);
            std::optional<std::vector<std::uint8_t>> fetched;
            try { fetched = reader(address,size); }
            catch (const std::bad_alloc&) { return fail(DrawCommandFailure::AllocationFailure,at); }
            catch (...) { return fail(DrawCommandFailure::ReaderException,at); }
            if (!fetched) return fail(DrawCommandFailure::MissingSource,at);
            if (fetched->size() != size) return fail(DrawCommandFailure::SourceSize,at);
            bytes = std::move(*fetched); return true;
        };
        for (std::size_t n = 0; n < candidate.actions.size(); ++n) {
            const auto& a = candidate.actions[n];
            const auto& p = input.packets()[n];
            const auto* v = input.words().data()+a.offsetWords+1;
            const auto assign = [&](std::uint32_t index, std::uint32_t value) {
                auto& reg = candidate.finalRegisters[index];
                reg.bits.assign(value); reg.lastAssignmentOffset = a.offsetWords;
            };
            switch (a.kind) {
            case DrawCommandKind::RegisterWrite:
                for (std::uint32_t i = 0; i < p.payloadWords; ++i)
                    assign(p.kind == Pm4Kind::RegisterPair ? (i ? p.secondRegister : p.firstRegister) :
                        p.firstRegister+(p.repeatRegister ? 0 : i),v[i]);
                break;
            case DrawCommandKind::RegisterModify: {
                auto& reg = candidate.finalRegisters[v[0]];
                reg.bits.modify(v[1],v[2]); reg.lastAssignmentOffset = a.offsetWords; break;
            }
            case DrawCommandKind::BinWrite:
                candidate.finalBins.words[p.opcode-0x60] = v[0];
                candidate.finalBins.knownWords |= 1u << (p.opcode-0x60); break;
            case DrawCommandKind::ShaderLoad: {
                DrawShaderCandidate shader;
                shader.offsetWords = a.offsetWords; shader.immediate = p.opcode == 0x2B;
                shader.stage = shader.immediate ? v[0] : v[0]&3;
                if (shader.immediate) {
                    shader.bigEndianCode.reserve(v[1]*4);
                    for (std::uint32_t i = 0; i < v[1]; ++i) for (unsigned lane = 0; lane < 4; ++lane)
                        shader.bigEndianCode.push_back(v[i+2] >> (24-lane*8));
                } else {
                    shader.physicalAddress = v[0]&~3u;
                    if (!read(shader.physicalAddress,v[1]*4,a.offsetWords,shader.bigEndianCode)) return false;
                }
                shaders[shader.stage] = a.detailIndex;
                candidate.shaders.push_back(std::move(shader)); break;
            }
            case DrawCommandKind::ConstantLoad: {
                DrawConstantCandidate constant;
                constant.offsetWords = a.offsetWords; constant.physicalAddress = v[0]; constant.firstRegister = 0x4000+v[1];
                if (!read(v[0],v[2]*4,a.offsetWords,constant.bigEndianWords)) return false;
                for (std::uint32_t i = 0; i < v[2]; ++i) {
                    const auto* b = constant.bigEndianWords.data()+i*4;
                    assign(constant.firstRegister+i,(std::uint32_t{b[0]}<<24) | (std::uint32_t{b[1]}<<16) |
                        (std::uint32_t{b[2]}<<8) | b[3]);
                }
                candidate.constants.push_back(std::move(constant)); break;
            }
            case DrawCommandKind::Draw: {
                const auto control = v[p.opcode == 0x22 ? 1 : 0];
                candidate.draws.push_back({a.offsetWords,p.opcode,control>>16,control&63,shaders,
                    {candidate.finalBins,p.predicated},candidate.finalRegisters}); break;
            }
            case DrawCommandKind::Wait:
                candidate.waits.push_back({a.offsetWords,a.priorDraws,candidate.finalRegisters}); break;
            // These obligations survive verbatim in commands/actions. In
            // particular, no busy bit is cleared, event written, shader reset,
            // cache flushed or draw retired by this candidate walk.
            default: break;
            }
        }
        output = std::move(candidate); error = {}; return true;
    } catch (const std::bad_alloc&) { return fail(DrawCommandFailure::AllocationFailure); }
}
}
