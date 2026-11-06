#pragma once
#include <cstdint>
#include <vector>

namespace recomp::gpu {
struct NativeClippedFanProgram {
    std::vector<std::uint8_t> vertexIdentity,pixelIdentity;
    std::vector<std::uint32_t> positionCode;
    unsigned stride=0;
};
}
