#pragma once
#include "device.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <cmath>

namespace recomp::gpu {
// Failure-only readback of actual encoded MSAA storage, not a game resolve.
// Pixel-major, then sample-major, RGBA8 bytes. Borrowed owner remains alive
// under the queue lock. Source begins/ends TRANSFER_SRC and is never written.
inline std::optional<std::vector<std::uint8_t>> snapshotNativeSampleWords(ProbeDevice& d,VkImage source,
        unsigned width,unsigned height,unsigned samples,const std::vector<std::uint32_t>& code,bool depth) {
    if(!d.device || !d.queue || !source || !width || !height || width>1280 || height>720 ||
       (samples!=2 && samples!=4) || code.size()<5 || code.size()>65536 || code[0]!=0x07230203)return std::nullopt;
    struct Resources {
        VkDevice d;VkImageView view{};VkSampler sampler{};VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};
        VkShaderModule shader{};VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkPipeline pipeline{};
        VkDescriptorPool descriptors{};VkDescriptorSet set{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
            if(layout)vkDestroyPipelineLayout(d,layout,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
            if(shader)vkDestroyShaderModule(d,shader,nullptr);if(sampler)vkDestroySampler(d,sampler,nullptr);
            if(view)vkDestroyImageView(d,view,nullptr);if(mapped)vkUnmapMemory(d,memory);
            if(buffer)vkDestroyBuffer(d,buffer,nullptr);if(memory)vkFreeMemory(d,memory,nullptr);
        }
    } r{d.device};
    const unsigned count=width*height*samples,bytes=32+count*8;
    if(bytes>d.properties.limits.maxStorageBufferRange)return std::nullopt;
    VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=source;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
    v.format=depth?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R8G8B8A8_UNORM;
    v.subresourceRange={VkImageAspectFlags(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
    if(vkCreateImageView(r.d,&v,nullptr,&r.view)!=VK_SUCCESS)return std::nullopt;
    VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;s.magFilter=s.minFilter=VK_FILTER_NEAREST;
    s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(r.d,&s,nullptr,&r.sampler)!=VK_SUCCESS)return std::nullopt;
    VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=bytes;b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if(vkCreateBuffer(r.d,&b,nullptr,&r.buffer)!=VK_SUCCESS)return std::nullopt;
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.buffer,&req);
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(d.physical,&props);unsigned type=props.memoryTypeCount;
    for(unsigned i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&6)==6){type=i;break;}
    if(type==props.memoryTypeCount)return std::nullopt;
    VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
    if(vkAllocateMemory(r.d,&alloc,nullptr,&r.memory)!=VK_SUCCESS || vkBindBufferMemory(r.d,r.buffer,r.memory,0)!=VK_SUCCESS ||
       vkMapMemory(r.d,r.memory,0,VK_WHOLE_SIZE,0,&r.mapped)!=VK_SUCCESS)return std::nullopt;
    const VkDescriptorSetLayoutBinding bindings[]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=2;sl.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(r.d,&sl,nullptr,&r.setLayout)!=VK_SUCCESS)return std::nullopt;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,12};
    VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=1;pl.pSetLayouts=&r.setLayout;
    pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;if(vkCreatePipelineLayout(r.d,&pl,nullptr,&r.layout)!=VK_SUCCESS)return std::nullopt;
    VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;sm.codeSize=code.size()*4;sm.pCode=code.data();
    if(vkCreateShaderModule(r.d,&sm,nullptr,&r.shader)!=VK_SUCCESS)return std::nullopt;
    VkComputePipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pipeline.layout=r.layout;
    pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module=r.shader;pipeline.stage.pName="main";
    if(vkCreateComputePipelines(r.d,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.pipeline)!=VK_SUCCESS)return std::nullopt;
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=sizes;
    if(vkCreateDescriptorPool(r.d,&dp,nullptr,&r.descriptors)!=VK_SUCCESS)return std::nullopt;
    VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ds.descriptorPool=r.descriptors;
    ds.descriptorSetCount=1;ds.pSetLayouts=&r.setLayout;if(vkAllocateDescriptorSets(r.d,&ds,&r.set)!=VK_SUCCESS)return std::nullopt;
    const VkDescriptorImageInfo image{r.sampler,r.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo output{r.buffer,0,bytes};std::array<VkWriteDescriptorSet,2> writes{};
    for(unsigned i=0;i<2;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=r.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=sizes[i].type;}
    writes[0].pImageInfo=&image;writes[1].pBufferInfo=&output;vkUpdateDescriptorSets(r.d,2,writes.data(),0,nullptr);
    VkCommandPoolCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cp.queueFamilyIndex=d.queueFamily;
    if(vkCreateCommandPool(r.d,&cp,nullptr,&r.pool)!=VK_SUCCESS)return std::nullopt;
    VkCommandBufferAllocateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cb.commandPool=r.pool;
    cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;if(vkAllocateCommandBuffers(r.d,&cb,&r.command)!=VK_SUCCESS)return std::nullopt;
    VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;if(vkCreateFence(r.d,&fence,nullptr,&r.fence)!=VK_SUCCESS)return std::nullopt;
    std::memset(r.mapped,0x5A,bytes);
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return std::nullopt;
    VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;barrier.image=source;
    barrier.subresourceRange={VkImageAspectFlags(depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,1,&barrier);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&r.set,0,nullptr);
    const unsigned shape[]{width,height,samples};vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,12,shape);vkCmdDispatch(r.command,(width+7)/8,(height+7)/8,1);
    barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,1,&barrier);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return std::nullopt;
    const auto* raw=static_cast<const std::uint8_t*>(r.mapped);
    if(!std::all_of(raw,raw+bytes,[](auto byte){return byte==0x5A;}) || vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return std::nullopt;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(d.queue,1,&submit,r.fence)!=VK_SUCCESS || vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
    if(!std::all_of(raw,raw+16,[](auto byte){return byte==0x5A;}) || !std::all_of(raw+bytes-16,raw+bytes,[](auto byte){return byte==0x5A;}))return std::nullopt;
    const auto* words=static_cast<const std::uint32_t*>(r.mapped);std::vector<std::uint8_t> result(count*4);
    for(unsigned i=0;i<count;++i) {
        if(words[5+i*2]!=0xC01AC01Au)return std::nullopt;
        if(depth){float value;std::memcpy(&value,words+4+i*2,4);if(!std::isfinite(value) || value<0 || value>1)return std::nullopt;}
        for(unsigned c=0;c<4;++c)result[i*4+c]=words[4+i*2]>>(c*8);
    }
    return result;
}
inline std::optional<std::vector<std::uint8_t>> snapshotNativeColorSamples(ProbeDevice& d,VkImage source,
        unsigned width,unsigned height,unsigned samples,const std::vector<std::uint32_t>& code) {
    return snapshotNativeSampleWords(d,source,width,height,samples,code,false);
}
// Actual expanded20e4 depth as little-endian float32 words, pixel/sample-major.
// Depth-only sampled view; both aspects transition together, neither is written.
inline std::optional<std::vector<std::uint8_t>> snapshotNativeDepthSamples(ProbeDevice& d,VkImage source,
        unsigned width,unsigned height,unsigned samples,const std::vector<std::uint32_t>& code) {
    return snapshotNativeSampleWords(d,source,width,height,samples,code,true);
}
}
