// SPDX-License-Identifier: GPL-3.0-or-later
#import "LauncherController.h"
#import <QuartzCore/QuartzCore.h>
#include "LauncherNavigation.hpp"
#define PORT_MENU_SCENE_VIEW_INTERFACE_ONLY
#include "ios_menu_paint.h"
#include <array>
// menu-redesign (2026-10-25; owner, via main: "The direction looks right; use the designer's recommended picks for the three choices and
// implement it with Nunito Sans."): the launcher drawn as the game's pause-menu frame (ios_menu_scenes.h, the Android launcher's own
// MenuScenes): the header plate, the tabs PLAY / SETTINGS / CONTROLLER / ABOUT, the column, the pane, the prompts, the status strip at
// the bottom left; the unsaved-changes question as the game's alert. The logic is unchanged (LauncherNavigation, the draft, Save /
// Restore Original / Discard, the host's actions); only the drawing and the taps' routing (the drawn frame's hit rectangles) are new.
using namespace recomp::apple::launcher;
namespace mu=recomp::apple::menu;namespace ms=recomp::apple::menu::scenes;namespace ml=recomp::apple::menu::layout;
@implementation PortLauncherRow
@end

@interface PortLauncherController () {
    Navigation _navigation;
    NSArray<PortLauncherRow*>* _rows;
    NSArray<NSString*>* _about;
    PortMenuSceneView* _canvas;
    mu::Scene _scene;
    NSInteger _rowFocused;
#if defined(RECOMP_DUO_EDITION)
    std::array<NSInteger,PortLauncherRowCount> _loaded; // (ios-duo: + the Screens row)
#else
    std::array<NSInteger,10> _loaded;
#endif
    BOOL _dirty,_closed,_busy,_leaving;
    NSUInteger _inputEpoch;
    NSInteger _leaveChoice;
}
@end
@implementation PortLauncherController
- (BOOL)inputClosed {return _closed;}
- (BOOL)hostBusy {return _busy;}
- (NSUInteger)inputEpoch {return _inputEpoch;}
- (instancetype)initWithRows:(NSArray<PortLauncherRow*>*)rows about:(NSArray<NSString*>*)sections {
    if((self=[super initWithNibName:nil bundle:nil])) {
#if defined(RECOMP_DUO_EDITION)
        if(rows.count!=PortLauncherRowCount || sections.count!=4)[NSException raise:NSInvalidArgumentException format:@"complete launcher snapshot required"];
        NSMutableArray* owned=[NSMutableArray new];std::array<bool,PortLauncherRowCount> seen{};
        constexpr std::array<NSUInteger,PortLauncherRowCount> counts{6,4,41,7,2,2,4,4,2,2,2}; // (ios-duo: Screens = Duo / Full screen)
#else
        if(rows.count!=10 || sections.count!=4)[NSException raise:NSInvalidArgumentException format:@"complete launcher snapshot required"];
        NSMutableArray* owned=[NSMutableArray new];std::array<bool,10> seen{};
        constexpr std::array<NSUInteger,10> counts{6,4,41,7,2,2,4,4,2,2};
#endif
        for(PortLauncherRow* source in rows) {
            const auto key=NSInteger(source.setting);
#if defined(RECOMP_DUO_EDITION)
            if(key<0 || key>=NSInteger(seen.size()) || seen[key] || !source.title.length || !source.choices.count ||
#else
            if(key<0 || key>=10 || seen[key] || !source.title.length || !source.choices.count ||
#endif
               source.value<0 || source.value>=NSInteger(source.choices.count) ||
               source.originalValue<0 || source.originalValue>=NSInteger(source.choices.count))
                [NSException raise:NSInvalidArgumentException format:@"invalid launcher setting snapshot"];
            if(source.choices.count!=counts[key] || !source.allowedValues ||
               (source.allowedValues.count && source.allowedValues.lastIndex>=source.choices.count) ||
               (source.editable && !source.allowedValues.count))
                [NSException raise:NSInvalidArgumentException format:@"explicit setting capability choices required"];
            NSMutableArray* choices=[NSMutableArray new];
            for(id choice in source.choices) {
                if(![choice isKindOfClass:NSString.class] || ![(NSString*)choice length])
                    [NSException raise:NSInvalidArgumentException format:@"nonempty setting labels required"];
                [choices addObject:[choice copy]];
            }
            seen[key]=true;PortLauncherRow* row=[PortLauncherRow new];row.setting=source.setting;
            row.title=source.title;row.choices=choices;row.value=source.value;row.originalValue=source.originalValue;
            row.editable=source.editable;row.allowedValues=source.allowedValues;row.help=source.help?:@"";_loaded[key]=source.value;
            [owned addObject:row];
        }
        NSMutableArray* about=[NSMutableArray new];
        for(id section in sections) {
            if(![section isKindOfClass:NSString.class] || ![(NSString*)section length])
                [NSException raise:NSInvalidArgumentException format:@"nonempty About sections required"];
            [about addObject:[section copy]];
        }
#if defined(RECOMP_DUO_EDITION)
        _rows=[owned copy];_about=[about copy];_statusText=@"FivePocket Duo · 1.1.0-alpha.1";_controllerText=@"No controller state supplied.";
#else
        _rows=[owned copy];_about=[about copy];_statusText=@"FivePocket iOS · 1.1.0-alpha.1";_controllerText=@"No controller state supplied.";
#endif
        _navigation.setColumnCount(Page::Play,3);_navigation.setColumnCount(Page::Settings,3);
        _navigation.setColumnCount(Page::Controller,1);_navigation.setColumnCount(Page::About,4);
    }
    return self;
}
- (NSInteger)currentPage {return NSInteger(_navigation.page());}
- (BOOL)dirty {return _dirty;}
- (void)viewDidLoad {
    [super viewDidLoad];self.view.backgroundColor=[UIColor colorWithRed:0x14/255.0 green:0x16/255.0 blue:0x14/255.0 alpha:1];
    _canvas=[[PortMenuSceneView alloc] initWithFrame:self.view.bounds];_canvas.liveBlur=NO; // (the backdrop: the design's colour run)
    _canvas.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    __weak PortLauncherController* weak=self;
    _canvas.scene=[weak]() -> const mu::Scene* {PortLauncherController* s=weak;return s?&s->_scene:nullptr;};
    _canvas.accessibilityIdentifier=@"launcher.frame";
    [self.view addSubview:_canvas];
    [self.view addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];
    for(NSNotificationName n in @[GCControllerDidConnectNotification,GCControllerDidDisconnectNotification]) // (the icons or the strip)
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(redraw) name:n object:nil];
    [self redraw];
}
// A new view size (rotation, the first layout): the frame again (MenuLayout.Frame.screen on a view taller than 16:9; LauncherScreen.onResized).
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const int h=ml::panelHeight(float(_canvas.bounds.size.width),float(_canvas.bounds.size.height),
                                float(std::min(_canvas.bounds.size.width/ml::W,_canvas.bounds.size.height/ml::H)));
    if(h!=_scene.panelH)[self redraw];
}
- (void)setStatusText:(NSString*)value {_statusText=[value copy];if(self.isViewLoaded)[self redraw];}
- (void)setControllerText:(NSString*)value {
    if(!NSThread.isMainThread || _closed || [_controllerText isEqualToString:value])return;
    _controllerText=[value copy];
    if(self.isViewLoaded && self.currentPage==2)[self redraw]; // (one frame drawn; no views rebuilt)
}
- (NSArray<PortLauncherRow*>*)categoryRows {
    NSMutableArray* values=[NSMutableArray new];const auto category=_navigation.column(Page::Settings);
    for(PortLauncherRow* row in _rows) {
        const BOOL display=row.setting==PortLauncherScaler || row.setting==PortLauncherSharpness || row.setting==PortLauncherResolution ||
#if defined(RECOMP_DUO_EDITION)
            row.setting==PortLauncherDrawDistance || row.setting==PortLauncherTrafficDensity || row.setting==PortLauncherShadowDistance || row.setting==PortLauncherScreen ||
            row.setting==PortLauncherDuoScreens; // (ios-duo: Screens under Display too)
#else
            row.setting==PortLauncherDrawDistance || row.setting==PortLauncherTrafficDensity || row.setting==PortLauncherShadowDistance || row.setting==PortLauncherScreen; // (Claude, 2026-10-20: under Display)
#endif
        if((category==0 && display) || (category==1 && !display))[values addObject:row];
    }
    return values;
}
static std::string str(NSString* s) {return std::string(s.UTF8String?:"");}
// What the frame shows now (ms::Page): the launcher's own state, as the Android launcher's ShellPages / SettingsPages describe theirs.
- (ms::Page)page {
    ms::Page p;
#if defined(RECOMP_DUO_EDITION)
    p.title="FivePocket"; // ios-duo (main 2026-10-25: the header "FivePocket"; the home-screen name "FivePocket Duo"; the edition in the status lines, as "THOR EDITION")
    p.status={{"FivePocket Duo",ms::TONE_NORMAL},{"1.1.0-alpha.1",ms::TONE_NORMAL}}; // (2026-11-05, main: a public app never shows "Private Build" / "v507 Apple Port")
#else
    p.title="FivePocket"; // (the header; the home-screen name is "FivePocket iOS") (owner 2026-10-25: the app's name; no GTA words in names or titles)
    p.status={{"FivePocket iOS",ms::TONE_NORMAL},{"1.1.0-alpha.1",ms::TONE_NORMAL}}; // (2026-11-05, main: a public app never shows "Private Build" / "v507 Apple Port")
#endif
    p.tabs={"Play","Settings","Controller","About"};p.tab=int(self.currentPage);
    const auto page=Page(self.currentPage);const int col=_navigation.column(page);
    p.hasColumn=true;p.columnSelected=col;p.columnLit=_navigation.level()!=Level::Strip;
    p.column=self.currentPage==0?std::vector<std::string>{"Play","Verify Game Data","Import Game Discs"}:
             self.currentPage==1?std::vector<std::string>{"Display","Frame Rate","Save And Restore"}:
             self.currentPage==2?std::vector<std::string>{"Controller Check"}:std::vector<std::string>{"About","Controls","Known Issues","Build"};
    if(_dirty && self.currentPage==1){p.columnMarks={0,0,ms::MARK_CHANGED};}
    const bool pane=_navigation.level()==Level::Pane;
    if(self.currentPage==0)p.info.push_back({col==2?"Copies the game's files from your own disc images (Disc 1 and Disc 2) into this app. The images are read only.":
        "Play prepares a fresh authenticated request. A failed attempt is retained; no automatic retry.",ms::TONE_NORMAL});
    else if(self.currentPage==2){p.diagram=true;p.info.push_back({str(self.controllerText?:@""),ms::TONE_NORMAL});}
    else if(self.currentPage==3) {
        p.hasText=true;
        p.text=ms::paneText(str(_about[std::size_t(col)]),true,recomp::apple::menu::paint::fontMeasure());
    } else if(col==2) {
        for(const char* t:{"Save Changes","Restore Original","Discard Changes"}){ms::Row r;r.label=t;r.enabled=!_closed && !_busy;p.rows.push_back(r);}
        if(pane)p.rowFocused=int(_rowFocused);
    } else {
        NSArray<PortLauncherRow*>* rows=[self categoryRows];
        for(PortLauncherRow* row in rows) {
            ms::Row r;r.label=str(row.title);r.value=str(row.choices[std::size_t(row.value)]);r.hasValue=true;r.enabled=row.editable;
            r.arrows=r.halves=row.editable;
            r.canLeft=[row.allowedValues indexLessThanIndex:row.value]!=NSNotFound;r.canRight=[row.allowedValues indexGreaterThanIndex:row.value]!=NSNotFound;
            r.marked=row.value!=_loaded[row.setting];
            p.rows.push_back(r);
        }
        if(pane && _rowFocused>=0 && _rowFocused<NSInteger(rows.count)) {
            p.rowFocused=int(_rowFocused);
            p.help.push_back({str(rows[_rowFocused].help),rows[_rowFocused].editable?ms::TONE_NORMAL:ms::TONE_WARNING});
        }
    }
    if(_navigation.level()!=Level::Strip)p.prompts.push_back({"Back","B",int(Command::Back)});
    if(_dirty)p.prompts.push_back({"Save","START",int(Command::Menu)});
    p.prompts.push_back({"Select","A",int(Command::Confirm)});
    p.touchIcons=!recomp::apple::menu::paint::physicalGamepad(); // (no gamepad: the X and check icons; one connected: the strip)
    if(self.statusText.length){p.hasFooter=true;p.footerStatus={str(_closed?@"Guest owns input; launcher closed.":self.statusText),ms::TONE_NORMAL};}
    if(_leaving) {
        p.hasAlert=true;p.alert.heading="alert";p.alert.title="Unsaved Changes";p.alert.detail="Save, discard, or stay on Settings?";
        p.alert.buttons={"Save","Discard","Stay"};p.alert.focused=int(_leaveChoice);
        p.prompts={{"Stay","B",int(Command::Back)},{"Select","A",int(Command::Confirm)}};
    }
    return p;
}
- (void)redraw {
    if(!self.isViewLoaded)return;
    const CGFloat w=_canvas.bounds.size.width,h=_canvas.bounds.size.height,u=std::min(w/ml::W,h/ml::H);
    _scene=ms::build([self page],recomp::apple::menu::paint::fontMeasure(),ml::Frame::screen(ml::panelHeight(float(w),float(h),float(u)))); // (full screen on a taller view)
    [_canvas setNeedsDisplay];
}
// A tap: what the drawn frame has under the finger (its hit rectangles, in panel pixels).
- (void)tapped:(UITapGestureRecognizer*)tap {
    [self noticeManualInput];
    if(![self canAct])return;
    const CGPoint p=[tap locationInView:_canvas];const CGFloat w=_canvas.bounds.size.width,h=_canvas.bounds.size.height;
    const CGFloat u=std::min(w/ml::W,h/ml::H);
    const int id=_scene.hitAt(float((p.x-(w-_scene.panelW*u)/2)/u),float((p.y-(h-_scene.panelH*u)/2)/u));
    if(id<0)return;
    if(_leaving) {
        if(id>=ml::HIT_ALERT_BUTTON && id<ml::HIT_TEXT){const int i=id-ml::HIT_ALERT_BUTTON;if(i==2)[self cancelLeave];else [self resolveLeave:i?PortLauncherDiscard:PortLauncherSave];}
        else if(id>=ml::HIT_PROMPT && id<ml::HIT_ALERT_BUTTON)[self handleCommand:id-ml::HIT_PROMPT repeat:NO];
        return;
    }
    if(id>=ml::HIT_PROMPT && id<ml::HIT_ALERT_BUTTON){[self handleCommand:id-ml::HIT_PROMPT repeat:NO];return;}
    ++_inputEpoch;
    if(id>=ml::HIT_TAB && id<ml::HIT_COLUMN) {
        const auto answer=_navigation.tapTab(Page(id-ml::HIT_TAB),_dirty);
        if(answer==Answer::Ask)[self askLeave];else [self redraw];
        return;
    }
    if(id>=ml::HIT_COLUMN && id<ml::HIT_ROW) {
        const auto answer=_navigation.tapColumn(id-ml::HIT_COLUMN,self.currentPage!=0);
        if(answer==Answer::Entered)_rowFocused=0;
        if(answer==Answer::Activate)[self activateColumn];
        [self redraw];return;
    }
    if(self.currentPage!=1)return;
    const bool left=id>=ml::HIT_ARROW_LEFT && id<ml::HIT_ARROW_RIGHT,right=id>=ml::HIT_ARROW_RIGHT && id<ml::HIT_PROMPT;
    const NSInteger i=left?id-ml::HIT_ARROW_LEFT:right?id-ml::HIT_ARROW_RIGHT:id-ml::HIT_ROW;
    if(id<ml::HIT_ROW || i<0)return;
    const BOOL again=_navigation.level()==Level::Pane && _rowFocused==i;
    _rowFocused=i;_navigation.intoPane(Page::Settings,_navigation.column(Page::Settings));
    if(_navigation.column(Page::Settings)==2){[self settingAction:i==0?PortLauncherSave:i==1?PortLauncherRestoreOriginal:PortLauncherDiscard];return;}
    if(left)[self step:-1];else if(right || again)[self step:1];else [self redraw]; // (a first tap highlights; the halves step)
}
- (BOOL)canAct {return NSThread.isMainThread && !_closed && !_busy;}
- (void)noticeManualInput {
    if(!NSThread.isMainThread || _closed)return;
    // Notify before rechecking action admission: takeover remains meaningful
    // while preparation is busy, and the callback may close/change the view.
    auto handler=self.manualInputHandler;if(handler)handler();
}
- (void)activateColumn {
    if(![self canAct])return;
    if(self.currentPage==0) {
        const auto col=_navigation.column(Page::Play);
        const auto action=col==0?PortLauncherPlay:col==1?PortLauncherVerify:PortLauncherImport;
        if(![self.delegate launcher:self action:action])self.statusText=@"Host rejected request; no automatic retry.";
    }
}
- (void)changedRow:(PortLauncherRow*)row value:(NSInteger)value {
    if(row.value==value)return;
    row.value=value;[self.delegate launcher:self changed:row.setting value:value];
    _dirty=NO;for(PortLauncherRow* candidate in _rows)if(candidate.value!=_loaded[candidate.setting])_dirty=YES;
}
- (void)step:(NSInteger)direction {
    const auto rows=[self categoryRows];
    if(_rowFocused<0 || _rowFocused>=NSInteger(rows.count))return;
    PortLauncherRow* row=rows[_rowFocused];
    if(!row.editable){[self redraw];return;}
    const auto value=direction>0?[row.allowedValues indexGreaterThanIndex:row.value]:[row.allowedValues indexLessThanIndex:row.value];
    if(value!=NSNotFound)[self changedRow:row value:NSInteger(value)];[self redraw];
}
- (BOOL)settingAction:(PortLauncherAction)action {
    if(![self canAct])return NO;
    if(action==PortLauncherRestoreOriginal) {
        for(PortLauncherRow* row in _rows)if(row.editable && [row.allowedValues containsIndex:row.originalValue])
            [self changedRow:row value:row.originalValue];
        [self redraw];return YES; // draft only, never an implicit Save
    }
    if(action!=PortLauncherSave && action!=PortLauncherDiscard)return NO;
    if(action==PortLauncherSave && !_dirty)return YES;
    if(![self.delegate launcher:self action:action]) {self.statusText=@"Host operation failed; unsaved choices retained.";return NO;}
    if(action==PortLauncherSave) {for(PortLauncherRow* row in _rows)_loaded[row.setting]=row.value;}
    else {for(PortLauncherRow* row in _rows)[self changedRow:row value:_loaded[row.setting]];}
    _dirty=NO;[self redraw];return YES;
}
// The unsaved-changes question, drawn as the game's alert (Save / Discard / Stay; Stay highlighted first).
- (void)askLeave {
    if(_leaving || ![self canAct])return;
    ++_inputEpoch;
    _leaveChoice=2;_leaving=YES;[self redraw];
}
- (BOOL)resolveLeave:(PortLauncherAction)action {
    if(![self canAct] || !_navigation.asking() || (action!=PortLauncherSave && action!=PortLauncherDiscard))return NO;
    ++_inputEpoch;
    const BOOL success=[self settingAction:action];
    if(success)_navigation.leaveGranted();else _navigation.leaveCancelled();
    _leaving=NO;[self redraw];return success;
}
- (void)cancelLeave {
    if(!NSThread.isMainThread)return;
    ++_inputEpoch;
    _navigation.leaveCancelled();_leaving=NO;[self redraw];
}
- (void)handleCommand:(NSInteger)value repeat:(BOOL)repeat {
    if(![self canAct] || value<0 || value>10)return;
    const auto command=Command(value);
    if(_navigation.asking()) {
        if(command==Command::Back) {[self cancelLeave];return;}
        if(command==Command::Up || command==Command::Left)_leaveChoice=std::max<NSInteger>(0,_leaveChoice-1);
        if(command==Command::Down || command==Command::Right)_leaveChoice=std::min<NSInteger>(2,_leaveChoice+1);
        if(command==Command::Confirm && !repeat) {
            if(_leaveChoice==2)[self cancelLeave];else [self resolveLeave:_leaveChoice?PortLauncherDiscard:PortLauncherSave];
            return;
        }
        [self redraw];return;
    }
    if(self.currentPage==1 && command==Command::Menu && !repeat) {[self settingAction:PortLauncherSave];return;}
    const auto answer=_navigation.command(command,repeat,self.currentPage!=0,_dirty);
    if(Navigation::endsHold(answer,command))++_inputEpoch;
    if(answer==Answer::Ask) {[self askLeave];return;}
    if(answer==Answer::Entered)_rowFocused=0;
    if(answer==Answer::Activate && !repeat)[self activateColumn];
    if(answer==Answer::PaneCommand && self.currentPage==1) {
        const NSInteger count=_navigation.column(Page::Settings)==2?3:NSInteger([self categoryRows].count);
        if(command==Command::Up)_rowFocused=std::max<NSInteger>(0,_rowFocused-1);
        if(command==Command::Down)_rowFocused=std::min(count-1,_rowFocused+1);
        if(_navigation.column(Page::Settings)==2) {
            if(command==Command::Confirm && !repeat)[self settingAction:_rowFocused==0?PortLauncherSave:_rowFocused==1?PortLauncherRestoreOriginal:PortLauncherDiscard];
        } else {
            if(command==Command::Left)[self step:-1];if(command==Command::Right)[self step:1];
            if(command==Command::Confirm && !repeat) {
                const auto rows=[self categoryRows];if(_rowFocused<NSInteger(rows.count) && (rows[_rowFocused].setting==PortLauncherFpsCounter || rows[_rowFocused].setting==PortLauncherFrameGeneration))
                    [self step:rows[_rowFocused].value?-1:1];
            }
        }
    }
    [self redraw];
}
- (void)setHostBusy:(BOOL)busy {if(!NSThread.isMainThread || _closed)return;if(_busy!=busy)++_inputEpoch;_busy=busy;[self redraw];}
- (NSString*)stateLine {
    NSMutableString* line=[NSMutableString stringWithFormat:@"page=%ld level=%d column=%d row=%ld",long(self.currentPage),int(_navigation.level()),
        _navigation.column(Page(self.currentPage)),long(_rowFocused)];
    for(PortLauncherRow* row in _rows)[line appendFormat:@" v%ld=%ld",long(row.setting),long(row.value)];
    [line appendFormat:@" dirty=%d asking=%d busy=%d closed=%d status=\"%@\"",int(_dirty),int(_navigation.asking()),int(_busy),int(_closed),self.statusText?:@""];
    return line;
}
- (void)closeForGuest {
    if(!NSThread.isMainThread || _closed)return;
    [self cancelLeave];_closed=YES;[self redraw];
}
@end
