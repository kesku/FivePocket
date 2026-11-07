#pragma once
#include <array>
#include <cstdint>

namespace recomp::bootstrap {
// Native compatibility profile for the observed ordinal-1DF verification hook.
// The legacy import table calls it KiApcNormalRoutineNop, but this call supplies
// an ME_INIT packet, not an APC argument list. This validates only the observed
// configuration; it does not reproduce an undocumented kernel verifier, execute
// microcode, initialize CP state or approve future arbitrary PM4 packets.
inline bool matchesNativeMeInitProfile(const std::array<std::uint32_t, 19>& words) noexcept {
    if (words[0] != 0xC0114800 || words[1] != 0x3FF || words[2] || words[3]) return false;
    for (std::uint32_t i = 0; i < 8; ++i) if (words[4 + i] != i * 0x80) return false;
    if (words[12] != 0x00010800 || words[13] != 7) return false;
    for (std::uint32_t i = 14; i < words.size(); ++i) if (words[i]) return false;
    return true;
}
}
