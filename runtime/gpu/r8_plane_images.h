#pragma once
// Three explicit native R8 images. No title residency or Xenos sampler claim.
#include "device.h"
#include <array>
namespace recomp::gpu {
struct R8PlaneImages {
    static constexpr unsigned ImageBytes=1382400;
    static constexpr std::array<VkExtent3D,3> Extents{{{1280,720,1},{640,360,1},{640,360,1}}};
    static constexpr std::array<VkDeviceSize,3> Offsets{{0,921600,1152000}};
    VkDevice d;
    std::array<VkImage,3> images{};
    std::array<VkDeviceMemory,3> memory{};
    std::array<VkImageView,3> views{};
    VkSampler sampler{};
    bool ready=false;
    int zeroPlane=-1;
    explicit R8PlaneImages(VkDevice device):d(device){}
    ~R8PlaneImages(){
        if(sampler)vkDestroySampler(d,sampler,nullptr);
        for(unsigned i=0;i<3;++i){
            if(views[i])vkDestroyImageView(d,views[i],nullptr);
            if(images[i])vkDestroyImage(d,images[i],nullptr);
            if(memory[i])vkFreeMemory(d,memory[i],nullptr);
        }
    }
    bool setup(const ProbeDevice& device){
        VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(device.physical,VK_FORMAT_R8_UNORM,&format);
        constexpr auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if((format.optimalTilingFeatures&required)!=required)return false;
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
        for(unsigned i=0;i<3;++i){
            VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R8_UNORM;image.extent=Extents[i];
            image.mipLevels=1;image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
            image.tiling=VK_IMAGE_TILING_OPTIMAL;
            image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkImageFormatProperties supported{};
            if(vkGetPhysicalDeviceImageFormatProperties(device.physical,image.format,image.imageType,image.tiling,
                    image.usage,0,&supported)!=VK_SUCCESS || supported.maxExtent.width<image.extent.width ||
                    supported.maxExtent.height<image.extent.height ||
                    !(supported.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return false;
            if(vkCreateImage(d,&image,nullptr,&images[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(d,images[i],&requirements);
            unsigned type=props.memoryTypeCount;
            for(unsigned j=0;j<props.memoryTypeCount;++j)if((requirements.memoryTypeBits&(1u<<j)) &&
                (props.memoryTypes[j].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=j;break;}
            if(type==props.memoryTypeCount)return false;
            VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
            if(vkAllocateMemory(d,&allocation,nullptr,&memory[i])!=VK_SUCCESS ||
                vkBindImageMemory(d,images[i],memory[i],0)!=VK_SUCCESS)return false;
            VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image=images[i];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;
            view.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
            view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(d,&view,nullptr,&views[i])!=VK_SUCCESS)return false;
        }
        VkSamplerCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter=VK_FILTER_LINEAR;info.minFilter=VK_FILTER_LINEAR;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;info.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;info.minLod=0;info.maxLod=0;
        return vkCreateSampler(d,&info,nullptr,&sampler)==VK_SUCCESS;
    }
    // Caller makes host writes visible to transfer before this. Reuse only after
    // the previous fence. Each upload is followed by transfer -> shader visibility.
    void upload(VkCommandBuffer command,VkBuffer source,VkPipelineStageFlags shaderStage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT){
        for(unsigned i=0;i<3;++i){
            VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask=ready?VK_ACCESS_SHADER_READ_BIT:0;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout=ready?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=images[i];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(command,ready?shaderStage:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            if(zeroPlane==static_cast<int>(i)){
                const VkClearColorValue zero{};
                vkCmdClearColorImage(command,images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&barrier.subresourceRange);
            }else{
                VkBufferImageCopy copy{};copy.bufferOffset=Offsets[i];
                copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=Extents[i];
                vkCmdCopyBufferToImage(command,source,images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            }
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,shaderStage,
                0,0,nullptr,0,nullptr,1,&barrier);
        }
    }
};
}
