// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-qol (Claude 2026-10-24, iOS tree 1; owner: "long press the map for the map toggle, or long press the weapon icon for the
// weapon wheel, or long press the character select wheel for the select wheel, it doesn't have to draw a new ui ... it just has to
// apply the same touch variables"). A C++ port of the single-screen edition's QolGesture.java and the Thor's MapTouch.java
// (local-analysis/single-screen-edition, android-qol v700; thor-edition apk-tree) with the same rules, zones and constants. No UIKit
// types: tests/ios_qol_gesture_test.cpp runs it on the host.
//
// The one iOS difference is the geometry (frame()): the iPad shows the 1280x720 picture centred and scaled (fill by crop: the larger
// axis ratio, the sides cut off; fit: the smaller one, bars), and ios-hud-fit draws the HUD shrunk about (640, 360) by
// k = min(visible x, visible y) so it stays inside the screen. A touch is taken into HUD guest pixels (where the game drew the minimap,
// the weapon HUD and its wheels before the shrink), clamped to the 1280x720 frame (a touch past an edge counts as that edge), then
// expressed in a frame of view pixels (1280 x 720 times the view pixels per HUD pixel) so the map touch's speeds stay in view pixels.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace recomp::apple::qol {

constexpr long long None=std::numeric_limits<long long>::max();

// The view -> HUD frame mapping. scale: view pixels per picture pixel; k: the HUD shrink (1 = none).
struct Frame {
    float scale=0,k=1,w=0,h=0;
    float ax=1,ay=1; // (ios-duo 2026-11-05, Stretch: the picture's extra scale per axis on top of scale; 1 = uniform, every iPad layout)
    bool valid() const noexcept {return scale>0 && k>0 && w>0 && h>0;}
    float s() const noexcept {return scale*k;}                 // view pixels per HUD guest pixel
    float fw() const noexcept {return 1280.0f*s()*ax;}
    float fh() const noexcept {return 720.0f*s()*ay;}
    // HUD guest pixels of a view point, clamped to the frame.
    float gx(float x) const noexcept {return std::clamp(640.0f+((x-w*0.5f)/(scale*ax))/k,0.0f,1280.0f);}
    float gy(float y) const noexcept {return std::clamp(360.0f+((y-h*0.5f)/(scale*ay))/k,0.0f,720.0f);}
    // The same point in the frame's own view-pixel coordinates (origin at the frame's top left).
    float fx(float x) const noexcept {return gx(x)*s()*ax;}
    float fy(float y) const noexcept {return gy(y)*s()*ay;}
};
// The iPad's layout (ios_layer_geometry.h iosLayerGeometry; generic_renderer.cpp ios-hud-fit, uniform shrink): fill picks the larger
// axis ratio, fit the smaller; the visible fractions are min(1, view / (picture * scale)); the HUD shrinks by min(vx, vy) when either
// is below 1 (and above 0.25, as the renderer), unless hudFit is off.
inline Frame frame(float w,float h,bool fill,bool hudFit=true) noexcept {
    Frame f;if(!(w>0) || !(h>0) || !std::isfinite(w) || !std::isfinite(h))return f;
    f.w=w;f.h=h;f.scale=fill?std::max(w/1280.0f,h/720.0f):std::min(w/1280.0f,h/720.0f);
    const float vx=std::min(1.0f,w/(1280.0f*f.scale)),vy=std::min(1.0f,h/(720.0f*f.scale)),m=std::min(vx,vy);
    f.k=(hudFit && (vx<1.0f || vy<1.0f) && m>0.25f)?m:1.0f;
    return f;
}

// ios-duo (2026-11-05): Stretch - the frame scaled per axis to the whole view (no bars, no crop): scale = the smaller ratio, the other
// axis stretched by ax / ay so the frame is exactly w x h; the HUD is not shrunk (nothing is cropped).
inline Frame stretchFrame(float w,float h) noexcept {
    Frame f;if(!(w>0) || !(h>0) || !std::isfinite(w) || !std::isfinite(h))return f;
    f.w=w;f.h=h;f.scale=std::min(w/1280.0f,h/720.0f);f.ax=(w/1280.0f)/f.scale;f.ay=(h/720.0f)/f.scale;f.k=1.0f;
    return f;
}

// The Thor's main-screen map touch (MapTouch.java): drag = pan, pinch = zoom, tap = waypoint, on the touchpad bit. SPEC 3.11 (the owner
// 2026-10-24: "a double tap on the open map should close it too (waypoint taps can get a short delay)"): with delayTaps (while the pause map
// is shown) a tap's waypoint waits DoubleTapMs after its lift; a second short tap whose down comes in that window is a double tap: no
// waypoint, takeClose() turns true (the gesture sends the close). A drag or a pinch in the window drops the waiting waypoint.
class MapTouch {
public:
    static constexpr int Touch=0x0400,Request=0x0800;
    static constexpr int Pinch=-32768; // (map_tap.h PinchCode)
    static constexpr long long TapMs=300,PulseMs=150,LockMs=700,TickMs=16,DoubleTapMs=300;
    static constexpr float TapSlop=0.03f,PanFull=480.0f,PanStill=15.0f,PanMin=0.30f,PanSmoothMs=50.0f,ZoomFull=2.0f,ZoomStill=0.2f,ZoomSmoothMs=60.0f;
    static constexpr int TriggerMin=80;
    enum class State {Idle,Armed,Drag,Pinch,Tap,Ignored};
    State state() const noexcept {return state_;}
    long long due() const noexcept {return next_;}
    int buttons() const noexcept {return buttons_;}
    int rightX() const noexcept {return rx_;}
    int rightY() const noexcept {return ry_;}
    bool active() const noexcept {return buttons_!=0;}
    /** SPEC 3.11: waypoint taps wait for a possible second tap (on: the pause map is shown). Off drops a waiting one. */
    void delayTaps(bool on) noexcept {if(!on && delayTaps_){pendUntil_=NoPend;second_=false;if(state_==State::Idle)next_=None;}delayTaps_=on;}
    /** A double tap closed the map (once). */
    bool takeClose() noexcept {const bool c=close_;close_=false;return c;}
    void down(long long t,float x,float y,float vw,float vh) noexcept {
        if(state_!=State::Idle)return;
        x0_=fx_=lastX_=x;y0_=fy_=lastY_=y;w_=vw;h_=vh;downAt_=lastT_=t;vx_=vy_=zr_=0;rx_=ry_=0;buttons_=0;
        second_=pending() && t-(pendUntil_-DoubleTapMs)<=DoubleTapMs; // (3.11: a second down in the window; the waypoint waits for its lift)
        state_=State::Armed;next_=None;
    }
    void move(long long t,float x,float y) noexcept {
        if(state_==State::Armed && std::hypot(x-x0_,y-y0_)>TapSlop*std::min(w_,h_)){pendUntil_=NoPend;second_=false; // (3.11: a drag drops a waiting waypoint)
            state_=State::Drag;buttons_=Touch;lastX_=x;lastY_=y;lastT_=t;next_=t+TickMs;return;}
        if(state_==State::Drag){drag(t,x,y);next_=t+TickMs;}
    }
    void pointerDown(long long t,float x,float y) noexcept {
        if(state_!=State::Armed && state_!=State::Drag)return;
        pendUntil_=NoPend;second_=false; // (3.11: a pinch drops a waiting waypoint)
        state_=State::Pinch;buttons_=Touch;px2_=x;py2_=y;lastT_=t;zr_=0;rx_=Pinch;ry_=0;
        lastLogD_=std::log(std::max(1.0,double(std::hypot(px2_-fx_,py2_-fy_))));next_=t+TickMs;
    }
    void move2(long long t,float x1,float y1,float x2,float y2) noexcept {
        if(state_!=State::Pinch)return;
        fx_=x1;fy_=y1;px2_=x2;py2_=y2;zoom(t);next_=t+TickMs;
    }
    void pointerUp(long long) noexcept {if(state_==State::Pinch){rest();state_=State::Ignored;}}
    void up(long long t) noexcept {
        switch(state_) {
        case State::Armed:
            if(delayTaps_ && second_){second_=false;pendUntil_=NoPend;state_=State::Idle;next_=None; // (3.11: the second tap)
                if(t-downAt_<=TapMs)close_=true;       // (a double tap: the close, no waypoint)
                break;}                                // (a hold: neither; the long press is the gesture's)
            if(delayTaps_ && t-downAt_<=TapMs) { // (3.11: the waypoint waits for a possible second tap)
                pendRx_=int(std::lround((x0_-w_*0.5f)*1280.0f/w_));pendRy_=int(std::lround((y0_-h_*0.5f)*720.0f/h_));pendUntil_=t+DoubleTapMs;
                state_=State::Idle;next_=pendUntil_;break;}
            if(t-downAt_<=TapMs && t>=lockUntil_) {
                state_=State::Tap;buttons_=Touch|Request;lockUntil_=t+LockMs;
                rx_=int(std::lround((x0_-w_*0.5f)*1280.0f/w_));ry_=int(std::lround((y0_-h_*0.5f)*720.0f/h_));next_=t+PulseMs;
            } else {state_=State::Idle;next_=None;}
            break;
        case State::Drag:case State::Pinch:case State::Ignored:rest();state_=State::Idle;break;
        default:break;
        }
    }
    void tick(long long t) noexcept {
        if(t<next_)return;
        if(state_==State::Idle && pending() && t>=pendUntil_) { // (3.11: no second tap came: the waypoint now)
            pendUntil_=NoPend;
            if(t>=lockUntil_){state_=State::Tap;buttons_=Touch|Request;lockUntil_=t+LockMs;rx_=pendRx_;ry_=pendRy_;next_=t+PulseMs;}
            else next_=None;
            return;
        }
        switch(state_) {
        case State::Drag:drag(t,lastX_,lastY_);next_=t+TickMs;break;
        case State::Pinch:zoom(t);next_=t+TickMs;break;
        case State::Tap:rest();state_=State::Idle;break;
        default:next_=None;break;
        }
    }
    void cancel() noexcept {rest();state_=State::Idle;pendUntil_=NoPend;second_=close_=false;}
private:
    static constexpr long long NoPend=std::numeric_limits<long long>::min();
    bool pending() const noexcept {return pendUntil_!=NoPend;}
    static int axis(float v) noexcept {return int(std::lround(32767.0f*std::clamp(v,-1.0f,1.0f)));}
    void drag(long long t,float x,float y) noexcept {
        const float dt=std::max(1.0f,float(t-lastT_)),a=std::min(1.0f,dt/PanSmoothMs);
        vx_+=a*((x-lastX_)*1000.0f/dt-vx_);vy_+=a*((y-lastY_)*1000.0f/dt-vy_);lastX_=x;lastY_=y;lastT_=t;
        const float speed=std::hypot(vx_,vy_);
        if(speed<PanStill){rx_=ry_=0;return;}
        const float k=(PanMin+(1.0f-PanMin)*std::min(1.0f,speed/PanFull))/speed;
        rx_=axis(-vx_*k);ry_=axis(vy_*k);
    }
    void zoom(long long t) noexcept {
        const double ld=std::log(std::max(1.0,double(std::hypot(px2_-fx_,py2_-fy_))));
        const float dt=std::max(1.0f,float(t-lastT_)),a=std::min(1.0f,dt/ZoomSmoothMs);
        zr_+=a*(float((ld-lastLogD_)*1000.0/dt)-zr_);lastLogD_=ld;lastT_=t;
        const int v=std::abs(zr_)<ZoomStill?0:int(std::lround(TriggerMin+(255-TriggerMin)*std::min(1.0f,std::abs(zr_)/ZoomFull)));
        rx_=Pinch;ry_=zr_>0?v:-v;
    }
    void rest() noexcept {buttons_=0;rx_=ry_=0;vx_=vy_=zr_=0;next_=None;}
    State state_=State::Idle;
    float x0_=0,y0_=0,fx_=0,fy_=0,px2_=0,py2_=0,w_=0,h_=0,vx_=0,vy_=0,zr_=0,lastX_=0,lastY_=0;double lastLogD_=0;
    long long downAt_=0,lastT_=0,next_=None,lockUntil_=std::numeric_limits<long long>::min();
    int rx_=0,ry_=0,buttons_=0;
    bool delayTaps_=false,second_=false,close_=false;int pendRx_=0,pendRy_=0;long long pendUntil_=NoPend; // (3.11: the waiting waypoint)
};

// The single-screen edition's long presses (QolGesture.java, the same values and state machine; frame() above instead of the
// letterbox). The physical pad always wins. iPad 235e: brought level with android-qol's QolGesture.java of 2026-10-24 ~18:00 - v701 (a
// long press with no zone = TOUCH | REQUEST with the stick at (CloseCode, CloseCode): back to the game from a pause screen, map_tap.h),
// the weapon wheel's measured centre (0.5, 0.413), and SPEC 3.6's latched wheels (a wheel's long press lifted without a slide keeps its
// button held; the next finger picks a slot inside the wheel's ring or cancels outside it; 10 s, a physical press, a context or pause-map
// change end it; while the physical pad holds a wheel's button a finger on its ring steers the right stick: the Thor's WheelTouch rule).
class Gesture {
public:
    static constexpr int LB=0x100,Left=0x0004,Down=0x0002,Start=0x0010,A=0x1000,B=0x2000,Request=MapTouch::Request,Touch=MapTouch::Touch;
    static constexpr int WheelButtons=LB|Left|Down;
    static constexpr int Close=-32768; // (v701: map_tap.h CloseCode)
    static constexpr long long LongMs=400;
    // SPEC 3.11 (owner 2026-10-24 ~21:10: "for map touch make the hold time shorter, and make it work with double tap"): the MAP zone
    // fires at 250 ms (the wheels and the no-zone close stay 400 ms); two taps on it, the second down within 300 ms of the first up (each
    // shorter than the hold, within the slop) fire the same request at the second down - only while the pause map is NOT shown.
    static constexpr long long MapLongMs=250,MapDoubleMs=300;
    static constexpr float LongSlop=MapTouch::TapSlop;
    static constexpr long long OpenMs=250,MinStickMs=133,CloseGapMs=66;
    static constexpr float Dead=0.10f;
    static constexpr long long RequestMs=150,RequestLockMs=700,FallbackStartMs=100,FallbackAMs=450,PulseMs=120;
    static constexpr int PadDeadzone=8689;
    static constexpr long long LatchMs=10000; // (SPEC 3.6: a latched wheel without a finger lets go after 10 s)
    // SPEC 3.6 v2 (two of the owner's latches were cancelled by his next finger): a near miss (up to NearMiss x the ring's outer radius)
    // selects the slot in its direction; farther, the stick side (x < StickSide of the view) does nothing and elsewhere only a quick tap
    // (lifted within LatchTapMs, within the slop) cancels; the centre still cancels at once.
    static constexpr float NearMiss=1.25f,StickSide=0.45f;static constexpr long long LatchTapMs=250;
    enum class Zone {None,Map,Weapon,Radio,Character};
    enum class State {Idle,Armed,Hold,Closing,Macro,Latched};
    struct Rect {float x0,y0,x1,y1;};
    struct Point {float x,y;};
    struct Ring {float inner,outer;};
    static constexpr Rect MapZone{0.0f,0.68f,0.25f,1.0f},WeaponZone{0.72f,0.0f,1.0f,0.25f},CharacterZone{0.80f,0.70f,1.0f,1.0f},RadioZone{0.0f,0.30f,0.12f,0.62f};
    // ios-touchpad (iPad 235; SPEC v2 3.3, QolGesture.java PAD_*): while the universal touch controls show, only the radar and the weapon icon
    // are zones (RADIO and SWITCH are buttons there). padZones(true) selects these.
    // (QolGesture.java PAD_MAP_ZONE since 2026-10-24 ~21:55, main: "a thumb resting on the stick must never open the map": with the pad shown
    //  the minimap zone is only the radar's drawn box - not the corner below / left of it, not the strip above)
    static constexpr Rect PadMapZone{0.02f,0.71f,0.22f,0.97f},PadWeaponZone{0.75f,0.0f,1.0f,0.20f};
    // The wheels' centres (shares of the frame): the weapon wheel (640, 297) (measured on the Fold), the radio wheel (640, 330), the
    // character wheel (1151.5, 619); a latched wheel's tap ring {inner, outer} (shares of the frame's smaller side).
    static constexpr Point WeaponCentre{0.5f,0.413f},RadioCentre{0.5f,330.0f/720.0f},CharacterCentre{1151.5f/1280.0f,619.0f/720.0f};
    static constexpr Ring WeaponRing{0.08f,0.45f},RadioRing{0.08f,0.45f},CharacterRing{0.025f,0.20f};

    // The zone of a view point (shares of the HUD frame, clamped).
    static Zone zone(float x,float y,const Frame& f,bool pad=false) noexcept {
        if(!f.valid() || !std::isfinite(x) || !std::isfinite(y))return Zone::None;
        const float sx=f.gx(x)/1280.0f,sy=f.gy(y)/720.0f;
        if(pad)return in(sx,sy,PadMapZone)?Zone::Map:in(sx,sy,PadWeaponZone)?Zone::Weapon:Zone::None;
        if(in(sx,sy,CharacterZone))return Zone::Character;
        if(in(sx,sy,MapZone))return Zone::Map;
        if(in(sx,sy,WeaponZone))return Zone::Weapon;
        if(in(sx,sy,RadioZone))return Zone::Radio;
        return Zone::None;
    }
    static int button(Zone z) noexcept {return z==Zone::Weapon?LB:z==Zone::Radio?Left:z==Zone::Character?Down:0;}
    static Point centre(Zone z) noexcept {return z==Zone::Radio?RadioCentre:z==Zone::Character?CharacterCentre:WeaponCentre;}
    static Ring ring(Zone z) noexcept {return z==Zone::Radio?RadioRing:z==Zone::Character?CharacterRing:WeaponRing;}
    /** The wheel a pad's buttons hold (LB, then D-pad LEFT, then DOWN), or None. */
    static Zone wheelOf(int buttons) noexcept {return (buttons&LB)?Zone::Weapon:(buttons&Left)?Zone::Radio:(buttons&Down)?Zone::Character:Zone::None;}
    // The right stick for a frame point from the wheel centre (shares): false inside the dead disc (dead x the frame's smaller side).
    static bool direction(float x,float y,float fw,float fh,float cx,float cy,int& sx,int& sy,float dead=Dead) noexcept {
        if(!(fw>0) || !(fh>0) || !std::isfinite(x) || !std::isfinite(y))return false;
        const double dx=x-fw*cx,dy=y-fh*cy,r=std::hypot(dx,dy);
        if(r<dead*std::min(fw,fh))return false;
        sx=int(std::lround(32767*dx/r));sy=int(std::lround(-32767*dy/r));return true;
    }
    struct Pad {int buttons,rightX,rightY;};
    // The pad the game is given: the physical / on-screen state, this gesture's buttons OR'd, its right stick (or the map touch's
    // channel) while it pushes, unless the physical right stick is past PadDeadzone (v701: the close code is never replaced: no push).
    static Pad merge(const Gesture* g,int buttons,int rightX,int rightY) noexcept {
        if(!g)return {buttons,rightX,rightY};
        const bool physical=std::abs(rightX)>PadDeadzone || std::abs(rightY)>PadDeadzone;
        Pad p{buttons|g->buttons_,rightX,rightY};
        if(g->stick_ && (!physical || g->state_==State::Macro)){p.rightX=g->rx_;p.rightY=g->ry_;}
        if(g->nativeMap_ && g->map_.active() && !physical){p.buttons|=g->map_.buttons();p.rightX=g->map_.rightX();p.rightY=g->map_.rightY();}
        return p;
    }

    void nativeMap(bool on) noexcept {nativeMap_=on;if(!on)map_.cancel();}
    void padZones(bool on) noexcept {padZones_=on;} // (ios-touchpad: the universal controls show)
    void mapShown(bool on) noexcept {mapShownNow_=on;map_.delayTaps(on && nativeMap_);} // (SPEC 3.11: the library's bit 15; on the map double taps are the map's)
    bool padZones() const noexcept {return padZones_;}
    bool nativeMap() const noexcept {return nativeMap_;}
    State state() const noexcept {return state_;}
    Zone held() const noexcept {return held_;}
    const MapTouch& map() const noexcept {return map_;}
    int buttons() const noexcept {return buttons_;}
    bool stickOn() const noexcept {return stick_;}
    int rightX() const noexcept {return rx_;}
    int rightY() const noexcept {return ry_;}
    bool mapOpen() const noexcept {return mapOpen_;}
    long long due() const noexcept {return std::min(next_,nativeMap_?map_.due():None);}
    // Whether the gesture puts anything into the pad (for the publisher: republish while true and at each change).
    bool active() const noexcept {return buttons_!=0 || stick_ || (nativeMap_ && map_.active());}
    bool latched() const noexcept {return state_==State::Latched;}
    /** SPEC 3.8: a wheel of this gesture is held, latched or selecting (the sticks are off meanwhile). */
    bool wheelOpen() const noexcept {return button(held_)!=0 && (state_==State::Hold || state_==State::Closing || state_==State::Latched);}
    /** Whether a new finger is the wheel's (the view gives it to this gesture before the on-screen pad): a latched wheel, or a wheel's
     *  button the physical pad holds. */
    bool wantsFinger() const noexcept {return !finger_ && (state_==State::Latched || (state_==State::Idle && wheelOf(padNow_)!=Zone::None));}
    /** The on-screen pad's wheel button was held and lifted without a slide: its button stays held here (Latched). */
    void latch(long long t,Zone z) noexcept {
        if(!button(z) || (state_!=State::Idle && state_!=State::Latched && state_!=State::Hold) || (state_==State::Hold && finger_))return;
        rest();held_=z;buttons_=button(z);state_=State::Latched;next_=t+LatchMs;padTap_=false;relative_=false;dead_=Dead;
    }
    /** A latched wheel lets go at once, without a push (the game keeps the slot it shows): the timeout, a context or pause-map change. */
    void endLatch(long long) noexcept {if(state_==State::Latched){rest();buttons_=0;state_=State::Idle;held_=Zone::None;next_=None;}}

    // The physical pad's buttons (every publish). A button pressed since the finger went down ends the gesture (the pad wins).
    void pad(long long,int physicalButtons) noexcept {
        const int fresh=physicalButtons&~padNow_;padNow_=physicalButtons;
        if(physicalButtons&(Start|B))mapOpen_=false;
        if(padTap_ && !(physicalButtons&button(held_))){rest();padTap_=false;state_=State::Idle;next_=None;held_=Zone::None;if(finger_)ignored_=true;return;} // (the pad selected)
        if(!fresh || (!finger_ && state_!=State::Hold && state_!=State::Closing && state_!=State::Latched))return;
        if(finger_ && !(physicalButtons&~padAtDown_))return;
        if(state_==State::Hold || state_==State::Closing || state_==State::Latched){rest();buttons_=0;state_=State::Idle;next_=None;held_=Zone::None;padTap_=false;}
        else if(state_==State::Armed){state_=State::Idle;next_=None;}
        if(mapFinger_){map_.cancel();mapFinger_=false;}
        if(finger_)ignored_=true;
    }
    void down(long long t,float x,float y,const Frame& f) noexcept {
        if(finger_)return; // (one finger at a time; a second one is the map's pinch: pointerDown)
        finger_=true;lifted_=false;slid_=false;mapFinger_=false;
        fw_=f.valid()?f.fw():0;fh_=f.valid()?f.fh():0;fx_=x0_=f.fx(x);fy_=y0_=f.fy(y);downAt_=t;padAtDown_=padNow_;
        if(fw_>0 && state_==State::Latched) { // (SPEC 3.6 v2: the latched wheel's finger: a slot, a near miss, the centre, or far)
            ignored_=false;const float rel=ringRel(held_);++latchTouches_;lastLatchRel_=rel;
            if(rel<0){lastLatchOutcome_='C';endLatch(t);ignored_=true;return;}          // (the centre: closed without a change)
            if(rel<=NearMiss){lastLatchOutcome_=rel<=1.0f?'S':'N';steer(t,false);return;} // (a slot, or a near miss: its direction)
            farStickSide_=x<StickSide*f.w;farMoved_=false;latchFar_=true;lastLatchOutcome_=farStickSide_?'I':'F';return; // (decided at the lift)
        }
        if(fw_>0 && state_==State::Idle && wheelOf(padNow_)!=Zone::None) { // (the pad holds a wheel's button: a finger on its ring steers)
            held_=wheelOf(padNow_);ignored_=!onRing(held_);
            if(ignored_){held_=Zone::None;return;}
            buttons_=0;steer(t,true);return;
        }
        if(state_!=State::Idle || !(fw_>0)){ignored_=true;return;} // (a gesture still ending)
        ignored_=false;
        zone_=zone(x,y,f,padZones_);state_=State::Armed;
        next_=zone_==Zone::None && !nativeMap_?None:t+(zone_==Zone::Map?MapLongMs:LongMs); // (v701: no zone = the close; SPEC 3.11: MAP 250 ms)
        if(zone_==Zone::Map && !mapShownNow_ && t-mapTapUp_<=MapDoubleMs){mapTapUp_=-(1LL<<40);fire(t);return;} // (SPEC 3.11: the double tap)
        if(nativeMap_ && map_.state()==MapTouch::State::Idle){map_.down(t,fx_,fy_,fw_,fh_);mapFinger_=true;}
    }
    void move(long long t,float x,float y,const Frame& f) noexcept {
        if(!finger_ || ignored_)return;
        fx_=f.fx(x);fy_=f.fy(y);
        if(latchFar_){if(std::hypot(fx_-x0_,fy_-y0_)>LongSlop*std::min(fw_,fh_))farMoved_=true;return;} // (a far finger on a latched wheel steers nothing)
        if(mapFinger_)map_.move(t,fx_,fy_);
        switch(state_) {
        case State::Armed:if(std::hypot(fx_-x0_,fy_-y0_)>LongSlop*std::min(fw_,fh_)){state_=State::Idle;next_=None;}break; // (a drag)
        case State::Hold:
            if(!slid_ && std::hypot(fx_-hx_,fy_-hy_)>LongSlop*std::min(fw_,fh_))slid_=true;
            if(slid_ && !lifted_ && t>=openAt_+OpenMs)aim(t);
            break;
        default:break;
        }
    }
    void pointerDown(long long t,float x,float y,const Frame& f) noexcept {
        if(!finger_ || ignored_)return;
        if(state_==State::Armed){state_=State::Idle;next_=None;}
        if(mapFinger_)map_.pointerDown(t,f.fx(x),f.fy(y));
    }
    void move2(long long t,float x1,float y1,float x2,float y2,const Frame& f) noexcept {
        if(!finger_ || ignored_ || !mapFinger_)return;
        map_.move2(t,f.fx(x1),f.fy(y1),f.fx(x2),f.fy(y2));
    }
    void pointerUp(long long t) noexcept {if(mapFinger_)map_.pointerUp(t);}
    void up(long long t) noexcept {
        if(!finger_)return;
        finger_=false;
        if(ignored_){ignored_=false;return;}
        if(latchFar_){latchFar_=false; // (3.6 v2: only a quick tap away from the stick side cancels; anything else leaves the wheel latched)
            if(!farStickSide_ && !farMoved_ && t-downAt_<=LatchTapMs){lastLatchOutcome_='X';endLatch(t);}
            return;}
        lifted_=true;
        if(mapFinger_){map_.up(t);mapFinger_=false; // (a short tap: the map's waypoint tap; off the map the library does nothing)
            if(map_.takeClose() && t>=lockUntil_){lockUntil_=t+RequestLockMs;startMacro(t,{Touch|Request,int(RequestMs)});stick_=true;rx_=ry_=Close;
                state_=State::Macro;return;}} // (3.11: a double tap on the open map = the close, as v701's long press)
        switch(state_) {
        case State::Armed:
            if(zone_==Zone::Map)mapTapUp_=t; // (SPEC 3.11: a tap on the minimap, shorter than the hold, within the slop: a double tap's first)
            state_=State::Idle;next_=None;break;
        case State::Hold:
            if(!slid_ && !padTap_){latch(t,held_);break;} // (SPEC 3.6: no slide = the wheel stays open for a tap)
            if(t<openAt_+OpenMs)break;
            {const long long e=pushEnds(t);if(e<=t)close(t);else next_=e;}
            break;
        default:break;
        }
    }
    void tick(long long t) noexcept {
        if(nativeMap_ && t>=map_.due())map_.tick(t);
        if(t<next_)return;
        switch(state_) {
        case State::Armed:fire(t);break;
        case State::Hold:
            if(!lifted_){if(slid_)aim(t);next_=None;break;}
            if(slid_ && !stick_){aim(t);if(stick_){next_=stickSince_+MinStickMs;break;}}
            close(t);break;
        case State::Closing:buttons_=0;state_=State::Idle;next_=None;held_=Zone::None;padTap_=false;if(finger_)ignored_=true;break;
        case State::Latched:endLatch(t);break; // (LatchMs without a finger)
        case State::Macro:
            macroAt_+=2;
            if(macroAt_>=macro_.size()){buttons_=0;rest();state_=State::Idle;next_=None;macro_.clear();held_=Zone::None;if(finger_)ignored_=true;break;}
            buttons_=macro_[macroAt_];next_=t+macro_[macroAt_+1];break;
        default:next_=None;break;
        }
    }
    void cancel() noexcept {rest();map_.cancel();buttons_=0;state_=State::Idle;held_=Zone::None;finger_=ignored_=lifted_=mapFinger_=padTap_=latchFar_=false;next_=None;macro_.clear();}
    // (QolTouch logs each latched-wheel touch: where, and what it did - S slot, N near miss, C centre, I stick side, F far, X far tap)
    unsigned latchTouches() const noexcept {return latchTouches_;}
    float lastLatchRel() const noexcept {return lastLatchRel_;}
    char lastLatchOutcome() const noexcept {return lastLatchOutcome_;}
private:
    static bool in(float sx,float sy,const Rect& z) noexcept {return sx>=z.x0 && sx<=z.x1 && sy>=z.y0 && sy<=z.y1;}
    // The finger's distance from the wheel's centre as a share of its ring's outer radius; -1 inside the inner (centre) radius.
    float ringRel(Zone z) const noexcept {
        const Point c=centre(z);const Ring g=ring(z);
        const double d=std::hypot(fx_-fw_*c.x,fy_-fh_*c.y),m=std::min(fw_,fh_);
        return d<g.inner*m?-1.0f:float(d/(g.outer*m));
    }
    bool onRing(Zone z) const noexcept {
        const Point c=centre(z);const Ring g=ring(z);
        const double d=std::hypot(fx_-fw_*c.x,fy_-fh_*c.y),m=std::min(fw_,fh_);
        return d>=g.inner*m && d<=g.outer*m;
    }
    // The wheel is already open: this finger steers from its first point (Hold, slid), the pad's tap or a latched wheel's.
    void steer(long long t,bool pad) noexcept {
        state_=State::Hold;padTap_=pad;slid_=true;lifted_=false;openAt_=t-OpenMs;relative_=false;dead_=ring(held_).inner;hx_=fx_;hy_=fy_;next_=None;aim(t);
    }
    void fire(long long t) {
        if(mapFinger_){map_.cancel();mapFinger_=false;}
        held_=zone_;hx_=fx_;hy_=fy_;
        if(zone_==Zone::None) { // v701: back to the game from a pause screen (the library ignores it in the game)
            if(t<lockUntil_){state_=State::Idle;next_=None;ignored_=true;return;}
            lockUntil_=t+RequestLockMs;startMacro(t,{Touch|Request,int(RequestMs)});
            stick_=true;rx_=ry_=Close;return;
        }
        if(zone_==Zone::Map) {
            if(t<lockUntil_){state_=State::Idle;next_=None;ignored_=true;return;}
            lockUntil_=t+RequestLockMs;
            if(nativeMap_)startMacro(t,{Request,int(RequestMs)});
            else if(mapOpen_){mapOpen_=false;startMacro(t,{Start,int(PulseMs),0,int(PulseMs)});}
            else {mapOpen_=true;startMacro(t,{Start,int(FallbackStartMs),A,int(FallbackAMs),0,int(PulseMs)});}
            return;
        }
        state_=State::Hold;buttons_=button(zone_);openAt_=t;rest();next_=t+OpenMs;relative_=padZones_ && zone_==Zone::Character;dead_=Dead;padTap_=false;
    }
    void startMacro(long long t,std::vector<int> steps) {macro_=std::move(steps);macroAt_=0;state_=State::Macro;buttons_=macro_[0];next_=t+macro_[1];}
    void aim(long long t) noexcept {
        const Point c=centre(held_);int sx=0,sy=0;
        const bool ok=relative_?direction(fx_-hx_+fw_*0.5f,fy_-hy_+fh_*0.5f,fw_,fh_,0.5f,0.5f,sx,sy):direction(fx_,fy_,fw_,fh_,c.x,c.y,sx,sy,dead_);
        if(!ok){rest();return;} // (the stick rests: the game keeps its highlight)
        if(!stick_)stickSince_=t;
        stick_=true;rx_=sx;ry_=sy;
    }
    void rest() noexcept {stick_=false;rx_=ry_=0;stickSince_=-1;}
    long long pushEnds(long long t) const noexcept {return stick_?std::max(t,stickSince_+MinStickMs):t;}
    void close(long long t) noexcept {rest();state_=State::Closing;next_=t+CloseGapMs;}

    MapTouch map_;
    bool nativeMap_=false,padZones_=false,mapShownNow_=false;
    long long mapTapUp_=-(1LL<<40); // (SPEC 3.11: the last minimap tap's lift)
    bool latchFar_=false,farStickSide_=false,farMoved_=false;unsigned latchTouches_=0;float lastLatchRel_=0;char lastLatchOutcome_=0; // (3.6 v2)
    State state_=State::Idle;
    Zone zone_=Zone::None,held_=Zone::None;
    bool finger_=false,ignored_=false,lifted_=false,slid_=false,mapFinger_=false,mapOpen_=false,padTap_=false,relative_=false;
    float dead_=Dead; // (the steering's dead disc: Dead, or a latched wheel's inner ring)
    float fx_=0,fy_=0,fw_=0,fh_=0,x0_=0,y0_=0,hx_=0,hy_=0;
    long long downAt_=0,openAt_=0,stickSince_=-1,next_=None,lockUntil_=std::numeric_limits<long long>::min();
    int buttons_=0,rx_=0,ry_=0,padNow_=0,padAtDown_=0;bool stick_=false;
    std::vector<int> macro_;std::size_t macroAt_=0;
};

} // namespace recomp::apple::qol
