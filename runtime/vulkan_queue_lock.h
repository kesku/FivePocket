#pragma once
#include "diag_timeline.h" // v159
// Attempt v119: one lock for the shared VkQueue. Vulkan requires external synchronization of a queue: every
// vkQueueSubmit (and the idle waits) from any thread holds sharedQueueMutex(). Until v118 the guest threads that
// reach the queue (render 6408e000, tile worker 640b1000, 641c8000) shared permit domain 2, so at most one ran;
// v119 may give them separate domains (diag-domain-split-render), which needs this lock. Lock order: callers may
// hold the registry lock or the generic renderer's mutex; nothing is acquired while this is held.
#include <vulkan/vulkan.h>
#include "apple/ios_gpu_lifecycle.h"
#include <cstdint>
#include <atomic>
#include <mutex>

namespace recomp::gpu {
inline std::mutex& sharedQueueMutex() noexcept { static std::mutex m; return m; }
// v170 (goal step 2, H2b): every submission to the shared queue that is NOT the generic renderer's increments the
// native ticket counter (under the queue lock, in queue order). A native fence that completed records the ticket
// it covers; with files/private-trace/diag-native-fence-only, a later native fence with no native submission since
// then skips its GPU wait instead of waiting for the whole queue (whose tail is the generic renderer's frame, which
// the game cannot observe through that fence: it reads generic GPU output only through the renderer's own readback
// and present writeback, and every guest byte a generic draw uses is copied at capture).
inline std::atomic<std::uint64_t>& nativeSubmissionTickets() noexcept { static std::atomic<std::uint64_t> n{0}; return n; }
inline std::atomic<std::uint64_t>& nativeFencedTickets() noexcept { static std::atomic<std::uint64_t> n{0}; return n; }
inline std::atomic<std::uint64_t>& nativeFencesSkipped() noexcept { static std::atomic<std::uint64_t> n{0}; return n; }
inline thread_local bool genericQueueSubmission = false;
struct GenericSubmissionScope {
    GenericSubmissionScope() noexcept { genericQueueSubmission = true; }
    ~GenericSubmissionScope() { genericQueueSubmission = false; }
};
inline VkResult lockedQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    auto operation=::recomp::apple::iosGpuLifecycle().acquire(queue);
    if(!operation)return VK_ERROR_DEVICE_LOST;
    std::lock_guard lock(sharedQueueMutex());
    if (!genericQueueSubmission) nativeSubmissionTickets().fetch_add(1, std::memory_order_acq_rel);
    return vkQueueSubmit(queue, count, submits, fence);
}
// True when no native submission happened since the last completed native fence (diag-native-fence-only only).
inline bool nativeFenceUnneeded(bool enabled) noexcept {
    if (!enabled) return false;
    if (nativeSubmissionTickets().load(std::memory_order_acquire) != nativeFencedTickets().load(std::memory_order_acquire)) return false;
    nativeFencesSkipped().fetch_add(1, std::memory_order_relaxed);
    return true;
}
// A completed fence covers every native ticket issued before `covered` was read: never claim a later one.
inline void noteNativeTicketsFenced(std::uint64_t covered) noexcept {
    auto seen = nativeFencedTickets().load(std::memory_order_relaxed);
    while (covered > seen && !nativeFencedTickets().compare_exchange_weak(seen, covered, std::memory_order_acq_rel)) {}
}
// Waits for every submission made to `queue` before the call (vkQueueWaitIdle's guarantee for work already
// submitted), without holding the lock while the GPU drains: an empty submission signals a fresh fence after all
// earlier work, and only that submission is locked. Falls back to a locked vkQueueWaitIdle if no fence can be made.
inline VkResult lockedQueueWaitIdle(VkDevice device, VkQueue queue) {
    auto operation=::recomp::apple::iosGpuLifecycle().acquire(queue);
    if(!operation)return VK_ERROR_DEVICE_LOST;
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &info, nullptr, &fence) != VK_SUCCESS) {
        std::lock_guard lock(sharedQueueMutex());
        return vkQueueWaitIdle(queue);
    }
    VkResult result;
    {
        std::lock_guard lock(sharedQueueMutex());
        result = vkQueueSubmit(queue, 0, nullptr, fence);
    }
    recomp::diag::timeline::mark(recomp::diag::timeline::GpuWaitBegin, 1); // v159: queue idle
    if (result == VK_SUCCESS) result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    recomp::diag::timeline::mark(recomp::diag::timeline::GpuWaitEnd, 1);
    vkDestroyFence(device, fence, nullptr);
    return result;
}
} // namespace recomp::gpu
