#pragma once
#include "native_rectangle_mrt_plan.h"
#include "native_translated_resources.h"
#include "native_rectangle_list.h"
#include "xenos_texture_2d.h"
#include "native_texture_image.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <span>

namespace recomp::gpu {
// ABI22 two-attachment rectangle target.
//
// The pass writes BOTH colour attachments in ONE render pass, because its
// pixel stage exports colour 0 and colour 1 from the same invocation. Running
// it twice into two single-attachment targets would execute the shader twice
// and is not the same draw, so this owns a genuine two-attachment framebuffer.
//
// It carries no depth attachment: the plan has already required that depth and
// stencil writes are disabled. Both images are read back independently so each
// target can be served separately afterwards.
//
// Nothing here presents, publishes to the guest, or infers an EDRAM alias.
class NativeRectangleMrtTarget {
    ProbeDevice& device_;
    NativeRectangleMrtPlan plan_;
    struct Attachment {
        VkImage image{};VkDeviceMemory memory{};VkImageView view{};
        VkBuffer output{};VkDeviceMemory outputMemory{};void* mapped=nullptr;
    };
    struct Resources {
        VkDevice d;
        std::array<Attachment,2> attachments{};
        VkRenderPass renderPass{};VkFramebuffer framebuffer{};
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};VkSampler sampler{};
        explicit Resources(VkDevice device):d(device) {}
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(sampler)vkDestroySampler(d,sampler,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
            if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
            for(auto& a:attachments) {
                if(a.mapped)vkUnmapMemory(d,a.outputMemory);
                if(a.output)vkDestroyBuffer(d,a.output,nullptr);
                if(a.outputMemory)vkFreeMemory(d,a.outputMemory,nullptr);
                if(a.view)vkDestroyImageView(d,a.view,nullptr);
                if(a.image)vkDestroyImage(d,a.image,nullptr);
                if(a.memory)vkFreeMemory(d,a.memory,nullptr);
            }
        }
    } r_;
    std::unique_ptr<NativeTranslatedResources> shader_;
    std::vector<std::array<std::uint32_t,6>> fetches_;
    std::vector<std::unique_ptr<NativeTextureImage>> images_;
    VkSampler sampler_{};
    bool valid_=false;
    unsigned width()const{return plan_.targets[0].width;}
    unsigned height()const{return plan_.targets[0].height;}
    // Each attachment takes the format its OWN surface header declares. An
    // 8-bit attachment would silently quantise these floating-point targets.
    static constexpr VkFormat attachmentFormat(unsigned format) {
        return format==7?VK_FORMAT_R16G16B16A16_SFLOAT:VK_FORMAT_R32_SFLOAT;
    }
    static constexpr unsigned attachmentTexelBytes(unsigned format) {
        return format==7?8u:4u;
    }
    unsigned pixelBytes(unsigned slot)const{return width()*height()*attachmentTexelBytes(plan_.formats[slot]);}

    explicit NativeRectangleMrtTarget(ProbeDevice& device,NativeRectangleMrtPlan plan)
        :device_(device),plan_(plan),r_(device.device) {}

    bool deviceMemory(VkMemoryRequirements req,VkMemoryPropertyFlags flags,VkDeviceMemory* out) {
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&props);
        unsigned type=props.memoryTypeCount;
        for(unsigned i=0;i<props.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
        if(type==props.memoryTypeCount)return false;
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        return vkAllocateMemory(r_.d,&allocation,nullptr,out)==VK_SUCCESS;
    }
    bool setup() {
        auto& r=r_;
        const auto& limits=device_.properties.limits;
        if(!width() || !height() || width()>1280 || height()>720 ||
           limits.maxFramebufferWidth<width() || limits.maxFramebufferHeight<height() ||
           limits.maxColorAttachments<2)return false;
        constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        for(unsigned slot=0;slot<2;++slot) {
            if(!nativeRectangleMrtFormatSupported(plan_.formats[slot]))return false;
            const auto format=attachmentFormat(plan_.formats[slot]);
            VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(device_.physical,format,&features);
            if((features.optimalTilingFeatures&required)!=required)return false;
        }
        for(unsigned slot=0;slot<2;++slot) {
            auto& a=r.attachments[slot];
            const auto Format=attachmentFormat(plan_.formats[slot]);
            VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image.imageType=VK_IMAGE_TYPE_2D;image.format=Format;image.extent={width(),height(),1};
            image.mipLevels=1;image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
            image.tiling=VK_IMAGE_TILING_OPTIMAL;
            image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if(vkCreateImage(r.d,&image,nullptr,&a.image)!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetImageMemoryRequirements(r.d,a.image,&req);
            if(!deviceMemory(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&a.memory) ||
               vkBindImageMemory(r.d,a.image,a.memory,0)!=VK_SUCCESS)return false;
            VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image=a.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=Format;
            view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(r.d,&view,nullptr,&a.view)!=VK_SUCCESS)return false;
            VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            b.size=pixelBytes(slot)+32;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if(vkCreateBuffer(r.d,&b,nullptr,&a.output)!=VK_SUCCESS)return false;
            VkMemoryRequirements breq{};vkGetBufferMemoryRequirements(r.d,a.output,&breq);
            if(!deviceMemory(breq,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&a.outputMemory) ||
               vkBindBufferMemory(r.d,a.output,a.outputMemory,0)!=VK_SUCCESS ||
               vkMapMemory(r.d,a.outputMemory,0,VK_WHOLE_SIZE,0,&a.mapped)!=VK_SUCCESS)return false;
        }
        // Both attachments are fully replaced by the pass, so neither is loaded
        // and no initial clear is invented for either.
        std::array<VkAttachmentDescription,2> descriptions{};
        std::array<VkAttachmentReference,2> references{};
        for(unsigned i=0;i<2;++i) {
            auto& a=descriptions[i];a.format=attachmentFormat(plan_.formats[i]);a.samples=VK_SAMPLE_COUNT_1_BIT;
            a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            references[i]={i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=2;subpass.pColorAttachments=references.data();
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
        pass.attachmentCount=descriptions.size();pass.pAttachments=descriptions.data();
        pass.subpassCount=1;pass.pSubpasses=&subpass;
        pass.dependencyCount=dependencies.size();pass.pDependencies=dependencies.data();
        if(vkCreateRenderPass(r.d,&pass,nullptr,&r.renderPass)!=VK_SUCCESS)return false;
        const std::array<VkImageView,2> views{r.attachments[0].view,r.attachments[1].view};
        VkFramebufferCreateInfo framebuffer{};framebuffer.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass=r.renderPass;framebuffer.attachmentCount=views.size();
        framebuffer.pAttachments=views.data();framebuffer.width=width();framebuffer.height=height();framebuffer.layers=1;
        if(vkCreateFramebuffer(r.d,&framebuffer,nullptr,&r.framebuffer)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r.d,&poolInfo,nullptr,&r.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool=r.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r.d,&allocate,&r.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r.d,&fence,nullptr,&r.fence)==VK_SUCCESS;
    }
public:
    static std::unique_ptr<NativeRectangleMrtTarget> create(ProbeDevice& device,const NativeRectangleMrtPlan& plan) {
        std::unique_ptr<NativeRectangleMrtTarget> target(new NativeRectangleMrtTarget(device,plan));
        if(!target->setup())return nullptr;
        return target;
    }
    const NativeRectangleMrtPlan& plan()const{return plan_;}
    bool valid()const{return valid_;}
    std::optional<std::vector<std::uint8_t>> readRgba(unsigned slot)const {
        if(!valid_ || slot>=2)return std::nullopt;
        const auto* bytes=static_cast<const std::uint8_t*>(r_.attachments[slot].mapped);
        return std::vector<std::uint8_t>(bytes+16,bytes+16+pixelBytes(slot));
    }
    // `stage` names the predicate that refused, so a failure is diagnosable
    // from one log line instead of collapsing into a single boolean.
    bool draw(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
              const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
              const char** stage=nullptr) {
        const auto refuse=[&](const char* why){if(stage)*stage=why;return false;};
        if(stage)*stage="entered";
        valid_=false;
        const char* planWhy="not-evaluated";
        const auto plan=planNativeRectangleMrtDraw(input,program,bindings,&planWhy);
        if(!plan)return refuse(planWhy);
        if(*plan!=plan_)return refuse("plan-mismatch-vs-target");
        if(positions.empty())return refuse("no-positions");
        if(!nativeFullRectangleList(input,positions))return refuse("rectangle-coverage");
        if(program.vertexCode.empty() || program.pixelCode.empty())return refuse("empty-shader-code");
        const auto slots=nativeTranslatedTextureSlots(program);
        if(slots.size()!=input.textures.size() || slots.empty())return refuse("texture-count");
        // Every sampled texture is built from its OWN captured bytes; none is
        // substituted for another or reused by address. This pass samples a
        // COLOUR buffer and a DEPTH buffer together, so each is imported by its
        // own owner: D24FS8 expands 20e4 into R32_SFLOAT, colour stays UNORM8.
        // Both are supplied as owned images, because the pipeline takes either
        // all external textures or none.
        std::vector<XenosTextureDescription> descriptions;
        std::vector<std::array<std::uint32_t,6>> fetches;
        std::vector<std::unique_ptr<NativeTextureImage>> images;
        for(std::size_t i=0;i<input.textures.size();++i) {
            const auto& texture=input.textures[i];
            if(texture.literalZero)return refuse("texture-literal-zero");
            const auto desc=XenosTextureDescription::decode(texture.fetch);
            if(!desc)return refuse("texture-decode");
            if(desc->width>2048 || desc->height>2048 ||
               desc->exponentBias || desc->mipMin || desc->mipMax)return refuse("texture-profile");
            auto image=desc->format==23?NativeTextureImage::createDepth24Float(device_,*desc,texture.source)
                                       :NativeTextureImage::create(device_,*desc,texture.source);
            if(!image)return refuse(desc->format==23?"depth-texture-create":"colour-texture-create");
            descriptions.push_back(*desc);fetches.push_back(texture.fetch);images.push_back(std::move(image));
        }
        auto& r=r_;
        if(!sampler_) {
            VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            s.minFilter=s.magFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
            s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if(vkCreateSampler(r.d,&s,nullptr,&r_.sampler)!=VK_SUCCESS)return refuse("sampler-create");
            sampler_=r_.sampler;
        }
        NativeTranslatedPipelineOptions options;options.colorAttachmentCount=2;
        for(const auto& image:images)
            options.externalTextures.push_back({sampler_,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        // The images are owned per draw, so the pipeline is rebuilt with them.
        auto next=std::make_unique<NativeTranslatedResources>(r.d);
        if(!next->setup(device_,r.renderPass,program,descriptions,NativeTranslatedBlend::Replace,options))
            return refuse("pipeline-setup");
        shader_=std::move(next);fetches_=fetches;images_=std::move(images);
        auto& t=*shader_;
        auto* dst=static_cast<std::uint8_t*>(t.inputMapped);
        const auto word=[&](unsigned at,std::uint32_t value){for(unsigned b=0;b<4;++b)dst[at+b]=value>>(24-8*b);};
        word(0,input.vertexCount);word(4,*gta5DrawRegister(input,0x2302)==4);word(8,0);word(12,0);
        for(unsigned i=0;i<1024;++i){word(16+4*i,input.vertexConstants[i]);word(4112+4*i,input.pixelConstants[i]);}
        std::memcpy(dst+8208,input.vertices.data(),input.vertices.size());
        for(unsigned i=0;i<2;++i)std::memset(r.attachments[i].mapped,0x5A,pixelBytes(i)+32); // Readback guard.
        if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)
            return refuse("command-pool-reset");
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return refuse("begin-command-buffer");
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&host,0,nullptr,0,nullptr);
        // Every texture is an owned image uploaded by its own owner, so there
        // is no staging copy to barrier here.
        VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass=r.renderPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{width(),height()}};
        vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,t.pipeline);
        const VkRect2D scissor{{0,0},{width(),height()}};vkCmdSetScissor(r.command,0,1,&scissor);
        const auto viewport=nativeTranslatedViewport(input);
        if(!viewport){vkCmdEndRenderPass(r.command);vkEndCommandBuffer(r.command);return refuse("viewport");}
        vkCmdPushConstants(r.command,t.layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,viewport->data());
        vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,t.layout,0,1,&t.set,0,nullptr);
        // Six emitted vertices per three original ones: the rectangle expansion
        // selects the rectangle with gl_VertexIndex/6 and the corner with %6.
        vkCmdDraw(r.command,input.vertexCount*2,1,0,0);
        vkCmdEndRenderPass(r.command);
        for(unsigned i=0;i<2;++i) {
            VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent={width(),height(),1};
            vkCmdCopyImageToBuffer(r.command,r.attachments[i].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                r.attachments[i].output,1,&copy);
        }
        VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return refuse("end-command-buffer");
        for(unsigned i=0;i<2;++i) {
            const auto* bytes=static_cast<const std::uint8_t*>(r.attachments[i].mapped);
            if(!std::all_of(bytes,bytes+pixelBytes(i)+32,[](auto b){return b==0x5A;}))return refuse("readback-dirty");
        }
        if(vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return refuse("fence-not-reset");
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r.fence)!=VK_SUCCESS ||
           vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        // The guard bytes on both sides of every readback must survive, or the
        // copy wrote outside the region this target owns.
        valid_=true;
        for(unsigned i=0;i<2;++i) {
            const auto* bytes=static_cast<const std::uint8_t*>(r.attachments[i].mapped);
            valid_=valid_ && std::all_of(bytes,bytes+16,[](auto b){return b==0x5A;}) &&
                std::all_of(bytes+pixelBytes(i)+16,bytes+pixelBytes(i)+32,[](auto b){return b==0x5A;});
        }
        if(!valid_)return refuse("readback-guard");
        if(stage)*stage="drawn";
        return true;
    }
};
}
