// SPDX-License-Identifier: GPL-3.0-or-later
#import "LauncherCoordinator.h"
#define PORT_MENU_SCENE_VIEW_INTERFACE_ONLY 1 // (PortMenuSceneView is defined in Main.mm's unit)
#include "ios_disc_import_view.h"
#include "ios_bundle_seed.h"
#ifndef RECOMP_PAYLOAD_MANIFEST_SHA256
#define RECOMP_PAYLOAD_MANIFEST_SHA256 "" // (claude_build.py compiles the payload manifest's SHA-256 in; "" = a build without a payload)
#endif
#include <memory>
using namespace recomp::apple::launcher;

#if defined(RECOMP_DUO_EDITION)
extern "C" int recomp_duo_split() noexcept; // ios-duo (Main.mm): 1 = the portrait split, 0 = fullscreen
extern "C" void recomp_duo_request_split(int on) noexcept; // ios-duo (Main.mm): writes settings/duo-screen.txt and applies it
#endif
@implementation PortLauncherCoordinator {
    std::unique_ptr<LauncherSettingsOwner> _owner;
    PortLauncherController* _controller;
#if defined(RECOMP_DUO_EDITION)
    NSInteger _duoDraft; // ios-duo: the Screens row's draft (0 Duo, 1 Full screen); saved with Save, as every other row
#endif
    BOOL _importShown; // (the disc import page opened by itself once this launch)
}
- (instancetype)initWithDirectory:(int)directory policy:(const SettingsPolicy&)policy about:(NSArray<NSString*>*)sections {
    if(!NSThread.isMainThread || !policy.validPolicy())
        @throw [NSException exceptionWithName:NSInvalidArgumentException reason:@"Main-thread captured configuration and capabilities required" userInfo:nil];
    if(policy.allowed[1]!=(1ull<<policy.original.values[1]) || policy.allowed[2]!=(1ull<<policy.original.values[2]))
        @throw [NSException exceptionWithName:NSInvalidArgumentException reason:@"Apple scaler/sharpness must be unavailable, not silently selectable" userInfo:nil];
    self=[super init];if(!self)return nil;
    _owner=std::make_unique<LauncherSettingsOwner>(directory,policy);
    NSArray* titles=@[@"Frame Rate Cap",@"Scaler",@"FSR Sharpness",@"Render Resolution",@"Frame-Rate Counter",@"Frame Generation",@"Draw Distance",@"Traffic Density",@"Shadow Distance",@"Screen"];
    NSMutableArray* unavailableSharp=[NSMutableArray new];
    for(unsigned i=0;i<41;++i)[unavailableSharp addObject:@"Unavailable on Apple"];
    NSArray* choices=@[@[@"Off (standing 60 fps)",@"60 fps",@"40 fps",@"30 fps",@"24 fps",@"Automatic"],
        @[@"Unavailable on Apple",@"Unavailable on Apple",@"Unavailable on Apple",@"Unavailable on Apple"],unavailableSharp,
        @[@"50% (internal)",@"75% (internal)",@"88% (internal)",@"100% (internal)",@"115% (internal)",@"125% (internal)",@"150% (internal)"],@[@"Off",@"On"],
        @[@"Off",@"On"],@[@"Original",@"90%",@"80%",@"70%"],@[@"Original",@"75%",@"50%",@"25%"],@[@"Original",@"Near"],@[@"Fill",@"Fit"]];
    const auto loaded=_owner->choices();NSMutableArray* rows=[NSMutableArray new];
    for(unsigned i=0;i<10;++i) {
        auto* row=[PortLauncherRow new];row.setting=PortLauncherSetting(i);row.title=titles[i];row.choices=choices[i];
        row.value=loaded.values[i];row.originalValue=policy.original.values[i];
        const bool unavailable=i==1 || i==2;row.editable=!unavailable;
        NSMutableIndexSet* allowed=[NSMutableIndexSet new];
        if(!unavailable)for(unsigned j=0;j<ChoiceCounts[i];++j)if(policy.allowed[i]&(1ull<<j))[allowed addIndex:j];
        row.allowedValues=allowed;
        row.help=unavailable?@"No Apple scaler/sharpness implementation; not an effective setting.":
            i==0?@"Original: 30 fps, evenly paced on the 60 Hz display. Off, 60 and 40 let the game run up to 60 fps.":
            i==3?@"Internal rendering against720p. Device-limit admission applies; this is not Native1080 output.":
            i==9?@"Fill covers the whole screen (the picture's edges are cut where the screen's shape differs from 16:9; the HUD stays whole). Fit shows the whole picture with bars, and is about 1 ms per frame faster while driving on the iPad mini.":
            i==8?@"Near leaves out the farthest shadow cascade: distant buildings and trees cast no shadow. Cuts CPU work while driving (about 1 ms per frame on the iPad mini).":
            i==7?@"How many moving and parked cars the game spawns around you. Lower values cut GPU and CPU work while driving (25-50%: about 1-2 ms per frame on the iPad mini). Missions that set their own traffic keep their lower value.":
            i==6?@"How far detailed buildings, roads and props reach before simpler models. Lower values cut GPU work while driving (70%: about 3 ms per frame on the iPad mini); some distance checks of traffic and pedestrians follow the same scale.":
            i==5?@"Generated frames between real ones: 30 fps game, 60 fps display. Uses the 30 fps cap; a scaled resolution renders through the 720p image.":
                 @"Renderer overlay selection; not a physical display-FPS measurement.";
        [rows addObject:row];
    }
#if defined(RECOMP_DUO_EDITION)
    { // ios-duo: the Screens row (Display): the portrait split with the lower screen, or the regular landscape full screen
        _duoDraft=recomp_duo_split()?0:1;
        auto* row=[PortLauncherRow new];row.setting=PortLauncherDuoScreens;row.title=@"Screens";row.choices=@[@"Duo (split)",@"Full screen"];
        row.value=_duoDraft;row.originalValue=0;row.editable=YES;row.allowedValues=[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0,2)];
        row.help=@"Duo: portrait, the game on the top half and the Thor edition's lower screen (map, wheels, Resume) on the bottom half. Full screen: landscape, as the regular app. In game: the button on the lower screen, or a two-finger double tap in full screen.";
        [rows addObject:row];
    }
#endif
    _controller=[[PortLauncherController alloc] initWithRows:rows about:sections];_controller.delegate=self;
    _controller.controllerText=@"A Select · B Back · LB/RB Tabs · Start Save. Physical menu event source pending qualification.";
    if(_owner->ownership()==SettingsOwnership::Disabled) {
        _controller.statusText=@"Settings unavailable or conflict with capabilities. No start or automatic recovery.";
        [_controller setHostBusy:YES];
    } else {
        _controller.statusText=@"Saved choices load on start; physical effects still require qualification.";
        [self seedFromBundle];
    }
    return self;
}
// (2026-11-05, the builder's iOS brief option (b)) the builder's parts inside the .app (payload/: the runtime catalog's assets that are not
// disc files - the switches the configuration gate reads, the template run, modules/boot, the shader pack, the file catalogs) are seeded
// into Library/r by Main.mm's setupLauncher BEFORE that gate (a fresh container without the switches never reaches the launcher: device
// check 2026-11-05), through this function - this unit carries the compiled payload pin. Clones on APFS, else checked copies.
extern "C" bool port_seed_bundle_payload(const char* root,char* error,size_t size) {
    static const std::string pin=RECOMP_PAYLOAD_MANIFEST_SHA256;
    if(pin.empty())return true; // (a build without a payload: its data came another way, e.g. the devicectl transfer)
    NSString* payload=[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"payload"];
    const auto r=recomp::apple::bundleseed::seed(payload.fileSystemRepresentation,root,pin);
    std::fprintf(stderr,"APPLE_IOS_BUNDLE_SEED ok=%d seeded=%d kept=%d cloned=%d copied=%d bytes=%llu error=%s\n",int(r.ok),r.seeded,r.kept,r.cloned,r.copied,
                 (unsigned long long)r.bytes,r.error.c_str());
    if(r.ok){NSURL* inputs=[NSURL fileURLWithPath:[NSString stringWithUTF8String:(std::string(root)+"/private-trace/inputs").c_str()] isDirectory:YES];
             [inputs setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];}
    else if(error && size)std::snprintf(error,size,"%s",r.error.c_str());
    return r.ok;
}
- (void)seedFromBundle {[self discStatus];} // (the seed itself ran before the launcher existed: port_seed_bundle_payload)
/** The disc files: while any is missing, the import page opens by itself (the owner: "the data needs to be imported from the disc no
 *  matter what" - the required first-run path), once per launch; closed without completing it, Import Game Discs stays on the Play tab. */
- (void)discStatus {
    const std::string inputs=std::string(NSHomeDirectory().fileSystemRepresentation)+"/Library/r/private-trace/inputs";
    if(recomp::apple::discimport::missing(inputs,recomp::apple::discimport::catalog::pins()).empty())return;
    _controller.statusText=@"The game's files from your discs are not in place yet: choose Import Game Discs.";
    [self openImportWhenShown:20];
}
- (void)openImportWhenShown:(int)tries {
    if(_importShown || tries<=0)return;
    if(!_controller.view.window || _controller.presentedViewController) { // (the launcher is not on screen yet: look again shortly)
        __weak PortLauncherCoordinator* weak=self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(0.25*NSEC_PER_SEC)),dispatch_get_main_queue(),^{[weak openImportWhenShown:tries-1];});
        return;
    }
    _importShown=YES;[self launcher:_controller action:PortLauncherImport];
}
- (PortLauncherController*)controller {return _controller;}
- (SettingsOwnership)ownership {return _owner->ownership();}
- (DisplayChoices)draftChoices {return _owner->choices();}
- (BOOL)launcher:(PortLauncherController*)view action:(PortLauncherAction)action {
    if(!NSThread.isMainThread || view!=_controller || _owner->ownership()!=SettingsOwnership::Menu)return NO;
    if(action==PortLauncherDiscard)return _owner->discard();
    if(action==PortLauncherSave) {
#if defined(RECOMP_DUO_EDITION)
        recomp_duo_request_split(_duoDraft==0?1:0); // ios-duo: the Screens row (its own file; the ten choices below as before)
#endif
        SavedSettings saved{SaveStatus::Failed,std::nullopt,0};
        try {saved=_owner->save();}
        catch(...) {[self invalidateWithStatus:@"Save raised an error. No successful Save claimed; restart required."];return NO;}
        const bool acknowledged=saved.status==SaveStatus::Saved && saved.value && !_owner->dirty();
        _controller.statusText=acknowledged?@"Saved atomically. Applied only at authenticated start; physical effects unproven.":
            @"Save failed or uncertain. Draft retained; no setting published, no automatic retry.";
        if(_owner->ownership()==SettingsOwnership::Disabled)[_controller setHostBusy:YES];
        return acknowledged;
    }
    if(action==PortLauncherImport) { // (2026-11-05: the on-device disc import; the launcher waits while its page is up)
        PortDiscImportController* page=[PortDiscImportController new];__weak PortLauncherController* c=_controller;
        page.onClose=^(BOOL complete){PortLauncherController* v=c;if(!v)return;[v setHostBusy:NO];
            v.statusText=complete?@"The game's files from both discs are in place.":@"Disc import not complete; choose Import Game Discs to go on.";};
        [_controller setHostBusy:YES];[_controller presentViewController:page animated:YES completion:nil];
        return YES;
    }
    const bool play=action==PortLauncherPlay,verify=action==PortLauncherVerify;
    if(!play && !verify)return NO;
    if(!(play?_owner->beginPreparation():_owner->beginVerification()))return NO;
    [_controller setHostBusy:YES];
    const BOOL admitted=play?[self.host launcherRequestsPlay:self]:[self.host launcherRequestsVerification:self];
    if(!admitted) {_owner->returnToMenu();[_controller setHostBusy:NO];}
    return admitted;
}
- (void)launcher:(PortLauncherController*)view changed:(PortLauncherSetting)setting value:(NSInteger)value {
    const auto field=NSInteger(setting);
#if defined(RECOMP_DUO_EDITION)
    if(field==NSInteger(PortLauncherDuoScreens) && view==_controller && (value==0 || value==1)){_duoDraft=value;return;} // ios-duo: a draft
#endif
    if(!NSThread.isMainThread || view!=_controller || field<0 || field>=NSInteger(ChoiceCounts.size()) || value<0 || value>=ChoiceCounts[unsigned(field)] ||
       !_owner->change(unsigned(field),unsigned(value)))
        [self invalidateWithStatus:@"Rejected unsupported or non-owned draft. Restart required; no data changed."];
}
- (void)verificationFinished {
    if(!NSThread.isMainThread || _owner->ownership()!=SettingsOwnership::Verifying)return;
    _owner->returnToMenu();[_controller setHostBusy:NO];
}
- (BOOL)commitAuthenticatedGuest:(BOOL)diagnostic {
    if(!NSThread.isMainThread || !_owner->commitGuest(diagnostic)) {
        [self invalidateWithStatus:@"Preparation handoff refused. Settings changed or unavailable; no game started."];return NO;
    }
    [_controller closeForGuest];return YES;
}
- (void)invalidateWithStatus:(NSString*)reason {
    if(!NSThread.isMainThread)return;
    _owner->invalidate();_controller.statusText=reason;[_controller setHostBusy:YES];
}
@end
