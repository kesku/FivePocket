// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fan_indices.hpp"

namespace fan_large_candidate {
using Policy = fan_indices_candidate::StartupPolicy;
// Independent opt-in. Existing small-fan policy is not consulted or changed.
inline Policy parsePolicy(const char* value) noexcept {
    if (!value) return Policy::Disabled;
    return fan_indices_candidate::parseStartupPolicy(value);
}
// The opt-in limit is exact; malformed environment values fail closed.
inline std::uint32_t parseLimit(const char* value) noexcept {
    if (!value || std::string_view(value) == "96") return 96;
    if (std::string_view(value) == "192") return 192;
    return 0;
}
inline bool eligible(Policy policy, unsigned primitive, bool indexed,
                     std::uint32_t count, std::uint32_t maxCount = 96) noexcept {
    return (maxCount == 96 || maxCount == 192) && policy == Policy::Enabled &&
           primitive == 5 && !indexed && count >= 65 && count <= maxCount;
}
inline fan_indices_candidate::Result write(Policy policy, unsigned primitive, bool indexed,
        std::uint32_t count, std::uint32_t* output, std::size_t capacity, std::uint32_t maxCount = 96) noexcept {
    if (!fan_large_candidate::eligible(policy, primitive, indexed, count, maxCount)) return {};
    const std::size_t needed = std::size_t(count - 2) * 3;
    if (!output || capacity < needed) return {};
    for (std::uint32_t next = 2; next < count; ++next) {
        output[3 * (next - 2)] = next - 1;
        output[3 * (next - 2) + 1] = next;
        output[3 * (next - 2) + 2] = 0;
    }
    return {true, needed};
}
struct Census {
    struct Counts { std::uint64_t small=0, large=0, above=0, indexed=0, n97_128=0, n129_256=0, above256=0; std::uint32_t maxNonindexed=0; } eligible{}, submitted{};
    static void record(Counts& counts, unsigned primitive, bool indexed, std::uint32_t n) noexcept {
        if (primitive != 5) return;
        if (indexed) { ++counts.indexed; return; }
        if (n < 3) return;
        if (n > counts.maxNonindexed) counts.maxNonindexed=n;
        if (n <= 64) ++counts.small;
        else if (n <= 96) ++counts.large;
        else {
            ++counts.above;
            if (n <= 128) ++counts.n97_128;
            else if (n <= 256) ++counts.n129_256;
            else ++counts.above256;
        }
    }
    void observe(unsigned p, bool i, std::uint32_t n) noexcept { record(eligible,p,i,n); }
    void submit(unsigned p, bool i, std::uint32_t n) noexcept { record(submitted,p,i,n); }
};
}
