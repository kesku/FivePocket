#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace recomp::gpu {
// Indexed inline format1 is 16-bit with 8-in-16 byte swap. The original
// caller copies BE16 indices verbatim. No restart/base-vertex mode is guessed.
inline std::optional<std::vector<std::uint32_t>> decodeNativeInlineIndices(
        unsigned count,unsigned format,unsigned minVertex,unsigned vertexCount,
        const std::vector<std::uint8_t>& bytes) {
    if(!count || count%3 || count>65535 || format!=1 || minVertex ||
       !vertexCount || bytes.size()!=std::uint64_t{count}*2)return std::nullopt;
    std::vector<std::uint32_t> indices;indices.reserve(count);
    for(unsigned i=0;i<count;++i) {
        const auto index=(std::uint32_t{bytes[i*2]}<<8)|bytes[i*2+1];
        if(index>=vertexCount)return std::nullopt;
        indices.push_back(index);
    }
    return indices;
}
}
