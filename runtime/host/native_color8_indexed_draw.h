#pragma once
#include "native_translated_resources.h"
#include <cstdlib>
#include <memory>

namespace recomp::gpu {
// Draws directly into completed, explicitly typed color/D32S8 images. ABI13
// color8 uses sample depth; ABI17 fixed16 uses explicit shader depth. They
// share Vulkan resource mechanics, NOT shader/state/format admission. ABI19
// expands each original three-vertex rectangle in the VS using vkCmdDraw;
// it never masquerades as an indexed guest submission.
// Replacement blending only. No implicit clears, resolves, target copies or
// game admission. Owner serializes the queue and invalidates on any failure.
enum class NativeIndexedColorEncoding { Color8, Fixed16 };
template<NativeIndexedColorEncoding Encoding> class NativeOwnedIndexedDraw {
    static_assert(Encoding==NativeIndexedColorEncoding::Color8 || Encoding==NativeIndexedColorEncoding::Fixed16);
    static constexpr bool Fixed16=Encoding==NativeIndexedColorEncoding::Fixed16;
    ProbeDevice& device_;
    unsigned width_,height_,samples_;
    std::uint32_t lastFragmentCount_=0;
    struct Resources {
        VkDevice d;std::array<VkImageView,2> views{};VkRenderPass pass{};VkFramebuffer framebuffer{};
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
            if(pass)vkDestroyRenderPass(d,pass,nullptr);
            for(auto v:views)if(v)vkDestroyImageView(d,v,nullptr);
        }
    } r_;
    NativeOwnedIndexedDraw(ProbeDevice& d,unsigned w,unsigned h,unsigned s):device_(d),width_(w),height_(h),samples_(s),r_{d.device}{}
    bool setup(VkImage color,VkImage depth) {
        std::array<VkAttachmentDescription,2> attachments{};
        for(unsigned i=0;i<2;++i) {
            auto& a=attachments[i];a.format=i?VK_FORMAT_D32_SFLOAT_S8_UINT:Fixed16?VK_FORMAT_R16G16_SNORM:VK_FORMAT_R8G8B8A8_UNORM;
            a.samples=static_cast<VkSampleCountFlagBits>(samples_);
            a.loadOp=a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp=a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            v.image=i?depth:color;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=a.format;
            v.subresourceRange={static_cast<VkImageAspectFlags>(i?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            if(vkCreateImageView(r_.d,&v,nullptr,&r_.views[i])!=VK_SUCCESS)return false;
        }
        const VkAttachmentReference colorRef{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depthRef{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=1;subpass.pColorAttachments=&colorRef;subpass.pDepthStencilAttachment=&depthRef;
        const auto stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        const auto access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        std::array<VkSubpassDependency,2> dependencies{};
        dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
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
        fb.attachmentCount=2;fb.pAttachments=r_.views.data();fb.width=width_;fb.height=height_;fb.layers=1;
        if(vkCreateFramebuffer(r_.d,&fb,nullptr,&r_.framebuffer)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r_.d,&pool,nullptr,&r_.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo cmd{};cmd.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cmd.commandPool=r_.pool;
        cmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cmd.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r_.d,&cmd,&r_.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r_.d,&f,nullptr,&r_.fence)==VK_SUCCESS;
    }
public:
    std::uint32_t diagnosticFragmentCount()const{return lastFragmentCount_;}
    static std::unique_ptr<NativeOwnedIndexedDraw> create(ProbeDevice& d,VkImage color,VkImage depth,unsigned w,unsigned h,unsigned s) {
        if(!d.device || !d.queue || !color || !depth || color==depth || !w || !h || w>1280 || h>720 ||
           (Fixed16?s!=1:(s!=1 && s!=2 && s!=4)))return nullptr;
        auto result=std::unique_ptr<NativeOwnedIndexedDraw>(new NativeOwnedIndexedDraw(d,w,h,s));
        return result->setup(color,depth)?std::move(result):nullptr;
    }
    bool draw(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::vector<std::uint32_t>& indices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const std::vector<XenosTextureDescription>& descriptions,const NativeTranslatedPipelineOptions& options) {
        lastFragmentCount_=0;
        const bool rectangle=Fixed16 && validateNativeFixed16RectangleProgram(program);
        const bool encoding=Fixed16?
            (rectangle || validateNativeFixed16Program(program)) && descriptions.size()==nativeTranslatedTextureSlots(program).size():
            program.abiVersion==13 && !program.geometryFixed16GradientDepth && program.geometryColor8Format<=1 && program.geometrySampleDepth;
        if(!encoding || program.indexed==rectangle || !program.geometryDepth20e4 ||
           !program.stride || program.stride%4 || !vertices || vertices>65536 || input.size()>NativeTranslatedResources::InputBytes ||
           input.size()!=8208+std::uint64_t{vertices}*program.stride ||
           (rectangle?(!indices.empty() || vertices%3):(indices.empty() || indices.size()>65535 || indices.size()%3)) ||
           options.samples!=samples_ || !options.depthStencil || options.sampleShading==Fixed16 ||
           descriptions.size()!=options.externalTextures.size())return false;
        const auto word=[&](unsigned at){return (unsigned(input[at])<<24)|(unsigned(input[at+1])<<16)|(unsigned(input[at+2])<<8)|input[at+3];};
        if(word(0)!=vertices || word(4)!=descriptions.size() || std::any_of(indices.begin(),indices.end(),[&](auto i){return i>=vertices;}) ||
           std::any_of(viewport.begin(),viewport.end(),[](float v){return !std::isfinite(v);}) ||
           scissor[0]>width_ || scissor[1]>height_ || scissor[2]>width_-scissor[0] || scissor[3]>height_-scissor[1])return false;
        NativeTranslatedResources pipeline(r_.d);
        if(!pipeline.setup(device_,r_.pass,program,descriptions,NativeTranslatedBlend::Replace,options))return false;
        std::memcpy(pipeline.inputMapped,input.data(),input.size());
        if(!rectangle)std::memcpy(pipeline.indexMapped,indices.data(),indices.size()*4);
        if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return false;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return false;
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&host,0,nullptr,0,nullptr);
        VkRenderPassBeginInfo render{};render.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;render.renderPass=r_.pass;
        render.framebuffer=r_.framebuffer;render.renderArea={{0,0},{width_,height_}};
        vkCmdBeginRenderPass(r_.command,&render,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(r_.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.pipeline);
        vkCmdBindDescriptorSets(r_.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.layout,0,1,&pipeline.set,0,nullptr);
        const VkRect2D rect{{static_cast<int>(scissor[0]),static_cast<int>(scissor[1])},{scissor[2],scissor[3]}};
        vkCmdSetScissor(r_.command,0,1,&rect);
        const std::array<float,6> viewportAndDepth{viewport[0],viewport[1],viewport[2],viewport[3],options.minDepth,options.maxDepth};
        vkCmdPushConstants(r_.command,pipeline.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,24,viewportAndDepth.data());
        if(rectangle)vkCmdDraw(r_.command,(vertices/3)*6,1,0,0);
        else {
            vkCmdBindIndexBuffer(r_.command,pipeline.indices,0,VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(r_.command,static_cast<unsigned>(indices.size()),1,0,0,0);
        }
        vkCmdEndRenderPass(r_.command);
        host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS)return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
           vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        const auto* completion=static_cast<const std::uint32_t*>(pipeline.completionMapped);
        for(unsigned i=0;i<16;++i)if(i!=4 && i!=5 && completion[i]!=0x5A5A5A5Au)return false;
        lastFragmentCount_=completion[5];
        return completion[4]==0 && std::memcmp(pipeline.inputMapped,input.data(),input.size())==0 &&
            (rectangle || std::memcmp(pipeline.indexMapped,indices.data(),indices.size()*4)==0);
    }
};
using NativeColor8IndexedDraw=NativeOwnedIndexedDraw<NativeIndexedColorEncoding::Color8>;
using NativeFixed16IndexedDraw=NativeOwnedIndexedDraw<NativeIndexedColorEncoding::Fixed16>;
}
