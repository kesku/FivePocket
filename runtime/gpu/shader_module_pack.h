#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recomp::gpu {
struct ShaderRequirements {
    bool int64 = false;
    bool clipDistance = false;
    bool sampledImageArrayDynamicIndexing = false;
    bool runtimeDescriptorArray = false;
    bool bufferDeviceAddress = false;
    // Native fragment diagnostics; not implied by imported shader capabilities.
    bool fragmentStoresAndAtomics = false;
    bool vertexStoresAndAtomics = false;
};
struct ShaderModule {
    struct VertexInput {
        uint32_t location = 0;
        uint32_t components = 0;
        uint32_t kind = 0; // 0: float32, 1: uint32, 2: int32.
    };
    uint32_t index = 0;
    uint32_t executionModel = 0;
    std::vector<uint32_t> words;
    std::vector<VertexInput> vertexInputs;
};
struct ShaderModulePack {
    uint32_t constantBinding = 0; // 1: 24-byte device addresses; 2: set-4 buffers.
    ShaderRequirements requirements;
    std::vector<ShaderModule> modules;
};
// Framing/capability gate only. Full spirv-val must have passed before packaging.
bool parseShaderModulePack(const uint8_t* data, size_t size, ShaderModulePack& output);
}
