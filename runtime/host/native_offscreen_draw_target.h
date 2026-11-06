#pragma once
#include "native_offscreen_draw_plan.h"
#include "native_translated_resources.h"
#include "texture_fragment_target.h"
#include "native_cube_rectangle_plan.h"
#include "native_composite_rectangle_plan.h"
#include "native_offset_sample_plan.h"
#include "native_lod_rectangle_plan.h"
#include "native_texture_image.h"
#include <algorithm>
#include <cstdlib>
#include <memory>

namespace recomp::gpu {
// Fenced native UNORM8 target for proven full-replacement API draws. Explicit
// clears require the caller's validated game request. No presentation, EDRAM
// alias inference or guest publication. Queue serialization belongs to caller.
class NativeOffscreenDrawTarget {
    ProbeDevice& device_;
    NativeOffscreenDrawPlan plan_;
    struct Resources {
        VkDevice d;VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};
        VkRenderPass renderPass{},loadPass{};VkFramebuffer framebuffer{};
        VkBuffer output{};VkDeviceMemory outputMemory{};void* mapped=nullptr;
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        VkSampler cubeSampler{},lodSampler{},offsetSampler{},offsetLinearSampler{};
        explicit Resources(VkDevice device):d(device){}
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(cubeSampler)vkDestroySampler(d,cubeSampler,nullptr);
            if(lodSampler)vkDestroySampler(d,lodSampler,nullptr);
            if(offsetSampler)vkDestroySampler(d,offsetSampler,nullptr);
            if(offsetLinearSampler)vkDestroySampler(d,offsetLinearSampler,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(mapped)vkUnmapMemory(d,outputMemory);
            if(output)vkDestroyBuffer(d,output,nullptr);
            if(outputMemory)vkFreeMemory(d,outputMemory,nullptr);
            if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
            if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
            if(loadPass)vkDestroyRenderPass(d,loadPass,nullptr);
            if(colorView)vkDestroyImageView(d,colorView,nullptr);
            if(colorImage)vkDestroyImage(d,colorImage,nullptr);
            if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        }
    } r_;
    std::unique_ptr<NativeTranslatedResources> shader_;
    std::unique_ptr<NativeTextureImage> cubeImage_;
    std::array<std::uint32_t,6> fetch_{};
    bool valid_=false;
    unsigned completedRows_=0;
    unsigned texelBytes()const{return nativeOffscreenTexelBytes(plan_.format);}
    unsigned pixelBytes()const{return plan_.width*plan_.height*texelBytes();}
    explicit NativeOffscreenDrawTarget(ProbeDevice& device,NativeOffscreenDrawPlan plan):device_(device),plan_(plan),r_(device.device){}
    bool setup() {
        auto& r=r_;
        FragmentTargetConfig config{plan_.width,plan_.height,
            plan_.format==7?VK_FORMAT_R16G16B16A16_SFLOAT:
            plan_.format==14?VK_FORMAT_R32_SFLOAT:
            // k_16_16 is signed fixed point over [-32,32]; the pixel stage
            // divides by 32 so SNORM's own encode stores the guest's int16.
            plan_.format==4?VK_FORMAT_R16G16_SNORM:VK_FORMAT_R8G8B8A8_UNORM};
        config.fullOverwrite=true;config.transferDestination=true;
        if(!setupTextureFragmentTarget(device_,r,config,&r.loadPass))return false;
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        b.size=pixelBytes()+32;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(r.d,&b,nullptr,&r.output)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.output,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&props);
        unsigned type=props.memoryTypeCount;
        constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(unsigned i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) &&
            (props.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
        if(type==props.memoryTypeCount)return false;
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=req.size;a.memoryTypeIndex=type;
        if(vkAllocateMemory(r.d,&a,nullptr,&r.outputMemory)!=VK_SUCCESS ||
           vkBindBufferMemory(r.d,r.output,r.outputMemory,0)!=VK_SUCCESS ||
           vkMapMemory(r.d,r.outputMemory,0,VK_WHOLE_SIZE,0,&r.mapped)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;p.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r.d,&p,nullptr,&r.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo c{};c.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        c.commandPool=r.pool;c.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;c.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r.d,&c,&r.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r.d,&f,nullptr,&r.fence)==VK_SUCCESS;
    }
public:
    static std::unique_ptr<NativeOffscreenDrawTarget> create(ProbeDevice& device,const NativeOffscreenDrawPlan& plan) {
        const auto tiles=nativeOffscreenTileCount(plan.pitch,plan.height,plan.format);
        if(!device.device || !device.queue || !plan.object || !plan.width || !plan.height ||
           plan.width>1280 || plan.height>720 || plan.pitch<plan.width || plan.pitch>16383 || plan.pitch%80 ||
           plan.baseTile>=2048 || !tiles || plan.tileCount!=*tiles ||
           plan.tileCount>2048-plan.baseTile)return nullptr;
        auto target=std::unique_ptr<NativeOffscreenDrawTarget>(new NativeOffscreenDrawTarget(device,plan));
        if(!target->setup())return nullptr;
        return target;
    }
    NativeOffscreenDrawTarget(const NativeOffscreenDrawTarget&)=delete;
    NativeOffscreenDrawTarget& operator=(const NativeOffscreenDrawTarget&)=delete;
    const NativeOffscreenDrawPlan& plan()const{return plan_;}
    // Whether the last draw's fenced readback landed with its guards intact.
    // readRgba() already refuses when this is false; the accessor lets a
    // consumer say so explicitly, as the rectangle MRT target's does.
    bool valid()const{return valid_ && completedRows_==plan_.height;}
    unsigned completedColorRows()const{return valid_ && !plan_.format?completedRows_:0;}
    bool draw(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
              const Gta5BoundSurfaceCapture& bindings) {
        return drawInternal(program,input,bindings,{});
    }
    bool drawCubeRectangles(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
            const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions) {
        if(positions.empty())return false;
        return drawInternal(program,input,bindings,positions);
    }
    // ABI20 composite band: same rectangle expansion and coverage proof as the
    // cube path, with its actual current owned target (which may be on-stack).
    // `stage` reports which predicate refused, so a failure names itself
    // instead of collapsing every cause into one boolean.
    bool drawCompositeRectangles(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
            const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
            const char** stage=nullptr) {
        if(stage)*stage="entered";
        if(positions.empty()){if(stage)*stage="no-positions";return false;}
        return drawInternal(program,input,bindings,positions,nullptr,true,stage);
    }
    // ABI23 offset sample band: same rectangle expansion and coverage proof as
    // the composite path, but its plan takes a single LIVE register target and
    // its source texture is SELECTED by the program's declared slot. The game
    // leaves nine textures bound here while the pair samples exactly one, so
    // textures[0] is the wrong texture for this band.
    bool drawOffsetSample(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
            const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
            const char** stage=nullptr) {
        if(stage)*stage="entered";
        if(positions.empty()){if(stage)*stage="no-positions";return false;}
        if(!drawInternal(program,input,bindings,positions,nullptr,false,stage,true))return false;
        if(stage)*stage="drawn"; // Say so on success, not just on refusal.
        return true;
    }
    bool drawLodRectangle(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
            const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
            const NativeMipTextureCapture& texture) {
        if(positions.empty())return false;
        return drawInternal(program,input,bindings,positions,&texture);
    }
private:
    bool drawInternal(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& input,
              const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
              const NativeMipTextureCapture* mipTexture=nullptr,bool composite=false,
              const char** stage=nullptr,bool offsetSample=false) {
        const auto refuse=[&](const char* why){if(stage)*stage=why;return false;};
        // The composite samples an ordinary 2D texture, so it takes the normal
        // decode path; only cube and LOD supply their images externally.
        const bool lod=mipTexture!=nullptr,cube=!lod && !composite && !offsetSample && !positions.empty(),
            external=cube || lod;
        // Whether the sampled image is owned elsewhere. The offset sample band
        // supplies its own NativeTextureImage, so this target must not allocate,
        // upload or transition internal texture resources for it -- setup() skips
        // creating them, and touching them would dereference null handles. It is
        // deliberately NOT folded into `external`, because the format, filter and
        // storage validation below must still run for this band.
        const bool externalImage=external || offsetSample;
        const auto previous=lod?readRgba():std::optional<std::vector<std::uint8_t>>{};
        valid_=false;completedRows_=0;
        const auto lodPlan=lod?planNativeLodRectangleDraw(input,program,bindings,*mipTexture):std::nullopt;
        const auto plan=lod?(lodPlan?std::optional{lodPlan->target}:std::nullopt):
            offsetSample?planNativeOffsetSampleDraw(input,program,bindings):
            composite?planNativeCompositeRectangleDraw(input,program,bindings):
            cube?planNativeCubeRectangleDraw(input,program,bindings):planNativeOffscreenDraw(input,program,bindings);
        if(!plan)return refuse("plan-rejected");
        if(plan!=std::optional{plan_})return refuse("plan-mismatch-vs-target");
        unsigned writtenRows=plan_.height;
        if(composite) {
            const auto rows=nativeCompositeCompletedRows(input,*plan,positions);
            if(!rows)return refuse("composite-tile-row-coverage");
            writtenRows=*rows;
        } else if((cube||offsetSample) && !nativeFullRectangleList(input,positions))return refuse("rectangle-coverage");
        if(lod && (!previous || !nativeLodRectanglePositions(input,*lodPlan,positions)))return refuse("lod-positions");
        if(program.vertexCode.empty() || program.pixelCode.empty())return refuse("empty-shader-code");
        // This band's inputs are not "the first bound texture": the declared
        // slot is selected out of the bound set. Every other path keeps its
        // existing single-texture contract untouched.
        const Gta5InlineDraw::Texture* selected=lod?nullptr:&input.textures[0];
        if(offsetSample) {
            const auto slots=nativeTranslatedTextureSlots(program);
            if(slots.size()!=1)return refuse("texture-slots");
            selected=nullptr;
            for(const auto& texture:input.textures)
                if(texture.slot==slots[0]) {
                    if(selected)return refuse("texture-slots");
                    selected=&texture;
                }
            if(!selected)return refuse("texture-slots");
        }
        if(!lod && !selected)return refuse("texture-slots");
        const auto& sourceFetch=lod?mipTexture->fetch:selected->fetch;
        const auto& sourceBytes=lod?mipTexture->base:selected->source;
        const auto desc=XenosTextureDescription::decode(sourceFetch);
        if(!desc)return refuse("texture-decode");
        if(!lod && selected->literalZero)return refuse("texture-literal-zero");
        std::optional<std::vector<std::uint8_t>> pixels;
        // Which filter encoding the sampled constant names; decided with the
        // other texture predicates below and consumed when the sampler is built.
        bool linearSource=false;
        if(!external) {
        // k_16_16_16_16_FLOAT carries sign and integer-component selects that
        // pick a numeric interpretation; they cannot turn a binary16 into a
        // normalised integer and they do not change the stored bytes, which is
        // the same reasoning planXenosFloatStorage records. Every other path
        // keeps the strict all-zero requirement.
        const bool signPermitted=offsetSample && desc->format!=25u?
            std::all_of(desc->sign.begin(),desc->sign.end(),[](unsigned v){return v<=1;}):
            desc->sign==std::array<unsigned,4>{0,0,0,0};
        // The band samples the pyramid's own levels: k_16_16_16_16_FLOAT for
        // every level it writes, and k_32_FLOAT for the full-resolution
        // luminance buffer the ABI22 resolve produces. Both are raw float
        // storage; nothing else is admitted here.
        // k_16_16 joins them: the tonemap pass reads back the very texture its
        // own earlier pair resolved into, so the band both writes and samples
        // this format.
        const bool sampledFormat=offsetSample?
            (desc->format==32u || desc->format==36u || desc->format==25u):desc->format==6u;
        if(!sampledFormat || desc->width>2048 || desc->height>2048 || !signPermitted ||
           (desc->clamp[0]!=0 && desc->clamp[0]!=2) || (desc->clamp[1]!=0 && desc->clamp[1]!=2) || desc->clamp[2]!=0 ||
           desc->exponentBias || ((!offsetSample || desc->format==25u) && desc->integerComponents) || desc->mipMin || desc->mipMax)return refuse("texture-profile");
        // fetch[3]: mag at 19, min at 21, mip at 23, two bits each. The
        // offscreen profile pins all-LINEAR (21). The composite band is an
        // explicit texel fetch with no mip chain, so it pins mip POINT (5) --
        // the encoding the composite translator profile exists for. Each path
        // requires one exact value and neither accepts the other's.
        // fetch[3]: mag at 19, min at 21, mip at 23, two bits each. The offscreen
        // profile pins all-LINEAR (21) and the composite mag/min LINEAR with mip
        // POINT (5). The offset sample band reads one texel per output pixel from
        // a resolve destination, and the constant the game writes is all-POINT
        // (0). Each path requires one exact value and none accepts another's.
        // The pyramid's levels are read one texel per output pixel, all-POINT
        // (0). The tonemap pass's k_16_16 source is read mag/min LINEAR with
        // mip POINT (5), which is the encoding the composite band pins too. The
        // sampler below is built to match whichever the constant names, so the
        // two cannot be silently swapped for one another.
        const auto filters=(desc->fetch[3]>>19)&63;
        linearSource=offsetSample && filters==5u && desc->format==25u;
        if(filters!=(offsetSample?(linearSource?5u:0u):composite?5u:21u))return refuse("texture-filters");
        for(const auto component:desc->swizzle)if(component>5)return refuse("texture-swizzle");
        // k_16_16_16_16_FLOAT must be linearized through its raw storage view.
        // linearizeXenosTexture2D keys off planXenosTexture2D, which refuses the
        // numeric selects this constant carries, so handing it the original
        // descriptor would fail on a texture whose bytes are perfectly readable.
        const auto storage=offsetSample?planXenosFloatStorage(*desc):std::nullopt;
        if(offsetSample && !storage)return refuse("texture-storage-view");
        pixels=linearizeXenosTexture2D(offsetSample?storage->description:*desc,sourceBytes);
        // One binary32 for k_32_FLOAT, four binary16 for k_16_16_16_16_FLOAT,
        // two 16-bit channels for k_16_16.
        const auto texel=offsetSample?xenosRawFloatStorageTexelBytes(desc->format):4u;
        if(!pixels || pixels->size()!=std::uint64_t{desc->width}*desc->height*texel)return refuse("texture-linearize");
        }
        NativeTranslatedPipelineOptions options;
        if(cube) {
            // Fresh owned bytes, all six faces. No address-only GPU image reuse.
            cubeImage_=NativeTextureImage::createCubeBase(device_,*desc,sourceBytes);
            if(!cubeImage_)return false;
            if(!r_.cubeSampler) {
                VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                s.minFilter=s.magFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
                s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                if(vkCreateSampler(r_.d,&s,nullptr,&r_.cubeSampler)!=VK_SUCCESS)return false;
            }
            options.externalTextures={{r_.cubeSampler,cubeImage_->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        }
        if(lod) {
            // Full independently owned storage snapshots, refreshed each draw.
            // Validated explicit integer LOD selects the preceding mip only;
            // uploading other resident levels does not mark them initialized.
            cubeImage_=NativeTextureImage::createUnpackedMips(device_,*desc,mipTexture->base,mipTexture->mips);
            if(!cubeImage_)return false;
            if(!r_.lodSampler) {
                VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                s.minFilter=s.magFilter=VK_FILTER_LINEAR;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
                s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;s.maxLod=3;
                if(vkCreateSampler(r_.d,&s,nullptr,&r_.lodSampler)!=VK_SUCCESS)return false;
            }
            options.externalTextures={{r_.lodSampler,cubeImage_->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        }
        if(offsetSample) {
            // The internal upload storage is R8/RGBA8 only; a float source must
            // come through the format-aware owner. The validation above already
            // proved the format, filters, swizzle and readable storage.
            cubeImage_=NativeTextureImage::create(device_,*desc,sourceBytes);
            if(!cubeImage_)return refuse("texture-image-create");
            // One sampler per filter encoding the constant may name, built to
            // match it. A LINEAR source also has to be one the device can
            // actually filter, which the image reports for its own format.
            auto& sampler=linearSource?r_.offsetLinearSampler:r_.offsetSampler;
            if(linearSource && !cubeImage_->supportsLinearFiltering())
                return refuse("texture-linear-unsupported");
            if(!sampler) {
                VkSamplerCreateInfo sam{};sam.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                sam.minFilter=sam.magFilter=linearSource?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
                sam.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
                sam.addressModeU=sam.addressModeV=sam.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                if(vkCreateSampler(r_.d,&sam,nullptr,&sampler)!=VK_SUCCESS)return refuse("sampler-create");
            }
            options.externalTextures={{sampler,cubeImage_->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        }
        if(externalImage || !shader_ || fetch_!=sourceFetch || shader_->vertexCode!=program.vertexCode || shader_->pixelCode!=program.pixelCode) {
            auto next=std::make_unique<NativeTranslatedResources>(r_.d);
            if(!next->setup(device_,r_.renderPass,program,{*desc},NativeTranslatedBlend::Replace,options))return refuse("pipeline-setup");
            shader_=std::move(next);fetch_=sourceFetch;
        }
        auto& t=*shader_;auto& r=r_;auto* dst=static_cast<std::uint8_t*>(t.inputMapped);
        const auto word=[&](unsigned at,std::uint32_t value){for(unsigned b=0;b<4;++b)dst[at+b]=value>>(24-8*b);};
        word(0,input.vertexCount);word(4,*gta5DrawRegister(input,0x2302)==4);word(8,0);word(12,0);
        for(unsigned i=0;i<1024;++i){word(16+4*i,input.vertexConstants[i]);word(4112+4*i,input.pixelConstants[i]);}
        std::memcpy(dst+8208,input.vertices.data(),input.vertices.size());
        auto& texture=t.textures[0];if(!externalImage)std::memcpy(texture.uploadMapped,pixels->data(),pixels->size());
        std::memset(r.mapped,0x5A,pixelBytes()+32); // Readback only, not target contents.
        if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return refuse("command-pool-reset");
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return refuse("begin-command-buffer");
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&host,0,nullptr,0,nullptr);
        if(!externalImage) {
        VkImageMemoryBarrier image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image.srcAccessMask=texture.ready?VK_ACCESS_SHADER_READ_BIT:0;image.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        image.oldLayout=texture.ready?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        image.image=texture.image;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(r.command,texture.ready?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
        VkBufferImageCopy upload{};upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};upload.imageExtent={desc->width,desc->height,1};
        vkCmdCopyBufferToImage(r.command,texture.upload,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload);
        image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
        }
        VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass=lod?r.loadPass:r.renderPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{plan_.width,plan_.height}};
        // Composite may replace only a proved tile-row prefix. DONT_CARE makes
        // the remainder UNKNOWN; it is never exposed as a valid full image.
        // LOD profiles require prior valid storage and LOAD. No invented clear.
        vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,t.pipeline);
        const VkRect2D scissor{{0,0},{plan_.width,plan_.height}};vkCmdSetScissor(r.command,0,1,&scissor);
        const auto viewport=nativeTranslatedViewport(input);
        vkCmdPushConstants(r.command,t.layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,viewport->data());
        vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,t.layout,0,1,&t.set,0,nullptr);
        // Every rectangle-expanding profile emits SIX vertices per three
        // original ones: the shader indexes corners with gl_VertexIndex%6 and
        // selects the rectangle with gl_VertexIndex/6. The cube and LOD bands
        // already doubled; the composite band expands identically and must too,
        // or only its first triangle is rasterised and half the band is never
        // written. The direct-triangle offscreen profile does not expand.
        const bool expands=external||composite||offsetSample;
        vkCmdDraw(r.command,expands?input.vertexCount*2:input.vertexCount,1,0,0);vkCmdEndRenderPass(r.command);
        VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={plan_.width,plan_.height,1};
        vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.output,1,&copy);
        VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
        const auto* bytes=static_cast<const std::uint8_t*>(r.mapped);
        if(!std::all_of(bytes,bytes+pixelBytes()+32,[](auto b){return b==0x5A;}) || vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r.fence)!=VK_SUCCESS ||
           vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        texture.ready=true;
        valid_=std::all_of(bytes,bytes+16,[](auto b){return b==0x5A;}) &&
            std::all_of(bytes+pixelBytes()+16,bytes+pixelBytes()+32,[](auto b){return b==0x5A;});
        if(valid_ && lod)for(unsigned y=0;y<plan_.height;++y)for(unsigned x=0;x<plan_.width;++x)
            if(x>=lodPlan->width || y>=lodPlan->height)for(unsigned c=0;c<4;++c) {
                const unsigned at=(y*plan_.width+x)*texelBytes()+c;
                if(bytes[16+at]!=(*previous)[at]){valid_=false;return false;}
            }
        if(valid_)completedRows_=writtenRows;
        return valid_;
    }
public:
    // Caller must retain the pre-clear readback for the resolve destination.
    // Bytes are the original format0 clear conversion, not a guessed color.
    bool clearColor(const std::array<std::uint8_t,4>& color) {
        return clearColorInternal(color,plan_.width,plan_.height);
    }
    // Separate partial-source clear, NEVER an initialization of a new target.
    // Original823C2C68 rounds resolve extents to eight; this narrow contract
    // admits already-aligned regions only, without guessing rounded neighbors.
    bool clearColorRegion(const std::array<std::uint8_t,4>& color,unsigned width,unsigned height) {
        if(!width || !height || width>plan_.width || height>plan_.height || width%8 || height%8)return false;
        return clearColorInternal(color,width,height);
    }
private:
    bool clearColorInternal(const std::array<std::uint8_t,4>& color,unsigned width,unsigned height) {
        if(!valid())return false;
        const bool full=width==plan_.width && height==plan_.height;
        const auto previous=full?std::optional<std::vector<std::uint8_t>>{}:readRgba();
        valid_=false;completedRows_=0;auto& r=r_;
        std::memset(r.mapped,0x5A,pixelBytes()+32);
        if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
        VkClearColorValue value{};
        for(unsigned c=0;c<4;++c)value.float32[c]=float(color[c])/255.f;
        if(full) {
        VkImageMemoryBarrier image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        image.image=r.colorImage;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&image);
        vkCmdClearColorImage(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&image.subresourceRange);
        image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,0,nullptr,1,&image);
        } else {
            VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            pass.renderPass=r.loadPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{plan_.width,plan_.height}};
            vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
            VkClearAttachment attachment{};attachment.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment=0;attachment.clearValue.color=value;
            const VkClearRect rectangle{{{0,0},{width,height}},0,1};
            vkCmdClearAttachments(r.command,1,&attachment,1,&rectangle);
            vkCmdEndRenderPass(r.command);
        }
        VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={plan_.width,plan_.height,1};
        vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.output,1,&copy);
        VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&after,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
        const auto* bytes=static_cast<const std::uint8_t*>(r.mapped);
        if(!std::all_of(bytes,bytes+pixelBytes()+32,[](auto b){return b==0x5A;}) || vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r.fence)!=VK_SUCCESS ||
           vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        if(!std::all_of(bytes,bytes+16,[](auto b){return b==0x5A;}) ||
           !std::all_of(bytes+pixelBytes()+16,bytes+pixelBytes()+32,[](auto b){return b==0x5A;}))return false;
        for(unsigned y=0;y<plan_.height;++y)for(unsigned x=0;x<plan_.width;++x)for(unsigned c=0;c<4;++c) {
            const unsigned at=(y*plan_.width+x)*4+c;
            const auto expected=x<width && y<height?color[c]:(*previous)[at];
            if(bytes[16+at]!=expected)return false;
        }
        valid_=true;completedRows_=plan_.height;return true;
    }
public:
    std::optional<std::vector<std::uint8_t>> readRgba()const {
        if(!valid())return std::nullopt;
        const auto* bytes=static_cast<const std::uint8_t*>(r_.mapped);
        return std::vector<std::uint8_t>(bytes+16,bytes+16+pixelBytes());
    }
    std::optional<std::vector<std::uint8_t>> readCompletedColorRows()const {
        const auto rows=completedColorRows();if(!rows)return std::nullopt;
        const auto* bytes=static_cast<const std::uint8_t*>(r_.mapped);
        return std::vector<std::uint8_t>(bytes+16,bytes+16+std::size_t{plan_.width}*rows*4);
    }
};
}
