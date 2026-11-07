#pragma once
#include "rectangle_depth_target.h"
#include <cstring>
#include <vector>

// Standalone native depth -> canonical reference EDRAM producer. The backing
// buffer belongs to the caller. No CPU expected pixels are uploaded here.
namespace recomp::gpu {
struct DepthChainInitialization {
    VkDevice d;
    bool depthWrite=true,stencilWrite=true,skipDraw=false,skipStore=false;
    unsigned stencilReference=0;
    VkSampleMask sampleMask=15;
    VkBuffer sourceBuffer{};VkDeviceMemory sourceMemory{};void* sourceMapped{};
    std::vector<std::uint8_t> source;
    VkShaderModule vertexShader{},storeShader{};VkPipeline pipeline{},storePipeline{};
    VkPipelineLayout layout{};VkDescriptorSetLayout setLayout{};
    VkDescriptorPool descriptors{};VkDescriptorSet set{};
    VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};
    std::array<VkImageView,2> sampledViews{};
    VkRenderPass renderPass{};VkFramebuffer framebuffer{};
    explicit DepthChainInitialization(VkDevice device):d(device){}
    DepthChainInitialization(const DepthChainInitialization&)=delete;
    DepthChainInitialization& operator=(const DepthChainInitialization&)=delete;
    ~DepthChainInitialization(){
        if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
        if(storePipeline)vkDestroyPipeline(d,storePipeline,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        if(vertexShader)vkDestroyShaderModule(d,vertexShader,nullptr);
        if(storeShader)vkDestroyShaderModule(d,storeShader,nullptr);
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
        if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
        for(auto v:sampledViews)if(v)vkDestroyImageView(d,v,nullptr);
        if(colorView)vkDestroyImageView(d,colorView,nullptr);
        if(colorImage)vkDestroyImage(d,colorImage,nullptr);
        if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        if(sourceMapped)vkUnmapMemory(d,sourceMemory);
        if(sourceBuffer)vkDestroyBuffer(d,sourceBuffer,nullptr);
        if(sourceMemory)vkFreeMemory(d,sourceMemory,nullptr);
    }
    bool setup(ProbeDevice& device,const std::array<std::vector<std::uint32_t>,2>& code,
            VkBuffer backing,unsigned backingBytes){
        if(source.empty() || source.size()>4096 || source.size()%4 || backingBytes!=10485792 || sampleMask>15 ||
            !(device.properties.limits.sampledImageDepthSampleCounts&VK_SAMPLE_COUNT_4_BIT) ||
            !(device.properties.limits.sampledImageStencilSampleCounts&VK_SAMPLE_COUNT_4_BIT))return false;
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size=source.size();buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(vkCreateBuffer(d,&buffer,nullptr,&sourceBuffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,sourceBuffer,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
        unsigned type=props.memoryTypeCount;
        constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(unsigned i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) &&
            (props.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
        if(type==props.memoryTypeCount)return false;
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        if(vkAllocateMemory(d,&allocation,nullptr,&sourceMemory)!=VK_SUCCESS ||
            vkBindBufferMemory(d,sourceBuffer,sourceMemory,0)!=VK_SUCCESS ||
            vkMapMemory(d,sourceMemory,0,VK_WHOLE_SIZE,0,&sourceMapped)!=VK_SUCCESS)return false;
        std::memcpy(sourceMapped,source.data(),source.size());
        for(unsigned i=0;i<2;++i){
            VkShaderModuleCreateInfo module{};module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            module.codeSize=code[i].size()*4;module.pCode=code[i].data();
            if(vkCreateShaderModule(d,&module,nullptr,i?&storeShader:&vertexShader)!=VK_SUCCESS)return false;
        }
        const RectangleDepthConfig config{640,360,VK_SAMPLE_COUNT_4_BIT,true,sampleMask};
        if(!setupRectangleDepthTarget(device,*this,config))return false;
        for(unsigned i=0;i<2;++i){
            VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image=colorImage;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
            view.subresourceRange={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
            if(vkCreateImageView(d,&view,nullptr,&sampledViews[i])!=VK_SUCCESS)return false;
        }
        const std::array<VkDescriptorSetLayoutBinding,4> bindings{{
            {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {3,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {4,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo setInfo{};setInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount=bindings.size();setInfo.pBindings=bindings.data();
        if(vkCreateDescriptorSetLayout(d,&setInfo,nullptr,&setLayout)!=VK_SUCCESS)return false;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
        VkPipelineLayoutCreateInfo layoutInfo{};layoutInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;
        layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(d,&layoutInfo,nullptr,&layout)!=VK_SUCCESS ||
            !createRectangleDepthPipeline(*this,config))return false;
        VkComputePipelineCreateInfo compute{};compute.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;compute.stage.module=storeShader;
        compute.stage.pName="main";compute.layout=layout;
        if(vkCreateComputePipelines(d,VK_NULL_HANDLE,1,&compute,nullptr,&storePipeline)!=VK_SUCCESS)return false;
        const std::array<VkDescriptorPoolSize,2> counts{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2}}};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets=1;pool.poolSizeCount=counts.size();pool.pPoolSizes=counts.data();
        if(vkCreateDescriptorPool(d,&pool,nullptr,&descriptors)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool=descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(d,&alloc,&set)!=VK_SUCCESS)return false;
        const std::array<VkDescriptorBufferInfo,2> buffers{{{sourceBuffer,0,source.size()},{backing,0,backingBytes}}};
        const std::array<VkDescriptorImageInfo,2> images{{
            {VK_NULL_HANDLE,sampledViews[0],VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE,sampledViews[1],VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}}};
        std::array<VkWriteDescriptorSet,4> writes{};
        for(unsigned i=0;i<4;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet=set;w.dstBinding=bindings[i].binding;w.descriptorCount=1;w.descriptorType=bindings[i].descriptorType;
            if(i<2)w.pBufferInfo=&buffers[i];else w.pImageInfo=&images[i-2];}
        vkUpdateDescriptorSets(d,writes.size(),writes.data(),0,nullptr);
        return true;
    }
    void record(VkCommandBuffer command){
        // The caller's HOST->shader barrier covers source and defined backing.
        // The target's render-pass dependencies cover depth/stencil -> compute.
        VkClearValue clear{};clear.depthStencil={.125f,0xA5u};
        VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass=renderPass;pass.framebuffer=framebuffer;pass.renderArea={{0,0},{640,360}};
        pass.clearValueCount=1;pass.pClearValues=&clear;
        vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
        if(!skipDraw)vkCmdDraw(command,6,1,0,0);
        vkCmdEndRenderPass(command);
        if(!skipStore){
            const std::array<unsigned,2> controls{0,0};
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,storePipeline);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
            vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,controls.data());
            vkCmdDispatch(command,80,45,1);
        }
        VkMemoryBarrier packed{};packed.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        packed.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        packed.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,1,&packed,0,nullptr,0,nullptr);
    }
    bool unchanged()const{return std::memcmp(sourceMapped,source.data(),source.size())==0;}
};
}
