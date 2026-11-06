#pragma once
#include "xenos_float_vertex_fetch.h"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace recomp::gpu {
enum class CopyShaderStage : std::uint32_t { Vertex, Pixel };
enum class CopyShaderFailure : std::uint32_t {
    None, InvalidInput, ControlLimit, UnsupportedControl, InvalidExecutionRange,
    OverlappingStorage, UnsupportedAllocation, RepeatedAllocation, UnsupportedAlu,
    UnsupportedDestination, UnsupportedSource, ExecutionLimit, MissingExports,
    UnsupportedFetch, FetchLimit
};
struct CopyShaderError { CopyShaderFailure failure = CopyShaderFailure::None; std::uint32_t index = 0; };
// Components are symbolic, not guessed initial register values. MAX(x,x) is
// treated as a copy ONLY for defined finite inputs. This is not a hardware
// NaN/denormal/negative-zero or general shader execution model.
struct CopyShaderProof {
    using Vector = std::array<std::uint16_t,4>;
    static constexpr std::uint16_t Unwritten = 0, Zero = 1, One = 2, Initial = 3;
    static constexpr std::uint16_t Fetch = Initial + 256;
    CopyShaderStage stage = CopyShaderStage::Vertex;
    std::uint32_t byteCount = 0, controlCount = 0, executedCount = 0;
    std::array<std::uint32_t,256> executedAddresses{};
    std::array<Vector,64> exports{};
    std::bitset<64> exported;
    std::bitset<256> initialComponentsRead;
};
struct FloatCopyShaderProof : CopyShaderProof {
    struct FetchRecord {
        FloatVertexFetch layout;
        std::uint16_t indexExpression=0;
        std::uint32_t instructionAddress=0;
    };
    std::array<FetchRecord,32> fetches{};
    std::uint32_t fetchCount=0;
};

// Fixed-storage, all-or-nothing native version of the restricted offline
// copy/constant analysis. Input is bounded owned BE microcode, not an invented
// ShaderContainer. Encodings: pinned XenosRecomp shader_code.h and Xenia ucode.h
// 95a5c3ee250f80c3b9d139658649d9ffb6db3eec. No fetch, relative addressing,
// predicate, scalar dependency, memory export or unknown control is accepted.
template<bool AllowFloatFetch,class Proof>
inline bool analyzeCopyShaderImpl(const std::uint8_t* bytes, std::size_t size, CopyShaderStage stage,
        Proof& output, CopyShaderError& error) noexcept {
    const auto fail = [&](CopyShaderFailure why, std::uint32_t index = 0) {
        error = {why,index}; return false;
    };
    if (!bytes || !size || size % 12 || size > 4096 ||
        (stage != CopyShaderStage::Vertex && stage != CopyShaderStage::Pixel) ||
        (AllowFloatFetch && stage != CopyShaderStage::Vertex))
        return fail(CopyShaderFailure::InvalidInput);
    const auto word = [&](std::uint32_t i) {
        const auto* p = bytes + std::size_t{i} * 4;
        return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
            (std::uint32_t{p[2]} << 8) | p[3];
    };
    struct Control { std::uint32_t opcode = 0, first = 0, count = 0, sequence = 0; };
    std::array<Control,128> controls{};
    std::uint32_t controlCount = 0;
    const auto instructions = static_cast<std::uint32_t>(size / 12);
    auto bodyStart = instructions;
    bool ended = false;
    for (std::uint32_t pair = 0; pair < bodyStart && !ended; ++pair) {
        const auto a = word(pair*3), b = word(pair*3+1), c = word(pair*3+2);
        const std::array<std::uint64_t,2> packed{{a | (std::uint64_t{b & 65535} << 32),
            (b >> 16) | (std::uint64_t{c} << 16)}};
        for (const auto cf : packed) {
            if (controlCount == controls.size()) return fail(CopyShaderFailure::ControlLimit,controlCount);
            const auto opcode = static_cast<std::uint32_t>(cf >> 44);
            Control control{opcode,0,0,0};
            if (!opcode) {
                if (cf) return fail(CopyShaderFailure::UnsupportedControl,controlCount);
            } else if (opcode == 12) {
                if (cf & ~((std::uint64_t{15} << 44) | (std::uint64_t{7} << 40) | 7))
                    return fail(CopyShaderFailure::UnsupportedAllocation,controlCount);
                control.first = (cf >> 41) & 3; control.count = (cf & 7) + 1;
                if ((control.first != 2 && !(stage == CopyShaderStage::Vertex && control.first == 1)) ||
                    (control.first == 1 && control.count != 1) ||
                    (stage == CopyShaderStage::Pixel && control.count > 4))
                    return fail(CopyShaderFailure::UnsupportedAllocation,controlCount);
            } else if (opcode == 1 || opcode == 2) {
                if (cf & ~((std::uint64_t{15} << 44) | (std::uint64_t{1} << 43) |
                    (std::uint64_t{1} << 41) | ((std::uint64_t{1} << 28) - 1) |
                    (AllowFloatFetch ? (std::uint64_t{63} << 28) : 0)))
                    return fail(CopyShaderFailure::UnsupportedControl,controlCount);
                control.first = cf & 4095; control.count = (cf >> 12) & 7;
                const auto sequence = static_cast<std::uint32_t>((cf >> 16) & 4095);
                control.sequence=sequence;
                if (!control.count || control.count > 6 || control.first + control.count > instructions ||
                    (sequence >> (control.count*2)))
                    return fail(CopyShaderFailure::InvalidExecutionRange,controlCount);
                if (!AllowFloatFetch && (sequence & 0x555)) return fail(CopyShaderFailure::UnsupportedControl,controlCount);
                if (control.first < bodyStart) bodyStart = control.first;
                ended = opcode == 2;
            } else return fail(CopyShaderFailure::UnsupportedControl,controlCount);
            controls[controlCount++] = control;
            if (ended) break;
        }
    }
    if (!ended || (controlCount+1)/2 > bodyStart) return fail(CopyShaderFailure::OverlappingStorage);
    Proof candidate;
    candidate.stage = stage; candidate.byteCount = static_cast<std::uint32_t>(size); candidate.controlCount = controlCount;
    std::array<CopyShaderProof::Vector,64> registers{};
    for (std::uint16_t r = 0; r < 64; ++r)
        for (std::uint16_t c = 0; c < 4; ++c) registers[r][c] = CopyShaderProof::Initial + r*4 + c;
    std::array<std::uint32_t,3> allocated{};
    std::optional<FloatVertexFetch> previousFull;
    std::uint16_t previousFullIndex=0;
    for (std::uint32_t k = 0; k < controlCount; ++k) {
        const auto& control = controls[k];
        if (control.opcode == 12) {
            if (allocated[control.first]) return fail(CopyShaderFailure::RepeatedAllocation,k);
            allocated[control.first] = control.count;
        }
        if (control.opcode != 1 && control.opcode != 2) continue;
        for (std::uint32_t at = control.first; at < control.first + control.count; ++at) {
            if (candidate.executedCount == candidate.executedAddresses.size()) return fail(CopyShaderFailure::ExecutionLimit,at);
            const auto a = word(at*3), b = word(at*3+1), c = word(at*3+2);
            if constexpr (AllowFloatFetch) {
                if (control.sequence & (1u << ((at-control.first)*2))) {
                    if (candidate.fetchCount==candidate.fetches.size()) return fail(CopyShaderFailure::FetchLimit,at);
                    FloatVertexFetch fetch;
                    if (!decodeFloatVertexFetch({a,b,c},previousFull,fetch)) return fail(CopyShaderFailure::UnsupportedFetch,at);
                    // Save the full fetch's symbolic index BEFORE it overwrites
                    // its own source register. Mini reuses that saved expression.
                    const auto index=fetch.mini?previousFullIndex:registers[fetch.sourceRegister][fetch.sourceComponent];
                    if (!fetch.mini) {previousFull=fetch;previousFullIndex=index;}
                    candidate.fetches[candidate.fetchCount]={fetch,index,at};
                    auto& target=registers[fetch.destinationRegister];
                    for (unsigned lane=0;lane<4;++lane) {
                        const auto s=fetch.swizzle[lane];
                        if (s<4) target[lane]=static_cast<std::uint16_t>(CopyShaderProof::Fetch+candidate.fetchCount*4+s);
                        else if (s<6) target[lane]=static_cast<std::uint16_t>(CopyShaderProof::Zero+s-4);
                    }
                    if (index>=CopyShaderProof::Initial && index<CopyShaderProof::Fetch)
                        candidate.initialComponentsRead.set(index-CopyShaderProof::Initial);
                    ++candidate.fetchCount;candidate.executedAddresses[candidate.executedCount++]=at;continue;
                }
            }
            if (((c >> 24) & 31) != 2 || (a >> 26) != 50 ||
                (a & ((1u << 6) | (1u << 7) | (3u << 24))) || (b >> 24))
                return fail(CopyShaderFailure::UnsupportedAlu,at);
            const bool exported = a & (1u << 15), relativeScalar = a & (1u << 14);
            const auto dest = a & 63, vector = (a >> 16) & 15, scalar = (a >> 20) & 15;
            auto vectorResult = vector, scalarResult = scalar;
            std::uint32_t zero = 0, one = 0;
            auto* target = &registers[dest];
            if (exported) {
                vectorResult &= ~scalar; scalarResult &= ~vector;
                zero = relativeScalar ? (15 & ~(vector | scalar)) : 0; one = vector & scalar;
                const auto kind = stage == CopyShaderStage::Vertex && dest == 62 ? 1 : 2;
                if (!allocated[kind] || (kind == 2 && dest >= allocated[kind]))
                    return fail(CopyShaderFailure::UnsupportedDestination,at);
                candidate.exported.set(dest); target = &candidate.exports[dest];
            } else if (relativeScalar) return fail(CopyShaderFailure::UnsupportedDestination,at);
            if (scalarResult) return fail(CopyShaderFailure::UnsupportedAlu,at);
            if (vectorResult) {
                const auto src1 = (c >> 16) & 255, src2 = (c >> 8) & 255;
                const auto swizzle1 = (b >> 16) & 255, swizzle2 = (b >> 8) & 255;
                if ((c >> 30) != 3 || src1 >= 64 || src1 != src2 || swizzle1 != swizzle2)
                    return fail(CopyShaderFailure::UnsupportedSource,at);
                // Snapshot before any component write: a swizzled in-place
                // assignment must not cascade through freshly written lanes.
                const auto source = registers[src1];
                for (std::uint32_t lane = 0; lane < 4; ++lane) if (vectorResult & (1u << lane)) {
                    const auto value = source[((swizzle1 >> (2*lane)) + lane) & 3];
                    if (value >= CopyShaderProof::Initial && value < CopyShaderProof::Fetch)
                        candidate.initialComponentsRead.set(value-CopyShaderProof::Initial);
                    (*target)[lane] = value;
                }
            }
            for (std::uint32_t lane = 0; lane < 4; ++lane) {
                if (zero & (1u << lane)) (*target)[lane] = CopyShaderProof::Zero;
                if (one & (1u << lane)) (*target)[lane] = CopyShaderProof::One;
            }
            candidate.executedAddresses[candidate.executedCount++] = at;
        }
    }
    if (candidate.exported.none()) return fail(CopyShaderFailure::MissingExports);
    output = candidate; error = {}; return true;
}

// Original no-fetch contract remains the default and keeps its original proof
// shape. The explicit float variant below adds symbolic dependencies only.
inline bool analyzeCopyShader(const std::uint8_t* bytes,std::size_t size,CopyShaderStage stage,
        CopyShaderProof& output,CopyShaderError& error) noexcept {
    return analyzeCopyShaderImpl<false>(bytes,size,stage,output,error);
}
// Native counterpart of raw_shader_analysis.py's allow_vertex_fetch mode. Cache
// hints are retained as accepted syntax, NOT executed cache operations. No
// resource data/index is read or assumed: arbitrary initial/fetch dependencies
// remain symbolic for a subsequent narrower consumer to reject or resolve.
inline bool analyzeFloatCopyShader(const std::uint8_t* bytes,std::size_t size,
        FloatCopyShaderProof& output,CopyShaderError& error) noexcept {
    return analyzeCopyShaderImpl<true>(bytes,size,CopyShaderStage::Vertex,output,error);
}

struct ConstantShaderPair {
    std::array<std::uint8_t,4> position{}, parameter{}; // Literal 0 or 1 only.
};
// Explicit native location-0 probe linkage. Does NOT validate the ordering of
// shader loads, draw/raster state, complete batch, or hardware float edge cases.
inline bool linkConstantShaderPair(const CopyShaderProof& vertex, const CopyShaderProof& pixel,
        std::uint32_t programControl, std::uint32_t contextMisc, ConstantShaderPair& output) noexcept {
    if (programControl != 0x1000000E || contextMisc || vertex.stage != CopyShaderStage::Vertex ||
        pixel.stage != CopyShaderStage::Pixel || vertex.exported.count() != 2 || !vertex.exported[0] ||
        !vertex.exported[62] || vertex.initialComponentsRead.any() || pixel.exported.count() != 1 || !pixel.exported[0])
        return false;
    ConstantShaderPair candidate;
    for (std::size_t i = 0; i < 4; ++i) {
        if (pixel.exports[0][i] != CopyShaderProof::Initial+i) return false;
        const auto p = vertex.exports[62][i], v = vertex.exports[0][i];
        if ((p != CopyShaderProof::Zero && p != CopyShaderProof::One) ||
            (v != CopyShaderProof::Zero && v != CopyShaderProof::One)) return false;
        candidate.position[i] = p-CopyShaderProof::Zero; candidate.parameter[i] = v-CopyShaderProof::Zero;
    }
    output = candidate; return true;
}
}
