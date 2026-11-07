#pragma once
#include "guest_me_init.h"
#include <vector>

namespace recomp::bootstrap {
struct NativeInitialCoherency {
    std::uint32_t physicalBase = 0, byteCount = 0, cacheFlags = 0;
};
inline bool validNativeInitialCoherency(const NativeInitialCoherency& request) noexcept {
    return (request.cacheFlags == 0x01000000 || request.cacheFlags == 0x03000000) &&
        request.byteCount && !((request.physicalBase | request.byteCount) & 0xFFF) &&
        std::uint64_t{request.physicalBase} + request.byteCount <= 0x20000000;
}
// Exact observed native cold-start command profile. NOT a general PM4 engine:
// root ME_INIT + one IB; child cache-register writes followed by an equality
// wait for busy bit 31 to clear. Ranges are bytes (title aligns them to 4 KiB).
// No wait is satisfied here and no state is published on failed validation.
inline bool parseNativeCacheCoherencyChild(const std::vector<std::uint32_t>& child,
        NativeInitialCoherency& output) noexcept {
    if (child.size() != 11 || child[0] != 0xA31 || child[2] != 0x10A2F ||
        child[5] != 0xC0043C00 || child[6] != 3 || child[7] != 0xA31 || child[8] != 0 ||
        child[9] != 0x80000000 || child[10] != 8) return false;
    const NativeInitialCoherency request{child[4],child[3],child[1]};
    if (!validNativeInitialCoherency(request)) return false;
    output = request; return true;
}
inline bool parseNativeEmptyCacheCoherency(const std::vector<std::uint32_t>& root,
        const std::vector<std::uint32_t>& child, NativeInitialCoherency& output) noexcept {
    return root.size() == 3 && root[0] == 0xC0013F00 && root[2] == 11 && !(root[1]&3) &&
        std::uint64_t{root[1]}+44 <= 0x20000000 && parseNativeCacheCoherencyChild(child,output);
}
inline bool parseNativeInitialCoherency(const std::vector<std::uint32_t>& root,
        const std::vector<std::uint32_t>& child, NativeInitialCoherency& output) noexcept {
    if (root.size() != 22 || root[19] != 0xC0013F00 || root[21] != 11 || (root[20] & 3) ||
        std::uint64_t{root[20]} + 44 > 0x20000000 || child.size() != 11) return false;
    std::array<std::uint32_t, 19> me{};
    for (std::size_t i = 0; i < me.size(); ++i) me[i] = root[i];
    return matchesNativeMeInitProfile(me) && parseNativeCacheCoherencyChild(child,output);
}
}
