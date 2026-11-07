// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include "linear_upload.hpp"
#include "presenter_timing.hpp"

namespace recomp::apple {
// A synchronous CPU-frame upload with exact GPU readback. Caller owns the
// device/queue, serializes access and keeps both alive through destruction.
// No game draw, swapchain, presentation, vblank or guest completion is implied.
class RgbaTransfer final {
public:
    enum class Result { Rejected, Recorded, Verified, DriverFailure, PixelMismatch };
    RgbaTransfer() = default;
    ~RgbaTransfer();
    RgbaTransfer(const RgbaTransfer&) = delete;
    RgbaTransfer& operator=(const RgbaTransfer&) = delete;
    bool create(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
                std::uint32_t family, std::uint32_t width, std::uint32_t height);
    Result uploadAndVerify(const std::uint8_t* rgba, std::size_t bytes, presenter_timing::Snapshot* timing=nullptr);
    // Caller has begun a primary command buffer on this device/queue family.
    // Records the upload AND its source readback, without submitting or claiming
    // verification. Append consumers that leave image() in imageLayout(),
    // end the buffer, then call submitAndVerify with that exact single buffer.
    // rgba remains borrowed and immutable until submitAndVerify returns. Neither
    // a second recording nor the standalone upload can overlap this transaction.
    Result recordUpload(VkCommandBuffer command, const std::uint8_t* rgba, std::size_t bytes);
    // Owns reset/submit/wait of a caller-owned fence; never trusts an unrelated
    // completion token. Submission/wait/pixel failure permanently poisons state.
    Result submitAndVerify(const VkSubmitInfo& submit, VkFence fence, presenter_timing::Snapshot* timing=nullptr, bool sourceOnly=false);
    // After Verified, layout is imageLayout(). External readers must
    // restore that layout and finish their GPU work before upload or destruction.
    VkImage image() const noexcept { return image_; }
    VkImageLayout imageLayout() const noexcept { return linear_?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    bool linearHostUpload() const noexcept { return linear_; }
    VkResult error() const noexcept { return error_; }
private:
    enum class LinearChoice { Unsupported, Ready, Failed };
    LinearChoice createLinear();
    bool memory(std::uint32_t bits, VkMemoryPropertyFlags properties, std::uint32_t& type);
    bool good(VkResult result) noexcept { error_=result; return result==VK_SUCCESS; }
    VkPhysicalDevice physical_=VK_NULL_HANDLE;
    VkDevice device_=VK_NULL_HANDLE;
    VkQueue queue_=VK_NULL_HANDLE;
    VkImage image_=VK_NULL_HANDLE;
    VkBuffer buffer_=VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_=VK_NULL_HANDLE, bufferMemory_=VK_NULL_HANDLE;
    VkCommandPool pool_=VK_NULL_HANDLE;
    VkCommandBuffer command_=VK_NULL_HANDLE;
    VkFence fence_=VK_NULL_HANDLE;
    VkCommandBuffer recorded_=VK_NULL_HANDLE;
    const std::uint8_t* source_=nullptr;
    void* mapped_=nullptr;
    std::uint32_t width_=0, height_=0;
    VkDeviceSize bytes_=0;
    VkResult error_=VK_SUCCESS;
    void* imageMapped_=nullptr;
    VkSubresourceLayout imageLayout_{};
    VkDeviceSize imageAllocation_=0;
    bool linear_=false;
    bool ready_=false, initialized_=false, pending_=false;
};
}
