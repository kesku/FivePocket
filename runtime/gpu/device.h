#pragma once
#include "shader_module_pack.h"
#include <vulkan/vulkan.h>
#include <mutex>

namespace recomp::gpu {
class ProbeDevice {
public:
    ~ProbeDevice();
    ProbeDevice() = default;
    ProbeDevice(const ProbeDevice&) = delete;
    ProbeDevice& operator=(const ProbeDevice&) = delete;
    bool inspect();
    bool create(const ShaderRequirements& requirements);
    // Explicit native resource requirement, separate from SPIR-V capabilities.
    // The one-argument API retains its original feature policy and symbol.
    bool create(const ShaderRequirements& requirements,bool samplerAnisotropy);
    bool create(const ShaderRequirements& requirements,bool samplerAnisotropy,bool sampleRateShading);
    bool create(const ShaderRequirements& requirements,bool samplerAnisotropy,bool sampleRateShading,bool independentBlend);
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    // Driver-owned compiled-shader cache, created with the device and passed to
    // every repeated graphics-pipeline creation. Without it each translated draw
    // recompiled its SPIR-V from scratch, which dominated per-draw cost. A
    // pipeline cache cannot change pipeline behaviour: it only lets the driver
    // reuse work it already did, so no admission or execution guard depends on it.
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    // vkCreateGraphicsPipelines requires external synchronisation of the cache.
    // Draw paths run under the renderer's owner gate, but resource setup is
    // reachable from more than one consumer, so the cache carries its own lock.
    std::mutex pipelineCacheMutex;
    uint32_t queueFamily = 0;
    bool fragmentStoresAndAtomicsEnabled = false;
    bool vertexStoresAndAtomicsEnabled = false;
    bool samplerAnisotropyEnabled = false;
    bool sampleRateShadingEnabled = false;
    bool independentBlendEnabled = false;
    VkPhysicalDeviceProperties properties{};
private:
    VkPhysicalDeviceFeatures features_{};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing_{};
    VkPhysicalDeviceBufferDeviceAddressFeatures addresses_{};
};
bool checkVulkan(VkResult result, const char* operation);
}
