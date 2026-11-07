#pragma once
#include "gta5_fixed16_clear_plan.h"
#include "native_fixed16_alias_plan.h"
#include "native_color8_indexed_draw.h"
#include "native_geometry_sampler.h"
#include "native_mip_texture_capture.h"
#include "device.h"
#include <cstdlib>
#include <memory>

namespace recomp::gpu {
// Persistent single-sample signed fixed16 RG storage with optional exact20e4/S8
// depth initialization. Uploads the ORIGINAL packed32-bit clear through a GPU
// fill/copy, avoiding a second floating-point clear quantization. Native sampling
// of RG16_SNORM returns fixed values /32; draw/resolve admission is separate.
// No display validity, guest memory, fake fence or ring counter is owned here.
class NativeFixed16ClearTarget {
    ProbeDevice& device_;
    Gta5Fixed16ClearPlan plan_;
    struct Resources {
        VkDevice d;
        std::array<VkImage,2> images{};
        std::array<VkDeviceMemory,2> imageMemory{};
        VkBuffer buffer{};VkDeviceMemory memory{};void* mapped=nullptr;
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        explicit Resources(VkDevice device):d(device){}
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(mapped)vkUnmapMemory(d,memory);
            if(buffer)vkDestroyBuffer(d,buffer,nullptr);
            if(memory)vkFreeMemory(d,memory,nullptr);
            for(unsigned i=0;i<2;++i) {
                if(images[i])vkDestroyImage(d,images[i],nullptr);
                if(imageMemory[i])vkFreeMemory(d,imageMemory[i],nullptr);
            }
        }
    } r_;
    std::unique_ptr<NativeFixed16IndexedDraw> indexed_;
    bool initialized_=false,valid_=false;
    std::uint64_t mutationSerial_=0;
    unsigned pixels()const{return plan_.width*plan_.height;}
    unsigned colorOffset()const{return 16;}
    unsigned depthOffset()const{return colorOffset()+pixels()*4+32;}
    unsigned stencilOffset()const{return depthOffset()+pixels()*4+32;}
    unsigned totalBytes()const{return stencilOffset()+pixels()+16;}
    NativeFixed16ClearTarget(ProbeDevice& d,const Gta5Fixed16ClearPlan& p):device_(d),plan_(p),r_(d.device){}
    bool allocate(const VkMemoryRequirements& req,VkMemoryPropertyFlags flags,VkDeviceMemory& result) {
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&props);
        for(unsigned i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) &&
            (props.memoryTypes[i].propertyFlags&flags)==flags) {
            VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            a.allocationSize=req.size;a.memoryTypeIndex=i;return vkAllocateMemory(r_.d,&a,nullptr,&result)==VK_SUCCESS;
        }
        return false;
    }
    bool setup() {
        for(unsigned i=0;i<2;++i) {
            if(i && !plan_.depth)continue;
            const auto format=i?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R16G16_SNORM;
            const auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
                (i?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            VkImageFormatProperties caps{};
            if(vkGetPhysicalDeviceImageFormatProperties(device_.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                usage,0,&caps)!=VK_SUCCESS || !(caps.sampleCounts&VK_SAMPLE_COUNT_1_BIT) ||
                plan_.width>caps.maxExtent.width || plan_.height>caps.maxExtent.height)return false;
            VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
            image.format=format;image.extent={plan_.width,plan_.height,1};image.mipLevels=image.arrayLayers=1;
            image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;
            image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            if(vkCreateImage(r_.d,&image,nullptr,&r_.images[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetImageMemoryRequirements(r_.d,r_.images[i],&req);
            if(req.size>caps.maxResourceSize || !allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,r_.imageMemory[i]) ||
                vkBindImageMemory(r_.d,r_.images[i],r_.imageMemory[i],0)!=VK_SUCCESS)return false;
        }
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;buffer.size=totalBytes();
        buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(r_.d,&buffer,nullptr,&r_.buffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r_.d,r_.buffer,&req);
        if(!allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,r_.memory) ||
            vkBindBufferMemory(r_.d,r_.buffer,r_.memory,0)!=VK_SUCCESS ||
            vkMapMemory(r_.d,r_.memory,0,VK_WHOLE_SIZE,0,&r_.mapped)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r_.d,&pool,nullptr,&r_.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo command{};command.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool=r_.pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r_.d,&command,&r_.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r_.d,&fence,nullptr,&r_.fence)==VK_SUCCESS;
    }
    bool readbackImages() {
        if(!initialized_ || !plan_.depth || vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS ||
           vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return false;
        auto* bytes=static_cast<std::uint8_t*>(r_.mapped);std::memset(bytes,0x5A,totalBytes());
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return false;
        VkMemoryBarrier memory{};memory.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memory.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        memory.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&memory,0,nullptr,0,nullptr);
        for(unsigned aspect=0;aspect<3;++aspect) {
            VkBufferImageCopy copy{};copy.bufferOffset=aspect==0?colorOffset():aspect==1?depthOffset():stencilOffset();
            copy.imageSubresource={static_cast<VkImageAspectFlags>(aspect==0?VK_IMAGE_ASPECT_COLOR_BIT:
                aspect==1?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT),0,0,1};
            copy.imageExtent={plan_.width,plan_.height,1};
            vkCmdCopyImageToBuffer(r_.command,r_.images[aspect?1:0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.buffer,1,&copy);
        }
        memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r_.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&memory,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS || vkGetFenceStatus(r_.d,r_.fence)!=VK_NOT_READY ||
           !std::all_of(bytes,bytes+totalBytes(),[](auto v){return v==0x5A;}))return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
           vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        const auto guard=[&](unsigned at,unsigned n){return std::all_of(bytes+at,bytes+at+n,[](auto v){return v==0x5A;});};
        if(!guard(0,16) || !guard(colorOffset()+pixels()*4,32) || !guard(depthOffset()+pixels()*4,32) ||
           !guard(stencilOffset()+pixels(),16))return false;
        for(unsigned i=0;i<pixels();++i) {
            float depth;std::memcpy(&depth,bytes+depthOffset()+4*i,4);
            if(!planExactXenosFloat24Clear(depth,bytes[stencilOffset()+i]))return false;
        }
        return true;
    }
public:
    static std::unique_ptr<NativeFixed16ClearTarget> create(ProbeDevice& device,const Gta5Fixed16ClearPlan& p) {
        if(!device.device || !device.queue || !p.object || p.object%4 || !p.width || !p.height || p.width>1280 || p.height>720 ||
           p.pitch<p.width || p.pitch>16383 || p.pitch%80 || p.samples!=1 || p.format!=4 || p.baseTile>=2048 ||
           p.tileCount!=(p.pitch/80)*((p.height+15)/16) || p.tileCount>2048-p.baseTile)return nullptr;
        if(p.depth) {
            const auto& z=*p.depth;
            if(!z.object || z.object%4 || z.object==p.object || z.width!=p.width || z.height!=p.height || z.samples!=1 ||
               z.format!=1 || z.pitch<p.width || z.pitch>16383 || z.pitch%80 || z.baseTile>=2048 ||
               z.tileCount!=(z.pitch/80)*((p.height+15)/16) || z.tileCount>2048-z.baseTile ||
               planExactXenosFloat24Clear(z.value.depth,z.value.stencil)!=std::optional{z.value} ||
               (z.baseTile<p.baseTile+p.tileCount && p.baseTile<z.baseTile+z.tileCount))return nullptr;
        }
        auto target=std::unique_ptr<NativeFixed16ClearTarget>(new NativeFixed16ClearTarget(device,p));
        return target->setup()?std::move(target):nullptr;
    }
    const Gta5Fixed16ClearPlan& plan()const{return plan_;}
    std::uint64_t diagnosticMutationSerial()const{return mutationSerial_;}
    std::uint32_t diagnosticFragmentCount()const{return indexed_?indexed_->diagnosticFragmentCount():0;}
    bool valid()const{return valid_;}
    // Guest ownership proof lives above this native-storage primitive. Retagging
    // performs no clear/copy/GPU work and cannot revive an invalid image.
    bool retagSameStorage(const Gta5Fixed16ClearPlan& alias) {
        if(!valid_ || !initialized_ || !nativeFixed16SameStorageRetag(plan_,alias))return false;
        plan_=alias;return true;
    }
    // Consume the SAME completed clear images. The typed draw preserves prior
    // depth/stencil via LOAD/STORE, leaves both images in transfer-source layout,
    // and has separate ABI17/18 contracts. A failure never revalidates this owner.
    // This is native storage execution, not live guest ownership/admission.
private:
    bool drawStoredFixed16(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::vector<std::uint32_t>& indices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const NativeTranslatedPipelineOptions& requestedOptions,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures={}) {
        const auto slots=nativeTranslatedTextureSlots(program);
        auto options=requestedOptions;
        if(!valid_ || !plan_.depth || !(validateNativeFixed16Program(program) || validateNativeFixed16RectangleProgram(program)) || textures.size()!=slots.size() ||
           options.samples!=1 || options.sampleShading || !options.depthStencil || !options.externalTextures.empty())return false;
        // Resource images use this target's device and outlive its fenced draw.
        // Captures must contain complete submitted mip storage; no blank or
        // constant texture stands in for missing guest bytes.
        std::vector<std::unique_ptr<NativeTextureImage>> images;
        std::vector<std::unique_ptr<NativeGeometrySampler>> samplers;
        std::vector<XenosTextureDescription> descriptions;
        for(unsigned i=0;i<slots.size();++i) {
            const auto& texture=textures[i].second;
            if(textures[i].first!=slots[i])return false;
            const auto desc=XenosTextureDescription::decode(texture.fetch);
            if(!desc || !planNativeGeometrySampler2D(texture.fetch))return false;
            descriptions.push_back(*desc);
        }
        for(unsigned i=0;i<slots.size();++i) {
            const auto& texture=textures[i].second;
            images.push_back(NativeTextureImage::createUnpackedMips(device_,descriptions[i],texture.base,texture.mips));
            if(!images.back())return false;
            samplers.push_back(NativeGeometrySampler::create(device_,*images.back()));
            if(!samplers.back())return false;
            options.externalTextures.push_back({samplers.back()->sampler(),images.back()->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        }
        if(!indexed_)indexed_=NativeFixed16IndexedDraw::create(device_,r_.images[0],r_.images[1],plan_.width,plan_.height,1);
        if(!indexed_)return false;
        valid_=false;++mutationSerial_;
        if(!indexed_->draw(program,input,vertices,indices,viewport,scissor,descriptions,options) || !readbackImages())return false;
        valid_=true;return true;
    }
public:
    bool drawIndexedFixed16(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::vector<std::uint32_t>& indices,const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const NativeTranslatedPipelineOptions& options,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures={}) {
        return validateNativeFixed16Program(program) && drawStoredFixed16(program,input,vertices,indices,viewport,scissor,options,textures);
    }
    bool drawRectangleFixed16(const NativeTranslatedDrawProgram& program,const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor,
            const NativeTranslatedPipelineOptions& options,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
        return validateNativeFixed16RectangleProgram(program) && drawStoredFixed16(program,input,vertices,{},viewport,scissor,options,textures);
    }
    bool clear(std::uint32_t packed,const std::optional<XenosFloat24Clear>& depth) {
        if(mutationSerial_!=UINT64_MAX)++mutationSerial_;
        valid_=false;
        if(bool(depth)!=bool(plan_.depth) || (depth && planExactXenosFloat24Clear(depth->depth,depth->stencil)!=depth))return false;
        if(vkResetCommandPool(r_.d,r_.pool,0)!=VK_SUCCESS || vkResetFences(r_.d,1,&r_.fence)!=VK_SUCCESS)return false;
        auto* bytes=static_cast<std::uint8_t*>(r_.mapped);std::memset(bytes,0x5A,totalBytes());
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(r_.command,&begin)!=VK_SUCCESS)return false;
        VkMemoryBarrier memory{};memory.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        const auto barrier=[&](VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to) {
            memory.srcAccessMask=src;memory.dstAccessMask=dst;
            vkCmdPipelineBarrier(r_.command,from,to,0,1,&memory,0,nullptr,0,nullptr);
        };
        barrier(VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        const auto imageBarrier=[&](unsigned i,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst) {
            VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcAccessMask=src;b.dstAccessMask=dst;
            b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=r_.images[i];
            b.subresourceRange={static_cast<VkImageAspectFlags>(i?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            vkCmdPipelineBarrier(r_.command,old==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        };
        for(unsigned i=0;i<(depth?2u:1u);++i)imageBarrier(i,initialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,initialized_?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(r_.command,r_.buffer,colorOffset(),pixels()*4,packed);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};copy.bufferOffset=colorOffset();copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.imageExtent={plan_.width,plan_.height,1};
        vkCmdCopyBufferToImage(r_.command,r_.buffer,r_.images[0],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        if(depth) {
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
            const VkClearDepthStencilValue value{depth->depth,depth->stencil};
            vkCmdClearDepthStencilImage(r_.command,r_.images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
        }
        for(unsigned i=0;i<(depth?2u:1u);++i)imageBarrier(i,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        // Erase the upload before readback; otherwise an absent GPU image copy
        // could accidentally appear to pass by leaving the source pattern.
        barrier(VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdFillBuffer(r_.command,r_.buffer,colorOffset(),pixels()*4,~packed);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(r_.command,r_.images[0],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.buffer,1,&copy);
        if(depth) {
            copy.bufferOffset=depthOffset();copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            vkCmdCopyImageToBuffer(r_.command,r_.images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.buffer,1,&copy);
            copy.bufferOffset=stencilOffset();copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
            vkCmdCopyImageToBuffer(r_.command,r_.images[1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r_.buffer,1,&copy);
        }
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        if(vkEndCommandBuffer(r_.command)!=VK_SUCCESS || vkGetFenceStatus(r_.d,r_.fence)!=VK_NOT_READY)return false;
        if(!std::all_of(bytes,bytes+totalBytes(),[](auto b){return b==0x5A;}))return false;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r_.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r_.fence)!=VK_SUCCESS ||
           vkWaitForFences(r_.d,1,&r_.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        initialized_=true;
        const auto guard=[&](unsigned at,unsigned n){return std::all_of(bytes+at,bytes+at+n,[](auto b){return b==0x5A;});};
        if(!guard(0,16) || !guard(colorOffset()+pixels()*4,32) || !guard(depthOffset()+pixels()*4,32) ||
           !guard(stencilOffset()+pixels(),16))return false;
        std::uint32_t zbits=0;if(depth)std::memcpy(&zbits,&depth->depth,4);
        for(unsigned i=0;i<pixels();++i) {
            std::uint32_t value=0;std::memcpy(&value,bytes+colorOffset()+4*i,4);if(value!=packed)return false;
            if(depth) {
                std::memcpy(&value,bytes+depthOffset()+4*i,4);
                if(value!=zbits || bytes[stencilOffset()+i]!=depth->stencil)return false;
            }
        }
        plan_.color=packed;if(depth)plan_.depth->value=*depth;valid_=true;return true;
    }
    std::optional<std::vector<std::uint8_t>> encodedSamples()const {
        if(!valid_)return std::nullopt;
        const auto* b=static_cast<const std::uint8_t*>(r_.mapped)+colorOffset();return std::vector<std::uint8_t>(b,b+pixels()*4);
    }
    std::optional<std::vector<float>> nativeDepthSamples()const {
        if(!valid_ || !plan_.depth)return std::nullopt;
        std::vector<float> values(pixels());std::memcpy(values.data(),static_cast<const std::uint8_t*>(r_.mapped)+depthOffset(),pixels()*4);
        return values;
    }
    std::optional<std::vector<std::uint8_t>> nativeStencilSamples()const {
        if(!valid_ || !plan_.depth)return std::nullopt;
        const auto* b=static_cast<const std::uint8_t*>(r_.mapped)+stencilOffset();return std::vector<std::uint8_t>(b,b+pixels());
    }
};
}
