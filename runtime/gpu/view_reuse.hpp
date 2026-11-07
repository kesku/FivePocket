// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <vulkan/vulkan.h>

namespace convert_view_reuse {
enum class Policy { Disabled, Enabled, Invalid };
inline Policy parsePolicy(const char* value) noexcept {
    if (!value || std::string_view(value)=="0") return Policy::Disabled;
    if (std::string_view(value)=="1") return Policy::Enabled;
    return Policy::Invalid;
}
// These are the actual Resolved/Image fields, not a separately maintained cache.
// A color resolve's makeImage/reuseSpare path establishes their view provenance.
template<class Resolved>
bool eligible(const Resolved& r, unsigned mip, unsigned slice) noexcept {
    const auto& i=r.image;
    return i.image!=VK_NULL_HANDLE && i.view!=VK_NULL_HANDLE && r.storage &&
        i.format==VK_FORMAT_R16G16B16A16_SFLOAT && i.aspect==VK_IMAGE_ASPECT_COLOR_BIT &&
        !i.cube && !i.array && !i.volume && !i.depthWord &&
        i.levels==1 && i.layers==1 && r.levels==1 && r.layers==1 && !r.cube &&
        mip==0 && slice==0 && r.view==std::array<unsigned,4>{0,1,2,3};
}
struct Counters {
    std::uint64_t viewRequests=0, eligible=0, borrowed=0, created=0, retireScheduled=0, createFailed=0;
};
}
