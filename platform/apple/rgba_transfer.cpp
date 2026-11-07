// SPDX-License-Identifier: GPL-3.0-or-later
#include "rgba_transfer.h"
#include "apple/ios_gpu_lifecycle.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace recomp::apple {
bool RgbaTransfer::memory(std::uint32_t bits, VkMemoryPropertyFlags flags, std::uint32_t& type) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
    for (std::uint32_t i=0;i<properties.memoryTypeCount;++i)
        if ((bits & (1u<<i)) && (properties.memoryTypes[i].propertyFlags & flags)==flags) {
            type=i; return true;
        }
    return false;
}

RgbaTransfer::~RgbaTransfer() {
    if (!device_) return;
    // A failed fence wait poisons the transfer object. Do not free resources
    // while the device may still own them. Process-level test supervision is
    // the bound if the driver itself cannot become idle.
    if (pending_) vkDeviceWaitIdle(device_);
    if (mapped_) vkUnmapMemory(device_, bufferMemory_);
    if (imageMapped_) vkUnmapMemory(device_, imageMemory_);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (image_) vkDestroyImage(device_, image_, nullptr);
    if (buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
    if (imageMemory_) vkFreeMemory(device_, imageMemory_, nullptr);
    if (bufferMemory_) vkFreeMemory(device_, bufferMemory_, nullptr);
}

RgbaTransfer::LinearChoice RgbaTransfer::createLinear() {
    VkFormatProperties f{};vkGetPhysicalDeviceFormatProperties(physical_,VK_FORMAT_R8G8B8A8_UNORM,&f);
    const auto needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if((f.linearTilingFeatures&needed)!=needed)return LinearChoice::Unsupported;
    VkImageFormatProperties cap{};const auto result=vkGetPhysicalDeviceImageFormatProperties(physical_,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_TRANSFER_SRC_BIT,0,&cap);
    if(result==VK_ERROR_FORMAT_NOT_SUPPORTED)return LinearChoice::Unsupported;
    if(!good(result))return LinearChoice::Failed;
    if(width_>cap.maxExtent.width||height_>cap.maxExtent.height||cap.maxExtent.depth<1||!cap.maxMipLevels||!cap.maxArrayLayers||!(cap.sampleCounts&VK_SAMPLE_COUNT_1_BIT))return LinearChoice::Unsupported;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R8G8B8A8_UNORM;info.extent={width_,height_,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_LINEAR;info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;info.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
    if(!good(vkCreateImage(device_,&info,nullptr,&image_)))return LinearChoice::Failed;
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device_,image_,&req);std::uint32_t type=0;
    if(!memory(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,type)){vkDestroyImage(device_,image_,nullptr);image_=VK_NULL_HANDLE;return LinearChoice::Unsupported;}
    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,0};vkGetImageSubresourceLayout(device_,image_,&sub,&imageLayout_);
    if(!linear_upload::fits(width_,height_,imageLayout_.offset,imageLayout_.rowPitch,imageLayout_.size,req.size)||req.size>cap.maxResourceSize){error_=VK_ERROR_INITIALIZATION_FAILED;return LinearChoice::Failed;}
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
    if(!good(vkAllocateMemory(device_,&alloc,nullptr,&imageMemory_))||!good(vkBindImageMemory(device_,image_,imageMemory_,0))||!good(vkMapMemory(device_,imageMemory_,0,VK_WHOLE_SIZE,0,&imageMapped_)))return LinearChoice::Failed;
    imageAllocation_=req.size;linear_=true;return LinearChoice::Ready;
}

bool RgbaTransfer::create(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                          std::uint32_t family, std::uint32_t width, std::uint32_t height) {
    // Two uint32 dimensions fit in uint64; multiplying that product by four
    // before applying the limit could wrap an oversized image to a small size.
    const auto pixels=std::uint64_t(width)*height;
    if (device_ || !physical || !device || !queue || !width || !height || pixels>(64*1024*1024)/4) return false;
    const auto size=pixels*4;
    const int requested=linear_upload::policy(std::getenv("RECOMP_IOS_LINEAR_UPLOAD"));
    if(requested<0){error_=VK_ERROR_INITIALIZATION_FAILED;std::fprintf(stderr,"APPLE_IOS_LINEAR_UPLOAD requested=invalid active=0 reason=invalid-policy\n");return false;}
    physical_=physical; device_=device; queue_=queue; width_=width; height_=height; bytes_=size;
    if(requested) {
        const auto choice=createLinear();
        if(choice==LinearChoice::Failed){std::fprintf(stderr,"APPLE_IOS_LINEAR_UPLOAD requested=1 active=0 reason=driver-or-layout-failure error=%d\n",int(error_));return false;}
    }
    std::fprintf(stderr,"APPLE_IOS_LINEAR_UPLOAD requested=%d active=%d reason=%s offset=%llu row_pitch=%llu allocation=%llu\n",requested,int(linear_),linear_?"linear-ready":requested?"unsupported":"disabled",(unsigned long long)imageLayout_.offset,(unsigned long long)imageLayout_.rowPitch,(unsigned long long)imageAllocation_);
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R8G8B8A8_UNORM, &format);
    constexpr auto flags=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & flags)!=flags) return false;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=bytes_*2; buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if (!good(vkCreateBuffer(device_, &buffer, nullptr, &buffer_))) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    std::uint32_t type=0;
    if (!memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type)) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size; allocation.memoryTypeIndex=type;
    if (!good(vkAllocateMemory(device_, &allocation, nullptr, &bufferMemory_)) ||
        !good(vkBindBufferMemory(device_, buffer_, bufferMemory_, 0)) ||
        !good(vkMapMemory(device_, bufferMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_))) return false;
    if(!linear_) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R8G8B8A8_UNORM;
    image.extent={width_,height_,1}; image.mipLevels=1; image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT; image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode=VK_SHARING_MODE_EXCLUSIVE; image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if (!good(vkCreateImage(device_, &image, nullptr, &image_))) return false;
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    if (!memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) return false;
    allocation.allocationSize=requirements.size; allocation.memoryTypeIndex=type;
    if (!good(vkAllocateMemory(device_, &allocation, nullptr, &imageMemory_)) ||
        !good(vkBindImageMemory(device_, image_, imageMemory_, 0))) return false;
    }
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex=family; pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!good(vkCreateCommandPool(device_, &pool, nullptr, &pool_))) return false;
    VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commands.commandPool=pool_; commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; commands.commandBufferCount=1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ready_=good(vkAllocateCommandBuffers(device_, &commands, &command_)) && good(vkCreateFence(device_, &fence, nullptr, &fence_));
    return ready_;
}

RgbaTransfer::Result RgbaTransfer::uploadAndVerify(const std::uint8_t* rgba, std::size_t bytes, presenter_timing::Snapshot* timing) {
    if (!ready_ || !rgba || bytes!=bytes_) return Result::Rejected;
    ready_=false;
    if (!good(presenter_timing::measure(timing,presenter_timing::Phase::SourcePrepare,[&]{return vkResetCommandBuffer(command_,0);}))) return Result::DriverFailure;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!good(presenter_timing::measure(timing,presenter_timing::Phase::SourcePrepare,[&]{return vkBeginCommandBuffer(command_,&begin);}))) return Result::DriverFailure;
    ready_=true;
    if (presenter_timing::measure(timing,presenter_timing::Phase::SourcePrepare,[&]{return recordUpload(command_,rgba,bytes);})!=Result::Recorded) return Result::Rejected;
    if (!good(presenter_timing::measure(timing,presenter_timing::Phase::SourcePrepare,[&]{return vkEndCommandBuffer(command_);}))) return Result::DriverFailure;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount=1; submit.pCommandBuffers=&command_;
    return submitAndVerify(submit,fence_,timing,true);
}

RgbaTransfer::Result RgbaTransfer::recordUpload(VkCommandBuffer command, const std::uint8_t* rgba, std::size_t bytes) {
    if (!ready_ || recorded_ || !command || !rgba || bytes!=bytes_) return Result::Rejected;
    ready_=false; // Recorded is not verified; failures never silently retry.
    recorded_=command; source_=rgba;
    if(linear_) {
        if(!linear_upload::copyRows(imageMapped_,imageAllocation_,imageLayout_.offset,imageLayout_.rowPitch,imageLayout_.size,width_,height_,rgba,bytes))return Result::Rejected;
        auto* dst=static_cast<std::uint8_t*>(imageMapped_)+imageLayout_.offset;
#if defined(APPLE_RGBA_MUTANT_LINEAR_PIXEL)
        dst[0]^=1;
#endif
    } else std::memcpy(mapped_, rgba, bytes);
#if defined(APPLE_RGBA_MUTANT_SWAP_CHANNELS)
    auto* data=static_cast<std::uint8_t*>(mapped_);
    for (std::size_t i=0;i<bytes;i+=4) { const auto red=data[i]; data[i]=data[i+2]; data[i+2]=red; }
#endif
    std::memset(static_cast<std::uint8_t*>(mapped_)+bytes_, 0xA5, bytes);
    if(linear_) {
        VkImageMemoryBarrier linear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};linear.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;linear.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;linear.oldLayout=initialized_?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_PREINITIALIZED;linear.newLayout=VK_IMAGE_LAYOUT_GENERAL;linear.srcQueueFamilyIndex=linear.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;linear.image=image_;linear.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&linear);
    } else {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask=initialized_?VK_ACCESS_TRANSFER_READ_BIT:0; barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout=initialized_?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image_; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(command, initialized_?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width_,height_,1};
    vkCmdCopyBufferToImage(command, buffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,0,nullptr,0,nullptr,1,&barrier);
    }
    VkBufferImageCopy readback{};readback.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};readback.imageExtent={width_,height_,1};
    readback.bufferOffset=bytes_;
    vkCmdCopyImageToBuffer(command, image_, imageLayout(), buffer_, 1, &readback);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&host,0,nullptr,0,nullptr);
    return Result::Recorded;
}

RgbaTransfer::Result RgbaTransfer::submitAndVerify(const VkSubmitInfo& submit, VkFence fence, presenter_timing::Snapshot* timing, bool sourceOnly) {
    auto operation=iosGpuLifecycle().acquire(queue_);
    if(!operation)return Result::Rejected;
    if (!recorded_ || pending_ || !fence || submit.sType!=VK_STRUCTURE_TYPE_SUBMIT_INFO ||
        submit.commandBufferCount!=1 || !submit.pCommandBuffers) return Result::Rejected;
#if !defined(APPLE_RGBA_MUTANT_UNBOUND_COMMAND)
    if (submit.pCommandBuffers[0]!=recorded_) return Result::Rejected;
#endif
    // Consume before any driver call: reset/submit/wait failures cannot retry.
    recorded_=VK_NULL_HANDLE;
    if (!good(presenter_timing::measure(timing,sourceOnly?presenter_timing::Phase::SourceSubmit:presenter_timing::Phase::Submit,[&]{return vkResetFences(device_,1,&fence);})) || !good(presenter_timing::measure(timing,sourceOnly?presenter_timing::Phase::SourceSubmit:presenter_timing::Phase::Submit,[&]{return vkQueueSubmit(queue_,1,&submit,fence);}))) return Result::DriverFailure;
    pending_=true;
    if (!good(presenter_timing::measure(timing,sourceOnly?presenter_timing::Phase::SourceFence:presenter_timing::Phase::Fence,[&]{return vkWaitForFences(device_,1,&fence,VK_TRUE,5'000'000'000ULL);}))) return Result::DriverFailure;
    pending_=false; initialized_=true;
    if (presenter_timing::measure(timing,presenter_timing::Phase::SourceCompare,[&]{return std::memcmp(static_cast<std::uint8_t*>(mapped_)+bytes_,source_,bytes_);})!=0) return Result::PixelMismatch;
    source_=nullptr;
    ready_=true;
    return Result::Verified;
}
}
