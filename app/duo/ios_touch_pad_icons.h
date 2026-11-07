// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-touchpad (Claude 2026-10-24, iPad 235c): the universal touch controls' look, ONE source with Android (lead: no separate iOS style).
// Icons: android-qol's icons.txt (built in as ios_touch_pad_icon_data.h by gen_icon_data.py; parsed by icon_path.h). Drawing: a port of
// android-qol's reference renderer touch-controls-spec/icons/TouchRender.swift (drawIcon / drawDisc / drawStick) with SPEC 3.5's numbers:
// the San Andreas mobile disc (clear middle, white ring inside a black ring), white glyphs with a black outline, the SA stick.
// No game asset: the shapes are android-qol's own vector drawings. The 360 pad mode keeps its letters in coloured rings.
#import <UIKit/UIKit.h>
#include <map>
#include <string>
#include "icon_path.h"
#include "ios_touch_pad.h"
#include "ios_touch_pad_icon_data.h"

namespace recomp::apple::touchpad::icons {
constexpr CGFloat Outline=1.5;    // black outline, icon units each side (24-unit box)
constexpr CGFloat IconShare=0.70; // the icon's half size as a share of the disc's radius
struct Layer {char type;CGFloat width;CGPathRef path;};
using Groups=std::vector<std::vector<Layer>>;

inline CGPathRef cgPath(const iconpath::Path& p) {
    CGMutablePathRef m=CGPathCreateMutable();
    for(const auto& s:p.segments) {
        switch(s.op) {
        case iconpath::Op::Move:CGPathMoveToPoint(m,nullptr,s.x,s.y);break;
        case iconpath::Op::Line:CGPathAddLineToPoint(m,nullptr,s.x,s.y);break;
        case iconpath::Op::Quad:CGPathAddQuadCurveToPoint(m,nullptr,s.x1,s.y1,s.x,s.y);break;
        case iconpath::Op::Cubic:CGPathAddCurveToPoint(m,nullptr,s.x1,s.y1,s.x2,s.y2,s.x,s.y);break;
        case iconpath::Op::Close:CGPathCloseSubpath(m);break;
        case iconpath::Op::Ellipse:CGPathAddEllipseInRect(m,nullptr,CGRectMake(s.x-s.x1,s.y-s.y1,2*s.x1,2*s.y1));break;
        case iconpath::Op::Rect:CGPathAddRect(m,nullptr,CGRectMake(s.x,s.y,s.x1,s.y1));break;
        }
    }
    return m;
}
/** The shared icons, parsed once (main thread). A line that does not parse is left out (the control then draws its disc only). */
inline const std::map<std::string,Groups>& all() {
    static const std::map<std::string,Groups> table=[]{
        std::map<std::string,Groups> t;
        const std::string_view text=icondata::Text;
        std::size_t at=0;
        while(at<text.size()) {
            const std::size_t end=std::min(text.find('\n',at),text.size());
            const std::string_view line=text.substr(at,end-at);at=end+1;
            const std::size_t tab=line.find('\t');
            if(line.empty() || line[0]=='#' || tab==std::string_view::npos)continue;
            const auto icon=iconpath::parseIcon(line.substr(tab+1));
            if(!icon.ok)continue;
            Groups g;
            for(const auto& group:icon.groups){g.emplace_back();for(const auto& l:group)g.back().push_back({l.type,l.width,cgPath(l.path)});}
            t.emplace(std::string(line.substr(0,tab)),std::move(g));
        }
        return t;
    }();
    return table;
}
// (grey levels set straight on the context: no CGColorRef borrowed from a temporary UIColor)
inline void fillGrey(CGContextRef c,CGFloat g,CGFloat a){CGContextSetGrayFillColor(c,g,a);}
inline void strokeGrey(CGContextRef c,CGFloat g,CGFloat a){CGContextSetGrayStrokeColor(c,g,a);}

/** One icon centred at (cx, cy), the 24-unit box scaled to 2 x half; white glyph (held: black with a white edge). False: no such icon. */
inline bool drawIcon(CGContextRef c,const char* name,CGFloat cx,CGFloat cy,CGFloat half,CGFloat a,bool held,CGFloat glyphGrey=1,CGFloat outline=Outline) {
    const auto& t=all();const auto it=t.find(name);
    if(it==t.end())return false;
    CGContextSaveGState(c);
    CGContextTranslateCTM(c,cx,cy);CGContextScaleCTM(c,half/12,half/12);CGContextTranslateCTM(c,-12,-12);
    CGContextSetLineCap(c,kCGLineCapRound);CGContextSetLineJoin(c,kCGLineJoinRound);
    const CGFloat fg=held?0:glyphGrey,edge=held?1:0,detail=held?1:0; // (grey levels; the edge at 85 %; SPEC 3.12's weapon glyph #B4B4B4)
    for(const auto& g:it->second) {
        for(const auto& l:g)if(l.type=='F' || l.type=='S'){
            CGContextAddPath(c,l.path);strokeGrey(c,edge,a*0.85);
            CGContextSetLineWidth(c,l.type=='F'?2*outline:l.width+2*outline);CGContextStrokePath(c);}
        for(const auto& l:g)if(l.type=='F'){CGContextAddPath(c,l.path);fillGrey(c,fg,a);CGContextEOFillPath(c);}
        for(const auto& l:g)if(l.type=='S'){CGContextAddPath(c,l.path);strokeGrey(c,fg,a);CGContextSetLineWidth(c,l.width);CGContextStrokePath(c);}
        for(const auto& l:g)if(l.type=='B'){CGContextAddPath(c,l.path);fillGrey(c,detail,a);CGContextEOFillPath(c);}
        for(const auto& l:g)if(l.type=='K'){CGContextAddPath(c,l.path);strokeGrey(c,detail,a);CGContextSetLineWidth(c,l.width);CGContextStrokePath(c);}
    }
    CGContextRestoreGState(c);
    return true;
}
inline CGRect circle(CGFloat x,CGFloat y,CGFloat r){return CGRectMake(x-r,y-r,2*r,2*r);}
/** The SA disc: a clear middle (black 18 %), a white ring inside a black ring; held = white 40 %; a held pedal gets its tint at 45 %. */
// tint: 0 none, else 0xRRGGBB, a held pedal's colour (gas / up / throttle #40BF4D, brake / down / reverse #D93333).
inline void drawDisc(CGContextRef c,CGFloat cx,CGFloat cy,CGFloat r,bool held,CGFloat a,CGFloat dp,std::uint32_t tint) {
    if(held)fillGrey(c,1,0.40*a);else fillGrey(c,0,0.18*a);
    CGContextFillEllipseInRect(c,circle(cx,cy,r));
    if(tint){CGContextSetRGBFillColor(c,((tint>>16)&0xFF)/255.0,((tint>>8)&0xFF)/255.0,(tint&0xFF)/255.0,0.45*a);CGContextFillEllipseInRect(c,circle(cx,cy,r));}
    CGContextSetLineWidth(c,2.2*dp);strokeGrey(c,0,0.55*a);CGContextStrokeEllipseInRect(c,circle(cx,cy,r-1.1*dp));
    CGContextSetLineWidth(c,1.8*dp);strokeGrey(c,1,0.9*a);CGContextStrokeEllipseInRect(c,circle(cx,cy,r-3.1*dp));
}
/** The SA stick: a black ring with a thin white inner line, a white knob with a black edge, no letters. */
inline void drawStick(CGContextRef c,CGFloat cx,CGFloat cy,CGFloat R,CGFloat kx,CGFloat ky,bool held,CGFloat a,CGFloat dp) {
    fillGrey(c,0,0.12*a);CGContextFillEllipseInRect(c,circle(cx,cy,R));
    const CGFloat w=std::max(3*dp,R*0.07);
    CGContextSetLineWidth(c,w);strokeGrey(c,0,(held?0.8:0.6)*a);CGContextStrokeEllipseInRect(c,circle(cx,cy,R));
    CGContextSetLineWidth(c,1.2*dp);strokeGrey(c,1,(held?0.9:0.55)*a);CGContextStrokeEllipseInRect(c,circle(cx,cy,R-w*0.5-0.6*dp));
    const CGFloat kr=R*0.36;
    fillGrey(c,1,(held?0.95:0.8)*a);CGContextFillEllipseInRect(c,circle(cx+kx,cy+ky,kr));
    CGContextSetLineWidth(c,1.6*dp);strokeGrey(c,0,0.7*a);CGContextStrokeEllipseInRect(c,circle(cx+kx,cy+ky,kr));
}
/** The 360 pad mode's letters (unchanged by the restyle: SPEC 3.5). */
inline void drawLetters(CGContextRef,const char* glyph,CGFloat cx,CGFloat cy,CGFloat r,UIColor* colour) {
    NSString* s=[NSString stringWithUTF8String:glyph];
    const CGFloat size=std::strlen(glyph)>1?r*0.62:r*0.9;
    NSDictionary* at=@{NSFontAttributeName:[UIFont systemFontOfSize:size weight:UIFontWeightHeavy],NSForegroundColorAttributeName:colour};
    const CGSize z=[s sizeWithAttributes:at];
    [s drawAtPoint:CGPointMake(cx-z.width*0.5,cy-z.height*0.5) withAttributes:at];
}
}
