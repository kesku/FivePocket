#pragma once
#include "xenos_stopped_work.h"

namespace recomp::gpu {
// Diagnostic bytes at the STOPPED CPU boundary, not GPU execution-time shader
// contents. Caller must have joined all guest writers, successfully inspected
// work, and supply an allocation-owned physical reader. Predicated draws and
// unknown preceding commands remain unexecuted; these snapshots do not establish
// which programs execute or whether preceding GPU writes would change them.
// All-or-nothing: no partial capture or borrowed guest pointer escapes.
inline bool captureStoppedShaderBytes(const StoppedGpuWork& work,
        const Pm4PhysicalReader& reader, std::vector<std::vector<std::uint8_t>>& output) {
    constexpr std::size_t MaxPrograms = 8, MaxBytes = 4096;
    if (work.shaders.size() > MaxPrograms) return false;
    std::size_t total = 0;
    for (const auto& item : work.shaders) {
        const auto& s = item.load;
        const auto bytes = std::uint64_t{s.instructionWords} * 4;
        if (s.stage > 1 || !bytes || s.instructionWords % 3 || bytes > MaxBytes - total)
            return false;
        total += static_cast<std::size_t>(bytes);
        if (s.immediate) {
            if (s.physicalAddress || s.inlineBigEndianBytes.size() != bytes) return false;
        } else if (!reader || !s.inlineBigEndianBytes.empty() || (s.physicalAddress & 3) ||
                std::uint64_t{s.physicalAddress} + bytes > 0x20000000) return false;
    }
    try {
        std::vector<std::vector<std::uint8_t>> candidate;
        candidate.reserve(work.shaders.size());
        for (const auto& item : work.shaders) {
            const auto& s = item.load;
            if (s.immediate) candidate.push_back(s.inlineBigEndianBytes);
            else {
                auto bytes = reader(s.physicalAddress, s.instructionWords * 4);
                if (!bytes || bytes->size() != std::size_t{s.instructionWords} * 4) return false;
                candidate.push_back(std::move(*bytes));
            }
        }
        output = std::move(candidate);
        return true;
    } catch (...) { return false; } // Failed reader/allocation cannot publish a prefix.
}
}
