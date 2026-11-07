#pragma once
#include "xenos_draw_command_plan.h"
#include <optional>

namespace recomp::gpu {
// Translation/pipeline association only, not an authenticated module ABI or a
// whole-batch admission certificate. Historical diagnostics remain explicit.
enum class NativeColorCoordinateProfile : unsigned { DiagnosticPositive, Padded1280W1 };
inline bool validNativeColorCoordinateProfile(NativeColorCoordinateProfile value) noexcept {
    return value==NativeColorCoordinateProfile::DiagnosticPositive || value==NativeColorCoordinateProfile::Padded1280W1;
}
struct NativeColorViewportRequest {
    std::uint32_t drawOffset=0;
    std::array<float,6> nativeViewport{};
    std::array<std::uint32_t,4> nativeScissor{};
};
// The narrow color coordinate/index/window request tested by D-190. Every word
// must be known; no register reset or previous-batch value is invented. Pixel
// center is VTX_CNTL 0x2302, NOT AA_CONFIG 0x2301. The padded host viewport keeps
// the captured full W1 rectangle away from native XY clipping; scissor is tight.
//
// This does NOT prove the shader's complete vertex exports/W1/rectangle, shader
// linkage, blend/depth state, samples, arbitrary clipping, WAIT/cache effects or
// allocation ownership. Those are still separate obligations of a live caller.
// It does not read resources, allocate, execute the GPU or change guest memory.
template<class ReadRegister>
bool matchesNativeColorViewportRegisters(const ReadRegister& read) noexcept {
    constexpr std::array<std::array<std::uint32_t,2>,28> required{{
        {{0x2000,0x14000500}},{{0x200E,0}},{{0x200F,0x20002000}},
        {{0x2080,0}},{{0x2081,0}},{{0x2082,0x2D00500}},
        {{0x2100,0xFFFFFF}},{{0x2101,0}},{{0x2102,0}},
        {{0x210F,0x44200000}},{{0x2110,0x44200000}},{{0x2111,0xC3B40000}},
        {{0x2112,0x43B40000}},{{0x2113,0xBF800000}},{{0x2114,0x3F800000}},
        {{0x2204,0x80000}},{{0x2205,0x18000}},{{0x2206,0x43F}},{{0x2208,4}},
        {{0x2284,0}},{{0x2292,0}},{{0x2293,0}},
        {{0x2301,0}},{{0x2302,4}},{{0x2303,0x40000000}},{{0x2304,0x3F800000}},
        {{0x2305,0x40000000}},{{0x2306,0x3F800000}}
    }};
    for (const auto& pair:required) {
        const auto value=read(pair[0]);
        if (!value || *value!=pair[1]) return false;
    }
    return true;
}
inline bool planNativeColorViewportRequest(const DrawCommandCandidate& draw,NativeColorViewportRequest& output) noexcept {
    if (!draw.offsetWords || (draw.opcode!=0x22 && draw.opcode!=0x36) || draw.indexCount!=4 || draw.primitive!=6 ||
        evaluateDrawBinPredicate(draw.predicate.state,draw.predicate.predicated)!=DrawBinDecision::Pass) return false;
    if(!matchesNativeColorViewportRegisters([&](unsigned index)->std::optional<std::uint32_t> {
        const auto it=draw.registers.find(index);
        if(it==draw.registers.end() || it->second.bits.known!=0xFFFFFFFF)return std::nullopt;
        return it->second.bits.value;
    }))return false;
    output={draw.offsetWords,{{0,0,2048,1024,1,0}},{{0,0,1280,720}}};return true;
}
}
