#pragma once
#include "xenos_stopped_constant_initialization.h"
#include "native_constant_point_inputs.h"
#include "native_gpu_word_readback.h"

namespace recomp::gpu {
// EXPERIMENTAL native replacement of the source-corroborated cold Xenos
// initialization sequence. This is NOT a claim of physical register fidelity.
// Known scratch/events become ordered native writes, constant masked points
// become real draws, WAIT_UNTIL becomes synchronous queue completion, and the
// state-pointer caches are rebuilt from the captured inline programs.
// Legacy SQ/EDRAM setup controls below are retained, not sent to an Adreno GPU.
// The unresolved hardware-equivalence question is documented in docs/fable/
// blockers/2026-09-06-second-gpu-batch/FOCUSED_QUESTION.md. No general register
// fallback, guessed reset values, interrupt, or later batch is authorized.
struct NativeInitializationReadback {
    std::array<std::uint8_t,256> bytes{};
    std::uint32_t byteCount = 0, draws = 0;
};
struct NativeInitializationError {
    std::uint32_t node = 0, offsetWords = 0, registerIndex = 0;
};
inline bool nativeInitializationRegisterValue(std::uint32_t index, std::uint32_t value) noexcept {
    switch (index) {
    case 0x1DD: return !(value & 3) && value < 0x20000000;
    case 0x1DC: return scratchWritebackBigEndianProfile(value);
    case 0x578: case 0x579: case 0x57A: case 0x57B:
    case 0x57C: case 0x57D: case 0x57E: case 0x57F: return true;
    case 0x5C8: return value == 0x20000;
    case 0xA02: return value == 0xC0100000;
    case 0xA03: return value == 0x07F00000;
    case 0xA04: return value == 0xC0000000;
    case 0xA05: return value == 0x00100000;
    case 0xC85: return value == 3;
    case 0xD00: return value == 0x40400 || value == 0x40401;
    case 0xD01: return value == 0x04000000;
    case 0xD02: return value == 0x10800;
    case 0xD04: return value == 0;
    case 0xE42: return value == 0x1F60;
    case 0xF01: return value == 0x200E;
    case 0x200D: case 0x2080: case 0x2081: case 0x2101: case 0x2102:
    case 0x2104: case 0x2181: case 0x2200: case 0x2203: case 0x2293: return value == 0;
    case 0x2082: return value == 0 || value == 0x100010;
    case 0x2100: case 0x2312: return value == 0xFFFF;
    case 0x2180: return value == 0x1000000E;
    case 0x2204: case 0x2205: return value == 0x10000;
    case 0x2206: return value == 0x300;
    case 0x2208: case 0x2302: return value == 4;
    case 0x2280: return value == 0x80008;
    default: return false;
    }
}
inline bool planExperimentalNativeInitialization(const Pm4Snapshot& root, const Pm4IndirectGraph& graph,
        StoppedConstantInitialization& output, NativeInitializationError& error) {
    StoppedConstantInitialization candidate; StoppedConstantError inspected;
    if (!inspectStoppedConstantInitialization(root,graph,candidate,inspected)) {
        error = {inspected.node,inspected.offsetWords,0}; return false;
    }
    // Every register write is checked at its execution site, including an
    // unsupported value overwritten later. A final-state map is insufficient.
    const auto check = [&](const Pm4Snapshot& snapshot, std::uint32_t node) {
        for (const auto& p : snapshot.packets()) {
            const auto* data = snapshot.words().data()+p.offsetWords+1;
            if (p.kind == Pm4Kind::RegisterSequence || p.kind == Pm4Kind::RegisterPair) {
                for (std::uint32_t i = 0; i < p.payloadWords; ++i) {
                    const auto index = p.kind == Pm4Kind::RegisterPair ? (i ? p.secondRegister : p.firstRegister) :
                        p.firstRegister+(p.repeatRegister ? 0 : i);
                    if (!nativeInitializationRegisterValue(index,data[i])) {
                        error = {node,p.offsetWords,index}; return false;
                    }
                }
            } else if (p.kind == Pm4Kind::Command && p.opcode == 0x21) {
                if (data[1] != 0xFFFFFFFF || !((data[0] == 0x81 && data[2] == 0x80010000) ||
                        (data[0] == 0x82 && data[2] == 0))) {
                    error = {node,p.offsetWords,data[0]}; return false;
                }
            }
        }
        return true;
    };
    if (!check(root,0)) return false;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
        if (!check(graph.nodes[i].snapshot,static_cast<std::uint32_t>(i+1))) return false;
    if (candidate.draws.empty() || candidate.draws.size() > 64 || candidate.memory.updates.bufferBytes > 256)
        return false;
    for (const auto& draw : candidate.draws) {
        NativeConstantPointInputs inputs;
        if (draw.pair.position != std::array<std::uint8_t,4>{0,0,0,1} ||
            !packNativeConstantPointInputs(draw.pair,inputs)) return false;
    }
    output = std::move(candidate); error = {}; return true;
}
}
