// Attempt v309: the on-screen frame-rate counter (files/private-trace/diag-fps-overlay). A 5x7 bitmap font drawn at scale 2
// into a small opaque RGBA8 strip (dark box, light text), which the renderer copies into the top-left corner of the present
// image after the frame's blit. Text: "<fps> FPS <ms> MS <worst> MAX" over the last half second of presents. Pure CPU;
// host test: tests/fps_overlay_test.cpp.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace recomp::gpu::generic::v309 {
constexpr unsigned GlyphW=5,GlyphH=7,Scale=2,Pad=4,Advance=(GlyphW+1)*Scale;
// Rows top to bottom, bit 4 = leftmost column.
inline const std::array<std::uint8_t,7>* glyph(char c) {
    static const std::array<std::uint8_t,7> digits[10]={
        {{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},{{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},{{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
        {{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},{{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},{{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
        {{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},{{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},{{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
        {{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}}};
    static const std::array<std::uint8_t,7> F{{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},P{{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        S{{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},M{{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},A{{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
        X{{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},dot{{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},space{{0,0,0,0,0,0,0}},
        R{{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},E{{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},L{{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
        open{{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},close{{0x08,0x04,0x02,0x02,0x02,0x04,0x08}}; // v572: "(30.1 REAL)"
    if(c>='0' && c<='9')return &digits[c-'0'];
    switch(c){case 'F':return &F;case 'P':return &P;case 'S':return &S;case 'M':return &M;case 'A':return &A;case 'X':return &X;
        case '.':return &dot;case ' ':return &space;case 'R':return &R;case 'E':return &E;case 'L':return &L;case '(':return &open;
        case ')':return &close;default:return nullptr;}
}
inline std::string text(double fps,double ms,double worstMs) {
    char b[64];std::snprintf(b,sizeof b,"%.1f FPS %.1f MS %.1f MAX",fps,ms,worstMs);return b;
}
// v572 (owner: "wants to see 60"): while frame generation shows extra pictures, the rate SHOWN (real frames + the pairs' second
// pictures, the presenter's own count) first and the real rate in brackets; ms / max stay the real frames' (the game's frame time).
// shownFps <= fps (no extra picture in the window): exactly the text above.
inline std::string text(double fps,double shownFps,double ms,double worstMs) {
    if(!(shownFps>fps))return text(fps,ms,worstMs);
    char b[80];std::snprintf(b,sizeof b,"%.1f FPS (%.1f REAL) %.1f MS %.1f MAX",shownFps,fps,ms,worstMs);return b;
}
struct Strip {unsigned width=0,height=0;std::vector<std::uint8_t> rgba;};
// Renders `s` (characters glyph() knows; others as spaces) into an opaque strip.
inline Strip render(const std::string& s) {
    Strip out;out.width=2*Pad+unsigned(s.size())*Advance;out.height=2*Pad+GlyphH*Scale;
    out.rgba.assign(std::size_t{out.width}*out.height*4,0);
    for(std::size_t i=0;i<out.rgba.size();i+=4){out.rgba[i]=16;out.rgba[i+1]=16;out.rgba[i+2]=16;out.rgba[i+3]=255;} // dark box
    for(std::size_t k=0;k<s.size();++k) {
        const auto* g=glyph(s[k]);if(!g)continue;
        for(unsigned y=0;y<GlyphH;++y)for(unsigned x=0;x<GlyphW;++x) {
            if(!(((*g)[y]>>(GlyphW-1-x))&1))continue;
            for(unsigned dy=0;dy<Scale;++dy)for(unsigned dx=0;dx<Scale;++dx) {
                const unsigned px=Pad+unsigned(k)*Advance+x*Scale+dx,py=Pad+y*Scale+dy;
                auto* p=&out.rgba[(std::size_t{py}*out.width+px)*4];p[0]=255;p[1]=235;p[2]=90;p[3]=255; // light yellow
            }
        }
    }
    return out;
}
// Present intervals averaged over windows of at least `windowNs`; the text changes once per window.
struct Meter {
    std::uint64_t windowStart=0,last=0,worst=0;unsigned frames=0;double fps=0,ms=0,worstMs=0;bool ready=false;
    // v572: `extraTotal` = a running count of extra pictures shown (frame generation's second pictures); the window's increase
    // gives shownFps = (real intervals + extra pictures) / span. Callers without one pass 0: shownFps = fps.
    std::uint64_t extraAtStart=0;double shownFps=0;
    // Returns true when a window closed (fps / ms / worstMs / shownFps updated).
    bool present(std::uint64_t nowNs,std::uint64_t windowNs=500'000'000ull,std::uint64_t extraTotal=0) {
        if(last){const auto d=nowNs-last;if(d>worst)worst=d;++frames;}
        last=nowNs;
        if(!windowStart){windowStart=nowNs;extraAtStart=extraTotal;return false;}
        if(nowNs-windowStart<windowNs || !frames)return false;
        const double span=double(nowNs-windowStart);
        const std::uint64_t extra=extraTotal>=extraAtStart?extraTotal-extraAtStart:0;
        fps=frames*1e9/span;ms=span/frames/1e6;worstMs=worst/1e6;shownFps=double(frames+extra)*1e9/span;ready=true;
        windowStart=nowNs;extraAtStart=extraTotal;frames=0;worst=0;return true;
    }
};
}

// v713 (owner 2026-10-25: "build a simple fps counter, one more aesthetic for a game, put in the top left corner"; SPEC 5.5): "60 FPS"
// in white with a black drop shadow and no box, the rate SHOWN; with frame generation the real rate small and grey beside it. The
// v309 glyphs made bold (each row OR'd with itself one column right: 6 x 7) at scale 3 (2 for the small number). The picture is copied
// into the frame (no blending), so the label carries its opaque pixels as runs: one copy region per run, the background untouched.
// files/private-trace/diag-fps-detail = the v309 strip (fps, ms, max) instead. Host test: tests/fps_label_test.cpp.
// v714 (owner 2026-10-25: "that fps counter is not clean. it needs to be minimalistic and much smaller"; his pick: "60" alone): the
// label moves to the app (an anti-aliased text view: white 78 %, a soft shadow, ~1.4 % of the frame high), which reads the rates through
// recomp_fps_v1 (generic_renderer.cpp). While an app has read them within AppHoldMs the library draws no label of its own; the v309
// strip (diag-fps-detail, our measuring) is drawn whatever the app does.
namespace recomp::gpu::generic::v714 {
constexpr long long AppHoldMs=2000;
inline bool appDrawing(long long nowMs,long long lastReadMs) noexcept {return lastReadMs>0 && nowMs>=lastReadMs && nowMs-lastReadMs<=AppHoldMs;}
}
namespace recomp::gpu::generic::v713 {
constexpr unsigned BigScale=3,SmallScale=2,GlyphW=6,GlyphH=7,Shadow=2,Gap=6;
constexpr std::uint8_t White=255,Grey=190;
struct Run {std::uint16_t x,y,w;};
struct Label {unsigned width=0,height=0;std::vector<std::uint8_t> rgba;std::vector<Run> runs;std::string big,small;};
inline std::uint8_t boldRow(std::uint8_t r) noexcept {return std::uint8_t((r<<1)|r);} // (bit 5 = leftmost of 6 columns)
inline unsigned advance(unsigned scale) noexcept {return (GlyphW+1)*scale;}
// The texts: "<shown> FPS", and the real rate when frame generation shows more pictures than the game draws ("" otherwise).
inline void texts(double fps,double shownFps,std::string& big,std::string& small) {
    const bool gen=shownFps>fps+0.5;char b[32];
    std::snprintf(b,sizeof b,"%d FPS",int(std::lround(gen?shownFps:fps)));big=b;
    if(gen){std::snprintf(b,sizeof b,"%d",int(std::lround(fps)));small=b;}else small.clear();
}
inline Label render(double fps,double shownFps) {
    Label out;texts(fps,shownFps,out.big,out.small);
    const unsigned bigW=unsigned(out.big.size())*advance(BigScale),smallW=out.small.empty()?0:Gap+unsigned(out.small.size())*advance(SmallScale);
    out.width=bigW+smallW+Shadow;out.height=GlyphH*BigScale+Shadow;
    out.rgba.assign(std::size_t{out.width}*out.height*4,0);
    const auto put=[&](unsigned x,unsigned y,std::uint8_t v){auto* p=&out.rgba[(std::size_t{y}*out.width+x)*4];p[0]=p[1]=p[2]=v;p[3]=255;};
    // pass 0: the shadow (black, down-right); pass 1: the glyphs over it
    for(int pass=0;pass<2;++pass) {
        unsigned x0=0;
        for(int part=0;part<2;++part) {
            const std::string& s=part?out.small:out.big;const unsigned sc=part?SmallScale:BigScale;
            if(part && !s.empty())x0+=Gap;
            const unsigned top=GlyphH*BigScale-GlyphH*sc; // (the small number sits on the big one's baseline)
            const std::uint8_t ink=pass==0?0:(part?Grey:White);const unsigned off=pass==0?(part?1u:Shadow):0u;
            for(std::size_t k=0;k<s.size();++k) {
                const auto* g=v309::glyph(s[k]);
                if(g)for(unsigned y=0;y<GlyphH;++y) {
                    const std::uint8_t row=boldRow((*g)[y]);
                    for(unsigned x=0;x<GlyphW;++x) {
                        if(!((row>>(GlyphW-1-x))&1))continue;
                        for(unsigned dy=0;dy<sc;++dy)for(unsigned dx=0;dx<sc;++dx)
                            put(x0+unsigned(k)*advance(sc)+x*sc+dx+off,top+y*sc+dy+off,ink);
                    }
                }
            }
            x0+=unsigned(s.size())*advance(sc);
        }
    }
    for(unsigned y=0;y<out.height;++y)for(unsigned x=0;x<out.width;) {
        if(!out.rgba[(std::size_t{y}*out.width+x)*4+3]){++x;continue;}
        unsigned e=x;while(e<out.width && out.rgba[(std::size_t{y}*out.width+e)*4+3])++e;
        out.runs.push_back(Run{std::uint16_t(x),std::uint16_t(y),std::uint16_t(e-x)});x=e;
    }
    return out;
}
}
#ifdef RECOMP_THOR_EDITION
// v826 (thor-radio; from v850's fps_overlay.h): the Thor edition's label switch for the v309 strip (map_tap_renderer.inc thorFpsDraw).
// ios-duo: 238e draws the game-style label itself (generic_renderer.cpp present()), so the Duo's thorFpsDraw draws nothing (see there).
#include <atomic>
namespace recomp::gpu::generic::v309 {
inline std::atomic<bool>& thorLabelOn() noexcept {static std::atomic<bool> b{false};return b;}
inline bool stripShown() noexcept {return !thorLabelOn().load(std::memory_order_relaxed);}
}
#endif /* RECOMP_THOR_EDITION */
