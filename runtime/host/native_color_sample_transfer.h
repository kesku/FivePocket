#pragma once
#include "device.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace recomp::gpu {
// Fenced, per-sample color conversion without averaging or hardware sRGB.
// The two images are explicitly BORROWED from an owner which must serialize
// queue access and keep them alive longer than this object. Source must have
// SAMPLED usage and TRANSFER_SRC layout; destination must have COLOR_ATTACHMENT
// usage and either UNDEFINED (first use) or TRANSFER_SRC layout. Formats and
// extents/sample counts are the owner's contract. No game/resolve admission.
// Every sample is written by a separate full-screen draw with a one-bit fixed
// sample mask. No optional sample-rate shading or multisample storage required.
// Both images end in TRANSFER_SRC after a fence, even on validation failure.
// Failure after submission makes destination contents invalid for publication.
// createDepthPreservingStencil uses the same witnessed transfer machinery for
// D32/S8 depth-only copy/conversion. The destination MUST be initialized and its
// stencil is loaded/stored unchanged. Neither color nor source depth is written.
class NativeColorSampleTransfer {
    ProbeDevice& device_;
    unsigned width_,height_,samples_;
    VkImage source_,destination_;
    bool decode_;
    bool depth_=false;
    struct Resources {
        VkDevice d;std::array<VkImageView,2> views{};VkSampler sampler{};
        VkRenderPass pass{};VkFramebuffer framebuffer{};
        VkBuffer status{};VkDeviceMemory memory{};void* mapped{};
        std::array<VkShaderModule,2> shaders{};std::array<VkPipeline,4> pipelines{};
        VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkDescriptorPool descriptors{};VkDescriptorSet set{};
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            for(auto p:pipelines)if(p)vkDestroyPipeline(d,p,nullptr);
            if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
            if(pass)vkDestroyRenderPass(d,pass,nullptr);
            if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
            if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
            if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
            for(auto s:shaders)if(s)vkDestroyShaderModule(d,s,nullptr);
            if(mapped)vkUnmapMemory(d,memory);
            if(status)vkDestroyBuffer(d,status,nullptr);
            if(memory)vkFreeMemory(d,memory,nullptr);
            if(sampler)vkDestroySampler(d,sampler,nullptr);
            for(auto v:views)if(v)vkDestroyImageView(d,v,nullptr);
        }
    } r_;
    NativeColorSampleTransfer(ProbeDevice& d,unsigned w,unsigned h,unsigned n,VkImage src,VkImage dst,bool decode):
        device_(d),width_(w),height_(h),samples_(n),source_(src),destination_(dst),decode_(decode),r_{d.device}{}
    bool setup(const std::vector<std::uint32_t>& vertex,const std::vector<std::uint32_t>& fragment) {
        const auto format=[&](unsigned i){return depth_?VK_FORMAT_D32_SFLOAT_S8_UINT:
            ((i==0)==decode_)?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R32G32B32A32_SFLOAT;};
        for(unsigned i=0;i<2;++i) {
            VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=i?destination_:source_;
            v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format(i);
            v.subresourceRange={depth_?VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|(i?VK_IMAGE_ASPECT_STENCIL_BIT:0)):
                VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            if(vkCreateImageView(r_.d,&v,nullptr,&r_.views[i])!=VK_SUCCESS)return false;
            const auto& code=i?fragment:vertex;
            if(code.size()<5 || code.size()>65536 || code[0]!=0x07230203)return false;
            VkShaderModuleCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;s.codeSize=code.size()*4;s.pCode=code.data();
            if(vkCreateShaderModule(r_.d,&s,nullptr,&r_.shaders[i])!=VK_SUCCESS)return false;
        }
        VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(vkCreateSampler(r_.d,&sampler,nullptr,&r_.sampler)!=VK_SUCCESS)return false;
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;buffer.size=13*4;buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(vkCreateBuffer(r_.d,&buffer,nullptr,&r_.status)!=VK_SUCCESS)return false;
        VkMemoryRequirements required{};vkGetBufferMemoryRequirements(r_.d,r_.status,&required);
        VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&memory);
        unsigned type=memory.memoryTypeCount;
        for(unsigned i=0;i<memory.memoryTypeCount;++i)if((required.memoryTypeBits&(1u<<i)) &&
            (memory.memoryTypes[i].propertyFlags&6)==6){type=i;break;}
        if(type==memory.memoryTypeCount)return false;
        VkMemoryAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize=required.size;allocate.memoryTypeIndex=type;
        if(vkAllocateMemory(r_.d,&allocate,nullptr,&r_.memory)!=VK_SUCCESS ||
           vkBindBufferMemory(r_.d,r_.status,r_.memory,0)!=VK_SUCCESS ||
           vkMapMemory(r_.d,r_.memory,0,VK_WHOLE_SIZE,0,&r_.mapped)!=VK_SUCCESS)return false;
        const VkDescriptorSetLayoutBinding bindings[]{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=2;sl.pBindings=bindings;
        if(vkCreateDescriptorSetLayout(r_.d,&sl,nullptr,&r_.setLayout)!=VK_SUCCESS)return false;
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,8};
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount=1;pl.pSetLayouts=&r_.setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(r_.d,&pl,nullptr,&r_.layout)!=VK_SUCCESS)return false;
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=sizes;
        if(vkCreateDescriptorPool(r_.d,&dp,nullptr,&r_.descriptors)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds.descriptorPool=r_.descriptors;ds.descriptorSetCount=1;ds.pSetLayouts=&r_.setLayout;
        if(vkAllocateDescriptorSets(r_.d,&ds,&r_.set)!=VK_SUCCESS)return false;
        const VkDescriptorImageInfo image{r_.sampler,r_.views[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo storage{r_.status,0,13*4};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(unsigned i=0;i<2;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=r_.set;
            w.dstBinding=i;w.descriptorCount=1;w.descriptorType=sizes[i].type;}
        writes[0].pImageInfo=&image;writes[1].pBufferInfo=&storage;vkUpdateDescriptorSets(r_.d,2,writes.data(),0,nullptr);
        VkAttachmentDescription attachment{};attachment.format=format(1);attachment.samples=static_cast<VkSampleCountFlagBits>(samples_);
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=depth_?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp=depth_?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=depth_?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkAttachmentReference color{0,attachment.initialLayout};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        if(depth_)subpass.pDepthStencilAttachment=&color;else{subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;}
        VkSubpassDependency dependency{};dependency.srcSubpass=0;dependency.dstSubpass=VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask=depth_?VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT:VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.srcAccessMask=depth_?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;pass.dependencyCount=1;pass.pDependencies=&dependency;
        if(vkCreateRenderPass(r_.d,&pass,nullptr,&r_.pass)!=VK_SUCCESS)return false;
        VkFramebufferCreateInfo fb{};fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fb.renderPass=r_.pass;
        fb.attachmentCount=1;fb.pAttachments=&r_.views[1];fb.width=width_;fb.height=height_;fb.layers=1;
        if(vkCreateFramebuffer(r_.d,&fb,nullptr,&r_.framebuffer)!=VK_SUCCESS)return false;
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for(unsigned i=0;i<2;++i){auto& s=stages[i];s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;s.module=r_.shaders[i];s.pName="main";}
        VkPipelineVertexInputStateCreateInfo vertexInput{};vertexInput.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        const VkViewport viewport{0,0,float(width_),float(height_),0,1};const VkRect2D scissor{{0,0},{width_,height_}};
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples=static_cast<VkSampleCountFlagBits>(samples_);
        VkPipelineColorBlendAttachmentState colorWrite{};colorWrite.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount=depth_?0:1;blend.pAttachments=depth_?nullptr:&colorWrite;
        VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        VkGraphicsPipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount=2;pipeline.pStages=stages.data();pipeline.pVertexInputState=&vertexInput;pipeline.pInputAssemblyState=&assembly;
        pipeline.pViewportState=&vp;pipeline.pRasterizationState=&raster;pipeline.pMultisampleState=&ms;pipeline.pColorBlendState=&blend;
        pipeline.layout=r_.layout;pipeline.renderPass=r_.pass;
        if(depth_)pipeline.pDepthStencilState=&depth;
        for(unsigned sample=0;sample<samples_;++sample) {
            const VkSampleMask mask=1u<<sample;ms.pSampleMask=&mask;
            if(vkCreateGraphicsPipelines(r_.d,VK_NULL_HANDLE,1,&pipeline,nullptr,&r_.pipelines[sample])!=VK_SUCCESS)return false;
        }
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r_.d,&pool,nullptr,&r_.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo command{};command.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool=r_.pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r_.d,&command,&r_.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r_.d,&fence,nullptr,&r_.fence)==VK_SUCCESS;
    }
public:
    static std::unique_ptr<NativeColorSampleTransfer> create(ProbeDevice& d,unsigned w,unsigned h,unsigned n,
            VkImage source,VkImage destination,bool decode,const std::vector<std::uint32_t>& vertex,const std::vector<std::uint32_t>& fragment) {
        if(!d.device || !d.queue || !w || !h || w>1280 || h>720 || (n!=1 && n!=2 && n!=4) || !source || !destination || source==destination)return nullptr;
        // Physical support alone is insufficient: creation explicitly requests
        // this optional feature and records what the logical device enabled.
        if(!d.fragmentStoresAndAtomicsEnabled)return nullptr;
        auto result=std::unique_ptr<NativeColorSampleTransfer>(new NativeColorSampleTransfer(d,w,h,n,source,destination,decode));
        return result->setup(vertex,fragment)?std::move(result):nullptr;
    }
    NativeColorSampleTransfer(const NativeColorSampleTransfer&)=delete;
    NativeColorSampleTransfer& operator=(const NativeColorSampleTransfer&)=delete;
    static std::unique_ptr<NativeColorSampleTransfer> createDepthPreservingStencil(ProbeDevice& d,unsigned w,unsigned h,unsigned n,
            VkImage source,VkImage destination,const std::vector<std::uint32_t>& vertex,const std::vector<std::uint32_t>& fragment) {
        if(!d.device || !d.queue || !d.fragmentStoresAndAtomicsEnabled || !w || !h || w>1280 || h>720 ||
            (n!=1 && n!=2 && n!=4) || !source || !destination || source==destination)return nullptr;
        auto result=std::unique_ptr<NativeColorSampleTransfer>(new NativeColorSampleTransfer(d,w,h,n,source,destination,false));
        result->depth_=true;
        return result->setup(vertex,fragment)?std::move(result):nullptr;
    }
    bool transfer(unsigned colorFormat,bool destinationInitialized) {
        // Depth mode0 copies D32, mode1 truncates to20e4 then expands exactly,
        // mode2 additionally rejects positive depth rounded down to zero.
        // Existing stencil must not be discarded via UNDEFINED/DONT_CARE.
        if(colorFormat>(depth_?2u:1u) || (depth_ && !destinationInitialized))return false;
        if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return false;
        std::array<std::uint32_t,13> initial{};initial.fill(0x5A5A5A5A);std::fill(initial.begin()+4,initial.begin()+9,0);
        std::memcpy(r_.mapped,initial.data(),sizeof(initial));
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return false;
        std::array<VkImageMemoryBarrier,2> images{};
        for(unsigned i=0;i<2;++i){auto& b=images[i];b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.image=i?destination_:source_;b.subresourceRange={depth_?VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):
                VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            b.oldLayout=i && !destinationInitialized?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout=i?(depth_?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL):VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask=i && !destinationInitialized?0:VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask=i?(depth_?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT):VK_ACCESS_SHADER_READ_BIT;}
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,1,&host,0,nullptr,2,images.data());
        VkRenderPassBeginInfo render{};render.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;render.renderPass=r_.pass;
        render.framebuffer=r_.framebuffer;render.renderArea={{0,0},{width_,height_}};
        vkCmdBeginRenderPass(r_.command,&render,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(r_.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r_.layout,0,1,&r_.set,0,nullptr);
        for(unsigned sample=0;sample<samples_;++sample) {
            const unsigned selection[]{sample,colorFormat};
            vkCmdBindPipeline(r_.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r_.pipelines[sample]);
            vkCmdPushConstants(r_.command,r_.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,8,selection);
            vkCmdDraw(r_.command,3,1,0,0);
        }
        vkCmdEndRenderPass(r_.command);
        auto& source=images[0];source.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;source.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        source.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;source.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,1,&source);
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS || std::memcmp(r_.mapped,initial.data(),sizeof(initial)) ||
            vkGetFenceStatus(r_.d,r_.fence)!=VK_NOT_READY)return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
           vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        const auto* status=static_cast<const std::uint32_t*>(r_.mapped);
        for(unsigned i=0;i<13;++i) {
            const auto expected=i>=4 && i<4+samples_?width_*height_:initial[i];
            if(status[i]!=expected)return false;
        }
        return true;
    }
};
}
