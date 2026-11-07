#pragma once
#include "native_gpu_word_updates.h"

namespace recomp::gpu {
struct AuditedNativeWord {
    std::uint32_t byteOffset = 0;
    std::array<std::uint8_t,4> bigEndianBytes{};
};
struct AuditedNativeWords {
    std::array<AuditedNativeWord,NativeWordUpdatePlan::Limit> words{};
    std::size_t count = 0;
};
enum class NativeWordReadbackFailure { None, InvalidPlan, InvalidReadback, Mismatch };
// Caller must wait for the REAL fence and establish GPU->host visibility before
// supplying a stable readback. This function cannot prove either condition.
// Validate the entire mutable plan again, collect each destination's LAST value,
// and compare every addressed byte before publishing even one result. Untouched
// bytes/canaries remain the caller's separate check. Output contains only NATIVE
// offsets and bytes, not guest ownership, lifecycle protection or write authority.
inline bool auditNativeWordReadback(const NativeWordUpdatePlan& plan, const std::uint8_t* bytes,
        std::size_t size, AuditedNativeWords& output, NativeWordReadbackFailure& error) noexcept {
    const auto fail = [&](NativeWordReadbackFailure why) { error = why; return false; };
    if (plan.count > NativeWordUpdatePlan::Limit) return fail(NativeWordReadbackFailure::InvalidPlan);
    std::array<NativeWordRequest,NativeWordUpdatePlan::Limit> requests{};
    for (std::size_t i = 0; i < plan.count; ++i) {
        const auto& w = plan.words[i];
        std::uint32_t value = 0;
        for (auto b : w.bigEndianBytes) value = (value << 8) | b;
        requests[i] = {w.byteOffset,value,w.phase};
    }
    NativeWordUpdatePlan checked;
    if (!planNativeWordUpdates(requests.data(),plan.count,plan.bufferBytes,checked))
        return fail(NativeWordReadbackFailure::InvalidPlan);
    if (!bytes || size != checked.bufferBytes) return fail(NativeWordReadbackFailure::InvalidReadback);
    AuditedNativeWords candidate;
    for (std::size_t i = 0; i < checked.count; ++i) {
        const auto& w = checked.words[i];
        std::size_t slot = 0;
        while (slot < candidate.count && candidate.words[slot].byteOffset != w.byteOffset) ++slot;
        if (slot == candidate.count) ++candidate.count;
        candidate.words[slot] = {w.byteOffset,w.bigEndianBytes};
    }
    for (std::size_t i = 0; i < candidate.count; ++i) {
        const auto& w = candidate.words[i];
        for (std::size_t lane = 0; lane < 4; ++lane)
            if (bytes[w.byteOffset+lane] != w.bigEndianBytes[lane]) return fail(NativeWordReadbackFailure::Mismatch);
    }
    output = candidate; error = NativeWordReadbackFailure::None; return true;
}
}
