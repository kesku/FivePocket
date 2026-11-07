// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger,PortLauncherAction) {
    PortLauncherPlay,PortLauncherVerify,PortLauncherSave,PortLauncherRestoreOriginal,PortLauncherDiscard,
    PortLauncherImport // (2026-11-05: the Play tab's "Import Game Discs": the on-device disc import, ios_disc_import_view.h)
};
typedef NS_ENUM(NSInteger,PortLauncherSetting) {
    PortLauncherFrameCap,PortLauncherScaler,PortLauncherSharpness,PortLauncherResolution,PortLauncherFpsCounter,PortLauncherFrameGeneration,
    PortLauncherDrawDistance,PortLauncherTrafficDensity,PortLauncherShadowDistance,PortLauncherScreen // (Claude, 2026-10-20/21)
#if defined(RECOMP_DUO_EDITION)
    ,PortLauncherDuoScreens // ios-duo (2026-10-24): Duo (split) / Full screen; settings/duo-screen.txt, applied on Save
#endif
};
#if defined(RECOMP_DUO_EDITION)
inline constexpr unsigned PortLauncherRowCount=11; // ios-duo: the ten rows + Screens
#endif
@class PortLauncherController;
// Acceptance is a host-operation result, not proof the game honours a setting.
// The integrating host must bind snapshots, persist successfully and validate
// each runtime consumer. This view never writes settings or starts the game.
@protocol PortLauncherDelegate <NSObject>
- (BOOL)launcher:(PortLauncherController*)launcher action:(PortLauncherAction)action;
// Draft change only. Persist solely in a successful explicit Save operation.
- (void)launcher:(PortLauncherController*)launcher changed:(PortLauncherSetting)setting value:(NSInteger)value;
@end

@interface PortLauncherRow : NSObject
@property(nonatomic) PortLauncherSetting setting;
@property(nonatomic,copy) NSString* title;
@property(nonatomic,copy) NSArray<NSString*>* choices;
@property(nonatomic) NSInteger value;
@property(nonatomic) NSInteger originalValue;
@property(nonatomic) BOOL editable;
// Required, explicit choice allowlist. Unavailable choices are never selected
// by stepping or Restore Original; a caller must supply real consumer evidence.
@property(nonatomic,copy) NSIndexSet* allowedValues;
@property(nonatomic,copy) NSString* help;
@end

@interface PortLauncherController : UIViewController
@property(nonatomic,weak) id<PortLauncherDelegate> delegate;
@property(nonatomic,copy) NSString* statusText;
@property(nonatomic,copy) NSString* controllerText;
// Optional MAIN-actor takeover notification at manual UIKit event entry only.
// Agent commands never invoke it. Owner binds Gamepad::ownerActivity; callback
// must not retain the view, and input lifetime must outlive that borrowed owner.
@property(nonatomic,copy) void(^manualInputHandler)(void);
@property(nonatomic,readonly) NSInteger currentPage;
@property(nonatomic,readonly) BOOL dirty;
// Read-only input lifetime/focus state; no guest/controller manipulation.
@property(nonatomic,readonly) BOOL inputClosed;
@property(nonatomic,readonly) BOOL hostBusy;
@property(nonatomic,readonly) NSUInteger inputEpoch;
// All ten rows must be supplied from the actual configuration, even when a
// capability is unavailable. No assumed Android defaults or effect claims.
- (instancetype)initWithRows:(NSArray<PortLauncherRow*>*)rows about:(NSArray<NSString*>*)sections;
// Navigation commands match LauncherNavigation::Command. This API consumes
// pre-game events; it never takes GameController handlers away from an owner.
- (void)handleCommand:(NSInteger)command repeat:(BOOL)repeat;
- (BOOL)resolveLeave:(PortLauncherAction)action;
- (void)cancelLeave;
- (void)setHostBusy:(BOOL)busy;
// Irreversible for this view/session. Call before guest ownership is published.
- (void)closeForGuest;
// (Claude, test runs) One line of the view's state: page, level, column, focused row, the rows' values, unsaved, status.
- (NSString*)stateLine;
@end
