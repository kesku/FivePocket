#pragma once
#include "native_translated_resources.h"
#include "native_color_sample_transfer.h"
#include "xenos_float24_clear.h"
#include <memory>
#include <cstdlib>
#include <cstring>

namespace recomp::gpu {
// Owned float sample storage for translated raster/blend work. Existing color
// may be initialized explicitly or decoded sample-by-sample from an encoded
// target, then loaded by subsequent draws. Depth/stencil defaults to explicit
// per-draw initialization; callers may now explicitly preserve completed native
// depth/stencil for ordered geometry. Attachment identity remains their duty.
// Not a gamma target, Xbox coverage oracle, game resolve or guest publication.
class NativeLinearSampleTarget {
    ProbeDevice& device_;
    unsigned width_,height_,samples_;
    bool drawn_=false,colorInitialized_=false,depthInitialized_=false;
    const char* lastFailure_=nullptr;
    std::optional<std::array<unsigned,2>> rejectedFloat_;
    struct Resources {
        VkDevice d;std::array<VkImage,3> image{};std::array<VkDeviceMemory,3> memory{};
        std::array<VkImageView,2> view{};VkRenderPass pass{};VkFramebuffer framebuffer{};
        VkBuffer output{};VkDeviceMemory outputMemory{};void* mapped{};
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(mapped)vkUnmapMemory(d,outputMemory);
            if(output)vkDestroyBuffer(d,output,nullptr);
            if(outputMemory)vkFreeMemory(d,outputMemory,nullptr);
            if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
            if(pass)vkDestroyRenderPass(d,pass,nullptr);
            for(auto v:view)if(v)vkDestroyImageView(d,v,nullptr);
            for(unsigned i=0;i<3;++i){if(image[i])vkDestroyImage(d,image[i],nullptr);if(memory[i])vkFreeMemory(d,memory[i],nullptr);}
        }
    } r_;
    bool allocate(const VkMemoryRequirements& required,VkMemoryPropertyFlags flags,VkDeviceMemory& result) {
        VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&p);
        for(unsigned i=0;i<p.memoryTypeCount;++i)if((required.memoryTypeBits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) {
            VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=required.size;a.memoryTypeIndex=i;
            return vkAllocateMemory(r_.d,&a,nullptr,&result)==VK_SUCCESS;
        }
        return false;
    }
    bool setup() {
        for(unsigned i=0;i<3;++i) {
            const auto format=i==1?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R32G32B32A32_SFLOAT;
            const auto samples=static_cast<VkSampleCountFlagBits>(i==2?1:samples_);
            const auto usage=i==1?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT:
                i==2?VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT:
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
            VkImageFormatProperties caps{};
            VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(device_.physical,format,&features);
            if(i==0 && !(features.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))return false;
            if(vkGetPhysicalDeviceImageFormatProperties(device_.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                usage,0,&caps)!=VK_SUCCESS || !(caps.sampleCounts&samples) || width_>caps.maxExtent.width || height_>caps.maxExtent.height)return false;
            VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
            image.format=format;image.extent={width_,height_,1};image.mipLevels=image.arrayLayers=1;
            image.samples=samples;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;
            if(vkCreateImage(r_.d,&image,nullptr,&r_.image[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements required{};vkGetImageMemoryRequirements(r_.d,r_.image[i],&required);
            if(!allocate(required,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,r_.memory[i]) ||
                vkBindImageMemory(r_.d,r_.image[i],r_.memory[i],0)!=VK_SUCCESS)return false;
            if(i<2) {
                VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=r_.image[i];v.viewType=VK_IMAGE_VIEW_TYPE_2D;
                v.format=format;v.subresourceRange={static_cast<VkImageAspectFlags>(i?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
                if(vkCreateImageView(r_.d,&v,nullptr,&r_.view[i])!=VK_SUCCESS)return false;
            }
        }
        std::array<VkAttachmentDescription,2> attachments{};
        for(unsigned i=0;i<2;++i) {
            auto& a=attachments[i];a.format=i?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R32G32B32A32_SFLOAT;
            a.samples=static_cast<VkSampleCountFlagBits>(samples_);a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.initialLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;a.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        const VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;subpass.pDepthStencilAttachment=&depth;
        std::array<VkSubpassDependency,2> dependencies{};
        const auto stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        const auto access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
        // LOAD may follow either an explicit transfer clear or a completed
        // render pass's color/depth/stencil writes. Include both producers;
        // a CPU fence alone is not the next attachment's memory dependency.
        dependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT|stages;dependencies[0].dstStageMask=stages;
        dependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|access;dependencies[0].dstAccessMask=access;
        dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask=stages;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask=access;dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass.attachmentCount=2;pass.pAttachments=attachments.data();pass.subpassCount=1;pass.pSubpasses=&subpass;
        pass.dependencyCount=2;pass.pDependencies=dependencies.data();
        if(vkCreateRenderPass(r_.d,&pass,nullptr,&r_.pass)!=VK_SUCCESS)return false;
        VkFramebufferCreateInfo fb{};fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fb.renderPass=r_.pass;
        fb.attachmentCount=2;fb.pAttachments=r_.view.data();fb.width=width_;fb.height=height_;fb.layers=1;
        if(vkCreateFramebuffer(r_.d,&fb,nullptr,&r_.framebuffer)!=VK_SUCCESS)return false;
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=std::uint64_t{width_}*height_*16+32;
        b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if(vkCreateBuffer(r_.d,&b,nullptr,&r_.output)!=VK_SUCCESS)return false;
        VkMemoryRequirements required{};vkGetBufferMemoryRequirements(r_.d,r_.output,&required);
        if(!allocate(required,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,r_.outputMemory) ||
            vkBindBufferMemory(r_.d,r_.output,r_.outputMemory,0)!=VK_SUCCESS ||
            vkMapMemory(r_.d,r_.outputMemory,0,VK_WHOLE_SIZE,0,&r_.mapped)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r_.d,&pool,nullptr,&r_.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo command{};command.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool=r_.pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r_.d,&command,&r_.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r_.d,&fence,nullptr,&r_.fence)==VK_SUCCESS;
    }
    NativeLinearSampleTarget(ProbeDevice& d,unsigned w,unsigned h,unsigned samples):device_(d),width_(w),height_(h),samples_(samples),r_{d.device}{}
public:
    // Diagnostics do not expose/accept failed color output or advance ownership.
    const char* lastDrawFailure()const{return lastFailure_;}
    std::optional<std::array<unsigned,2>> rejectedFloat()const{return rejectedFloat_;}
    // After a completed draw only. Borrowed for read-only sample diagnostics;
    // restore TRANSFER_SRC layout and fence before drawing again/destruction.
    VkImage borrowColorSamplesForReadOnlyDiagnostic()const{return drawn_?r_.image[0]:VK_NULL_HANDLE;}
    // D32/S8 native diagnostic only; not a D24FS8 writeback contract. Layout
    // restoration and queue fencing are the same obligation as color above.
    VkImage borrowDepthStencilSamplesForReadOnlyDiagnostic()const{return drawn_ && depthInitialized_?r_.image[1]:VK_NULL_HANDLE;}
    // Explicit owned initialization/write. Does not touch depth/stencil or
    // claim a game draw. No stale resolved readback is exposed.
    bool replaceColorFromEncoded(VkImage source,unsigned format,const std::vector<std::uint32_t>& vertex,
            const std::vector<std::uint32_t>& fragment) {
        if(format>1)return false;
        auto converter=NativeColorSampleTransfer::create(device_,width_,height_,samples_,source,r_.image[0],true,vertex,fragment);
        if(!converter)return false;
        drawn_=false;drawn_=converter->transfer(format,colorInitialized_);
        if(drawn_)colorInitialized_=true;return drawn_;
    }
    // Copy both aspects of a completed D32/S8 owner, sample-for-sample.
    // Caller proves format, allocation ownership and queue serialization.
    // Source remains TRANSFER_SRC; no historical clear replaces its contents.
    bool replaceDepthStencilFromNative(VkImage source,unsigned width,unsigned height,unsigned samples) {
        if(!source || source==r_.image[1] || width!=width_ || height!=height_ || samples!=samples_)return false;
        const bool initialized=depthInitialized_;depthInitialized_=false;
        if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return false;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return false;
        std::array<VkImageMemoryBarrier,2> barriers{};
        for(auto& b:barriers){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};}
        barriers[0].image=source;barriers[0].oldLayout=barriers[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image=r_.image[1];barriers[1].oldLayout=initialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcAccessMask=initialized?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT:0;
        barriers[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers.data());
        std::array<VkImageCopy,2> copies{};
        for(unsigned i=0;i<2;++i){const auto aspect=i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT;
            copies[i].srcSubresource={static_cast<VkImageAspectFlags>(aspect),0,0,1};copies[i].dstSubresource=copies[i].srcSubresource;
            copies[i].extent={width_,height_,1};}
        vkCmdCopyImage(r_.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.image[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,2,copies.data());
        auto& b=barriers[1];b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,nullptr,0,nullptr,1,&b);
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS)return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS || vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        depthInitialized_=true;return true;
    }
    // Explicit completed single-sample contents. No guessed clear, attachment
    // identity, format conversion or publication. Intended for a consumer that
    // has just read both disjoint ranges from its current physical owner.
    bool loadSingleSampleContents(const std::vector<float>& rgba,const std::vector<std::uint32_t>& depthWords) {
        const auto pixels=std::size_t{width_}*height_;
        if(samples_!=1 || rgba.size()!=pixels*4 || depthWords.size()!=pixels ||
           std::any_of(rgba.begin(),rgba.end(),[](float f){return !std::isfinite(f);}))return false;
        const bool colorWas=colorInitialized_,depthWas=depthInitialized_;
        drawn_=colorInitialized_=depthInitialized_=false;
        for(unsigned part=0;part<2;++part) {
            const auto bytes=part?pixels*5:pixels*16;
            std::memset(r_.mapped,0x5A,pixels*16+32);
            auto* data=static_cast<std::uint8_t*>(r_.mapped);
            if(!part)std::memcpy(data+16,rgba.data(),bytes);
            else for(std::size_t i=0;i<pixels;++i) {
                const float value=*decodeXenosFloat24(depthWords[i]>>8);
                std::memcpy(data+16+4*i,&value,4);data[16+4*pixels+i]=depthWords[i]&255;
            }
            if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return false;
            VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return false;
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.image=r_.image[part];b.subresourceRange={static_cast<VkImageAspectFlags>(part?
                VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.oldLayout=(part?depthWas:colorWas)?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcAccessMask=b.oldLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,1,&b);
            std::array<VkBufferImageCopy,2> copies{};
            copies[0].bufferOffset=16;copies[0].imageSubresource={static_cast<VkImageAspectFlags>(part?
                VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,0,1};
            copies[1].bufferOffset=16+pixels*4;copies[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};
            for(auto& c:copies)c.imageExtent={width_,height_,1};
            vkCmdCopyBufferToImage(r_.command,r_.output,r_.image[part],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                part?2:1,copies.data());
            b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_MEMORY_READ_BIT;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0,0,nullptr,0,nullptr,1,&b);
            if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS)return false;
            VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
            if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
               vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
            if(!std::all_of(data,data+16,[](auto b){return b==0x5A;}) ||
               !std::all_of(data+16+bytes,data+pixels*16+32,[](auto b){return b==0x5A;}))return false;
        }
        drawn_=colorInitialized_=depthInitialized_=true;return true;
    }
    // Exact representability readback, not arbitrary D32 ->20e4 conversion.
    // A read-only-depth consumer compares these against its imported raw words.
    std::optional<std::vector<std::uint32_t>> readSingleSampleDepthWords() {
        if(samples_!=1 || !drawn_ || !depthInitialized_)return std::nullopt;
        const auto pixels=std::size_t{width_}*height_,bytes=pixels*5;
        std::memset(r_.mapped,0x5A,pixels*16+32);
        if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return std::nullopt;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return std::nullopt;
        VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        std::array<VkBufferImageCopy,2> copies{};
        copies[0].bufferOffset=16;copies[0].imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        copies[1].bufferOffset=16+4*pixels;copies[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};
        for(auto& c:copies)c.imageExtent={width_,height_,1};
        vkCmdCopyImageToBuffer(r_.command,r_.image[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.output,2,copies.data());
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS)return std::nullopt;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
           vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        const auto* data=static_cast<const std::uint8_t*>(r_.mapped);
        if(!std::all_of(data,data+16,[](auto b){return b==0x5A;}) ||
           !std::all_of(data+16+bytes,data+pixels*16+32,[](auto b){return b==0x5A;}))return std::nullopt;
        std::vector<std::uint32_t> result(pixels);
        for(std::size_t i=0;i<pixels;++i) {
            float depth;std::memcpy(&depth,data+16+4*i,4);const auto encoded=encodeExactXenosFloat24(depth);
            if(!encoded)return std::nullopt;
            result[i]=(*encoded<<8)|data[16+4*pixels+i];
        }
        return result;
    }
    NativeLinearSampleTarget(const NativeLinearSampleTarget&)=delete;
    NativeLinearSampleTarget& operator=(const NativeLinearSampleTarget&)=delete;
    static std::unique_ptr<NativeLinearSampleTarget> create(ProbeDevice& d,unsigned width,unsigned height,unsigned samples) {
        if(!d.device || !d.queue || !width || !height || width>1280 || height>720 ||
            (samples!=1 && samples!=2 && samples!=4))return nullptr;
        auto result=std::unique_ptr<NativeLinearSampleTarget>(new NativeLinearSampleTarget(d,width,height,samples));
        return result->setup()?std::move(result):nullptr;
    }
    std::optional<std::vector<float>> draw(const NativeTranslatedDrawProgram& program,
            const std::vector<std::uint8_t>& input,unsigned vertices,const std::array<float,4>& viewport,
            const std::vector<XenosTextureDescription>& descriptions,const NativeTranslatedPipelineOptions& options,
            NativeTranslatedBlend blend,const std::array<float,4>& clear,float depth,unsigned stencil,bool loadExistingColor=false,
            const std::vector<std::uint32_t>& indices={},bool loadExistingDepth=false,
            const std::optional<std::array<unsigned,4>>& windowScissor=std::nullopt) {
        lastFailure_="input-validation";rejectedFloat_.reset();
        if(options.samples!=samples_ || !options.depthStencil || input.size()>NativeTranslatedResources::InputBytes ||
            input.size()<8208 || !vertices || vertices>196608 || !std::isfinite(depth) || depth<0 || depth>1 || stencil>255 ||
            (loadExistingColor && !drawn_) || (loadExistingDepth && !depthInitialized_) ||
            program.indexed!=!indices.empty())return std::nullopt;
        if(program.indexed) {
            // The ABI carries all vertices; gl_VertexIndex sees the caller's
            // already interpreted effective index. No second base offset.
            if((program.abiVersion!=12 && program.abiVersion!=30) || !program.stride || program.stride%4 || vertices>65536 ||
                indices.size()>65535 || indices.size()%3 ||
                input.size()!=8208+std::uint64_t{vertices}*program.stride)return std::nullopt;
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            if(count!=vertices || std::any_of(indices.begin(),indices.end(),[&](auto i){return i>=vertices;}))return std::nullopt;
        }
        if(program.abiVersion==35) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            if(!validateNativeFilterWeightsProgram(program) || width_!=640 || height_!=360 ||
               count!=3 || vertices!=6 || input.size()!=8208+72 || loadExistingColor || loadExistingDepth ||
               std::any_of(input.begin()+4,input.begin()+16,[](auto b){return b!=0;}))return std::nullopt;
        }
        if(program.abiVersion==41) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            // Exactly the two observed pass71 rows (native_expanded16_copy_plan.h).
            if(!validateNativeExpanded16CopyProgram(program) || !((width_==160 && height_==90) || (width_==80 && height_==45)) || samples_!=1 ||
               count!=3 || vertices!=6 || input.size()!=8208+72 || loadExistingColor || loadExistingDepth ||
               descriptions.size()!=1 || std::any_of(input.begin()+4,input.begin()+16,[](auto b){return b!=0;}))return std::nullopt;
        }
        if(program.abiVersion==42 || program.abiVersion==43) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            // Exactly the observed pass81/82 rows (native_postfx_blur_plan.h).
            if(!validateNativePostfxBlurFamilyProgram(program) || !((width_==80 && height_==45) || (width_==160 && height_==90) ||
               (width_==320 && height_==180)) || samples_!=1 ||
               count!=3 || vertices!=6 || input.size()!=8208+72 || loadExistingColor || loadExistingDepth ||
               descriptions.size()!=1 || std::any_of(input.begin()+4,input.begin()+16,[](auto b){return b!=0;}))return std::nullopt;
        }
        if(program.abiVersion==44) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            // Exactly the observed pass83 rows (native_postfx_upsample_plan.h),
            // source term only: never loads the target; the ONE/ONE add is the
            // consumer's, onto known physical words.
            if(!validateNativePostfxUpsampleProgram(program) || !((width_==160 && height_==90) || (width_==320 && height_==180)) || samples_!=1 ||
               count!=3 || vertices!=6 || input.size()!=8208+72 || loadExistingColor || loadExistingDepth ||
               descriptions.size()!=1 || std::any_of(input.begin()+4,input.begin()+16,[](auto b){return b!=0;}))return std::nullopt;
        }
        if(program.abiVersion==40) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            if(!validateNativeBrightPassProgram(program) || width_!=320 || height_!=180 || samples_!=1 ||
               count!=3 || vertices!=6 || input.size()!=8208+72 || loadExistingColor || loadExistingDepth ||
               descriptions.size()!=3 || std::any_of(input.begin()+4,input.begin()+16,[](auto b){return b!=0;}))return std::nullopt;
        }
        if(program.abiVersion==30 && (!validateNativeIndexedRgb7e3Program(program) ||
            !loadExistingColor || !loadExistingDepth ||
            std::any_of(input.begin()+4,input.begin()+16,[](auto b){return b!=0;})))return std::nullopt;
        if(program.screenStrip) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            if(!validateNativeScreenStripProgram(program) || count<3 || count>32768 || vertices!=(count-2)*3 ||
               input.size()!=8208+std::uint64_t{count}*8 || !loadExistingColor || !loadExistingDepth ||
               input[4] || input[5] || input[6] || input[7] || input[8] || input[9] || input[10] ||
               input[11] || input[12] || input[13] || input[14] || input[15])return std::nullopt;
        }
        if(program.worldStrip || program.abiVersion==29) {
            const auto count=(std::uint32_t{input[0]}<<24)|(std::uint32_t{input[1]}<<16)|
                (std::uint32_t{input[2]}<<8)|input[3];
            if(!validateNativeWorldStripProgram(program) || count!=4 || vertices!=6 ||
               input.size()!=8208+144 || !loadExistingColor || !loadExistingDepth ||
               input[4] || input[5] || input[6] || input[7] || input[8] || input[9] || input[10] ||
               input[11] || input[12] || input[13] || input[14] || input[15])return std::nullopt;
        }
        if(windowScissor) {
            const auto& s=*windowScissor;
            if(s[0]>width_ || s[1]>height_ || s[2]>width_-s[0] || s[3]>height_-s[1])return std::nullopt;
        }
        for(const auto c:clear)if(!std::isfinite(c))return std::nullopt;
        for(const auto c:viewport)if(!std::isfinite(c))return std::nullopt;
        drawn_=false;
        NativeTranslatedResources pipeline(r_.d);
        lastFailure_="pipeline-resources";
        if(!pipeline.setup(device_,r_.pass,program,descriptions,blend,options))return std::nullopt;
        std::memcpy(pipeline.inputMapped,input.data(),input.size());
        if(program.indexed)std::memcpy(pipeline.indexMapped,indices.data(),indices.size()*4);
        const auto bytes=width_*height_*16;std::memset(r_.mapped,0x5A,bytes+32);
        lastFailure_="command-begin";
        if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return std::nullopt;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return std::nullopt;
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
            VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&host,0,nullptr,0,nullptr);
        if(!loadExistingDepth) {
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.image=r_.image[1];b.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.oldLayout=depthInitialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcAccessMask=depthInitialized_?VK_ACCESS_TRANSFER_READ_BIT:0;
            b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            const VkClearDepthStencilValue value{depth,stencil};
            vkCmdClearDepthStencilImage(r_.command,r_.image[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&b.subresourceRange);
            b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        }
        depthInitialized_=false;
        if(!loadExistingColor) {
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.image=r_.image[0];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.oldLayout=colorInitialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcAccessMask=colorInitialized_?VK_ACCESS_TRANSFER_READ_BIT:0;
            b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkClearColorValue value{};std::copy(clear.begin(),clear.end(),value.float32);
            vkCmdClearColorImage(r_.command,r_.image[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&b.subresourceRange);
            b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        }
        std::array<VkClearValue,2> values{};std::copy(clear.begin(),clear.end(),values[0].color.float32);values[1].depthStencil={depth,stencil};
        VkRenderPassBeginInfo render{};render.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;render.renderPass=r_.pass;
        render.framebuffer=r_.framebuffer;render.renderArea={{0,0},{width_,height_}};render.clearValueCount=2;render.pClearValues=values.data();
        vkCmdBeginRenderPass(r_.command,&render,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(r_.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.pipeline);
        vkCmdBindDescriptorSets(r_.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.layout,0,1,&pipeline.set,0,nullptr);
        const auto s=windowScissor.value_or(std::array<unsigned,4>{0,0,width_,height_});
        const VkRect2D scissor{{static_cast<int>(s[0]),static_cast<int>(s[1])},{s[2],s[3]}};
        vkCmdSetScissor(r_.command,0,1,&scissor);
        vkCmdPushConstants(r_.command,pipeline.layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,viewport.data());
        if(program.indexed) {
            vkCmdBindIndexBuffer(r_.command,pipeline.indices,0,VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r_.command,static_cast<unsigned>(indices.size()),1,0,0,0);
        } else vkCmdDraw(r_.command,vertices,1,0,0);
        vkCmdEndRenderPass(r_.command);
        unsigned source=0;
        if(samples_>1) {
            VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.image=r_.image[2];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkImageResolve resolve{};resolve.srcSubresource=resolve.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};resolve.extent={width_,height_,1};
            vkCmdResolveImage(r_.command,r_.image[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.image[2],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&resolve);
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);source=2;
        }
        VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width_,height_,1};
        vkCmdCopyImageToBuffer(r_.command,r_.image[source],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.output,1,&copy);
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        lastFailure_="command-end";
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS)return std::nullopt;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
            vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        colorInitialized_=depthInitialized_=true;
        lastFailure_="output-or-input-guard";
        const auto* raw=static_cast<const std::uint8_t*>(r_.mapped);
        if(!std::all_of(raw,raw+16,[](auto b){return b==0x5A;}) ||
            !std::all_of(raw+bytes+16,raw+bytes+32,[](auto b){return b==0x5A;}) ||
            std::memcmp(pipeline.inputMapped,input.data(),input.size()) ||
            (program.indexed && std::memcmp(pipeline.indexMapped,indices.data(),indices.size()*4)))return std::nullopt;
        std::vector<float> result(bytes/4);std::memcpy(result.data(),raw+16,bytes);
        for(unsigned i=0;i<result.size();++i)if(!std::isfinite(result[i])) {
            unsigned bits;std::memcpy(&bits,&result[i],4);
            lastFailure_="nonfinite-color-output";rejectedFloat_=std::array<unsigned,2>{i,bits};return std::nullopt;
        }
        lastFailure_=nullptr;drawn_=true;return result;
    }
};
}
