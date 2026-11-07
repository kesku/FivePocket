// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#import "LauncherController.h"
#include "apple/gamepad_input.h"
#include "AgentInputPolicy.hpp"
namespace recomp::apple::fulltouch {class Gamepad;}
namespace recomp::apple::agentmenu {struct Hooks;}

// Main-thread lifetime only. Observes the existing adapter; a timer drives only
// repeat deadlines, never polls GCController or delivers inputs to the guest.
@interface PortLauncherInput : NSObject
- (instancetype)initWithSource:(recomp::apple::AppleGamepadInput*)source controller:(PortLauncherController*)controller;
- (instancetype)initWithAppSource:(recomp::apple::fulltouch::Gamepad*)source controller:(PortLauncherController*)controller;
// Optional MAIN-actor receipt hooks, borrowed until close. They must name the
// very same app source. Default initializer retains the old no-agent path.
- (instancetype)initWithAppSource:(recomp::apple::fulltouch::Gamepad*)source controller:(PortLauncherController*)controller agentHooks:(const recomp::apple::agentmenu::Hooks*)hooks;
@property(nonatomic,readonly) BOOL active;
// Available ONLY while a real admitted input event synchronously dispatches
// its UI action. Never a cached intent, snapshot, repeat or consumer ACK.
- (recomp::apple::agentinput::Publication)executingAgentPublication;
// Explicit enabled-channel preflight only. Read the SAME app source and consume
// its coherent neutral state through the real ready menu input path. Not a
// timer-only freshness/ACK update; never polls GCController or accepts held input.
// The ordinary repeat timer does not call this. No lease/image/authority created.
- (BOOL)consumeIdleAppSource;
- (void)close;
#if defined(APPLE_LAUNCHER_INPUT_TESTING)
// Host fixture channel only, compiled out of every production build.
- (instancetype)initWithChannel:(recomp::apple::gamepad::EventChannel*)channel controller:(PortLauncherController*)controller;
- (void)testRepeatAt:(std::uint64_t)milliseconds;
- (void)testEventAt:(std::uint64_t)milliseconds;
#endif
@end
