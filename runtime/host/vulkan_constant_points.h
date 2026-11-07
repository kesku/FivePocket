#pragma once
#include "device.h"
#include "native_constant_point_inputs.h"
#include <array>
#include <vector>
#include <limits>

// Shared native point pipeline used by the readback controls and the live
// experimental initialization backend. Never owns or aliases guest memory.
namespace recomp::gpu::constant_points {
using recomp::gpu::checkVulkan;
constexpr uint32_t Side = 16, Pixels = Side * Side, Bytes = Pixels * 4;
constexpr uint32_t CompletionBytes = 64, TotalBytes = Bytes + CompletionBytes;
struct Resources {
    explicit Resources(VkDevice input, uint32_t tailBytes = CompletionBytes) : completionBytes(tailBytes), device(input) {}
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;
    uint32_t completionBytes;
    VkDevice device;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE, bufferMemory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::array<VkShaderModule, 2> shaders{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool constantInputs = false;
    bool corruptRecordedUpdate = false; // Negative diagnostic only, never a guest mode.
    ~Resources() {
        if (mapped) vkUnmapMemory(device, bufferMemory);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        for (auto shader : shaders) if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
        if (pass) vkDestroyRenderPass(device, pass, nullptr);
        if (view) vkDestroyImageView(device, view, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (imageMemory) vkFreeMemory(device, imageMemory, nullptr);
        if (bufferMemory) vkFreeMemory(device, bufferMemory, nullptr);
    }
};
inline bool allocate(recomp::gpu::ProbeDevice& device, const VkMemoryRequirements& requirements,
              VkMemoryPropertyFlags flags, VkDeviceMemory& memory) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device.physical, &properties);
    uint32_t type = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags) { type = i; break; }
    if (type == std::numeric_limits<uint32_t>::max()) return false;
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = requirements.size; info.memoryTypeIndex = type;
    return checkVulkan(vkAllocateMemory(device.device, &info, nullptr, &memory), "point-memory");
}
inline bool setup(recomp::gpu::ProbeDevice& device, Resources& r,
           const std::array<std::vector<uint32_t>, 2>& code, bool constantInputs = false) {
    VkImageFormatProperties format{};
    if (vkGetPhysicalDeviceImageFormatProperties(device.physical, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &format) != VK_SUCCESS ||
        format.maxExtent.width < Side || format.maxExtent.height < Side) return false;
    for (size_t i = 0; i < code.size(); ++i) {
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = code[i].size() * 4; shader.pCode = code[i].data();
        if (!checkVulkan(vkCreateShaderModule(device.device, &shader, nullptr, &r.shaders[i]), "point-shader")) return false;
    }
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D; image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {Side, Side, 1}; image.mipLevels = 1; image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE; image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!checkVulkan(vkCreateImage(device.device, &image, nullptr, &r.image), "point-image")) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device.device, r.image, &requirements);
    if (!allocate(device, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, r.imageMemory) ||
        !checkVulkan(vkBindImageMemory(device.device, r.image, r.imageMemory, 0), "point-bind-image")) return false;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = r.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!checkVulkan(vkCreateImageView(device.device, &view, nullptr, &r.view), "point-view")) return false;
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = Bytes + r.completionBytes; buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!checkVulkan(vkCreateBuffer(device.device, &buffer, nullptr, &r.buffer), "point-readback")) return false;
    vkGetBufferMemoryRequirements(device.device, r.buffer, &requirements);
    if (!allocate(device, requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, r.bufferMemory) ||
        !checkVulkan(vkBindBufferMemory(device.device, r.buffer, r.bufferMemory, 0), "point-bind-readback") ||
        !checkVulkan(vkMapMemory(device.device, r.bufferMemory, 0, requirements.size, 0, &r.mapped), "point-map")) return false;
    VkAttachmentDescription attachment{};
    attachment.format = image.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1; pass.pAttachments = &attachment;
    pass.subpassCount = 1; pass.pSubpasses = &subpass;
    pass.dependencyCount = dependencies.size(); pass.pDependencies = dependencies.data();
    if (!checkVulkan(vkCreateRenderPass(device.device, &pass, nullptr, &r.pass), "point-render-pass")) return false;
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = r.pass; framebuffer.attachmentCount = 1; framebuffer.pAttachments = &r.view;
    framebuffer.width = Side; framebuffer.height = Side; framebuffer.layers = 1;
    if (!checkVulkan(vkCreateFramebuffer(device.device, &framebuffer, nullptr, &r.framebuffer), "point-framebuffer")) return false;
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPushConstantRange constantRange{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(recomp::gpu::NativeConstantPointInputs)};
    if (constantInputs) {
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(device.physical,&properties);
        if (properties.limits.maxPushConstantsSize < constantRange.size) return false;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &constantRange;
    }
    if (!checkVulkan(vkCreatePipelineLayout(device.device, &layout, nullptr, &r.layout), "point-layout")) return false;
    r.constantInputs = constantInputs;
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pool.queueFamilyIndex = device.queueFamily;
    if (!checkVulkan(vkCreateCommandPool(device.device, &pool, nullptr, &r.pool), "point-pool")) return false;
    VkCommandBufferAllocateInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool = r.pool; command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount = 1;
    if (!checkVulkan(vkAllocateCommandBuffers(device.device, &command, &r.command), "point-command")) return false;
    VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return checkVulkan(vkCreateFence(device.device, &fence, nullptr, &r.fence), "point-fence");
}
inline bool pipeline(Resources& r, uint32_t writeMask) {
    if (r.pipeline) { vkDestroyPipeline(r.device, r.pipeline, nullptr); r.pipeline = VK_NULL_HANDLE; }
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (size_t i = 0; i < stages.size(); ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = r.shaders[i]; stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo input{}; input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkViewport viewport{0, 0, Side, Side, 0, 1}; VkRect2D scissor{{0,0},{Side,Side}};
    VkPipelineViewportStateCreateInfo view{}; view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1; view.pViewports = &viewport; view.scissorCount = 1; view.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{}; color.colorWriteMask = writeMask;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1; blend.pAttachments = &color;
    VkGraphicsPipelineCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = stages.size(); info.pStages = stages.data(); info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly; info.pViewportState = &view; info.pRasterizationState = &raster;
    info.pMultisampleState = &samples; info.pColorBlendState = &blend;
    info.layout = r.layout; info.renderPass = r.pass;
    return checkVulkan(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &info, nullptr, &r.pipeline), "point-pipeline");
}
}
