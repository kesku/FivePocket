// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <string_view>
namespace recomp::apple::fulltouch {
enum class Route {Normal,LegacyDiagnostic,Invalid};
// Measurement alone is observational and must not silently remove normal touch.
// Existing recorded/system routes remain native and separate from app-owned UI.
inline Route route(std::span<const std::string_view> arguments) noexcept {
    unsigned start=0,recorded=0,system=0,virtualPad=0,aim=0,capture=0,lifecycle=0;
    for(auto arg:arguments) {
        start+=arg=="--start-recorded-test";recorded+=arg=="--recorded-input";
        system+=arg=="--system-touch-controller";virtualPad+=arg=="--virtual-controller";
        aim+=arg=="--recorded-aim-input";capture+=arg=="--metal-capture";lifecycle+=arg=="--resumable-lifecycle";
    }
    if(start>1 || recorded>1 || system>1 || virtualPad>1 || aim>1 || capture>1 || lifecycle>1 ||
        (system && recorded) || ((recorded || system) && !virtualPad) || ((aim || capture) && !recorded))return Route::Invalid;
    const bool legacy=start || recorded || system;
    if(virtualPad && !legacy)return Route::Invalid;
    return legacy?Route::LegacyDiagnostic:Route::Normal;
}
}
