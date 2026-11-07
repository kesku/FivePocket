// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace recomp::apple {
inline bool hasExtension(std::span<const VkExtensionProperties> all, const char* name) {
    return std::any_of(all.begin(), all.end(), [=](const auto& x) { return std::strcmp(x.extensionName, name) == 0; });
}
// Static MoltenVK can enumerate without the loader extension. If advertised,
// enable both the portability extension and its matching instance-create flag.
inline bool selectInstanceExtensions(std::span<const VkExtensionProperties> all,
        bool surface, std::vector<const char*>& out, VkInstanceCreateFlags& flags) {
    out.clear(); flags = 0;
    if (surface && (!hasExtension(all, "VK_KHR_surface") || !hasExtension(all, "VK_EXT_metal_surface"))) return false;
    if (hasExtension(all, "VK_KHR_portability_enumeration")) {
        out.push_back("VK_KHR_portability_enumeration");
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    if (surface) {
        out.push_back("VK_KHR_surface");
#if !defined(APPLE_VULKAN_MUTANT_OMIT_METAL)
        out.push_back("VK_EXT_metal_surface");
#endif
    }
    return true;
}
inline bool instanceExtensions(bool surface, std::vector<const char*>& out, VkInstanceCreateFlags& flags) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count > 1024) return false;
    std::vector<VkExtensionProperties> all(count);
    if (count && vkEnumerateInstanceExtensionProperties(nullptr, &count, all.data()) != VK_SUCCESS) return false;
    all.resize(count);
    return selectInstanceExtensions(all, surface, out, flags);
}
inline void appendDevicePortability(std::span<const VkExtensionProperties> all, std::vector<const char*>& out) {
    if (hasExtension(all, "VK_KHR_portability_subset") &&
            std::none_of(out.begin(), out.end(), [](const char* x) { return std::strcmp(x,"VK_KHR_portability_subset") == 0; }))
        out.push_back("VK_KHR_portability_subset");
}
}
