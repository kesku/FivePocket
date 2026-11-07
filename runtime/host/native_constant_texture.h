#pragma once
#include <array>
#include <cstdint>

namespace recomp::gpu {
// Narrow constant-result descriptor, not a guest-memory allocation. Xenia
// 95a5c3ee250f80c3b9d139658649d9ffb6db3eec xenos.h defines word3 bit1..12
// as four 3-bit swizzles; selector4 is literal zero. All other fields below
// match the observed 1x1, pitch256, base0, R8 descriptor exactly. No generic
// null-texture fallback, residency, arbitrary sampler or retail bus claim.
// A native zero image may represent this literal result, but must NEVER be
// reported as bytes read from guest physical address zero.
inline bool isNativeConstantZeroTexture(const std::array<std::uint32_t,6>& words) noexcept {
    return words==std::array<std::uint32_t,6>{0x02024802,2,0,0x00281249,3,0x210};
}
}
