#pragma once
#include <array>
#include <cstdint>

namespace recomp::input {
// Host-native snapshot. Android owns device discovery and event collection;
// guest byte order is applied only at the import boundary.
struct GamepadState {
    std::uint32_t packet=0;
    std::uint16_t buttons=0;
    std::uint8_t leftTrigger=0,rightTrigger=0;
    std::int16_t leftX=0,leftY=0,rightX=0,rightY=0;
};
static_assert(sizeof(GamepadState)==16);
inline std::array<std::uint8_t,16> encodeGamepadState(const GamepadState& s) {
    std::array<std::uint8_t,16> out{};
    for(unsigned i=0;i<4;++i)out[i]=static_cast<std::uint8_t>(s.packet>>(24-i*8));
    const auto word=[&](unsigned at,std::uint16_t v){out[at]=static_cast<std::uint8_t>(v>>8);out[at+1]=static_cast<std::uint8_t>(v);};
    word(4,s.buttons);out[6]=s.leftTrigger;out[7]=s.rightTrigger;
    word(8,static_cast<std::uint16_t>(s.leftX));word(10,static_cast<std::uint16_t>(s.leftY));
    word(12,static_cast<std::uint16_t>(s.rightX));word(14,static_cast<std::uint16_t>(s.rightY));
    return out;
}
}
