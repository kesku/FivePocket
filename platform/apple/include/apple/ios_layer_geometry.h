// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

// screen-fit: the visible fraction of the 1280x720 picture (1 = all of it), for the frame-rate counter's position
// (generic_renderer.cpp defines them; the layout below stores them).
extern "C" std::atomic<float> recomp_ios_visible_x, recomp_ios_visible_y;
// screen-fit A/B (Claude 2026-10-20): Main.mm's 0.5 s check stores 1 (fill) or 0 (fit) here and lays the view out again; -1 before
// its first check (the launch environment decides). generic_renderer.cpp defines it.
extern "C" std::atomic<int> recomp_ios_screen_fill;
namespace recomp::apple::detail {
struct IosLayerGeometry {
    double boundsWidth=0,boundsHeight=0,scale=0;
    bool valid=false;
    // fill by crop (Claude 2026-10-20): the layer's transform per axis and its contentsRect (unit square = the whole picture).
    // Fit and the old overhanging fill: scaleX = scaleY = scale, crop = whole.
    double scaleX=0,scaleY=0,cropX=0,cropY=0,cropW=1,cropH=1;
};
// The fill mode: 0 fit (letterbox), 1 fill by crop (the default), 2 fill by overhang (the c1-fill-137 layout: the scaled layer
// overhangs the screen and the system clips it; c357: -1.3 ms per real frame when switched to fit while driving, a
// compositing cost), 3 fill by crop with the scale capped at 2.0 (A/B aid for the hypothesis that the display hardware scales a
// layer up to 2x and a larger factor falls back to GPU composition; thin bars remain where 2.0 does not reach the edges).
// RECOMP_IOS_SCREEN_FIT=fit|overhang picks the start; the A/B check below can change it at run time.
inline int iosScreenFillMode() noexcept {
    static const int start=[] {const char* e=std::getenv("RECOMP_IOS_SCREEN_FIT");
        return !e?1:!std::strcmp(e,"fit")?0:!std::strcmp(e,"overhang")?2:1;}();
    const int now=recomp_ios_screen_fill.load(std::memory_order_relaxed);
    return now<0?start:now;
}
// screen-fit (Claude 2026-10-20, owner: "resolution needs to ... adapt to different screen sizes without black bars"): the
// game renders a fixed 1280x720 (16:9) picture. FILL (the default) scales it to cover the whole screen, keeping its aspect
// ratio; the overflow past the screen edges is clipped (iPad mini 1.52:1: ~7 % off each side; a wider phone loses a
// little top and bottom instead). No bars and no stretching. RECOMP_IOS_SCREEN_FIT=fit restores the letterbox.
inline bool iosScreenFill() noexcept {return iosScreenFillMode()!=0;}
// Keep the layer's natural pixel extent fixed. Its parent-space transform fits or fills it without altering guest pixels or
// the Vulkan swapchain extent.
inline IosLayerGeometry iosLayerGeometry(double width,double height) noexcept {
    if(!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0)return {};
    const int mode=iosScreenFillMode();
    double scale=mode?std::max(width/1280.0,height/720.0):std::min(width/1280.0,height/720.0);
    if(mode==3)scale=std::min(scale,std::max(2.0,std::min(width/1280.0,height/720.0))); // (never below fit's scale)
    if(!std::isfinite(scale) || scale<=0)return {};
    const double vx=std::min(1.0,width/(1280.0*scale)),vy=std::min(1.0,height/(720.0*scale));
    recomp_ios_visible_x.store(float(vx),std::memory_order_relaxed);
    recomp_ios_visible_y.store(float(vy),std::memory_order_relaxed);
#if defined(APPLE_IOS_PRESENTER_MUTANT_GEOMETRY)
    return {1280.0*scale,720.0*scale,1.0,true,1.0,1.0};
#else
    if(mode==1 || mode==3) // fill by crop: the 1280x720 layer covers the screen (per-axis scale) and shows only the visible part
        return {1280.0,720.0,scale,true,vx*scale,vy*scale,(1.0-vx)/2.0,(1.0-vy)/2.0,vx,vy};
    return {1280.0,720.0,scale,true,scale,scale};
#endif
}
}
