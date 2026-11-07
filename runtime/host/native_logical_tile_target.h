#pragma once
#include "device.h"
#include "gta5_driver_tiling.h"
#include "gta5_logical_tile_clear.h"
#include "gta5_logical_tile_resolve.h"
#include "gta5_logical_tile_depth_resolve.h"
#include "gta5_tile_replay.h"
#include "native_translated_resources.h"
#include "native_depth_color_witness.h"
#include "native_scene_color_alias.h"
#include <cmath>
#include <cstring>
#include <memory>

namespace recomp::gpu {
// Native full-size storage for a logical tiled target, with up to four RGBA8
// color attachments and one D32F/S8 attachment. Single sample ONLY. Colors are
// already-encoded bytes; the caller supplies any guest gamma/float24 conversion.
//
// This is a storage/clear/draw primitive, NOT game admission: the caller must prove
// current resource headers, formats, ownership, ordering, selected-bin semantics
// and aliases before using it. It publishes no guest pixels or ring receipts.
// Device/queue access is externally serialized and the device must outlive it.
class NativeLogicalTileTarget {
public:
    struct Clear {
        unsigned tiles=0,aspects=0; // Tile bits; color0..3, depth4, stencil5.
        std::array<std::array<std::uint8_t,4>,4> colors{};
        float depth=0;
        std::uint8_t stencil=0;
        std::optional<std::array<unsigned,4>> rectangle; // Logical clip; absent means complete selected tiles.
    };
    struct Snapshot {
        std::array<std::vector<std::uint8_t>,4> colors;
        std::vector<float> depth;
        std::vector<std::uint8_t> stencil;
    };
private:
    ProbeDevice& device_;
    Gta5DriverTiling layout_;
    unsigned aspects_=0,allTiles_=0;
    std::array<std::uint32_t,6> surfaceRegisters_{};
    std::array<unsigned,6> initialized_{};
    NativeDepthColorWitness depthColorWitness_;
    std::uint64_t carrySerial_=0;
    std::uint64_t drawSerial_=0;
    // Retain a timed-out submission's inputs/pipeline until queue retirement.
    std::unique_ptr<NativeTranslatedResources> pendingDraw_;
    std::shared_ptr<void> pendingDrawResources_;
    std::array<VkImage,5> images_{};
    std::array<VkImageView,5> views_{};
    std::array<VkDeviceMemory,5> memory_{};
    VkRenderPass pass_{};VkFramebuffer framebuffer_{};
    VkCommandPool pool_{};VkCommandBuffer command_{};VkFence fence_{};
    // Optional boolean visibility measurement. Reused only after this target's
    // draw fence; retained with the command buffer if submission times out.
    VkQueryPool visibilityQuery_{};
    VkBuffer output_{};VkDeviceMemory outputMemory_{};void* mapped_=nullptr;
    std::array<std::size_t,6> offsets_{};
    std::size_t outputBytes_=0;
    bool usable_=true,pending_=false;
    std::uint64_t serial_=0;
    NativeLogicalTileTarget(ProbeDevice& d,const Gta5DriverTiling& p):device_(d),layout_(p){}
    bool sameCapturedSurfaces(const Gta5ClearCapture& capture,const Gta5DriverTiling& layout)const {
        if(layout.width!=layout_.width || layout.height!=layout_.height ||
           layout.targets!=layout_.targets || layout.tiles.size()!=layout_.tiles.size())return false;
        for(unsigned i=0;i<layout_.tiles.size();++i)if(layout.tiles[i].logical!=layout_.tiles[i].logical ||
                layout.tiles[i].origin!=layout_.tiles[i].origin)return false;
        const auto& bytes=*capture.state;
        for(unsigned i=0;i<6;++i) {
            const auto at=10368+i*4;
            const auto value=(std::uint32_t{bytes[at]}<<24)|(std::uint32_t{bytes[at+1]}<<16)|
                (std::uint32_t{bytes[at+2]}<<8)|bytes[at+3];
            if(value!=surfaceRegisters_[i])return false;
        }
        return true;
    }
    unsigned pixels()const{return layout_.width*layout_.height;}
    VkImageAspectFlags imageAspects(unsigned i)const {
        return i==4?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    }
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
        if(!usable_ || pending_)return false;
        if(vkResetCommandPool(device_.device,pool_,0)!=VK_SUCCESS ||
           vkResetFences(device_.device,1,&fence_)!=VK_SUCCESS){usable_=false;return false;}
        VkCommandBufferBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(command_,&b)!=VK_SUCCESS){usable_=false;return false;}
        return true;
    }
    bool finish() {
        if(vkEndCommandBuffer(command_)!=VK_SUCCESS){usable_=false;return false;}
        VkSubmitInfo s{};s.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;s.commandBufferCount=1;s.pCommandBuffers=&command_;
        if(vkQueueSubmit(device_.queue,1,&s,fence_)!=VK_SUCCESS){usable_=false;return false;}
        pending_=true;
        if(vkWaitForFences(device_.device,1,&fence_,VK_TRUE,5000000000ULL)!=VK_SUCCESS) {
            usable_=false;return false;
        }
        pending_=false;return true;
    }
    void barrier(VkPipelineStageFlags from,VkAccessFlags source,VkPipelineStageFlags to,VkAccessFlags destination) {
        VkMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;b.srcAccessMask=source;b.dstAccessMask=destination;
        vkCmdPipelineBarrier(command_,from,to,0,1,&b,0,nullptr,0,nullptr);
    }
    bool setup() {
        const auto d=device_.device;
        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkImageView> boundViews;
        std::array<VkAttachmentReference,4> colors{};
        for(auto& c:colors)c={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
        for(unsigned i=0;i<5;++i)if(layout_.targets[i]) {
            const auto format=i==4?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R8G8B8A8_UNORM;
            const VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
                VK_IMAGE_USAGE_SAMPLED_BIT|(i==4?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            VkImageFormatProperties caps{};
            if(vkGetPhysicalDeviceImageFormatProperties(device_.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                    usage,0,&caps)!=VK_SUCCESS || !(caps.sampleCounts&VK_SAMPLE_COUNT_1_BIT) ||
                    layout_.width>caps.maxExtent.width || layout_.height>caps.maxExtent.height)return false;
            VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
            image.format=format;image.extent={layout_.width,layout_.height,1};image.mipLevels=image.arrayLayers=1;
            image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;
            image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            if(vkCreateImage(d,&image,nullptr,&images_[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements r{};vkGetImageMemoryRequirements(d,images_[i],&r);
            if(r.size>caps.maxResourceSize || !allocate(r,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,memory_[i]) ||
               vkBindImageMemory(d,images_[i],memory_[i],0)!=VK_SUCCESS)return false;
            VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=images_[i];
            v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;v.subresourceRange={imageAspects(i),0,1,0,1};
            if(vkCreateImageView(d,&v,nullptr,&views_[i])!=VK_SUCCESS)return false;
            VkAttachmentDescription a{};a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;
            a.loadOp=a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp=a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
            a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
            const VkAttachmentReference ref{static_cast<unsigned>(attachments.size()),VK_IMAGE_LAYOUT_GENERAL};
            if(i<4)colors[i]=ref;else depth=ref;
            attachments.push_back(a);boundViews.push_back(views_[i]);
        }
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=4;sub.pColorAttachments=colors.data();sub.pDepthStencilAttachment=&depth;
        VkSubpassDependency dependency{};dependency.srcSubpass=VK_SUBPASS_EXTERNAL;dependency.dstSubpass=0;
        dependency.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependency.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        dependency.dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
        dependency.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        p.attachmentCount=attachments.size();p.pAttachments=attachments.data();p.subpassCount=1;p.pSubpasses=&sub;
        p.dependencyCount=1;p.pDependencies=&dependency;
        if(vkCreateRenderPass(d,&p,nullptr,&pass_)!=VK_SUCCESS)return false;
        VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=pass_;
        f.attachmentCount=boundViews.size();f.pAttachments=boundViews.data();f.width=layout_.width;f.height=layout_.height;f.layers=1;
        if(vkCreateFramebuffer(d,&f,nullptr,&framebuffer_)!=VK_SUCCESS)return false;
        outputBytes_=16;
        for(unsigned i=0;i<6;++i)if(aspects_&(1u<<i)) {
            offsets_[i]=outputBytes_;outputBytes_=(outputBytes_+pixels()*(i==5?1u:4u)+3u)&~std::size_t{3};
        }
        outputBytes_+=16;
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=outputBytes_;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(d,&b,nullptr,&output_)!=VK_SUCCESS)return false;
        VkMemoryRequirements r{};vkGetBufferMemoryRequirements(d,output_,&r);
        if(!allocate(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,outputMemory_) ||
           vkBindBufferMemory(d,output_,outputMemory_,0)!=VK_SUCCESS ||
           vkMapMemory(d,outputMemory_,0,VK_WHOLE_SIZE,0,&mapped_)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(d,&pool,nullptr,&pool_)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo c{};c.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;c.commandPool=pool_;
        c.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;c.commandBufferCount=1;
        if(vkAllocateCommandBuffers(d,&c,&command_)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if(vkCreateFence(d,&fence,nullptr,&fence_)!=VK_SUCCESS || !begin())return false;
        for(unsigned i=0;i<5;++i)if(images_[i]) {
            VkImageMemoryBarrier transition{};transition.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            transition.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;transition.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            transition.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            transition.srcQueueFamilyIndex=transition.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            transition.image=images_[i];transition.subresourceRange={imageAspects(i),0,1,0,1};
            vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0,0,nullptr,0,nullptr,1,&transition);
        }
        return finish(); // Layout transition only; NO initialized pixels claimed.
    }
public:
    ~NativeLogicalTileTarget() {
        const auto d=device_.device;
        // Never free resources still used by a submission that timed out.
        if(pending_)vkQueueWaitIdle(device_.queue);
        pendingDraw_.reset();
        pendingDrawResources_.reset();
        if(visibilityQuery_)vkDestroyQueryPool(d,visibilityQuery_,nullptr);
        if(fence_)vkDestroyFence(d,fence_,nullptr);
        if(pool_)vkDestroyCommandPool(d,pool_,nullptr);
        if(framebuffer_)vkDestroyFramebuffer(d,framebuffer_,nullptr);
        if(pass_)vkDestroyRenderPass(d,pass_,nullptr);
        if(mapped_)vkUnmapMemory(d,outputMemory_);
        if(output_)vkDestroyBuffer(d,output_,nullptr);
        if(outputMemory_)vkFreeMemory(d,outputMemory_,nullptr);
        for(unsigned i=0;i<5;++i) {
            if(views_[i])vkDestroyImageView(d,views_[i],nullptr);
            if(images_[i])vkDestroyImage(d,images_[i],nullptr);
            if(memory_[i])vkFreeMemory(d,memory_[i],nullptr);
        }
    }
    NativeLogicalTileTarget(const NativeLogicalTileTarget&)=delete;
    NativeLogicalTileTarget& operator=(const NativeLogicalTileTarget&)=delete;
    static std::unique_ptr<NativeLogicalTileTarget> create(ProbeDevice& device,const std::vector<std::uint8_t>& state) {
        auto p=describeGta5DriverTiling(state);
        if(!device.device || !device.queue || device.properties.limits.maxColorAttachments<4 || !p ||
           !p->boundTargetsUseTiles || !p->disjointFullCoverage || std::uint64_t{p->width}*p->height>4u*1024*1024)return nullptr;
        const auto word=[&](unsigned at){return (std::uint32_t{state[at]}<<24)|(std::uint32_t{state[at+1]}<<16)|
            (std::uint32_t{state[at+2]}<<8)|state[at+3];};
        // Single-sample encoded RGBA8 and float24 depth register formats.
        if(word(10368)&0x30000)return nullptr;
        for(unsigned i=0;i<5;++i)for(unsigned j=0;j<i;++j)
            if(p->targets[i] && p->targets[i]==p->targets[j])return nullptr;
        for(unsigned i=0;i<4;++i)if(p->targets[i] && ((word(i?10376+i*4:10372)>>16)&15)>1)return nullptr;
        if(p->targets[4] && ((word(10376)>>16)&1)!=1)return nullptr;
        auto result=std::unique_ptr<NativeLogicalTileTarget>(new NativeLogicalTileTarget(device,*p));
        for(unsigned i=0;i<6;++i)result->surfaceRegisters_[i]=word(10368+i*4);
        for(unsigned i=0;i<4;++i)if(p->targets[i])result->aspects_|=1u<<i;
        if(p->targets[4])result->aspects_|=0x30;
        if(!result->aspects_)return nullptr;
        result->allTiles_=(1u<<p->tiles.size())-1;
        return result->setup()?std::move(result):nullptr;
    }
    bool clear(const Clear& c) {
        if(!usable_ || !c.tiles || (c.tiles&~allTiles_) || !c.aspects || (c.aspects&~aspects_) ||
           ((c.aspects&0x10) && (!std::isfinite(c.depth) || c.depth<0 || c.depth>1)))return false;
        if(c.rectangle && ((*c.rectangle)[0]>=(*c.rectangle)[2] || (*c.rectangle)[1]>=(*c.rectangle)[3] ||
            (*c.rectangle)[2]>layout_.width || (*c.rectangle)[3]>layout_.height))return false;
        std::vector<VkClearAttachment> attachments;
        for(unsigned i=0;i<4;++i)if(c.aspects&(1u<<i)) {
            VkClearAttachment a{};a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;a.colorAttachment=i;
            for(unsigned j=0;j<4;++j)a.clearValue.color.float32[j]=float(c.colors[i][j])/255.f;
            attachments.push_back(a);
        }
        if(c.aspects&0x30) {
            VkClearAttachment a{};a.aspectMask=(c.aspects&0x10?VK_IMAGE_ASPECT_DEPTH_BIT:0u)|
                (c.aspects&0x20?VK_IMAGE_ASPECT_STENCIL_BIT:0u);
            a.clearValue.depthStencil={c.depth,c.stencil};attachments.push_back(a);
        }
        std::vector<VkClearRect> rectangles;
        unsigned completelyCleared=0,touched=0;
        for(unsigned i=0;i<layout_.tiles.size();++i)if(c.tiles&(1u<<i)) {
            auto r=layout_.tiles[i].logical;
            if(c.rectangle)for(unsigned j=0;j<4;++j)
                r[j]=j<2?std::max(r[j],(*c.rectangle)[j]):std::min(r[j],(*c.rectangle)[j]);
            if(r[0]>=r[2] || r[1]>=r[3])continue;
            touched|=1u<<i;
            if(r==layout_.tiles[i].logical)completelyCleared|=1u<<i;
            rectangles.push_back({{{static_cast<int>(r[0]),static_cast<int>(r[1])},{r[2]-r[0],r[3]-r[1]}},0,1});
        }
        if(rectangles.empty())return false;
        if(!begin())return false;
        VkRenderPassBeginInfo p{};p.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;p.renderPass=pass_;p.framebuffer=framebuffer_;
        p.renderArea.extent={layout_.width,layout_.height};
        vkCmdBeginRenderPass(command_,&p,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdClearAttachments(command_,attachments.size(),attachments.data(),rectangles.size(),rectangles.data());
        vkCmdEndRenderPass(command_);
        barrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
        if(!finish())return false;
        for(unsigned i=0;i<6;++i)if(c.aspects&(1u<<i))initialized_[i]|=completelyCleared;
        depthColorWitness_.cleared(touched,completelyCleared,c.aspects,c.depth);
        ++serial_;return true;
    }
    // Connected captured-API -> GPU clear. Snapshot ownership/lifetime and
    // recording completion remain the runtime caller's responsibility.
    bool clearCapturedCall(const Gta5ClearCapture& capture) {
        const auto p=planGta5LogicalTileClear(capture);
        if(!usable_ || !p || !sameCapturedSurfaces(capture,p->layout))return false;
        if(p->empty())return true; // Proven zero effects, no submission/serial fabricated.
        return clear(Clear{p->tiles,p->aspects,p->colors,p->depth,p->stencil,p->rectangle});
    }
    bool clearCapturedTile(const Gta5ClearCapture& capture,unsigned tile) {
        const auto p=planGta5LogicalTileClearForReplay(capture,tile);
        if(!usable_ || !p || !sameCapturedSurfaces(capture,p->layout))return false;
        if(p->empty())return true;
        return clear(Clear{p->tiles,p->aspects,p->colors,p->depth,p->stencil,p->rectangle});
    }
    // Storage-level carry between consecutive completed passes. Original CPU
    // interpreter 836BE870 replays descriptors in bin order, while physical
    // target register identities stay fixed. Snapshot PRE-clear resolve output
    // before calling this; source images now contain the POST-clear contents.
    // All bound aspects are carried, including undefined ones, but undefined
    // source data must never become readable proof just because it was copied.
    // External session ownership/order remains mandatory; no guest completion.
    bool carryCapturedTile(const Gta5ClearCapture& capture,unsigned from,unsigned to) {
        const auto p=planGta5LogicalTileCarry(capture,from,to);
        const auto surfaces=describeGta5LogicalTileSurfaces(capture);
        if(!usable_ || !p || !surfaces || !sameCapturedSurfaces(capture,surfaces->layout) || !begin())return false;
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        for(unsigned slot=0;slot<6;++slot)if(aspects_&(1u<<slot)) {
            VkImageCopy copy{};
            const auto aspect=slot<4?VK_IMAGE_ASPECT_COLOR_BIT:
                slot==4?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT;
            copy.srcSubresource=copy.dstSubresource={static_cast<VkImageAspectFlags>(aspect),0,0,1};
            copy.srcOffset={static_cast<int>(p->source[0]),static_cast<int>(p->source[1]),0};
            copy.dstOffset={static_cast<int>(p->destination[0]),static_cast<int>(p->destination[1]),0};
            copy.extent={p->source[2]-p->source[0],p->source[3]-p->source[1],1};
            const auto image=images_[std::min(slot,4u)];
            vkCmdCopyImage(command_,image,VK_IMAGE_LAYOUT_GENERAL,image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        }
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
        if(!finish())return false;
        for(unsigned slot=0;slot<6;++slot) {
            const bool defined=(initialized_[slot]&(1u<<from))!=0;
            initialized_[slot]&=~(1u<<to);
            if(defined)initialized_[slot]|=1u<<to;
        }
        depthColorWitness_.carried(from,to);
        ++carrySerial_;return true;
    }
    // Execute one bin's indexed draw in logical coordinates. This does not
    // interpret recording lists or select guest bins. The exclusive session
    // supplies original-order calls, owned shaders/textures and viewport state.
    // Blending requires proven initialized destination storage. A partial raster draw
    // NEVER promotes an undefined whole tile to initialized/readable status.
    // ABI25: a non-indexed, stencil-only draw into this already-completed
    // target. It owns no attachment of its own -- the colour targets are bound
    // and masked off, and the depth/stencil one is this target's own EDRAM,
    // which the draw reads and updates in place. That is why it belongs here
    // rather than in a band with its own image: the stencil it writes is the
    // state later draws test.
    bool drawCapturedStencilOnly(const Gta5ClearCapture& capture,unsigned tile,
            const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const NativeTranslatedPipelineOptions& options,const char** why=nullptr) {
        // Every refusal names itself, so a failure is diagnosable from the
        // caller's one log line rather than from a boolean.
        const auto no=[&](const char* reason){if(why)*why=reason;return false;};
        if(why)*why="entered";
        if(!usable_ || pending_)return no("target-unusable");
        if(tile>=layout_.tiles.size())return no("tile-range");
        if(program.abiVersion!=25 ||
           !validateNativeStencilOnlyProgram(program) || program.indexed ||
           !program.stride || program.stride%4 || !vertices || vertices%3 || vertices>65536 ||
           input.size()>NativeTranslatedResources::InputBytes ||
           input.size()!=8208+std::uint64_t{vertices}*program.stride)return no("program-input");
        if(options.samples!=1 || options.colorAttachmentCount!=4 || !options.depthStencil ||
           options.sampleShading || options.depthWitnessPixels ||
           !options.externalTextures.empty() ||
           options.colorWriteMasks!=std::array<unsigned,4>{0,0,0,0})return no("pipeline-options");
        if(std::any_of(viewport.begin(),viewport.end(),[](auto v){return !std::isfinite(v);}) ||
           scissor[0]>layout_.width || scissor[1]>layout_.height ||
           scissor[2]>layout_.width-scissor[0] || scissor[3]>layout_.height-scissor[1])return no("viewport-scissor");
        const auto word=[&](unsigned at){return (unsigned(input[at])<<24)|(unsigned(input[at+1])<<16)|
            (unsigned(input[at+2])<<8)|input[at+3];};
        if(word(0)!=vertices || word(4)!=0)return no("input-header");
        const auto& depth=*options.depthStencil;
        // Stencil is the output, so this draw may only run against a tile whose
        // stencil this target actually owns and has initialized. Depth is read
        // when tested, and never written by this band.
        if(!depth.stencilEnable || depth.depthWrite)return no("stencil-state");
        if(!(aspects_&0x20))return no("no-stencil-aspect");
        if(!(initialized_[5]&(1u<<tile)))return no("stencil-uninitialized");
        if(depth.depthEnable && !(aspects_&0x10))return no("no-depth-aspect");
        if(depth.depthEnable && !(initialized_[4]&(1u<<tile)))return no("depth-uninitialized");
        const auto surfaces=describeGta5LogicalTileSurfaces(capture);
        if(!surfaces)return no("surfaces-undescribed");
        if(!sameCapturedSurfaces(capture,surfaces->layout))return no("surfaces-differ");
        const auto& t=layout_.tiles[tile].logical;
        const unsigned left=std::max(t[0],scissor[0]),top=std::max(t[1],scissor[1]);
        const unsigned right=std::min(t[2],scissor[0]+scissor[2]),bottom=std::min(t[3],scissor[1]+scissor[3]);
        auto pipeline=std::make_unique<NativeTranslatedResources>(device_.device);
        if(!pipeline->setup(device_,pass_,program,{},NativeTranslatedBlend::Replace,options))return no("pipeline-setup");
        // A validated empty intersection writes no stencil. This is not a GPU
        // submission, and nothing about the target changes.
        if(left>=right || top>=bottom){if(why)*why="empty-intersection";return true;}
        std::memcpy(pipeline->inputMapped,input.data(),input.size());
        if(!begin())return no("command-begin");
        barrier(VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        VkRenderPassBeginInfo render{};render.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render.renderPass=pass_;render.framebuffer=framebuffer_;
        render.renderArea={{static_cast<int>(t[0]),static_cast<int>(t[1])},{t[2]-t[0],t[3]-t[1]}};
        vkCmdBeginRenderPass(command_,&render,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->pipeline);
        vkCmdBindDescriptorSets(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->layout,0,1,&pipeline->set,0,nullptr);
        const VkRect2D clip{{static_cast<int>(left),static_cast<int>(top)},{right-left,bottom-top}};
        vkCmdSetScissor(command_,0,1,&clip);
        // Sixteen bytes, vertex stage only: with no fragment stage the layout
        // carries no fragment range to push into.
        const std::array<float,4> constants{viewport[0],viewport[1],viewport[2],viewport[3]};
        vkCmdPushConstants(command_,pipeline->layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,constants.data());
        vkCmdDraw(command_,vertices,1,0,0);
        vkCmdEndRenderPass(command_);
        barrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT);
        if(!finish()) {
            if(pending_)pendingDraw_=std::move(pipeline);
            return no("fence");
        }
        // The input the vertex stage read must be unchanged, and every guard
        // word intact, exactly as the indexed path requires.
        const auto* completion=static_cast<const std::uint32_t*>(pipeline->completionMapped);
        bool valid=completion[4]==0 && std::memcmp(pipeline->inputMapped,input.data(),input.size())==0;
        for(unsigned i=0;i<16;++i)if(i!=4 && i!=5 && completion[i]!=0x5A5A5A5Au)valid=false;
        if(!valid){usable_=false;return no("completion-guards");}
        ++drawSerial_;
        if(why)*why="drawn";
        return true;
    }
    bool drawCapturedIndexed(const Gta5ClearCapture& capture,unsigned tile,
            const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::vector<std::uint32_t>& indices,const std::array<float,4>& viewport,
            const std::array<unsigned,4>& scissor,const std::vector<XenosTextureDescription>& descriptions,
            const NativeTranslatedPipelineOptions& options,std::shared_ptr<void> sampledResourceOwner={},
            bool* visibleSamples=nullptr,std::uint32_t* fragmentDiagnostics=nullptr) {
        if(!usable_ || pending_ || tile>=layout_.tiles.size() || (program.abiVersion<14 || program.abiVersion>16) || !program.indexed ||
           !program.stride || program.stride%4 || !vertices || vertices>65536 ||
           input.size()>NativeTranslatedResources::InputBytes || input.size()!=8208+std::uint64_t{vertices}*program.stride ||
           indices.empty() || indices.size()>65535 || indices.size()%3 || options.samples!=1 ||
           options.colorAttachmentCount!=4 || !options.depthStencil || options.sampleShading || options.depthWitnessPixels ||
           descriptions.size()!=options.externalTextures.size() ||
           std::any_of(indices.begin(),indices.end(),[&](auto i){return i>=vertices;}) ||
           std::any_of(viewport.begin(),viewport.end(),[](auto v){return !std::isfinite(v);}) ||
           scissor[0]>layout_.width || scissor[1]>layout_.height ||
           scissor[2]>layout_.width-scissor[0] || scissor[3]>layout_.height-scissor[1])return false;
        const auto word=[&](unsigned at){return (unsigned(input[at])<<24)|(unsigned(input[at+1])<<16)|
            (unsigned(input[at+2])<<8)|input[at+3];};
        if(word(0)!=vertices || word(4)!=descriptions.size())return false;
        unsigned witnessRequired=0;
        for(unsigned i=0;i<4;++i) {
            const auto format=layout_.targets[i]?((surfaceRegisters_[i?i+2:1]>>16)&15):UINT32_MAX;
            if(program.geometryMrtFormats[i]!=format)return false;
            const auto blend=decodeXenosMrtBlend(program.geometryMrtBlendControls[i]);
            const auto writes=program.geometryMrtOutputs&(1u<<i)?options.colorWriteMasks[i]:0;
            if(!blend)return false;
            if(blend->readsDestination(writes) && !(initialized_[i]&(1u<<tile)))witnessRequired|=1u<<i;
        }
        const auto& depth=*options.depthStencil;
        if(witnessRequired && (!program.geometryDepthColorWitness ||
                !depthColorWitness_.permits(tile,witnessRequired,depth)))return false;
        if((depth.depthEnable && (!(aspects_&0x10) || !(initialized_[4]&(1u<<tile)))) ||
           (depth.stencilEnable && (!(aspects_&0x20) || !(initialized_[5]&(1u<<tile)))))return false;
        // Validate physical surfaces, not an unrequested clear. This storage
        // primitive does not authorize original driver auxiliary work/replay.
        const auto surfaces=describeGta5LogicalTileSurfaces(capture);
        if(!surfaces || !sameCapturedSurfaces(capture,surfaces->layout))return false;
        const auto& t=layout_.tiles[tile].logical;
        const unsigned left=std::max(t[0],scissor[0]),top=std::max(t[1],scissor[1]);
        const unsigned right=std::min(t[2],scissor[0]+scissor[2]),bottom=std::min(t[3],scissor[1]+scissor[3]);
        std::vector<float> depthSnapshot;
        auto nativeOptions=options;
        if(witnessRequired && left<right && top<bottom) {
            // Read ONLY the initialized tile region from this exact completed
            // target/session. Shader words are a pre-draw immutable snapshot,
            // not an alias of the attachment being rendered.
            auto values=readDepthRegion(t);if(!values)return false;
            depthSnapshot=std::move(*values);nativeOptions.depthWitnessPixels=depthSnapshot.size();
        }
        auto pipeline=std::make_unique<NativeTranslatedResources>(device_.device);
        if(!pipeline->setup(device_,pass_,program,descriptions,NativeTranslatedBlend::Replace,nativeOptions))return false;
        if(left>=right || top>=bottom) {
            // A validated empty intersection has no passing samples. This is
            // not a GPU submission or a guest query-completion receipt.
            if(visibleSamples)*visibleSamples=false;
            if(fragmentDiagnostics)*fragmentDiagnostics=0; // ABI resets word5, unlike canary words.
            return true;
        }
        if(visibleSamples && !visibilityQuery_) {
            VkQueryPoolCreateInfo query{};query.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType=VK_QUERY_TYPE_OCCLUSION;query.queryCount=1;
            if(vkCreateQueryPool(device_.device,&query,nullptr,&visibilityQuery_)!=VK_SUCCESS)return false;
        }
        std::memcpy(pipeline->inputMapped,input.data(),input.size());
        std::memcpy(pipeline->indexMapped,indices.data(),indices.size()*4);
        if(!depthSnapshot.empty())std::memcpy(static_cast<std::uint32_t*>(pipeline->completionMapped)+16,
            depthSnapshot.data(),depthSnapshot.size()*4);
        if(!begin())return false;
        if(visibleSamples)vkCmdResetQueryPool(command_,visibilityQuery_,0,1);
        barrier(VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDEX_READ_BIT);
        VkRenderPassBeginInfo render{};render.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render.renderPass=pass_;render.framebuffer=framebuffer_;
        render.renderArea={{static_cast<int>(t[0]),static_cast<int>(t[1])},{t[2]-t[0],t[3]-t[1]}};
        vkCmdBeginRenderPass(command_,&render,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->pipeline);
        vkCmdBindDescriptorSets(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline->layout,0,1,&pipeline->set,0,nullptr);
        const VkRect2D clip{{static_cast<int>(left),static_cast<int>(top)},{right-left,bottom-top}};
        vkCmdSetScissor(command_,0,1,&clip);
        const std::array<float,6> constants{viewport[0],viewport[1],viewport[2],viewport[3],options.minDepth,options.maxDepth};
        vkCmdPushConstants(command_,pipeline->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,24,constants.data());
        if(program.geometryDepthColorWitness) {
            const std::array<unsigned,5> witness{t[0],t[1],t[2]-t[0],t[3]-t[1],witnessRequired};
            vkCmdPushConstants(command_,pipeline->layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,24,20,witness.data());
        }
        vkCmdBindIndexBuffer(command_,pipeline->indices,0,VK_INDEX_TYPE_UINT32);
        // Flags=0 requires no optional precise-query feature. Only zero versus
        // nonzero is meaningful; never advertise this as an exact sample count.
        if(visibleSamples)vkCmdBeginQuery(command_,visibilityQuery_,0,0);
        vkCmdDrawIndexed(command_,static_cast<unsigned>(indices.size()),1,0,0,0);
        if(visibleSamples)vkCmdEndQuery(command_,visibilityQuery_,0);
        vkCmdEndRenderPass(command_);
        barrier(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT);
        if(!finish()) {
            if(pending_) {
                pendingDraw_=std::move(pipeline);
                pendingDrawResources_=std::move(sampledResourceOwner);
            }
            return false;
        }
        const auto* completion=static_cast<const std::uint32_t*>(pipeline->completionMapped);
        bool valid=completion[4]==0 && std::memcmp(pipeline->inputMapped,input.data(),input.size())==0 &&
            std::memcmp(pipeline->indexMapped,indices.data(),indices.size()*4)==0;
        for(unsigned i=0;i<16;++i)if(i!=4 && i!=5 && completion[i]!=0x5A5A5A5Au)valid=false;
        if(!depthSnapshot.empty() && std::memcmp(completion+16,depthSnapshot.data(),depthSnapshot.size()*4))valid=false;
        if(!valid){usable_=false;return false;}
        std::uint64_t samples=0;
        if(visibleSamples && vkGetQueryPoolResults(device_.device,visibilityQuery_,0,1,sizeof(samples),
                &samples,sizeof(samples),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS) {
            usable_=false;return false;
        }
        depthColorWitness_.drew(tile,depth,program.geometryMrtOutputs,options.colorWriteMasks,program.geometryMrtBlendControls);
        ++drawSerial_;
        // Failure paths leave the caller's result untouched. The draw fence,
        // shader checks and query availability must all precede publication.
        if(visibleSamples)*visibleSamples=samples!=0;
        // Shader-owned diagnostic word, not a VIZ_QUERY status or exact
        // sample count. Its interpretation belongs to the bound shader ABI.
        if(fragmentDiagnostics)*fragmentDiagnostics=completion[5];
        return true;
    }
    // Every completed draw destroys its derived pipeline/descriptors/sampled
    // bindings before returning. Only image contents and raw surface identity
    // survive. An invalidation therefore has no resident binding to discard;
    // reject failed/in-flight state instead of freeing a timed-out submission.
    bool invalidateDerivedState(const ProbeDevice& owner,std::uint32_t mask) const noexcept {
        return &owner==&device_ && mask==0x7FFF && usable_ && !pending_ && !pendingDraw_ && !pendingDrawResources_;
    }
    // Prepare destination bytes from completed PRE-clear GPU data, then perform
    // the requested source clear. Caller must validate/publish the owned guest
    // allocation in the SAME exclusive transaction; failure must retire session
    // validity. No guest write, ring completion or tile replay is implied here.
    std::optional<std::vector<std::uint8_t>> prepareCapturedColorResolve(
            const Gta5ClearCapture& surfaces,const NativeOffscreenResolveRequest& request,
            const NativeOffscreenResolveArguments& arguments,const XenosTextureDescription& destination,
            const std::vector<std::uint8_t>& previous) {
        // finish() fences every prior native submission. readColorRegion uses
        // ALL_COMMANDS/write -> TRANSFER/read and TRANSFER/write -> HOST/read
        // barriers and a fence; there is no emulated EDRAM/texture-cache alias.
        // Do not consume this contract while pending or after a failed fence.
        if(!usable_ || pending_)return std::nullopt;
        const auto p=planGta5LogicalTileResolve(surfaces,request,arguments,destination,
            Gta5ResolveSynchronization::FencedNativeImages);
        if(!usable_ || !p || previous.size()!=p->storage.sourceBytes || !sameCapturedSurfaces(surfaces,p->layout))return std::nullopt;
        // A resolve must not inherit the unrequested clear-shader-ready guard.
        // The planner and exact target binding both validate before any copy.
        auto completed=previous;
        for(const auto& region:p->regions) {
            const auto rgba=readColorRegion(p->slot,region.source);
            const auto packed=rgba?packGta5LogicalTileResolveRegion(*p,region,*rgba,completed):std::nullopt;
            if(!packed)return std::nullopt;
            completed=*packed;
        }
        if(p->clearAfterCopy && p->tiles) {
            Clear clear;clear.tiles=p->tiles;clear.aspects=1u<<p->slot;
            clear.colors[p->slot]=*p->clearAfterCopy;clear.rectangle=p->rectangle;
            if(!this->clear(clear))return std::nullopt;
        }
        return completed;
    }
    // Owned depth/stencil -> original packed texture storage. This leaves the
    // native producer intact and grants no guest publication/completion.
    std::optional<std::vector<std::uint8_t>> prepareCapturedDepthResolve(
            const Gta5ClearCapture& surfaces,const NativeOffscreenResolveRequest& request,
            const NativeOffscreenResolveArguments& arguments,const XenosTextureDescription& destination,
            const std::vector<std::uint8_t>& previous) {
        // Same fenced, barriered native-image contract as color; no mutation
        // of captured/guest dirty flags and no guest completion granted.
        if(!usable_ || pending_)return std::nullopt;
        const auto p=planGta5LogicalTileDepthResolve(surfaces,request,arguments,destination,
            Gta5ResolveSynchronization::FencedNativeImages);
        if(!usable_ || !p || previous.size()!=p->geometry.storage.sourceBytes ||
           !sameCapturedSurfaces(surfaces,p->geometry.layout))return std::nullopt;
        auto completed=previous;
        for(const auto& region:p->geometry.regions) {
            const auto depth=readDepthRegion(region.source);
            const auto stencil=readStencilRegion(region.source);
            if(!depth || !stencil)return std::nullopt;
            auto packed=packGta5LogicalTileDepthResolveRegion(*p,region,*depth,*stencil,completed);
            if(!packed)return std::nullopt;
            completed=std::move(*packed);
        }
        return completed;
    }
    std::optional<Snapshot> readback() {
        if(!usable_)return std::nullopt;
        for(unsigned i=0;i<6;++i)if((aspects_&(1u<<i)) && initialized_[i]!=allTiles_)return std::nullopt;
        std::memset(mapped_,0x5A,outputBytes_);
        if(!begin())return std::nullopt;
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        for(unsigned i=0;i<6;++i)if(aspects_&(1u<<i)) {
            VkBufferImageCopy copy{};copy.bufferOffset=offsets_[i];
            copy.imageSubresource={static_cast<VkImageAspectFlags>(i<4?VK_IMAGE_ASPECT_COLOR_BIT:
                i==4?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT),0,0,1};
            copy.imageExtent={layout_.width,layout_.height,1};
            vkCmdCopyImageToBuffer(command_,images_[std::min(i,4u)],VK_IMAGE_LAYOUT_GENERAL,output_,1,&copy);
        }
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        if(!finish())return std::nullopt;
        const auto* bytes=static_cast<const std::uint8_t*>(mapped_);
        for(unsigned i=0;i<16;++i)if(bytes[i]!=0x5A || bytes[outputBytes_-16+i]!=0x5A){usable_=false;return std::nullopt;}
        Snapshot s;
        for(unsigned i=0;i<4;++i)if(aspects_&(1u<<i))s.colors[i].assign(bytes+offsets_[i],bytes+offsets_[i]+pixels()*4);
        if(aspects_&0x10){s.depth.resize(pixels());std::memcpy(s.depth.data(),bytes+offsets_[4],pixels()*4);}
        if(aspects_&0x20)s.stencil.assign(bytes+offsets_[5],bytes+offsets_[5]+pixels());
        return s;
    }
private:
    // Tight row-major bytes of one aspect. A region may be read independently
    // of other aspects/tiles; reading depth must not require inventing colors.
    std::optional<std::vector<std::uint8_t>> readAspectRegion(unsigned slot,const std::array<unsigned,4>& r) {
        if(!usable_ || slot>=6 || !(aspects_&(1u<<slot)) || r[0]>=r[2] || r[1]>=r[3] ||
           r[2]>layout_.width || r[3]>layout_.height)return std::nullopt;
        for(unsigned i=0;i<layout_.tiles.size();++i) {
            const auto& t=layout_.tiles[i].logical;
            const std::array<unsigned,4> overlap{std::max(t[0],r[0]),std::max(t[1],r[1]),
                std::min(t[2],r[2]),std::min(t[3],r[3])};
            if(overlap[0]>=overlap[2] || overlap[1]>=overlap[3] || (initialized_[slot]&(1u<<i)))continue;
            if(slot>=4 || !depthColorWitness_.witnesses(i,1u<<slot))return std::nullopt;
            const auto depth=readDepthRegion(overlap);
            if(!depth || std::any_of(depth->begin(),depth->end(),[](float z){return !std::isfinite(z) || z<=0 || z>1;}))
                return std::nullopt;
        }
        const auto count=std::size_t{r[2]-r[0]}*(r[3]-r[1])*(slot==5?1u:4u);
        std::memset(mapped_,0x5A,outputBytes_);
        if(!begin())return std::nullopt;
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
        const auto aspect=slot<4?VK_IMAGE_ASPECT_COLOR_BIT:slot==4?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT;
        VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={static_cast<VkImageAspectFlags>(aspect),0,0,1};
        copy.imageOffset={static_cast<int>(r[0]),static_cast<int>(r[1]),0};copy.imageExtent={r[2]-r[0],r[3]-r[1],1};
        vkCmdCopyImageToBuffer(command_,images_[std::min(slot,4u)],VK_IMAGE_LAYOUT_GENERAL,output_,1,&copy);
        barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        if(!finish())return std::nullopt;
        const auto* bytes=static_cast<const std::uint8_t*>(mapped_);
        for(unsigned i=0;i<16;++i)if(bytes[i]!=0x5A || bytes[16+count+i]!=0x5A){usable_=false;return std::nullopt;}
        return std::vector<std::uint8_t>(bytes+16,bytes+16+count);
    }
public:
    // Current native target -> ONE just-completed bin's raw colour bytes.
    // Matching surface identity, initialization and a real readback fence are
    // required. This is storage evidence only, not scene/ring completion.
    std::optional<std::vector<std::uint8_t>> readCapturedColorTile(
            const Gta5ClearCapture& capture,unsigned bin,unsigned slot) {
        if(!usable_ || pending_)return std::nullopt;
        const auto surfaces=describeGta5LogicalTileSurfaces(capture);
        const auto plan=planNativeSceneColorAlias(capture,bin,slot);
        if(!surfaces || !plan || !sameCapturedSurfaces(capture,surfaces->layout))return std::nullopt;
        return readColorRegion(slot,plan->sourceRectangle);
    }
    // Native resolve adapters still own sampling conversion, destination
    // storage, original20e4 packing and guest publication.
    std::optional<std::vector<std::uint8_t>> readColorRegion(unsigned slot,const std::array<unsigned,4>& r) {
        return slot<4?readAspectRegion(slot,r):std::nullopt;
    }
    std::optional<std::vector<float>> readDepthRegion(const std::array<unsigned,4>& r) {
        auto bytes=readAspectRegion(4,r);if(!bytes)return std::nullopt;
        std::vector<float> values(bytes->size()/4);std::memcpy(values.data(),bytes->data(),bytes->size());return values;
    }
    std::optional<std::vector<std::uint8_t>> readStencilRegion(const std::array<unsigned,4>& r) {
        return readAspectRegion(5,r);
    }
    // How many logical tiles this target covers. A consumer that must visit
    // every tile needs this; it does not imply any tile is initialized.
    unsigned tileCount()const{return static_cast<unsigned>(layout_.tiles.size());}
    std::uint64_t completedClearSerial()const{return serial_;}
    std::uint64_t completedCarrySerial()const{return carrySerial_;}
    std::uint64_t completedDrawSerial()const{return drawSerial_;}
};
}
