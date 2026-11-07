// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-duo (2026-10-24): the Duo's touch gestures merged into user 0's pad, as plain C++ (no UIKit), so the host test runs the same
// code the app runs (tests/duo_touch_chain_test.cpp: a touch on a drawn control through the touch backend and this merge).
// duo_app.h owns the one Input, the clock, the lock and the touch log; Session::snapshot calls its mergePad -> mergeCore.
#include "duo_gesture.h" // (beside this header: the stage, or the build's v507-alias overlay)
#include <cstdint>
#include <cstring>
#include <mutex>

namespace recomp::duo::app {
struct Input {
    std::mutex m;
    WheelGesture lower;MapTouch top;
    int mask=WheelGesture::mask(false,false,false);bool lastLb=false;
    Pad lastOut{};std::uint32_t serial=0;bool merged=false;
};
// The pad the game is given (in.m held by the caller; t = the gesture clock in ms): the physical / on-screen-control state with the
// gestures merged as WheelTouch.merge does (buttons OR'd, the right stick replaced only while a gesture pushes). An unchanged result
// leaves st exactly as it came (packet included); a changed one gets a packet number of its own. *in_ / *out_: the two pads (log).
template<class State> inline void mergeCore(Input& in,State& st,long long t,Pad* in_=nullptr,Pad* out_=nullptr) noexcept {
    const bool lb=WheelGesture::held(st.buttons,in.mask);
    in.lower.origin(WheelGesture::originX(st.buttons,in.mask));
    in.lower.pad(st.buttons);
    if(lb!=in.lastLb){in.lastLb=lb;in.lower.physical(t,lb);}
    if(in.lower.due()<=t)in.lower.tick(t);
    if(in.top.due()<=t)in.top.tick(t);
    Pad p{st.buttons,st.rightX,st.rightY,st.leftX,st.leftY,st.leftTrigger,st.rightTrigger};
    const Pad o=MapTouch::merge(&in.top,WheelGesture::merge(&in.lower,p));
    if(in_)*in_=p;
    if(out_)*out_=o;
    const bool same=o.buttons==p.buttons && o.rightX==p.rightX && o.rightY==p.rightY && o.leftX==p.leftX && o.leftY==p.leftY &&
                    o.leftTrigger==p.leftTrigger && o.rightTrigger==p.rightTrigger;
    if(same){in.merged=false;return;}
    if(!in.merged || std::memcmp(&o,&in.lastOut,sizeof o)){++in.serial;in.lastOut=o;in.merged=true;}
    st.buttons=decltype(st.buttons)(o.buttons);st.rightX=decltype(st.rightX)(o.rightX);st.rightY=decltype(st.rightY)(o.rightY);
    st.leftX=decltype(st.leftX)(o.leftX);st.leftY=decltype(st.leftY)(o.leftY);
    st.leftTrigger=decltype(st.leftTrigger)(o.leftTrigger);st.rightTrigger=decltype(st.rightTrigger)(o.rightTrigger);
    st.packet=0x40000000u|((in.serial&0x3Fu)<<24)|(st.packet&0x00FFFFFFu); // (map_tap.h uses bit 31 for its own presses)
}

}
