// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// menu-redesign on the iPad (2026-10-25): the C++ port of android-qol's MenuScenes.java - one menu frame (the game's pause menu: the header
// plate, the tab strip, the column, the pane with its info block, rows, "more" row and help strip, or a text; the prompts; the alerts)
// built into a Scene from a plain description (Page). Line for line; no UIKit. DESIGN.md (local-analysis/menu-redesign) is the spec.
#include "ios_menu_scene.h"

namespace recomp::apple::menu {
namespace scenes {
using namespace layout;
constexpr int TONE_NORMAL=0,TONE_WARNING=1,TONE_ERROR=2,TONE_GOOD=3;
constexpr int MARK_NONE=0,MARK_CHANGED=1,MARK_WARNING=2;
inline const std::string ALERT_HEADING="alert",DIAGRAM_HEADING="ON FOOT",RESTART="RESTART";

struct Line {std::string text;int tone=TONE_NORMAL;};
struct Row {
    std::string label;std::string value;bool hasValue=false;
    bool arrows=false,canLeft=false,canRight=false,halves=false,enabled=true,marked=false,restart=false;
    float fraction=-1.0f;int valueTone=TONE_NORMAL;
};
struct Prompt {std::string label,glyph;int command=0;};
struct ConfirmOverlay {
    std::string title,detail,heading;std::vector<std::string> buttons;int focused=0,scroll=0;
};
struct Page {
    std::string title;bool header=true,portrait=true;std::uint32_t accent=ACCENT;bool diagram=false,backdrop=true;
    std::vector<Line> status;std::vector<std::string> tabs;int tab=0;bool shoulders=true;
    bool hasColumn=false;std::vector<std::string> column;std::vector<bool> columnEnabled;std::vector<int> columnMarks;int columnSelected=0;bool columnLit=false;
    std::vector<Line> info;float infoBar=-1.0f;
    std::vector<Row> rows;int rowFocused=-1,rowFirst=0;
    std::vector<Line> help;
    bool hasText=false;std::vector<std::string> text;int textScroll=0;bool textFocused=false;
    std::vector<Prompt> prompts;bool hasFooter=false;Line footerStatus;
    bool touchIcons=false; // (the prompts as the X and check icons: a touch screen without a gamepad, owner 2026-10-25; with one the strip)
    bool hasAlert=false;ConfirmOverlay alert;
};

inline std::uint32_t color(int tone,std::uint32_t normal) {
    switch(tone){case TONE_WARNING:return WARNING;case TONE_ERROR:return ERROR;case TONE_GOOD:return ACCENT_FRANKLIN;default:return normal;}
}
inline void chevronShape(Scene& s,float x,float y,float w,float h,float t,int direction,std::uint32_t c) {s.poly(c,chevron(x,y,w,h,t,direction));}
inline void wrapInto(std::vector<Line>& out,const std::vector<Line>& parts,int size,float width,const Measure& m) {
    for(const auto& part:parts)for(auto& l:wrap(part.text,size,scene::REGULAR,width,m))out.push_back({l,part.tone});
}
// Wraps a text for the pane's text view (MenuScenes.paneText).
inline std::vector<std::string> paneText(const std::string& text,bool column,const Measure& m) {
    return wrap(text,ROW_PX,scene::REGULAR,float(DEFAULT_FRAME.paneWidth(column)-2*ROW_PAD),m);
}
inline float markWidth(const std::string& glyph) {return glyph.size()==1?float(MARK_D):float(START_W);}
// A button mark centred at (cx, cy), d across: our own disc in the button's colour with its letter (A, B, X, Y).
inline void mark(Scene& s,const std::string& glyph,float cx,float cy,float d) {
    const bool a=glyph=="A",b=glyph=="B",x=glyph=="X";
    s.disc(cx,cy,d/2.0f,a?MARK_A:b?MARK_B:x?MARK_X:MARK_Y);
    const int size=int(std::lround(d*float(MARK_PX)/float(MARK_D)));
    s.text(glyph,cx-d/2.0f,cy-d/2.0f,d,d,size,scene::BOLD,scene::CENTER,a?MARK_A_INK:b?MARK_B_INK:x?MARK_X_INK:MARK_Y_INK);
}
// The portrait box: black, with a head (a disc) and shoulders (a rounded body cut by the box's lower edge), white.
inline void portrait(Scene& s,float x,float y,float w,float h) {
    s.rect(x,y,w,h,0xD9000000);
    s.disc(x+w/2.0f,y+w*0.36f,w*0.19f,TEXT);
    const float bodyTop=y+w*0.60f;
    s.round(x+w*0.17f,bodyTop,w*0.66f,y+h-bodyTop,w*0.25f,TEXT);
    s.rect(x+w*0.17f,y+h-w*0.12f,w*0.66f,w*0.12f,TEXT);
}
inline void header(Scene& s,const Page& page,const Measure& m) {
    s.rect(LEFT,PLATE_TOP,BLOCK_W,PLATE_H,PLATE);
    const int statusRight=page.portrait?STATUS_RIGHT:RIGHT-ROW_PAD-8;
    const float titleWidth=float(statusRight-STATUS_W-TITLE_LEFT);
    s.text(fit(page.title,TITLE_PX,scene::TITLE,titleWidth,m),TITLE_LEFT,TITLE_TOP,titleWidth,TITLE_H,TITLE_PX,scene::TITLE,scene::LEFT,TEXT,true);
    const int count=std::min(int(page.status.size()),STATUS_LINES);
    float top=float(STATUS_TOP)+float(STATUS_LINES-count)*STATUS_PITCH;
    for(int i=0;i<count;++i,top+=STATUS_PITCH) {
        const auto& line=page.status[std::size_t(i)];
        s.text(fit(upper(line.text),CAPS_PX,scene::CAPS,STATUS_W,m),float(statusRight-STATUS_W),top,STATUS_W,STATUS_LINE_H,CAPS_PX,scene::CAPS,
               scene::RIGHT,color(line.tone,TEXT),true);
    }
    if(page.portrait)portrait(s,PORTRAIT_LEFT,PORTRAIT_TOP,PORTRAIT_W,PORTRAIT_H);
}
inline void strip(Scene& s,const Page& page,const Measure& m,const Frame& f) {
    const int count=int(page.tabs.size()),stripTop=f.stripTop;
    const int touchTop=stripTop-TAB_LINE-ROW_PAD,touchHeight=f.bodyTop-ROW_PAD-touchTop;
    for(int i=0;i<count;++i) {
        const int left=f.tabLeft(i,count),width=f.tabWidth(i,count);const bool current=i==page.tab;
        s.rect(left,stripTop,width,STRIP_H,current?SELECT:PANEL);
        if(current)s.rect(left,stripTop-TAB_LINE,width,TAB_LINE,page.accent);
        s.text(fit(upper(page.tabs[std::size_t(i)]),TAB_PX,scene::REGULAR,float(width-2*ROW_PAD),m),left,stripTop,width,STRIP_H,TAB_PX,
               scene::REGULAR,scene::CENTER,current?SELECT_TEXT:TEXT);
        if(page.shoulders || count>1)s.hit(left,touchTop,width,touchHeight,HIT_TAB+i);
    }
    if(!page.shoulders || f.left<ARROW_OUT+ARROW_W)return;
    const int arrowTop=stripTop+(STRIP_H-ARROW_H)/2-ARROW_RISE;
    chevronShape(s,f.left-ARROW_OUT-ARROW_W,arrowTop,ARROW_W,ARROW_H,ARROW_T,POINT_LEFT,page.tab>0?TEXT:ARROW_DIM);
    chevronShape(s,f.right+ARROW_OUT,arrowTop,ARROW_W,ARROW_H,ARROW_T,POINT_RIGHT,page.tab<count-1?TEXT:ARROW_DIM);
    s.hit(f.left-ARROW_TOUCH,touchTop,ARROW_TOUCH,touchHeight,HIT_PROMPT+nav::PAGE_PREV);
    s.hit(f.right,touchTop,ARROW_TOUCH,touchHeight,HIT_PROMPT+nav::PAGE_NEXT);
}
inline void column(Scene& s,const Page& page,const Measure& m,const Frame& f) {
    const int textWidth=COLUMN_W-2*ROW_PAD,L=f.left;
    for(int i=0;i<int(page.column.size()) && i<f.bodySlots;++i) {
        const int top=f.slotTop(i);
        const bool lit=i==page.columnSelected && page.columnLit;
        const bool enabled=page.columnEnabled.empty() || page.columnEnabled[std::size_t(i)];
        s.rect(L,top,COLUMN_W,ROW_H,lit?SELECT:PANEL);
        const int mk=page.columnMarks.empty()?MARK_NONE:page.columnMarks[std::size_t(i)];
        if(mk!=MARK_NONE)s.rect(L,top,MARK_W,ROW_H,mk==MARK_WARNING?WARNING:lit?MARK_ON_SELECT:page.accent);
        s.text(fit(page.column[std::size_t(i)],ROW_PX,scene::REGULAR,textWidth,m),L+ROW_PAD,top,textWidth,ROW_H,ROW_PX,scene::REGULAR,scene::LEFT,
               lit?(enabled?SELECT_TEXT:SELECT_TEXT_OFF):enabled?TEXT:TEXT_OFF);
        s.hit(L,float(top)-ROW_GAP/2.0f,COLUMN_W,ROW_PITCH,HIT_COLUMN+i);
    }
}
// The first row the pane draws (what pane() draws; the in-game menu keeps it between frames).
inline int firstRow(const Page& page,const Measure& m,const Frame& f) {
    const bool col=page.hasColumn;const int width=f.paneWidth(col),textWidth=width-2*ROW_PAD;
    std::vector<Line> info;wrapInto(info,page.info,ROW_PX,float(page.infoBar>=0.0f?textWidth-BAR_W-BAR_GAP:textWidth),m);
    const int slot=infoSlots(int(info.size()))+(page.diagram?DIAGRAM_SLOTS:0);
    std::vector<Line> help;wrapInto(help,page.help,HELP_PX,float(textWidth),m);
    const int count=int(page.rows.size()),visible=f.visibleRows(count,slot,std::min(int(help.size()),HELP_LINES));
    return windowStart(page.rowFirst,page.rowFocused,count,visible);
}
inline void scrollRow(Scene& s,int left,int top,int width,bool lit,bool up,bool down) {
    s.layer(scene::CHROME);
    s.rect(left,top,width,ROW_H,lit?SELECT:PANEL);
    const std::uint32_t ink=lit?SELECT_TEXT:TEXT,faint=lit?0x33000000u:0x40F0F0F0u;
    const int x=left+(width-SCROLL_W)/2,y=top+(ROW_H-2*SCROLL_H-SCROLL_GAP)/2;
    chevronShape(s,x,y,SCROLL_W,SCROLL_H,SCROLL_T,POINT_UP,up?ink:faint);
    chevronShape(s,x,y+SCROLL_H+SCROLL_GAP,SCROLL_W,SCROLL_H,SCROLL_T,POINT_DOWN,down?ink:faint);
}
inline void row(Scene& s,const Row& r,int index,int top,int left,int width,bool focused,std::uint32_t accent,const Measure& m) {
    const std::uint32_t ink=focused?(r.enabled?SELECT_TEXT:SELECT_TEXT_OFF):r.enabled?TEXT:TEXT_OFF;
    s.layer(scene::CHROME);
    if(focused)s.rect(left,top,width,ROW_H,SELECT);
    if(r.marked)s.rect(left,top,MARK_W,ROW_H,focused?MARK_ON_SELECT:accent);
    s.hit(left,float(top)-ROW_GAP/2.0f,width,ROW_PITCH,HIT_ROW+index);
    s.layer(scene::CONTENT);
    float labelLeft=float(left+ROW_PAD),right=float(left+width-VALUE_PAD),labelRight=right;
    const bool slider=r.fraction>=0.0f;
    if(r.hasValue && slider) {
        const int barLeft=left+width-VALUE_PAD-BAR_W,bt=barTop(top,ROW_H);
        if(focused) {
            s.rect(barLeft,bt-BAR_FRAME,BAR_W,BAR_H+2*BAR_FRAME,TRACK_ON_SELECT);
            s.rect(barLeft+BAR_FRAME,bt,barFill(r.fraction,BAR_W-2*BAR_FRAME),BAR_H,r.enabled?accent:TEXT_OFF);
        } else {
            s.rect(barLeft,bt,BAR_W,BAR_H,r.enabled?track(accent):TRACK_OFF);
            s.rect(barLeft,bt,barFill(r.fraction),BAR_H,r.enabled?accent:TEXT_OFF);
        }
        right=float(barLeft-BAR_GAP);
        labelRight=right-2*ROW_PAD;
    }
    if(r.hasValue && (!slider || focused)) {
        const bool arrows=focused && r.arrows && r.enabled;
        const int chevronTop=top+(ROW_H-CHEVRON_H)/2;
        if(arrows) {
            chevronShape(s,right-CHEVRON_W,chevronTop,CHEVRON_W,CHEVRON_H,CHEVRON_T,POINT_RIGHT,r.canRight?SELECT_TEXT:0x33000000u);
            right-=CHEVRON_W+CHEVRON_GAP;
        }
        const float labelWidth=std::min(m(r.label,ROW_PX,scene::REGULAR),float(width)*0.45f);
        const float room=right-(labelLeft+labelWidth+2*ROW_PAD)-(arrows?CHEVRON_W+CHEVRON_GAP:0);
        const std::string value=fit(r.value,ROW_PX,scene::REGULAR,room,m);
        const float valueWidth=m(value,ROW_PX,scene::REGULAR);
        s.text(value,right-valueWidth,top,valueWidth,ROW_H,ROW_PX,scene::REGULAR,scene::RIGHT,focused?ink:color(r.valueTone,ink));
        labelRight=right-valueWidth-2*ROW_PAD;
        if(arrows) {
            chevronShape(s,right-valueWidth-CHEVRON_GAP-CHEVRON_W,chevronTop,CHEVRON_W,CHEVRON_H,CHEVRON_T,POINT_LEFT,r.canLeft?SELECT_TEXT:0x33000000u);
            labelRight-=CHEVRON_W+CHEVRON_GAP;
        }
    }
    if(r.restart && r.hasValue) {
        const float w=m(RESTART,RESTART_PX,scene::REGULAR),x=labelRight+ROW_PAD-w;
        if(x>labelLeft+4*ROW_PAD) {
            s.text(RESTART,x,top,w,ROW_H,RESTART_PX,scene::REGULAR,scene::LEFT,focused?SELECT_TEXT_OFF:TEXT_DIM);
            labelRight=x-2*ROW_PAD;
        }
    }
    s.text(fit(r.label,ROW_PX,scene::REGULAR,labelRight-labelLeft,m),labelLeft,top,labelRight-labelLeft,ROW_H,ROW_PX,scene::REGULAR,scene::LEFT,ink);
    if(focused && r.halves) {
        s.hit(left,float(top)-ROW_GAP/2.0f,float(width)/2.0f,ROW_PITCH,HIT_ARROW_LEFT+index);
        s.hit(float(left)+float(width)/2.0f,float(top)-ROW_GAP/2.0f,float(width)/2.0f,ROW_PITCH,HIT_ARROW_RIGHT+index);
    }
}

// ---- the controller diagram (DESIGN.md 5.7): our own outline of a 360-style pad, the nine on-foot controls at each side
struct Pad {
    static const std::vector<std::vector<float>>& outline() {
        static const std::vector<std::vector<float>> paths=[]{
            std::vector<std::vector<float>> p;
            const float body[10][8]{{-60,-62,-85,-66,-108,-62,-118,-50},{-118,-50,-138,-30,-152,10,-150,40},{-150,40,-150,75,-138,95,-118,92},
                {-118,92,-95,88,-80,58,-62,52},{-62,52,-30,45,30,45,62,52},{62,52,80,58,95,88,118,92},{118,92,138,95,150,75,150,40},
                {150,40,152,10,138,-30,118,-50},{118,-50,108,-62,85,-66,60,-62},{60,-62,30,-55,-30,-55,-60,-62}};
            std::vector<float> o;
            for(const auto& c:body)for(int step=o.empty()?0:1;step<=12;++step) {
                const float u=float(step)/12.0f,v=1-u;
                o.push_back(v*v*v*c[0]+3*v*v*u*c[2]+3*v*u*u*c[4]+u*u*u*c[6]);
                o.push_back(v*v*v*c[1]+3*v*v*u*c[3]+3*v*u*u*c[5]+u*u*u*c[7]);
            }
            p.push_back(o);
            for(int side=-1;side<=1;side+=2) {
                const float x0=side<0?-112.0f:82.0f;
                p.push_back({x0+7,-98,x0+23,-98,x0+30,-91,x0+30,-75,x0+23,-68,x0+7,-68,x0,-75,x0,-91,x0+7,-98});
                p.push_back({float(side*64),-70,float(side*112),-64});
            }
            const float a=7,b=20,dx=-46,dy=30;
            p.push_back({dx-a,dy-b,dx+a,dy-b,dx+a,dy-a,dx+b,dy-a,dx+b,dy+a,dx+a,dy+a,dx+a,dy+b,dx-a,dy+b,dx-a,dy+a,dx-b,dy+a,dx-b,dy-a,dx-a,dy-a,dx-a,dy-b});
            return p;
        }();
        return paths;
    }
    static constexpr float RINGS[7][3]{{-92,-12,22},{-92,-12,13},{48,30,22},{48,30,13},{0,-22,15},{-30,-22,6},{30,-22,6}};
    static constexpr float FACES[8]{92,-36,72,-16,112,-16,92,4};
    static const std::vector<std::string>& labels(int side) {
        static const std::vector<std::string> l[2]{
            {"TARGET LOCK ON","WEAPON WHEEL","SWITCH CAMERA","MOVE","STEALTH MODE","USE PHONE","PHONE LEFT","CHARACTER SWITCH","PHONE RIGHT"},
            {"FIRE WEAPON","TAKE COVER","PAUSE MENU","ENTER VEHICLE","RELOAD","SPRINT","JUMP","LOOK BEHIND","LOOK"}};
        return l[side];
    }
    static constexpr float TARGETS[2][18]{{-97,-98,-88,-67,-30,-28,-114,-12,-92,-12,-46,10,-66,30,-46,50,-26,30},
                                          {97,-98,88,-67,30,-28,92,-45,121,-16,92,13,63,-16,48,30,70,30}};
};
inline std::vector<float> place(const std::vector<float>& g,float cx,float cy) {
    std::vector<float> out(g.size());for(std::size_t i=0;i+1<g.size();i+=2){out[i]=cx+g[i]*K;out[i+1]=cy+g[i+1]*K;}return out;
}
inline void diagram(Scene& s,int left,int top,int width,const Measure& m) {
    s.rect(left,top,width,slotsHeight(DIAGRAM_SLOTS),PANEL);
    s.text(DIAGRAM_HEADING,left,top+8,width,ROW_H-16,ROW_PX,scene::REGULAR,scene::CENTER,TEXT);
    const float cx=float(left)+float(width)/2.0f,cy=float(top)+165*K,t=2.4f*K;
    for(const auto& path:Pad::outline())s.line(TEXT,t,place(path,cx,cy));
    for(const auto& r:Pad::RINGS)s.ring(cx+r[0]*K,cy+r[1]*K,r[2]*K,t,TEXT);
    s.line(TEXT,3*K,place({-8,-30,8,-14},cx,cy));
    s.line(TEXT,3*K,place({8,-30,-8,-14},cx,cy));
    const char* faces[]{"Y","X","B","A"};
    for(int i=0;i<4;++i)mark(s,faces[i],cx+Pad::FACES[2*i]*K,cy+Pad::FACES[2*i+1]*K,19*K);
    const std::uint32_t lead=0x9FF0F0F0;
    for(int side=0;side<2;++side) {
        const float sign=side==0?-1.0f:1.0f;
        const auto& labs=Pad::labels(side);
        for(std::size_t i=0;i<labs.size();++i) {
            const float y=float(top)+(52+30*float(i))*K,labelX=cx+sign*185*K;
            const float w=m(labs[i],DIAGRAM_PX,scene::REGULAR);
            s.text(labs[i],side==0?labelX-w:labelX,y-15,w,30,DIAGRAM_PX,scene::REGULAR,side==0?scene::RIGHT:scene::LEFT,TEXT);
            const float tx=cx+Pad::TARGETS[side][2*i]*K,ty=cy+Pad::TARGETS[side][2*i+1]*K,sx=labelX-sign*7*K;
            if(std::fabs(ty-y)<3*K)s.line(lead,1.4f*K,{sx,y,tx,y});else s.line(lead,1.4f*K,{sx,y,tx,y,tx,ty});
        }
    }
}
inline void pane(Scene& s,const Page& page,bool col,const Measure& m,const Frame& f) {
    const int left=f.paneLeft(col),width=f.paneWidth(col),textWidth=width-2*ROW_PAD,BT=f.bodyTop;
    const bool bar=page.infoBar>=0.0f;
    std::vector<Line> info;wrapInto(info,page.info,ROW_PX,float(bar?textWidth-BAR_W-BAR_GAP:textWidth),m);
    int slot=infoSlots(int(info.size()));
    if(slot>0) {
        const int height=slotsHeight(slot);
        const float top=float(BT)+float(height-int(info.size())*INFO_LINE)/2.0f;
        s.layer(scene::CHROME);s.rect(left,BT,width,height,PANEL);
        s.layer(scene::CONTENT);
        for(std::size_t i=0;i<info.size();++i)
            s.text(info[i].text,left+ROW_PAD,top+float(i)*INFO_LINE,textWidth,INFO_LINE,ROW_PX,scene::REGULAR,scene::LEFT,color(info[i].tone,TEXT));
        if(bar) {
            const int barLeft=left+width-VALUE_PAD-BAR_W,bt=barTop(BT,height);
            s.rect(barLeft,bt,BAR_W,BAR_H,track(page.accent));s.rect(barLeft,bt,barFill(page.infoBar),BAR_H,page.accent);
        }
    }
    if(page.diagram){s.layer(scene::CHROME);diagram(s,left,f.slotTop(slot),width,m);slot+=DIAGRAM_SLOTS;}
    std::vector<Line> help;wrapInto(help,page.help,HELP_PX,float(textWidth),m);
    const int helpLines=f.helpLinesShown(slot,std::min(int(help.size()),HELP_LINES));
    const int count=int(page.rows.size());
    if(count>0) {
        const int visible=f.visibleRows(count,slot,helpLines);
        const int first=windowStart(page.rowFirst,page.rowFocused,count,visible),last=std::min(count,first+visible);
        s.layer(scene::CHROME);
        s.rect(left,f.slotTop(slot),width,slotsHeight(last-first+(visible<count?1:0)),PANEL);
        for(int index=first;index<last;++index)
            row(s,page.rows[std::size_t(index)],index,f.slotTop(slot+index-first),left,width,index==page.rowFocused,page.accent,m);
        slot+=last-first;
        if(visible<count){scrollRow(s,left,f.slotTop(slot),width,false,first>0,last<count);slot++;}
    }
    if(helpLines>0) {
        const int top=f.slotTop(slot);
        s.layer(scene::CHROME);s.rect(left,top,width,helpHeight(helpLines),PANEL);
        s.layer(scene::CONTENT);
        for(int i=0;i<helpLines;++i) {
            std::string line=help[std::size_t(i)].text;
            if(i==helpLines-1 && int(help.size())>helpLines)line=fit(line+" "+Ellipsis+Ellipsis,HELP_PX,scene::REGULAR,float(textWidth),m);
            s.text(line,left+ROW_PAD,float(top)+float(ROW_H-HELP_LINE)/2.0f+float(i*HELP_LINE),textWidth,HELP_LINE,HELP_PX,scene::REGULAR,scene::LEFT,
                   color(help[std::size_t(i)].tone,TEXT));
        }
    }
}
inline void textPane(Scene& s,const Page& page,bool col,const Frame& f) {
    const int left=f.paneLeft(col),width=f.paneWidth(col),total=int(page.text.size()),visible=f.textLinesVisible();
    const bool more=total>visible;
    const int shown=more?visible:total,first=more?clampScroll(page.textScroll,total,visible):0;
    const int height=more?slotsHeight(f.bodySlots-1):slotsHeight(textSlots(shown));
    const float top=more?float(f.bodyTop+TEXT_PAD):float(f.bodyTop)+float(height-shown*TEXT_LINE)/2.0f;
    s.rect(left,f.bodyTop,width,height,PANEL);
    s.layer(scene::CONTENT);
    for(int i=0;i<shown;++i)
        s.text(page.text[std::size_t(first+i)],left+ROW_PAD,top+float(i*TEXT_LINE),width-2*ROW_PAD,TEXT_LINE,ROW_PX,scene::REGULAR,scene::LEFT,TEXT);
    if(more)scrollRow(s,left,f.slotTop(f.bodySlots-1),width,page.textFocused,first>0,first+visible<total);
    s.hit(left,f.bodyTop,width,f.bodyBottom-f.bodyTop,HIT_TEXT);
}
// The prompts: one dark strip ending in the frame's bottom right corner; each its label, then its button mark. Returns the strip's left.
// The two touch buttons in place of the prompt strip (TOUCH_BUTTON): an X (B: back, then close) and a check (A: the highlighted row) at
// the right, each a square in the Quick Save button's style with a white glyph. A frame of touch buttons (the Thor's lower screen) always
// has both; a touch screen without a gamepad (Page.touchIcons) has the X where the page's prompts offer B and the check where they offer
// A, each in its own place (START and Y prompts get none: their rows do the same by touch). Their touch rectangles reach TOUCH_BUTTON_PAD
// beyond the squares. Returns the leftmost drawn square's left edge.
inline bool offers(const Page& page,int command) {for(const auto& p:page.prompts)if(p.command==command)return true;return false;}
inline float touchButtons(Scene& s,const Page& page,const Frame& f) {
    const auto boxes=f.touchButtonBoxes();const int commands[2]={nav::BACK,nav::CONFIRM};
    float leftmost=float(f.promptRight);
    for(int i=0;i<2;++i) {
        if(!f.touchButtons && !offers(page,commands[i]))continue;
        const auto& b=boxes[std::size_t(i)];
        leftmost=std::min(leftmost,b[0]);
        const float side=b[2]-b[0],t=side*TOUCH_GLYPH_STROKE,x=b[0],y=b[1];
        s.round(x,y,side,side,TOUCH_BUTTON_RADIUS,TOUCH_BUTTON_FILL);
        if(i==0) {
            const float g=side*0.29f;
            s.line(TOUCH_BUTTON_GLYPH,t,{x+g,y+g,x+side-g,y+side-g});
            s.line(TOUCH_BUTTON_GLYPH,t,{x+side-g,y+g,x+g,y+side-g});
        } else s.line(TOUCH_BUTTON_GLYPH,t,{x+side*0.22f,y+side*0.53f,x+side*0.42f,y+side*0.72f,x+side*0.79f,y+side*0.30f});
        const float p=TOUCH_BUTTON_PAD;
        s.hit(b[0]-p,b[1]-p,side+2*p,side+2*p,HIT_PROMPT+commands[i]);
    }
    return leftmost;
}
inline float prompts(Scene& s,const Page& page,const Measure& m,const Frame& f) {
    if(f.touchButtons || page.touchIcons)return touchButtons(s,page,f);
    const int count=int(page.prompts.size()),PT=f.promptTop,PW=f.promptRight;
    if(!count)return float(PW);
    float total=float(PROMPT_PAD+PROMPT_END+(count-1)*PROMPT_SPACE);
    for(const auto& p:page.prompts)total+=m(p.label,PROMPT_PX,scene::REGULAR)+PROMPT_GAP+markWidth(p.glyph);
    const float left=float(PW)-total;float x=left+PROMPT_PAD;const float cy=float(PT)+PROMPT_H/2.0f;
    s.rect(left,PT,total,PROMPT_H,PROMPT_STRIP);
    for(const auto& p:page.prompts) {
        const float label=m(p.label,PROMPT_PX,scene::REGULAR),mk=markWidth(p.glyph),markLeft=x+label+PROMPT_GAP;
        s.text(p.label,x,PT,label,PROMPT_H,PROMPT_PX,scene::REGULAR,scene::LEFT,TEXT);
        if(p.glyph.size()==1)mark(s,p.glyph,markLeft+MARK_D/2.0f,cy,MARK_D);
        else {
            s.round(markLeft,cy-START_H/2.0f,mk,START_H,START_H/2.0f,MARK_START);
            s.text(p.glyph,markLeft,PT,mk,PROMPT_H,START_PX,scene::BOLD,scene::CENTER,MARK_START_INK);
        }
        const float hitLeft=std::max(left,x-PROMPT_SPACE/2.0f),hitRight=std::min(float(PW),markLeft+mk+PROMPT_SPACE/2.0f);
        s.hit(hitLeft,float(PT-PROMPT_TOUCH_UP),hitRight-hitLeft,PROMPT_H+PROMPT_TOUCH_UP,HIT_PROMPT+p.command);
        x=markLeft+mk+PROMPT_SPACE;
    }
    return left;
}
inline void status(Scene& s,const Page& page,float promptsLeft,const Measure& m,const Frame& f) {
    if(!page.hasFooter)return;
    const float room=promptsLeft-PROMPT_SPACE-2*PROMPT_PAD-float(f.left);float widest=0;
    const auto lines=wrap(page.footerStatus.text,PROMPT_PX,scene::REGULAR,room,m);
    for(const auto& l:lines)widest=std::max(widest,m(l,PROMPT_PX,scene::REGULAR));
    // (its bottom level with the strip's; with the icons of a touch screen without a gamepad, with theirs)
    const float bottom=float(page.touchIcons && !f.touchButtons?f.iconTop+TOUCH_BUTTON:f.promptTop+PROMPT_H);
    const float height=float(PROMPT_H+(int(lines.size())-1)*HELP_LINE),top=bottom-height;
    s.rect(f.left,top,widest+2*PROMPT_PAD,height,PROMPT_STRIP);
    for(std::size_t i=0;i<lines.size();++i)
        s.text(lines[i],f.left+PROMPT_PAD,top+float(PROMPT_H-HELP_LINE)/2.0f+float(i*HELP_LINE),room,HELP_LINE,PROMPT_PX,scene::REGULAR,scene::LEFT,
               color(page.footerStatus.tone,TEXT));
}
inline std::vector<std::string> alertWrap(const std::string& text,const Measure& m) {return wrap(text,ALERT_PX,scene::REGULAR,ALERT_W-4*SCROLL_W,m);}
inline void alert(Scene& s,const ConfirmOverlay& a,const Measure& m) {
    s.fill(W,H,ALERT_BLACK);
    const auto title=a.title.empty()?std::vector<std::string>{}:alertWrap(a.title,m),detail=a.detail.empty()?std::vector<std::string>{}:alertWrap(a.detail,m);
    const int shown=std::min(int(detail.size()),ALERT_LINES),first=clampScroll(a.scroll,int(detail.size()),shown);
    const int ruleLeft=(W-ALERT_W)/2,shortRule=(W-ALERT_RULE_W)/2;int y=alertTop(int(title.size()),int(detail.size()));
    s.text(a.heading.empty()?ALERT_HEADING:a.heading,0,y,W,ALERT_TITLE_H,ALERT_TITLE_PX,scene::HEAVY,scene::CENTER,ALERT_YELLOW);
    y+=ALERT_TITLE_H+ALERT_SPACE;s.rect(shortRule,y,ALERT_RULE_W,ALERT_RULE,TEXT);y+=ALERT_RULE+ALERT_SPACE;
    for(const auto& l:title){s.text(l,ruleLeft,y,ALERT_W,ALERT_LINE,ALERT_PX,scene::REGULAR,scene::CENTER,TEXT);y+=ALERT_LINE;}
    if(int(detail.size())>shown) {
        const int x=ruleLeft+ALERT_W-SCROLL_W,markTop=y+(shown*ALERT_LINE-2*SCROLL_H-SCROLL_GAP)/2;
        chevronShape(s,x,markTop,SCROLL_W,SCROLL_H,SCROLL_T,POINT_UP,first>0?TEXT:0x40F0F0F0u);
        chevronShape(s,x,markTop+SCROLL_H+SCROLL_GAP,SCROLL_W,SCROLL_H,SCROLL_T,POINT_DOWN,first+shown<int(detail.size())?TEXT:0x40F0F0F0u);
    }
    for(int i=0;i<shown;++i){s.text(detail[std::size_t(first+i)],ruleLeft,y,ALERT_W,ALERT_LINE,ALERT_PX,scene::REGULAR,scene::CENTER,TEXT);y+=ALERT_LINE;}
    y+=ALERT_SPACE;s.rect(shortRule,y,ALERT_RULE_W,ALERT_RULE,TEXT);y+=ALERT_RULE+2*ALERT_SPACE;
    for(int i=0;i<int(a.buttons.size());++i) {
        const int x=choiceLeft(i,int(a.buttons.size()));const bool lit=i==a.focused;
        s.rect(x,y,CHOICE_W,CHOICE_H,lit?SELECT:ALERT_CELL);
        if(lit)s.rect(x,y-TAB_LINE,CHOICE_W,TAB_LINE,ACCENT_FRANKLIN);
        s.text(fit(titleCase(a.buttons[std::size_t(i)]),ROW_PX,scene::REGULAR,CHOICE_W-2*ROW_PAD,m),x,y,CHOICE_W,CHOICE_H,ROW_PX,scene::REGULAR,scene::CENTER,
               lit?SELECT_TEXT:TEXT);
        s.hit(x,y-CHOICE_TOUCH,CHOICE_W,CHOICE_H+2*CHOICE_TOUCH,HIT_ALERT_BUTTON+i);
    }
}

inline Scene build(const Page& page,const Measure& m,const Frame& f=DEFAULT_FRAME) {
    Scene s;s.panelW=f.w;s.panelH=f.h;
    if(page.hasAlert) {
        s.layer(scene::ALERT);alert(s,page.alert,m);prompts(s,page,m,f);s.contentKey="alert";return s;
    }
    if(page.backdrop)s.backdrop(W,H,BACKDROP_TOP,BACKDROP_BOTTOM);else s.fill(W,H,DIM_OVER_HUD);
    if(page.header && f.header)header(s,page,m);
    strip(s,page,m,f);
    const bool col=page.hasColumn;
    if(col)column(s,page,m,f);
    if(page.hasText)textPane(s,page,col,f);else pane(s,page,col,m,f);
    s.layer(scene::CHROME);
    const float promptsLeft=prompts(s,page,m,f);
    status(s,page,promptsLeft,m,f);
    s.contentKey=std::to_string(page.tab)+"/"+std::to_string(page.columnSelected)+"/"+(page.hasText?"true":"false");
    return s;
}
// The in-game restart notice (DESIGN.md 5.8, the drawn version of the owner's alert, layout A): black, "alert" in yellow, the message
// between two 150 x 3 rules at the measured places, "Continue" with A at the bottom right.
// touchIcons: its Continue as the check icon (a touch screen without a gamepad).
inline Scene restartAlert(const std::string& message,const Measure& m,bool touchIcons=false) {
    Scene s;s.layer(scene::ALERT);s.fill(W,H,ALERT_BLACK);
    s.text(ALERT_HEADING,0,RESTART_ALERT_TITLE_TOP,W,ALERT_TITLE_H,ALERT_TITLE_PX,scene::HEAVY,scene::CENTER,ALERT_YELLOW);
    const int rule=(W-ALERT_RULE_W)/2;
    s.rect(rule,RESTART_ALERT_RULE1,ALERT_RULE_W,ALERT_RULE,TEXT);
    s.text(fit(message,ALERT_PX,scene::REGULAR,ALERT_W,m),0,RESTART_ALERT_TEXT_TOP,W,ALERT_LINE,ALERT_PX,scene::REGULAR,scene::CENTER,TEXT);
    s.rect(rule,RESTART_ALERT_RULE2,ALERT_RULE_W,ALERT_RULE,TEXT);
    Page page;page.touchIcons=touchIcons;page.prompts.push_back({"Continue","A",nav::CONFIRM});
    prompts(s,page,m,Frame(W,H,LEFT,BLOCK_W,false,STRIP_TOP,BODY_TOP,BODY_SLOTS,W-98,H-55));
    s.hit(0,0,W,H,HIT_ALERT_BUTTON);
    s.contentKey="restart-alert";
    return s;
}
}
}
