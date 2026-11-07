// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LauncherDisplayOptions.hpp"
#include "pace_caps.h"
#include "render_scale.h"
extern "C" std::uint32_t recomp_apple_launcher_display_options() noexcept;
namespace recomp::apple::launcher::bridge {
inline std::uint32_t word() noexcept {
    return ::recomp_apple_launcher_display_options();
}
inline bool fps(bool legacy) noexcept {return fpsCounter(word(),legacy);}
// (Claude, 2026-10-18) the saved Frame Generation choice (false without a saved word)
inline bool frameGeneration() noexcept {return recomp::apple::launcher::frameGeneration(word());}
// (Claude, 2026-10-20) the saved Draw Distance in percent (100 without a saved word or at Original)
inline unsigned drawDistance() noexcept {const auto p=drawDistancePercent(word());return p?*p:100u;}
// (Claude, 2026-10-20) the saved Traffic Density in percent (100 without a saved word or at Original)
inline unsigned traffic() noexcept {const auto p=trafficPercent(word());return p?*p:100u;}
// (Claude, 2026-10-20) the saved Shadow Distance: Near (false without a saved word or at Original)
inline bool shadowNear() noexcept {return recomp::apple::launcher::shadowNear(word());}
inline unsigned pace(unsigned legacy) noexcept {
    namespace pc=recomp::pace;
    return paceSwitches(word(),legacy,pc::SwCaps,pc::SwCap40,pc::SwCap30,pc::SwCap24,pc::SwAuto);
}
inline recomp::gpu::rs::Setting resolution(recomp::gpu::rs::Setting legacy) noexcept {
    const auto percent=resolutionPercent(word());if(!percent)return legacy;
    char text[4];const auto end=std::to_chars(text,text+sizeof text,*percent);
    return recomp::gpu::rs::parse(text,std::size_t(end.ptr-text));
}
}
