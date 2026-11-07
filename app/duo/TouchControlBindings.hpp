// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include "FullTouchPolicy.hpp"

namespace recomp::apple::fulltouch {
inline NSArray<NSString*>* elementNames() {
    return @[GCInputLeftThumbstick,GCInputRightThumbstick,
        GCInputButtonA,GCInputButtonB,GCInputButtonX,GCInputButtonY,
        GCInputLeftShoulder,GCInputRightShoulder,GCInputLeftTrigger,GCInputRightTrigger,
#if defined(APPLE_TOUCH_MUTANT_DPAD_BINDING)
        GCInputRightThumbstick,
#else
        GCInputDirectionPad,
#endif
        GCInputButtonMenu,GCInputButtonOptions,GCInputLeftThumbstickButton,GCInputRightThumbstickButton];
}
inline NSArray<NSString*>* elementTitles() {
    return @[@"L",@"R",@"A",@"B",@"X",@"Y",@"L1",@"R1",@"L2",@"R2",
        @"D-pad",@"Start",@"Back",@"L3",@"R3"];
}
inline bool directionElement(Element element) noexcept {
    return element==Element::LeftStick || element==Element::RightStick || element==Element::Dpad;
}
inline NSSet<NSString*>* controllerElements(bool fadingTouch) {
    const auto names=elementNames();
    // Apple documents a D-pad/left-stick exclusion and only eleven possible
    // configuration names. A general GC name is NOT virtual-profile support.
    if(fadingTouch) {
#if defined(APPLE_TOUCH_MUTANT_UNSUPPORTED_VIRTUAL)
        return [NSSet setWithArray:names];
#else
        @throw [NSException exceptionWithName:NSInvalidArgumentException
            reason:@"Full touch controls require an app-owned input backend, not unsupported GCVirtualController elements."
            userInfo:nil];
#endif
    }
    return [NSSet setWithArray:[names subarrayWithRange:NSMakeRange(0,10)]];
}
}
