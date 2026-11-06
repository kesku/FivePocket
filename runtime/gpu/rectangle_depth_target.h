#pragma once
#include "device.h"
#include <array>

// Standalone D32-float/S8 attachment helper. This is NOT Xenos D24/EDRAM storage,
// compression, sample layout, title raster state or a depth-only hardware oracle.
namespace recomp::gpu {
struct RectangleDepthConfig {
    unsigned width=32,height=32;
    VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;
    bool sampled=false;
    VkSampleMask sampleMask=0xFFFFFFFFu;
};
template<class R> bool setupRectangleDepthTarget(ProbeDevice& device,R& r,RectangleDepthConfig config={}) {
    if(!config.width || !config.height || config.width>1280 || config.height>720 ||
        (config.samples!=VK_SAMPLE_COUNT_1_BIT && config.samples!=VK_SAMPLE_COUNT_4_BIT) ||
        (!config.sampled && config.samples!=VK_SAMPLE_COUNT_1_BIT))return false;
    constexpr VkFormat format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
        (config.sampled?VK_IMAGE_USAGE_SAMPLED_BIT:VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(device.physical,format,&features);
    const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|
        (config.sampled?VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT:VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    if((features.optimalTilingFeatures&required)!=required)return false;
    VkImageFormatProperties supported{};
    if(vkGetPhysicalDeviceImageFormatProperties(device.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        usage,0,&supported)!=VK_SUCCESS || supported.maxExtent.width<config.width || supported.maxExtent.height<config.height ||
        !(supported.sampleCounts&config.samples))return false;
    VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType=VK_IMAGE_TYPE_2D;image.format=format;image.extent={config.width,config.height,1};image.mipLevels=1;image.arrayLayers=1;
    image.samples=config.samples;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;
    // Existing fixture's owned image fields are named colorImage/Memory/View.
    if(vkCreateImage(r.d,&image,nullptr,&r.colorImage)!=VK_SUCCESS)return false;
    VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(r.d,r.colorImage,&requirements);
    VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(device.physical,&properties);
    unsigned type=properties.memoryTypeCount;
    for(unsigned i=0;i<properties.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) &&
        (properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
    if(type==properties.memoryTypeCount)return false;
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
    if(vkAllocateMemory(r.d,&allocation,nullptr,&r.colorMemory)!=VK_SUCCESS ||
        vkBindImageMemory(r.d,r.colorImage,r.colorMemory,0)!=VK_SUCCESS)return false;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image=r.colorImage;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;
    view.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    if(vkCreateImageView(r.d,&view,nullptr,&r.colorView)!=VK_SUCCESS)return false;
    VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=config.samples;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout=config.sampled?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference depth{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment=&depth;
    std::array<VkSubpassDependency,2> dependencies{};
    constexpr auto tests=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=config.sampled?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].srcAccessMask=config.sampled?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[0].dstStageMask=tests;
    dependencies[0].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=tests;dependencies[1].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask=dependencies[0].srcStageMask;dependencies[1].dstAccessMask=dependencies[0].srcAccessMask;
    VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;
    pass.dependencyCount=dependencies.size();pass.pDependencies=dependencies.data();
    if(vkCreateRenderPass(r.d,&pass,nullptr,&r.renderPass)!=VK_SUCCESS)return false;
    VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass=r.renderPass;framebuffer.attachmentCount=1;framebuffer.pAttachments=&r.colorView;
    framebuffer.width=config.width;framebuffer.height=config.height;framebuffer.layers=1;
    return vkCreateFramebuffer(r.d,&framebuffer,nullptr,&r.framebuffer)==VK_SUCCESS;
}

template<class R> bool createRectangleDepthPipeline(R& r,RectangleDepthConfig config={}) {
    VkPipelineShaderStageCreateInfo stage{};stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.module=r.vertexShader;stage.pName="main";
    VkPipelineVertexInputStateCreateInfo input{};input.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,static_cast<float>(config.width),static_cast<float>(config.height),0,1};
    VkRect2D scissor{{0,0},{config.width,config.height}};
    VkPipelineViewportStateCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount=1;view.pViewports=&viewport;view.scissorCount=1;view.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{};samples.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples=config.samples;samples.pSampleMask=&config.sampleMask;
    VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable=VK_TRUE;depth.depthWriteEnable=r.depthWrite;depth.depthCompareOp=VK_COMPARE_OP_ALWAYS;
    depth.stencilTestEnable=VK_TRUE;
    depth.front={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,
        0xFFu,r.stencilWrite?0xFFu:0u,r.stencilReference};depth.back=depth.front;
    VkGraphicsPipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount=1;pipeline.pStages=&stage;pipeline.pVertexInputState=&input;
    pipeline.pInputAssemblyState=&assembly;pipeline.pViewportState=&view;pipeline.pRasterizationState=&raster;
    pipeline.pMultisampleState=&samples;pipeline.pDepthStencilState=&depth;
    pipeline.layout=r.layout;pipeline.renderPass=r.renderPass;
    return vkCreateGraphicsPipelines(r.d,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.pipeline)==VK_SUCCESS;
}
}
