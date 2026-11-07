// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-duo (2026-10-24): the Thor edition's two touch gestures, ported line for line from the Thor APK tree (plain Java, no Android
// types) to C++ for the iOS Duo edition's app (Main.mm, RECOMP_DUO_EDITION). No UIKit types: the host test runs it
// (ios-duo-edition/tests/duo_gesture_test.cpp mirrors WheelGestureTest.java and MapTouchTest.java case for case).
//   WheelGesture  local-analysis/thor-edition/apk-tree/android/app/src/debug/java/dev/recompbootstrap/smoke/WheelGesture.java
//                 (the lower screen: the weapon / radio / character wheels while their button is held, the map-tap request, Resume)
//   MapTouch      .../MapTouch.java (the top screen while split: drag pans, pinch zooms, tap = waypoint, through the right stick
//                 with the touchpad bit TOUCH; the library passes it to the game only while its pause full map is on screen)
// The design notes, constants and their measured sources are in the Java files; only behaviour that differs is commented here.
// Java's Math.round(float) is floor(x + 0.5) as int, Math.round(double) floor(x + 0.5) as long: jround() keeps those results.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

namespace recomp::duo {
inline int jround(float v) noexcept {return int(std::floor(v+0.5f));}
inline long long jround(double v) noexcept {return (long long)std::floor(v+0.5);}
constexpr long long None=LLONG_MAX;

// The pad the game is given (as Java's int[] {buttons, rx, ry, lx, ly, lt, rt}).
struct Pad {int buttons=0,rightX=0,rightY=0,leftX=0,leftY=0,leftTrigger=0,rightTrigger=0;};

class WheelGesture {
public:
    static constexpr int LB=0x100,LEFT=0x0004,DOWN=0x0002;
    static constexpr float CHAR_CX=0.75f;
    static constexpr long long HOLD_MS=200,OPEN_MS=250,MIN_STICK_MS=133,CLOSE_GAP_MS=66,NONE=None;
    static constexpr float DEAD=0.10f;
    static constexpr bool OWN_ENABLED=false;
    static constexpr bool MAP_TAP=true;
    static constexpr int START=0x0010,A=0x1000,B=0x2000;
    static constexpr long long TAP_MS=300,PULSE_MS=120,MENU_OPEN_MS=250;
    static constexpr int REQUEST=0x0800;
    static constexpr long long REQUEST_MS=150,REQUEST_LOCK_MS=700,FALLBACK_START_MS=100,FALLBACK_A_MS=450;
    static constexpr float TAP_SLOP=0.04f;
    static constexpr bool MAP_TOUCH=false; // (v667: the map's touch controls are on the TOP screen - MapTouch)
    static constexpr int TOUCH=0x0400;
    static constexpr float PAN_FULL=320.f,PAN_STILL=15.f,PAN_MIN=0.30f,PAN_SMOOTH_MS=50.f,ZOOM_FULL=2.0f,ZOOM_STILL=0.2f,ZOOM_SMOOTH_MS=60.f;
    static constexpr int TRIGGER_MIN=80;
    static constexpr long long PAN_TICK_MS=16;
    static constexpr float RESUME_W=900,RESUME_H=92;
    enum class State {IDLE,ARMING,OWN,STEER,CLOSING,MACRO,DRAG,PINCH};
    // (2026-10-25, the Thor mirror: WheelGesture.java of 09:20 EDT) v815 the phone's taps / swipes, v828 a tap on the phone's item
    // (PHONE_TAP_AT + the packed position), v820 Quick Save (SAVE_CODE in saveRect), v832 the trainer button's box beside it.
    static constexpr int PHONE_TAP=-32766,PHONE_SWIPE=-32765,PHONE_A=0,PHONE_BACK=1,SWIPE_UP=1,SWIPE_RIGHT=2,SWIPE_DOWN=3,SWIPE_LEFT=4;
    static constexpr long long SWIPE_MS=600,PHONE_LOCK_MS=250;static constexpr float SWIPE_MIN=0.06f;
    static constexpr float PHONE_ASPECT=237.f/343.f,PHONE_MARGIN_1080=56.f,PHONE_BAR_SHARE=0.1f,PHONE_BACK_SHARE=0.4f;
    static constexpr int PHONE_TAP_AT=-32763,SAVE_CODE=-32764;
    static constexpr float SAVE_W_1080=88.f,SAVE_H_1080=88.f,SAVE_RIGHT_1080=32.f,SAVE_BOTTOM_1080=184.f,SAVE_PAD_1080=8.f,SAVE_MIN_SCALE=0.95f,SAVE_GAP_1080=16.f;
    struct Box {bool ok=false;float x0=0,y0=0,x1=0,y1=0;};
    void phoneTouch(bool on) noexcept {phoneTouch_=on;}
    void phoneTapAt(bool on) noexcept {phoneTapAt_=on;}
    void saveTouch(bool on) noexcept {saveTouch_=on;}
    static int phoneTapPacked(float x,float y,float w,float h) noexcept {
        const int a=jround(std::max(0.f,std::min(1.f,x/w))*255.f),b=jround(std::max(0.f,std::min(1.f,y/h))*255.f);
        return int(std::int16_t(std::uint16_t((a<<8)|b)));
    }
    static Box phoneRect(float w,float h) noexcept {
        const float m=float(jround(PHONE_MARGIN_1080*h/1080.f));
        float ph=h-2.f*m,pw=float(jround(ph*PHONE_ASPECT));
        if(pw>w-2.f*m){pw=w-2.f*m;ph=float(jround(pw/PHONE_ASPECT));}
        const float x0=std::floor((w-pw)*0.5f),y0=std::floor((h-ph)*0.5f);
        return {true,x0,y0,x0+pw,y0+ph};
    }
    static int phoneTapKey(float x,float y,float w,float h) noexcept {
        const Box r=phoneRect(w,h);
        const bool bar=y>=r.y1-(r.y1-r.y0)*PHONE_BAR_SHARE && y<r.y1+(r.y1-r.y0)*0.05f,right=x>=r.x1-(r.x1-r.x0)*PHONE_BACK_SHARE;
        return bar && right?PHONE_BACK:PHONE_A;
    }
    static int swipeDir(float x0,float y0,float x1,float y1,float w,float h) noexcept {
        const float dx=x1-x0,dy=y1-y0;
        if(std::hypot(double(dx),double(dy))<double(SWIPE_MIN*std::min(w,h)))return 0;
        return std::fabs(dx)>=std::fabs(dy)?(dx>0?SWIPE_RIGHT:SWIPE_LEFT):(dy>0?SWIPE_DOWN:SWIPE_UP);
    }
    static Box saveRect(float w,float h) noexcept {
        if(!(w>0) || !(h>0))return {};
        const float s=std::min(1.f,h/1080.f);
        if(s<SAVE_MIN_SCALE)return {};
        const float x1=w-float(jround(SAVE_RIGHT_1080*s)),x0=x1-float(jround(SAVE_W_1080*s)),y1=h-float(jround(SAVE_BOTTOM_1080*s)),y0=y1-float(jround(SAVE_H_1080*s));
        if(x0<0 || y0<0)return {};
        const float p=float(jround(SAVE_PAD_1080*s));
        return {true,x0-p,y0-p,x1+p,y1+p};
    }
    static Box trainerBox(float w,float h) noexcept {
        const Box s=saveRect(w,h);if(!s.ok)return {};
        const float k=std::min(1.f,h/1080.f),p=float(jround(SAVE_PAD_1080*k)),gap=float(jround(SAVE_GAP_1080*k)),side=float(jround(SAVE_W_1080*k));
        const float x1=s.x0+p-gap,y0=s.y0+p;
        return {true,x1-side,y0,x1,y0+side};
    }
    static bool onSave(float x,float y,float w,float h) noexcept {const Box r=saveRect(w,h);return r.ok && x>=r.x0 && x<r.x1 && y>=r.y0 && y<r.y1;}

    void nativeMap(bool on) noexcept {nativeMap_=on;}
    // ios-duo (2026-10-24): the map tap only from a touch that went down inside this rectangle (the lower screen's map; the Duo's
    // on-screen controls lie round it, so a stray tap between them does nothing). Unset (x1 <= x0) = anywhere, as on the Thor.
    void tapZone(float x0,float y0,float x1,float y1) noexcept {zone[0]=x0;zone[1]=y0;zone[2]=x1;zone[3]=y1;}
    bool inTapZone(float x,float y) const noexcept {return !(zone[2]>zone[0]) || (x>=zone[0] && x<zone[2] && y>=zone[1] && y<zone[3]);}
    bool nativeMap() const noexcept {return nativeMap_;}
    static bool onResume(float x,float y,float w,float h) noexcept {
        if(!(w>0) || !(h>0))return false;
        const float s=std::min(1.f,std::min(w/(RESUME_W+80.f),h/(RESUME_H*3.f)));
        const float bw=float(jround(RESUME_W*s)),bh=float(jround(RESUME_H*s)),x0=float(jround((w-bw)*0.5f)),y0=float(jround((h-bh)*0.5f));
        return x>=x0 && x<x0+bw && y>=y0 && y<y0+bh;
    }
    int leftX() const noexcept {return lx;}
    int leftY() const noexcept {return ly;}
    int leftTrigger() const noexcept {return lt;}
    int rightTrigger() const noexcept {return rt;}
    State state() const noexcept {return state_;}
    int buttons() const noexcept {return buttons_;}
    bool stickOn() const noexcept {return stick;}
    int rightX() const noexcept {return rx;}
    int rightY() const noexcept {return ry;}
    long long due() const noexcept {return next;}
    bool mapOpen() const noexcept {return mapOpen_;}
    void pad(int physicalButtons) noexcept {if((physicalButtons&(START|B))!=0)mapOpen_=false;}

    // The right stick for a finger at (x, y) in a w x h view: {x, y} (y up, full deflection); false inside the dead disc.
    static bool direction(float x,float y,float w,float h,int out[2],float shareX=0.5f) noexcept {
        if(!(w>0) || !(h>0) || !std::isfinite(x) || !std::isfinite(y))return false;
        const double dx=double(x)-double(w*shareX),dy=double(y)-double(h)*0.5,r=std::hypot(dx,dy);
        if(r<double(DEAD*std::min(w,h)))return false;
        out[0]=int(jround(32767*dx/r));out[1]=int(jround(-32767*dy/r));
        return true;
    }
    static int mask(bool weaponMain,bool radioMain) noexcept {return (weaponMain?0:LB)|(radioMain?0:LEFT);}
    static int mask(bool weaponMain,bool radioMain,bool characterMain) noexcept {return mask(weaponMain,radioMain)|(characterMain?0:DOWN);}
    static float originX(int buttons,int mask) noexcept {const int b=buttons&mask;return b==DOWN?CHAR_CX:0.5f;}
    void origin(float shareX) noexcept {ox=shareX;}
    static bool held(int buttons,int mask) noexcept {return (buttons&mask)!=0;}
    static int slot(int sx,int sy,int n) noexcept {
        if(n<=0 || (sx==0 && sy==0))return -1;
        double a=std::atan2(double(sx),double(sy));
        if(a<0)a+=2*M_PI;
        return int(std::floor(a/(2*M_PI/n)+0.5))%n;
    }
    static Pad merge(const WheelGesture* g,const Pad& p) noexcept {
        if(!g)return p;
        const bool drag=g->state_==State::DRAG,pinch=g->state_==State::PINCH;
        Pad o;o.buttons=p.buttons|g->buttons_;o.rightX=g->stick?g->rx:p.rightX;o.rightY=g->stick?g->ry:p.rightY;
        o.leftX=drag?g->lx:pinch?0:p.leftX;o.leftY=drag?g->ly:pinch?0:p.leftY;
        o.leftTrigger=pinch?g->lt:p.leftTrigger;o.rightTrigger=pinch?g->rt:p.rightTrigger;
        return o;
    }

    void down(long long t,float x,float y,float w,float h,bool physicalLB) noexcept {
        if(finger)return;
        finger=true;fx=x;fy=y;fw=w;fh=h;lifted=false;downAt=t;dx0=x;dy0=y;moved=false;
        if(state_!=State::IDLE){ignored=true;return;}
        ignored=false;
        if(physicalLB){state_=State::STEER;next=NONE;aim(t);}
        else {state_=State::ARMING;next=OWN_ENABLED?t+HOLD_MS:NONE;}
    }
    void move(long long t,float x,float y,float w,float h) noexcept {
        if(!finger || ignored)return;
        fx=x;fy=y;fw=w;fh=h;
        if(std::hypot(double(x-dx0),double(y-dy0))>double(TAP_SLOP*std::min(w,h)))moved=true;
        if(state_==State::ARMING && moved && nativeMap_ && MAP_TOUCH){state_=State::DRAG;buttons_=TOUCH;touchRest();lastX=x;lastY=y;lastMoveT=t;next=t+PAN_TICK_MS;return;}
        if(state_==State::DRAG){drag(t,x,y);next=t+PAN_TICK_MS;return;}
        if(state_==State::STEER || (state_==State::OWN && t>=openAt+OPEN_MS))aim(t);
    }
    void pointerDown(long long t,float x,float y) noexcept {
        if(!finger || ignored || !nativeMap_ || !MAP_TOUCH || (state_!=State::ARMING && state_!=State::DRAG))return;
        state_=State::PINCH;buttons_=TOUCH;touchRest();moved=true;px2=x;py2=y;lastMoveT=t;
        lastLogD=std::log(std::max(1.0,std::hypot(double(x-fx),double(y-fy))));next=t+PAN_TICK_MS;
    }
    void move2(long long t,float x1,float y1,float x2,float y2) noexcept {
        if(state_!=State::PINCH || ignored)return;
        fx=x1;fy=y1;px2=x2;py2=y2;zoom(t,x1,y1,x2,y2);next=t+PAN_TICK_MS;
    }
    void pointerUp(long long) noexcept {
        if(state_!=State::PINCH)return;
        touchRest();buttons_=0;state_=State::IDLE;next=NONE;if(finger)ignored=true;
    }
    void up(long long t) noexcept {
        if(!finger)return;
        finger=false;
        if(ignored){ignored=false;return;}
        lifted=true;
        switch(state_){
            case State::DRAG:case State::PINCH:touchRest();buttons_=0;state_=State::IDLE;next=NONE;break;
            case State::ARMING:
                if(phoneTouch_ && nativeMap_ && t>=lockUntil && t-downAt<=SWIPE_MS && moved) { // v815: a swipe (the phone's D-pad)
                    const int dir=swipeDir(dx0,dy0,fx,fy,fw,fh);
                    if(dir!=0){lockUntil=t+PHONE_LOCK_MS;phoneMacro(t,PHONE_SWIPE,dir);}else{state_=State::IDLE;next=NONE;}
                } else if(MAP_TAP && !moved && t-downAt<=TAP_MS && t>=lockUntil) {
                    // (ios-duo: the map-rect zone limits only the plain map tap; Quick Save and the phone take their own rectangles, as on the Thor)
                    if(saveTouch_ && nativeMap_ && onSave(dx0,dy0,fw,fh)){lockUntil=t+REQUEST_LOCK_MS;phoneMacro(t,SAVE_CODE,0);} // (v820)
                    else if(phoneTouch_ && nativeMap_){lockUntil=t+PHONE_LOCK_MS;const int key=phoneTapKey(dx0,dy0,fw,fh); // (v815; v828)
                        if(phoneTapAt_ && key==PHONE_A)phoneMacro(t,PHONE_TAP_AT,phoneTapPacked(dx0,dy0,fw,fh));else phoneMacro(t,PHONE_TAP,key);}
                    else if(!inTapZone(dx0,dy0)){state_=State::IDLE;next=NONE;}
                    else if(nativeMap_){lockUntil=t+REQUEST_LOCK_MS;
                        startMacro(t,{MAP_TOUCH && !onResume(dx0,dy0,fw,fh)?REQUEST|TOUCH:REQUEST,int(REQUEST_MS)});}
                    else if(mapOpen_){mapOpen_=false;startMacro(t,{START,int(PULSE_MS),0,int(PULSE_MS)});}
                    else {mapOpen_=true;startMacro(t,{START,int(FALLBACK_START_MS),A,int(FALLBACK_A_MS),0,int(PULSE_MS)});}
                } else {state_=State::IDLE;next=NONE;}
                break;
            case State::STEER:{const long long e=pushEnds(t);if(e<=t)idle();else next=e;break;}
            case State::OWN:
                if(t<openAt+OPEN_MS)break;
                {const long long e=pushEnds(t);if(e<=t)close(t);else next=e;}
                break;
            default:break;
        }
    }
    void physical(long long t,bool physicalLB) noexcept {
        if((state_==State::DRAG || state_==State::PINCH) && physicalLB){touchRest();buttons_=0;state_=State::ARMING;}
        if(state_==State::ARMING && physicalLB){state_=State::STEER;next=NONE;aim(t);}
        else if(state_==State::STEER && !physicalLB)idle();
    }
    void tick(long long t) noexcept {
        if(t<next)return;
        switch(state_){
            case State::ARMING:state_=State::OWN;openAt=t;buttons_=LB;rest();next=t+OPEN_MS;break;
            case State::OWN:
                if(!lifted){aim(t);next=NONE;break;}
                if(!stick){aim(t);if(stick){next=stickSince+MIN_STICK_MS;break;}}
                close(t);break;
            case State::STEER:idle();break;
            case State::CLOSING:buttons_=0;state_=State::IDLE;next=NONE;if(finger)ignored=true;break;
            case State::DRAG:drag(t,lastX,lastY);next=t+PAN_TICK_MS;break;
            case State::PINCH:zoom(t,fx,fy,px2,py2);next=t+PAN_TICK_MS;break;
            case State::MACRO:
                macroAt+=2;
                if(macroAt>=int(macro.size())){buttons_=0;state_=State::IDLE;next=NONE;macro.clear();rest();if(finger)ignored=true;break;} // (v815: the phone code's stick released)
                buttons_=macro[std::size_t(macroAt)];next=t+macro[std::size_t(macroAt)+1];break;
            default:next=NONE;break;
        }
    }
    void cancel() noexcept {rest();touchRest();buttons_=0;state_=State::IDLE;finger=ignored=lifted=false;next=NONE;macro.clear();}

private:
    bool nativeMap_=false,phoneTouch_=false,phoneTapAt_=false,saveTouch_=false;long long lockUntil=LLONG_MIN;
    float zone[4]{};
    float vx=0,vy=0,zr=0,lastX=0,lastY=0,px2=0,py2=0;long long lastMoveT=0;double lastLogD=0;int lx=0,ly=0,lt=0,rt=0;
    State state_=State::IDLE;
    bool finger=false,ignored=false,lifted=false;
    float fx=0,fy=0,fw=0,fh=0;
    long long openAt=0,stickSince=-1,next=NONE;
    int buttons_=0,rx=0,ry=0;bool stick=false;
    long long downAt=0;float dx0=0,dy0=0;bool moved=false,mapOpen_=false;
    std::vector<int> macro;int macroAt=0;
    float ox=0.5f;

    void phoneMacro(long long t,int code,int value){startMacro(t,{REQUEST|TOUCH,int(REQUEST_MS)});stick=true;rx=code;ry=value;}
    void startMacro(long long t,std::vector<int> steps){macro=std::move(steps);macroAt=0;state_=State::MACRO;buttons_=macro[0];next=t+macro[1];}
    static int axis(float v) noexcept {return jround(32767*std::max(-1.f,std::min(1.f,v)));}
    void touchRest() noexcept {lx=ly=lt=rt=0;vx=vy=zr=0;}
    void drag(long long t,float x,float y) noexcept {
        const float dt=std::max(1.f,float(t-lastMoveT)),a=std::min(1.f,dt/PAN_SMOOTH_MS);
        vx+=a*((x-lastX)*1000.f/dt-vx);vy+=a*((y-lastY)*1000.f/dt-vy);
        lastX=x;lastY=y;lastMoveT=t;
        const float speed=float(std::hypot(double(vx),double(vy)));
        if(speed<PAN_STILL){lx=ly=0;return;}
        const float k=(PAN_MIN+(1.f-PAN_MIN)*std::min(1.f,speed/PAN_FULL))/speed;
        lx=axis(-vx*k);ly=axis(vy*k);
    }
    void zoom(long long t,float x1,float y1,float x2,float y2) noexcept {
        const double d=std::max(1.0,std::hypot(double(x2-x1),double(y2-y1))),ld=std::log(d);
        const float dt=std::max(1.f,float(t-lastMoveT)),a=std::min(1.f,dt/ZOOM_SMOOTH_MS);
        zr+=a*(float((ld-lastLogD)*1000.0/dt)-zr);lastLogD=ld;lastMoveT=t;
        const int v=std::fabs(zr)<ZOOM_STILL?0:jround(TRIGGER_MIN+(255-TRIGGER_MIN)*std::min(1.f,std::fabs(zr)/ZOOM_FULL));
        rt=zr>0?v:0;lt=zr<0?v:0;
    }
    void aim(long long t) noexcept {
        int d[2];
        if(!direction(fx,fy,fw,fh,d,ox)){rest();return;}
        if(!stick)stickSince=t;
        stick=true;rx=d[0];ry=d[1];
    }
    void rest() noexcept {stick=false;rx=ry=0;stickSince=-1;}
    long long pushEnds(long long t) const noexcept {return stick?std::max(t,stickSince+MIN_STICK_MS):t;}
    void close(long long t) noexcept {rest();state_=State::CLOSING;next=t+CLOSE_GAP_MS;}
    void idle() noexcept {rest();state_=State::IDLE;next=NONE;if(finger)ignored=true;}
};

// EdgeSwipe.java (thor-radio v828): the lower screen's left-edge swipe that opens the trainer. A finger that lands in the strip EDGE of
// the width and moves right OPEN_DX of the width (more right than down, twice) within OPEN_MS = true once. One finger: the first in the strip.
class EdgeSwipe {
public:
    static constexpr float EDGE=0.035f,OPEN_DX=0.06f;static constexpr long long OPEN_MS=600; // (SideMenu.java)
    void view(float width) noexcept {w=std::max(1.f,width);}
    void down(long long id,long long t,float x,float y) noexcept {
        if(finger>=0)return;
        if(x<=EDGE*w){finger=id;x0=x;y0=y;t0=t;done=false;}
    }
    bool move(long long id,long long t,float x,float y) noexcept {
        if(id!=finger || done)return false;
        if(t-t0>OPEN_MS){finger=-1;return false;}
        const float dx=x-x0,dy=std::fabs(y-y0);
        if(dx>=OPEN_DX*w && dx>2.f*dy){done=true;return true;}
        return false;
    }
    void up(long long id) noexcept {if(id==finger)finger=-1;}
    void cancel() noexcept {finger=-1;}
private:
    long long finger=-1,t0=0;float x0=0,y0=0,w=1;bool done=false;
};

class MapTouch {
public:
    static constexpr int TOUCH=0x0400,REQUEST=0x0800;
    static constexpr int PINCH=-32768; // (Short.MIN_VALUE; map_tap.h PinchCode)
    static constexpr long long TAP_MS=300,PULSE_MS=150,LOCK_MS=700,TICK_MS=16,NONE=None;
    static constexpr float TAP_SLOP=0.03f,PAN_FULL=480.f,PAN_STILL=15.f,PAN_MIN=0.30f,PAN_SMOOTH_MS=50.f,ZOOM_FULL=2.0f,ZOOM_STILL=0.2f,ZOOM_SMOOTH_MS=60.f;
    static constexpr int TRIGGER_MIN=80;
    enum class State {IDLE,ARMED,DRAG,PINCH,TAP,IGNORED};

    State state() const noexcept {return state_;}
    long long due() const noexcept {return next;}
    int buttons() const noexcept {return buttons_;}
    int rightX() const noexcept {return rx;}
    int rightY() const noexcept {return ry;}
    bool active() const noexcept {return buttons_!=0;}
    static Pad merge(const MapTouch* g,const Pad& p) noexcept {
        if(!g || !g->active())return p;
        Pad o=p;o.buttons=p.buttons|g->buttons_;o.rightX=g->rx;o.rightY=g->ry;return o;
    }
    void down(long long t,float x,float y,float vw,float vh) noexcept {
        if(state_!=State::IDLE)return;
        x0=fx=lastX=x;y0=fy=lastY=y;w=vw;h=vh;downAt=lastT=t;vx=vy=zr=0;rx=ry=0;buttons_=0;
        state_=State::ARMED;next=NONE;
    }
    void move(long long t,float x,float y) noexcept {
        if(state_==State::ARMED && std::hypot(double(x-x0),double(y-y0))>double(TAP_SLOP*std::min(w,h))){state_=State::DRAG;buttons_=TOUCH;lastX=x;lastY=y;lastT=t;next=t+TICK_MS;return;}
        if(state_==State::DRAG){drag(t,x,y);next=t+TICK_MS;}
    }
    void pointerDown(long long t,float x,float y) noexcept {
        if(state_!=State::ARMED && state_!=State::DRAG)return;
        state_=State::PINCH;buttons_=TOUCH;px2=x;py2=y;lastT=t;zr=0;rx=PINCH;ry=0;
        lastLogD=std::log(std::max(1.0,std::hypot(double(px2-fx),double(py2-fy))));next=t+TICK_MS;
    }
    void move2(long long t,float x1,float y1,float x2,float y2) noexcept {
        if(state_!=State::PINCH)return;
        fx=x1;fy=y1;px2=x2;py2=y2;zoom(t);next=t+TICK_MS;
    }
    void pointerUp(long long) noexcept {if(state_==State::PINCH){rest();state_=State::IGNORED;}}
    void up(long long t) noexcept {
        switch(state_){
            case State::ARMED:
                if(t-downAt<=TAP_MS && t>=lockUntil){
                    state_=State::TAP;buttons_=TOUCH|REQUEST;lockUntil=t+LOCK_MS;
                    rx=jround((x0-w*0.5f)*1280.f/w);ry=jround((y0-h*0.5f)*720.f/h);next=t+PULSE_MS;
                } else {state_=State::IDLE;next=NONE;}
                break;
            case State::DRAG:case State::PINCH:case State::IGNORED:rest();state_=State::IDLE;break;
            default:break;
        }
    }
    void tick(long long t) noexcept {
        if(t<next)return;
        switch(state_){
            case State::DRAG:drag(t,lastX,lastY);next=t+TICK_MS;break;
            case State::PINCH:zoom(t);next=t+TICK_MS;break;
            case State::TAP:rest();state_=State::IDLE;break;
            default:next=NONE;break;
        }
    }
    void cancel() noexcept {rest();state_=State::IDLE;}

private:
    State state_=State::IDLE;
    float x0=0,y0=0,fx=0,fy=0,px2=0,py2=0,w=0,h=0,vx=0,vy=0,zr=0,lastX=0,lastY=0;double lastLogD=0;
    long long downAt=0,lastT=0,next=NONE,lockUntil=LLONG_MIN;
    int rx=0,ry=0,buttons_=0;
    static int axis(float v) noexcept {return jround(32767*std::max(-1.f,std::min(1.f,v)));}
    void drag(long long t,float x,float y) noexcept {
        const float dt=std::max(1.f,float(t-lastT)),a=std::min(1.f,dt/PAN_SMOOTH_MS);
        vx+=a*((x-lastX)*1000.f/dt-vx);vy+=a*((y-lastY)*1000.f/dt-vy);lastX=x;lastY=y;lastT=t;
        const float speed=float(std::hypot(double(vx),double(vy)));
        if(speed<PAN_STILL){rx=ry=0;return;}
        const float k=(PAN_MIN+(1.f-PAN_MIN)*std::min(1.f,speed/PAN_FULL))/speed;
        rx=axis(-vx*k);ry=axis(vy*k);
    }
    void zoom(long long t) noexcept {
        const double ld=std::log(std::max(1.0,std::hypot(double(px2-fx),double(py2-fy))));
        const float dt=std::max(1.f,float(t-lastT)),a=std::min(1.f,dt/ZOOM_SMOOTH_MS);
        zr+=a*(float((ld-lastLogD)*1000.0/dt)-zr);lastLogD=ld;lastT=t;
        const int v=std::fabs(zr)<ZOOM_STILL?0:jround(TRIGGER_MIN+(255-TRIGGER_MIN)*std::min(1.f,std::fabs(zr)/ZOOM_FULL));
        rx=PINCH;ry=zr>0?v:-v;
    }
    void rest() noexcept {buttons_=0;rx=ry=0;vx=vy=zr=0;next=NONE;}
};
}
