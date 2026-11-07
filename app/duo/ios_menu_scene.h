// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// menu-redesign on the iPad (2026-10-25; owner, via main: "The direction looks right; use the designer's recommended picks for the three
// choices and implement it with Nunito Sans."; local-analysis/menu-redesign/DESIGN.md, IOS-HANDOFF.md): the C++ port of android-qol's
// Scene.java (one drawn menu frame as plain shapes and texts on a 1920 x 1080 panel, with its touch rectangles; text fitting and
// wrapping) and MenuLayout.java (every colour, size and place of the game's pause-menu frame). Line for line; no UIKit. The iPad's
// painter is ios_qol_touch_view.h (PortMenuPainter); the host tests are tests_menu_scene.cpp and tests_side_menu.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <array>
#include <vector>

namespace recomp::apple::menu {
// ================================================================================================ Scene
namespace scene {
// BACKDROP: what lies behind the menu (the blurred game under the shade, or a colour run color -> color2); FILL: one colour over the
// window; ROUND: a rectangle with corners of radius stroke; LINE: an open line through points, stroke thick; RING: a circle's outline.
enum Type : int {RECT=0,DISC=1,POLY=2,TEXT=3,BACKDROP=4,FILL=5,ROUND=6,LINE=7,RING=8};
enum Align : int {LEFT=0,CENTER=1,RIGHT=2};
enum Layer : int {CHROME=0,CONTENT=1,ALERT=2};
// Nunito Sans: REGULAR, TITLE, CAPS at weight 400; BOLD 700 (the button marks' letters); HEAVY 900 (the alert's "alert").
enum Face : int {REGULAR=0,BOLD=1,TITLE=2,CAPS=3,HEAVY=4};
}
// Width of a text in panel pixels at a size in panel pixels, in one of the faces.
using Measure=std::function<float(const std::string&,int,int)>;

struct Item {
    int type=0,layer=0;std::uint32_t color=0,color2=0;int size=0,align=0,face=0;
    float x=0,y=0,w=0,h=0,stroke=0;
    std::string text;std::vector<float> points;
    bool shadow=false; // (a text on the backdrop itself: MenuLayout's soft shadow under it)
};
struct Hit {float x,y,w,h;int id;};

struct Scene {
    std::vector<Item> items;std::vector<Hit> hits;
    std::string contentKey; // (another page: the painter fades the content in)
    int panelW=1920,panelH=1080; // (the panel this scene was made for: its frame's w x h; the view centres it)
    int layer_=scene::CHROME;
    void layer(int l) {layer_=l;}
    Item& add(int type,float x,float y,float w,float h,std::uint32_t color) {
        Item i;i.type=type;i.layer=layer_;i.x=x;i.y=y;i.w=w;i.h=h;i.color=color;items.push_back(std::move(i));return items.back();
    }
    void rect(float x,float y,float w,float h,std::uint32_t c) {add(scene::RECT,x,y,w,h,c);}
    void round(float x,float y,float w,float h,float radius,std::uint32_t c) {add(scene::ROUND,x,y,w,h,c).stroke=radius;}
    void backdrop(float W,float H,std::uint32_t top,std::uint32_t bottom) {add(scene::BACKDROP,0,0,W,H,top).color2=bottom;}
    void fill(float W,float H,std::uint32_t c) {add(scene::FILL,0,0,W,H,c);}
    void disc(float cx,float cy,float r,std::uint32_t c) {add(scene::DISC,cx-r,cy-r,2*r,2*r,c);}
    void poly(std::uint32_t c,std::vector<float> pts) {add(scene::POLY,0,0,0,0,c).points=std::move(pts);}
    void line(std::uint32_t c,float thickness,std::vector<float> pts) {auto& i=add(scene::LINE,0,0,0,0,c);i.points=std::move(pts);i.stroke=thickness;}
    void ring(float cx,float cy,float r,float thickness,std::uint32_t c) {add(scene::RING,cx-r,cy-r,2*r,2*r,c).stroke=thickness;}
    void text(const std::string& t,float x,float y,float w,float h,int size,int face,int align,std::uint32_t c,bool shadow=false) {
        if(t.empty())return;
        auto& i=add(scene::TEXT,x,y,w,h,c);i.text=t;i.size=size;i.face=face;i.align=align;i.shadow=shadow;
    }
    void hit(float x,float y,float w,float h,int id) {hits.push_back({x,y,w,h,id});}
    // The id of the touch rectangle at a point, the one added last when several overlap; -1 when there is none.
    int hitAt(float x,float y) const {
        for(auto i=hits.size();i-->0;){const auto& h=hits[i];if(x>=h.x && x<h.x+h.w && y>=h.y && y<h.y+h.h)return h.id;}
        return -1;
    }
};

// ---- text fitting (UTF-8: cuts only at code point starts)
inline const std::string Ellipsis="\xE2\x80\xA6"; // "…"
inline std::size_t prevCodePoint(const std::string& s,std::size_t end) {
    if(!end)return 0;
    std::size_t i=end-1;while(i>0 && (static_cast<unsigned char>(s[i])&0xC0)==0x80)--i;
    return i;
}
inline std::string trim(std::string_view s) {
    std::size_t a=0,b=s.size();while(a<b && s[a]==' ')++a;while(b>a && s[b-1]==' ')--b;
    return std::string(s.substr(a,b-a));
}
// "Frame-rate counter" -> "Frame-Rate Counter": every word starts with a capital, also after a hyphen.
inline std::string titleCase(std::string_view text) {
    std::string out;bool start=true;
    for(char c:text){out+=start && c>='a' && c<='z'?char(c-'a'+'A'):c;start=c==' ' || c=='-';}
    return out;
}
inline std::string upper(std::string_view text) {std::string o(text);for(auto& c:o)if(c>='a' && c<='z')c=char(c-'a'+'A');return o;}
// The text itself when it fits into maxWidth, else its longest beginning that fits with an ellipsis at the end.
inline std::string fit(const std::string& text,int size,int face,float maxWidth,const Measure& measure) {
    if(measure(text,size,face)<=maxWidth)return text;
    std::size_t end=text.size();
    while(end>0 && measure(trim(std::string_view(text).substr(0,end))+Ellipsis,size,face)>maxWidth)end=prevCodePoint(text,end);
    return trim(std::string_view(text).substr(0,end))+Ellipsis;
}
// Breaks a text into lines no wider than maxWidth (Scene.wrap): a line break always starts a new line; otherwise at spaces; a word wider
// than a line is cut; a paragraph's leading spaces are kept.
inline std::vector<std::string> wrap(const std::string& text,int size,int face,float maxWidth,const Measure& measure) {
    std::vector<std::string> lines;
    std::size_t p0=0;
    while(true) {
        const std::size_t nl=text.find('\n',p0);
        const std::string paragraph=text.substr(p0,nl==std::string::npos?std::string::npos:nl-p0);
        std::string line;std::size_t at=0;while(at<paragraph.size() && paragraph[at]==' ')++at;
        const std::string indent=paragraph.substr(0,at);
        std::vector<std::string> words;
        {std::size_t i=at;while(i<=paragraph.size()){const std::size_t sp=paragraph.find(' ',i);const auto end=sp==std::string::npos?paragraph.size():sp;
            words.push_back(paragraph.substr(i,end-i));if(sp==std::string::npos)break;i=sp+1;while(i<paragraph.size() && paragraph[i]==' ')++i;}}
        while(words.size()>1 && words.back().empty())words.pop_back(); // (Java's split drops trailing empty words)
        for(std::string word:words) {
            std::string start=line.empty()?indent:line+" ";
            if(!line.empty() && measure(start+word,size,face)>maxWidth){lines.push_back(line);line.clear();start=indent;}
            while(measure(start+word,size,face)>maxWidth && word.size()>1) {
                std::size_t cut=prevCodePoint(word,word.size());
                while(cut>1 && measure(start+word.substr(0,cut),size,face)>maxWidth)cut=prevCodePoint(word,cut);
                if(!cut)break;
                lines.push_back(start+word.substr(0,cut));word=word.substr(cut);start=indent;
            }
            line=start+word;
        }
        lines.push_back(trim(line).empty()?std::string():line);
        if(nl==std::string::npos)break;
        p0=nl+1;
    }
    return lines;
}

// ================================================================================================ MenuLayout
namespace layout {
constexpr int W=1920,H=1080;
constexpr float K=1.4f; // (panel pixels per game pixel)
constexpr int BLOCK_W=1824,LEFT=(W-BLOCK_W)/2,RIGHT=LEFT+BLOCK_W,COLUMN_W=605,PANE_GAP=4;
// colours (ARGB)
constexpr std::uint32_t BACKDROP_TOP=0xFF2B2F2C,BACKDROP_BOTTOM=0xFF141614,BACKDROP_SHADE=0x94000000;
constexpr std::uint32_t PANEL=0xBA000000,PLATE=0x38000000,PROMPT_STRIP=0x99000000;
constexpr std::uint32_t SELECT=0xFFEFF1EE,SELECT_TEXT=0xFF000000,SELECT_TEXT_OFF=0xFF7D7F7C;
constexpr std::uint32_t TEXT=0xFFF0F0F0,TEXT_OFF=0xFF858785,TEXT_DIM=0xFFB4B6B3,ARROW_DIM=0x2BF0F0F0;
constexpr std::uint32_t ACCENT=0xFFACECA9,ACCENT_FRANKLIN=ACCENT,ACCENT_MICHAEL=0xFF81B7D2,ACCENT_TREVOR=0xFFF0A75E;
constexpr std::uint32_t TRACK=0x4DACECA9,TRACK_ON_SELECT=0xE6000000,TRACK_OFF=0x4D858785,MARK_ON_SELECT=0xFF4E9E4B;
constexpr std::uint32_t WARNING=0xFFF0C850,ERROR=0xFFFF7A70,ALERT_YELLOW=0xFFF0C84B;
constexpr std::uint32_t MARK_A=0xFF5FB400,MARK_A_INK=0xFFD8F5B2,MARK_B=0xFFAC0400,MARK_B_INK=0xFFF9C6C2,MARK_X=0xFF0866C9,MARK_X_INK=0xFFC6DDF7,
                        MARK_Y=0xFFD1A600,MARK_Y_INK=0xFFFAF0C0,MARK_START=0xFFEFF1EE,MARK_START_INK=0xFF101010;
constexpr std::uint32_t ALERT_BLACK=0xFF000000,ALERT_CELL=0xFF242624,DIM_OVER_HUD=0xB3000000;
constexpr std::uint32_t track(std::uint32_t accent) {return 0x4D000000u|(accent&0xFFFFFFu);}
// text sizes (Nunito Sans: capital height 0.705 of the size)
constexpr float CAP=0.705f;
constexpr int TITLE_PX=50,CAPS_PX=24,TAB_PX=28,ROW_PX=28,HELP_PX=26,PROMPT_PX=26,MARK_PX=21,START_PX=13,ALERT_TITLE_PX=100,ALERT_PX=31,
              RESTART_PX=20,DIAGRAM_PX=24;
constexpr std::uint32_t SHADOW=0xD9000000;
constexpr float shadowOffset(int sizePx) {return float(sizePx)*CAP*0.08f;}
constexpr float shadowBlur(int sizePx) {return float(sizePx)*CAP*0.18f;}
// header
constexpr int PLATE_TOP=84,PLATE_H=134,TITLE_LEFT=LEFT+22,TITLE_TOP=PLATE_TOP,TITLE_H=PLATE_H;
constexpr int STATUS_TOP=106,STATUS_LINE_H=29,STATUS_LINES=3,STATUS_W=640,STATUS_RIGHT=RIGHT-112;
constexpr float STATUS_PITCH=21*K;
constexpr int PORTRAIT_W=77,PORTRAIT_H=76,PORTRAIT_TOP=113,PORTRAIT_LEFT=RIGHT-18-PORTRAIT_W;
// tab strip
constexpr int STRIP_TOP=220,STRIP_H=54,TAB_GAP=4,TAB_LINE=10;
constexpr int ARROW_OUT=17,ARROW_W=18,ARROW_H=27,ARROW_RISE=1,ARROW_TOUCH=48;
constexpr float ARROW_T=5.7f,CHEVRON_T=4.27f,SCROLL_T=4.72f;
enum Point : int {POINT_LEFT=0,POINT_RIGHT=1,POINT_UP=2,POINT_DOWN=3};
// body
constexpr int BODY_TOP=302,ROW_H=52,ROW_GAP=5,ROW_PITCH=ROW_H+ROW_GAP,BODY_SLOTS=12,BODY_BOTTOM=BODY_TOP+BODY_SLOTS*ROW_PITCH-ROW_GAP;
constexpr int ROW_PAD=14,VALUE_PAD=17,MARK_W=6,BAR_W=262,BAR_H=11,BAR_GAP=22,BAR_FRAME=3;
constexpr int CHEVRON_W=12,CHEVRON_H=19,CHEVRON_GAP=10;
constexpr int SCROLL_W=24,SCROLL_H=16,SCROLL_GAP=4;
constexpr int INFO_LINE=34,INFO_PAD=5,HELP_LINE=34,HELP_LINES=4,TEXT_LINE=42,TEXT_PAD=14;
constexpr int DIAGRAM_SLOTS=8;
// prompts
constexpr int PROMPT_H=52,PROMPT_BOTTOM=1042,PROMPT_TOP=PROMPT_BOTTOM-PROMPT_H,PROMPT_RIGHT=RIGHT,PROMPT_PAD=18,PROMPT_END=18,PROMPT_GAP=18,
              PROMPT_SPACE=39,MARK_D=29,START_W=55,START_H=24,PROMPT_TOUCH_UP=21;
// A touch screen without a gamepad (owner 2026-10-25: "only on touch screen devices, and only when a gamepad isn't connected") shows the
// X and check buttons (TOUCH_BUTTON) in place of the strip, the check at the strip's right edge; their bottom ICON_DROP below the strip's
// (24 px above a 1080 screen's bottom), so that they stay under the body (BODY_BOTTOM 981). The Quick Save button's style: a square, black
// at 78 %, round corners, a white glyph stroked 0.15 of the side (MenuLayout.java TOUCH_BUTTON*).
constexpr int ICON_DROP=14,TOUCH_BUTTON=69,TOUCH_BUTTON_GAP=12,TOUCH_BUTTON_RADIUS=8,TOUCH_BUTTON_PAD=6;
constexpr std::uint32_t TOUCH_BUTTON_FILL=0xC7000000u,TOUCH_BUTTON_GLYPH=0xFFFFFFFFu;constexpr float TOUCH_GLYPH_STROKE=0.15f;
// alert
constexpr int ALERT_W=1100,ALERT_RULE=3,ALERT_RULE_W=150,ALERT_TITLE_H=110,ALERT_LINE=44,ALERT_LINES=10,ALERT_SPACE=26,CHOICE_W=300,CHOICE_H=52,
              CHOICE_TOUCH=20;
constexpr int FADE_MS=120;constexpr float FADE_FROM=0.35f;
// touch rectangles: base + index (or + the NavInput command for a prompt)
constexpr int HIT_TAB=100,HIT_COLUMN=200,HIT_ROW=300,HIT_ARROW_LEFT=400,HIT_ARROW_RIGHT=500,HIT_PROMPT=600,HIT_ALERT_BUTTON=700,HIT_TEXT=800;
// NavInput's commands (the prompts' hit ids)
namespace nav {constexpr int NONE=0,UP=1,DOWN=2,LEFTWARD=3,RIGHTWARD=4,CONFIRM=5,BACK=6,PAGE_PREV=7,PAGE_NEXT=8,MENU=9,ALT=10;}

constexpr int slotsHeight(int slots) {return slots*ROW_PITCH-ROW_GAP;}
constexpr int helpHeight(int lines) {return lines<=0?0:ROW_H+(lines-1)*HELP_LINE;}
constexpr int infoSlots(int lines) {return lines<=0?0:std::max(2,(lines*INFO_LINE+2*INFO_PAD+ROW_GAP+ROW_PITCH-1)/ROW_PITCH);}
constexpr int textSlots(int lines) {return std::max(1,(lines*TEXT_LINE+2*INFO_PAD+ROW_GAP+ROW_PITCH-1)/ROW_PITCH);}
constexpr int clampScroll(int scroll,int total,int visible) {return std::max(0,std::min(scroll,total-visible));}
// The first row to draw so that the focused row is on screen: the list moves only as far as needed, never past its ends.
constexpr int windowStart(int first,int focused,int count,int visible) {
    if(focused>=0 && focused<first)first=focused;
    if(focused>=first+visible)first=focused-visible+1;
    return std::max(0,std::min(first,count-visible));
}
inline int barFill(float fraction,int width=BAR_W) {if(!(fraction>0.0f))return 0;return int(std::lround(float(width)*std::min(1.0f,fraction)));}
constexpr int barTop(int top,int height) {return top+(height-BAR_H)/2;}
inline float fadeAlpha(long long elapsedMs) {
    if(elapsedMs>=FADE_MS)return 1.0f;if(elapsedMs<=0)return FADE_FROM;
    return FADE_FROM+(1.0f-FADE_FROM)*float(elapsedMs)/float(FADE_MS);
}

struct Frame {
    int w,h,left,right,blockW,stripTop,bodyTop,bodySlots,bodyBottom,promptRight,promptTop;
    int iconTop; // (top of the X and check buttons: promptTop in a frame of touch buttons, else ICON_DROP under the strip's bottom)
    bool header;
    bool touchButtons; // (the bottom right holds the two touch buttons instead of the strip: the Thor's lower screen)
    Frame(int w_,int h_,int left_,int blockW_,bool header_,int stripTop_,int bodyTop_,int bodySlots_,int promptRight_,int promptBottom,bool touchButtons_=false)
        :w(w_),h(h_),left(left_),right(left_+blockW_),blockW(blockW_),stripTop(stripTop_),bodyTop(bodyTop_),bodySlots(bodySlots_),
         bodyBottom(bodyTop_+bodySlots_*ROW_PITCH-ROW_GAP),promptRight(promptRight_),promptTop(promptBottom-(touchButtons_?TOUCH_BUTTON:PROMPT_H)),
         iconTop(touchButtons_?promptBottom-TOUCH_BUTTON:promptBottom+ICON_DROP-TOUCH_BUTTON),header(header_),touchButtons(touchButtons_) {}
    // The launcher's frame on a screen taller than 16:9 (owner 2026-10-25, via main: the Fold's launcher full screen): DEFAULT's width,
    // header, strip and body top; the prompts (or the icons) at the screen's bottom, as far above it as on a 1080 screen; as many body
    // slots as fit between. h <= H: DEFAULT itself (MenuLayout.Frame.screen).
    static Frame screen(int h_);
    // The touch buttons' boxes {x0, y0, x1, y1}: [0] the X (back, close), [1] the check (select) at the right end.
    std::array<std::array<float,4>,2> touchButtonBoxes() const {
        const float y0=float(iconTop),y1=float(iconTop+TOUCH_BUTTON),checkX1=float(promptRight),checkX0=checkX1-TOUCH_BUTTON;
        const float xX1=checkX0-TOUCH_BUTTON_GAP,xX0=xX1-TOUCH_BUTTON;
        return {{{xX0,y0,xX1,y1},{checkX0,y0,checkX1,y1}}};
    }
    // A panel of w x h without the header (the Thor's lower-screen trainer).
    static Frame compact(int w,int h) {
        const int margin=22,stripTop=margin+TAB_LINE,bodyTop=stripTop+STRIP_H+(BODY_TOP-STRIP_TOP-STRIP_H);
        const int promptBottom=h-14,slots=std::max(1,(promptBottom-PROMPT_H-8-bodyTop+ROW_GAP)/ROW_PITCH);
        return Frame(w,h,margin,w-2*margin,false,stripTop,bodyTop,slots,w-margin,promptBottom);
    }
    int tabLeft(int index,int count) const {return left+index*(blockW+TAB_GAP)/count;}
    int tabWidth(int index,int count) const {return tabLeft(index+1,count)-TAB_GAP-tabLeft(index,count);}
    int paneLeft(bool column) const {return column?left+COLUMN_W+PANE_GAP:left;}
    int paneWidth(bool column) const {return right-paneLeft(column);}
    int slotTop(int slot) const {return bodyTop+slot*ROW_PITCH;}
    int helpLinesShown(int infoSlots_,int helpLines) const {
        int lines=helpLines;while(lines>0 && slotTop(infoSlots_+2)+helpHeight(lines)>bodyBottom)lines--;return lines;
    }
    int visibleRows(int count,int infoSlots_,int helpLines) const {
        helpLines=helpLinesShown(infoSlots_,helpLines);
        const int room=bodyBottom-slotTop(infoSlots_)-(helpLines>0?helpHeight(helpLines)+ROW_GAP:0);
        const int fit=(room+ROW_GAP)/ROW_PITCH;
        if(count<=fit)return count;
        return std::max(1,fit-1);
    }
    int textLinesVisible() const {return (slotsHeight(bodySlots-1)-2*TEXT_PAD)/TEXT_LINE;}
};
inline const Frame DEFAULT_FRAME(W,H,LEFT,BLOCK_W,true,STRIP_TOP,BODY_TOP,BODY_SLOTS,PROMPT_RIGHT,PROMPT_BOTTOM);
inline Frame Frame::screen(int h_) {
    if(h_<=H)return DEFAULT_FRAME;
    const int promptBottom=h_-(H-PROMPT_BOTTOM);
    const int slots=(promptBottom+ICON_DROP-TOUCH_BUTTON-TOUCH_BUTTON_PAD-BODY_TOP+ROW_GAP)/ROW_PITCH;
    return Frame(W,h_,LEFT,BLOCK_W,true,STRIP_TOP,BODY_TOP,slots,PROMPT_RIGHT,promptBottom);
}
// The panel's height in a view of viewW x viewH shown at u device px per panel px: H, or more in a landscape view taller than 16:9
// (its frame: Frame::screen; MenuLayout.panelHeight).
inline int panelHeight(float viewW,float viewH,float u) {
#if defined(RECOMP_DUO_EDITION)
    (void)viewW;if(u<=0.0f)return H; // ios-duo (menu-redesign's addendum: "for the Duo apply it to the portrait screen too"): any view taller than 16:9
#else
    if(u<=0.0f || viewW<viewH)return H;
#endif
    return std::max(H,int(std::lround(viewH/u)));
}
// Device pixels per panel pixel that make a row pitch 4.4 mm on a screen of xdpi pixels per inch (not above 3).
inline float touchUnit(float xdpi) {
    if(!(xdpi>0.0f))return 1.0f;
    const float mmPerPx=25.4f/xdpi,k=4.4f/(mmPerPx*40.5f);
    return std::min(3.0f,std::max(0.5f,k/K));
}
// The angle between an arm of an arrow head and its direction for which a head of thickness t with square-cut ends fills the box.
inline double armAngle(float along,float across,float t) {
    double low=0,high=M_PI/2;
    for(int i=0;i<40;++i){const double mid=(low+high)/2,s=std::sin(mid);if(along*s-t*s*s-across/2*std::cos(mid)<0)low=mid;else high=mid;}
    return (low+high)/2;
}
// An arrow head as the game draws it: one bent line of thickness t, ends cut square, filling the box exactly. Six corners, tip first.
inline std::vector<float> chevron(float x,float y,float w,float h,float t,int direction) {
    const bool sideways=direction==POINT_LEFT || direction==POINT_RIGHT;
    const float along=sideways?w:h,across=sideways?h:w,half=across/2.0f;
    const double angle=armAngle(along,across,t),s=std::sin(angle),c=std::cos(angle);
    const float cut=float(t*s),drop=float(t*c),notch=float(t/s);
    const float shape[]{along,half,cut,0,0,drop,along-notch,half,0,across-drop,cut,across};
    std::vector<float> corners(12);
    for(int i=0;i<12;i+=2) {
        const float ahead=shape[i],side=shape[i+1];
        switch(direction) {
        case POINT_RIGHT:corners[i]=x+ahead;corners[i+1]=y+side;break;
        case POINT_LEFT:corners[i]=x+w-ahead;corners[i+1]=y+side;break;
        case POINT_DOWN:corners[i]=x+side;corners[i+1]=y+ahead;break;
        default:corners[i]=x+side;corners[i+1]=y+h-ahead;break; // (up)
        }
    }
    return corners;
}
inline int choiceLeft(int index,int count) {const int line=count*CHOICE_W+(count-1)*TAB_GAP;return (W-line)/2+index*(CHOICE_W+TAB_GAP);}
inline int alertHeight(int titleLines,int detailLines) {
    const int lines=titleLines+std::min(detailLines,ALERT_LINES);
    return ALERT_TITLE_H+ALERT_SPACE+ALERT_RULE+ALERT_SPACE+lines*ALERT_LINE+ALERT_SPACE+ALERT_RULE+2*ALERT_SPACE+CHOICE_H;
}
inline int alertTop(int titleLines,int detailLines) {return (PROMPT_TOP-alertHeight(titleLines,detailLines))/2;}
// The owner's alert (layout A), measured: the heading's ink from y 468, the rules at y 583 and 659, the message at 633.
constexpr int RESTART_ALERT_TITLE_TOP=456,RESTART_ALERT_RULE1=583,RESTART_ALERT_RULE2=659,RESTART_ALERT_TEXT_TOP=599;
}
}
