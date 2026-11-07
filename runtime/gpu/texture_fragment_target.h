#pragma once
#include "device.h"
#include <array>

// Standalone offscreen float attachment; bounded explicit test configuration.
// No guest viewport, EDRAM, shader linkage or predicate state is inferred here.
namespace recomp::gpu {
// Explicit coordinate experiments, NOT inferred or admitted guest state. The
// historical positive-height diagnostic remains the default for every caller.
enum class FragmentViewportMode { DiagnosticPositive, Color1280NegativeY, Color1280NegativeYHalfPixel, Color1280ShaderHalfPixel };
struct FragmentTargetConfig {
    unsigned width=32,height=32;
    VkFormat format=VK_FORMAT_R32G32B32A32_SFLOAT;
    bool alphaBlend=false;
    bool transferDestination=false;
    FragmentViewportMode viewportMode=FragmentViewportMode::DiagnosticPositive;
    // No initialization of guest content. Caller must independently prove that
    // its draw overwrites every subsequently read pixel with all channels.
    bool fullOverwrite=false;
    // R8G8B8A8_UNORM and the single-channel R32_SFLOAT are admitted only for a
    // full overwrite without blending: neither carries the float range the two
    // wide formats do, so a partial write or a blend would read back whatever
    // the attachment held. Vulkan mandates COLOR_ATTACHMENT for both, and
    // setupTextureFragmentTarget still queries this device's own format
    // features before creating anything.
    constexpr bool valid() const {
        if (viewportMode!=FragmentViewportMode::DiagnosticPositive &&
            viewportMode!=FragmentViewportMode::Color1280NegativeY &&
            viewportMode!=FragmentViewportMode::Color1280NegativeYHalfPixel &&
            viewportMode!=FragmentViewportMode::Color1280ShaderHalfPixel) return false;
        if (viewportMode!=FragmentViewportMode::DiagnosticPositive && (width!=1280 || height!=720)) return false;
        return width && height && width<=1280 && height<=720 &&
            (format==VK_FORMAT_R32G32B32A32_SFLOAT || format==VK_FORMAT_R16G16B16A16_SFLOAT ||
             (fullOverwrite && !alphaBlend &&
              (format==VK_FORMAT_R8G8B8A8_UNORM || format==VK_FORMAT_R32_SFLOAT ||
               format==VK_FORMAT_R16G16_SNORM)));
    }
};
// The ABI23 pyramid's last level writes k_32_FLOAT, so a narrower allow-list
// here refuses the attachment and the band stops with target-image-create.
// These pin both halves of that: admitted for a full overwrite, refused the
// moment either condition it rests on is dropped.
static_assert(FragmentTargetConfig{40,11,VK_FORMAT_R32_SFLOAT,false,true,
    FragmentViewportMode::DiagnosticPositive,true}.valid());
static_assert(!FragmentTargetConfig{40,11,VK_FORMAT_R32_SFLOAT,false,true,
    FragmentViewportMode::DiagnosticPositive,false}.valid());
static_assert(!FragmentTargetConfig{40,11,VK_FORMAT_R32_SFLOAT,true,true,
    FragmentViewportMode::DiagnosticPositive,true}.valid());
// k_16_16 rides the same full-overwrite terms.
static_assert(FragmentTargetConfig{640,360,VK_FORMAT_R16G16_SNORM,false,true,
    FragmentViewportMode::DiagnosticPositive,true}.valid());
static_assert(!FragmentTargetConfig{640,360,VK_FORMAT_R16G16_SNORM,false,true,
    FragmentViewportMode::DiagnosticPositive,false}.valid());
inline bool fragmentTargetCapabilities(const VkPhysicalDeviceProperties& properties,FragmentTargetConfig config) {
    if(!config.valid())return false;
    const auto& limits=properties.limits;
    const bool halfPixel=config.viewportMode==FragmentViewportMode::Color1280NegativeYHalfPixel;
    const bool padded=config.viewportMode==FragmentViewportMode::Color1280ShaderHalfPixel;
    const unsigned viewportWidth=padded?2048:config.width,viewportHeight=padded?1024:config.height;
    // Negative height is core in Vulkan 1.1; ProbeDevice requires Vulkan 1.2.
    // Refuse hardware that cannot represent this experiment's fractional origin.
    if (properties.apiVersion<VK_API_VERSION_1_1 ||
        (halfPixel && limits.viewportSubPixelBits<1) || !(limits.viewportBoundsRange[0]<=0) ||
        !(limits.viewportBoundsRange[1]>=float(viewportWidth)+(halfPixel?.5f:0.f)) ||
        !(limits.viewportBoundsRange[1]>=float(viewportHeight)+(halfPixel?.5f:0.f))) return false;
    if(limits.maxFramebufferWidth<config.width || limits.maxFramebufferHeight<config.height ||
        limits.maxViewportDimensions[0]<viewportWidth || limits.maxViewportDimensions[1]<viewportHeight ||
        !(limits.framebufferColorSampleCounts&VK_SAMPLE_COUNT_1_BIT))return false;
    return true;
}
template<class R> bool setupTextureFragmentTarget(ProbeDevice& device,R& r,FragmentTargetConfig config={},VkRenderPass* loadPass=nullptr) {
    if(!fragmentTargetCapabilities(device.properties,config))return false;
    const auto format=config.format;
    VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(device.physical,format,&features);
    const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|
        (config.alphaBlend?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT:0)|
        (config.transferDestination?VK_FORMAT_FEATURE_TRANSFER_DST_BIT:0);
    if((features.optimalTilingFeatures&required)!=required)return false;
    VkImageFormatProperties supported{};
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
        (config.transferDestination?VK_IMAGE_USAGE_TRANSFER_DST_BIT:0);
    if(vkGetPhysicalDeviceImageFormatProperties(device.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        usage,0,&supported)!=VK_SUCCESS ||
        supported.maxExtent.width<config.width || supported.maxExtent.height<config.height ||
        !(supported.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return false;
    VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType=VK_IMAGE_TYPE_2D;image.format=format;image.extent={config.width,config.height,1};image.mipLevels=1;image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=usage;
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
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(r.d,&view,nullptr,&r.colorView)!=VK_SUCCESS)return false;
    VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=config.fullOverwrite?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
    std::array<VkSubpassDependency,2> dependencies{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;
    pass.dependencyCount=dependencies.size();pass.pDependencies=dependencies.data();
    if(vkCreateRenderPass(r.d,&pass,nullptr,&r.renderPass)!=VK_SUCCESS)return false;
    if(loadPass){
        // Compatible with the CLEAR pass: only loadOp and initial layout differ.
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.initialLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if(vkCreateRenderPass(r.d,&pass,nullptr,loadPass)!=VK_SUCCESS)return false;
    }
    VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass=r.renderPass;framebuffer.attachmentCount=1;framebuffer.pAttachments=&r.colorView;
    framebuffer.width=config.width;framebuffer.height=config.height;framebuffer.layers=1;
    return vkCreateFramebuffer(r.d,&framebuffer,nullptr,&r.framebuffer)==VK_SUCCESS;
}

template<class R> bool createTextureFragmentPipeline(R& r,bool strip=false,FragmentTargetConfig config={}) {
    if(!config.valid())return false;
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module=i?r.shader:r.vertexShader;stages[i].pName="main";}
    VkPipelineVertexInputStateCreateInfo input{};input.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=strip?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,float(config.width),float(config.height),0,1};
    if (config.viewportMode==FragmentViewportMode::Color1280ShaderHalfPixel) {
        // Matching translated VS applies Y and the half-pixel offset itself.
        viewport={0,0,2048,1024,1,0};
    } else if (config.viewportMode!=FragmentViewportMode::DiagnosticPositive) {
        const float origin=config.viewportMode==FragmentViewportMode::Color1280NegativeYHalfPixel?.5f:0.f;
        viewport={origin,float(config.height)+origin,float(config.width),-float(config.height),1,0};
    }
    VkRect2D scissor{{0,0},{config.width,config.height}};
    VkPipelineViewportStateCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount=1;view.pViewports=&viewport;view.scissorCount=1;view.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{};samples.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=15;
    if(config.alphaBlend){
        color.blendEnable=VK_TRUE;
        color.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.colorBlendOp=VK_BLEND_OP_ADD;
        color.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.alphaBlendOp=VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount=1;blend.pAttachments=&color;
    VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount=stages.size();info.pStages=stages.data();info.pVertexInputState=&input;
    info.pInputAssemblyState=&assembly;info.pViewportState=&view;info.pRasterizationState=&raster;
    info.pMultisampleState=&samples;info.pColorBlendState=&blend;info.layout=r.layout;info.renderPass=r.renderPass;
    return vkCreateGraphicsPipelines(r.d,VK_NULL_HANDLE,1,&info,nullptr,&r.pipeline)==VK_SUCCESS;
}
}
