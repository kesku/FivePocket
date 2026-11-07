#ifdef RECOMP_THOR_EDITION
// ios-duo (2026-10-24): two texts in one file: the Thor Edition's v850 (RECOMP_THOR_EDITION: the Duo) and the regular iOS build's 238e.
#pragma once
// thor-maptap (v658, 2026-10-24; THOR EDITION ONLY: included only under RECOMP_THOR_EDITION). Owner: "get it as low as you possibly can,
// for ms wise, it needs to look almost seamless. Maybe ... a brief GTA V native blur goes over the screen before the map loads".
//
// A tap on the Thor's lower screen opens the pause menu's full map. Until v656 the app pressed START, waited a fixed 250 ms and
// pressed A; the menu's MAP tab (title bar, tabs, the map in a window) was visible for 0.2-0.4 s before A took effect. This file is
// the library's side of a request from the app (the reserved XINPUT bit 0x0800 in the pad the app publishes; the game never sees
// it): the library presses the buttons itself, on the pad reads, and hides the MAP-tab frames.
//
// What was measured (thor-maptap, Thor, v656, warm, diag-stick presses; local-analysis/agents/thor-maptap/trials.tsv, video/):
//  - START opens the menu; the game's own pause blur covers the frame at once, the MAP tab's content fades in ~200 ms later.
//  - The MAP tab acts on A's RELEASE (A held 1.5 s: MAP tab until the release, then the full map). A released 251-271 ms after
//    START's release = MAP tab (6/6), 301 ms = full map (6/6, START 30-40 ms): the menu ignores A until ~0.3 s after it opened.
//    The first open after a while needed more (A released 370 ms after START's release = MAP tab).
//  - Every MAP-tab frame draws the map window's operations with the guest scissor (207,159)-(1078,591) on the 1280-pitch main
//    surface (census 2026-10-24 c3; thor-play-1021 g2-map-1); the full map draws the same shaders with (1,1)-(1280,720).
// So the library knows, per frame, whether the MAP tab is on screen. The macro (Machine):
//  Start    START held from the first read that carries the request until >= StartMinPresents presents and >= StartMinMs.
//  WaitTab  nothing pressed; the first presented frame that drew the MAP tab -> PressA.
//  PressA   A held; released once TabFramesBeforeRelease presents with the tab have passed (the menu takes input ~1-2 frames after
//           its content starts to fade in) and A was down >= AMinPresents presents and AMinMs (A acts on its release).
//  Verify   DoneFrames presents without the MAP tab = the full map is up: the end. The MAP tab still there RetryMs after the
//           release (the menu was not ready yet, e.g. the first open after a load) = A once more (PressA), at most RetryMax times.
// v661 (the owner: "can't be improved more?"; v659's own taps: MAP tab drawn 566-598 ms after the request, A up 633-665 ms, full map
// ~0.7 s, while the menu takes A ~0.3 s after START's release): A is pressed at the first frame that draws the game's pause blur (the
// menu opened; no wheel button held in the last 600 ms) and released earlyMs (300) after START's release; the full map's own draws
// (scissor (1,1)-(1280,720)) end the episode. A MAP tab after that release (the menu was not ready: the first open after a load) =
// the v658 path: A again, released 2 MAP-tab frames later.
// Every frame of the episode that drew the MAP tab is replaced by the last frame that did not (the game's own pause blur, or the
// last game frame): the player sees the game -> its own pause blur -> the full map. Never stuck: an episode ends after
// EpisodeMaxMs whatever happens; no MAP tab within TabWaitMs of START = the menu did not open (the end, nothing more pressed).
// Every tap is a request (the app keeps no guess of its own): the library knows whether the MAP tab is on screen (draws) and whether
// the full map it opened is still up (no START / B of the player's and no MAP tab since): MAP tab = A alone; its full map = START
// alone (closes the menu); otherwise the open macro above. (2026-10-24 device: the app's own guess stayed "open" after a request the
// game ignored during a phone call, and its next tap opened the menu instead.)
// Injected buttons are OR'd into the player's pad with a fresh packet number (as diag-stick does); the request bit is always removed.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#include "quick_save.h" // v820 (thor-radio): the lower screen's Quick Save button
#include "phone_touch.h" // v828 (thor-radio): a tap on a phone item selects it

namespace recomp::gpu::maptap {

// The capability marker the app looks for in the library (zero-terminated, WheelTouch scans private-trace-live.so for it).
inline constexpr char Marker[]="recomp-map-tap-v1";
constexpr std::uint16_t Request=0x0800,Start=0x0010,PadA=0x1000,PlayerB=0x2000; // (0x0800: reserved in XINPUT_GAMEPAD; the app's JNI passes it)
constexpr int TabX0=207,TabY0=159,TabX1=1078,TabY1=591; // the MAP tab's map window (guest pixels)

// A draw's guest scissor on a pass of passWidth: the MAP tab's map window (+-TabSlack pixels)?
// v703 (Fold qol-touch, cross-port 2026-10-24, CROSS-PORT-LOG 15:25 / 15:31 / 16:25; into the Thor line in v698): the iPad's MAP tab
// measured 206,158..1075,588, the Thor's 207,159..1078,591; the +-1 match with an exact x0 never fired on the iPad. +-4 on every edge.
constexpr int TabSlack=4;
constexpr bool tabScissor(int x0,int y0,int x1,int y1,unsigned passWidth) noexcept {
    const auto near=[](int a,int b){return a-b<=TabSlack && b-a<=TabSlack;};
    return passWidth==1280 && near(x0,TabX0) && near(y0,TabY0) && near(x1,TabX1) && near(y1,TabY1);
}

struct Params {
    std::int64_t startMinMs=50;std::uint64_t startMinPresents=2;  // START down: at least two game frames see it
    std::int64_t earlyMs=300;                                      // v661: A pressed at the first pause-blur frame, released this long
                                                                   // after START's release (0 = off: A only once the MAP tab is drawn)
    std::uint64_t tabFramesBeforeRelease=2;                        // presents with the MAP tab before A is released
    std::int64_t aMinMs=50;std::uint64_t aMinPresents=2;           // A down at least this long
    std::int64_t retryMs=300;unsigned retryMax=3;                  // the MAP tab still there this long after a release: A again
    std::int64_t tabWaitMs=2500;                                   // no MAP tab this long after START: the menu did not open
    std::int64_t episodeMaxMs=8000;                                // hard cap of an episode (v665: 8 s; the first open after a load took 3-4 s)
    std::int64_t requestGapMs=250;                                 // a new request needs the bit absent this long
    std::uint64_t doneFrames=2;                                    // presents without the MAP tab after a release = the full map
    bool hide=true;                                                // replace the MAP-tab frames (false: the macro alone)
    bool off=false;                                                // requests ignored (the bit is still removed)
};
// files/private-trace/diag-maptap-tune (optional, read at each request): "key=value" words, e.g. "tab=3 retry=250 hide=0 early=0".
inline void parseTune(const char* text,Params& p) noexcept {
    if(!text)return;
    const char* s=text;
    while(*s) {
        while(*s==' ' || *s=='\n' || *s=='\t' || *s=='\r')++s;
        char key[16]{};long long v=0;int used=0;
        if(std::sscanf(s,"%15[a-z]=%lld%n",key,&v,&used)==2 && used>0 && v>=0 && v<=60000) {
            const auto is=[&](const char* k){for(int i=0;;++i){if(key[i]!=k[i])return false;if(!k[i])return true;}};
            if(is("startms"))p.startMinMs=v;else if(is("startframes"))p.startMinPresents=std::uint64_t(v);
            else if(is("tab"))p.tabFramesBeforeRelease=std::uint64_t(v);else if(is("ams"))p.aMinMs=v;else if(is("aframes"))p.aMinPresents=std::uint64_t(v);
            else if(is("retry"))p.retryMs=v;else if(is("retries"))p.retryMax=unsigned(v);else if(is("wait"))p.tabWaitMs=v;
            else if(is("max"))p.episodeMaxMs=v;else if(is("done"))p.doneFrames=std::uint64_t(v);else if(is("hide"))p.hide=v!=0;
            else if(is("off"))p.off=v!=0;else if(is("early"))p.earlyMs=v;
            s+=used;
        } else {while(*s && *s!=' ' && *s!='\n')++s;}
    }
}

enum class Phase:std::uint8_t {Idle,Start,WaitTab,PressA,Verify,PanTo};
enum class End:std::uint8_t {None,Full,NoMenu,Retries,Timeout,Closed,Placed};
enum class Mode:std::uint8_t {Open,Enter,Close,Waypoint};
inline const char* endName(End e) noexcept {
    switch(e){case End::Full:return "full-map";case End::NoMenu:return "no-menu";case End::Retries:return "retries";case End::Timeout:return "timeout";
        case End::Closed:return "closed";case End::Placed:return "a-pressed";default:return "-";}
}
inline const char* modeName(Mode m) noexcept {return m==Mode::Enter?"enter":m==Mode::Close?"close":m==Mode::Waypoint?"waypoint":"open";}
// v663 (touch map, owner 2026-10-24: "drag to pan, pinch to zoom, tap for a waypoint"): the app's touchpad channel. While the bit Touch
// (0x0400, XINPUT's guide bit: the game ignores it, the Thor's mapper sets it only for a MODE key) is in the pad, the app has put a
// one-finger drag into the left stick and a pinch into LT / RT; the library passes them to the game only while the pause full map is
// on screen (its own draws, in a paused frame), else it gives the game a resting left stick and released triggers for those reads.
// Touch with Request = a tap outside the Resume button: on the full map A (set / remove the waypoint at the cursor), elsewhere the
// usual request (open the map). Request alone = a tap on the Resume button: on the full map START (resume).
// v667 (owner 2026-10-24: "the touch controls on the map need to be on the top screen"): the gestures come from the MAIN screen and
// travel in the RIGHT stick (the player's own left stick and triggers are never replaced outside the map): a Touch read carries a drag
// as RX / RY = the left stick wanted, a pinch as RX = PinchCode and RY = the trigger wanted (+ RT, - LT); on the full map the library
// moves them to LS / LT / RT, elsewhere it only rests the right stick for those reads. Touch with Request = a tap on the main screen,
// RX / RY = its offset from the screen's centre in guest pixels (1280x720): on the full map the cursor is panned to it (PanTo, from the
// map's own transform, MapXform) and A sets the waypoint there; elsewhere nothing at all. Request alone = the lower screen's tap.
constexpr std::uint16_t Touch=0x0400;
// v815: the phone's codes in the right stick of a Request | Touch (WheelGesture.java PHONE_TAP / PHONE_SWIPE; never stick positions:
// the main screen's map touch carries -640..640).
constexpr std::int16_t PhoneTapCode=-32766,PhoneSwipeCode=-32765;
// v828: a tap ON the phone (not its Back zone): RY = phonetouch::pack(the lower screen's x share, y share). The renderer reads the phone
// screen, plans the D-pad steps to the tapped item and A (phone_touch.h), and this machine presses them (PhoneStep* timing).
constexpr std::int16_t PhoneTapAtCode=-32763;
constexpr std::int64_t PhoneStepDownMs=120,PhoneStepGapMs=230,PhoneStepBeforeAMs=380,PhoneTapWaitMs=1500;
inline constexpr char PhoneTouchMarker[]="recomp-phone-touch-v2"; // (WheelTouch.java PHONE_MARKER_V2: the app sends PhoneTapAtCode only with it)
constexpr int PhoneTapA=0,PhoneTapBack=1,PhoneSwipeUp=1,PhoneSwipeRight=2,PhoneSwipeDown=3,PhoneSwipeLeft=4;
constexpr std::int64_t PhoneKeyMs=100;
inline constexpr char PhoneMarker[]="recomp-phone-touch-v1"; // (WheelTouch.java PHONE_MARKER: the app sends the phone codes only with it)
constexpr std::int16_t PinchCode=-32768;
struct Sticks {std::int16_t* lx=nullptr;std::int16_t* ly=nullptr;std::uint8_t* lt=nullptr;std::uint8_t* rt=nullptr;std::int16_t* rx=nullptr;std::int16_t* ry=nullptr;};
// v667: the pause map's projection as its own draws carry it (vertex constants c8 / c9 / c11 of the map layer's tile and vector draws, ps
// 4a524266 / 6ed05821 on the 1280 pass with the full-map scissor; census 2026-10-24 out: c8.x = 0.0033, c9.y = 0.0062, c11 = (3.4995,
// -1.0895, ...)): clip x = wx * sx + tx, clip y = wy * sy + ty; guest pixel = ((clip x + 1) * 640, (1 - clip y) * 360).
struct MapXform {float sx=0,sy=0,tx=0,ty=0;std::uint64_t present=0;bool ok() const noexcept {return sx!=0 && sy!=0;}};
constexpr std::uint64_t MapTilePs=0x4a5242663895a27dull,MapVectorPs=0x6ed058218489bbbeull;
// (v673 device, v672: 4 px was never reached - the stick's smallest push past the game's dead zone moves the map ~3-5 px a frame, so it
// circled 5.7 px off for the rest of the 3 s; 8 guest px = 12 px of the main panel, the size of the cursor's centre ring)
constexpr float PanStopPx=8.0f,PanMinStick=0.3f,PanGainPx=60.0f,PanTimeoutPx=24.0f; // (guest px; the stick from the remaining distance)
constexpr std::int64_t PanMaxMs=3000;

// What a presented frame drew (the renderer's draw hook, v661): the MAP tab's map window, the full map's (scissor (1,1)-(1280,720)),
// the game's pause blur (ps 0ce70535f29e499b on the main surface: census 2026-10-24 c3 / c4, absent from game frames c1 / c2; the
// weapon wheel's blur draws it too - the macro only looks right after its own START, with no wheel button held).
constexpr unsigned SeenTab=1u,SeenFull=2u,SeenBlur=4u,SeenPost=8u;
constexpr std::uint64_t BlurPs=0x0ce70535f29e499bull;
// v661: the game's tonemap (post_fidelity.h PostfxShaders[0]): every game frame draws it, the pause menu's frames do not (census c1 / c2
// vs c3 / c4; the weapon wheel's blur frames do: thor-play-1021 wheel-0..2). Paused = the pause blur without the tonemap.
constexpr std::uint64_t TonemapPs=0x08dfab6c16ec854cull;
constexpr bool pausedFrame(unsigned seen) noexcept {return (seen&SeenBlur) && !(seen&SeenPost);}
constexpr bool fullScissor(int x0,int y0,int x1,int y1,unsigned passWidth) noexcept {return passWidth==1280 && x0==1 && y0==1 && x1==1280 && y1==720;}

// What one episode did (the renderer logs it once the episode ends): times in ms from the first read with the request.
struct Summary {
    std::uint64_t n=0;End end=End::None;Mode mode=Mode::Open;
    std::int64_t startUpMs=-1,blurMs=-1,tabMs=-1,firstReleaseMs=-1,lastReleaseMs=-1,doneMs=-1;
    std::uint64_t startPresents=0,tabToRelease=0;unsigned retries=0,held=0,tabFrames=0,panFrames=0;bool early=false,fullSeen=false,panned=false;
    int tap[2]{};float xf[4]{};float panLeft=-1; // (v671: the waypoint tap, the projection at the tap, the distance left when A was pressed)
};

class Machine {
public:
    // A pad read of user 0 (any thread): the buttons the game is given (the request removed, the macro's buttons OR'd).
    // *fresh: the macro's buttons differ from the player's alone - the caller gives the read a packet number of its own (serial).
    // v815 (thor-radio, the phone on the lower screen): the app's lower-screen tap / swipe come as Request | Touch with a code in the right
    // stick (PhoneTapCode: ry = PhoneTapA / PhoneTapBack; PhoneSwipeCode: ry = PhoneSwipeUp..Left). While the phone is up (the renderer's
    // setPhone) a tap = A or B, a swipe = the D-pad direction, each pressed for PhoneKeyMs; the game never sees the request. Without the
    // phone a tap is the usual map-tap request (no touch position) and a swipe nothing.
    // v820 (thor-radio): a tap on the lower screen's Quick Save button comes as Request | Touch with SaveCode in the right stick: while
    // the button is shown (the HUD screen's setSaveShown) the QuickSave macro starts (quick_save.h) and the game never sees the request;
    // with the phone up nothing; otherwise the tap is the usual map-tap request (the button was not on screen).
    std::uint16_t read(std::uint16_t buttons,std::int64_t now,bool* fresh,std::uint32_t* serial,const Sticks& sticks=Sticks{}) noexcept {
        if((buttons&Touch) && (buttons&Request) && sticks.rx && *sticks.rx==SaveCode) {
            *sticks.rx=0;if(sticks.ry)*sticks.ry=0;
            if(phone_.load(std::memory_order_relaxed))buttons=std::uint16_t(buttons&~(Request|Touch));
            else if(saveShown_.load(std::memory_order_relaxed) && !loading_.load(std::memory_order_relaxed) && !pausedNow_.load(std::memory_order_relaxed)) {
                if(!saveHeld_){saveHeld_=true;
                    const auto hook=saveMenu_.load(std::memory_order_acquire); // (v834: the native path first)
                    const bool nativeOk=hook && hook(12,0,0,0)>=0; // (trainer.h OpenSaveMenu)
                    std::lock_guard l(m_);qs_.start(now,phoneKnown_.load(std::memory_order_relaxed),nativeOk);}
                buttons=std::uint16_t(buttons&~(Request|Touch));
            } else buttons=std::uint16_t(buttons&~Touch);
        } else if((buttons&Touch) && (buttons&Request) && sticks.rx && *sticks.rx==PhoneTapAtCode) { // v828
            const std::int16_t at=sticks.ry?*sticks.ry:0;
            *sticks.rx=0;if(sticks.ry)*sticks.ry=0;
            if(phone_.load(std::memory_order_relaxed)) {
                if(!phoneHeld_){phoneHeld_=true;std::lock_guard l(m_);if(phoneSteps_.empty()){tapAt_=at;tapAtMs_=now;++tapSerial_;}}
                buttons=std::uint16_t(buttons&~(Request|Touch));
            } else buttons=std::uint16_t(buttons&~Touch); // (no phone: the usual map-tap request)
        } else if((buttons&Touch) && (buttons&Request) && sticks.rx && (*sticks.rx==PhoneTapCode || *sticks.rx==PhoneSwipeCode)) {
            const int code=*sticks.rx,val=sticks.ry?*sticks.ry:0;
            *sticks.rx=0;if(sticks.ry)*sticks.ry=0;
            if(phone_.load(std::memory_order_relaxed)) {
                if(!phoneHeld_) {
                    phoneHeld_=true;std::lock_guard l(m_);
                    phoneKey_=code==PhoneTapCode?(val==PhoneTapBack?PlayerB:PadA):phoneDir(val);
                    phoneUntil_=now+PhoneKeyMs;++serial_;++phoneCmds_;
                }
                buttons=std::uint16_t(buttons&~(Request|Touch));
            } else buttons=std::uint16_t(code==PhoneTapCode?buttons&~Touch:buttons&~(Request|Touch));
        } else if(!(buttons&Request)){phoneHeld_=false;saveHeld_=false;}
        const std::uint16_t player=buttons;
        bool f=false;std::uint32_t sr=0;
        std::uint16_t out=readMap(buttons,now,&f,&sr,sticks);
        {std::lock_guard l(m_);
         if(now<phoneUntil_ && phoneKey_){out=std::uint16_t(out|phoneKey_);f=true;sr=serial_;}
         const std::uint16_t st=stepButtons(now); // (v828: a tapped item's steps)
         if(st!=stepLast_){stepLast_=st;++serial_;}
         if(st){out=std::uint16_t(out|st);f=true;sr=serial_;}
         const std::uint16_t q=qs_.buttons(now,phone_.load(std::memory_order_relaxed),std::uint16_t(player&~(Request|Touch)), // (v820; v834: state-driven)
             phoneSel_.load(std::memory_order_relaxed),presents_,pausedNow_.load(std::memory_order_relaxed));
         if(q!=qsLast_){qsLast_=q;++serial_;}
         if(q){out=std::uint16_t(out|q);f=true;sr=serial_;}}
        if(fresh)*fresh=f;
        if(serial)*serial=sr;
        return out;
    }
    void setPhone(bool up) noexcept {phone_.store(up,std::memory_order_relaxed);if(!up){std::lock_guard l(m_);phoneSteps_.clear();stepAt_=0;tapTaken_=tapSerial_;}}
    // v828: the renderer's half of a tap on the phone. takeTap: a tap waiting for its plan (once; the packed position); a tap older than
    // PhoneTapWaitMs is dropped. setSteps: the plan's keys, pressed from the next read (at most one plan at a time; a new tap waits).
    bool takeTap(std::int64_t now,std::int16_t& at) noexcept {
        std::lock_guard l(m_);
        if(tapSerial_==tapTaken_)return false;
        tapTaken_=tapSerial_;
        if(now-tapAtMs_>PhoneTapWaitMs)return false;
        at=tapAt_;return true;
    }
    void setSteps(const std::vector<phonetouch::Key>& keys,std::int64_t now) noexcept {
        std::lock_guard l(m_);phoneSteps_=keys;stepAt_=0;stepNext_=now;++phoneCmds_;
    }
    std::size_t stepsLeft() noexcept {std::lock_guard l(m_);return phoneSteps_.size()-std::min(phoneSteps_.size(),stepAt_);}
    // v820: the renderer, per present: whether it can see the phone (mirrored below); the HUD screen, per picture: the button is shown.
    void setPhoneKnown(bool k) noexcept {phoneKnown_.store(k,std::memory_order_relaxed);}
    void setSaveShown(bool on) noexcept {saveShown_.store(on,std::memory_order_relaxed);}
    bool saveShown() const noexcept {return saveShown_.load(std::memory_order_relaxed);} // (v832: recomp_lower_buttons_v1)
    bool quickSaveBusy() noexcept {std::lock_guard l(m_);return qs_.busy();}
    // v834: the native save menu's request (recomp_trainer_v1; the renderer sets it), the phone grid's highlight for the state-driven macro
    // (the renderer's read of the phone screen; -1 none), and whether the macro wants it read.
    void setSaveMenuHook(int(*f)(int,int,int,int)) noexcept {saveMenu_.store(f,std::memory_order_release);}
    void setPhoneSel(int sel) noexcept {phoneSel_.store(sel,std::memory_order_relaxed);}
    bool quickSaveWantsSel() noexcept {std::lock_guard l(m_);return qs_.wantsSel();}
    std::int64_t quickSaveMs() noexcept {std::lock_guard l(m_);return qs_.doneMs;}
    struct QsStatus {std::uint64_t runs=0,ends=0;QuickSave::End last=QuickSave::End::None;bool busy=false,known=false;};
    QsStatus quickSaveStatus() noexcept {std::lock_guard l(m_);return {qs_.runs,qs_.ends,qs_.last,qs_.busy(),qs_.known};}
    bool phone() const noexcept {return phone_.load(std::memory_order_relaxed);}
    std::uint64_t phoneCommands() noexcept {std::lock_guard l(m_);return phoneCmds_;}
    static constexpr std::uint16_t phoneDir(int v) noexcept {return v==PhoneSwipeUp?0x0001:v==PhoneSwipeDown?0x0002:v==PhoneSwipeLeft?0x0004:v==PhoneSwipeRight?0x0008:0;}
    std::uint16_t readMap(std::uint16_t buttons,std::int64_t now,bool* fresh,std::uint32_t* serial,const Sticks& sticks=Sticks{}) noexcept {
        const bool request=(buttons&Request)!=0,touch=(buttons&Touch)!=0;buttons=std::uint16_t(buttons&~(Request|Touch));
        if(buttons&Start){if(!startHeld_.exchange(true,std::memory_order_relaxed))startAt_.store(now,std::memory_order_relaxed);} // (thor-char v689: the player's START)
        else if(startHeld_.load(std::memory_order_relaxed))startHeld_.store(false,std::memory_order_relaxed);
        if(fresh)*fresh=false;
        std::int16_t tapX=0,tapY=0;
        if(touch) { // v667: the main screen's gesture, in the right stick
            const std::int16_t x=sticks.rx?*sticks.rx:0,y=sticks.ry?*sticks.ry:0;
            if(sticks.rx)*sticks.rx=0;if(sticks.ry)*sticks.ry=0; // (the game never sees the gesture in the right stick)
            if(request){tapX=x;tapY=y;}
            else if(mapShown_.load(std::memory_order_relaxed) && !busy_.load(std::memory_order_relaxed)) {
                if(x==PinchCode){const int v=std::clamp<int>(y,-255,255);if(sticks.rt)*sticks.rt=std::uint8_t(v>0?v:0);if(sticks.lt)*sticks.lt=std::uint8_t(v<0?-v:0);
                    if(sticks.lx)*sticks.lx=0;if(sticks.ly)*sticks.ly=0;}
                else {if(sticks.lx)*sticks.lx=x;if(sticks.ly)*sticks.ly=y;}
            }
            if(request && !mapShown_.load(std::memory_order_relaxed))return buttons; // (a main-screen tap off the map: nothing)
        }
        if(!request && !busy_.load(std::memory_order_relaxed) && !((buttons&(Start|PlayerB)) && mapOpen_.load(std::memory_order_relaxed)) &&
           !(buttons&WheelButtons) && !((buttons&Start) && pausedNow_.load(std::memory_order_relaxed)))return buttons;
        std::lock_guard l(m_);
        if((buttons&Start) && pausedNow_.load(std::memory_order_relaxed) && !(lastStart_))lastClose_=now; // (v677: the player's own START closes a pause screen)
        lastStart_=(buttons&Start)!=0;
        if(buttons&(Start|PlayerB))mapOpen_.store(false,std::memory_order_relaxed); // (the player's own START / B: the menu's state is theirs)
        if(buttons&WheelButtons)lastWheel_=now; // (LB / D-pad LEFT: a wheel's blur is no pause menu)
        if(request) {
            lastRequest_=now;
            if(armed_ && phase_==Phase::Idle && !p_.off && !loading_.load(std::memory_order_relaxed) && !tapIgnored(touch))begin(now,touch,tapX,tapY); // (thor-char v685: not during a switch)
        } else if(!armed_ && now-lastRequest_>=p_.requestGapMs)armed_=true;
        advance(now);
        busy_.store(phase_!=Phase::Idle || !armed_,std::memory_order_relaxed);
        if(inject_) {buttons=std::uint16_t(buttons|inject_);if(fresh)*fresh=true;if(serial)*serial=serial_;}
        if(phase_==Phase::PanTo){if(sticks.lx)*sticks.lx=panX_;if(sticks.ly)*sticks.ly=panY_;if(fresh)*fresh=true;if(serial)*serial=serial_;} // (v667)
        return buttons;
    }
    // v667: the renderer's draw hook: this frame's map projection (from a map-layer draw's vertex constants).
    void noteXform(float sx,float sy,float tx,float ty) noexcept {std::lock_guard l(xm_);xform_={sx,sy,tx,ty,presents_+1};}
    // The app's parameters for the next request (read by the caller at a request; never during an episode).
    void tune(const Params& p) noexcept {std::lock_guard l(m_);if(phase_==Phase::Idle)p_=p;}
    Params params() noexcept {std::lock_guard l(m_);return p_;}
    // Whether an episode runs.
    bool watching() const noexcept {return watch_.load(std::memory_order_relaxed);}
    struct Frame {bool active=false,hold=false,ended=false;Summary summary;};
    // The renderer's present of a real frame (renderer thread) with what it drew (Seen* bits). Returns whether an episode runs (the
    // renderer keeps a copy of non-tab frames) and whether this frame is replaced by that copy.
    Frame present(std::int64_t now,unsigned seen) noexcept {
        const bool tab=(seen&SeenTab)!=0,full=(seen&SeenFull)!=0 && !tab,paused=pausedFrame(seen);
        std::lock_guard l(m_);
        ++presents_;
        if(paused)lastPaused_=presents_;
        pausedNow_.store(paused,std::memory_order_relaxed);
        MapXform xf;{std::lock_guard xl(xm_);xf=xform_;}
        const bool xfNow=xf.ok() && xf.present==presents_;
        if(full && paused)lastFull_=presents_; // v663: the full map on screen
        mapShown_.store(lastFull_ && presents_-lastFull_<=3,std::memory_order_relaxed);
        if(tab){lastTab_=presents_;mapOpen_.store(false,std::memory_order_relaxed);} // (the MAP tab on screen: not the full map)
        Frame f;
        if(phase_==Phase::Idle){if(pendingEnd_){f.ended=true;f.summary=sum_;pendingEnd_=false;}return f;}
        if(tab)++sum_.tabFrames;
        if(full)sum_.fullSeen=true;
        switch(phase_) {
            case Phase::WaitTab:
                if(tab){tabSeen(now);press(now,false);}
                else if(paused && p_.earlyMs>0 && now-lastWheel_>WheelGraceMs){sum_.blurMs=now-t0_;press(now,true);} // v661: the menu opened
                break;
            case Phase::PressA:
                if(tab && !sawTab_)tabSeen(now); // (the MAP tab while the early A is down: released at the later of the two times)
                break;
            case Phase::PanTo: { // v667: the target's place on screen now, from this frame's projection; A once it is under the cursor
                if(!xfNow)break;
                const float gx=(target_[0]*xf.sx+xf.tx+1.0f)*640.0f-640.0f,gy=(1.0f-(target_[1]*xf.sy+xf.ty))*360.0f-360.0f,d=std::hypot(gx,gy);
                sum_.panLeft=d;
                if(d<=PanStopPx){panX_=panY_=0;setInject(PadA);phase_=Phase::PressA;phaseMs_=now;phasePresent_=presents_;early_=false;sawTab_=false;break;}
                const float k=std::min(1.0f,PanMinStick+d/PanGainPx)/d;
                panX_=std::int16_t(std::lround(std::clamp(gx*k,-1.0f,1.0f)*32767.0f));panY_=std::int16_t(std::lround(std::clamp(-gy*k,-1.0f,1.0f)*32767.0f));
                ++serial_;sum_.panFrames++;
                break;}
            case Phase::Verify:
                if(full){finish(now,End::Full);break;}                 // v661: the full map's own draws
                if(tab){if(!sawTab_)tabSeen(now);noTab_=0;}else if(sawTab_)++noTab_; // (v661: a MAP tab after an early A = the menu was not ready)
                if(sawTab_ && noTab_>=p_.doneFrames){finish(now,End::Full);break;}
                // A again only when the MAP tab is still there retryMs after the release (a MAP tab still shown for a frame or two while
                // the full map comes is no reason: that A would land in the full map = a waypoint)
                if(tab && now-releaseMs_>=p_.retryMs) {
                    if(sum_.retries>=p_.retryMax){finish(now,End::Retries);break;}
                    ++sum_.retries;press(now,false);
                }
                break;
            default:break;
        }
        advance(now);
        f.active=phase_!=Phase::Idle || pendingEnd_;
        f.hold=tab && p_.hide && (phase_!=Phase::Idle || (pendingEnd_ && sum_.end==End::Full));
        if(f.hold)++sum_.held;
        if(phase_==Phase::Idle && pendingEnd_){f.ended=true;f.summary=sum_;pendingEnd_=false;}
        return f;
    }
    Frame present(std::int64_t now,bool tab) noexcept {return present(now,tab?SeenTab:0u);}
    Phase phase() noexcept {std::lock_guard l(m_);return phase_;}
    bool mapOpen() const noexcept {return mapOpen_.load(std::memory_order_relaxed);}
    bool mapShown() const noexcept {return mapShown_.load(std::memory_order_relaxed);}
    // RC1.1 (v811): the renderer, per present: whether the game's pause menu is open (touch_context.h IS_PAUSE_MENU_ACTIVE).
    void setPauseMenu(bool open) noexcept {pauseMenu_.store(open,std::memory_order_relaxed);}
    // v667: the pressed look of the lower screen's Resume pill: a Close request within the last 600 ms (until the menu has gone).
    bool closing(std::int64_t now) noexcept {std::lock_guard l(m_);return now-lastClose_<600;}
    // v677: when the pause screen was last closed (the lower screen's tap, or the player's own START during a pause), ms.
    std::int64_t lastCloseMs() noexcept {std::lock_guard l(m_);return lastClose_;}
    std::uint16_t injected() noexcept {std::lock_guard l(m_);return inject_;}
    bool wheelRecent(std::int64_t now) noexcept {std::lock_guard l(m_);return now-lastWheel_<=WheelGraceMs;}
    // thor-char (v685): a character switch is running (the lower screen says Loading): taps on the lower screen do nothing.
    void setLoading(bool on) noexcept {loading_.store(on,std::memory_order_relaxed);}
    // thor-char (v689): when the player last pressed START (its rising edge in the pad reads; any state of the game).
    std::int64_t lastStartPressMs() const noexcept {return startAt_.load(std::memory_order_relaxed);}
    std::atomic<std::int64_t> startAt_{-(std::int64_t(1)<<40)};std::atomic<bool> startHeld_{false};
    std::atomic<bool> loading_{false};
    static constexpr std::uint16_t WheelButtons=0x0104; // LB, D-pad LEFT
    static constexpr std::int64_t WheelGraceMs=600;
private:
    // A request: the MAP tab on screen (the menu is open on it) = A alone (Enter); the full map this macro opened and nothing since
    // (no START / B of the player's, no MAP tab), or (v661) any other pause screen = START alone (Close, "Resume Game"); otherwise
    // START, then A (Open).
    // v663: a touch tap (outside the Resume button) while the full map is on screen = A alone (Waypoint).
    // RC1.1 (v811; owner 2026-10-24 ~22:50: "for the thor, can you make the resume pill only appear when in the map screen?"): with the
    // pause menu open (the renderer's game-state read, setPauseMenu) a lower-screen tap acts only on the map - the full map on screen
    // (Close: the Resume pill's tap) or the MAP tab (Enter: the full map); on any other tab it does nothing (the player is using the menu;
    // the map is never opened from there).
    bool tapIgnored(bool touchTap) const noexcept {
        return !touchTap && pauseMenu_.load(std::memory_order_relaxed) && !mapShown_.load(std::memory_order_relaxed) && !(lastTab_ && presents_-lastTab_<=2);
    }
    void begin(std::int64_t now,bool touchTap,std::int16_t tapX=0,std::int16_t tapY=0) noexcept {
        armed_=false;t0_=phaseMs_=now;phasePresent_=presents_;
        sum_=Summary{};sum_.n=++episodes_;noTab_=0;tabFirst_=0;sawTab_=false;early_=false;watch_.store(true,std::memory_order_relaxed);
        if(touchTap && mapShown_.load(std::memory_order_relaxed)) {
            sum_.mode=Mode::Waypoint;
            MapXform xf;{std::lock_guard xl(xm_);xf=xform_;}
            // (v671: xf.present is the frame being drawn when noted = presents_ + 1 before its present: v670's presents_ - xf.present wrapped)
            if(xf.ok() && xf.present<=presents_+1 && presents_+1-xf.present<=4 && (tapX || tapY)) { // v667: the tapped point in map units
                const float cx=(640.0f+float(tapX))/640.0f-1.0f,cy=1.0f-(360.0f+float(tapY))/360.0f;
                target_[0]=(cx-xf.tx)/xf.sx;target_[1]=(cy-xf.ty)/xf.sy;phase_=Phase::PanTo;panX_=panY_=0;sum_.panned=true;
                sum_.tap[0]=tapX;sum_.tap[1]=tapY;sum_.xf[0]=xf.sx;sum_.xf[1]=xf.sy;sum_.xf[2]=xf.tx;sum_.xf[3]=xf.ty;
            } else press(now,false);
        }
        else if(lastTab_ && presents_-lastTab_<=2){
            sum_.mode=Mode::Enter;sawTab_=true;tabFirst_=presents_>p_.tabFramesBeforeRelease?presents_-p_.tabFramesBeforeRelease:0;sum_.tabMs=0;press(now,false);
        }
        else if(mapOpen_.load(std::memory_order_relaxed) || (lastPaused_ && presents_-lastPaused_<=2)){ // (v661: or any pause screen but the MAP tab)
            sum_.mode=Mode::Close;mapOpen_.store(false,std::memory_order_relaxed);phase_=Phase::Start;setInject(Start);lastClose_=now;}
        else {sum_.mode=Mode::Open;phase_=Phase::Start;setInject(Start);}
    }
    void tabSeen(std::int64_t now) noexcept {tabFirst_=presents_;sawTab_=true;if(sum_.tabMs<0)sum_.tabMs=now-t0_;}
    void press(std::int64_t now,bool early) noexcept {phase_=Phase::PressA;phaseMs_=now;phasePresent_=presents_;early_=early;setInject(PadA);}
    void finish(std::int64_t now,End e) noexcept {
        phase_=Phase::Idle;setInject(0);sum_.end=e;sum_.doneMs=now-t0_;pendingEnd_=true;watch_.store(false,std::memory_order_relaxed);
        if(e==End::Full){mapOpen_.store(true,std::memory_order_relaxed);sum_.early=sum_.blurMs>=0 && !sawTab_;}
    }
    void setInject(std::uint16_t b) noexcept {if(b!=inject_){inject_=b;++serial_;}}
    void advance(std::int64_t now) noexcept {
        if(phase_==Phase::Idle)return;
        if(now-t0_>=p_.episodeMaxMs){finish(now,End::Timeout);return;}
        switch(phase_) {
            case Phase::Start:
                if(now-phaseMs_>=p_.startMinMs && presents_-phasePresent_>=p_.startMinPresents) {
                    phase_=Phase::WaitTab;phaseMs_=startUpAt_=now;setInject(0);sum_.startUpMs=now-t0_;sum_.startPresents=presents_-phasePresent_;
                    if(sum_.mode==Mode::Close)finish(now,End::Closed);
                }
                break;
            case Phase::WaitTab:
                if(now-phaseMs_>=p_.tabWaitMs)finish(now,End::NoMenu);
                break;
            case Phase::PanTo:
                if(now-phaseMs_>=PanMaxMs) { // (time up: A when it got close, else nothing - never a waypoint far from the tap)
                    if(sum_.panLeft>=0 && sum_.panLeft<=PanTimeoutPx){panX_=panY_=0;setInject(PadA);phase_=Phase::PressA;phaseMs_=now;phasePresent_=presents_;early_=false;sawTab_=false;}
                    else finish(now,End::Timeout);
                }
                break;
            case Phase::PressA: {
                const bool held=presents_-phasePresent_>=p_.aMinPresents && now-phaseMs_>=p_.aMinMs;
                const bool due=(!early_ || now-startUpAt_>=p_.earlyMs) && (!sawTab_ || presents_-tabFirst_>=p_.tabFramesBeforeRelease);
                if(held && due && sum_.mode==Mode::Waypoint){sum_.firstReleaseMs=sum_.lastReleaseMs=now-t0_;finish(now,End::Placed);break;}
                if(held && due) {
                    phase_=Phase::Verify;setInject(0);releaseMs_=now;noTab_=0;
                    if(sum_.firstReleaseMs<0){sum_.firstReleaseMs=now-t0_;sum_.tabToRelease=sawTab_?presents_-tabFirst_:0;}
                    sum_.lastReleaseMs=now-t0_;
                }
                break;}
            case Phase::Verify:
                // (an early A, then neither the map nor its tab while the game is no longer paused; v665: a pause still on screen = the menu is
                // still loading - v663 device, first open after a load: 3 s at 2 fps, the MAP tab came after v663's 2.5 s and stayed shown)
                if(!sawTab_ && now-releaseMs_>=p_.tabWaitMs && !(lastPaused_ && presents_-lastPaused_<=2))finish(now,End::NoMenu);
                break;
            default:break;
        }
    }
    std::mutex m_;Params p_;Phase phase_=Phase::Idle;
    std::atomic<bool> busy_{false},watch_{false},mapOpen_{false},mapShown_{false},pauseMenu_{false};
    bool armed_=true,pendingEnd_=false,sawTab_=false,early_=false;
    std::int64_t lastRequest_=-(std::int64_t(1)<<40),lastWheel_=-(std::int64_t(1)<<40),t0_=0,phaseMs_=0,releaseMs_=0,startUpAt_=0;
    std::uint64_t presents_=0,phasePresent_=0,tabFirst_=0,noTab_=0,episodes_=0,lastTab_=0,lastPaused_=0,lastFull_=0;
    std::uint16_t inject_=0;std::uint32_t serial_=0;Summary sum_;
    // v828: the tapped item's steps (under m_): the keys, the next one's index, when it goes down; the pending tap
    std::vector<phonetouch::Key> phoneSteps_;std::size_t stepAt_=0;std::int64_t stepNext_=0;std::uint16_t stepLast_=0;
    std::int16_t tapAt_=0;std::int64_t tapAtMs_=0;std::uint64_t tapSerial_=0,tapTaken_=0;
    static constexpr std::uint16_t keyBits(phonetouch::Key k) noexcept {
        return k==phonetouch::Key::Up?0x0001:k==phonetouch::Key::Down?0x0002:k==phonetouch::Key::Left?0x0004:k==phonetouch::Key::Right?0x0008:k==phonetouch::Key::B?PlayerB:PadA;
    }
    // (under m_) the step's key while it is down: PhoneStepDownMs, then PhoneStepGapMs (PhoneStepBeforeAMs before A) to the next one
    std::uint16_t stepButtons(std::int64_t now) noexcept {
        if(stepAt_>=phoneSteps_.size()){if(!phoneSteps_.empty()){phoneSteps_.clear();stepAt_=0;}return 0;}
        if(now<stepNext_)return 0;
        if(now<stepNext_+PhoneStepDownMs)return keyBits(phoneSteps_[stepAt_]);
        ++stepAt_;
        const bool nextA=stepAt_<phoneSteps_.size() && phoneSteps_[stepAt_]==phonetouch::Key::A;
        stepNext_=now+(nextA?PhoneStepBeforeAMs:PhoneStepGapMs);
        return 0;
    }
    std::atomic<bool> phoneKnown_{false},saveShown_{false};bool saveHeld_=false;QuickSave qs_;std::uint16_t qsLast_=0; // (v820)
    std::atomic<int(*)(int,int,int,int)> saveMenu_{nullptr};std::atomic<int> phoneSel_{-1}; // (v834)
    std::atomic<bool> phone_{false};bool phoneHeld_=false;std::uint16_t phoneKey_=0;std::int64_t phoneUntil_=0;std::uint64_t phoneCmds_=0; // (v815)
    std::int64_t lastClose_=-(std::int64_t(1)<<40);bool lastStart_=false;std::atomic<bool> pausedNow_{false};
    std::mutex xm_;MapXform xform_;float target_[2]{};std::int16_t panX_=0,panY_=0; // (v667)
};
inline Machine& machine() noexcept {static Machine m;return m;}
// v661 (owner 2026-10-24: "on the map screen ... make a resume game button", "GTA's own pause menu obv, ideally should be the same color
// as the map screen"): while the game shows a pause screen the lower screen is a Resume Game button on the pause screen's own colour.
// (v663: v661's ResumeGate (2-frame hysteresis) and dominantColour (live sampling) removed: the owner saw the delay and a wrong shade.)
// v665 (owner 2026-10-24: "does it change when outdoors?", "maybe you can make it dynamic?"; main: from the game's own map drawing, not
// from screen pixels, which caught the blur): the pause map screen's colour from the MAP LAYER itself. Censuses 2026-10-24 (c4 indoor,
// out outdoor): the game draws the map (base tiles 4a524266 with 512x512 k_DXT4_5 atlas tiles, vectors 6ed05821, the interior plan
// 79c5b389) on its own 1280x720 surface cleared to (0,0,0,0), resolves it, then on the frontend surface draws the blurred backdrop
// (62ff5489), an untextured tint quad (d2452cdc: its vertex colour was white, then transparent black - no background colour) and the
// map layer over them with ps e2649e6b, sampler s2 = the layer (MapLayerPs). The renderer blits that layer (bytes in the order the
// screen shows them, as the present image) to 16x9 texels every MapLayerEvery-th paused frame and takes the dominant colour of the
// opaque texels (layerColour); fewer than MapLayerMinShare opaque texels (the port's outdoor map: no land / sea layer drawn) = the
// fixed #A3A6AA.
constexpr std::uint64_t MapLayerPs=0xe2649e6bc8ee4db8ull;
constexpr unsigned MapLayerW=16,MapLayerH=9,MapLayerEvery=4,MapLayerLatency=4;
constexpr float MapLayerMinShare=0.2f;
// The colours of n texels `rgba` (RGBA8) where the map layer's texels `mask` are opaque (alpha >= 230): the most common colour (4 bits
// per channel), its mean; false = too few opaque layer texels. v665 device: the layer itself is brighter than the screen (182,186,193
// in the layer, 163,166,170 on screen: the composite over the backdrop darkens it), so `rgba` is the present image at the same 16x9
// points: the colour the main screen shows where the map is opaque (never the blur: those points are masked out). mask = rgba: the
// layer's own colour.
inline bool layerColour(const std::uint8_t* rgba,const std::uint8_t* mask,unsigned n,std::uint8_t out[3]) noexcept {
    if(!n)return false;
    std::vector<std::uint16_t> count(4096,0);unsigned opaque=0;
    const auto key=[&](unsigned i){return unsigned(rgba[4*i]>>4)<<8|unsigned(rgba[4*i+1]>>4)<<4|unsigned(rgba[4*i+2]>>4);};
    for(unsigned i=0;i<n;++i)if(mask[4*i+3]>=230){++opaque;++count[key(i)];}
    if(!opaque || float(opaque)<MapLayerMinShare*float(n))return false;
    unsigned best=0;for(unsigned k=1;k<4096;++k)if(count[k]>count[best])best=k;
    unsigned sum[3]{},m=0;
    for(unsigned i=0;i<n;++i)if(mask[4*i+3]>=230 && key(i)==best){++m;for(unsigned c=0;c<3;++c)sum[c]+=rgba[4*i+c];}
    for(unsigned c=0;c<3;++c)out[c]=std::uint8_t((sum[c]+m/2)/m);
    return true;
}

// The renderer's per-frame Seen* bits (set by draw(), taken by present()).
inline std::atomic<unsigned>& seenDrawn() noexcept {static std::atomic<unsigned> b{0};return b;}

} // namespace recomp::gpu::maptap
#else /* RECOMP_THOR_EDITION */
#pragma once
// thor-maptap (v658, 2026-10-24; included under RECOMP_THOR_EDITION, and since v700 under RECOMP_TOUCH_MAPTAP for the single-screen
// QoL edition: nothing here touches a second screen). Owner: "get it as low as you possibly can,
// for ms wise, it needs to look almost seamless. Maybe ... a brief GTA V native blur goes over the screen before the map loads".
//
// A tap on the Thor's lower screen opens the pause menu's full map. Until v656 the app pressed START, waited a fixed 250 ms and
// pressed A; the menu's MAP tab (title bar, tabs, the map in a window) was visible for 0.2-0.4 s before A took effect. This file is
// the library's side of a request from the app (the reserved XINPUT bit 0x0800 in the pad the app publishes; the game never sees
// it): the library presses the buttons itself, on the pad reads, and hides the MAP-tab frames.
//
// What was measured (thor-maptap, Thor, v656, warm, diag-stick presses; local-analysis/agents/thor-maptap/trials.tsv, video/):
//  - START opens the menu; the game's own pause blur covers the frame at once, the MAP tab's content fades in ~200 ms later.
//  - The MAP tab acts on A's RELEASE (A held 1.5 s: MAP tab until the release, then the full map). A released 251-271 ms after
//    START's release = MAP tab (6/6), 301 ms = full map (6/6, START 30-40 ms): the menu ignores A until ~0.3 s after it opened.
//    The first open after a while needed more (A released 370 ms after START's release = MAP tab).
//  - Every MAP-tab frame draws the map window's operations with the guest scissor (207,159)-(1078,591) on the 1280-pitch main
//    surface (census 2026-10-24 c3; thor-play-1021 g2-map-1); the full map draws the same shaders with (1,1)-(1280,720).
// So the library knows, per frame, whether the MAP tab is on screen. The macro (Machine):
//  Start    START held from the first read that carries the request until >= StartMinPresents presents and >= StartMinMs.
//  WaitTab  nothing pressed; the first presented frame that drew the MAP tab -> PressA.
//  PressA   A held; released once TabFramesBeforeRelease presents with the tab have passed (the menu takes input ~1-2 frames after
//           its content starts to fade in) and A was down >= AMinPresents presents and AMinMs (A acts on its release).
//  Verify   DoneFrames presents without the MAP tab = the full map is up: the end. The MAP tab still there RetryMs after the
//           release (the menu was not ready yet, e.g. the first open after a load) = A once more (PressA), at most RetryMax times.
// v661 (the owner: "can't be improved more?"; v659's own taps: MAP tab drawn 566-598 ms after the request, A up 633-665 ms, full map
// ~0.7 s, while the menu takes A ~0.3 s after START's release): A is pressed at the first frame that draws the game's pause blur (the
// menu opened; no wheel button held in the last 600 ms) and released earlyMs (300) after START's release; the full map's own draws
// (scissor (1,1)-(1280,720)) end the episode. A MAP tab after that release (the menu was not ready: the first open after a load) =
// the v658 path: A again, released 2 MAP-tab frames later.
// Every frame of the episode that drew the MAP tab is replaced by the last frame that did not (the game's own pause blur, or the
// last game frame): the player sees the game -> its own pause blur -> the full map. Never stuck: an episode ends after
// EpisodeMaxMs whatever happens; no MAP tab within TabWaitMs of START = the menu did not open (the end, nothing more pressed).
// Every tap is a request (the app keeps no guess of its own): the library knows whether the MAP tab is on screen (draws) and whether
// the full map it opened is still up (no START / B of the player's and no MAP tab since): MAP tab = A alone; its full map = START
// alone (closes the menu); otherwise the open macro above. (2026-10-24 device: the app's own guess stayed "open" after a request the
// game ignored during a phone call, and its next tap opened the menu instead.)
// Injected buttons are OR'd into the player's pad with a fresh packet number (as diag-stick does); the request bit is always removed.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace recomp::gpu::maptap {

// The capability marker the app looks for in the library (zero-terminated, WheelTouch scans private-trace-live.so for it).
inline constexpr char Marker[]="recomp-map-tap-v1";
constexpr std::uint16_t Request=0x0800,Start=0x0010,PadA=0x1000,PlayerB=0x2000; // (0x0800: reserved in XINPUT_GAMEPAD; the app's JNI passes it)
constexpr int TabX0=207,TabY0=159,TabX1=1078,TabY1=591; // the MAP tab's map window (guest pixels)

// A draw's guest scissor on a pass of passWidth: the MAP tab's map window (+-TabSlack pixels)?
// v703 (cross-port 2026-10-24, CROSS-PORT-LOG 15:25 / 15:31 / 16:25): the iPad's MAP tab measured 206,158..1075,588, the Thor's
// 207,159..1078,591; the +-1 match with an exact x0 never fired on the iPad. +-4 on every edge covers both and any phone's rounding.
constexpr int TabSlack=4;
constexpr bool tabScissor(int x0,int y0,int x1,int y1,unsigned passWidth) noexcept {
    const auto near=[](int a,int b){return a-b<=TabSlack && b-a<=TabSlack;};
    return passWidth==1280 && near(x0,TabX0) && near(y0,TabY0) && near(x1,TabX1) && near(y1,TabY1);
}

struct Params {
    std::int64_t startMinMs=50;std::uint64_t startMinPresents=2;  // START down: at least two game frames see it
    std::int64_t earlyMs=300;                                      // v661: A pressed at the first pause-blur frame, released this long
                                                                   // after START's release (0 = off: A only once the MAP tab is drawn)
    std::uint64_t tabFramesBeforeRelease=2;                        // presents with the MAP tab before A is released
    std::int64_t aMinMs=50;std::uint64_t aMinPresents=2;           // A down at least this long
    std::int64_t retryMs=300;unsigned retryMax=3;                  // the MAP tab still there this long after a release: A again
    std::int64_t tabWaitMs=2500;                                   // no MAP tab this long after START: the menu did not open
    std::int64_t episodeMaxMs=8000;                                // hard cap of an episode (v665: 8 s; the first open after a load took 3-4 s)
    std::int64_t requestGapMs=250;                                 // a new request needs the bit absent this long
    std::uint64_t doneFrames=2;                                    // presents without the MAP tab after a release = the full map
    bool hide=true;                                                // replace the MAP-tab frames (false: the macro alone)
    bool off=false;                                                // requests ignored (the bit is still removed)
};
// files/private-trace/diag-maptap-tune (optional, read at each request): "key=value" words, e.g. "tab=3 retry=250 hide=0 early=0".
inline void parseTune(const char* text,Params& p) noexcept {
    if(!text)return;
    const char* s=text;
    while(*s) {
        while(*s==' ' || *s=='\n' || *s=='\t' || *s=='\r')++s;
        char key[16]{};long long v=0;int used=0;
        if(std::sscanf(s,"%15[a-z]=%lld%n",key,&v,&used)==2 && used>0 && v>=0 && v<=60000) {
            const auto is=[&](const char* k){for(int i=0;;++i){if(key[i]!=k[i])return false;if(!k[i])return true;}};
            if(is("startms"))p.startMinMs=v;else if(is("startframes"))p.startMinPresents=std::uint64_t(v);
            else if(is("tab"))p.tabFramesBeforeRelease=std::uint64_t(v);else if(is("ams"))p.aMinMs=v;else if(is("aframes"))p.aMinPresents=std::uint64_t(v);
            else if(is("retry"))p.retryMs=v;else if(is("retries"))p.retryMax=unsigned(v);else if(is("wait"))p.tabWaitMs=v;
            else if(is("max"))p.episodeMaxMs=v;else if(is("done"))p.doneFrames=std::uint64_t(v);else if(is("hide"))p.hide=v!=0;
            else if(is("off"))p.off=v!=0;else if(is("early"))p.earlyMs=v;
            s+=used;
        } else {while(*s && *s!=' ' && *s!='\n')++s;}
    }
}

enum class Phase:std::uint8_t {Idle,Start,WaitTab,PressA,Verify,PanTo};
enum class End:std::uint8_t {None,Full,NoMenu,Retries,Timeout,Closed,Placed};
enum class Mode:std::uint8_t {Open,Enter,Close,Waypoint};
inline const char* endName(End e) noexcept {
    switch(e){case End::Full:return "full-map";case End::NoMenu:return "no-menu";case End::Retries:return "retries";case End::Timeout:return "timeout";
        case End::Closed:return "closed";case End::Placed:return "a-pressed";default:return "-";}
}
inline const char* modeName(Mode m) noexcept {return m==Mode::Enter?"enter":m==Mode::Close?"close":m==Mode::Waypoint?"waypoint":"open";}
// v663 (touch map, owner 2026-10-24: "drag to pan, pinch to zoom, tap for a waypoint"): the app's touchpad channel. While the bit Touch
// (0x0400, XINPUT's guide bit: the game ignores it, the Thor's mapper sets it only for a MODE key) is in the pad, the app has put a
// one-finger drag into the left stick and a pinch into LT / RT; the library passes them to the game only while the pause full map is
// on screen (its own draws, in a paused frame), else it gives the game a resting left stick and released triggers for those reads.
// Touch with Request = a tap outside the Resume button: on the full map A (set / remove the waypoint at the cursor), elsewhere the
// usual request (open the map). Request alone = a tap on the Resume button: on the full map START (resume).
// v667 (owner 2026-10-24: "the touch controls on the map need to be on the top screen"): the gestures come from the MAIN screen and
// travel in the RIGHT stick (the player's own left stick and triggers are never replaced outside the map): a Touch read carries a drag
// as RX / RY = the left stick wanted, a pinch as RX = PinchCode and RY = the trigger wanted (+ RT, - LT); on the full map the library
// moves them to LS / LT / RT, elsewhere it only rests the right stick for those reads. Touch with Request = a tap on the main screen,
// RX / RY = its offset from the screen's centre in guest pixels (1280x720): on the full map the cursor is panned to it (PanTo, from the
// map's own transform, MapXform) and A sets the waypoint there; elsewhere nothing at all. Request alone = the lower screen's tap.
constexpr std::uint16_t Touch=0x0400;
constexpr std::int16_t PinchCode=-32768;
// v701 (qol-touch, owner 2026-10-24: "long pressing the map should allow you to go back to gameplay like the thor, only on the main
// screen"): Touch | Request with RX = RY = CloseCode = the single-screen app's long press with no control or zone under it: on the full
// map or any pause screen (pausedFrame) the usual request (START: Close, back to the game), in the game nothing at all. The Thor app never
// sends it (its taps carry a guest-pixel offset, which is never (-32768, -32768)).
constexpr std::int16_t CloseCode=-32768;
struct Sticks {std::int16_t* lx=nullptr;std::int16_t* ly=nullptr;std::uint8_t* lt=nullptr;std::uint8_t* rt=nullptr;std::int16_t* rx=nullptr;std::int16_t* ry=nullptr;};
// v667: the pause map's projection as its own draws carry it (vertex constants c8 / c9 / c11 of the map layer's tile and vector draws, ps
// 4a524266 / 6ed05821 on the 1280 pass with the full-map scissor; census 2026-10-24 out: c8.x = 0.0033, c9.y = 0.0062, c11 = (3.4995,
// -1.0895, ...)): clip x = wx * sx + tx, clip y = wy * sy + ty; guest pixel = ((clip x + 1) * 640, (1 - clip y) * 360).
struct MapXform {float sx=0,sy=0,tx=0,ty=0;std::uint64_t present=0;bool ok() const noexcept {return sx!=0 && sy!=0;}};
constexpr std::uint64_t MapTilePs=0x4a5242663895a27dull,MapVectorPs=0x6ed058218489bbbeull;
// (v673 device, v672: 4 px was never reached - the stick's smallest push past the game's dead zone moves the map ~3-5 px a frame, so it
// circled 5.7 px off for the rest of the 3 s; 8 guest px = 12 px of the main panel, the size of the cursor's centre ring)
constexpr float PanStopPx=8.0f,PanMinStick=0.3f,PanGainPx=60.0f,PanTimeoutPx=24.0f; // (guest px; the stick from the remaining distance)
constexpr std::int64_t PanMaxMs=3000;

// What a presented frame drew (the renderer's draw hook, v661): the MAP tab's map window, the full map's (scissor (1,1)-(1280,720)),
// the game's pause blur (ps 0ce70535f29e499b on the main surface: census 2026-10-24 c3 / c4, absent from game frames c1 / c2; the
// weapon wheel's blur draws it too - the macro only looks right after its own START, with no wheel button held).
constexpr unsigned SeenTab=1u,SeenFull=2u,SeenBlur=4u,SeenPost=8u;
constexpr std::uint64_t BlurPs=0x0ce70535f29e499bull;
// v661: the game's tonemap (post_fidelity.h PostfxShaders[0]): every game frame draws it, the pause menu's frames do not (census c1 / c2
// vs c3 / c4; the weapon wheel's blur frames do: thor-play-1021 wheel-0..2). Paused = the pause blur without the tonemap.
constexpr std::uint64_t TonemapPs=0x08dfab6c16ec854cull;
constexpr bool pausedFrame(unsigned seen) noexcept {return (seen&SeenBlur) && !(seen&SeenPost);}
constexpr bool fullScissor(int x0,int y0,int x1,int y1,unsigned passWidth) noexcept {return passWidth==1280 && x0==1 && y0==1 && x1==1280 && y1==720;}

// What one episode did (the renderer logs it once the episode ends): times in ms from the first read with the request.
struct Summary {
    std::uint64_t n=0;End end=End::None;Mode mode=Mode::Open;
    std::int64_t startUpMs=-1,blurMs=-1,tabMs=-1,firstReleaseMs=-1,lastReleaseMs=-1,doneMs=-1;
    std::uint64_t startPresents=0,tabToRelease=0;unsigned retries=0,held=0,tabFrames=0,panFrames=0;bool early=false,fullSeen=false,panned=false;
    int tap[2]{};float xf[4]{};float panLeft=-1; // (v671: the waypoint tap, the projection at the tap, the distance left when A was pressed)
};

class Machine {
public:
    // A pad read of user 0 (any thread): the buttons the game is given (the request removed, the macro's buttons OR'd).
    // *fresh: the macro's buttons differ from the player's alone - the caller gives the read a packet number of its own (serial).
    std::uint16_t read(std::uint16_t buttons,std::int64_t now,bool* fresh,std::uint32_t* serial,const Sticks& sticks=Sticks{}) noexcept {
        const bool request=(buttons&Request)!=0;bool touch=(buttons&Touch)!=0;buttons=std::uint16_t(buttons&~(Request|Touch));
        if(fresh)*fresh=false;
        std::int16_t tapX=0,tapY=0;
        if(touch) { // v667: the main screen's gesture, in the right stick
            const std::int16_t x=sticks.rx?*sticks.rx:0,y=sticks.ry?*sticks.ry:0;
            if(sticks.rx)*sticks.rx=0;if(sticks.ry)*sticks.ry=0; // (the game never sees the gesture in the right stick)
            if(request){tapX=x;tapY=y;}
            else if(mapShown_.load(std::memory_order_relaxed) && !busy_.load(std::memory_order_relaxed)) {
                if(x==PinchCode){const int v=std::clamp<int>(y,-255,255);if(sticks.rt)*sticks.rt=std::uint8_t(v>0?v:0);if(sticks.lt)*sticks.lt=std::uint8_t(v<0?-v:0);
                    if(sticks.lx)*sticks.lx=0;if(sticks.ly)*sticks.ly=0;}
                else {if(sticks.lx)*sticks.lx=x;if(sticks.ly)*sticks.ly=y;}
            }
            if(request && x==CloseCode && y==CloseCode) { // v701 (qol-touch): a long press on a pause screen = back to the game
                if(!mapShown_.load(std::memory_order_relaxed) && !pausedNow_.load(std::memory_order_relaxed))return buttons; // (in the game: nothing)
                touch=false; // (from here on the usual request: on the full map / a pause screen = START, Close)
            }
            else if(request && !mapShown_.load(std::memory_order_relaxed))return buttons; // (a main-screen tap off the map: nothing)
        }
        if(!request && !busy_.load(std::memory_order_relaxed) && !((buttons&(Start|PlayerB)) && mapOpen_.load(std::memory_order_relaxed)) &&
           !(buttons&WheelButtons) && !((buttons&Start) && pausedNow_.load(std::memory_order_relaxed)))return buttons;
        std::lock_guard l(m_);
        if((buttons&Start) && pausedNow_.load(std::memory_order_relaxed) && !(lastStart_))lastClose_=now; // (v677: the player's own START closes a pause screen)
        lastStart_=(buttons&Start)!=0;
        if(buttons&(Start|PlayerB))mapOpen_.store(false,std::memory_order_relaxed); // (the player's own START / B: the menu's state is theirs)
        if(buttons&WheelButtons)lastWheel_=now; // (LB / D-pad LEFT: a wheel's blur is no pause menu)
        if(request) {
            lastRequest_=now;
            if(armed_ && phase_==Phase::Idle && !p_.off && !loading_.load(std::memory_order_relaxed))begin(now,touch,tapX,tapY); // (thor-char v685: not during a switch)
        } else if(!armed_ && now-lastRequest_>=p_.requestGapMs)armed_=true;
        advance(now);
        busy_.store(phase_!=Phase::Idle || !armed_,std::memory_order_relaxed);
        if(inject_) {buttons=std::uint16_t(buttons|inject_);if(fresh)*fresh=true;if(serial)*serial=serial_;}
        if(phase_==Phase::PanTo){if(sticks.lx)*sticks.lx=panX_;if(sticks.ly)*sticks.ly=panY_;if(fresh)*fresh=true;if(serial)*serial=serial_;} // (v667)
        return buttons;
    }
    // v667: the renderer's draw hook: this frame's map projection (from a map-layer draw's vertex constants).
    void noteXform(float sx,float sy,float tx,float ty) noexcept {std::lock_guard l(xm_);xform_={sx,sy,tx,ty,presents_+1};}
    // The app's parameters for the next request (read by the caller at a request; never during an episode).
    void tune(const Params& p) noexcept {std::lock_guard l(m_);if(phase_==Phase::Idle)p_=p;}
    Params params() noexcept {std::lock_guard l(m_);return p_;}
    // Whether an episode runs.
    bool watching() const noexcept {return watch_.load(std::memory_order_relaxed);}
    struct Frame {bool active=false,hold=false,ended=false;Summary summary;};
    // The renderer's present of a real frame (renderer thread) with what it drew (Seen* bits). Returns whether an episode runs (the
    // renderer keeps a copy of non-tab frames) and whether this frame is replaced by that copy.
    Frame present(std::int64_t now,unsigned seen) noexcept {
        const bool tab=(seen&SeenTab)!=0,full=(seen&SeenFull)!=0 && !tab,paused=pausedFrame(seen);
        std::lock_guard l(m_);
        ++presents_;
        if(paused)lastPaused_=presents_;
        pausedNow_.store(paused,std::memory_order_relaxed);
        MapXform xf;{std::lock_guard xl(xm_);xf=xform_;}
        const bool xfNow=xf.ok() && xf.present==presents_;
        if(full && paused)lastFull_=presents_; // v663: the full map on screen
        mapShown_.store(lastFull_ && presents_-lastFull_<=3,std::memory_order_relaxed);
        if(tab){lastTab_=presents_;mapOpen_.store(false,std::memory_order_relaxed);} // (the MAP tab on screen: not the full map)
        Frame f;
        if(phase_==Phase::Idle){if(pendingEnd_){f.ended=true;f.summary=sum_;pendingEnd_=false;}return f;}
        if(tab)++sum_.tabFrames;
        if(full)sum_.fullSeen=true;
        switch(phase_) {
            case Phase::WaitTab:
                if(tab){tabSeen(now);press(now,false);}
                else if(paused && p_.earlyMs>0 && now-lastWheel_>WheelGraceMs){sum_.blurMs=now-t0_;press(now,true);} // v661: the menu opened
                break;
            case Phase::PressA:
                if(tab && !sawTab_)tabSeen(now); // (the MAP tab while the early A is down: released at the later of the two times)
                break;
            case Phase::PanTo: { // v667: the target's place on screen now, from this frame's projection; A once it is under the cursor
                if(!xfNow)break;
                const float gx=(target_[0]*xf.sx+xf.tx+1.0f)*640.0f-640.0f,gy=(1.0f-(target_[1]*xf.sy+xf.ty))*360.0f-360.0f,d=std::hypot(gx,gy);
                sum_.panLeft=d;
                if(d<=PanStopPx){panX_=panY_=0;setInject(PadA);phase_=Phase::PressA;phaseMs_=now;phasePresent_=presents_;early_=false;sawTab_=false;break;}
                const float k=std::min(1.0f,PanMinStick+d/PanGainPx)/d;
                panX_=std::int16_t(std::lround(std::clamp(gx*k,-1.0f,1.0f)*32767.0f));panY_=std::int16_t(std::lround(std::clamp(-gy*k,-1.0f,1.0f)*32767.0f));
                ++serial_;sum_.panFrames++;
                break;}
            case Phase::Verify:
                if(full){finish(now,End::Full);break;}                 // v661: the full map's own draws
                if(tab){if(!sawTab_)tabSeen(now);noTab_=0;}else if(sawTab_)++noTab_; // (v661: a MAP tab after an early A = the menu was not ready)
                if(sawTab_ && noTab_>=p_.doneFrames){finish(now,End::Full);break;}
                // A again only when the MAP tab is still there retryMs after the release (a MAP tab still shown for a frame or two while
                // the full map comes is no reason: that A would land in the full map = a waypoint)
                if(tab && now-releaseMs_>=p_.retryMs) {
                    if(sum_.retries>=p_.retryMax){finish(now,End::Retries);break;}
                    ++sum_.retries;press(now,false);
                }
                break;
            default:break;
        }
        advance(now);
        f.active=phase_!=Phase::Idle || pendingEnd_;
        f.hold=tab && p_.hide && (phase_!=Phase::Idle || (pendingEnd_ && sum_.end==End::Full));
        if(f.hold)++sum_.held;
        if(phase_==Phase::Idle && pendingEnd_){f.ended=true;f.summary=sum_;pendingEnd_=false;}
        return f;
    }
    Frame present(std::int64_t now,bool tab) noexcept {return present(now,tab?SeenTab:0u);}
    Phase phase() noexcept {std::lock_guard l(m_);return phase_;}
    bool mapOpen() const noexcept {return mapOpen_.load(std::memory_order_relaxed);}
    bool mapShown() const noexcept {return mapShown_.load(std::memory_order_relaxed);}
    // v667: the pressed look of the lower screen's Resume pill: a Close request within the last 600 ms (until the menu has gone).
    bool closing(std::int64_t now) noexcept {std::lock_guard l(m_);return now-lastClose_<600;}
    // v677: when the pause screen was last closed (the lower screen's tap, or the player's own START during a pause), ms.
    std::int64_t lastCloseMs() noexcept {std::lock_guard l(m_);return lastClose_;}
    std::uint16_t injected() noexcept {std::lock_guard l(m_);return inject_;}
    bool wheelRecent(std::int64_t now) noexcept {std::lock_guard l(m_);return now-lastWheel_<=WheelGraceMs;}
    // thor-char (v685): a character switch is running (the lower screen says Loading): taps on the lower screen do nothing.
    void setLoading(bool on) noexcept {loading_.store(on,std::memory_order_relaxed);}
    std::atomic<bool> loading_{false};
    static constexpr std::uint16_t WheelButtons=0x0104; // LB, D-pad LEFT
    static constexpr std::int64_t WheelGraceMs=600;
private:
    // A request: the MAP tab on screen (the menu is open on it) = A alone (Enter); the full map this macro opened and nothing since
    // (no START / B of the player's, no MAP tab), or (v661) any other pause screen = START alone (Close, "Resume Game"); otherwise
    // START, then A (Open).
    // v663: a touch tap (outside the Resume button) while the full map is on screen = A alone (Waypoint).
    void begin(std::int64_t now,bool touchTap,std::int16_t tapX=0,std::int16_t tapY=0) noexcept {
        armed_=false;t0_=phaseMs_=now;phasePresent_=presents_;
        sum_=Summary{};sum_.n=++episodes_;noTab_=0;tabFirst_=0;sawTab_=false;early_=false;watch_.store(true,std::memory_order_relaxed);
        if(touchTap && mapShown_.load(std::memory_order_relaxed)) {
            sum_.mode=Mode::Waypoint;
            MapXform xf;{std::lock_guard xl(xm_);xf=xform_;}
            // (v671: xf.present is the frame being drawn when noted = presents_ + 1 before its present: v670's presents_ - xf.present wrapped)
            if(xf.ok() && xf.present<=presents_+1 && presents_+1-xf.present<=4 && (tapX || tapY)) { // v667: the tapped point in map units
                const float cx=(640.0f+float(tapX))/640.0f-1.0f,cy=1.0f-(360.0f+float(tapY))/360.0f;
                target_[0]=(cx-xf.tx)/xf.sx;target_[1]=(cy-xf.ty)/xf.sy;phase_=Phase::PanTo;panX_=panY_=0;sum_.panned=true;
                sum_.tap[0]=tapX;sum_.tap[1]=tapY;sum_.xf[0]=xf.sx;sum_.xf[1]=xf.sy;sum_.xf[2]=xf.tx;sum_.xf[3]=xf.ty;
            } else press(now,false);
        }
        else if(lastTab_ && presents_-lastTab_<=2){
            sum_.mode=Mode::Enter;sawTab_=true;tabFirst_=presents_>p_.tabFramesBeforeRelease?presents_-p_.tabFramesBeforeRelease:0;sum_.tabMs=0;press(now,false);
        }
        else if(mapOpen_.load(std::memory_order_relaxed) || (lastPaused_ && presents_-lastPaused_<=2)){ // (v661: or any pause screen but the MAP tab)
            sum_.mode=Mode::Close;mapOpen_.store(false,std::memory_order_relaxed);phase_=Phase::Start;setInject(Start);lastClose_=now;}
        else {sum_.mode=Mode::Open;phase_=Phase::Start;setInject(Start);}
    }
    void tabSeen(std::int64_t now) noexcept {tabFirst_=presents_;sawTab_=true;if(sum_.tabMs<0)sum_.tabMs=now-t0_;}
    void press(std::int64_t now,bool early) noexcept {phase_=Phase::PressA;phaseMs_=now;phasePresent_=presents_;early_=early;setInject(PadA);}
    void finish(std::int64_t now,End e) noexcept {
        phase_=Phase::Idle;setInject(0);sum_.end=e;sum_.doneMs=now-t0_;pendingEnd_=true;watch_.store(false,std::memory_order_relaxed);
        if(e==End::Full){mapOpen_.store(true,std::memory_order_relaxed);sum_.early=sum_.blurMs>=0 && !sawTab_;}
    }
    void setInject(std::uint16_t b) noexcept {if(b!=inject_){inject_=b;++serial_;}}
    void advance(std::int64_t now) noexcept {
        if(phase_==Phase::Idle)return;
        if(now-t0_>=p_.episodeMaxMs){finish(now,End::Timeout);return;}
        switch(phase_) {
            case Phase::Start:
                if(now-phaseMs_>=p_.startMinMs && presents_-phasePresent_>=p_.startMinPresents) {
                    phase_=Phase::WaitTab;phaseMs_=startUpAt_=now;setInject(0);sum_.startUpMs=now-t0_;sum_.startPresents=presents_-phasePresent_;
                    if(sum_.mode==Mode::Close)finish(now,End::Closed);
                }
                break;
            case Phase::WaitTab:
                if(now-phaseMs_>=p_.tabWaitMs)finish(now,End::NoMenu);
                break;
            case Phase::PanTo:
                if(now-phaseMs_>=PanMaxMs) { // (time up: A when it got close, else nothing - never a waypoint far from the tap)
                    if(sum_.panLeft>=0 && sum_.panLeft<=PanTimeoutPx){panX_=panY_=0;setInject(PadA);phase_=Phase::PressA;phaseMs_=now;phasePresent_=presents_;early_=false;sawTab_=false;}
                    else finish(now,End::Timeout);
                }
                break;
            case Phase::PressA: {
                const bool held=presents_-phasePresent_>=p_.aMinPresents && now-phaseMs_>=p_.aMinMs;
                const bool due=(!early_ || now-startUpAt_>=p_.earlyMs) && (!sawTab_ || presents_-tabFirst_>=p_.tabFramesBeforeRelease);
                if(held && due && sum_.mode==Mode::Waypoint){sum_.firstReleaseMs=sum_.lastReleaseMs=now-t0_;finish(now,End::Placed);break;}
                if(held && due) {
                    phase_=Phase::Verify;setInject(0);releaseMs_=now;noTab_=0;
                    if(sum_.firstReleaseMs<0){sum_.firstReleaseMs=now-t0_;sum_.tabToRelease=sawTab_?presents_-tabFirst_:0;}
                    sum_.lastReleaseMs=now-t0_;
                }
                break;}
            case Phase::Verify:
                // (an early A, then neither the map nor its tab while the game is no longer paused; v665: a pause still on screen = the menu is
                // still loading - v663 device, first open after a load: 3 s at 2 fps, the MAP tab came after v663's 2.5 s and stayed shown)
                if(!sawTab_ && now-releaseMs_>=p_.tabWaitMs && !(lastPaused_ && presents_-lastPaused_<=2))finish(now,End::NoMenu);
                break;
            default:break;
        }
    }
    std::mutex m_;Params p_;Phase phase_=Phase::Idle;
    std::atomic<bool> busy_{false},watch_{false},mapOpen_{false},mapShown_{false};
    bool armed_=true,pendingEnd_=false,sawTab_=false,early_=false;
    std::int64_t lastRequest_=-(std::int64_t(1)<<40),lastWheel_=-(std::int64_t(1)<<40),t0_=0,phaseMs_=0,releaseMs_=0,startUpAt_=0;
    std::uint64_t presents_=0,phasePresent_=0,tabFirst_=0,noTab_=0,episodes_=0,lastTab_=0,lastPaused_=0,lastFull_=0;
    std::uint16_t inject_=0;std::uint32_t serial_=0;Summary sum_;
    std::int64_t lastClose_=-(std::int64_t(1)<<40);bool lastStart_=false;std::atomic<bool> pausedNow_{false};
    std::mutex xm_;MapXform xform_;float target_[2]{};std::int16_t panX_=0,panY_=0; // (v667)
};
inline Machine& machine() noexcept {static Machine m;return m;}
// v661 (owner 2026-10-24: "on the map screen ... make a resume game button", "GTA's own pause menu obv, ideally should be the same color
// as the map screen"): while the game shows a pause screen the lower screen is a Resume Game button on the pause screen's own colour.
// (v663: v661's ResumeGate (2-frame hysteresis) and dominantColour (live sampling) removed: the owner saw the delay and a wrong shade.)
// v665 (owner 2026-10-24: "does it change when outdoors?", "maybe you can make it dynamic?"; main: from the game's own map drawing, not
// from screen pixels, which caught the blur): the pause map screen's colour from the MAP LAYER itself. Censuses 2026-10-24 (c4 indoor,
// out outdoor): the game draws the map (base tiles 4a524266 with 512x512 k_DXT4_5 atlas tiles, vectors 6ed05821, the interior plan
// 79c5b389) on its own 1280x720 surface cleared to (0,0,0,0), resolves it, then on the frontend surface draws the blurred backdrop
// (62ff5489), an untextured tint quad (d2452cdc: its vertex colour was white, then transparent black - no background colour) and the
// map layer over them with ps e2649e6b, sampler s2 = the layer (MapLayerPs). The renderer blits that layer (bytes in the order the
// screen shows them, as the present image) to 16x9 texels every MapLayerEvery-th paused frame and takes the dominant colour of the
// opaque texels (layerColour); fewer than MapLayerMinShare opaque texels (the port's outdoor map: no land / sea layer drawn) = the
// fixed #A3A6AA.
constexpr std::uint64_t MapLayerPs=0xe2649e6bc8ee4db8ull;
constexpr unsigned MapLayerW=16,MapLayerH=9,MapLayerEvery=4,MapLayerLatency=4;
constexpr float MapLayerMinShare=0.2f;
// The colours of n texels `rgba` (RGBA8) where the map layer's texels `mask` are opaque (alpha >= 230): the most common colour (4 bits
// per channel), its mean; false = too few opaque layer texels. v665 device: the layer itself is brighter than the screen (182,186,193
// in the layer, 163,166,170 on screen: the composite over the backdrop darkens it), so `rgba` is the present image at the same 16x9
// points: the colour the main screen shows where the map is opaque (never the blur: those points are masked out). mask = rgba: the
// layer's own colour.
inline bool layerColour(const std::uint8_t* rgba,const std::uint8_t* mask,unsigned n,std::uint8_t out[3]) noexcept {
    if(!n)return false;
    std::vector<std::uint16_t> count(4096,0);unsigned opaque=0;
    const auto key=[&](unsigned i){return unsigned(rgba[4*i]>>4)<<8|unsigned(rgba[4*i+1]>>4)<<4|unsigned(rgba[4*i+2]>>4);};
    for(unsigned i=0;i<n;++i)if(mask[4*i+3]>=230){++opaque;++count[key(i)];}
    if(!opaque || float(opaque)<MapLayerMinShare*float(n))return false;
    unsigned best=0;for(unsigned k=1;k<4096;++k)if(count[k]>count[best])best=k;
    unsigned sum[3]{},m=0;
    for(unsigned i=0;i<n;++i)if(mask[4*i+3]>=230 && key(i)==best){++m;for(unsigned c=0;c<3;++c)sum[c]+=rgba[4*i+c];}
    for(unsigned c=0;c<3;++c)out[c]=std::uint8_t((sum[c]+m/2)/m);
    return true;
}

// The renderer's per-frame Seen* bits (set by draw(), taken by present()).
inline std::atomic<unsigned>& seenDrawn() noexcept {static std::atomic<unsigned> b{0};return b;}

} // namespace recomp::gpu::maptap
#endif /* RECOMP_THOR_EDITION */
