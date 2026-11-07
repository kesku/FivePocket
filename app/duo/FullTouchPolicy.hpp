// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace recomp::apple::fulltouch {
inline constexpr double IdleSeconds=3.0;
inline constexpr double IdleAlpha=0.18; // Keep UIKit hit testing active (>0.01).
inline constexpr double FadeSeconds=0.25;
enum class Element {LeftStick,RightStick,A,B,X,Y,L1,R1,L2,R2,Dpad,Start,Back,L3,R3,Count};
inline constexpr std::size_t ElementCount=static_cast<std::size_t>(Element::Count);
enum class OverlayMode {System,FadingTouch,Recorded};
inline OverlayMode overlayMode(bool recorded,bool system) noexcept {
#if defined(APPLE_TOUCH_MUTANT_RECORDED_OVERLAY)
    if(recorded)return OverlayMode::FadingTouch;
#else
    if(recorded)return OverlayMode::Recorded;
#endif
    return system?OverlayMode::System:OverlayMode::FadingTouch;
}

class FadePolicy final {
    double last_;
    unsigned held_=0;
    bool closed_=false;
public:
    explicit FadePolicy(double now=0) noexcept:last_(now) {}
    bool press(double now) noexcept {
        if(closed_)return false;
        ++held_;last_=now;return true;
    }
    bool release(double now) noexcept {
        if(closed_ || !held_)return false;
        --held_;last_=now;return !held_;
    }
    bool idle(double now) const noexcept {
#if defined(APPLE_TOUCH_MUTANT_HELD_FADE)
        return !closed_ && now-last_>=IdleSeconds;
#else
        return !closed_ && !held_ && now-last_>=IdleSeconds;
#endif
    }
    bool closed() const noexcept {return closed_;}
    unsigned held() const noexcept {return held_;}
    void close() noexcept {closed_=true;held_=0;}
};

struct Position {double x=0,y=0;};
inline Position stickPosition(double x,double y,double width,double height) noexcept {
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
       !std::isfinite(height) || width<=0 || height<=0)return {};
    const double radius=std::min(width,height)*0.32;
    Position p{(x-width/2)/radius,(height/2-y)/radius}; // GameController Y is up.
#if defined(APPLE_TOUCH_MUTANT_Y_DOWN)
    p.y=-p.y;
#endif
    const double length=std::hypot(p.x,p.y);
    if(length>1){p.x/=length;p.y/=length;}
    return p;
}

inline Position dpadPosition(double x,double y,double width,double height) noexcept {
    const auto p=stickPosition(x,y,width,height);
    const double length=std::hypot(p.x,p.y);
    if(length<0.25)return {};
    // Eight directions, including diagonals. The central dead zone releases.
    constexpr double octant=0.3826834323650898; // sin(pi/8)
    Position result{std::abs(p.x)>=octant*length?(p.x<0?-1.0:1.0):0,
                    std::abs(p.y)>=octant*length?(p.y<0?-1.0:1.0):0};
#if defined(APPLE_TOUCH_MUTANT_DPAD_DOWN)
    result.y=-result.y;
#endif
    return result;
}

struct Rect {double x,y,width,height;};
struct Layout {
    Rect elements[ElementCount]{};
};
inline Layout layout(double x,double y,double width,double height) noexcept {
    const double size=std::min(140.0,std::max(88.0,std::min(width*0.19,height*0.3)));
#if defined(APPLE_TOUCH_MUTANT_TARGET_OMIT)
    const double diameter=std::min(48.0,height*0.15),auxiliaryHeight=40;
#else
    const double diameter=std::max(44.0,std::min(48.0,height*0.15)),auxiliaryHeight=44;
#endif
    const double spacing=diameter+4;
    const double right=x+width,bottom=y+height;
    Layout r;
    r.elements[0]={x+16,bottom-size-18,size,size};
    r.elements[1]={right-size-16,bottom-size-18,size,size};
    // Face buttons sit to the left of the right stick, clear of both sticks
    // and the shoulder row even in compact landscape. Hit regions never fade.
    const double cx=right-size-16-spacing-diameter/2-12;
    const double cy=bottom-std::max(size/2,spacing+diameter/2)-18;
    const Position face[]{{cx,cy+spacing},{cx+spacing,cy},{cx-spacing,cy},{cx,cy-spacing}};
    for(std::size_t i=0;i<4;++i)r.elements[i+2]={face[i].x-diameter/2,face[i].y-diameter/2,diameter,diameter};
    r.elements[6]={x+92,y+18,64,44};r.elements[8]={x+16,y+18,64,44};
    r.elements[7]={right-156,y+18,64,44};r.elements[9]={right-80,y+18,64,44};
    const auto index=[](Element value){return static_cast<std::size_t>(value);};
    const double pad=std::min(88.0,width*0.14);
    r.elements[index(Element::Dpad)]={x+size+28,bottom-pad-18,pad,pad};
    const double center=x+width/2;
    r.elements[index(Element::Back)]={center-48,y+18,44,auxiliaryHeight};
    r.elements[index(Element::Start)]={center+4,y+18,44,auxiliaryHeight};
    r.elements[index(Element::L3)]={x+16+(size-44)/2,bottom-size-62,44,auxiliaryHeight};
    r.elements[index(Element::R3)]={right-size-16+(size-44)/2,bottom-size-62,44,auxiliaryHeight};
#if defined(APPLE_TOUCH_MUTANT_OVERLAP)
    r.elements[index(Element::Dpad)]=r.elements[index(Element::LeftStick)];
#endif
    return r;
}
}
