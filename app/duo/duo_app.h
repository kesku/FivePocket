// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-duo (2026-10-24): the iOS Duo edition's app side (Main.mm only, RECOMP_DUO_EDITION). The Thor APK tree's second-screen app
// code mapped onto one iOS window in portrait:
//   SecondScreen.java (a Presentation on the lower display) -> the presenter's lower view (ios_frame_presenter.mm duoLayout) handed
//                     to the library through recomp_set_hud_window_v1 (registerLowerScreen below; acquire = the layer while split)
//   WheelTouch.java   (the lower screen's touch listener)  -> DuoTouchView kind Lower: WheelGesture (duo_gesture.h)
//   WheelTouch.MainTouch (the main screen's touch)          -> DuoTouchView kind Top:   MapTouch, in the game frame's 1920x1080 space
//   PrivateGamepadInput.publish's merge                     -> mergePad(), called from Session::snapshot for user 0
//   second-screen.txt words wheel=main / radio=main / character=main (SettingsSchema rows) -> the same words read here (mask)
// plus the Duo's own: settings/duo-screen.txt ("full" = fullscreen landscape, else / absent = the portrait split), the toggle
// button on the lower screen, and a two-finger double tap in fullscreen to return to the split.
// Log lines APPLE_IOS_DUO_*: touch is labelled "touch input" (never physical / synthetic).
#include "duo_state.h"
#include "duo_gesture.h"
#include "duo_input.h"
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>

struct ANativeWindow;
struct RecompHudWindowV1 {
    std::uint32_t structureBytes,version,reserved0,reserved1;
    void* windowUser;
    ANativeWindow* (*acquireWindow)(void*,std::uint64_t*) noexcept;
    void (*releaseWindow)(void*,ANativeWindow*) noexcept;
    std::uint64_t (*windowGeneration)(void*) noexcept;
}; // (= tools/host_guest_trace/private_trace_hud_api.h; the library checks structureBytes)
extern "C" int recomp_set_hud_window_v1(const RecompHudWindowV1* window) noexcept;
extern "C" void recomp_duo_set_lower_screen_v1(int on) noexcept;
extern "C" int recomp_duo_lower_map_rect_v1(int* r) noexcept;
extern "C" __attribute__((visibility("default"))) int recomp_lower_buttons_v1(); // (v832: bit 0 = the Quick Save button shown)

namespace recomp::duo::app {
namespace ds=recomp::duo::state;
inline long long nowMs() noexcept {return (long long)std::llround(CACurrentMediaTime()*1000.0);}

// ---- settings
inline std::string& root() {static std::string r;return r;}
inline std::string settingsPath(const char* name) {return root().empty()?std::string():root()+"/settings/"+name;}
inline bool readWords(const char* name,std::string& text) {
    text.clear();const auto p=settingsPath(name);if(p.empty())return false;
    FILE* f=std::fopen(p.c_str(),"r");if(!f)return false;
    char b[129]{};const auto n=std::fread(b,1,128,f);std::fclose(f);text.assign(b,n);return true;
}
inline bool hasWord(const std::string& text,const char* word) {
    std::size_t i=0;const std::size_t w=std::strlen(word);
    while(i<text.size()) {
        while(i<text.size() && std::isspace((unsigned char)text[i]))++i;
        const std::size_t s=i;while(i<text.size() && !std::isspace((unsigned char)text[i]) && text[i]!=0)++i;
        if(i-s==w && !text.compare(s,w,word))return true;
        if(i<text.size() && text[i]==0)++i;
    }
    return false;
}

// ---- the touch log (2026-10-24, owner: "duo needs to fix its touch controls as they dont work"; no console in his sessions):
// files/private-trace/duo-touch.log, appended with a millisecond stamp per line, at most 512 KiB per launch (then silent), opened
// on the first line. Lines: every on-screen control's down / up (FullTouchControls.hpp), every Duo touch view's touches and gesture
// state, every change of what mergePad gives the game. Any thread (its own mutex). Labelled touch input; never physical proof.
inline std::mutex& touchLogMutex() {static auto* m=new std::mutex;return *m;}
inline void touchLog(const char* line) noexcept {
    std::lock_guard l(touchLogMutex());
    static FILE* f=nullptr;static bool tried=false;static std::size_t bytes=0;
    if(!tried && !root().empty()){tried=true;f=std::fopen((root()+"/private-trace/duo-touch.log").c_str(),"a");
        if(f)std::fprintf(f,"---- launch split=%d\n",ds::split.load());}
    if(!f || bytes>512*1024)return;
    bytes+=std::size_t(std::fprintf(f,"%lld %s\n",nowMs(),line));std::fflush(f);
}
// ---- input (main thread: touches; guest threads: mergePad; one mutex)
// (struct Input and the merge itself: duo_input.h, host-tested)
inline Input& input() {static auto* i=new Input;return *i;}
inline void readMask() {
    std::string t;const bool ok=readWords("second-screen.txt",t);
    auto& in=input();std::lock_guard l(in.m);
    in.mask=WheelGesture::mask(ok && hasWord(t,"wheel=main"),ok && hasWord(t,"radio=main"),ok && hasWord(t,"character=main"));
    in.lower.nativeMap(true); // (the Duo library always runs the map macro: map_tap.h is in every Duo build)
    in.lower.phoneTouch(true);in.lower.saveTouch(true);in.lower.phoneTapAt(true); // (v815 / v820 / v828: WheelTouch's markers; the Duo links v850's map_tap.h, phone_touch.h, quick_save.h)
    std::fprintf(stderr,"APPLE_IOS_DUO_TOUCH mask=%x map_tap=library\n",unsigned(in.mask));
}
inline void cancelAll() {auto& in=input();std::lock_guard l(in.m);in.lower.cancel();in.top.cancel();}
inline void logStates(Input& in) {
    {char l[160];std::snprintf(l,sizeof l,"gesture lower=%d buttons=%04x stick=%d,%d top=%d buttons=%04x rs=%d,%d",int(in.lower.state()),
        unsigned(in.lower.buttons()),in.lower.rightX(),in.lower.rightY(),int(in.top.state()),unsigned(in.top.buttons()),in.top.rightX(),in.top.rightY());
     static std::string last;if(last!=l){last=l;touchLog(l);}}
    if(in.lower.state()!=in.lastLowerState){in.lastLowerState=in.lower.state();
        std::fprintf(stderr,"APPLE_IOS_DUO_WHEEL_TOUCH state=%d buttons=%04x stick=%d,%d (touch input)\n",int(in.lower.state()),unsigned(in.lower.buttons()),in.lower.rightX(),in.lower.rightY());}
    if(in.top.state()!=in.lastTopState){in.lastTopState=in.top.state();
        std::fprintf(stderr,"APPLE_IOS_DUO_MAP_TOUCH state=%d rx=%d ry=%d (touch input)\n",int(in.top.state()),in.top.rightX(),in.top.rightY());}
}
// The pad user 0 is given (any guest thread): the physical / touch-control state with the gestures merged as WheelTouch.merge does
// (buttons OR'd, the right stick replaced only while a gesture pushes). A changed result gets a packet number of its own.
template<class State> inline void mergePad(State& st) noexcept {
    if(!ds::split.load(std::memory_order_relaxed))return;
    auto& in=input();std::lock_guard l(in.m);
    Pad p{},o{};mergeCore(in,st,nowMs(),&p,&o);
    logStates(in);
    { // (the touch log: what the game is given, on every change of the input or the result)
        char l[200];std::snprintf(l,sizeof l,"pad in=%04x ls=%d,%d rs=%d,%d t=%u,%u out=%04x rs=%d,%d ls=%d,%d t=%d,%d",unsigned(p.buttons),p.leftX,p.leftY,
            p.rightX,p.rightY,unsigned(p.leftTrigger),unsigned(p.rightTrigger),unsigned(o.buttons),o.rightX,o.rightY,o.leftX,o.leftY,o.leftTrigger,o.rightTrigger);
        static std::string last;if(last!=l){last=l;touchLog(l);}
    }
}

// ---- the library's lower-screen window (recomp_set_hud_window_v1 before the V6 entry)
inline ANativeWindow* acquireLower(void*,std::uint64_t* g) noexcept {
    if(g)*g=ds::lowerGeneration.load(std::memory_order_acquire);
    return static_cast<ANativeWindow*>(ds::lowerLayer.load(std::memory_order_acquire));
}
inline void releaseLower(void*,ANativeWindow*) noexcept {} // (the presenter keeps its one retained reference for the process)
inline std::uint64_t lowerGeneration(void*) noexcept {return ds::lowerGeneration.load(std::memory_order_acquire);}
inline void registerLowerScreen() {
    static const RecompHudWindowV1 w{sizeof(RecompHudWindowV1),1,0,0,nullptr,acquireLower,releaseLower,lowerGeneration};
    const int r=recomp_set_hud_window_v1(&w);
    recomp_duo_set_lower_screen_v1(ds::split.load(std::memory_order_relaxed));
    std::fprintf(stderr,"APPLE_IOS_DUO_HUD_WINDOW result=%d split=%d\n",r,ds::split.load(std::memory_order_relaxed));
}

// ---- the mode
// (run claude-duo-03: forcing landscape with requestGeometryUpdate on a portrait-held iPad under iPadOS 26 turned the app into a
// landscape-shaped window over the home screen, which stayed after the switch back.) Both modes allow every orientation and nothing
// is requested: the split needs a portrait view, full screen fits the frame in portrait and is the regular layout in landscape.
inline UIInterfaceOrientationMask orientations() noexcept {return UIInterfaceOrientationMaskPortrait|UIInterfaceOrientationMaskLandscape;}
inline void loadMode(const std::string& dataRoot) {
    root()=dataRoot;
    std::string t;const bool full=readWords("duo-screen.txt",t) && hasWord(t,"full");
    ds::split.store(full?0:1,std::memory_order_relaxed);
    std::fprintf(stderr,"APPLE_IOS_DUO_MODE split=%d source=%s\n",full?0:1,t.empty()?"default":"settings");
    std::string top;readWords("duo-top-screen.txt",top); // (2026-11-05: Fit / Zoom / Stretch / 4:3; no file = Fit)
    ds::topMode.store(ds::topModeOf(top),std::memory_order_relaxed);
    const char* envMode=std::getenv("RECOMP_DUO_TOP_MODE"); // (a test launch's mode for this run only: never written to the settings file)
    if(envMode && *envMode)ds::topMode.store(ds::topModeOf(envMode),std::memory_order_relaxed);
    // (28s) the 4:3 layout for this process: the mode 4:3, or RECOMP_DUO_4X3=1 (a test launch). The game's video mode query
    // (guest_import_registry.cpp) reads RECOMP_DUO_4X3 when the game starts, after this scene setup: set here for it.
    {const char* e=std::getenv("RECOMP_DUO_4X3");
     const bool on=ds::launch4x3(e,ds::topMode.load(std::memory_order_relaxed));
     if(on && !(e && e[0]=='1'))::setenv("RECOMP_DUO_4X3","1",1);
     ds::frame4x3.store(on?1:0,std::memory_order_relaxed);}
    std::fprintf(stderr,"APPLE_IOS_DUO_TOP_SCREEN mode=%s frame=%s source=%s\n",ds::TopModeNames[ds::topMode.load()],ds::frame4x3.load()?"4:3":"16:9",
        envMode && *envMode?"env (this run only)":top.empty()?"default":"settings");
    readMask();
}
inline void requestOrientation(UIViewController* rootController) { // (no geometry request: see orientations())
    if(rootController)[rootController setNeedsUpdateOfSupportedInterfaceOrientations];
}
}

extern "C" void recomp_duo_touch_log(const char* line) noexcept {recomp::duo::app::touchLog(line);} // (FullTouchControls.hpp)

// ---- the lower screen's trainer button (TrainerButtonView.java, thor-radio v832; the Thor mirror 2026-10-25)
// Beside the library's Quick Save (WheelGesture::trainerBox, lower-screen pixels), shown while the Trainer switch is on, the side menu is
// closed and the library shows its button row (recomp_lower_buttons_v1 bit 0: the HUD map, no wheel / phone / pause screen / Loading).
// Its icon: the bundle's trainer-icon.png (the owner's own picture, owner builds only: duo_build.py --no-game-art leaves it out), else
// the dotted skull drawn here. A tap on it (DuoTouchView) opens the trainer; the view takes no touches itself. The open sound: the
// bundle's trainer-open.m4a (the owner's, owner builds only; absent = silent).
@interface DuoTrainerButton : UIView
@property(nonatomic,weak) PortQolTouchView* qol;
- (BOOL)hitPx:(CGPoint)p size:(CGSize)sz;
- (void)playOpen;
@end
@implementation DuoTrainerButton {
    UIImage* icon_;AVAudioPlayer* sound_;BOOL shown_;NSTimer* poll_;
}
static const char* const DuoTrainerDots[]={"...#######...","..#########..",".###########.",".##...#...##.",".##...#...##.",".###########.","..####.####..",
    "...#######...","...#.#.#.#...","#...........#",".##.......##.","...##...##...",".##.......##.","#...........#"};
- (instancetype)initWithFrame:(CGRect)frame {
    if((self=[super initWithFrame:frame])) {
        self.userInteractionEnabled=NO;self.backgroundColor=UIColor.clearColor;self.opaque=NO;
        if(NSString* p=[NSBundle.mainBundle pathForResource:@"trainer-icon" ofType:@"png"])icon_=[UIImage imageWithContentsOfFile:p];
        if(NSString* p=[NSBundle.mainBundle pathForResource:@"trainer-open" ofType:@"m4a"]) {
            sound_=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:p] error:nil];[sound_ prepareToPlay];}
        __weak DuoTrainerButton* weak=self;
        poll_=[NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer*){[weak poll];}];
        std::fprintf(stderr,"APPLE_IOS_DUO_TRAINER_BUTTON ready icon=%s sound=%d\n",icon_?"owner":"dots",sound_?1:0);
    }
    return self;
}
- (void)poll {
    PortQolTouchView* q=_qol;
    const BOOL s=q && [q duoTrainerAllowed] && ![q duoMenuOpen] && !self.superview.hidden && (recomp_lower_buttons_v1()&1)!=0;
    if(s!=shown_){shown_=s;[self setNeedsDisplay];}
}
- (CGSize)px {const float k=recomp::duo::state::lowerScale.load();return CGSizeMake(self.bounds.size.width*k,self.bounds.size.height*k);}
- (BOOL)hitPx:(CGPoint)p size:(CGSize)sz {
    [self poll];
    if(!shown_)return NO;
    const auto b=recomp::duo::WheelGesture::trainerBox(float(sz.width),float(sz.height));if(!b.ok)return NO;
    const float pad=float(recomp::duo::jround(recomp::duo::WheelGesture::SAVE_PAD_1080*std::min(1.f,float(sz.height)/1080.f)));
    return p.x>=b.x0-pad && p.x<b.x1+pad && p.y>=b.y0-pad && p.y<b.y1+pad;
}
- (void)playOpen {if(sound_){sound_.currentTime=0;[sound_ play];}}
- (void)drawRect:(CGRect)rect {
    if(!shown_)return;
    const CGSize sz=[self px];const float k=recomp::duo::state::lowerScale.load();if(!(k>0))return;
    const auto b=recomp::duo::WheelGesture::trainerBox(float(sz.width),float(sz.height));if(!b.ok)return;
    const CGFloat s=std::min<CGFloat>(1,sz.height/1080.0);
    const CGRect r=CGRectMake(b.x0/k,b.y0/k,(b.x1-b.x0)/k,(b.y1-b.y0)/k);
    [[UIColor colorWithWhite:0 alpha:0.78] setFill];[[UIBezierPath bezierPathWithRoundedRect:r cornerRadius:10*s/k] fill]; // (quick_save.h: black 0.78, radius 10)
    const CGFloat side=72*s/k,x0=CGRectGetMidX(r)-side/2,y0=CGRectGetMidY(r)-side/2;
    if(icon_){[icon_ drawInRect:CGRectMake(x0,y0,side,side)];return;}
    const CGFloat cell=side/14,dot=cell*0.74,left=x0+(side-13*cell)/2; // (the dotted skull: public builds)
    [UIColor.whiteColor setFill];
    for(int j=0;j<14;++j)for(int i=0;DuoTrainerDots[j][i];++i)if(DuoTrainerDots[j][i]=='#')UIRectFill(CGRectMake(left+i*cell+(cell-dot)/2,y0+j*cell+(cell-dot)/2,dot,dot));
    [[UIColor colorWithRed:0xEB/255.0 green:0x1F/255.0 blue:0x1F/255.0 alpha:1] setFill];
    for(const CGFloat col:{4.5,8.5}){const CGFloat cx=left+col*cell,cy=y0+4*cell;UIRectFill(CGRectMake(cx-dot*0.6,cy-dot*0.6,dot*1.2,dot*1.2));}
}
@end

// ---- the touch views and their container
typedef NS_ENUM(NSInteger,DuoTouchKind){DuoTouchLower,DuoTouchTop};
@interface DuoTouchView : UIView {
@public DuoTouchKind kind;
}
@property(nonatomic,weak) DuoTrainerButton* trainer; // (lower: the trainer button and the left-edge swipe, SecondScreen.LowerTouch)
@end
@implementation DuoTouchView {
    UITouch* first;UITouch* second;
    recomp::duo::EdgeSwipe edge;BOOL held; // (lower: held = the touch that opened the trainer, ignored until every finger lifts)
}
- (instancetype)initWithKind:(DuoTouchKind)k {
    if((self=[super initWithFrame:CGRectZero])){kind=k;self.multipleTouchEnabled=YES;self.backgroundColor=UIColor.clearColor;}
    return self;
}
// Lower: the lower screen's pixels (as the library lays it out); top: the game frame mapped to the Thor's 1920x1080 main panel
// (MapTouch's measured pan / zoom gains are in those pixels; its tap offset maps the frame to 1280x720 guest pixels).
- (CGPoint)map:(UITouch*)t {
    namespace ds=recomp::duo::state;
    const CGPoint p=[t locationInView:self];
    if(kind==DuoTouchLower){const float s=ds::lowerScale.load();return CGPointMake(p.x*s,p.y*s);}
    const float gx=ds::gameX.load(),gy=ds::gameY.load(),gw=ds::gameW.load(),gh=ds::gameH.load();
    if(!(gw>0) || !(gh>0))return CGPointMake(-1,-1);
    const CGPoint q=[self convertPoint:p toView:self.superview];
    return CGPointMake((q.x-gx)*1920.0/gw,(q.y-gy)*1080.0/gh);
}
- (CGSize)space {
    namespace ds=recomp::duo::state;
    if(kind==DuoTouchLower){const float s=ds::lowerScale.load();return CGSizeMake(self.bounds.size.width*s,self.bounds.size.height*s);}
    return CGSizeMake(1920,1080);
}
// (SecondScreen.LowerTouch, v826..v832) The trainer open = the side menu at its Trainer tab (over the top half on the Duo). YES = opened.
- (BOOL)openTrainer:(const char*)by {
    PortQolTouchView* q=_trainer.qol;
    const BOOL ok=q && [q duoOpenTrainer];
    std::fprintf(stderr,"APPLE_IOS_DUO_LOWER_TRAINER %s opened=%d (touch input)\n",by,ok?1:0);
    {char line[96];std::snprintf(line,sizeof line,"trainer %s opened=%d",by,ok?1:0);recomp::duo::app::touchLog(line);}
    if(ok)[_trainer playOpen];
    return ok;
}
- (BOOL)lowerMenuOwns {PortQolTouchView* q=_trainer.qol;return q && [q duoMenuOpen];} // (the menu open: no lower touch reaches the game)
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if(kind==DuoTouchLower) {
        if(held || [self lowerMenuOwns]){held=YES;return;}
        const CGSize ps=[self space];edge.view(float(ps.width));const long long t0=recomp::duo::app::nowMs();
        for(UITouch* u in touches) {
            const CGPoint p=[self map:u];
            if(!first && _trainer && [_trainer hitPx:p size:ps] && [self openTrainer:"button"]){recomp::duo::app::cancelAll();first=second=nil;held=YES;return;}
            edge.down((long long)(std::uintptr_t)(__bridge void*)u,t0,float(p.x),float(p.y));
        }
    }
    auto& in=recomp::duo::app::input();std::lock_guard l(in.m);const long long t=recomp::duo::app::nowMs();const CGSize sz=[self space];
    for(UITouch* u in touches) {
        const CGPoint p=[self map:u];
        {char line[128];std::snprintf(line,sizeof line,"view %s down x=%.0f y=%.0f space=%.0fx%.0f",kind==DuoTouchLower?"lower":"top",p.x,p.y,sz.width,sz.height);
         recomp::duo::app::touchLog(line);}
        if(!first){first=u;second=nil;
            if(kind==DuoTouchLower) {
                int r[4]{};if(recomp_duo_lower_map_rect_v1(r))in.lower.tapZone(float(r[0]),float(r[1]),float(r[2]),float(r[3])); // (the map's own area)
                in.lower.down(t,p.x,p.y,sz.width,sz.height,in.lastLb);
            } else in.top.down(t,p.x,p.y,sz.width,sz.height);}
        else if(!second && u!=first){second=u;
            if(kind==DuoTouchLower)in.lower.pointerDown(t,p.x,p.y);else in.top.pointerDown(t,p.x,p.y);}
    }
    recomp::duo::app::logStates(in);
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if(kind==DuoTouchLower) {
        if(held)return;
        const long long t0=recomp::duo::app::nowMs();
        for(UITouch* u in touches) {
            const CGPoint p=[self map:u];
            if(!edge.move((long long)(std::uintptr_t)(__bridge void*)u,t0,float(p.x),float(p.y)))continue;
            bool wheel=false;{auto& in=recomp::duo::app::input();std::lock_guard l(in.m);wheel=in.lastLb;}
            PortQolTouchView* q=_trainer.qol;const bool allowed=q && [q duoTrainerAllowed] && !wheel;
            std::fprintf(stderr,"APPLE_IOS_DUO_LOWER_TRAINER edge-swipe allowed=%d (touch input)\n",allowed?1:0);
            if(allowed && [self openTrainer:"edge-swipe"]){recomp::duo::app::cancelAll();first=second=nil;held=YES;return;}
        }
    }
    auto& in=recomp::duo::app::input();std::lock_guard l(in.m);const long long t=recomp::duo::app::nowMs();const CGSize sz=[self space];
    if(!first)return;
    const CGPoint a=[self map:first];
    if(second){const CGPoint b=[self map:second];
        if(kind==DuoTouchLower)in.lower.move2(t,a.x,a.y,b.x,b.y);else in.top.move2(t,a.x,a.y,b.x,b.y);}
    else if(kind==DuoTouchLower)in.lower.move(t,a.x,a.y,sz.width,sz.height);else in.top.move(t,a.x,a.y);
    recomp::duo::app::logStates(in);
}
- (void)lift:(NSSet<UITouch*>*)touches event:(UIEvent*)event {
    if(kind==DuoTouchLower) {
        for(UITouch* u in touches)edge.up((long long)(std::uintptr_t)(__bridge void*)u);
        if(held) { // (the rest of the gesture that opened the trainer, or touches while the menu is open: every finger up = done)
            NSUInteger down=0;for(UITouch* u in [event touchesForView:self])if(u.phase!=UITouchPhaseEnded && u.phase!=UITouchPhaseCancelled)++down;
            if(!down){held=NO;edge.cancel();}
            return;
        }
    }
    auto& in=recomp::duo::app::input();std::lock_guard l(in.m);const long long t=recomp::duo::app::nowMs();
    {char line[64];std::snprintf(line,sizeof line,"view %s up touches=%lu",kind==DuoTouchLower?"lower":"top",(unsigned long)touches.count);
     recomp::duo::app::touchLog(line);}
    for(UITouch* u in touches) {
        if(u!=first && u!=second)continue;
        if(kind==DuoTouchLower)in.lower.pointerUp(t);else in.top.pointerUp(t); // (either finger of a pinch: the pinch ends; no-op otherwise)
        if(u==second)second=nil;
        if(u==first){first=nil;if(kind==DuoTouchLower)in.lower.up(t);else in.top.up(t);}
    }
    if(!first)second=nil;
    recomp::duo::app::logStates(in);
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {[self lift:touches event:event];}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {[self lift:touches event:event];}
@end

// A pass-through container over the whole game view: in the split it lays the lower touch view over the bottom half and the top one
// over the top half (and the touch-control overlay, when shown, over the top half only); in fullscreen it hides both.
@interface DuoOverlay : UIView
@property(nonatomic,strong) DuoTouchView* lower;
@property(nonatomic,strong) DuoTouchView* top;
@property(nonatomic,strong) UIButton* toggle;
@property(nonatomic,strong) DuoTrainerButton* trainer;
@property(nonatomic,weak) UIView* touchControls;
@property(nonatomic,weak) UIViewController* owner;
@property(nonatomic,strong) NSTimer* settingsWatch;
- (void)setSplit:(bool)on;
- (void)setSplit:(bool)on source:(const char*)source;
@end
@implementation DuoOverlay
- (instancetype)initWithOwner:(UIViewController*)owner {
    if((self=[super initWithFrame:CGRectZero])) {
        _owner=owner;self.backgroundColor=UIColor.clearColor;
        _top=[[DuoTouchView alloc] initWithKind:DuoTouchTop];[self addSubview:_top];
        _lower=[[DuoTouchView alloc] initWithKind:DuoTouchLower];[self addSubview:_lower];
        _toggle=[UIButton buttonWithType:UIButtonTypeSystem];
        [_toggle setImage:[UIImage systemImageNamed:@"arrow.up.left.and.arrow.down.right"] forState:UIControlStateNormal];
        _toggle.tintColor=[UIColor colorWithWhite:1 alpha:0.55];_toggle.backgroundColor=[UIColor colorWithWhite:0 alpha:0.35];
        _toggle.layer.cornerRadius=18;_toggle.accessibilityLabel=@"Full screen";
        [_toggle addTarget:self action:@selector(flip) forControlEvents:UIControlEventTouchUpInside];
        [_lower addSubview:_toggle];
        _trainer=[[DuoTrainerButton alloc] initWithFrame:CGRectZero];[_lower insertSubview:_trainer belowSubview:_toggle];_lower.trainer=_trainer;
        // settings/duo-screen.txt written while the game runs (a settings tool, a test over devicectl) applies within 0.5 s: its
        // modification time is checked; a changed file's word (full / else split) becomes the mode as the toggle would set it.
        __weak DuoOverlay* weakSelf=self;
        _settingsWatch=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*){[weakSelf watchSettings];}];
    }
    return self;
}
- (void)watchSettings {
    static struct timespec seen{};static bool first=true;
    const auto p=recomp::duo::app::settingsPath("duo-screen.txt");if(p.empty())return;
    struct stat st{};const bool exists=::stat(p.c_str(),&st)==0;
    // (run claude-duo-02: the file absent at launch, then written: its first appearance was taken as the launch's value) The first
    // check records the launch's state, absent included; any later appearance or change is applied.
    if(first){first=false;if(exists)seen=st.st_mtimespec;return;} // (the launch's value was read by loadMode)
    if(!exists || (st.st_mtimespec.tv_sec==seen.tv_sec && st.st_mtimespec.tv_nsec==seen.tv_nsec))return;
    seen=st.st_mtimespec;
    std::string t;if(!recomp::duo::app::readWords("duo-screen.txt",t))return;
    const bool split=!recomp::duo::app::hasWord(t,"full");
    std::fprintf(stderr,"APPLE_IOS_DUO_MODE file_changed=1 split=%d\n",split?1:0);
    [self setSplit:split source:"settings file"];
}
- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {UIView* v=[super hitTest:point withEvent:event];return v==self?nil:v;}
- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat w=self.bounds.size.width,h=self.bounds.size.height;
    const bool split=recomp::duo::state::splitLayout(w,h);
    const auto g=recomp::duo::state::splitGeometry(w,h);const CGFloat top=g.top;
    _lower.hidden=!split;_top.hidden=YES; // (the 238e base's universal pad does the top half's map touch: this view is unused)
    // (2026-10-25, the Thor mirror) In the split the universal pad (238e) covers the top half (the game) and the lower half is the
    // Thor's lower screen with its own touch: wheels, map tap, phone, Quick Save, the trainer button and its left-edge swipe. Full screen:
    // the pad over the whole view, as the regular build. The toggle: duo_state.h toggleRect (the lower screen's top-left corner).
    if(split){_top.frame=CGRectMake(0,0,w,top);_lower.frame=CGRectMake(g.lower.x,g.lower.y,g.lower.w,g.lower.h);{const auto tr=recomp::duo::state::toggleRect(g.lower.w,g.lower.h);_toggle.frame=CGRectMake(tr.x,tr.y,tr.w,tr.h);}}
    if(UIView* c=_touchControls){const auto r=recomp::duo::state::controlsRect(w,h);c.frame=CGRectMake(r.x,r.y,r.w,r.h);
        // (owner 2026-10-25: the Duo's menu "needs to be full screen") while the side menu is open, the pad view that owns it covers
        // both halves, so the menu (Frame::screen's full-screen panel) fills the screen and takes every touch; back to the top half on close.
        if([c isKindOfClass:PortQolTouchView.class] && [(PortQolTouchView*)c duoMenuOpen])c.frame=CGRectMake(0,0,w,h);}
    _trainer.frame=_lower.bounds;
    if([_touchControls isKindOfClass:PortQolTouchView.class])_trainer.qol=(PortQolTouchView*)_touchControls;
    [_trainer setNeedsDisplay];
}
- (void)flip {[self setSplit:!recomp::duo::state::split.load() source:"button (touch input)"];}
- (void)setSplit:(bool)on {[self setSplit:on source:"settings"];}
- (void)setSplit:(bool)on source:(const char*)source {
    namespace ds=recomp::duo::state;
    if(ds::split.exchange(on?1:0)==(on?1:0))return;
    recomp::duo::app::cancelAll();
    const auto p=recomp::duo::app::settingsPath("duo-screen.txt");
    if(!p.empty()) {
        ::mkdir((recomp::duo::app::root()+"/settings").c_str(),0700);
        if(FILE* f=std::fopen(p.c_str(),"w")){std::fputs(on?"split\n":"full\n",f);std::fclose(f);}
    }
    recomp_duo_set_lower_screen_v1(on?1:0);
    std::fprintf(stderr,"APPLE_IOS_DUO_MODE split=%d source=%s\n",on?1:0,source);
    recomp::duo::app::requestOrientation(_owner);
    for(UIView* v in _owner.view.subviews)[v setNeedsLayout];
    [self setNeedsLayout];
}
@end
