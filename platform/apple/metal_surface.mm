// SPDX-License-Identifier: GPL-3.0-or-later
#define VK_USE_PLATFORM_METAL_EXT 1
#include "apple/metal_surface.h"
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

namespace recomp::apple {
VkResult createMetalSurface(VkInstance instance, void* layer, VkSurfaceKHR* surface) {
    if (!surface) return VK_ERROR_INITIALIZATION_FAILED;
    *surface = VK_NULL_HANDLE;
    if (!instance || !layer || ![NSThread isMainThread]) return VK_ERROR_INITIALIZATION_FAILED;
    CAMetalLayer* metal = (__bridge CAMetalLayer*)layer;
    if (![metal isKindOfClass:[CAMetalLayer class]]) return VK_ERROR_INITIALIZATION_FAILED;
    const auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    info.pLayer = metal;
    return create(instance, &info, nullptr, surface);
}
}
