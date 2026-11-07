// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-touchpad (Claude 2026-10-24, iPad build 235; iOS tree 1): the universal touch controls (owner 16:45: "These are the universal touch
// controls, and will be the controls on the fold, and on the IOS build, as well as the Duo build in LANDSCAPE MODE"; "ipads touch controller
// doesnt even work. Make your own"; "both analog sticks should be animated"; "we need a hide touch controls button too on all builds";
// "when a gamepad is detected, it auto hides the touch controls"). Spec: local-analysis/touch-controls-spec/SPEC.md v2. The behaviour is a
// port of android-qol's TouchPad.java (fingers, floating drag-follow sticks, the XInput-like curve, analog trigger slides, the aim latch, the
// sprint auto-tap, tap-or-wheel, the hide button, auto-hide, arbitration, fade); the slots are SPEC v2 3.2 (corners, column, small row).
// Pure C++ (no UIKit): tests_touch_pad.cpp runs it on the host.
//
// Coordinates: view pixels. The frame is the HUD's 16:9 rectangle inside the view (ios_qol_gesture.h Frame: fill crop + the HUD shrink), so
// the controls sit where the HUD's radar and weapon icon are. Positions in frame units are moved into the space outside the frame by half
// of it where that helps (SPEC 3.1). Time: monotonic milliseconds. A touch that starts on a control is that control for its whole life;
// one that starts in an active long-press zone (and on no control) is not taken (down() returns false: the gesture gets it); any other
// touch is the left or right floating stick by side.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "ios_qol_gesture.h"

namespace recomp::apple::touchpad {
// XInput bits (XINPUT_GAMEPAD_*)
constexpr int DU=0x1,DD=0x2,DL=0x4,DR=0x8,START=0x10,BACK=0x20,L3=0x40,R3=0x80,LB=0x100,RB=0x200,A=0x1000,B=0x2000,X=0x4000,Y=0x8000;
// control contexts (touch_context.h; recomp_touch_context_v1)
constexpr int NONE=0,FOOT=1,CAR=2,BIKE=3,BICYCLE=4,BOAT=5,SUB=6,HELI=7,PLANE=8,TRAIN=9;
enum class Mode {Context,Pad,Off};
enum class Kind {Button,TriggerL,TriggerR,Fire,Wheel,TapOrWheel,Dpad,AimLatch,Autotap,Hide,More,WeaponTap,App,Steer}; // (App: SPEC 3.14, the app acts; nothing for the game. Steer: SPEC 3.16)
/** A wheel button: held past its tap = the wheel's button held (a slide steers); WeaponTap also opens the latched wheel on a tap. */
constexpr bool wheelKind(Kind k) noexcept {return k==Kind::Wheel || k==Kind::TapOrWheel || k==Kind::WeaponTap;}
// 360 colours (ARGB) and the neutral one
constexpr std::uint32_t GREEN=0xFF5BB53A,RED=0xFFD9312B,BLUE=0xFF2E6FD8,YELLOW=0xFFF2B51C,GREY=0xFF9AA0A6;
// The icon a control draws (ios_touch_pad_icons.h; no words on the buttons, SPEC 3.5).
enum class Icon {Hide,Pause,Cam,Phone,Sprint,Jump,Fire,Aim,Melee,Weapons,Switch,Enter,Cover,Stealth,Look,Gas,Brake,Handbrake,DriveBy,
                 Lights,Radio,Exit,Horn,Cinema,Up,Down,YawR,YawL,Missile,WeaponCam,Gear,Pedal,TurnL,TurnR,Dive,Rise,Face,Shoulder,DpadIcon,Stick3,More,Skip,WeaponGroup,Prompt,
                 Qsave,Trainer,PhoneOk,PhoneBack,CallAnswer,CallDecline,SteerL,SteerR}; // (SPEC 3.14 / 3.15: drawn by id from icons.txt; Trainer: ios_qol_touch_view.h drawTrainer)
// SPEC 3.10 (owner 2026-10-24 ~21:15: "touch controls should not appear in menus or cutscenes"): the overlay, from the library's word
// (first match wins): MAP (bit 15) no controls; SKIP (cutscene bit 19, faded out bit 20, control off bit 21) a faded SKIP after a touch;
// MENU (frontend bit 18) the menu set; PLAY the gameplay layout. The sticks are off in every overlay but PLAY.
// SPEC 3.15: PHONE = gameplay with the game's phone up (phoneOverlay; the library's recomp_phone_state_v1 bit 0).
enum class Overlay {Play,Map,Skip,Menu,Phone};

struct Control {
    const char* id;Icon icon;const char* glyph; // glyph: the 360 letter for Face / Shoulder / Stick3 icons (360 pad mode), else ""
    Kind kind;int bits;float fx,fy,d;std::uint32_t color;
    float cx=0,cy=0,r=0; // view pixels (layout)
    bool hit(float x,float y) const noexcept {const float dx=x-cx,dy=y-cy;return dx*dx+dy*dy<=r*r;}
    bool is(std::string_view s) const noexcept {return s==id;}
};
// tunables (SPEC 3, 4, 5)
constexpr float StickDead=0.10f,StickCurve=1.3f,LsXinputDead=7849.0f/32767.0f,RsXinputDead=8689.0f/32767.0f;
constexpr float LsR=0.13f,RsR=0.11f;
constexpr float LsHomeX=0.16f,LsHomeY=0.56f,RsHomeX=0.56f,RsHomeY=0.68f,LeftSide=0.45f;
constexpr float MinButtonMm=7.5f,MinRingMm=22.0f;
constexpr long long SpringMs=90,StickFadeMs=150,FadeIdleMs=4000,FadeMs=250,TapMs=250,DoubleTapMs=300,AutotapHalfMs=62,PulseMs=100;
// The Fold, 21:30: an 80 ms PAUSE tap never opened the pause menu (200 / 400 ms did): a tapped button is held at least MinPressMs,
// START / BACK at least MinMenuPressMs (through the pulse bits).
constexpr long long MinPressMs=100,MinMenuPressMs=250;
constexpr float IdleAlpha=0.35f;
constexpr int TriggerDown=180,TriggerMin=77,PhysicalTrigger=30;
// Slots (SPEC v2 3.2; frame units, d = diameter in frame heights)
struct Slot {float x,y,d;};
constexpr Slot CornerA{0.915f,0.83f,0.12f},CornerB{0.80f,0.89f,0.10f},CornerC{0.81f,0.68f,0.12f},CornerD{0.69f,0.80f,0.10f},
               CornerE{0.935f,0.645f,0.09f},CornerF{0.70f,0.62f,0.085f},Column1{0.945f,0.28f,0.085f},Column2{0.945f,0.40f,0.085f},
               Column3{0.945f,0.52f,0.085f},Row1{0.40f,0.92f,0.07f},Row2{0.46f,0.92f,0.07f},
               TopPhone{0.36f,0.06f,0.06f},TopCam{0.45f,0.06f,0.06f},TopPause{0.55f,0.06f,0.06f},HideSlot{0.035f,0.07f,0.06f},
               WeaponSlot{0.86f,0.16f,0.085f}, // (SPEC 3.12: top right, under the game's ammo count, on the weapon zone it stands for)
               // SPEC 3.12 v15 ("GTA 4 style", the default since the owner's OK; `wstyle=disc` = the old slot): right-aligned under the HUD
               WeaponSlotIv{0.934f,0.175f,0.10f},
               // SPEC 3.14 (Fold ship 2026-10-25, the Thor's lower-screen Quick Save and Lester trainer buttons on one screen): the top row's
               // left end, in the bar in Fit (the right end is the WEAPON button's long-press zone)
               TopSave{0.27f,0.06f,0.06f},TopTrainer{0.18f,0.06f,0.06f},
               // SPEC 3.15: the menu set's A / B slots, right of the game's phone
               PhoneOkSlot{0.90f,0.80f,0.12f},PhoneBackSlot{0.96f,0.60f,0.10f},
               // SPEC 3.16 (owner 2026-10-27: "we should have for cars left and right arrow buttons like the game"): the steering arrows,
               // left side above the minimap (its long-press zone stays free), the floating stick still on the rest of the left side
               SteerLSlot{0.07f,0.52f,0.13f},SteerRSlot{0.20f,0.52f,0.13f};
// SPEC 3.16: the arrows' directions (Control.bits of a Steer control, not pad bits); the vehicles steered left / right only.
constexpr int SteerLeft=1,SteerRight=2;
constexpr bool steerArrows(int ctx) noexcept {return ctx==CAR || ctx==BIKE || ctx==BICYCLE || ctx==BOAT;}
/** The left stick's x from the held arrows: full left / right; both = 0; none held = no override (NoSteer). */
constexpr int NoSteer=-0x7FFFFFFF-1;
constexpr int steerX(int held) noexcept {return held==SteerLeft?-32767:held==SteerRight?32767:held!=0?0:NoSteer;}

// The pad from the touches: XInput order and ranges (sticks -32767..32767 y up, triggers 0..255).
struct State {int buttons=0,lx=0,ly=0,rx=0,ry=0,lt=0,rt=0;};
struct Gesture {int buttons=0,rx=0,ry=0;}; // the long-press gestures' contribution (qol::Gesture::merge)

inline Control make(const char* id,Icon icon,Kind kind,int bits,Slot s,std::uint32_t color,const char* glyph="") {
    Control c{id,icon,glyph,kind,bits,s.x,s.y,s.d,color};return c;
}

/** The controls of one context (SPEC v2 2 and 3.2). */
inline std::vector<Control> contextLayout(int ctx,bool weaponIv=false) {
    std::vector<Control> l;
    l.push_back(make("hide",Icon::Hide,Kind::Hide,0,HideSlot,GREY));
    l.push_back(make("pause",Icon::Pause,Kind::Button,START,TopPause,GREY));
    l.push_back(make("cam",Icon::Cam,Kind::Button,BACK,TopCam,GREY)); // (PHONE: on foot only, as TouchPad.java base())
    switch(ctx) {
    case CAR:case BIKE:case BICYCLE:
        l.push_back(make("gas",Icon::Gas,Kind::TriggerR,0,CornerA,GREEN));
        l.push_back(make("brake",Icon::Brake,Kind::TriggerL,0,CornerB,RED));
        l.push_back(make("handbrake",Icon::Handbrake,Kind::Button,RB,CornerC,GREY));
        l.push_back(make("driveby",Icon::DriveBy,Kind::Button,LB,CornerD,GREY));
        if(ctx==CAR)l.push_back(make("lights",Icon::Lights,Kind::Button,DR,CornerE,GREY));
        if(ctx==BICYCLE)l.push_back(make("pedal",Icon::Pedal,Kind::Autotap,A,CornerE,GREEN));
        l.push_back(make("radio",Icon::Radio,Kind::TapOrWheel,DL,Column1,GREY));
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,Column2,YELLOW));
        l.push_back(make("horn",Icon::Horn,Kind::Button,L3,Column3,GREY));
        l.push_back(make("look",Icon::Look,Kind::Button,R3,Row1,GREY));
        if(ctx==CAR)l.push_back(make("cinema",Icon::Cinema,Kind::Button,B,Row2,RED));
        break;
    case BOAT:case SUB:
        l.push_back(make("throttle",Icon::Gas,Kind::TriggerR,0,CornerA,GREEN));
        l.push_back(make("reverse",Icon::Brake,Kind::TriggerL,0,CornerB,RED));
        if(ctx==SUB) {
            l.push_back(make("turnr",Icon::TurnR,Kind::Button,RB,CornerC,GREY));
            l.push_back(make("turnl",Icon::TurnL,Kind::Button,LB,CornerD,GREY));
            l.push_back(make("dive",Icon::Dive,Kind::Button,A,CornerE,GREEN));
            l.push_back(make("rise",Icon::Rise,Kind::Button,X,CornerF,BLUE));
        }
        l.push_back(make("radio",Icon::Radio,Kind::TapOrWheel,DL,Column1,GREY));
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,Column2,YELLOW));
        l.push_back(make("horn",Icon::Horn,Kind::Button,L3,Column3,GREY));
        break;
    case HELI:case PLANE: {
        const bool heli=ctx==HELI;
        l.push_back(make("up",Icon::Up,Kind::TriggerR,0,CornerA,GREEN));
        l.push_back(make("down",Icon::Down,Kind::TriggerL,0,CornerB,RED));
        l.push_back(make("yawr",Icon::YawR,Kind::Button,RB,CornerC,GREY));
        l.push_back(make("yawl",Icon::YawL,Kind::Button,LB,CornerD,GREY));
        l.push_back(make("fire",Icon::Missile,Kind::Button,X,CornerE,BLUE));
        l.push_back(make("radio",Icon::Radio,Kind::TapOrWheel,DL,Column1,GREY));
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,Column2,YELLOW));
        l.push_back(heli?make("wcam",Icon::WeaponCam,Kind::Button,R3,Column3,GREY):make("gear",Icon::Gear,Kind::Button,L3,Column3,GREY));
        break;}
    case TRAIN:
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,Column2,YELLOW));
        break;
    default: // FOOT (and NONE: loading / menus)
        l.push_back(make("sprint",Icon::Sprint,Kind::Autotap,A,CornerA,GREEN));
        l.push_back(make("jump",Icon::Jump,Kind::Button,X,CornerB,BLUE));
        l.push_back(make("fire",Icon::Fire,Kind::Fire,0,CornerC,RED));
        l.push_back(make("aim",Icon::Aim,Kind::AimLatch,0,CornerD,GREY));
        l.push_back(make("melee",Icon::Melee,Kind::Button,B,CornerE,RED));
        l.push_back(make("weapons",Icon::Weapons,Kind::Wheel,LB,CornerF,GREY));
        l.push_back(make("switch",Icon::Switch,Kind::Wheel,DD,Column1,GREY));
        l.push_back(make("enter",Icon::Enter,Kind::Button,Y,Column2,YELLOW));
        l.push_back(make("cover",Icon::Cover,Kind::Button,RB,Column3,GREY));
        l.push_back(make("stealth",Icon::Stealth,Kind::Button,L3,Row1,GREY));
        l.push_back(make("look",Icon::Look,Kind::Button,R3,Row2,GREY));
        l.push_back(make("phone",Icon::Phone,Kind::Button,DU,TopPhone,GREY));
        l.push_back(make("weapon",Icon::WeaponGroup,Kind::WeaponTap,LB,weaponIv?WeaponSlotIv:WeaponSlot,GREY)); // (SPEC 3.12)
        break;
    }
    return l;
}
// Layout B (SPEC 3.7, the GTA-mobile references taken literally; a candidate beside A, the owner picks; `layout=b`): 3-4 BIG buttons round
// the bottom-right corner, a column of small ones on the right edge, bigger sticks with the left one above the minimap, and MORE (top bar)
// opening a bottom row with the rest. Every control of layout A is in B, visible or in the MORE row (tests_touch_pad).
constexpr Slot BBig{0.905f,0.79f,0.20f},BBig2{0.775f,0.875f,0.16f},BMid1{0.785f,0.64f,0.15f},BMid2{0.86f,0.535f,0.15f},
               BCol1{0.945f,0.25f,0.085f},BCol2{0.945f,0.355f,0.085f},BCol3{0.945f,0.46f,0.085f},BCol4{0.945f,0.565f,0.085f},
               BMore{0.64f,0.06f,0.06f},BRow1{0.40f,0.92f,0.07f},BRow2{0.46f,0.92f,0.07f},BRow3{0.52f,0.92f,0.07f},BRow4{0.58f,0.92f,0.07f};
constexpr float BLsX=0.17f,BLsY=0.50f,BLsR=0.17f,BRsX=0.56f,BRsY=0.70f,BRsR=0.15f;
inline Slot sized(Slot s,float d){s.d=d;return s;}
/** Layout B of one context; more = the MORE row shown. */
inline std::vector<Control> contextLayoutB(int ctx,bool more,bool weaponIv=false) {
    // (SPEC 3.12 v15: the columns 0.035 lower, room for the weapon under the HUD)
    const float dy=weaponIv?0.035f:0.0f;
    const Slot BCol1{0.945f,0.25f+dy,0.085f},BCol2{0.945f,0.355f+dy,0.085f},BCol3{0.945f,0.46f+dy,0.085f},BCol4{0.945f,0.565f+dy,0.085f};
    std::vector<Control> l;
    l.push_back(make("hide",Icon::Hide,Kind::Hide,0,HideSlot,GREY));
    l.push_back(make("pause",Icon::Pause,Kind::Button,START,TopPause,GREY));
    l.push_back(make("cam",Icon::Cam,Kind::Button,BACK,TopCam,GREY));
    l.push_back(make("more",Icon::More,Kind::More,0,BMore,GREY));
    switch(ctx) {
    case CAR:case BIKE:case BICYCLE:
        l.push_back(make("gas",Icon::Gas,Kind::TriggerR,0,BBig,GREY));
        l.push_back(make("brake",Icon::Brake,Kind::TriggerL,0,BBig2,GREY));
        l.push_back(make("handbrake",Icon::Handbrake,Kind::Button,RB,sized(BMid1,0.14f),GREY));
        if(ctx==BICYCLE)l.push_back(make("pedal",Icon::Pedal,Kind::Autotap,A,sized(BMid2,0.14f),GREEN));
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,BCol1,YELLOW));
        l.push_back(make("horn",Icon::Horn,Kind::Button,L3,BCol2,GREY));
        l.push_back(make("radio",Icon::Radio,Kind::TapOrWheel,DL,BCol3,GREY));
        l.push_back(make("look",Icon::Look,Kind::Button,R3,BCol4,GREY));
        if(more) {
            l.push_back(make("driveby",Icon::DriveBy,Kind::Button,LB,BRow1,GREY));
            if(ctx==CAR){l.push_back(make("lights",Icon::Lights,Kind::Button,DR,BRow2,GREY));l.push_back(make("cinema",Icon::Cinema,Kind::Button,B,BRow3,RED));}
        }
        break;
    case BOAT:case SUB:
        l.push_back(make("throttle",Icon::Gas,Kind::TriggerR,0,BBig,GREY));
        l.push_back(make("reverse",Icon::Brake,Kind::TriggerL,0,BBig2,GREY));
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,BCol1,YELLOW));
        l.push_back(make("horn",Icon::Horn,Kind::Button,L3,BCol2,GREY));
        l.push_back(make("radio",Icon::Radio,Kind::TapOrWheel,DL,BCol3,GREY));
        if(ctx==SUB) {
            l.push_back(make("dive",Icon::Dive,Kind::Button,A,sized(BMid1,0.13f),GREEN));
            l.push_back(make("rise",Icon::Rise,Kind::Button,X,sized(BMid2,0.13f),BLUE));
            if(more){l.push_back(make("turnl",Icon::TurnL,Kind::Button,LB,BRow1,GREY));l.push_back(make("turnr",Icon::TurnR,Kind::Button,RB,BRow2,GREY));}
        }
        break;
    case HELI:case PLANE: {
        const bool heli=ctx==HELI;
        l.push_back(make("up",Icon::Up,Kind::TriggerR,0,BBig,GREY));
        l.push_back(make("down",Icon::Down,Kind::TriggerL,0,BBig2,GREY));
        l.push_back(make("yawl",Icon::YawL,Kind::Button,LB,sized(BMid1,0.13f),GREY));
        l.push_back(make("yawr",Icon::YawR,Kind::Button,RB,sized(BMid2,0.13f),GREY));
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,BCol1,YELLOW));
        l.push_back(make("radio",Icon::Radio,Kind::TapOrWheel,DL,BCol2,GREY));
        l.push_back(make("fire",Icon::Missile,Kind::Button,X,BCol3,BLUE));
        l.push_back(heli?make("wcam",Icon::WeaponCam,Kind::Button,R3,BCol4,GREY):make("gear",Icon::Gear,Kind::Button,L3,BCol4,GREY));
        break;}
    case TRAIN:
        l.push_back(make("exit",Icon::Exit,Kind::Button,Y,BCol1,YELLOW));
        break;
    default: // FOOT (and NONE)
        l.push_back(make("fire",Icon::Fire,Kind::Fire,0,BBig,GREY));
        l.push_back(make("sprint",Icon::Sprint,Kind::Autotap,A,BBig2,GREEN));
        l.push_back(make("jump",Icon::Jump,Kind::Button,X,BMid1,BLUE));
        l.push_back(make("aim",Icon::Aim,Kind::AimLatch,0,BMid2,GREY));
        l.push_back(make("enter",Icon::Enter,Kind::Button,Y,BCol1,YELLOW));
        l.push_back(make("cover",Icon::Cover,Kind::Button,RB,BCol2,GREY));
        l.push_back(make("melee",Icon::Melee,Kind::Button,B,BCol3,RED)); // (B: melee, and reload)
        l.push_back(make("phone",Icon::Phone,Kind::Button,DU,TopPhone,GREY));
        l.push_back(make("weapon",Icon::WeaponGroup,Kind::WeaponTap,LB,weaponIv?WeaponSlotIv:WeaponSlot,GREY)); // (SPEC 3.12)
        if(more) {
            l.push_back(make("stealth",Icon::Stealth,Kind::Button,L3,BRow1,GREY));
            l.push_back(make("look",Icon::Look,Kind::Button,R3,BRow2,GREY));
            l.push_back(make("switch",Icon::Switch,Kind::Wheel,DD,BRow3,GREY));
            l.push_back(make("weapons",Icon::Weapons,Kind::Wheel,LB,BRow4,GREY));
        }
        break;
    }
    return l;
}
/** The plain 360 pad (SPEC v2 2, "360 PAD mode"), as TouchPad.padLayout(): the face buttons in the 360 colours and places. */
inline std::vector<Control> padLayout() {
    constexpr float DiaX=0.84f,DiaY=0.66f,DiaStep=0.13f,DiaD=0.11f,StepX=DiaStep*720.0f/1280.0f,RowD=0.07f,TopY=0.06f,TopD=0.06f;
    std::vector<Control> l;
    l.push_back(make("hide",Icon::Hide,Kind::Hide,0,HideSlot,GREY));
    l.push_back(make("y",Icon::Face,Kind::Button,Y,{DiaX,DiaY-DiaStep,DiaD},YELLOW,"Y"));
    l.push_back(make("a",Icon::Face,Kind::Button,A,{DiaX,DiaY+DiaStep,DiaD},GREEN,"A"));
    l.push_back(make("x",Icon::Face,Kind::Button,X,{DiaX-StepX,DiaY,DiaD},BLUE,"X"));
    l.push_back(make("b",Icon::Face,Kind::Button,B,{DiaX+StepX,DiaY,DiaD},RED,"B"));
    l.push_back(make("lt",Icon::Shoulder,Kind::TriggerL,0,{0.07f,0.20f,0.11f},GREY,"LT"));
    l.push_back(make("lb",Icon::Shoulder,Kind::Button,LB,{0.18f,0.20f,0.10f},GREY,"LB"));
    l.push_back(make("rb",Icon::Shoulder,Kind::Button,RB,{0.82f,0.20f,0.10f},GREY,"RB"));
    l.push_back(make("rt",Icon::Shoulder,Kind::TriggerR,0,{0.93f,0.20f,0.11f},GREY,"RT"));
    l.push_back(make("back",Icon::Cam,Kind::Button,BACK,{0.44f,TopY,TopD},GREY));
    l.push_back(make("start",Icon::Pause,Kind::Button,START,{0.56f,TopY,TopD},GREY));
    l.push_back(make("l3",Icon::Stick3,Kind::Button,L3,{0.27f,0.40f,RowD},GREY,"L3"));
    l.push_back(make("r3",Icon::Stick3,Kind::Button,R3,{0.52f,0.50f,RowD},GREY,"R3"));
    l.push_back(make("dpad",Icon::DpadIcon,Kind::Dpad,0,{0.36f,0.78f,0.20f},GREY));
    return l;
}
/** The shared icon (icons.txt, SPEC 3.5) a control draws: its id, with these exceptions - HIDE shows "show" while hidden, the aircraft
 *  FIRE draws "missile", the 360 pad's BACK / START the CAM / PAUSE glyphs; "" = the 360 pad's letters (face, shoulder, stick buttons). */
inline const char* iconName(const Control& c,bool hidden) noexcept {
    switch(c.icon) {
    case Icon::Hide:return hidden?"show":"hide";
    case Icon::Missile:return "missile";
    case Icon::Cam:return "cam";
    case Icon::Pause:return "pause";
    case Icon::DpadIcon:return "dpad";
    case Icon::Skip:return "skip";
    case Icon::WeaponGroup:return "wg0"; // (the fallback; the view draws wg<group> from the library's bits 26-29: weaponIcon())
    case Icon::Face:case Icon::Shoulder:case Icon::Stick3:case Icon::Prompt:return "";
    default:return c.id;
    }
}
/** SPEC 3.10's MENU set: the D-pad, A (accept), B (back), LB / RB (tabs), START (resume), HIDE. */
inline std::vector<Control> menuLayout() {
    std::vector<Control> l;
    l.push_back(make("hide",Icon::Hide,Kind::Hide,0,HideSlot,GREY));
    l.push_back(make("dpad",Icon::DpadIcon,Kind::Dpad,0,{0.12f,0.74f,0.20f},GREY));
    l.push_back(make("a",Icon::Face,Kind::Button,A,{0.90f,0.80f,0.12f},GREEN,"A"));
    l.push_back(make("b",Icon::Face,Kind::Button,B,{0.96f,0.60f,0.10f},RED,"B"));
    l.push_back(make("lb",Icon::Shoulder,Kind::Button,LB,{0.06f,0.24f,0.09f},GREY,"LB"));
    l.push_back(make("rb",Icon::Shoulder,Kind::Button,RB,{0.94f,0.24f,0.09f},GREY,"RB"));
    l.push_back(make("start",Icon::Pause,Kind::Button,START,{0.55f,0.06f,0.06f},GREY));
    l.push_back(make("x",Icon::Face,Kind::Button,X,{0.80f,0.62f,0.09f},BLUE,"X")); // (SPEC 12.8: the menu's prompts also name X and Y,
    l.push_back(make("y",Icon::Face,Kind::Button,Y,{0.86f,0.44f,0.09f},YELLOW,"Y")); //  e.g. "Hide Menu Y")
    return l;
}
/** SPEC 12.8 (owner 2026-10-24: "on the map indoors, make the buttons on the bottom work with touch, to switch view and such"): on the open
 *  map, HIDE and a row of the game's prompt buttons (BACK = "Switch View" indoors, the D-pad = "Select Blip", Y, X, B, A = "Waypoint"
 *  outdoors), left to right in the frame's right half; Pad::relayout puts the row's y (promptY). The rest of the map stays the map touch's
 *  (pan, pinch, waypoint tap). Drawn as GTA prompt icons (ios_qol_touch_view.h drawPrompt), glyph = the key. */
constexpr float PromptY=0.84f,PromptD=0.075f,PromptX0=0.62f,PromptStep=0.062f,PromptDpadD=0.10f;
inline std::vector<Control> mapLayout() {
    std::vector<Control> l;
    l.push_back(make("hide",Icon::Hide,Kind::Hide,0,HideSlot,GREY));
    const auto at=[](int i,float d){return Slot{PromptX0+float(i)*PromptStep,PromptY,d};};
    l.push_back(make("prompt_back",Icon::Prompt,Kind::Button,BACK,at(0,PromptD),GREY,"BACK"));
    l.push_back(make("prompt_dpad",Icon::Prompt,Kind::Dpad,0,at(1,PromptDpadD),GREY,"DPAD"));
    l.push_back(make("prompt_y",Icon::Prompt,Kind::Button,Y,at(2,PromptD),YELLOW,"Y"));
    l.push_back(make("prompt_x",Icon::Prompt,Kind::Button,X,at(3,PromptD),BLUE,"X"));
    l.push_back(make("prompt_b",Icon::Prompt,Kind::Button,B,at(4,PromptD),RED,"B"));
    l.push_back(make("prompt_a",Icon::Prompt,Kind::Button,A,at(5,PromptD),GREEN,"A"));
    return l;
}
inline bool isPrompt(const Control& c) noexcept {return c.icon==Icon::Prompt;}
/** SPEC 12.5 v2 (owner 2026-10-25: "that fps counter is not clean. it needs to be minimalistic and much smaller"; his pick: the shown
 *  number alone; android-qol FpsLabel.java): the frame-rate label the app draws from the library's recomp_fps_v1. White at 78 %
 *  (FpsColorAlpha) in the system font, medium, with a soft faint shadow (black 45 %, blur 5, 1.5 down, in 720p frame pixels x k), no box.
 *  fpsText: the shown rate rounded ("60"), "" with the counter off (on != 1) or before the first window. fpsPlace: {x, baseline, text
 *  size, k} in view pixels: size 1.95 % of the 16:9 frame's height; x = the shown part's left + max(8 k, 0.075 x its width), the baseline
 *  8 k below its top + 0.95 size (the shown part: the whole view in Fill, the 16:9 frame in Fit; k = frame height / 720). */
constexpr float FpsSize=0.0195f,FpsX=0.075f,FpsTop=8.0f/720.0f,FpsColorAlpha=0xC7/255.0f,FpsShadowAlpha=0x73/255.0f;
inline std::string fpsText(int on,float shown) {
    if(on!=1 || !(shown>0.0f) || !std::isfinite(shown))return {};
    return std::to_string(long(std::lround(shown)));
}
struct FpsPlace {float x,baseline,size,k;};
inline FpsPlace fpsPlace(float w,float h,bool fill) noexcept {
    const float sc=std::min(w/1280.0f,h/720.0f),fw=1280.0f*sc,fh=720.0f*sc,fx=(w-fw)*0.5f,fy=(h-fh)*0.5f,k=fh/720.0f,size=FpsSize*fh;
    const float left=fill?0.0f:fx,width=fill?w:fw,top=fill?0.0f:fy;
    return {left+std::max(8.0f*k,FpsX*width),top+FpsTop*fh+size*0.95f,size,k};
}
/** SPEC 3.10's SKIP: one faded button (bottom right), A while held (GTA V's skip; a stray tap never skips: the first touch only shows it). */
inline std::vector<Control> skipLayout() {return {make("skip",Icon::Skip,Kind::Button,A,{0.93f,0.86f,0.08f},GREY)};}
/** SPEC 3.15 (owner 2026-10-25: "when the phone comes up, we need the touch controls to change also. all builds"): gameplay with the phone
 *  up is the PHONE pad; a menu, the map or a cutscene keep their own overlay. */
constexpr Overlay phoneOverlay(Overlay o,bool phone) noexcept {return o==Overlay::Play && phone?Overlay::Phone:o;}
/** The PHONE set: HIDE, PAUSE, the D-pad (the menu set's slot), select = A drawn as a check, back = B drawn as an X; a call (ringing or
 *  ongoing) relabels the same A / B as Answer (green) and Decline / hang up (red). No stick, fire or combat button, no SAVE / TRAINER. */
inline std::vector<Control> phoneLayout(bool call) {
    std::vector<Control> l;
    l.push_back(make("hide",Icon::Hide,Kind::Hide,0,HideSlot,GREY));
    l.push_back(make("pause",Icon::Pause,Kind::Button,START,{0.55f,0.06f,0.06f},GREY));
    l.push_back(make("dpad",Icon::DpadIcon,Kind::Dpad,0,{0.12f,0.74f,0.20f},GREY));
    if(call) {
        l.push_back(make("call_answer",Icon::CallAnswer,Kind::Button,A,PhoneOkSlot,GREEN));
        l.push_back(make("call_decline",Icon::CallDecline,Kind::Button,B,PhoneBackSlot,RED));
    } else {
        l.push_back(make("phone_ok",Icon::PhoneOk,Kind::Button,A,PhoneOkSlot,GREEN));
        l.push_back(make("phone_back",Icon::PhoneBack,Kind::Button,B,PhoneBackSlot,RED));
    }
    return l;
}
/** SPEC 3.12: the WEAPON button's icon for the selected weapon's group (0 unknown .. 11 other; wg0 the fallback). */
inline std::string weaponIcon(int group) {return "wg"+std::to_string(group>=0 && group<=11?group:0);}
/** The D-pad: 8 directions from the centre, a 25 % dead centre (sin(pi/8) octant rule). */
inline int dpadBits(float dx,float dy,float r) noexcept {
    const double len=std::hypot(dx,dy);
    if(len<0.25*r)return 0;
    const double o=0.3826834323650898*len;int b=0;
    if(std::abs(dx)>=o)b|=dx<0?DL:DR;
    if(std::abs(dy)>=o)b|=dy<0?DU:DD;
    return b;
}

struct Stick {
    float hx=0,hy=0,cx=0,cy=0,kx=0,ky=0,R=0;bool held=false;std::uintptr_t finger=0;long long releasedAt=-1;
    float relKx=0,relKy=0,relCx=0,relCy=0;int outX=0,outY=0;
};
/** XInput-like output (SPEC 4): radial dead zone, then from the game's own dead zone to full along a 1.3 curve; y up. */
inline void stickValue(float kx,float ky,float R,bool left,int& ox,int& oy) noexcept {
    ox=oy=0;
    if(!(R>0))return;
    const double vx=kx/R,vy=-ky/R,len=std::hypot(vx,vy),m=std::min(1.0,len);
    if(m<=StickDead)return;
    const double xd=left?LsXinputDead:RsXinputDead;
    const double out=xd+(1-xd)*std::pow(std::max(0.0,m-StickDead)/(1-StickDead),double(StickCurve));
    ox=int(std::lround(32767*out*vx/len));oy=int(std::lround(32767*out*vy/len));
}
/** The knob follows the finger inside the ring; beyond it the ring's centre trails the finger (drag-follow). */
inline void drag(Stick& s,float x,float y) noexcept {
    float dx=x-s.cx,dy=y-s.cy;const float d=std::hypot(dx,dy);
    if(d>s.R){const float k=(d-s.R)/d;s.cx+=dx*k;s.cy+=dy*k;dx=x-s.cx;dy=y-s.cy;}
    s.kx=dx;s.ky=dy;
}

/** The pad the game is given (SPEC 5), input by input: buttons OR'd; a physical stick past XInput's dead zone, or a trigger above 30,
 *  replaces the touch one; the long-press gestures' right stick comes before the pad's. */
inline State arbitrate(const State& physical,const Gesture& gesture,const State& pad) noexcept {
    const bool ls=std::abs(physical.lx)>7849 || std::abs(physical.ly)>7849,rs=std::abs(physical.rx)>8689 || std::abs(physical.ry)>8689;
    const bool gestureStick=gesture.rx!=physical.rx || gesture.ry!=physical.ry;
    const bool padLs=pad.lx!=0 || pad.ly!=0,padRs=pad.rx!=0 || pad.ry!=0;
    State o;
    o.buttons=gesture.buttons|pad.buttons;
    o.lx=ls || !padLs?physical.lx:pad.lx;o.ly=ls || !padLs?physical.ly:pad.ly;
    o.rx=gestureStick?gesture.rx:rs || !padRs?physical.rx:pad.rx;o.ry=gestureStick?gesture.ry:rs || !padRs?physical.ry:pad.ry;
    o.lt=physical.lt>PhysicalTrigger?physical.lt:std::max(physical.lt,pad.lt);
    o.rt=physical.rt>PhysicalTrigger?physical.rt:std::max(physical.rt,pad.rt);
    return o;
}
/** A fresh physical input (SPEC 5): a button not held before, a stick past XInput's dead zone, a trigger above 30. */
inline bool freshPhysical(const State& p,int previousButtons) noexcept {
    return (p.buttons&~previousButtons)!=0 || std::abs(p.lx)>7849 || std::abs(p.ly)>7849 || std::abs(p.rx)>8689 || std::abs(p.ry)>8689 ||
           p.lt>PhysicalTrigger || p.rt>PhysicalTrigger;
}

class Pad {
public:
    // settings (Library/r/settings/touch-controls.txt words, as Android's files/settings/touch-controls.txt)
    Mode mode=Mode::Context;bool hideWithController=true,showOnTouch=true,layoutB=true;float sizeScale=1.0f,opacity=0.8f; // (layout B: the owner's pick)
    // Owner 2026-10-24 ~20:35: "make the left stick appear anywhere on the left side of the screen, and the right stick map the axis to
    // swipes on the right side of the screen ... The speed of which goes by how far the right stick moves". The right stick is a swipe
    // area: no ring is drawn; a finger anywhere on the right side (off the controls) turns the camera by how far it has moved from where it
    // touched down (the same curve, full speed at R). rswipe=0 draws the floating right stick again.
    bool rightSwipe=true;
    bool weaponIv=true; // (SPEC 3.12 v15, GTA IV style weapon: the default since the owner approved v15, 2026-10-24; `wstyle=disc` = the old disc)

    /** The view in pixels, its pixels per millimetre and the HUD frame (left, top, width, height) inside it. */
    void view(float w,float h,float pxPerMm,float fl,float ft,float fw,float fh) {
        vw_=w;vh_=h;mmPx_=pxPerMm>0?pxPerMm:10.0f;frame_[0]=fl;frame_[1]=ft;frame_[2]=fw;frame_[3]=fh;relayout();
    }
    void view(float w,float h,float pxPerMm,const qol::Frame& f) {
        const float fw=f.valid()?f.fw():0,fh=f.valid()?f.fh():0,top=(h-fh)*0.5f;
        // Portrait (the iPad held upright: the picture is a 16:9 strip with tall bars): SPEC v2 lays out landscape only. iOS rule (iPad 235):
        // the layout frame runs from the picture's top to the view's bottom over the full width, so the thumb controls sit in the free space
        // under the picture; sizes come from min(that height, 0.75 x the width); no bar shift (the frame touches the view's sides and bottom).
        if(h>w && fw>0){portrait_=true;hud_[0]=(w-fw)*0.5f;hud_[1]=top;hud_[2]=fw;hud_[3]=fh;view(w,h,pxPerMm,0,top,w,h-top);return;}
        portrait_=false;view(w,h,pxPerMm,(w-fw)*0.5f,top,fw,fh);
    }
    bool portrait() const noexcept {return portrait_;}
    const float* frame() const noexcept {return frame_;}
    float viewW() const noexcept {return vw_;}
    float viewH() const noexcept {return vh_;}
    /** Frame units -> view pixels, moved outside the frame by half the margin where that helps (SPEC 3.1): left / right halves out by half
     *  the side margin; the bottom half down by half the bottom margin, the top row (fy < TopRow) up by half the top one. The controls in
     *  between stay (SPEC v2's right column would otherwise rise into the weapon-icon zone on a letterboxed view: the Fold, the iPad). */
    static constexpr float TopRow=0.15f;
    float px(float fx) const noexcept {const float x=frame_[0]+fx*frame_[2],bar=portrait_?0.0f:frame_[0];return fx<0.5f?x-bar*0.5f:x+bar*0.5f;}
    // SPEC 12.8: the prompt row's centre y: in the middle of the bar below the frame when it has room (nothing of the map covered), else
    // just above the game's prompt strip (PromptY of the frame; the strip is at ~0.92).
    float promptY(float r) const noexcept {const float bar=frame_[1];return bar>=2.4f*r?frame_[1]+frame_[3]+bar*0.5f:frame_[1]+PromptY*frame_[3];}
    float py(float fy) const noexcept {const float y=frame_[1]+fy*frame_[3],bar=portrait_?0.0f:frame_[1];return fy>0.5f?y+bar*0.5f:fy<TopRow?y-bar*0.5f:y;}
    const std::vector<Control>& controls() const noexcept {return controls_;}
    const Control* control(std::string_view id) const noexcept {for(const auto& c:controls_)if(c.is(id))return &c;return nullptr;}
    Stick sticks[2];

    /** Whether the long-press gesture's reduced zones apply (the universal controls are showing: SPEC v2 3.3). */
    bool padZones() const noexcept {return mode==Mode::Context && !hidden_ && !userHidden_;}
    /** Whether a long-press zone is active (SPEC v2 3.3): with the universal controls showing only the radar and the weapon icon; the 360
     *  pad only the radar; hidden or off: all four. */
    bool zoneActive(qol::Gesture::Zone z) const noexcept {
        if(mode==Mode::Off || hidden_ || userHidden_)return true;
        if(mode==Mode::Pad)return z==qol::Gesture::Zone::Map;
        return z==qol::Gesture::Zone::Map || z==qol::Gesture::Zone::Weapon;
    }
    /** The zone of a view point, with the zones this mode keeps (frame shares; SPEC v2 3.3 rectangles while the controls show). */
    qol::Gesture::Zone zone(float x,float y,const qol::Frame& f) const noexcept {
        // (portrait, iOS rule: the zones clamp a point onto the picture's edge - "a bar beside or below it counts", SPEC 3.3 - which a
        //  715 px bar would turn into a radar zone over the whole lower left; beyond 10 % of the picture's height above / below: none)
        if(portrait_ && !userHidden_ && (y>hud_[1]+hud_[3]*1.1f || y<hud_[1]-hud_[3]*0.1f))return qol::Gesture::Zone::None;
        return qol::Gesture::zone(x,y,f,padZones());
    }

    // ======== context
    /** The library's word (recomp_touch_context_v1), polled every 100 ms; < 0 unknown. A change must be read twice in a row. */
    void contextWord(int word) {
        mapShown_=word>=0 && (word&0x8000)!=0;
        screens_=word>=0?word&Screens:0; // (v704: a wheel, the radio wheel, a frontend menu, a cutscene on screen; SPEC 3.8)
        weaponGroup_=word>=0?int((unsigned(word)>>26)&0xF):0; // (v707: the selected weapon's group, SPEC 3.12)
        { // (SPEC 3.10: the overlay - the map at once, the others after 2 reads in a row; as TouchPad.java)
            const bool cut=skippable(word); // (3.10 v2: SKIP only where the game can skip - a cutscene)
            if(cut!=cutscene_){cutscene_=cut;if(overlayMode_==Overlay::Skip)relayout();}
            const Overlay o=phoneOverlay(overlayFor(word),phoneUp_);
            if(o!=overlayMode_) {
                const int need=overlayMode_==Overlay::Map && o==Overlay::Menu?MenuFromMapReads:2; // (no menu flash around the map)
                if(o==Overlay::Map || (o==overlayNext_ && ++overlayPending_>=need)){overlayMode_=o;skipRevealed_=false;overlayPending_=0;relayout();}
                else if(o!=overlayNext_){overlayNext_=o;overlayPending_=1;}
            } else {overlayNext_=o;overlayPending_=0;}
        }
        const int ctx=word<0?-1:(word&0xFF);
        if(ctx<0 || ctx==NONE){pendingCount_=0;return;} // (loading / no answer: keep the last layout)
        if(ctx==context_){pendingCount_=0;if(shownContext_!=context_){shownContext_=context_;relayout();}return;}
        if(ctx==pendingContext_){if(++pendingCount_>=2){context_=ctx;shownContext_=ctx;pendingCount_=0;moreOpen_=false;relayout();}} // (a context change closes MORE)
        else {pendingContext_=ctx;pendingCount_=1;}
    }
    int context() const noexcept {return shownContext_<0?FOOT:shownContext_;}
    bool mapShown() const noexcept {return mapShown_;}

    // ======== fingers (id: any per-touch identity, e.g. the UITouch's address)
    /** A finger down at (x, y): true when the pad takes it (a control or a stick), false when it belongs to the gesture (a zone, the map).
     *  zoneOf: the long-press zone at the point (zone() above, with the view's frame). */
    bool down(std::uintptr_t id,long long t,float x,float y,qol::Gesture::Zone zoneOf) {
        if(mode==Mode::Off)return false;
        if(userHidden_) {
            const Control* c=hitControl(x,y);if(!c || c->kind!=Kind::Hide)return false;
            userHidden_=false;lastTouch_=visibleSince_=t;return true; // (SHOW: the controls come back; this finger does nothing else)
        }
        if(mode!=Mode::Pad && overlayMode_==Overlay::Skip && !skipShown(t)){ // (SPEC 3.10: the first touch only shows SKIP)
            const bool was=skipRevealed_;skipRevealed_=true;lastTouch_=t;if(!was)relayout();return true;}
        lastTouch_=t;
        if(Finger* old=finger(id)){const Finger f=*old;erase(id);release(f,t);} // (an id still held = its lift was lost)
        if(hidden_){if(!showOnTouch)return false;hidden_=false;visibleSince_=t;} // (SPEC v2 5: a touch shows them again when allowed)
        const Control* c=hitControl(x,y);
        Finger f;f.id=id;f.x0=f.x=x;f.y0=f.y=y;f.downAt=t;
        if(!c) {
            if(zoneOf!=qol::Gesture::Zone::None && zoneActive(zoneOf))return false;
            const int side=x<vw_*LeftSide?0:1;
            Stick& s=sticks[side];
            if(s.held)return false; // (one finger per stick)
            if(sticksOff(t))return false; // (a wheel or the pause map is open: no stick; the finger is the gesture's)
            s.held=true;s.finger=id;s.releasedAt=-1;s.cx=x;s.cy=y;s.kx=s.ky=0;f.stick=side+1;fingers_.push_back(f);stickOut(s,side);return true;
        }
        if(c->kind==Kind::Hide){userHidden_=true;visibleSince_=t;cancel(t);aimLatched_=false;return true;} // (HIDE: every input released)
        if(c->kind==Kind::App){appAction_=c->id;return true;} // (SPEC 3.14: the app acts on it once - takeAppAction; nothing for the game)
        if(c->kind==Kind::More){moreOpen_=!moreOpen_;relayout();return true;} // (layout B: the MORE row opens / closes; nothing for the game)
        f.control=*c;f.index=int(c-controls_.data());f.generation=generation_;fingers_.push_back(f);press(fingers_.back(),t);return true;
    }
    bool userHidden() const noexcept {return userHidden_;}
    /** SPEC 12.7 (android-qol v21, TouchPad.controlAt): a shown control under (x, y) - only the eye while the player has hidden the
     *  controls, none while the pad is off or hidden by a controller. The side menu's edge strip leaves such a finger to the pad. */
    bool controlAt(float x,float y) const noexcept {
        if(mode==Mode::Off || hidden_)return false;
        const Control* c=hitControl(x,y);
        return c && (!userHidden_ || c->kind==Kind::Hide);
    }
    /** ios-sidemenu (SPEC 12.2 Touch Controls: Show Touch Controls = the HIDE eye's state): hidden releases every input, as HIDE does. */
    void showControls(bool shown,long long t) {
        if(shown==!userHidden_)return;
        if(shown){userHidden_=false;lastTouch_=visibleSince_=t;}
        else {userHidden_=true;visibleSince_=t;cancel(t);aimLatched_=false;}
    }
    bool moreOpen() const noexcept {return moreOpen_;}
    // ---- SPEC 3.14: the app's own buttons (SAVE: the library's OpenSaveMenu; TRAINER: the side menu's Trainer tab), as TouchPad.java
    /** The app button tapped since the last call ("qsave" / "trainer"), once; else "". */
    std::string takeAppAction() {std::string a=std::move(appAction_);appAction_.clear();return a;}
    /** TRAINER is shown while the trainer is switched on (the library's state). */
    void trainerButton(bool on) {if(on!=trainerButton_){trainerButton_=on;relayout();}}
    // ---- SPEC 3.15: the phone (the library's recomp_phone_state_v1, polled with the context word): bit 0 up, bit 1 ringing, bit 2 a call
    void phone(int bits) {
        phoneUp_=bits>0 && (bits&1)!=0;
        const bool call=phoneUp_ && (bits&6)!=0;
        if(call!=phoneCall_){phoneCall_=call;if(overlayMode_==Overlay::Phone)relayout();} // (a call relabels at once)
    }
    /** SPEC 3.8 (owner 2026-10-24 ~21:00: "the stick needs to disable when on any screens such as radio, character switch, weapon wheel
     *  etc."), as TouchPad.java: a wheel is open outside the pad (the gesture's wheelOpen(), or the physical pad holds a wheel's button:
     *  physicalWheel()). Set before every state() and on every touch / tick. */
    void overlay(bool open) noexcept {overlay_=open;}
    static constexpr int Screens=0xF0000; // (the library's screen bits 16-19: weapon wheel, radio wheel, frontend menu, cutscene)
    static constexpr int FrontendBit=1<<18,SkipBits=(1<<19)|(1<<20)|(1<<21); // (SPEC 3.10: v705's cutscene / faded out / control off)
    // SPEC 3.10 "No menu flash around the map" (owner 2026-10-24: "the menu controls briefly pop up when toggling the map"): v711's bit 30 = a
    // map-tap episode runs (it passes through the pause menu for ~0.8 s) counts as MAP at once; from MAP the menu set needs MenuFromMapReads
    // reads in a row (a frontend flag left for a moment after the close is no menu; a real pause from play still shows it after 2).
    static constexpr int MapMacroBit=1<<30,MenuFromMapReads=15;
    Overlay overlayMode() const noexcept {return overlayMode_;}
    /** The overlay a library word asks for (first match): the pause map, a frontend menu (the Fold 21:30: the pause menu also turns control
     *  off - the menu wins), a cutscene / faded-out screen / control off. */
    static Overlay overlayFor(int word) noexcept {
        // (owner 2026-10-24 ~23:02: "Fold controls still come up when game first launches": no library / no guest memory yet (< 0) or no
        //  player (context NONE: the logos, the loading screen, the save load) = SKIP like a cutscene - the pad only in real gameplay)
        if(word<0 || (word&0xFF)==NONE)return Overlay::Skip;
        if(word&(0x8000|MapMacroBit))return Overlay::Map; // (v711 bit 30: the map macro passes through the pause menu - no menu flash)
        if(word&FrontendBit)return Overlay::Menu;
        if(word&SkipBits)return Overlay::Skip;
        return Overlay::Play;
    }
    // SPEC 3.10 v2 (owner 2026-10-27: "intro skip button no work" - he tapped it ~30 times on the load after a save, word 0x100001 =
    // faded out, which no button skips): the SKIP button only where a cutscene can be playing - the cutscene bit (19; A skips it), or
    // control off (21) without a fade (20). A fade (loading, the save load), no answer or no player alone: the pad hidden, no button.
    static constexpr bool skippable(int word) noexcept {
        return word>=0 && ((word&(1<<19))!=0 || ((word&(1<<21))!=0 && (word&(1<<20))==0));
    }
    int weaponGroup() const noexcept {return weaponGroup_;}
    /** What the drawing shows apart from touches (TouchPad.visualKey): the view compares it at every context poll and redraws on a change,
     *  so a weapon switch or a screen bit shows with no touch (owner's bug: the weapon icon only changed on the next touch). */
    std::uint64_t visualKey(long long t) const noexcept {
        return std::uint64_t(weaponGroup_)|std::uint64_t(overlayMode_)<<4|std::uint64_t(context())<<8|std::uint64_t(mapShown_)<<16|
               std::uint64_t(hidden_)<<17|std::uint64_t(userHidden_)<<18|std::uint64_t(moreOpen_)<<19|std::uint64_t(skipRevealed_)<<20|
               std::uint64_t(sticksOff(t))<<21|std::uint64_t(trainerButton_)<<22|std::uint64_t(phoneCall_)<<23|std::uint64_t(unsigned(screens_)>>16)<<24|std::uint64_t(cutscene_)<<32;
    }
    /** SPEC 3.12 v15: the weapon's opacity without the idle fade; it still follows hidden / OFF / the map. */
    float weaponAlpha(long long t) const noexcept {
        if(mode==Mode::Off || hidden_)return alpha(t);
        return opacity*(mapShown_?0.2f:1.0f);
    }
    /** SPEC 3.10 SKIP: the button shows after a touch, until the idle fade (4 s) with no finger on it. */
    bool skipShown(long long t) const noexcept {return mode!=Mode::Pad && overlayMode_==Overlay::Skip && skipRevealed_ && (!fingers_.empty() || t-lastTouch_<FadeIdleMs);}
    int screens() const noexcept {return screens_;}
    /** The physical pad holds a wheel's button: LB or D-pad DOWN on foot, D-pad LEFT in a vehicle (LB there is the drive-by). */
    static bool physicalWheel(int context,int buttons) noexcept {return context==FOOT || context==NONE?(buttons&(LB|DD))!=0:(buttons&DL)!=0;}
    /** Whether the sticks are off now (SPEC 3.8): a screen from the library's word, a wheel outside the pad, or one of this pad's wheel
     *  buttons held past its tap. The pause map is not a screen here (its touches are the map's). */
    bool sticksOff(long long t) const noexcept {
        if(screens_ || overlay_ || (mode!=Mode::Pad && overlayMode_!=Overlay::Play))return true; // (SPEC 3.10: no sticks in MAP / SKIP / MENU; the 360 pad ignores the overlays, as its layout does)
        for(const auto& f:fingers_)if(f.stick==0 && wheelKind(f.control.kind) && (t-f.downAt>TapMs || f.moved))return true;
        return false;
    }
    /** A physical controller is detected: hide when the setting says so; a touch on the screen shows them again. */
    void controllerPresent(long long t,bool present) {
        if(present && hideWithController && !hidden_ && mode!=Mode::Off){hidden_=true;visibleSince_=t;cancel(t);aimLatched_=false;}
    }
    void move(std::uintptr_t id,long long t,float x,float y) {
        Finger* f=finger(id);if(!f)return;
        f->x=x;f->y=y;lastTouch_=t;
        if(std::hypot(x-f->x0,y-f->y0)>qol::Gesture::LongSlop*std::min(frame_[2],frame_[3]))f->moved=true;
        if(f->stick>0){Stick& s=sticks[f->stick-1];drag(s,x,y);stickOut(s,f->stick-1);}
        else {const Kind k=f->control.kind;if(k==Kind::TriggerL || k==Kind::TriggerR)f->value=triggerValue(*f);
            else if(k==Kind::Steer){const Control* h=hitControl(x,y);if(h && h->kind==Kind::Steer){f->control=*h;f->index=int(h-controls_.data());}}} // (3.16: a thumb slides from arrow to arrow)
    }
    void up(std::uintptr_t id,long long t) {Finger* f=finger(id);if(!f)return;const Finger copy=*f;erase(id);lastTouch_=t;release(copy,t);}
    /** A cancel, focus loss, a physical takeover: every touch input released at once. */
    void cancel(long long t) {
        const auto all=fingers_;fingers_.clear();for(const auto& f:all)release(f,t);
        autotap_=false;autotapFinger_=0;releaseBits_=0;pulseBits_=0;latchRequest_=qol::Gesture::Zone::None;
    }
    /** The wheel a lifted wheel button hands over to the gesture's latch() (once), or None (SPEC 3.6: a wheel button held still and
     *  lifted keeps the wheel open; the gesture holds its button and takes the next finger). */
    qol::Gesture::Zone takeLatch() noexcept {const auto z=latchRequest_;latchRequest_=qol::Gesture::Zone::None;return z;}
    bool owns(std::uintptr_t id) const noexcept {for(const auto& f:fingers_)if(f.id==id)return true;return false;}
    /** The stale-finger watchdog (owner 2026-10-24 ~21:45: "the joystick circle pad on the fold can get stuck"; TouchPad.retain): every pad
     *  finger whose id is not among the event's touches (its lift was lost) is released. Returns how many. */
    int retain(const std::uintptr_t* ids,std::size_t n,long long t) {
        int released=0;
        const auto all=fingers_;
        for(const auto& f:all) {
            bool present=false;for(std::size_t k=0;k<n;++k)if(ids[k]==f.id){present=true;break;}
            if(!present){erase(f.id);release(f,t);++released;}
        }
        return released;
    }
    /** What a pad finger is, for the log: left-stick / right-stick / the control's id / nullptr (not the pad's). */
    const char* fingerName(std::uintptr_t id) const noexcept {
        for(const auto& f:fingers_)if(f.id==id)return f.stick==1?"left-stick":f.stick==2?"right-stick":f.control.id;
        return nullptr;
    }
    /** A new gesture (no finger down) or the last lift: any finger still held is stale - released, so nothing stays stuck. */
    void noFingers(long long t) {if(!fingers_.empty()){const auto all=fingers_;fingers_.clear();for(const auto& f:all)release(f,t);}}
    std::size_t fingerCount() const noexcept {return fingers_.size();}

    // ======== animation (drawing only; the output never waits for it)
    struct Draw {float cx,cy,kx,ky;};
    /** Where a stick's ring centre and knob are drawn at time t. SPEC 4.2: after the lift the knob springs back while the ring stays
     *  where it was let go (and fades: stickAlpha); no slide back home. */
    Draw stickDraw(int side,long long t) {
        Stick& s=sticks[side];
        if(s.held || s.releasedAt<0)return {s.cx,s.cy,s.kx,s.ky};
        const double dt=double(t-s.releasedAt);
        const double w=6.6/double(SpringMs),k=(1+w*dt)*std::exp(-w*dt); // (critically damped: ~1 % left at SpringMs)
        const Draw d{s.relCx,s.relCy,float(s.relKx*k),float(s.relKy*k)};
        if(!settling(s,t)){s.releasedAt=-1;s.cx=s.hx;s.cy=s.hy;s.kx=s.ky=0;}
        return d;
    }
    /** SPEC 4.2 (owner 2026-10-25, relayed by main: "circle pad on the z flip is still static. Make it disappear"; android-qol v27): a
     *  stick's opacity at t: 1 while a finger holds it, down to 0 over StickFadeMs after the lift, 0 at rest (nothing drawn at home).
     *  Read before stickDraw, which ends the release. */
    float stickAlpha(int side,long long t) const noexcept {
        const Stick& s=sticks[side];
        if(s.held)return 1.0f;
        if(s.releasedAt<0)return 0.0f;
        return std::max(0.0f,1.0f-float(t-s.releasedAt)/float(StickFadeMs));
    }
    // (by time, not by stickDraw clearing releasedAt: a stick that is not drawn - the right swipe's - would keep the view redrawing)
    static bool settling(const Stick& s,long long t) noexcept {return s.releasedAt>=0 && t-s.releasedAt<std::max(SpringMs,StickFadeMs);}
    bool animating(long long t) const noexcept {return settling(sticks[0],t) || settling(sticks[1],t) || alphaChanging(t);}

    // ======== output
    /** The pad from the touches at time t. */
    State state(long long t) {
        State o;
        if(!sticksOff(t)){o.lx=sticks[0].outX;o.ly=sticks[0].outY;o.rx=sticks[1].outX;o.ry=sticks[1].outY;} // (owner ~21:00: off on a wheel)
        int steer=0; // (3.16: the held arrows)
        for(const auto& f:fingers_) {
            if(f.stick>0)continue;
            const Control& c=f.control;
            switch(c.kind) {
            case Kind::Button:o.buttons|=c.bits;break;
            case Kind::TriggerL:o.lt=std::max(o.lt,int(std::lround(f.value)));break;
            case Kind::TriggerR:o.rt=std::max(o.rt,int(std::lround(f.value)));break;
            case Kind::Fire:o.rt=255;break;
            case Kind::AimLatch:o.lt=255;break;
            case Kind::Autotap:if(f.id!=autotapFinger_ || !autotap_ || ((t-f.downAt)/AutotapHalfMs)%2==0)o.buttons|=c.bits;break;
            case Kind::Wheel:case Kind::TapOrWheel:case Kind::WeaponTap:
                if(t-f.downAt>TapMs || f.moved) {
                    o.buttons|=c.bits;
                    if(f.moved && t-f.downAt>=qol::Gesture::OpenMs+TapMs) { // (the wheel is open: the finger steers from its press point)
                        int sx,sy;
                        if(qol::Gesture::direction(f.x-f.x0+frame_[2]*0.5f,f.y-f.y0+frame_[3]*0.5f,frame_[2],frame_[3],0.5f,0.5f,sx,sy)){o.rx=sx;o.ry=sy;}
                    }
                }
                break;
            case Kind::Dpad:o.buttons|=dpadBits(f.x-c.cx,f.y-c.cy,c.r);break;
            case Kind::Steer:steer|=c.bits;break; // (3.16)
            case Kind::Hide:case Kind::More:case Kind::App:break; // (the app's own: nothing for the game)
            }
        }
        if(const int sx=steerX(steer);sx!=NoSteer && !sticksOff(t))o.lx=sx; // (3.16: a held arrow sets the left stick's x; its y stays)
        if(aimLatched_)o.lt=255;
        if(t<pulseUntil_)o.buttons|=pulseBits_;else pulseBits_=0;
        if(t<releaseAt_)o.buttons|=releaseBits_;else releaseBits_=0;
        return o;
    }
    /** When state() can change without a touch (an auto-tap, a pulse's or a wheel's release, a wheel opening): the next time. */
    long long due(long long t) const noexcept {
        long long d=qol::None;
        if(autotap_)for(const auto& f:fingers_)if(f.id==autotapFinger_)d=std::min(d,t+AutotapHalfMs-(t-f.downAt)%AutotapHalfMs);
        if(pulseBits_)d=std::min(d,pulseUntil_);
        if(releaseBits_)d=std::min(d,releaseAt_);
        for(const auto& f:fingers_)if(f.stick==0) {
            const Kind k=f.control.kind;
            if(wheelKind(k) && t-f.downAt<=TapMs)d=std::min(d,f.downAt+TapMs+1);
            if(wheelKind(k) && f.moved && t-f.downAt<qol::Gesture::OpenMs+TapMs)d=std::min(d,f.downAt+qol::Gesture::OpenMs+TapMs);
        }
        return d;
    }
    /** Whether any touch input is active. */
    bool active() const noexcept {return !fingers_.empty() || aimLatched_ || pulseBits_!=0 || releaseBits_!=0;}

    /** A fresh physical input hides the controls when the setting says so. */
    void physical(long long t,bool freshInput) {
        if(!freshInput || !hideWithController || hidden_ || mode==Mode::Off)return;
        hidden_=true;visibleSince_=t;cancel(t);aimLatched_=false;
    }
    bool hidden() const noexcept {return hidden_;}
    // ======== drawing alpha (SPEC 5)
    float alpha(long long t) const noexcept {
        if(mode==Mode::Off)return 0.0f;
        const float since=std::min(1.0f,float(t-visibleSince_)/float(FadeMs));
        if(hidden_)return opacity*(1.0f-since)*idleFactor(t);
        return opacity*idleFactor(t)*(mapShown_?0.2f:1.0f);
    }
    bool held(const Control& c) const noexcept {
        const int i=int(&c-controls_.data());
        for(const auto& f:fingers_)if(f.stick==0 && f.generation==generation_ && f.index==i)return true;
        return c.kind==Kind::AimLatch && aimLatched_;
    }
    bool driveByHeld() const noexcept {for(const auto& f:fingers_)if(f.stick==0 && f.control.is("driveby"))return true;return false;}
    /** A held analog trigger's value (0..255) for drawing its depth. */
    int triggerHeld(const Control& c) const noexcept {
        const int i=int(&c-controls_.data());int v=0;
        for(const auto& f:fingers_)if(f.stick==0 && f.generation==generation_ && f.index==i)v=std::max(v,int(f.value));
        return v;
    }

    // ======== settings (touch-controls.txt words; SPEC 5)
    void settings(const char* text) {
        mode=Mode::Context;hideWithController=true;showOnTouch=true;layoutB=true;moreOpen_=false;rightSwipe=true;weaponIv=true;sizeScale=1.0f;opacity=0.8f;
        if(text) {
            const char* p=text;
            while(*p) {
                while(*p && std::isspace(static_cast<unsigned char>(*p)))++p;
                const char* w=p;while(*p && !std::isspace(static_cast<unsigned char>(*p)))++p;
                const std::string_view word(w,std::size_t(p-w));
                const auto eq=word.find('=');if(eq==std::string_view::npos || eq==0)continue;
                const auto k=word.substr(0,eq),v=word.substr(eq+1);
                if(k=="mode")mode=v=="pad"?Mode::Pad:v=="off"?Mode::Off:Mode::Context;
                else if(k=="hide")hideWithController=v!="0";
                else if(k=="show")showOnTouch=v!="0"; // (SPEC v2 5: a touch shows auto-hidden controls again)
                else if(k=="layout")layoutB=v!="a"; // (SPEC 3.7: layout B is the default since the owner picked it, 2026-10-24; layout=a = A)
                else if(k=="rswipe")rightSwipe=v!="0"; // (owner 20:35: the right side is a swipe area)
                else if(k=="wstyle")weaponIv=v!="disc"; // (SPEC 3.12 v15)
                else if(k=="size"){const int s=number(v);if(s>=70 && s<=130)sizeScale=float(s)/100.0f;}
                else if(k=="opacity"){const int o=number(v);if(o>=20 && o<=100)opacity=float(o)/100.0f;}
            }
        }
        relayout();
    }

private:
    struct Finger {std::uintptr_t id=0;Control control{};int index=-1;unsigned generation=0;int stick=0;float x0=0,y0=0,x=0,y=0;long long downAt=0;bool moved=false;float value=0;};
    static int number(std::string_view v) noexcept {
        if(v.empty() || v.size()>4)return -1;int n=0;for(char ch:v){if(ch<'0' || ch>'9')return -1;n=n*10+(ch-'0');}return n;
    }
    float H() const noexcept {return portrait_?std::min(frame_[3],frame_[2]*0.75f):frame_[3];}
    float size(float d,float minMm) const noexcept {return std::max(d*H(),minMm*mmPx_)*sizeScale;}
    void relayout() {
        // A control held through a layout switch (SPEC v2 2, "Switching") keeps its input until the lift: its finger holds a copy of the old
        // control (as the Java model's finger keeps the old Control object), and the new layout's control there is not drawn held.
        ++generation_;
        controls_.clear();
        if(mode==Mode::Off || !(frame_[2]>0))return;
        const int ctx=shownContext_<0?FOOT:shownContext_;
        if(mode!=Mode::Pad && overlayMode_!=Overlay::Play) { // (SPEC 3.10: the overlay's own set; the 360 pad ignores the overlays)
            if(overlayMode_==Overlay::Menu)controls_=menuLayout();
            else if(overlayMode_==Overlay::Skip && skipRevealed_ && cutscene_)controls_=skipLayout(); // (SKIP only after the revealing touch, 3.10 v2: in a cutscene)
            else if(overlayMode_==Overlay::Map)controls_=mapLayout(); // (SPEC 12.8: HIDE and the prompt row)
            else if(overlayMode_==Overlay::Phone)controls_=phoneLayout(phoneCall_); // (SPEC 3.15)
        } else {
            controls_=mode==Mode::Pad?padLayout():layoutB?contextLayoutB(ctx,moreOpen_,weaponIv):contextLayout(ctx,weaponIv);
            if(mode==Mode::Context) { // (SPEC 3.14: app actions, never in the 360 pad mode or an overlay)
                controls_.push_back(make("qsave",Icon::Qsave,Kind::App,0,TopSave,GREY));
                if(trainerButton_)controls_.push_back(make("trainer",Icon::Trainer,Kind::App,0,TopTrainer,GREY));
                if(steerArrows(ctx)) { // (3.16: bits = the direction, SteerLeft / SteerRight, not pad bits)
                    controls_.push_back(make("steer_l",Icon::SteerL,Kind::Steer,SteerLeft,SteerLSlot,GREY));
                    controls_.push_back(make("steer_r",Icon::SteerR,Kind::Steer,SteerRight,SteerRSlot,GREY));
                }
            }
        }
        for(auto& c:controls_) {
            c.r=size(c.d,MinButtonMm)*0.5f; // (kept inside the view: a physical floor can outgrow a frame-unit margin)
            c.cx=std::max(c.r,std::min(vw_-c.r,px(c.fx)));c.cy=std::max(c.r,std::min(vh_-c.r,isPrompt(c)?promptY(c.r):py(c.fy)));
        }
        if(portrait_)portraitFit();
        homeSticks();
    }
    // Portrait (iOS rule): a control on an active long-press zone (the HUD's radar / weapon icon in the picture) moves just below it; then
    // overlapping neighbours are pushed apart sideways (the narrow width cannot keep SPEC's frame-unit spacing at the 7.5 mm floor).
    void portraitFit() {
        const float gap=2.0f;
        const struct {float x0,y0,x1,y1;qol::Gesture::Zone z;} zones[]={
            // (the minimap zone of this mode: the radar box while the pad's zones apply, else the larger original + the clamp band of zone())
            {hud_[0]+(padZones()?qol::Gesture::PadMapZone.x0:0.0f)*hud_[2],hud_[1]+(padZones()?qol::Gesture::PadMapZone.y0:qol::Gesture::MapZone.y0)*hud_[3],
             hud_[0]+(padZones()?qol::Gesture::PadMapZone.x1:qol::Gesture::MapZone.x1)*hud_[2],hud_[1]+(padZones()?qol::Gesture::PadMapZone.y1:1.1f)*hud_[3],qol::Gesture::Zone::Map},
            {hud_[0]+0.75f*hud_[2],hud_[1],hud_[0]+hud_[2],hud_[1]+0.20f*hud_[3],qol::Gesture::Zone::Weapon}};
        for(auto& c:controls_)for(const auto& z:zones) {
            if(!zoneActive(z.z))continue;
            const float nx=std::clamp(c.cx,z.x0,z.x1),ny=std::clamp(c.cy,z.y0,z.y1);
            if(std::hypot(c.cx-nx,c.cy-ny)<c.r+gap)c.cy=std::min(vh_-c.r,z.y1+c.r+gap);
        }
        for(int pass=0;pass<8;++pass) {
            bool moved=false;
            for(auto& a:controls_)for(auto& b:controls_) {
                if(&a==&b || b.cx<a.cx || (b.cx==a.cx && &b<&a))continue;
                const float d=std::hypot(a.cx-b.cx,a.cy-b.cy),need=a.r+b.r+gap;
                if(d>=need)continue;
                const float dy=b.cy-a.cy,dx=std::sqrt(std::max(0.0f,need*need-dy*dy));
                b.cx=std::min(vw_-b.r,a.cx+dx);moved=true;
            }
            if(!moved)break;
        }
    }
    void homeSticks() {
        const bool b=layoutB && mode==Mode::Context;
        const float home[2][3]={{b?BLsX:LsHomeX,b?BLsY:LsHomeY,b?BLsR:LsR},{b?BRsX:RsHomeX,b?BRsY:RsHomeY,b?BRsR:RsR}};
        for(int i=0;i<2;++i) {
            Stick& s=sticks[i];s.hx=px(home[i][0]);s.hy=py(home[i][1]);s.R=size(2*home[i][2],MinRingMm)*0.5f;
            if(!s.held && s.releasedAt<0){s.cx=s.hx;s.cy=s.hy;}
        }
    }
    Finger* finger(std::uintptr_t id) noexcept {for(auto& f:fingers_)if(f.id==id)return &f;return nullptr;}
    void erase(std::uintptr_t id) noexcept {
        fingers_.erase(std::remove_if(fingers_.begin(),fingers_.end(),[id](const Finger& f){return f.id==id;}),fingers_.end());
    }
    const Control* hitControl(float x,float y) const noexcept {for(const auto& c:controls_)if(c.hit(x,y))return &c;return nullptr;}
    void press(Finger& f,long long t) {
        const Control& c=f.control;
        if(c.kind==Kind::TriggerL || c.kind==Kind::TriggerR)f.value=float(TriggerDown);
        if(c.kind==Kind::Autotap){if(t-lastSprintTap_<=DoubleTapMs){autotap_=true;autotapFinger_=f.id;}lastSprintTap_=t;}
    }
    void release(const Finger& f,long long t) {
        if(f.stick>0) {
            Stick& s=sticks[f.stick-1];s.held=false;s.finger=0;s.releasedAt=t;s.relKx=s.kx;s.relKy=s.ky;s.relCx=s.cx;s.relCy=s.cy;s.outX=s.outY=0;
            return;
        }
        const Control& c=f.control;
        if(c.kind==Kind::AimLatch && t-f.downAt<=TapMs && !f.moved)aimLatched_=!aimLatched_;
        else if(c.kind==Kind::AimLatch)aimLatched_=false;
        if(c.kind==Kind::TapOrWheel && t-f.downAt<=TapMs && !f.moved){pulseBits_|=c.bits;pulseUntil_=t+PulseMs;}
        if(c.kind==Kind::Button){const long long min=(c.bits&(START|BACK))?MinMenuPressMs:MinPressMs; // (a short tap is held to its minimum)
            if(t-f.downAt<min){pulseBits_|=c.bits;pulseUntil_=std::max(t<pulseUntil_?pulseUntil_:t,f.downAt+min);}}
        if(wheelKind(c.kind) && f.moved){releaseBits_|=c.bits;releaseAt_=t+qol::Gesture::CloseGapMs;} // (slid: select)
        else if(wheelKind(c.kind) && t-f.downAt>TapMs)latchRequest_=qol::Gesture::wheelOf(c.bits); // (held still: latched, SPEC 3.6)
        else if(c.kind==Kind::WeaponTap)latchRequest_=qol::Gesture::Zone::Weapon; // (SPEC 3.12: the WEAPON button's tap = the latched weapon wheel)
        if(autotap_ && f.id==autotapFinger_){autotap_=false;autotapFinger_=0;}
    }
    void stickOut(Stick& s,int side) noexcept {stickValue(s.kx,s.ky,s.R,side==0,s.outX,s.outY);}
    float triggerValue(const Finger& f) const noexcept {
        const float h=f.control.r*2,d=(f.y0-f.y)/h;
        const float v=float(TriggerDown)+d*(d>0?float(255-TriggerDown):float(TriggerDown-TriggerMin)); // (one button height: 255 up, 77 down)
        return std::clamp(v,float(TriggerMin),255.0f);
    }
    float idleFactor(long long t) const noexcept {
        if(!fingers_.empty())return 1.0f;
        const long long quiet=t-lastTouch_;
        if(quiet<FadeIdleMs)return 1.0f;
        const float k=std::min(1.0f,float(quiet-FadeIdleMs)/float(FadeMs));
        return 1.0f-(1.0f-IdleAlpha)*k;
    }
    bool alphaChanging(long long t) const noexcept {
        const long long quiet=t-lastTouch_;
        return t-visibleSince_<FadeMs || (quiet>=FadeIdleMs && quiet<FadeIdleMs+FadeMs);
    }

    float vw_=0,vh_=0,mmPx_=10.0f;float frame_[4]={0,0,0,0};bool portrait_=false;float hud_[4]={0,0,0,0};
    std::vector<Control> controls_;
    int context_=FOOT,shownContext_=-1,pendingContext_=-1,pendingCount_=0;
    bool mapShown_=false;
    std::vector<Finger> fingers_;
    unsigned generation_=0;
    qol::Gesture::Zone latchRequest_=qol::Gesture::Zone::None;
    bool moreOpen_=false; // (layout B: the MORE row is open)
    std::string appAction_;bool trainerButton_=false,phoneUp_=false,phoneCall_=false; // (SPEC 3.14 / 3.15)
    bool cutscene_=false; // (SPEC 3.10 v2: the word says a cutscene can be playing - skippable())
    bool overlay_=false; // (a wheel is open elsewhere: overlay())
    int screens_=0,weaponGroup_=0;
    Overlay overlayMode_=Overlay::Skip,overlayNext_=Overlay::Skip;int overlayPending_=0;bool skipRevealed_=false; // (SPEC 3.10; the start: no pad until 2 reads of real gameplay)
    bool aimLatched_=false;long long lastSprintTap_=-(1LL<<40);bool autotap_=false;std::uintptr_t autotapFinger_=0;
    long long pulseUntil_=-(1LL<<40);int pulseBits_=0;
    long long releaseAt_=-(1LL<<40);int releaseBits_=0;
    bool hidden_=false,userHidden_=false;long long lastTouch_=0,visibleSince_=-(1LL<<40);
};
}
