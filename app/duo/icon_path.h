// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-touchpad (Claude 2026-10-24, iPad 235+): the shared touch-control icon format's parser. ONE icon set for Android and iOS:
// android-qol's local-analysis/touch-controls-spec/icons/icons.txt (SPEC 3.5). One line per control id, `id<TAB>group||group`, a group is
// `layer;layer`, a layer `TYPE:path`: F white fill, S<w> white stroke (both get the black outline), B black fill, K<w> black stroke.
// Paths in a 24 x 24 box: M L C Q Z (plus H V and relative forms, accepted) and the shapes O cx cy r, E cx cy rx ry, R x y w h; fill even-odd.
// Pure C++: tests_icon_path.cpp runs it on the host (and checks the shared file); ios_touch_pad_icons.h turns it into CGPaths.
// Malformed data parses to nothing (ok == false), never half a shape.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::apple::touchpad::iconpath {
enum class Op {Move,Line,Quad,Cubic,Close,Ellipse,Rect};
struct Segment {Op op;float x1=0,y1=0,x2=0,y2=0,x=0,y=0;}; // absolute; Quad: (x1,y1) control; Cubic: (x1,y1),(x2,y2) controls;
                                                       // Ellipse: centre (x,y), radii (x1,y1); Rect: origin (x,y), size (x1,y1)
struct Path {std::vector<Segment> segments;bool ok=false;};

inline Path parse(std::string_view d) {
    Path out;std::size_t i=0;
    const auto skip=[&]{while(i<d.size() && (std::isspace(static_cast<unsigned char>(d[i])) || d[i]==','))++i;};
    const auto number=[&](float& v)->bool {
        skip();if(i>=d.size())return false;
        const std::size_t start=i;
        if(d[i]=='+' || d[i]=='-')++i;
        bool digits=false,dot=false;
        while(i<d.size() && (std::isdigit(static_cast<unsigned char>(d[i])) || (d[i]=='.' && !dot))){if(d[i]=='.')dot=true;else digits=true;++i;}
        if(digits && i<d.size() && (d[i]=='e' || d[i]=='E')){
            std::size_t j=i+1;if(j<d.size() && (d[j]=='+' || d[j]=='-'))++j;
            if(j<d.size() && std::isdigit(static_cast<unsigned char>(d[j]))){i=j;while(i<d.size() && std::isdigit(static_cast<unsigned char>(d[i])))++i;}
        }
        if(!digits){i=start;return false;}
        v=std::strtof(std::string(d.substr(start,i-start)).c_str(),nullptr);return true;
    };
    float cx=0,cy=0,sx=0,sy=0;char cmd=0;bool started=false;
    while(true) {
        skip();if(i>=d.size())break;
        if(std::isalpha(static_cast<unsigned char>(d[i]))){cmd=d[i++];}
        else if(!cmd)return {};
        const bool rel=std::islower(static_cast<unsigned char>(cmd));
        const float ox=rel?cx:0,oy=rel?cy:0;
        Segment s{};
        switch(std::toupper(static_cast<unsigned char>(cmd))) {
        case 'M':
            if(!number(s.x) || !number(s.y))return {};
            s.x+=ox;s.y+=oy;s.op=Op::Move;cx=sx=s.x;cy=sy=s.y;started=true;
            out.segments.push_back(s);cmd=rel?'l':'L';continue; // (further pairs are line-tos)
        case 'L':if(!number(s.x) || !number(s.y))return {};s.x+=ox;s.y+=oy;s.op=Op::Line;break;
        case 'H':if(!number(s.x))return {};s.x+=ox;s.y=cy;s.op=Op::Line;break;
        case 'V':if(!number(s.y))return {};s.y+=oy;s.x=cx;s.op=Op::Line;break;
        case 'Q':if(!number(s.x1) || !number(s.y1) || !number(s.x) || !number(s.y))return {};
            s.x1+=ox;s.y1+=oy;s.x+=ox;s.y+=oy;s.op=Op::Quad;break;
        case 'C':if(!number(s.x1) || !number(s.y1) || !number(s.x2) || !number(s.y2) || !number(s.x) || !number(s.y))return {};
            s.x1+=ox;s.y1+=oy;s.x2+=ox;s.y2+=oy;s.x+=ox;s.y+=oy;s.op=Op::Cubic;break;
        case 'O':if(!number(s.x) || !number(s.y) || !number(s.x1) || s.x1<=0)return {};
            s.y1=s.x1;s.op=Op::Ellipse;out.segments.push_back(s);started=true;cmd=0;continue; // (a whole subpath; no current point change)
        case 'E':if(!number(s.x) || !number(s.y) || !number(s.x1) || !number(s.y1) || s.x1<=0 || s.y1<=0)return {};
            s.op=Op::Ellipse;out.segments.push_back(s);started=true;cmd=0;continue;
        case 'R':if(!number(s.x) || !number(s.y) || !number(s.x1) || !number(s.y1) || s.x1<=0 || s.y1<=0)return {};
            s.op=Op::Rect;out.segments.push_back(s);started=true;cmd=0;continue;
        case 'Z':s.op=Op::Close;s.x=sx;s.y=sy;out.segments.push_back(s);cx=sx;cy=sy;cmd=0;continue;
        default:return {};
        }
        if(!started)return {}; // (a drawing command before any move-to)
        out.segments.push_back(s);cx=s.x;cy=s.y;
    }
    out.ok=started;
    if(!out.ok)out.segments.clear();
    return out;
}
/** Whether every point (end points, controls, shape extents) lies inside the 0..box square. */
inline bool inBox(const Path& p,float box=100.0f) noexcept {
    const auto in=[box](float v){return v>=0.0f && v<=box;};
    for(const auto& s:p.segments) {
        if(s.op==Op::Ellipse){if(!in(s.x-s.x1) || !in(s.x+s.x1) || !in(s.y-s.y1) || !in(s.y+s.y1))return false;continue;}
        if(s.op==Op::Rect){if(!in(s.x) || !in(s.y) || !in(s.x+s.x1) || !in(s.y+s.y1))return false;continue;}
        if(!in(s.x) || !in(s.y))return false;
        if((s.op==Op::Quad || s.op==Op::Cubic) && (!in(s.x1) || !in(s.y1)))return false;
        if(s.op==Op::Cubic && (!in(s.x2) || !in(s.y2)))return false;
    }
    return true;
}

// ======== the layered icon line (SPEC 3.5)
struct Layer {char type=0;float width=0;Path path;}; // type F S B K; width for S / K
struct Icon {std::vector<std::vector<Layer>> groups;bool ok=false;};
constexpr float Box=24.0f;
/** One icon's text after the tab: `group||group`, group `layer;layer`, layer `TYPE:path`. */
inline Icon parseIcon(std::string_view text) {
    Icon icon;
    std::size_t g0=0;
    while(g0<=text.size()) {
        const std::size_t g1=std::min(text.find("||",g0),text.size());
        std::vector<Layer> group;
        std::size_t l0=g0;
        while(l0<g1) {
            const std::size_t l1=std::min(text.find(';',l0),g1);
            const std::string_view layer=text.substr(l0,l1-l0);
            const std::size_t colon=layer.find(':');
            if(colon==std::string_view::npos || colon==0)return {};
            Layer l;l.type=layer[0];
            if(l.type!='F' && l.type!='S' && l.type!='B' && l.type!='K')return {};
            if(colon>1){l.width=std::strtof(std::string(layer.substr(1,colon-1)).c_str(),nullptr);if(!(l.width>0))return {};}
            else if(l.type=='S' || l.type=='K')return {}; // (a stroke needs its width)
            l.path=parse(layer.substr(colon+1));
            if(!l.path.ok)return {};
            group.push_back(std::move(l));
            if(l1+1==g1)return {}; // (an empty layer after a trailing ';')
            l0=l1+1;
        }
        if(group.empty())return {};
        icon.groups.push_back(std::move(group));
        if(g1==text.size())break;
        g0=g1+2;
        if(g0>=text.size())return {}; // (an empty group after a trailing '||')
    }
    icon.ok=!icon.groups.empty();
    return icon;
}
/** Whether every layer of an icon lies inside the 24-unit box (strokes: their path; the outline may reach past it when drawn). */
inline bool inBox(const Icon& icon) noexcept {
    for(const auto& g:icon.groups)for(const auto& l:g)if(!inBox(l.path,Box))return false;
    return icon.ok;
}
}
