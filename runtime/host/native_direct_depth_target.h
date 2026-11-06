#pragma once
#include "native_stencil_pipeline_options.h"
#include "xenos_float24_clear.h"
#include <memory>

namespace recomp::gpu {
// Single-sample physical-depth storage, NOT a tiled scene target. It imports
// complete known D24FS8 raw words, never a clear or a retained earlier scene.
// The live owner must prove current physical coverage and
// invalidate on every other writer. This primitive does not grant ownership.
// Only stencil-only writes are exposed; high24 depth bits must stay exact.
class NativeDirectDepthTarget {
    ProbeDevice& device_;
    NativeStencilOnlyPlan plan_;
    std::vector<std::uint32_t> original_;
    VkImage image_{};VkDeviceMemory memory_{};VkImageView view_{};
    VkRenderPass pass_{};VkFramebuffer frame_{};
    VkBuffer transfer_{};VkDeviceMemory transferMemory_{};void* mapped_=nullptr;
    VkCommandPool pool_{};VkCommandBuffer command_{};VkFence fence_{};
    std::unique_ptr<NativeTranslatedResources> pipeline_;
    std::size_t stencilOffset_=0,bytes_=0;
    bool usable_=true,pending_=false;
    NativeDirectDepthTarget(ProbeDevice& d,const NativeStencilOnlyPlan& p):device_(d),plan_(p){}
    bool allocate(const VkMemoryRequirements& r,VkMemoryPropertyFlags flags,VkDeviceMemory& result) {
        VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&p);
        for(unsigned i=0;i<p.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i)) &&
                (p.memoryTypes[i].propertyFlags&flags)==flags) {
            VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            a.allocationSize=r.size;a.memoryTypeIndex=i;
            return vkAllocateMemory(device_.device,&a,nullptr,&result)==VK_SUCCESS;
        }
        return false;
    }
    bool begin() {
        if(!usable_ || pending_ || vkResetCommandPool(device_.device,pool_,0)!=VK_SUCCESS ||
           vkResetFences(device_.device,1,&fence_)!=VK_SUCCESS)return false;
        VkCommandBufferBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(command_,&b)==VK_SUCCESS;
    }
    bool finish() {
        if(vkEndCommandBuffer(command_)!=VK_SUCCESS){usable_=false;return false;}
        VkSubmitInfo s{};s.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;s.commandBufferCount=1;s.pCommandBuffers=&command_;
        if(vkQueueSubmit(device_.queue,1,&s,fence_)!=VK_SUCCESS){usable_=false;return false;}
        pending_=true;
        if(vkWaitForFences(device_.device,1,&fence_,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){usable_=false;return false;}
        pending_=false;return true;
    }
    void barrier() {
        VkMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);
    }
    std::array<VkBufferImageCopy,2> copies()const {
        std::array<VkBufferImageCopy,2> c{};
        c[0].bufferOffset=16;c[1].bufferOffset=stencilOffset_;
        c[0].imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        c[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};
        for(auto& v:c)v.imageExtent={plan_.width,plan_.height,1};
        return c;
    }
    bool setup() {
        const auto d=device_.device;
        const VkImageUsageFlags usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImageFormatProperties caps{};
        if(vkGetPhysicalDeviceImageFormatProperties(device_.physical,VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&caps)!=VK_SUCCESS ||
            !(caps.sampleCounts&VK_SAMPLE_COUNT_1_BIT) || plan_.width>caps.maxExtent.width ||
            plan_.height>caps.maxExtent.height)return false;
        VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;
        i.format=VK_FORMAT_D32_SFLOAT_S8_UINT;i.extent={plan_.width,plan_.height,1};
        i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=usage;
        if(vkCreateImage(d,&i,nullptr,&image_)!=VK_SUCCESS)return false;
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(d,image_,&r);
        if(r.size>caps.maxResourceSize || !allocate(r,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,memory_) ||
           vkBindImageMemory(d,image_,memory_,0)!=VK_SUCCESS)return false;
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=image_;
        v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=i.format;
        v.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
        if(vkCreateImageView(d,&v,nullptr,&view_)!=VK_SUCCESS)return false;
        VkAttachmentDescription attachment{};attachment.format=i.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp=attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&ref;
        VkRenderPassCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        p.attachmentCount=1;p.pAttachments=&attachment;p.subpassCount=1;p.pSubpasses=&sub;
        if(vkCreateRenderPass(d,&p,nullptr,&pass_)!=VK_SUCCESS)return false;
        VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=pass_;
        f.attachmentCount=1;f.pAttachments=&view_;f.width=plan_.width;f.height=plan_.height;f.layers=1;
        if(vkCreateFramebuffer(d,&f,nullptr,&frame_)!=VK_SUCCESS)return false;
        stencilOffset_=32+original_.size()*4;bytes_=(stencilOffset_+original_.size()+15)/16*16+16;
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=bytes_;
        b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(d,&b,nullptr,&transfer_)!=VK_SUCCESS)return false;
        vkGetBufferMemoryRequirements(d,transfer_,&r);
        if(!allocate(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,transferMemory_) ||
           vkBindBufferMemory(d,transfer_,transferMemory_,0)!=VK_SUCCESS ||
           vkMapMemory(d,transferMemory_,0,VK_WHOLE_SIZE,0,&mapped_)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(d,&pool,nullptr,&pool_)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo c{};c.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;c.commandPool=pool_;
        c.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;c.commandBufferCount=1;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if(vkAllocateCommandBuffers(d,&c,&command_)!=VK_SUCCESS || vkCreateFence(d,&fence,nullptr,&fence_)!=VK_SUCCESS)return false;
        auto* bytes=static_cast<std::uint8_t*>(mapped_);std::memset(bytes,0x5A,bytes_);
        for(std::size_t n=0;n<original_.size();++n) {
            const float depth=*decodeXenosFloat24(original_[n]>>8);
            std::memcpy(bytes+16+4*n,&depth,4);bytes[stencilOffset_+n]=original_[n]&255;
        }
        if(!begin())return false;
        VkImageMemoryBarrier transition{};transition.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        transition.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;transition.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        transition.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;transition.image=image_;
        transition.srcQueueFamilyIndex=transition.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        transition.subresourceRange=v.subresourceRange;
        vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&transition);
        barrier();const auto regions=copies();
        vkCmdCopyBufferToImage(command_,transfer_,image_,VK_IMAGE_LAYOUT_GENERAL,regions.size(),regions.data());
        barrier();return finish();
    }
public:
    ~NativeDirectDepthTarget() {
        const auto d=device_.device;if(pending_)vkQueueWaitIdle(device_.queue);
        pipeline_.reset();
        if(fence_)vkDestroyFence(d,fence_,nullptr);if(pool_)vkDestroyCommandPool(d,pool_,nullptr);
        if(mapped_)vkUnmapMemory(d,transferMemory_);
        if(transfer_)vkDestroyBuffer(d,transfer_,nullptr);if(transferMemory_)vkFreeMemory(d,transferMemory_,nullptr);
        if(frame_)vkDestroyFramebuffer(d,frame_,nullptr);if(pass_)vkDestroyRenderPass(d,pass_,nullptr);
        if(view_)vkDestroyImageView(d,view_,nullptr);if(image_)vkDestroyImage(d,image_,nullptr);
        if(memory_)vkFreeMemory(d,memory_,nullptr);
    }
    NativeDirectDepthTarget(const NativeDirectDepthTarget&)=delete;
    static std::unique_ptr<NativeDirectDepthTarget> create(ProbeDevice& d,const NativeStencilOnlyPlan& plan,
            const std::vector<std::uint32_t>& completedWords) {
        if(!d.device || !d.queue || !plan.depthObject || plan.depthObject%4 || !plan.width || !plan.height ||
           plan.width>2048 || plan.height>1024 || // Current native raster canvas.
           plan.pitch<plan.width || plan.pitch%80 || plan.depthBase>=2048 ||
           std::uint64_t{plan.pitch/80}*((std::uint64_t{plan.height}+15)/16)>2048-plan.depthBase ||
           std::uint64_t{plan.width}*plan.height>4u*1024*1024 ||
           completedWords.size()!=std::uint64_t{plan.width}*plan.height)return nullptr;
        auto result=std::unique_ptr<NativeDirectDepthTarget>(new NativeDirectDepthTarget(d,plan));
        result->original_=completedWords;
        return result->setup()?std::move(result):nullptr;
    }
    std::optional<std::vector<std::uint32_t>> readWords() {
        if(!usable_ || pending_)return std::nullopt;
        std::memset(mapped_,0x5A,bytes_);if(!begin())return std::nullopt;
        barrier();const auto regions=copies();
        vkCmdCopyImageToBuffer(command_,image_,VK_IMAGE_LAYOUT_GENERAL,transfer_,regions.size(),regions.data());
        barrier();if(!finish())return std::nullopt;
        const auto* b=static_cast<const std::uint8_t*>(mapped_);
        const auto guard=[&](std::size_t at,std::size_t end){return std::all_of(b+at,b+end,[](auto v){return v==0x5A;});};
        if(!guard(0,16) || !guard(16+4*original_.size(),stencilOffset_) ||
           !guard(stencilOffset_+original_.size(),bytes_)){usable_=false;return std::nullopt;}
        auto result=original_;
        for(std::size_t n=0;n<result.size();++n) {
            float depth;std::memcpy(&depth,b+16+4*n,4);
            if(depth!=*decodeXenosFloat24(original_[n]>>8)){usable_=false;return std::nullopt;}
            result[n]=(result[n]&0xFFFFFF00u)|b[stencilOffset_+n];
        }
        return result;
    }
private:
    bool drawInternal(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,
            unsigned vertices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const std::vector<XenosTextureDescription>& descriptions,const std::vector<VkDescriptorImageInfo>& textures) {
        const bool sampled=validateNativeSampledStencilProgram(program);
        if(!usable_ || pending_ || (!sampled && !validateNativeStencilOnlyProgram(program)) || !vertices || vertices%3 ||
           (sampled?(descriptions.size()!=1 || textures.size()!=1):(!descriptions.empty() || !textures.empty())) ||
           vertices>4096 || input.size()!=8208+std::uint64_t{vertices}*program.stride ||
           input.size()>NativeTranslatedResources::InputBytes || !plan_.state.stencilEnable || plan_.state.depthWrite ||
           std::any_of(viewport.begin(),viewport.end(),[](auto f){return !std::isfinite(f);}) ||
           scissor[0]>plan_.width || scissor[1]>plan_.height || !scissor[2] || !scissor[3] ||
           scissor[2]>plan_.width-scissor[0] || scissor[3]>plan_.height-scissor[1])return false;
        const auto word=[&](unsigned at){return (std::uint32_t{input[at]}<<24)|(std::uint32_t{input[at+1]}<<16)|
            (std::uint32_t{input[at+2]}<<8)|input[at+3];};
        if(word(0)!=vertices || word(4)!=0 || word(8)!=0 || word(12)!=0)return false;
        pipeline_=std::make_unique<NativeTranslatedResources>(device_.device);
        auto options=nativeDirectStencilPipelineOptions(plan_);options.externalTextures=textures;
        if(!pipeline_->setup(device_,pass_,program,descriptions,NativeTranslatedBlend::Replace,options))return false;
        std::memcpy(pipeline_->inputMapped,input.data(),input.size());if(!begin())return false;
        barrier();VkRenderPassBeginInfo r{};r.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        r.renderPass=pass_;r.framebuffer=frame_;r.renderArea={{0,0},{plan_.width,plan_.height}};
        vkCmdBeginRenderPass(command_,&r,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_->pipeline);
        vkCmdBindDescriptorSets(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_->layout,0,1,&pipeline_->set,0,nullptr);
        VkRect2D clip{{static_cast<int>(scissor[0]),static_cast<int>(scissor[1])},{scissor[2],scissor[3]}};
        vkCmdSetScissor(command_,0,1,&clip);
        vkCmdPushConstants(command_,pipeline_->layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,viewport.data());
        vkCmdDraw(command_,vertices,1,0,0);vkCmdEndRenderPass(command_);barrier();if(!finish())return false;
        const auto* guards=static_cast<const std::uint32_t*>(pipeline_->completionMapped);
        bool valid=guards[4]==0 && std::memcmp(pipeline_->inputMapped,input.data(),input.size())==0;
        for(unsigned i=0;i<16;++i)if(i!=4 && i!=5 && guards[i]!=0x5A5A5A5Au)valid=false;
        if(!valid)usable_=false;
        return valid;
    }
public:
    // Live callers must first run actual state/identity/physical-owner proof.
    // The existing absent-PS entry point remains separate and unchanged.
    bool draw(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,
            unsigned vertices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor) {
        return validateNativeStencilOnlyProgram(program) && drawInternal(program,input,vertices,viewport,scissor,{},{});
    }
    bool drawSampled(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,
            unsigned vertices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const XenosTextureDescription& description,const VkDescriptorImageInfo& texture) {
        return validateNativeSampledStencilProgram(program) &&
            drawInternal(program,input,vertices,viewport,scissor,{description},{texture});
    }
};
}
