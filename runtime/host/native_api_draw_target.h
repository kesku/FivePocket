#pragma once
#include "device.h"
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace recomp::gpu {
class NativeColorChainProgram;
struct NativeTranslatedDrawProgram;
struct Gta5InlineDraw;
// Persistent target and pipelines for the existing two startup shader ABIs.
// No command-ring/frame counters. The caller validates effective shader
// identities, geometry, sampler and render state before drawValidatedAbi().
// Queue use must be serialized; the device outlives this object. Every method
// completes its real GPU work before returning. No guest publication is implied.
class NativeApiDrawTarget {
public:
    static std::unique_ptr<NativeApiDrawTarget> create(ProbeDevice&,const NativeColorChainProgram&);
    ~NativeApiDrawTarget();
    NativeApiDrawTarget(const NativeApiDrawTarget&)=delete;
    NativeApiDrawTarget& operator=(const NativeApiDrawTarget&)=delete;
    // Exact owned RGBA16F pixels, little-endian, no padding/canaries. Required
    // before drawing: undefined target contents are never replaced by a mock clear.
    bool loadHalf(const std::vector<std::uint8_t>&);
    // Full-target API clear. Values come from the game's clear request after
    // conversion to the bound guest target format, never fixture pixels.
    bool clearColor(const std::array<float,4>&);
    bool drawValidatedAbi(unsigned pair,const std::vector<std::uint8_t>&);
    bool drawTranslated(const NativeTranslatedDrawProgram&,const Gta5InlineDraw&,
                        const std::vector<std::uint32_t>& indices={});
    std::optional<std::vector<std::uint8_t>> readHalf()const;
    // Actual GPU 7e3 store / tiled RGBA8 resolve. Preserve destination padding.
    // Caller validates the bound source and destination profile and ownership.
    std::optional<std::vector<std::uint8_t>> resolveTiled(const std::vector<std::uint8_t>& previous);
private:
    struct Impl;
    explicit NativeApiDrawTarget(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
