// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-duo (2026-10-24): the Duo edition's screen state shared by the presenter view (ios_frame_presenter.mm: the layout) and the app
// (Main.mm / duo_app.h: the settings, the toggle, the touch, the library's lower-screen window). RECOMP_DUO_EDITION builds only.
//   split   1 = portrait split (the default): the game's 16:9 frame fitted into the top half, the bottom half = the lower screen;
//           0 = fullscreen: the regular build's landscape layout, no lower screen.
//   lower*  the lower screen's CAMetalLayer (retained once, for the process) while it is laid out and shown, else null; its generation
//           changes on every show / hide / resize (recomp_set_hud_window_v1's contract: the library then makes its swapchain again).
//   game*   the game frame's rectangle in the root view (points), for the top screen's map touch (MapTouch maps it to 1920x1080).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::duo::state {
inline std::atomic<int> split{1};
inline std::atomic<void*> lowerLayer{nullptr};
inline std::atomic<std::uint64_t> lowerGeneration{1};
inline std::atomic<float> gameX{0},gameY{0},gameW{0},gameH{0};
inline std::atomic<float> lowerW{0},lowerH{0},lowerScale{1}; // (points; the layer's contentsScale)
// The split layout applies to a portrait (taller than wide) view only: a landscape view in split mode (before the system rotated it)
// keeps the regular layout until the rotation arrives.
// (2026-11-03, the real Duo) the hinge, from UIKit's UIHingeInteraction (iOS 27.1; ios_frame_presenter.mm): -1 = no hinge reported,
// otherwise UIHingeStatus (0 unknown, 1 closed, 2 partially open, 3 fully open) and the angle in radians. Partially open = the
// Thor / DS posture: the split sits at the fold (assumed the middle of the screen); otherwise the game is flush at the top (no bars).
inline std::atomic<int> hingeStatus{-1};
inline std::atomic<float> hingeAngle{0};
inline bool atFold() noexcept {return hingeStatus.load(std::memory_order_relaxed)==2;}
inline bool splitLayout(double width,double height) noexcept {return split.load(std::memory_order_relaxed)!=0 && height>width;}
// The split's rectangles in the view (points): the top half (floor of half the height), the 1280x720 frame FIT (16:9, uniform scale)
// and centred in it, and the lower screen = the rest of the height. ios_frame_presenter.mm (duoLayout) and duo_app.h use it.
struct Rect {double x=0,y=0,w=0,h=0;};
struct SplitGeometry {double top=0,scale=0;Rect game,lower;};
// The top screen's picture. 2026-11-05: 25s's Stretch drew "ewwww stretch looks ugly" from the owner; main: the default is Fit, every
// mode stays in the toggle, plus Fit + Blur. The divider stays at the middle of a portrait view (669 x 951 pt: 475 / 476; the hinge
// is logged, it does not move it). The 1280x720 frame goes into the TOP AREA (the top half while split; the whole view in landscape
// or with the lower screen off) by the TOP MODE, live from the side menu's Dual Screens > Top Screen row (settings/duo-top-screen.txt):
//   0 Fit (the default): one scale (the smaller), the whole frame centred, bars where the area's shape differs;
//   1 Zoom: one scale (the larger), centred, the overflow cropped by the layer's contentsRect (never drawn over the lower screen);
//   2 Stretch: the area exactly, one scale per axis (no bars, no crop, the aspect not kept).
// (Fit + Blur was mode 1 in 26s: its CAReplicatorLayer around the game's Metal layer left the Duo's screen black, 2026-11-05 15:16; out
// until a way of drawing the blurred copy is proven on the device. topPicture still reports the bars.)
//   3 4:3 (28s, main 2026-11-05: Fit the default, 4:3 a choice): the game's own non-widescreen layout, drawn as Fit. It needs a
//     restart: the game asks for the video mode once at its start.
// frame4x3 = the 4:3 layout is on for THIS process (fixed at launch by duo_app.h loadMode: the saved mode 4:3, or RECOMP_DUO_4X3=1 in
// a test launch's environment): the runtime tells the game it is on a 4:3 display (guest_import_registry.cpp: fIsWideScreen = 0) and
// the game composes a 4:3 picture into the same 1280x720 buffer (as for a 4:3 TV's scaler); the frame is then laid out as 960 x 720
// logical units, every mode the same way (a live switch away from 4:3 keeps the 4:3 frame until the restart).
inline std::atomic<int> topMode{0};
inline std::atomic<int> frame4x3{0};
inline constexpr int TopModes=4,Mode4x3=3;
inline constexpr const char* TopModeNames[TopModes]{"fit","zoom","stretch","4x3"};
inline int clampMode(int m) noexcept {return m>=0 && m<TopModes?m:0;}
inline int pictureMode(int m) noexcept {const int c=clampMode(m);return c==Mode4x3?0:c;} // (4:3 is drawn as Fit)
inline bool restartPending(int chosen,bool active4x3) noexcept {return (clampMode(chosen)==Mode4x3)!=active4x3;}
// The launch's choice (duo_app.h loadMode): RECOMP_DUO_4X3's value (or null) and the mode read at launch.
inline bool launch4x3(const char* env,int mode) noexcept {return (env && env[0]=='1') || clampMode(mode)==Mode4x3;}
inline std::string topModeText(int m) {return std::string(TopModeNames[clampMode(m)])+"\n";}
inline Rect topArea(double width,double height) noexcept {return splitLayout(width,height)?Rect{0,0,width,std::floor(height/2)}:Rect{0,0,width,height};}
inline double frameUnitsWide(bool narrow) noexcept {return narrow?960.0:1280.0;} // (the frame's shape in logical units: 16:9 or 4:3)
// The game frame in the area: picture = where the whole frame lies (it overhangs the area in Zoom; the touch maps use it); the
// layer: its centre, per-axis scale of the 1280x720 layer and the visible part (contentsRect: x, y, w, h in the unit square). Fit +
// Fit: the bars (the area outside the picture: up to two strips).
struct TopPicture {Rect picture;double cx=0,cy=0,scaleX=0,scaleY=0,cropX=0,cropY=0,cropW=1,cropH=1;bool valid=false;
    Rect bars[2];int barCount=0;};
inline TopPicture topPicture(const Rect& area,int mode,bool narrow=false) noexcept {
    TopPicture t;if(!(area.w>0) || !(area.h>0) || !std::isfinite(area.w) || !std::isfinite(area.h))return t;
    const double fw=frameUnitsWide(narrow),fh=720.0,px=fw/1280.0; // (px: logical units per layer pixel across)
    t.cx=area.x+area.w/2;t.cy=area.y+area.h/2;t.valid=true;
    const double fit=std::min(area.w/fw,area.h/fh),cover=std::max(area.w/fw,area.h/fh);
    switch(pictureMode(mode)) {
    case 2:t.scaleX=area.w/1280.0;t.scaleY=area.h/720.0;t.picture=area;break;
    case 1:{const double s=cover;t.picture={t.cx-fw*s/2,t.cy-fh*s/2,fw*s,fh*s};
        t.cropW=std::min(1.0,area.w/(fw*s));t.cropH=std::min(1.0,area.h/(fh*s));t.cropX=(1-t.cropW)/2;t.cropY=(1-t.cropH)/2;
        t.scaleX=t.cropW*s*px;t.scaleY=t.cropH*s;break;} // (the layer covers the area; contentsRect shows the middle of the frame)
    default:{const double s=fit;t.scaleX=s*px;t.scaleY=s;t.picture={t.cx-fw*s/2,t.cy-fh*s/2,fw*s,fh*s};
        if(t.picture.h<area.h-0.5){t.bars[0]={area.x,area.y,area.w,t.picture.y-area.y};t.bars[1]={area.x,t.picture.y+t.picture.h,area.w,area.y+area.h-(t.picture.y+t.picture.h)};t.barCount=2;}
        else if(t.picture.w<area.w-0.5){t.bars[0]={area.x,area.y,t.picture.x-area.x,area.h};t.bars[1]={t.picture.x+t.picture.w,area.y,area.x+area.w-(t.picture.x+t.picture.w),area.h};t.barCount=2;}
        break;}
    }
    return t;
}
// The split's rectangles (portrait, split on): the top area, the game picture in it by the current mode, the lower screen below.
inline SplitGeometry splitGeometry(double width,double height) noexcept {
    SplitGeometry g;g.top=std::floor(height/2);
    const auto t=topPicture({0,0,width,g.top},topMode.load(std::memory_order_relaxed),frame4x3.load(std::memory_order_relaxed)!=0);
    g.scale=t.scaleY;g.game=t.picture;g.lower={0,g.top,width,height-g.top};
    return g;
}
// The layout line is printed again whenever any of its values changes (a rotation, a size change, the mode, the lower screen).
struct LayoutKey {int split=-1,mode=-1,narrow=0;long w=0,h=0,px=0,py=0,pw=0,ph=0,lw=0,lh=0;
    bool operator==(const LayoutKey&) const=default;};
inline LayoutKey layoutKey(bool split,int mode,double w,double h,const Rect& picture,double lw,double lh,bool narrow=false) noexcept {
    return {split?1:0,mode,narrow?1:0,std::lround(w),std::lround(h),std::lround(picture.x),std::lround(picture.y),std::lround(picture.w),std::lround(picture.h),
            std::lround(lw),std::lround(lh)};
}
// The on-screen controls' rectangle (duo_app.h DuoOverlay) and the full-screen toggle in the lower half's own coordinates.
// (2026-10-25, the Thor mirror) the universal pad (238e's PortQolTouchView) over the TOP half in the split - the lower half is the Thor's
// lower screen with its own touch (wheels, map tap, phone, Quick Save) - and over the whole view otherwise. The toggle: the lower half's
// top-left corner, above the minimap frame and left of the stars (tests/duo_controls_layout_test.cpp: mid-height on the left edge
// overlapped the frame, 77 px from the edge on the iPad mini).
inline Rect controlsRect(double width,double height) noexcept {
    return splitLayout(width,height)?Rect{0,0,width,splitGeometry(width,height).top}:Rect{0,0,width,height};
}
inline Rect toggleRect(double lowerWidth,double lowerHeight) noexcept {(void)lowerWidth;(void)lowerHeight;return {8,8,36,36};}

// The side menu's Dual Screens rows -> the Thor's files (RuntimeMapping.java withWord; hud_screen_layout.h parseSettings):
// settings/second-screen.txt keeps its other words, with "hud" first (the library takes the options only after it) and the four tokens
// wheel= / radio= / character= / phone=main present exactly when chosen.
inline std::vector<std::string> words(const std::string& t) {
    std::vector<std::string> w;std::string cur;
    for(const char c:t){if(c==' ' || c=='\t' || c=='\n' || c=='\r' || c==0){if(!cur.empty())w.push_back(cur);cur.clear();}else cur+=c;}
    if(!cur.empty())w.push_back(cur);
    return w;
}
inline bool lowerIs(const std::string& a,const char* b) {
    std::size_t i=0;for(;b[i];++i)if(i>=a.size() || std::tolower((unsigned char)a[i])!=b[i])return false;
    return i==a.size();
}
inline std::string secondScreenText(const std::string& old,bool weaponMain,bool radioMain,bool characterMain,bool phoneMain) {
    std::string out="hud";
    for(const auto& w:words(old)) {
        if(lowerIs(w,"hud") || w=="wheel=main" || w=="radio=main" || w=="character=main" || w=="phone=main")continue;
        out+=' ';out+=w;
    }
    if(weaponMain)out+=" wheel=main";
    if(radioMain)out+=" radio=main";
    if(characterMain)out+=" character=main";
    if(phoneMain)out+=" phone=main";
    return out+"\n";
}
inline bool hasToken(const std::string& t,const char* token) {for(const auto& w:words(t))if(w==token)return true;return false;}
// private-trace/diag-phone-pov for the Phone Background row (RuntimeMapping.java): floor = no words, close w=0.07, wide w=0.14, black off=1.
inline std::string phonePovText(int bg) {return bg==3?"off=1\n":bg==1?"w=0.07\n":bg==2?"w=0.14\n":"";}
inline int phoneBgOf(const std::string& t) {return hasToken(t,"off=1")?3:hasToken(t,"w=0.07")?1:hasToken(t,"w=0.14")?2:0;}
// settings/minimap.txt (hud_replay.h parseMapMode: absent / anything else = bottom): the row's 0 = both, 1 = bottom.
inline int minimapOf(const std::string& t) {const auto w=words(t);return !w.empty() && lowerIs(w[0],"both")?0:1;}
// settings/duo-top-screen.txt: stretch | fit | zoom (topMode above)
inline int topModeOf(const std::string& t) { // (no file or an unknown word: Fit)
    const auto w=words(t);if(w.empty())return 0;
    for(int m=0;m<TopModes;++m)if(lowerIs(w[0],TopModeNames[m]))return m;
    return 0;
}
}
