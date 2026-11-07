// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import "LauncherController.h"
#ifdef __cplusplus
#include "LauncherSettingsOwner.hpp"
#endif
@class PortLauncherCoordinator;
@protocol PortLauncherHost <NSObject>
// YES means an existing authenticated host operation was admitted, not that the
// game has started. The host MUST call a completion or invalidate on all paths.
- (BOOL)launcherRequestsPlay:(PortLauncherCoordinator*)owner;
- (BOOL)launcherRequestsVerification:(PortLauncherCoordinator*)owner;
@end

@interface PortLauncherCoordinator : NSObject<PortLauncherDelegate>
@property(nonatomic,weak) id<PortLauncherHost> host;
@property(nonatomic,strong,readonly) PortLauncherController* controller;
#ifdef __cplusplus
- (instancetype)initWithDirectory:(int)directory policy:(const recomp::apple::launcher::SettingsPolicy&)policy
                            about:(NSArray<NSString*>*)sections;
- (recomp::apple::launcher::SettingsOwnership)ownership;
- (recomp::apple::launcher::DisplayChoices)draftChoices;
#endif
- (void)verificationFinished;
// Must be called inside the authenticated preparation admission immediately
// BEFORE starting the worker. A failure forbids start. Diagnostic freezes0.
- (BOOL)commitAuthenticatedGuest:(BOOL)diagnostic;
- (void)invalidateWithStatus:(NSString*)reason;
@end
