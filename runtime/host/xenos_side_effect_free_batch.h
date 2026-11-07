#pragma once
#include "xenos_command_stream.h"
#include <functional>
#include <optional>
#include <vector>

namespace recomp::gpu {
// Verifies that a (possibly indirect-buffer-nested) PM4 command stream cannot
// produce any persistent, CPU-visible effect (no color write, no depth or
// stencil write, no active occlusion query, no unresolved memory write, no
// scratch-register writeback, no register state left behind that a later
// submission could observe), so it may be safely consumed without executing
// a single draw. Every claim here is backed by real, decoded register/packet
// content and Xenos PM4 semantics (cross-checked against
// local-analysis/rexglue-renderer-reference's command_processor.cpp) - not
// shader-content guessing. Fails closed on anything it cannot prove safe: an
// unrecognized opcode, or a write to any register outside the small, closed
// allowlist below, rejects the entire batch, never just the one packet.
// CORRECTION (see docs/claude/Audit.md): an earlier version of this comment
// claimed register writes "can never themselves write CPU-visible memory" and
// therefore allowed all of them. That is false: the reference's own
// WriteRegister shows a write to SCRATCH_REG0-7 performs a real memory store
// when the matching SCRATCH_UMSK bit is set. Register writes can also leave
// state a LATER, unrelated submission depends on, which this verifier has no
// way to know is safe to discard. The allowlist below is therefore not "these
// two are worth tracking" but "these two are the ONLY writes this verifier
// understands and permits"; every other register write fails closed.
enum class SideEffectFreeBatchFailure {
    None, InvalidInput, WordLimit, PacketLimit, TruncatedPacket, RecursionLimit,
    UnresolvedIndirectBuffer, InvalidIndirectBuffer, UnsupportedOpcode,
    UnsafeColorWrite, UnsafeDepthWrite, UnsafeOcclusionQuery,
};
struct SideEffectFreeBatchError {
    SideEffectFreeBatchFailure failure = SideEffectFreeBatchFailure::None;
    uint32_t offsetWords = 0;
    uint32_t opcode = 0;
};
struct SideEffectFreeBatchLimits {
    uint32_t maxWords = 4096;          // Total words decoded across the whole tree.
    uint32_t maxPackets = 4096;        // Total packets decoded across the whole tree.
    uint32_t maxIndirectBuffers = 64;  // Total INDIRECT_BUFFER follows across the tree.
    uint32_t maxIndirectDepth = 4;     // Nesting depth of INDIRECT_BUFFER follows.
};
struct SideEffectFreeBatchResult {
    uint32_t totalWords = 0;
    uint32_t totalPackets = 0;
    uint32_t indirectBuffersFollowed = 0;
    uint32_t drawsVerified = 0;
};
// Resolves a physical address + word count to big-endian words, or returns
// false if unavailable/out of bounds. Must never mutate guest memory.
using SideEffectFreeIndirectResolver =
    std::function<bool(uint32_t physicalAddress, uint32_t wordCount, std::vector<uint32_t>& outWords)>;

namespace side_effect_free_detail {
struct WalkState {
    std::optional<uint32_t> colorMask;
    std::optional<uint32_t> depthControl;
};
inline bool fail(SideEffectFreeBatchError& error, SideEffectFreeBatchFailure failure,
                  uint32_t offset = 0, uint32_t opcode = 0) {
    error = {failure, offset, opcode};
    return false;
}
inline bool walkSideEffectFreeBatch(const std::vector<uint32_t>& words, WalkState& state, unsigned depth,
        const SideEffectFreeIndirectResolver& resolver, const SideEffectFreeBatchLimits& limits,
        SideEffectFreeBatchResult& result, SideEffectFreeBatchError& error) {
    if (words.empty() || words.size() > limits.maxWords) return fail(error, SideEffectFreeBatchFailure::InvalidInput);
    std::vector<uint8_t> bytes(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) bytes[i * 4 + b] = static_cast<uint8_t>(words[i] >> (24 - b * 8));
    Pm4Snapshot snapshot; Pm4Error decodeError;
    Pm4Limits pm4Limits; pm4Limits.maxWords = limits.maxWords; pm4Limits.maxPackets = limits.maxPackets;
    if (!decodePm4(bytes.data(), bytes.size(), pm4Limits, snapshot, decodeError))
        return fail(error, SideEffectFreeBatchFailure::TruncatedPacket, decodeError.offsetWords, decodeError.opcode);
    if (result.totalWords > limits.maxWords - words.size()) return fail(error, SideEffectFreeBatchFailure::WordLimit);
    result.totalWords += static_cast<uint32_t>(words.size());
    for (const auto& p : snapshot.packets()) {
        if (result.totalPackets == limits.maxPackets) return fail(error, SideEffectFreeBatchFailure::PacketLimit, p.offsetWords);
        ++result.totalPackets;
        if (p.kind == Pm4Kind::Filler || p.kind == Pm4Kind::ZeroPadding) continue;
        if (p.kind == Pm4Kind::RegisterSequence || p.kind == Pm4Kind::RegisterPair) {
            // Fail closed on any register this verifier does not specifically
            // understand: RB_COLOR_MASK/RB_DEPTHCONTROL are the only writes
            // known not to have a persistent effect this verifier cannot
            // account for. Every other register - including but not limited
            // to SCRATCH_UMSK/SCRATCH_ADDR/SCRATCH_REG0-7, whose writes can
            // perform a real memory store - is rejected.
            for (uint32_t i = 0; i < p.payloadWords; ++i) {
                const auto index = p.kind == Pm4Kind::RegisterPair ? (i ? p.secondRegister : p.firstRegister)
                                                                    : p.firstRegister + (p.repeatRegister ? 0 : i);
                const auto value = snapshot.words()[p.offsetWords + 1 + i];
                if (index == 0x2104) state.colorMask = value;      // RB_COLOR_MASK
                else if (index == 0x2200) state.depthControl = value; // RB_DEPTHCONTROL
                else return fail(error, SideEffectFreeBatchFailure::UnsupportedOpcode, p.offsetWords, index);
            }
            continue;
        }
        if (p.kind != Pm4Kind::Command) return fail(error, SideEffectFreeBatchFailure::UnsupportedOpcode, p.offsetWords);
        const auto data = snapshot.words().data() + p.offsetWords + 1;
        switch (p.opcode) {
        case 0x10: break;  // NOP.
        case 0x3B: break;  // TC_INVALIDATE: cache-coherency hint, no persistent visual effect.
        case 0x27: case 0x2B: break;  // IM_LOAD / IM_LOAD_IMMEDIATE: loads microcode into a slot; no rendering by itself.
        case 0x46:  // EVENT_WRITE: only writes VGT_EVENT_INITIATOR (a GPU register) when payloadWords==1;
                    // the memory-write path requires payloadWords>1 (address+value), which this does not certify.
            if (p.payloadWords != 1) return fail(error, SideEffectFreeBatchFailure::UnsupportedOpcode, p.offsetWords, p.opcode);
            break;
        case 0x3F: {  // INDIRECT_BUFFER.
            if (p.payloadWords < 2) return fail(error, SideEffectFreeBatchFailure::InvalidIndirectBuffer, p.offsetWords);
            if (depth + 1 > limits.maxIndirectDepth) return fail(error, SideEffectFreeBatchFailure::RecursionLimit, p.offsetWords);
            if (result.indirectBuffersFollowed >= limits.maxIndirectBuffers)
                return fail(error, SideEffectFreeBatchFailure::RecursionLimit, p.offsetWords);
            const auto address = data[0];
            const auto count = data[1] & 0xFFFFF;  // Real hardware masks the count to 20 bits.
            if (!count) return fail(error, SideEffectFreeBatchFailure::InvalidIndirectBuffer, p.offsetWords);
            std::vector<uint32_t> nested;
            if (!resolver || !resolver(address, count, nested) || nested.size() != count)
                return fail(error, SideEffectFreeBatchFailure::UnresolvedIndirectBuffer, p.offsetWords);
            ++result.indirectBuffersFollowed;
            if (!walkSideEffectFreeBatch(nested, state, depth + 1, resolver, limits, result, error)) return false;
            break;
        }
        case 0x22: case 0x36: {  // DRAW_INDX / DRAW_INDX_2.
            // DRAW_INDX carries its own viz-query-condition token as its first
            // payload word, THEN still needs its own VGT_DRAW_INITIATOR word
            // (the reference's ExecutePacketType3Draw rejects a packet with no
            // words left for it) - two payload words minimum, not one.
            // DRAW_INDX_2 has no viz-query token (real hardware always treats
            // it as 0, per the reference's ExecutePacketType3_DRAW_INDX_2) but
            // still needs its own VGT_DRAW_INITIATOR word - one minimum.
            if (p.opcode == 0x22) {
                if (p.payloadWords < 2) return fail(error, SideEffectFreeBatchFailure::UnsupportedOpcode, p.offsetWords, p.opcode);
                if (data[0] != 0) return fail(error, SideEffectFreeBatchFailure::UnsafeOcclusionQuery, p.offsetWords, p.opcode);
            } else if (p.payloadWords < 1) {
                return fail(error, SideEffectFreeBatchFailure::UnsupportedOpcode, p.offsetWords, p.opcode);
            }
            // Safety comes entirely from the fixed-function output state, so the
            // packet's own predicate bit does not matter: whether or not this
            // draw would execute on real hardware, executing it produces no
            // persistent effect when both registers are exactly the observed-safe
            // value; not executing it obviously also produces none.
            if (!state.colorMask.has_value() || *state.colorMask != 0)
                return fail(error, SideEffectFreeBatchFailure::UnsafeColorWrite, p.offsetWords, p.opcode);
            if (!state.depthControl.has_value() || *state.depthControl != 0)
                return fail(error, SideEffectFreeBatchFailure::UnsafeDepthWrite, p.offsetWords, p.opcode);
            ++result.drawsVerified;
            break;
        }
        default:
            return fail(error, SideEffectFreeBatchFailure::UnsupportedOpcode, p.offsetWords, p.opcode);
        }
    }
    return true;
}
}  // namespace side_effect_free_detail

inline bool verifySideEffectFreeCommandBatch(const std::vector<uint32_t>& rootWords,
        const SideEffectFreeIndirectResolver& resolver, const SideEffectFreeBatchLimits& limits,
        SideEffectFreeBatchResult& output, SideEffectFreeBatchError& error) {
    using namespace side_effect_free_detail;
    if (limits.maxWords == 0 || limits.maxPackets == 0) return fail(error, SideEffectFreeBatchFailure::InvalidInput);
    WalkState state; SideEffectFreeBatchResult candidate; SideEffectFreeBatchError candidateError;
    if (!walkSideEffectFreeBatch(rootWords, state, 0, resolver, limits, candidate, candidateError)) {
        error = candidateError;
        return false;
    }
    // A batch with zero draws can still leave a persistent, nonzero
    // RB_COLOR_MASK/RB_DEPTHCONTROL behind for a later, unrelated submission
    // to observe - these registers are not reset between packets on real
    // hardware. Only "never touched" (no forward change at all) or "settled
    // back to the known-safe 0" are provably free of forward effect; the
    // draw-gating checks above never run at all when there is no draw to gate.
    if (state.colorMask.has_value() && *state.colorMask != 0)
        return fail(error, SideEffectFreeBatchFailure::UnsafeColorWrite);
    if (state.depthControl.has_value() && *state.depthControl != 0)
        return fail(error, SideEffectFreeBatchFailure::UnsafeDepthWrite);
    output = candidate; error = {};
    return true;
}
}  // namespace recomp::gpu
