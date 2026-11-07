#pragma once
// Attempt v177: every vkQueueSubmit / vkQueueWaitIdle written after this include in a translation unit goes through
// recomp::gpu::sharedQueueMutex() (vulkan_queue_lock.h). v119 locked the call sites in its own files, but the native
// engine's helper headers (native_*_target.h, native_*_draw.h, native_*swap_gpu_execution.h, vulkan_maintenance_points.h,
// included by vulkan_engine.h / renderer_boundary_trace.cpp) still called the queue directly; they were safe only
// because every thread reaching them shared permit domain 2 with the generic renderer's submitters. v177 lets the
// generic renderer's worker thread submit itself (generic_renderer.cpp queueSubmit), so the lock must cover them too.
// Include it first (before any header that calls the queue): <vulkan/vulkan.h> is declared by vulkan_queue_lock.h
// before the macros exist, and the lock's own calls (in vulkan_queue_lock.h) stay the real functions. Only the six
// translation units that reference the queue (checked with llvm-nm on every object of the v176 link) include it.
#include "vulkan_queue_lock.h"
namespace recomp::gpu {
inline VkResult lockedQueueWaitIdleHeld(VkQueue queue) {
    auto operation=::recomp::apple::iosGpuLifecycle().acquire(queue);
    if(!operation)return VK_ERROR_DEVICE_LOST;
    std::lock_guard lock(sharedQueueMutex());
    return vkQueueWaitIdle(queue);
}
} // namespace recomp::gpu
#define vkQueueSubmit(...) ::recomp::gpu::lockedQueueSubmit(__VA_ARGS__)
#define vkQueueWaitIdle(...) ::recomp::gpu::lockedQueueWaitIdleHeld(__VA_ARGS__)
