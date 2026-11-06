#pragma once
// Explicit 32x32 RGBA8 native image. Input is already decoded, tightly packed.
#include "device.h"
namespace recomp::gpu {
template<class R> bool setupTexture32Image(const ProbeDevice& device,R& r){
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(device.physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((format.optimalTilingFeatures&required)!=required)return false;
    VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
    image.format=VK_FORMAT_R8G8B8A8_UNORM;image.extent={32,32,1};image.mipLevels=1;image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    if(vkGetPhysicalDeviceImageFormatProperties(device.physical,image.format,image.imageType,image.tiling,
        image.usage,0,&supported)!=VK_SUCCESS || supported.maxExtent.width<32 || supported.maxExtent.height<32 ||
        !(supported.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return false;
    if(vkCreateImage(r.d,&image,nullptr,&r.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(r.d,r.image,&requirements);
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
    unsigned memoryType=props.memoryTypeCount;
    for(unsigned i=0;i<props.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) &&
        (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){memoryType=i;break;}
    if(memoryType==props.memoryTypeCount)return false;
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType;
    if(vkAllocateMemory(r.d,&allocation,nullptr,&r.imageMemory)!=VK_SUCCESS ||
        vkBindImageMemory(r.d,r.image,r.imageMemory,0)!=VK_SUCCESS)return false;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image=r.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(r.d,&view,nullptr,&r.view)!=VK_SUCCESS)return false;
    VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter=VK_FILTER_LINEAR;sampler.minFilter=VK_FILTER_LINEAR;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;sampler.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;sampler.minLod=0;sampler.maxLod=0;
    return vkCreateSampler(r.d,&sampler,nullptr,&r.sampler)==VK_SUCCESS;
}
// Caller provides host -> transfer visibility and waits for the preceding fence.
template<class R> void uploadTexture32Image(R& r,VkCommandBuffer command,VkBuffer source,VkPipelineStageFlags stage){
    VkImageMemoryBarrier upload{};upload.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    upload.srcAccessMask=r.imageReady?VK_ACCESS_SHADER_READ_BIT:0;upload.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    upload.oldLayout=r.imageReady?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    upload.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    upload.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;upload.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    upload.image=r.image;upload.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(command,r.imageReady?stage:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&upload);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={32,32,1};
    if(r.replaceImageControl){const VkClearColorValue zero{};
        vkCmdClearColorImage(command,r.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&upload.subresourceRange);}
    else vkCmdCopyBufferToImage(command,source,r.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    upload.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;upload.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    upload.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;upload.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,stage,0,0,nullptr,0,nullptr,1,&upload);
}
}
