#pragma once
#include "gta5_color8_clear_plan.h"
#include "device.h"
#include "native_color_sample_transfer.h"
#include "native_linear_sample_target.h"
#include "native_color8_indexed_draw.h"
#include "native_gamma_fan_state.h"
#include "native_gamma_quad_state.h"
#include "native_texture_image.h"
#include "native_color_sample_snapshot.h"
#include <cstdlib>
#include <memory>

namespace recomp::gpu {
// Real, persistent 1x/2x/4x RGBA8 sample storage. Gamma uses original encoded
// bytes, NOT sRGB. Full clears and explicit per-sample linear-result conversion
// are supported. Draw admission, blending and game resolves remain separate
// contracts. The single-sample image is a
// diagnostic resolve of a uniform clear, never evidence of general MSAA/Xenos
// resolve equivalence. Queue access must be serialized by the owner.
class NativeColor8ClearTarget {
    ProbeDevice& device_;
    Gta5Color8ClearPlan plan_;
    struct Resources {
        VkDevice d;std::array<VkImage,3> images{};std::array<VkDeviceMemory,3> memory{};
        VkBuffer output{};VkDeviceMemory outputMemory{};void* mapped=nullptr;
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        explicit Resources(VkDevice device):d(device){}
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(mapped)vkUnmapMemory(d,outputMemory);
            if(output)vkDestroyBuffer(d,output,nullptr);
            if(outputMemory)vkFreeMemory(d,outputMemory,nullptr);
            for(unsigned i=0;i<3;++i) {
                if(images[i])vkDestroyImage(d,images[i],nullptr);
                if(memory[i])vkFreeMemory(d,memory[i],nullptr);
            }
        }
    } r_;
    bool initialized_=false,valid_=false,uniformClear_=false,depthUniform_=false;
    std::uint64_t mutationSerial_=0;
    void beginPersistentMutation() {
        if(mutationSerial_!=UINT64_MAX)++mutationSerial_;
    }
    std::unique_ptr<NativeLinearSampleTarget> linear_;
    std::unique_ptr<NativeColor8IndexedDraw> indexed_;
    unsigned pixelBytes()const{return plan_.width*plan_.height*4;}
    NativeColor8ClearTarget(ProbeDevice& device,const Gta5Color8ClearPlan& p):device_(device),plan_(p),r_(device.device){}
    bool allocate(const VkMemoryRequirements& req,VkMemoryPropertyFlags flags,VkDeviceMemory& result) {
        VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&properties);
        for(unsigned i=0;i<properties.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) &&
                (properties.memoryTypes[i].propertyFlags&flags)==flags) {
            VkMemoryAllocateInfo info{};info.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            info.allocationSize=req.size;info.memoryTypeIndex=i;
            return vkAllocateMemory(r_.d,&info,nullptr,&result)==VK_SUCCESS;
        }
        return false;
    }
    bool setup() {
        auto& r=r_;
        for(unsigned i=0;i<3;++i) {
            if((i==1 && plan_.samples==1) || (i==2 && !plan_.depth))continue;
            const auto samples=static_cast<VkSampleCountFlagBits>(i==1?1:plan_.samples);
            const auto format=i==2?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R8G8B8A8_UNORM;
            const auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                (i==1?0u:VK_IMAGE_USAGE_SAMPLED_BIT|(i==2?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
            VkImageFormatProperties caps{};
            if(vkGetPhysicalDeviceImageFormatProperties(device_.physical,format,VK_IMAGE_TYPE_2D,
                    VK_IMAGE_TILING_OPTIMAL,usage,0,&caps)!=VK_SUCCESS || !(caps.sampleCounts&samples) ||
                    plan_.width>caps.maxExtent.width || plan_.height>caps.maxExtent.height)return false;
            VkImageCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;info.imageType=VK_IMAGE_TYPE_2D;
            info.format=format;info.extent={plan_.width,plan_.height,1};
            info.mipLevels=info.arrayLayers=1;info.samples=samples;info.tiling=VK_IMAGE_TILING_OPTIMAL;
            info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            if(vkCreateImage(r.d,&info,nullptr,&r.images[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetImageMemoryRequirements(r.d,r.images[i],&req);
            if(req.size>caps.maxResourceSize || !allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,r.memory[i]) ||
                vkBindImageMemory(r.d,r.images[i],r.memory[i],0)!=VK_SUCCESS)return false;
        }
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=pixelBytes()+32;
        b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(r.d,&b,nullptr,&r.output)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.output,&req);
        if(!allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,r.outputMemory) ||
            vkBindBufferMemory(r.d,r.output,r.outputMemory,0)!=VK_SUCCESS ||
            vkMapMemory(r.d,r.outputMemory,0,VK_WHOLE_SIZE,0,&r.mapped)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r.d,&pool,nullptr,&r.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo cmd{};cmd.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd.commandPool=r.pool;cmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cmd.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r.d,&cmd,&r.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r.d,&f,nullptr,&r.fence)==VK_SUCCESS;
    }
public:
    static std::unique_ptr<NativeColor8ClearTarget> create(ProbeDevice& d,const Gta5Color8ClearPlan& p) {
        if(!d.device || !d.queue || !p.object || p.object%4 || !p.width || !p.height ||
            p.width>1280 || p.height>720 || p.pitch<p.width || p.pitch>16383 || p.pitch%80 ||
            (p.samples!=1 && p.samples!=2 && p.samples!=4) || p.format>1 || p.baseTile>=2048)return nullptr;
        const auto tiles=(p.pitch*(p.samples==4?2:1)/80)*((p.height*(p.samples>=2?2:1)+15)/16);
        if(!tiles || p.tileCount!=tiles || tiles>2048-p.baseTile)return nullptr;
        if(p.depth) {
            const auto& z=*p.depth;
            const auto value=planExactXenosFloat24Clear(z.value.depth,z.value.stencil);
            if(!z.object || z.object%4 || z.object==p.object || z.width!=p.width || z.height!=p.height ||
                z.samples!=p.samples || z.format!=1 || z.pitch<z.width || z.pitch>16383 || z.pitch%80 ||
                z.baseTile>=2048 || !value || *value!=z.value)return nullptr;
            const auto zTiles=(z.pitch*(z.samples==4?2:1)/80)*((z.height*(z.samples>=2?2:1)+15)/16);
            if(!zTiles || z.tileCount!=zTiles || zTiles>2048-z.baseTile ||
                (z.baseTile<p.baseTile+p.tileCount && p.baseTile<z.baseTile+z.tileCount))return nullptr;
        }
        auto result=std::unique_ptr<NativeColor8ClearTarget>(new NativeColor8ClearTarget(d,p));
        return result->setup()?std::move(result):nullptr;
    }
    NativeColor8ClearTarget(const NativeColor8ClearTarget&)=delete;
    NativeColor8ClearTarget& operator=(const NativeColor8ClearTarget&)=delete;
    const Gta5Color8ClearPlan& plan()const{return plan_;}
    // Changes BEFORE every possible persistent color/depth write, including
    // writes that later fail. Scratch draws and read-only snapshots do not
    // change it. A matching epoch is not proof of guest ownership or validity.
    std::uint64_t diagnosticMutationSerial()const{return mutationSerial_;}
    bool clear(const std::array<std::uint8_t,4>& color,const std::optional<XenosFloat24Clear>& requestedDepth={}) {
        const auto depth=requestedDepth?requestedDepth:(plan_.depth?std::optional{plan_.depth->value}:std::nullopt);
        return clearImpl(color,depth,false);
    }
    // Resolve flag0x100 clears ONLY color after copying. In particular the
    // historical plan.depth.value must not replace nonuniform completed depth.
    // Requires a valid owner; this is not recovery from an incomplete write.
    bool clearColorPreservingDepthStencil(const std::array<std::uint8_t,4>& color) {
        if(!valid_)return false;
        return clearImpl(color,std::nullopt,true);
    }
private:
    bool clearImpl(const std::array<std::uint8_t,4>& color,const std::optional<XenosFloat24Clear>& depth,bool preserveDepth) {
        auto& r=r_;beginPersistentMutation();valid_=false;uniformClear_=false;
        if((!preserveDepth && bool(depth)!=bool(plan_.depth)) || (preserveDepth && depth) ||
            (depth && planExactXenosFloat24Clear(depth->depth,depth->stencil)!=depth))return false;
        if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
        std::memset(r.mapped,0x5A,pixelBytes()+32);
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
        const auto barrier=[&](unsigned i,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst) {
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=src;b.dstAccessMask=dst;
            b.oldLayout=before;b.newLayout=after;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.image=r.images[i];b.subresourceRange={static_cast<VkImageAspectFlags>(i==2?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            vkCmdPipelineBarrier(r.command,before==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        };
        barrier(0,initialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            initialized_?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue value{};for(unsigned i=0;i<4;++i)value.float32[i]=float(color[i])/255.f;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(r.command,r.images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
        barrier(0,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        if(depth) {
            barrier(2,initialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                initialized_?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
            // Exact20e4 clear values expanded losslessly into D32, plus the
            // requested S8. No quantization mode, general depth test or later
            // raster draw is authorized by this representation choice.
            const VkClearDepthStencilValue value{depth->depth,depth->stencil};
            vkCmdClearDepthStencilImage(r.command,r.images[2],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
            barrier(2,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        }
        const unsigned output=plan_.samples==1?0:1;
        if(output) {
            barrier(1,initialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                initialized_?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageResolve resolve{};resolve.srcSubresource=resolve.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            resolve.extent={plan_.width,plan_.height,1};
            vkCmdResolveImage(r.command,r.images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&resolve);
            barrier(1,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        }
        VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={plan_.width,plan_.height,1};
        vkCmdCopyImageToBuffer(r.command,r.images[output],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.output,1,&copy);
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
        const auto* bytes=static_cast<const std::uint8_t*>(r.mapped);
        if(!std::all_of(bytes,bytes+pixelBytes()+32,[](auto b){return b==0x5A;}) || vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r.fence)!=VK_SUCCESS ||
            vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        initialized_=true;
        if(!std::all_of(bytes,bytes+16,[](auto b){return b==0x5A;}) ||
           !std::all_of(bytes+pixelBytes()+16,bytes+pixelBytes()+32,[](auto b){return b==0x5A;}))return false;
        for(unsigned i=0;i<pixelBytes();++i)if(bytes[16+i]!=color[i%4])return false;
        plan_.color=color;if(depth)plan_.depth->value=*depth;valid_=true;uniformClear_=true;
        if(!preserveDepth)depthUniform_=bool(depth);
        return true;
    }
public:
    // Owner-controlled write, distinct from the read-only diagnostic borrowing.
    // Source is a completed RGBA32F image with matching shape/sample count,
    // SAMPLED usage and TRANSFER_SRC layout. Shader pack must be the authored
    // color_sample_transfer modules for this sample count. No averaging. Depth
    // is not attached or touched. On any submitted failure, this target remains
    // invalid; a successful clear is required before retrying.
    bool replaceSamplesFromLinear(VkImage source,unsigned width,unsigned height,unsigned samples,
            const std::vector<std::uint32_t>& vertex,const std::vector<std::uint32_t>& fragment) {
        if(!valid_ || !source || width!=plan_.width || height!=plan_.height || samples!=plan_.samples)return false;
        auto converter=NativeColorSampleTransfer::create(device_,width,height,samples,source,r_.images[0],false,vertex,fragment);
        if(!converter)return false;
        beginPersistentMutation();valid_=false;uniformClear_=false;
        valid_=converter->transfer(plan_.format,true);return valid_;
    }
    bool isUniformClear()const{return valid_ && uniformClear_;}
    bool isDepthStencilUniform()const{return valid_ && plan_.depth && depthUniform_;}
    // Owner-controlled, per-sample encoded replacement. LOADs completed depth
    // (including previous nonuniform geometry), never the historical clear.
    // Original-state/resource admission and ring retirement remain external.
    bool drawIndexedColor8(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::vector<std::uint32_t>& indices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const std::vector<XenosTextureDescription>& descriptions,const NativeTranslatedPipelineOptions& options) {
        if(!valid_ || !plan_.depth || program.abiVersion!=13 || program.geometryColor8Format!=plan_.format ||
           !program.geometryDepth20e4 || !program.geometrySampleDepth || !options.depthStencil)return false;
        if(!indexed_)indexed_=NativeColor8IndexedDraw::create(device_,r_.images[0],r_.images[2],plan_.width,plan_.height,plan_.samples);
        if(!indexed_)return false;
        beginPersistentMutation();valid_=false;uniformClear_=false;
        const auto& z=*options.depthStencil;
        if((z.depthEnable && z.depthWrite) || (z.stencilEnable && (z.front.writeMask || (z.backfaceEnable && z.back.writeMask))))depthUniform_=false;
        valid_=indexed_->draw(program,input,vertices,indices,viewport,scissor,descriptions,options);
        return valid_;
    }
    // Owner-controlled depth-only transfer, preserving every existing stencil
    // sample and all color samples. Explicit native20e4 TRUNC policy, not game
    // raster admission. Source is completed D32/S8 with matching shape/samples,
    // sampled usage and TRANSFER_SRC layout. Future uniform-depth-only paths
    // MUST reject after this operation; plan.depth.value is a historical clear.
    bool replaceDepthFromNativeD32(VkImage source,unsigned width,unsigned height,unsigned samples,
            const std::vector<std::uint32_t>& vertex,const std::vector<std::uint32_t>& fragment,bool rejectPositiveUnderflow=false) {
        if(!valid_ || !plan_.depth || !source || width!=plan_.width || height!=plan_.height || samples!=plan_.samples)return false;
        auto transfer=NativeColorSampleTransfer::createDepthPreservingStencil(device_,width,height,samples,source,r_.images[2],vertex,fragment);
        if(!transfer)return false;
        beginPersistentMutation();valid_=false;depthUniform_=false;
        valid_=transfer->transfer(rejectPositiveUnderflow?2:1,true);return valid_;
    }
    std::optional<std::vector<std::uint8_t>> snapshotEncodedSamples(const std::vector<std::uint32_t>& code)const {
        return valid_?snapshotNativeColorSamples(device_,r_.images[0],plan_.width,plan_.height,plan_.samples,code):std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> snapshotDepthSamples(const std::vector<std::uint32_t>& code)const {
        return valid_ && plan_.depth?snapshotNativeDepthSamples(device_,r_.images[2],plan_.width,plan_.height,plan_.samples,code):std::nullopt;
    }
    // Full native draw, not a clipping/clear shortcut. The original VS/PS runs
    // over owned input, preserving every previous encoded sample and quantizing
    // after this single-coverage draw. Copy actual completed depth AND stencil
    // into the scratch target, including earlier indexed geometry. This draw
    // is depth/stencil-read-only; the original depth image is never modified.
    bool drawGammaConvexFan(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
            const std::vector<std::uint32_t>& transferVertex,const std::vector<std::uint32_t>& encodePixel,
            const std::vector<std::uint32_t>& decodePixel,const NativeMipTextureCapture* mipTexture=nullptr) {
        if(!valid_ || !nativeGammaFanState(draw,program,plan_,bindings,mipTexture) || !nativeSingleCoverageConvexFan(draw,positions))return false;
        std::unique_ptr<NativeTextureImage> texture;
        struct Sampler {VkDevice d;VkSampler sampler{};~Sampler(){if(sampler)vkDestroySampler(d,sampler,nullptr);}} sampler{device_.device};
        std::vector<XenosTextureDescription> descriptions;
        if(mipTexture) {
            descriptions.push_back(*XenosTextureDescription::decode(mipTexture->fetch));
            texture=NativeTextureImage::createUnpackedMips(device_,descriptions[0],mipTexture->base,mipTexture->mips);
            if(!texture)return false;
            VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
            si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod=float(descriptions[0].mipMax);
            if(vkCreateSampler(device_.device,&si,nullptr,&sampler.sampler)!=VK_SUCCESS)return false;
        }
        if(!linear_)linear_=NativeLinearSampleTarget::create(device_,plan_.width,plan_.height,plan_.samples);
        if(!linear_ || !linear_->replaceColorFromEncoded(r_.images[0],plan_.format,transferVertex,decodePixel) ||
           !linear_->replaceDepthStencilFromNative(r_.images[2],plan_.width,plan_.height,plan_.samples))return false;
        std::vector<std::uint8_t> input(8208+draw.vertices.size());
        const auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)input[at+i]=v>>(24-8*i);};
        put(0,draw.vertexCount);put(4,static_cast<unsigned>(descriptions.size()));
        for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
        std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
        NativeTranslatedPipelineOptions options;options.samples=plan_.samples;options.minDepth=options.maxDepth=1;
        options.depthStencil=decodeXenosDepthStencilState(*gta5DrawRegister(draw,0x2200),
            *gta5DrawRegister(draw,0x210D),*gta5DrawRegister(draw,0x210C));
        if(texture)options.externalTextures={{sampler.sampler,texture->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const auto result=linear_->draw(program,input,(draw.vertexCount-2)*3,*nativeTranslatedViewport(draw),descriptions,options,
            NativeTranslatedBlend::DestinationAlphaAdd,{0,0,0,0},plan_.depth->value.depth,plan_.depth->value.stencil,true,{},true);
        if(!result)return false;
        return replaceSamplesFromLinear(linear_->borrowColorSamplesForReadOnlyDiagnostic(),plan_.width,plan_.height,plan_.samples,transferVertex,encodePixel);
    }
    // ABI13 opts into direct persistent color/depth drawing, with20e4 depth
    // conversion BEFORE comparisons against earlier geometry. The legacy
    // scratch/post-quantization path still requires its uniform zero/FF clear.
    bool drawGammaAxisQuads(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bindings,std::span<const std::array<float,4>> positions,
            const std::vector<std::uint32_t>& transferVertex,const std::vector<std::uint32_t>& encodePixel,
            const std::vector<std::uint32_t>& decodePixel,const std::vector<std::uint32_t>& depthPixel,
            const NativeTranslatedDrawProgram* persistentIndexed=nullptr) {
        if(!valid_ || (!depthUniform_ && !persistentIndexed) || !nativeGammaQuadState(draw,program,plan_,bindings) ||
           !nativeSingleCoverageAxisQuadList(draw,positions))return false;
        const auto& source=draw.textures[0];const auto description=*XenosTextureDescription::decode(source.fetch);
        auto texture=NativeTextureImage::create(device_,description,source.source);if(!texture)return false;
        struct Sampler {VkDevice d;VkSampler sampler{};~Sampler(){if(sampler)vkDestroySampler(d,sampler,nullptr);}} sampler{device_.device};
        VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if(vkCreateSampler(device_.device,&si,nullptr,&sampler.sampler)!=VK_SUCCESS)return false;
        std::vector<std::uint8_t> input(8208+draw.vertices.size());
        const auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)input[at+i]=v>>(24-8*i);};
        put(0,draw.vertexCount);
        for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
        std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
        NativeTranslatedPipelineOptions options;options.samples=plan_.samples;options.minDepth=0;options.maxDepth=1;
        options.depthStencil=decodeXenosDepthStencilState(*gta5DrawRegister(draw,0x2200),
            *gta5DrawRegister(draw,0x210D),*gta5DrawRegister(draw,0x210C));
        options.externalTextures={{sampler.sampler,texture->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        options.cullMode=VK_CULL_MODE_BACK_BIT;options.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
        if(persistentIndexed) {
            const auto& p=*persistentIndexed;
            if(p.abiVersion!=13 || !p.indexed || !p.geometryDepth20e4 || !p.geometrySampleDepth || p.geometryColor8Format!=plan_.format ||
               p.geometryCentroidMask || p.stride!=program.stride || p.vertexIdentity!=program.vertexIdentity ||
               p.pixelIdentity!=program.pixelIdentity || nativeTranslatedTextureSlots(p)!=std::vector<unsigned>{3})return false;
            std::vector<std::uint32_t> indices;indices.reserve(draw.vertexCount/4*6);
            for(unsigned i=0;i<draw.vertexCount;i+=4)for(unsigned corner:{0u,1u,2u,0u,2u,3u})indices.push_back(i+corner);
            put(4,1);options.sampleShading=true;
            return drawIndexedColor8(p,input,draw.vertexCount,indices,*nativeTranslatedViewport(draw),*nativeTranslatedScissor(draw),{description},options);
        }
        if(!linear_)linear_=NativeLinearSampleTarget::create(device_,plan_.width,plan_.height,plan_.samples);
        if(!linear_ || !linear_->replaceColorFromEncoded(r_.images[0],plan_.format,transferVertex,decodePixel))return false;
        if(!linear_->draw(program,input,draw.vertexCount/4*6,*nativeTranslatedViewport(draw),{description},options,
            NativeTranslatedBlend::Replace,{0,0,0,0},plan_.depth->value.depth,plan_.depth->value.stencil,true))return false;
        // Single coverage + GREATER against zero makes post-draw quantization
        // equivalent only when positive depths stay positive after truncation.
        const bool completed=replaceDepthFromNativeD32(linear_->borrowDepthStencilSamplesForReadOnlyDiagnostic(),
            plan_.width,plan_.height,plan_.samples,transferVertex,depthPixel,true) &&
            replaceSamplesFromLinear(linear_->borrowColorSamplesForReadOnlyDiagnostic(),
                plan_.width,plan_.height,plan_.samples,transferVertex,encodePixel);
        if(!completed)valid_=false; // Neither half of a partial commit may publish.
        return completed;
    }
    std::optional<std::vector<std::uint8_t>> readUniformResolved()const {
        if(!valid_ || !uniformClear_)return std::nullopt;
        const auto* bytes=static_cast<const std::uint8_t*>(r_.mapped);
        return std::vector<std::uint8_t>(bytes+16,bytes+16+pixelBytes());
    }
    // Diagnostic borrowing only, under the same queue lock. A probe must not
    // write pixels and must restore TRANSFER_SRC_OPTIMAL before returning.
    VkImage borrowSamplesForReadOnlyDiagnostic()const{return valid_?r_.images[0]:VK_NULL_HANDLE;}
    VkImage borrowDepthForReadOnlyDiagnostic()const{return valid_ && plan_.depth?r_.images[2]:VK_NULL_HANDLE;}
};
}
