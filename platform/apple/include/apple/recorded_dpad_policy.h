// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace recomp::apple::gamepad {
// Development-only supplement for an explicitly leased, selected virtual pad.
// This never selects a controller or replaces real GameController values.
class RecordedDpadPolicy final {
public:
    std::uint64_t acquire(std::uint64_t owner,std::uint64_t selected,bool available) noexcept {
        if (owner_ || !owner || owner!=selected || !available) return 0;
        if (++generation_==0) ++generation_;
        owner_=owner;bits_=0;return generation_;
    }
    bool reconcile(std::uint64_t selected,bool available) noexcept {
        if (!owner_ || (owner_==selected && available)) return false;
        owner_=0;bits_=0;return true;
    }
    bool active(std::uint64_t generation) const noexcept {
        return owner_ && generation && generation==generation_;
    }
    bool set(std::uint64_t generation,std::uint16_t bits) noexcept {
        if (!active(generation)) return false;
        // A malformed command may clear its own hold, never another lease.
        if ((bits&~0x000fu) || (bits&3u)==3u || (bits&12u)==12u) {
            bits_=0;return false;
        }
        bits_=bits;return true;
    }
    bool close(std::uint64_t generation) noexcept {
        if (!active(generation)) return false;
        owner_=0;bits_=0;return true;
    }
    std::uint64_t owner() const noexcept {return owner_;}
    std::uint16_t bits(std::uint64_t selected) const noexcept {
        return owner_ && owner_==selected?bits_:0;
    }
    std::uint16_t capabilities(std::uint64_t selected) const noexcept {
        return owner_ && owner_==selected?0x000f:0;
    }
private:
    std::uint64_t owner_=0,generation_=0;
    std::uint16_t bits_=0;
};
}
