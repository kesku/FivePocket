// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan.h>
namespace recomp::apple {
// Call on the main thread with a live CAMetalLayer bridged to void*. The view
// owner retains that layer until after vkDestroySurfaceKHR. No hidden window,
// resize loop, queue ownership or automatic retry is introduced here.
VkResult createMetalSurface(VkInstance instance, void* layer, VkSurfaceKHR* surface);
}
