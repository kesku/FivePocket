// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-qol (Claude 2026-10-24, iPad build 231; iOS tree 1): the long presses on the game picture (the single-screen edition's rules,
// android-qol v700; ios_qol_gesture.h). Touches are taken in pixels (points x contentScaleFactor: the map touch's speeds are per pixel,
// as on Android), mapped through the iPad's own layout (fill by crop or fit, as ios_layer_geometry.h lays it out: the caller's fill();
// the HUD shrink: ios-hud-fit) and fed to the gesture the full-touch input owns; every change republishes the pad (FullTouchInput.hpp
// publish() merges the gesture). A display link ticks the gesture while it is due.
// files/private-trace/ios-qol-off (read at each touch down): the long presses do nothing (touches ignored, nothing held).
//
// ios-touchpad (iPad build 235): the same full-screen view hosts the universal touch controls (ios_touch_pad.h; SPEC v2), replacing the
// FullTouch* controls (owner: "ipads touch controller doesnt even work. Make your own"). It routes every finger as android-qol's QolTouch:
// a finger on a control or a stick is the pad's, anything else (a long-press zone, the pause map) the gesture's; it draws the pad (icons:
// ios_touch_pad_icons.h) with the display link (60-120 Hz, independent of the game's frame rate), polls recomp_touch_context_v1 every
// 100 ms, and reads Library/r/settings/touch-controls.txt (mode=context|pad|off hide=0|1 size=.. opacity=..) once at creation.
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#include <functional>
#include <memory>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ios_qol_gesture.h"
#if defined(RECOMP_DUO_EDITION)
#include "duo_state.h" // (ios-duo 2026-11-05: the top mode, for Stretch's frame)
#endif
#include "ios_side_menu.h"
#include "ios_menu_paint.h"
#include "ios_touch_pad.h"
#include "ios_touch_pad_icons.h"

#import <UIKit/UIGestureRecognizerSubclass.h>
// ios-touch probe (iPad 235k; 235i's session logged no touch at the pad view): a passive recognizer on the window that logs where each touch
// lands and which view UIKit's hit test picks (capped: the first 60 touches, then every 100th). It never recognizes (fails at the first
// touch's end), never delays or cancels touches: the views get exactly what they got before.
@interface PortTouchProbe : UIGestureRecognizer
@end
@implementation PortTouchProbe {unsigned long long n_;}
- (instancetype)init {
    if((self=[super initWithTarget:nil action:nil])){self.cancelsTouchesInView=NO;self.delaysTouchesBegan=NO;self.delaysTouchesEnded=NO;}
    return self;
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UIView* root=self.view;
    for(UITouch* t in touches) {
        if(++n_>60 && (n_%100))continue;
        const CGPoint p=[t locationInView:root];
        UIView* hit=[root hitTest:p withEvent:event];
        UIView* target=t.view;
        std::fprintf(stderr,"APPLE_IOS_TOUCH_PROBE n=%llu x=%.0f y=%.0f hit=%s target=%s hit_ui=%d hit_alpha=%.2f root=%s %.0fx%.0f (touch input)\n",n_,
            double(p.x),double(p.y),NSStringFromClass(hit.class).UTF8String,NSStringFromClass(target.class).UTF8String,
            int(hit.userInteractionEnabled),double(hit.alpha),NSStringFromClass(root.class).UTF8String,double(root.bounds.size.width),double(root.bounds.size.height));
    }
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {(void)touches;(void)event;self.state=UIGestureRecognizerStateFailed;}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {(void)touches;(void)event;self.state=UIGestureRecognizerStateFailed;}
- (BOOL)canPreventGestureRecognizer:(UIGestureRecognizer*)other {(void)other;return NO;}
- (BOOL)canBePreventedByGestureRecognizer:(UIGestureRecognizer*)other {(void)other;return NO;}
@end

// The display link's target (a block, so the link does not keep the view alive).
@interface PortQolTicker : NSObject
- (instancetype)initWithBlock:(void (^)(void))block;
- (void)fire;
@end
@implementation PortQolTicker {void (^block_)(void);}
- (instancetype)initWithBlock:(void (^)(void))block {if((self=[super init]))block_=[block copy];return self;}
- (void)fire {if(block_)block_();}
@end

@interface PortQolTouchView : UIView
- (instancetype)initWithGesture:(std::function<recomp::apple::qol::Gesture*()>)gesture
                            pad:(std::function<recomp::apple::touchpad::Pad*()>)pad
                        context:(std::function<int()>)context
                        changed:(std::function<void()>)changed
                            now:(std::function<long long()>)now fill:(std::function<bool()>)fill offPath:(NSString*)offPath
                  hudFitOffPath:(NSString*)hudFitOffPath settingsPath:(NSString*)settingsPath;
- (void)close;
- (void)setApplicationActive:(BOOL)active;
- (void)controllerPresent:(BOOL)present; // (a physical controller is detected: the pad hides when its setting says so)
// ios-sidemenu (SPEC §12): the side menu over the game. rows / effects: ios_side_menu.h (Main.mm builds them from the launcher's store);
// open: the menu opened / closed (the game's input neutral while open); buttons: the physical pad's buttons (XInput bits) to drive it.
- (void)attachMenu:(std::shared_ptr<recomp::apple::sidemenu::IosRows>)rows effects:(recomp::apple::sidemenu::Effects)effects
              open:(std::function<void(bool)>)open buttons:(std::function<unsigned()>)buttons;
- (void)padSettingsChanged; // (the pad's words or its HIDE state changed outside a touch: zones, publish, redraw)
- (recomp::apple::sidemenu::Menu*)menuModel; // (render_touch_pad.mm: the menu's pages drawn on the Mac)
- (UIView*)menuSceneView;                     // (render_touch_pad.mm: the view the menu's frame is painted in)
#if defined(RECOMP_DUO_EDITION)
// ios-duo (the Thor mirror, thor-radio v826..v832): the lower screen's trainer button / left-edge swipe open this side menu at its
// Trainer tab (the Thor opens its lower-screen trainer; the Duo shows it over the top half). Allowed = the Trainer switch on
// (ThorMenus.lowerAllowed). duoOpenTrainer: NO when not allowed or already open.
- (BOOL)duoTrainerAllowed;
- (BOOL)duoMenuOpen;
- (BOOL)duoOpenTrainer;
#endif
@end

@implementation PortQolTouchView {
    std::function<recomp::apple::qol::Gesture*()> gesture_;
    std::function<recomp::apple::touchpad::Pad*()> pad_;
    std::function<int()> context_;
    std::function<void()> changed_;
    std::function<long long()> now_;
    std::function<bool()> fill_;
    NSString* offPath_;NSString* hudFitOffPath_;
    CADisplayLink* link_;NSTimer* poll_;
    UITouch* first_;UITouch* second_;
    BOOL closed_,off_,controller_;
    unsigned long long downs_,fires_,padDowns_;
    float viewKey_[4];int lastWord_;unsigned loggedLatch_;std::uint64_t visualKey_;
    // ios-sidemenu
    std::shared_ptr<recomp::apple::sidemenu::IosRows> menuRows_;std::unique_ptr<recomp::apple::sidemenu::Menu> menu_;
    recomp::apple::sidemenu::Effects menuFx_;std::function<void(bool)> menuOpened_;std::function<unsigned()> menuButtons_;
    UITouch* menuFinger_;NSTimer* menuPad_;unsigned menuLast_,menuTicks_;BOOL menuWasOpen_;struct timespec menuCmd_;
    std::string fpsText_;unsigned fpsTick_; // (SPEC 12.5 v2: the app-drawn frame-rate label)
    UIVisualEffectView* menuBlur_;PortMenuSceneView* menuScene_; // (menu-redesign: the game blurred behind the pause-menu frame)
    int phoneBits_;AVAudioPlayer* trainerSound_;UIImage* trainerIcon_;BOOL trainerAssetsTried_; // (SPEC 3.15 / 3.14)
}
- (instancetype)initWithGesture:(std::function<recomp::apple::qol::Gesture*()>)gesture
                            pad:(std::function<recomp::apple::touchpad::Pad*()>)pad
                        context:(std::function<int()>)context
                        changed:(std::function<void()>)changed
                            now:(std::function<long long()>)now fill:(std::function<bool()>)fill offPath:(NSString*)offPath
                  hudFitOffPath:(NSString*)hudFitOffPath settingsPath:(NSString*)settingsPath {
    if((self=[super initWithFrame:CGRectZero])) {
        gesture_=std::move(gesture);pad_=std::move(pad);context_=std::move(context);changed_=std::move(changed);now_=std::move(now);
        fill_=std::move(fill);offPath_=offPath;hudFitOffPath_=hudFitOffPath;lastWord_=-3;
        self.backgroundColor=UIColor.clearColor;self.opaque=NO;self.multipleTouchEnabled=YES;self.isAccessibilityElement=NO;
        self.contentMode=UIViewContentModeRedraw;
        if(auto* g=gesture_())g->nativeMap(true); // (the map-tap macro is in this build's library: generic_renderer.cpp RECOMP_TOUCH_MAPTAP)
        if(auto* p=pad_()) {
            NSString* words=settingsPath?[NSString stringWithContentsOfFile:settingsPath encoding:NSASCIIStringEncoding error:nil]:nil;
            p->settings(words?words.UTF8String:nullptr);
            std::fprintf(stderr,"APPLE_IOS_TOUCH_PAD ready mode=%s hide=%d size=%.2f opacity=%.2f settings=%s\n",
                p->mode==recomp::apple::touchpad::Mode::Pad?"pad":p->mode==recomp::apple::touchpad::Mode::Off?"off":"context",
                int(p->hideWithController),double(p->sizeScale),double(p->opacity),words?"file":"default");
        }
        __weak PortQolTouchView* weak=self;
        link_=[CADisplayLink displayLinkWithTarget:[[PortQolTicker alloc] initWithBlock:^{[weak tick];}] selector:@selector(fire)];
        link_.paused=YES;[link_ addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        poll_=[NSTimer timerWithTimeInterval:0.1 repeats:YES block:^(NSTimer*){[weak poll];}];
        [NSRunLoop.mainRunLoop addTimer:poll_ forMode:NSRunLoopCommonModes];
        std::fputs("APPLE_IOS_QOL_TOUCH ready native_map=1 long_ms=400 zones=map,weapon,radio,character\n",stderr);
    }
    return self;
}
- (CGFloat)pxScale {return self.contentScaleFactor>0?self.contentScaleFactor:1;}
- (recomp::apple::qol::Frame)qolFrame {
    const CGFloat s=[self pxScale];
    const bool hudFit=::access(hudFitOffPath_.fileSystemRepresentation,F_OK)!=0;
#if defined(RECOMP_DUO_EDITION) // ios-duo (2026-11-05): Stretch maps the frame per axis onto the whole view (duo_state.h topPicture)
    if(recomp::duo::state::topMode.load(std::memory_order_relaxed)==2)return recomp::apple::qol::stretchFrame(float(self.bounds.size.width*s),float(self.bounds.size.height*s));
#endif
    return recomp::apple::qol::frame(float(self.bounds.size.width*s),float(self.bounds.size.height*s),(fill_?fill_():true),hudFit);
}
- (CGPoint)pixel:(UITouch*)t {const CGPoint p=[t locationInView:self];const CGFloat s=[self pxScale];return CGPointMake(p.x*s,p.y*s);}
static std::uintptr_t touchId(UITouch* t) {return reinterpret_cast<std::uintptr_t>((__bridge void*)t);}
// The pad's view: the size in pixels, ~12.8 px per mm at 2x (the iPad mini's 326 ppi), the HUD frame. Redone when any of them changes.
- (void)padView {
    auto* p=pad_();if(!p)return;
    const auto f=[self qolFrame];const CGFloat s=[self pxScale];
    const float key[4]={float(self.bounds.size.width*s),float(self.bounds.size.height*s),f.valid()?f.fw():0,f.valid()?f.fh():0};
    if(!std::memcmp(key,viewKey_,sizeof key))return;
    std::memcpy(viewKey_,key,sizeof key);
    p->view(key[0],key[1],float(326.0/25.4*s/2.0),f);
    if(auto* g=gesture_())g->padZones(p->padZones());
    [self setNeedsDisplay];
}
- (void)layoutSubviews {
    [super layoutSubviews];[self padView];
    if(menu_){const CGFloat s=[self pxScale];menu_->view(float(self.bounds.size.width*s),float(self.bounds.size.height*s));}
}
- (void)poll {
    namespace tp=recomp::apple::touchpad;
    if(closed_)return;
    [self padView];
    [self menuCommand];
    if(!(fpsTick_++%5))[self fpsRead]; // (SPEC 12.5 v2: twice a second)
    auto* p=pad_();if(!p)return;
    const int word=context_?context_():-1;
    const int before=p->context();const bool mapBefore=p->mapShown();const bool hidBefore=p->hidden();const auto overlayBefore=p->overlayMode();
    { // (SPEC 3.14: TRAINER while the trainer is on; SPEC 3.15: the phone's bits, read with the context word; absent exports = off)
        using StateFn=unsigned(*)();using PhoneFn=int(*)();
        static const StateFn trainerState=reinterpret_cast<StateFn>(::dlsym(RTLD_DEFAULT,"recomp_trainer_state_v1"));
        static const PhoneFn phoneState=reinterpret_cast<PhoneFn>(::dlsym(RTLD_DEFAULT,"recomp_phone_state_v1"));
        p->trainerButton(trainerState && (trainerState()&recomp::apple::sidemenu::StateEnabled)!=0);
        const int bits=phoneState?phoneState():0;
        if(bits!=phoneBits_){std::fprintf(stderr,"APPLE_IOS_TOUCH_PHONE bits=%d\n",bits);phoneBits_=bits;}
        p->phone(bits);
    }
    p->contextWord(word);
    if(p->overlayMode()!=overlayBefore)std::fprintf(stderr,"APPLE_IOS_TOUCH_OVERLAY %d (word %x)\n",int(p->overlayMode()),unsigned(word));
    if(auto* g=gesture_())g->mapShown(p->mapShown()); // (SPEC 3.11: on the pause map double taps are the map's)
    const int sc=0x3FF0000; // (bits 16-25: the screens and v706's pause-menu evidence bytes)
    if(word!=lastWord_ && ((word&0xFF)!=(lastWord_&0xFF) || (word&sc)!=(lastWord_&sc) || (word&0x8000)!=(lastWord_&0x8000)))
        std::fprintf(stderr,"APPLE_IOS_TOUCH_CONTEXT word=%d ctx=%d type=%d map=%d screens=%x layout=%d\n",word,word<0?word:(word&0xFF),
            word<0?-1:((word>>8)&0x7F)-1,word>=0 && (word&0x8000)?1:0,word<0?0:(word>>16)&0x3FF,p->context()); // (screens: bits 16-25, 22-25 = bit 18's evidence)
    if((word&sc)!=(lastWord_&sc))[self setNeedsDisplay];
    {const auto key=p->visualKey(now_());if(key!=visualKey_){visualKey_=key;[self setNeedsDisplay];}} // (a weapon switch shows with no touch)
    lastWord_=word;
    if(p->context()!=before || p->mapShown()!=mapBefore) // (SPEC 3.6: a latched wheel lets go on a context or pause-map change)
        if(auto* g=gesture_())if(g->latched())g->endLatch(now_());
    if(p->context()!=before || p->mapShown()!=mapBefore || p->hidden()!=hidBefore)[self padChanged];
    else if(p->animating(now_()))[self after];
}
- (void)controllerPresent:(BOOL)present {
    if(closed_ || present==controller_)return;
    controller_=present;
    if(menu_){menu_->gamepad(present);[menuScene_ setNeedsDisplay];} // (the menu's prompts: the strip with a pad, else the X and check icons)
    auto* p=pad_();if(!p)return;
    const bool was=p->hidden();p->controllerPresent(now_(),present);
    if(was!=p->hidden()){std::fputs("APPLE_IOS_TOUCH_PAD hidden: a controller is connected\n",stderr);[self padChanged];}
}
- (void)padChanged {
    if(auto* g=gesture_())if(auto* p=pad_())g->padZones(p->padZones());
    [self after];
}
// The stale-finger watchdog (TouchPad.retain): on every touch event but a new gesture's first and a cancel, a pad finger that is not among
// the view's current touches (any phase) lost its lift: released.
- (void)retainFrom:(UIEvent*)event {
    auto* p=pad_();if(!p || closed_)return;
    std::vector<std::uintptr_t> ids;
    for(UITouch* t in [event touchesForView:self])ids.push_back(touchId(t));
    const int n=p->retain(ids.data(),ids.size(),now_());
    if(n>0)std::fprintf(stderr,"APPLE_IOS_TOUCH_PAD stale fingers released n=%d (not in the event: a lost lift) (touch input)\n",n);
}
// SPEC 3.14 (QolTouch.app + PrivateTraceActivity's handler): the pad's app buttons, acted on after the touch on the next main-loop turn.
// SAVE asks the library for the trainer's OpenSaveMenu (the game's Save Game menu; the library refuses on a mission, with stars, during an
// autosave); TRAINER plays the owner's sound (owner builds) and opens the side menu on its Trainer tab.
- (void)appAction {
    auto* p=pad_();if(!p)return;
    const std::string a=p->takeAppAction();if(a.empty())return;
    std::fprintf(stderr,"APPLE_IOS_TOUCH_PAD app %s (touch input)\n",a.c_str());
    __weak PortQolTouchView* weak=self;
    dispatch_async(dispatch_get_main_queue(),^{
        PortQolTouchView* v=weak;if(!v || v->closed_)return;
        if(a=="qsave") {
            using Fn=int(*)(int,int,int,int);
            static const Fn fn=reinterpret_cast<Fn>(::dlsym(RTLD_DEFAULT,"recomp_trainer_v1"));
            std::fprintf(stderr,"APPLE_IOS_QUICK_SAVE request -> %d\n",fn?fn(recomp::apple::sidemenu::TrOpenSaveMenu,0,0,0):-100);
        } else if(a=="trainer" && v->menu_) {
            [v trainerAssets];if(v->trainerSound_){v->trainerSound_.currentTime=0;[v->trainerSound_ play];}
            const BOOL was=v->menu_->isOpen();
            if(!was)v->menu_->openMenu();
            v->menu_->toTab(recomp::apple::sidemenu::trainerTab(v->menu_->tabs()));
            std::fputs("APPLE_IOS_SIDE_MENU trainer button\n",stderr);
            [v menuAfter:was];
        }
    });
}
// The owner's trainer picture and sound (trainer-icon.png, trainer-open.m4a: claude_build.py bundles them in the owner's builds only);
// absent in a public build: the dotted skull, no sound.
- (void)trainerAssets {
    if(trainerAssetsTried_)return;
    trainerAssetsTried_=YES;
    if(NSString* path=[NSBundle.mainBundle pathForResource:@"trainer-icon" ofType:@"png"])trainerIcon_=[UIImage imageWithContentsOfFile:path];
    if(NSString* path=[NSBundle.mainBundle pathForResource:@"trainer-open" ofType:@"m4a"]) {
        trainerSound_=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:path] error:nil];
        [trainerSound_ prepareToPlay];
    }
}
- (void)after {
    [self appAction];
    if(auto* g=gesture_())if(g->latchTouches()!=loggedLatch_) { // (SPEC 3.6 v2: each latched-wheel touch: where it landed, what it did)
        loggedLatch_=g->latchTouches();
        std::fprintf(stderr,"APPLE_IOS_QOL_LATCH_TOUCH n=%u rel=%.2f outcome=%c (touch input)\n",loggedLatch_,double(g->lastLatchRel()),g->lastLatchOutcome()?g->lastLatchOutcome():'-');
    }
    if(auto* p=pad_())if(auto* g=gesture_()) { // (SPEC 3.6: a wheel button held still and lifted: kept held by the gesture)
        const auto z=p->takeLatch();
        if(z!=recomp::apple::qol::Gesture::Zone::None){g->latch(now_(),z);std::fprintf(stderr,"APPLE_IOS_TOUCH_PAD latch zone=%d (touch input)\n",int(z));}
    }
    if(changed_)changed_();
    auto* g=gesture_();auto* p=pad_();
    const long long t=now_();
    const bool due=(g && g->due()!=recomp::apple::qol::None) || (p && p->due(t)!=recomp::apple::qol::None);
    link_.paused=closed_ || !(due || (p && p->animating(t)));
    [self setNeedsDisplay];
}
- (void)tick {
    if(closed_){link_.paused=YES;return;}
    auto* g=gesture_();auto* p=pad_();
    const long long t=now_();
    if(g && g->due()<=t) {
        const auto before=g->state();g->tick(t);
        if(before==recomp::apple::qol::Gesture::State::Armed && g->state()!=before)
            std::fprintf(stderr,"APPLE_IOS_QOL_TOUCH fire zone=%d buttons=0x%x n=%llu (touch input)\n",int(g->held()),g->buttons(),++fires_);
    }
    (void)p;[self after];
}
- (void)gameBegan:(NSSet<UITouch*>*)touches event:(UIEvent*)event {
    auto* g=gesture_();auto* p=pad_();
    {static unsigned long long seen=0;
     for(UITouch* t in touches)if(++seen<=60 || !(seen%100)){const CGPoint q=[self pixel:t];
         std::fprintf(stderr,"APPLE_IOS_TOUCH_VIEW n=%llu x=%.0f y=%.0f closed=%d ui=%d hidden=%d mode=%d (touch input)\n",seen,double(q.x),double(q.y),
             int(closed_),int(self.userInteractionEnabled),int(p && (p->hidden() || p->userHidden())),p?int(p->mode):-1);}}
    if(closed_)return;
    const long long now=now_();
    const auto f=[self qolFrame];
    // A new gesture (every finger on the view is new): nothing of the pad can still be down (a lost lift); otherwise the watchdog.
    if(p && [event touchesForView:self].count==touches.count)p->noFingers(now);
    else [self retainFrom:event];
    for(UITouch* t in touches) {
        const CGPoint q=[self pixel:t];
        if(p && !(g && !first_ && g->wantsFinger())) { // (SPEC 3.6: a latched wheel's finger, or one while the pad holds a wheel's button, is the gesture's)
            const bool wasHidden=p->hidden() || p->userHidden();
            if(p->down(touchId(t),now,float(q.x),float(q.y),p->zone(float(q.x),float(q.y),f))) {
                if(++padDowns_<=40 || !(padDowns_%200))std::fprintf(stderr,"APPLE_IOS_TOUCH_PAD down n=%llu x=%.0f y=%.0f ctx=%d hidden=%d (touch input)\n",
                    padDowns_,double(q.x),double(q.y),p->context(),int(p->userHidden()));
                if(wasHidden!=(p->hidden() || p->userHidden()) && g)g->padZones(p->padZones());
                continue;
            }
            if(wasHidden!=(p->hidden() || p->userHidden()) && g)g->padZones(p->padZones());
        }
        if(!g)continue;
        if(!first_) {
            off_=::access(offPath_.fileSystemRepresentation,F_OK)==0;if(off_)continue;
            first_=t;++downs_;g->down(now,float(q.x),float(q.y),f);
        } else if(!second_ && !off_) {second_=t;g->pointerDown(now,float(q.x),float(q.y),f);}
    }
    [self after];
}
- (void)gameMoved:(NSSet<UITouch*>*)touches event:(UIEvent*)event {
    auto* g=gesture_();auto* p=pad_();if(closed_)return;
    [self retainFrom:event];
    const long long now=now_();
    if(p)for(UITouch* t in touches)if(p->owns(touchId(t))){const CGPoint q=[self pixel:t];p->move(touchId(t),now,float(q.x),float(q.y));}
    if(g && !off_ && first_) {
        const CGPoint a=[self pixel:first_];const auto f=[self qolFrame];
        if(second_){const CGPoint b=[self pixel:second_];g->move2(now,float(a.x),float(a.y),float(b.x),float(b.y),f);}
        else g->move(now,float(a.x),float(a.y),f);
    }
    [self after];
}
- (void)ended:(NSSet<UITouch*>*)touches event:(UIEvent*)event cancelled:(BOOL)cancelled {
    auto* g=gesture_();auto* p=pad_();if(closed_)return;
    const long long now=now_();
    for(UITouch* t in touches) {
        if(p && p->owns(touchId(t))){p->up(touchId(t),now);continue;}
        if(!g)continue;
        if(t==second_){second_=nil;if(!off_)g->pointerUp(now);}
        else if(t==first_){first_=nil;second_=nil;if(!off_)g->up(now);}
    }
    if(p) {
        // The last finger lifted (or a cancel): any finger of the pad still held is stale (SPEC 5 robustness: nothing stays stuck).
        bool any=false;
        for(UITouch* t in [event touchesForView:self])if(![touches containsObject:t] && t.phase!=UITouchPhaseEnded && t.phase!=UITouchPhaseCancelled)any=true;
        if(cancelled && !any)p->cancel(now);
        else if(!any)p->noFingers(now);
    }
    [self after];
}
// ios-sidemenu (SPEC 12.1; android-qol QolTouch.routeMenu): the menu has the first look at every touch. A finger landing in the left edge
// strip (with no other finger down) is the menu's gesture only, never a stick or a control; while the menu is open every touch is the
// menu's. The edge finger's moves still go on to the game's handlers (they own no such finger: nothing happens there).
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if(menu_ && !closed_) {
        const BOOL wasOpen=menu_->isOpen();
        const BOOL fresh=[event touchesForView:self].count==touches.count;
        NSMutableSet<UITouch*>* rest=[touches mutableCopy];
        for(UITouch* t in touches)if(!menuFinger_ && (fresh || wasOpen)){const CGPoint q=[self pixel:t];
            if(menu_->down(touchId(t),now_(),float(q.x),float(q.y))){menuFinger_=t;[rest removeObject:t];}}
        [self menuAfter:wasOpen];
        if(menu_->isOpen() || !rest.count)return;
        touches=rest;
    }
    [self gameBegan:touches event:event];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if(menu_ && !closed_) {
        const BOOL wasOpen=menu_->isOpen();
        if(menuFinger_ && [touches containsObject:menuFinger_]){const CGPoint q=[self pixel:menuFinger_];menu_->move(touchId(menuFinger_),now_(),float(q.x),float(q.y));}
        [self menuAfter:wasOpen];
        if(wasOpen || menu_->isOpen())return;
    }
    [self gameMoved:touches event:event];
}
- (BOOL)menuEnded:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled {
    if(!menu_ || closed_)return NO;
    const BOOL wasOpen=menu_->isOpen();
    if(menuFinger_ && [touches containsObject:menuFinger_]) {
        const CGPoint q=[self pixel:menuFinger_];
        if(cancelled)menu_->cancel();else menu_->up(touchId(menuFinger_),now_(),float(q.x),float(q.y));
        menuFinger_=nil;
    }
    [self menuAfter:wasOpen];
    return wasOpen || menu_->isOpen();
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if([self menuEnded:touches cancelled:NO])return;
    [self retainFrom:event];[self ended:touches event:event cancelled:NO];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if([self menuEnded:touches cancelled:YES])return;
    [self ended:touches event:event cancelled:YES];
}

// ======== ios-sidemenu: attaching, events, the gamepad, the test command
- (void)attachMenu:(std::shared_ptr<recomp::apple::sidemenu::IosRows>)rows effects:(recomp::apple::sidemenu::Effects)effects
              open:(std::function<void(bool)>)open buttons:(std::function<unsigned()>)buttons {
    if(closed_ || !rows)return;
    menuRows_=std::move(rows);menuFx_=std::move(effects);menuOpened_=std::move(open);menuButtons_=std::move(buttons);
    menu_=std::make_unique<recomp::apple::sidemenu::Menu>(*menuRows_);menu_->gamepad(controller_);
    menu_->measure(recomp::apple::menu::paint::fontMeasure()); // (the texts are fitted with the face they are drawn in)
    // menu-redesign: the frame over a live blur of the game (Android blurs a captured copy; DESIGN.md 7.3), both hidden until it opens
    menuBlur_=[[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
    menuBlur_.userInteractionEnabled=NO;menuBlur_.hidden=YES;menuBlur_.frame=self.bounds;
    menuBlur_.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    menuScene_=[[PortMenuSceneView alloc] initWithFrame:self.bounds];menuScene_.hidden=YES;menuScene_.liveBlur=YES;
    menuScene_.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    {auto* m=menu_.get();menuScene_.scene=[m]() -> const recomp::apple::menu::Scene* {return m->isOpen()?&m->scene():nullptr;};
     menuScene_.alertShown=[m]{return m->alertShown();};}
    [self addSubview:menuBlur_];[self addSubview:menuScene_];
    {auto padFn=pad_;menu_->controls([padFn](float x,float y){auto* p=padFn?padFn():nullptr;return p && p->controlAt(x,y);});} // (SPEC 12.7: the eye stays the pad's)
    const CGFloat s=[self pxScale];menu_->view(float(self.bounds.size.width*s),float(self.bounds.size.height*s));
    // (the test command's file as it was at start: only a later change acts)
    struct stat st{};menuCmd_={};
    if(::stat([self menuCommandPath].fileSystemRepresentation,&st)==0)menuCmd_=st.st_mtimespec;
    std::fprintf(stderr,"APPLE_IOS_SIDE_MENU ready settings=%d trainer=%d script_mods=%d edge=%.3f open_dx=%.2f open_ms=%lld\n",
        int(menuRows_->settingsAvailable),int(menuRows_->hasTrainer()),int(menuRows_->scriptModsPresent),
        double(recomp::apple::sidemenu::Edge),double(recomp::apple::sidemenu::OpenDx),recomp::apple::sidemenu::OpenMs);
}
- (recomp::apple::sidemenu::Menu*)menuModel {return menu_.get();}
#if defined(RECOMP_DUO_EDITION)
- (BOOL)duoTrainerAllowed {return !closed_ && menu_ && menuRows_ && menuRows_->trainerOn();}
- (BOOL)duoMenuOpen {return menu_ && menu_->isOpen();}
- (BOOL)duoOpenTrainer {
    if(![self duoTrainerAllowed] || menu_->isOpen())return NO;
    menu_->openMenu();
    const auto ts=menu_->tabs();
    for(std::size_t i=0;i<ts.size();++i)if(ts[i].title=="Trainer"){menu_->toTab(int(i));break;}
    [self menuAfter:NO];
    return YES;
}
#endif
- (UIView*)menuSceneView {return menuScene_;}
- (NSString*)menuCommandPath {return [offPath_.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"ios-sidemenu-cmd"];}
// TEST ONLY (the screenshot check, like android-qol's debug broadcast): private-trace/ios-sidemenu-cmd written with "open", "alert"
// (the ALERT without changing a setting) or "close"; acted on when the file's time changes.
- (void)menuCommand {
    if(!menu_)return;
    struct stat st{};
    if(::stat([self menuCommandPath].fileSystemRepresentation,&st)!=0)return;
    if(st.st_mtimespec.tv_sec==menuCmd_.tv_sec && st.st_mtimespec.tv_nsec==menuCmd_.tv_nsec)return;
    menuCmd_=st.st_mtimespec;
    NSString* text=[[NSString stringWithContentsOfFile:[self menuCommandPath] encoding:NSASCIIStringEncoding error:nil]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    const BOOL wasOpen=menu_->isOpen();
    if([text isEqualToString:@"open"])menu_->openMenu();
    else if([text isEqualToString:@"alert"])menu_->alertForTest();
    else if([text isEqualToString:@"close"])menu_->close();
    std::fprintf(stderr,"APPLE_IOS_SIDE_MENU command=%s\n",text.UTF8String?:"?");
    [self menuAfter:wasOpen];
}
// After every menu input: its events applied (ios_side_menu.h apply), an opening lets every game touch go and makes the game's input
// neutral, a closing gives it back; the gamepad timer runs while open.
- (void)menuAfter:(BOOL)wasOpen {
    if(!menu_)return;
    const long long t=now_();
    if(!wasOpen && menu_->isOpen()) {
        first_=second_=nil;
        if(auto* g=gesture_())g->cancel();
        if(auto* p=pad_())p->cancel(t);
        if(menuOpened_)menuOpened_(true);
        menuLast_=menuButtons_?menuButtons_():0u;menuTicks_=0; // (a button held at the opening does nothing)
        __weak PortQolTouchView* weak=self;
        [menuPad_ invalidate];menuPad_=[NSTimer timerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer*){[weak menuPadTick];}];
        [NSRunLoop.mainRunLoop addTimer:menuPad_ forMode:NSRunLoopCommonModes];
    }
    for(const auto& e:menu_->take()) {
        std::fprintf(stderr,"APPLE_IOS_SIDE_MENU %s page=%s (touch input)\n",e.str().c_str(),recomp::apple::sidemenu::pageName(menu_->page()));
        recomp::apple::sidemenu::apply(*menuRows_,menuFx_,e);
    }
    if(wasOpen && !menu_->isOpen()) {
        [menuPad_ invalidate];menuPad_=nil;menuFinger_=nil;
        if(menuOpened_)menuOpened_(false);
    }
    menuBlur_.hidden=menuScene_.hidden=!menu_->isOpen();
    if(menu_->isOpen())[menuScene_ setNeedsDisplay];
    if(wasOpen || menu_->isOpen())[self after];
}
- (void)menuPadTick {
    namespace tp=recomp::apple::touchpad;
    if(closed_ || !menu_ || !menu_->isOpen()){[menuPad_ invalidate];menuPad_=nil;return;}
    const unsigned b=menuButtons_?menuButtons_():0u,press=b&~menuLast_;menuLast_=b;
    if(recomp::apple::sidemenu::trainerPage(menu_->page()) && !(++menuTicks_%15))[menuScene_ setNeedsDisplay]; // (the trainer's state, 4 per s)
    if(!press)return;
    if(press&tp::DU)menu_->up();
    if(press&tp::DD)menu_->down();
    if(press&tp::DL)menu_->left();
    if(press&tp::DR)menu_->right();
    if(press&tp::A)menu_->select();
    if(press&tp::B)menu_->back();
    if(press&tp::START)menu_->close();
    if(press&tp::LB)menu_->prevTab(); // (menu-redesign: L1 / R1 change the tab)
    if(press&tp::RB)menu_->nextTab();
    [self menuAfter:YES];
}
- (void)padSettingsChanged {
    if(closed_)return;
    std::memset(viewKey_,0,sizeof viewKey_);[self padView];[self padChanged];
}
- (void)setApplicationActive:(BOOL)active {
    if(closed_)return;
    if(!active){first_=second_=nil;if(auto* g=gesture_())g->cancel();if(auto* p=pad_())p->cancel(now_());if(menu_)menu_->cancel();menuFinger_=nil;[self after];}
    self.userInteractionEnabled=active;
}
- (void)close {
    if(closed_)return;closed_=YES;first_=second_=nil;
    [menuPad_ invalidate];menuPad_=nil;menuFinger_=nil;if(menu_ && menu_->isOpen() && menuOpened_)menuOpened_(false);
    if(auto* g=gesture_())g->cancel();
    if(auto* p=pad_())p->cancel(now_());
    if(changed_)changed_();
    [poll_ invalidate];poll_=nil;[link_ invalidate];link_=nil;[self removeFromSuperview];
    std::fprintf(stderr,"APPLE_IOS_QOL_TOUCH closed downs=%llu fires=%llu pad_downs=%llu\n",downs_,fires_,padDowns_);
}
- (void)dealloc {[link_ invalidate];[poll_ invalidate];[menuPad_ invalidate];}

// ======== drawing (SPEC 3.5, the GTA-mobile look shared with Android: ios_touch_pad_icons.h, a port of android-qol's TouchRender.swift).
// dp = 1 point. The 360 pad mode keeps its letters in coloured rings.
// SPEC 12.5 v2: the library's rates (recomp_fps_v1 in the same binary, looked up once: absent = no label). A read also tells the
// library to draw no label of its own for 2 s. Redrawn only when the text changes.
- (void)fpsRead {
    using Fn=int(*)(float*,int);
    static const Fn fn=reinterpret_cast<Fn>(::dlsym(RTLD_DEFAULT,"recomp_fps_v1"));
    if(!fn)return;
    float out[3]{};const int on=fn(out,3);
    auto text=recomp::apple::touchpad::fpsText(on,out[0]);
    if(text!=fpsText_){fpsText_=std::move(text);[self setNeedsDisplay];}
}
- (void)drawFps {
    namespace tp=recomp::apple::touchpad;
    if(fpsText_.empty())return;
    const CGFloat s=[self pxScale];
    const auto pl=tp::fpsPlace(float(self.bounds.size.width*s),float(self.bounds.size.height*s),fill_?fill_():true);
    UIFont* f=[UIFont systemFontOfSize:pl.size/s weight:UIFontWeightMedium];
    NSShadow* shadow=[NSShadow new];shadow.shadowColor=[UIColor colorWithWhite:0 alpha:tp::FpsShadowAlpha];
    shadow.shadowBlurRadius=5*pl.k/s;shadow.shadowOffset=CGSizeMake(0,1.5*pl.k/s);
    NSDictionary* at=@{NSFontAttributeName:f,NSForegroundColorAttributeName:[UIColor colorWithWhite:1 alpha:tp::FpsColorAlpha],NSShadowAttributeName:shadow};
    [[NSString stringWithUTF8String:fpsText_.c_str()] drawAtPoint:CGPointMake(pl.x/s,pl.baseline/s-f.ascender) withAttributes:at];
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    [self drawPad];
    if(!closed_)[self drawFps];
}
// SPEC 12.8 (android-qol TouchPadView.drawPrompt): a GTA prompt icon - a face button as a filled circle in its colour with its white
// letter; the D-pad as a white cross on a dark disc; BACK (the indoor map's "Switch View") a white circle with a black left triangle;
// held = brighter. At the pad's opacity (not the map's 20 %).
static void drawPrompt(CGContextRef c,const recomp::apple::touchpad::Control& k,bool held,CGFloat cx,CGFloat cy,CGFloat kr,CGFloat a,CGFloat dp) {
    const CGFloat r=kr*0.82;const std::string_view key=k.glyph;
    const auto fillCircle=[&](CGFloat rad){CGContextFillEllipseInRect(c,CGRectMake(cx-rad,cy-rad,2*rad,2*rad));};
    if(key=="DPAD") {
        CGContextSetGrayFillColor(c,held?0x8A/255.0:0,(held?1.0:0.70)*a);fillCircle(kr);
        const CGFloat w=kr*0.36,l=kr*0.82;
        CGContextSetGrayFillColor(c,1,0.8*0.8*a);
        CGContextFillRect(c,CGRectMake(cx-w,cy-l,2*w,2*l));CGContextFillRect(c,CGRectMake(cx-l,cy-w,2*l,2*w));
        return;
    }
    if(key=="BACK") {
        CGContextSetGrayFillColor(c,held?0xBD/255.0:1,a);fillCircle(r);
        CGContextSetGrayFillColor(c,0,a);
        CGContextMoveToPoint(c,cx-0.45*r,cy);CGContextAddLineToPoint(c,cx+0.30*r,cy-0.42*r);CGContextAddLineToPoint(c,cx+0.30*r,cy+0.42*r);
        CGContextClosePath(c);CGContextFillPath(c);
        return;
    }
    const std::uint32_t col=k.color;
    CGContextSetRGBFillColor(c,((col>>16)&0xFF)/255.0,((col>>8)&0xFF)/255.0,(col&0xFF)/255.0,a);fillCircle(r);
    if(held){CGContextSetGrayFillColor(c,1,0.4*a);fillCircle(r);}
    CGContextSetGrayStrokeColor(c,0,0.6*a);CGContextSetLineWidth(c,1.4*dp);CGContextStrokeEllipseInRect(c,CGRectMake(cx-r,cy-r,2*r,2*r));
    UIFont* f=[UIFont boldSystemFontOfSize:r*1.05];
    NSString* t=[NSString stringWithUTF8String:k.glyph];
    NSDictionary* at=@{NSFontAttributeName:f,NSForegroundColorAttributeName:[UIColor colorWithWhite:1 alpha:a]};
    const CGSize sz=[t sizeWithAttributes:at];
    [t drawAtPoint:CGPointMake(cx-sz.width*0.5,cy-sz.height*0.5) withAttributes:at];
}
// SPEC 3.14 (TouchPadView.drawTrainer): the owner's picture clipped to the disc, else the Thor's dotted skull and crossbones.
static constexpr const char* TrainerDots[]={"...#######...","..#########..",".###########.",".##...#...##.",".##...#...##.",".###########.",
    "..####.####..","...#######...","...#.#.#.#...","#...........#",".##.......##.","...##...##...",".##.......##.","#...........#"};
- (void)drawTrainer:(CGContextRef)c cx:(CGFloat)cx cy:(CGFloat)cy r:(CGFloat)kr alpha:(CGFloat)a dp:(CGFloat)dp {
    [self trainerAssets];
    const CGFloat r=kr-3.1*dp;
    if(trainerIcon_) {
        CGContextSaveGState(c);CGContextAddEllipseInRect(c,CGRectMake(cx-r,cy-r,2*r,2*r));CGContextClip(c);
        [trainerIcon_ drawInRect:CGRectMake(cx-r,cy-r,2*r,2*r) blendMode:kCGBlendModeNormal alpha:std::clamp<CGFloat>(a,0,1)];
        CGContextRestoreGState(c);return;
    }
    const CGFloat side=kr*1.3,cell=side/14,dot=cell*0.74,x0=cx-13*cell*0.5,y0=cy-side*0.5;
    CGContextSetGrayFillColor(c,1,a);
    for(int j=0;j<14;++j)for(int i=0;TrainerDots[j][i];++i)if(TrainerDots[j][i]=='#')
        CGContextFillRect(c,CGRectMake(x0+i*cell+(cell-dot)*0.5,y0+j*cell+(cell-dot)*0.5,dot,dot));
    CGContextSetRGBFillColor(c,0xEB/255.0,0x1F/255.0,0x1F/255.0,a);
    for(const CGFloat col:{4.5,8.5}){const CGFloat ex=x0+col*cell,ey=y0+4*cell;CGContextFillRect(c,CGRectMake(ex-dot*0.6,ey-dot*0.6,dot*1.2,dot*1.2));}
}
- (void)drawPad {
    namespace tp=recomp::apple::touchpad;namespace ic=recomp::apple::touchpad::icons;
    auto* p=pad_();if(!p || closed_)return;
    const long long t=now_();
    const float a=p->alpha(t);
    if(a<=0.01f && !p->userHidden())return;
    CGContextRef c=UIGraphicsGetCurrentContext();
    const CGFloat s=[self pxScale],dp=1;
    for(const auto& k:p->controls()) {
        if(p->userHidden() && k.kind!=tp::Kind::Hide)continue;
        if(tp::isPrompt(k)){drawPrompt(c,k,p->held(k),k.cx/s,k.cy/s,k.r/s,p->opacity,dp);continue;} // (SPEC 12.8: at the pad's opacity over the map)
        const bool skipOverlay=p->mode!=tp::Mode::Pad && p->overlayMode()==tp::Overlay::Skip; // (the 360 pad ignores the overlays)
        if(skipOverlay && !p->skipShown(t))continue; // (SPEC 3.10: SKIP only after a touch)
        if(p->mapShown() && k.bits!=tp::START && k.kind!=tp::Kind::Hide)continue;
        const CGFloat ka=p->userHidden()?std::max<CGFloat>(a,0.5):skipOverlay?a*0.5f:a; // (SKIP is faded)
        const bool held=p->held(k);
        const CGFloat cx=k.cx/s,cy=k.cy/s,r=k.r/s;
        const bool pedal=k.kind==tp::Kind::TriggerL || k.kind==tp::Kind::TriggerR;
        const bool callKey=k.is("call_answer") || k.is("call_decline"); // (SPEC 3.15: a call's buttons always tinted)
        const std::uint32_t tint=callKey?(k.is("call_answer")?0x40BF4Du:0xD93333u):held && pedal?(k.kind==tp::Kind::TriggerR?0x40BF4Du:0xD93333u):0u;
        if(k.kind==tp::Kind::WeaponTap && p->weaponIv) { // (SPEC 3.12 v15, GTA IV style: no disc - the group's silhouette at 1.15 r, the game's grey
            //  #A8A8A8 with a thin dark edge (0.6 units), white while pressed; never dimmed by the idle fade)
            ic::drawIcon(c,tp::weaponIcon(p->weaponGroup()).c_str(),cx,cy,r*1.15f,p->weaponAlpha(t),false,held?1.0:0xA8/255.0,0.6);
            continue;
        }
        ic::drawDisc(c,cx,cy,r,held && !pedal && !callKey,ka,dp,tint);
        if(k.is("trainer")){[self drawTrainer:c cx:cx cy:cy r:r alpha:ka dp:dp];continue;} // (SPEC 3.14)
        const char* name=tp::iconName(k,p->userHidden());
        if(k.is("handbrake") && p->driveByHeld())name="fire"; // (SPEC 2: while DRIVE-BY is held, HANDBRAKE is FIRE: LB + RB)
        if(k.kind==tp::Kind::WeaponTap) // (SPEC 3.12: the selected weapon's group silhouette, grey #B4B4B4 like the game's weapon icons)
            ic::drawIcon(c,tp::weaponIcon(p->weaponGroup()).c_str(),cx,cy,r*ic::IconShare,ka,held,0xB4/255.0);
        else if(*name)ic::drawIcon(c,name,cx,cy,r*ic::IconShare,ka,held && !pedal);
        else { // (the 360 pad mode: its letter in the button's colour ring)
            const std::uint32_t col=k.color;
            CGContextSetLineWidth(c,3*dp);CGContextSetRGBStrokeColor(c,((col>>16)&0xFF)/255.0,((col>>8)&0xFF)/255.0,(col&0xFF)/255.0,ka);
            CGContextStrokeEllipseInRect(c,ic::circle(cx,cy,r-6*dp));
            ic::drawLetters(c,k.glyph,cx,cy,r,[UIColor colorWithWhite:held?0:1 alpha:0.95*ka]);
        }
    }
    if(p->mapShown() || p->userHidden() || p->mode==tp::Mode::Off)return;
    if(p->sticksOff(t))return; // (SPEC 3.8: a screen is open - the sticks and the swipe area are not drawn)
    const float sa=a;
    for(int side=0;side<2;++side) {
        if(side==1 && p->rightSwipe && p->mode!=tp::Mode::Pad){(void)p->stickDraw(side,t);continue;} // (owner 20:35: the right side is a
                                                                                                    //  swipe area; SPEC 4.1: the 360 pad draws it)
        const auto& st=p->sticks[side];
        const float stickA=sa*p->stickAlpha(side,t); // (SPEC 4.2: only under a finger and while it fades; read before stickDraw ends the release)
        const auto d=p->stickDraw(side,t);
        if(stickA<=0.01f)continue;
        ic::drawStick(c,d.cx/s,d.cy/s,st.R/s,d.kx/s,d.ky/s,st.held,stickA,dp);
    }
}
@end
