#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace recomp::gpu {
enum class NativeWordPhase : std::uint32_t { BeforeGraphics, AfterGraphics };
struct NativeWordRequest {
    std::uint32_t byteOffset = 0, value = 0;
    NativeWordPhase phase = NativeWordPhase::BeforeGraphics;
};
struct alignas(4) NativeWordUpdate {
    std::uint32_t byteOffset = 0;
    NativeWordPhase phase = NativeWordPhase::BeforeGraphics;
    std::array<std::uint8_t, 4> bigEndianBytes{};
};
struct NativeWordUpdatePlan {
    static constexpr std::size_t Limit = 64;
    std::array<NativeWordUpdate, Limit> words{};
    std::size_t count = 0;
    std::uint32_t bufferBytes = 0;
};
// Validate a complete list before recording any GPU command. Offsets refer only
// to one caller-owned native staging buffer, NEVER to guest addresses. This
// does not validate shaders, hardware effects, guest ownership or completion.
// Recorder contract: before-graphics updates -> transfer/graphics dependency ->
// draw work -> all-earlier-writes/transfer dependency -> after-graphics updates
// -> transfer/host dependency -> real fence -> readback. Duplicate destinations
// within one phase reject; an ordered cross-phase overwrite is allowed.
inline bool planNativeWordUpdates(const NativeWordRequest* requests, std::size_t count,
        std::uint32_t bufferBytes, NativeWordUpdatePlan& output) noexcept {
    if (count > NativeWordUpdatePlan::Limit || (count && !requests) ||
        !bufferBytes || (bufferBytes & 3) || bufferBytes > 4 * 1024 * 1024) return false;
    NativeWordUpdatePlan candidate;
    candidate.count = count; candidate.bufferBytes = bufferBytes;
    bool afterGraphics = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        if (static_cast<std::uint32_t>(r.phase) > 1 || (r.byteOffset & 3) ||
            std::uint64_t{r.byteOffset} + 4 > bufferBytes ||
            (afterGraphics && r.phase == NativeWordPhase::BeforeGraphics)) return false;
        afterGraphics |= r.phase == NativeWordPhase::AfterGraphics;
        for (std::size_t j = 0; j < i; ++j) {
            if (requests[j].phase == r.phase && requests[j].byteOffset == r.byteOffset) return false;
        }
        candidate.words[i] = {r.byteOffset, r.phase, {std::uint8_t(r.value >> 24),
            std::uint8_t(r.value >> 16), std::uint8_t(r.value >> 8), std::uint8_t(r.value)}};
    }
    output = candidate; return true;
}
}
