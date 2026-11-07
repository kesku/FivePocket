// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// ios-sidemenu (iPad, SPEC §12; the owner 2026-10-25 ~00:40: "we also need a side menu for all devices. You swipe from the left side and
// get a menu, one option in that menu is the settings which allows you to change the settings at the startup of the game then and
// there. ..."), redesigned (2026-10-25, menu-redesign; the owner, via main: "The direction looks right; use the designer's recommended
// picks for the three choices and implement it with Nunito Sans."): the C++ port of android-qol's SideMenu.java as the game's pause-menu
// frame - tabs SETTINGS / TOUCH / TRAINER / MODS, a column of sections, the pane of the section's rows (ios_menu_scenes.h draws it) - with
// iOS rows (the iOS launcher's own ten settings, the touch controls, the trainer, script mods) and their effects. No UIKit: the host test
// (tests_side_menu.cpp) runs it; ios_qol_touch_view.h routes the touches and paints the frame.
//
// Live or restart on iOS, from the iOS readers (the launcher's choices word, recomp_apple_launcher_display_options):
//  LIVE    Frame Rate Cap (the renderer's pace poll and the guest limiter, both per switch window), Frame-Rate Counter (the switch
//          poll), Shadow Distance (the switch poll), Screen (Main.mm's 0.5 s layout timer), Frame Generation (fgWindow / fgGenWindow)
//          while the render resolution at start is 100 % (rsbLoad picks stage A at start for a scaled size), every touch row (the app).
//  RESTART Render Resolution (rsLoad at start), Draw Distance and Traffic Density (SPEC 12.2: the world sliders), Frame Generation at
//          a scaled start resolution, Script Mods.
// The launcher's word is frozen at start (LauncherSettingsOwner::commitGuest); the menu saves through the launcher's own store and
// publishes a second, live word in which every RESTART field keeps its start value (liveChoices), so a restart row never takes
// effect early and a saved restart change applies at the next launch.
#include "apple/launcher_display_options.h" // (LauncherDisplayOptions.hpp: the choices, their counts)
#include "ios_menu_scenes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace recomp::apple::launcher {
// ios-sidemenu (launcher_display_options.cpp): the in-game live word (main thread, only after the start's freeze); 0 = none (the frozen
// word stands). The reader (recomp_apple_launcher_display_options) returns it in place of the frozen one.
bool publishLiveLauncherDisplayOptions(const DisplayChoices& choices) noexcept;
}
namespace recomp::apple::sidemenu {
namespace ml=recomp::apple::menu::layout;namespace ms=recomp::apple::menu::scenes;
using recomp::apple::menu::Scene;using recomp::apple::menu::Measure;
inline constexpr float Edge=0.035f,OpenDx=0.06f;
inline constexpr long long OpenMs=600;
// A tap moves at most this share of the view's smaller side; a drag beyond it on the pane moves through the rows.
inline constexpr float TapSlop=0.02f;

// The sections (a tab's column items).
#if defined(RECOMP_DUO_EDITION)
// ios-duo (the Thor mirror: SideMenuRows.java's "Dual Screens" tab): SCR_LOWER, SCR_WHEELS, SCR_PHONE
enum class Page {SetDisplay,SetFrameRate,SetWorld,SetControls,Touch,TrTrainer,TrPlayer,TrVehicles,TrTeleport,TrWorld,Mods,ScrLower,ScrWheels,ScrPhone};
inline constexpr unsigned PageCount=14;
inline const char* pageName(Page p) noexcept {
    static constexpr const char* n[]{"SET_DISPLAY","SET_FRAME_RATE","SET_WORLD","SET_CONTROLS","TOUCH","TR_TRAINER","TR_PLAYER","TR_VEHICLES",
                                     "TR_TELEPORT","TR_WORLD","MODS","SCR_LOWER","SCR_WHEELS","SCR_PHONE"};
    return n[unsigned(p)];
}
#else
enum class Page {SetDisplay,SetFrameRate,SetWorld,SetControls,Touch,TrTrainer,TrPlayer,TrVehicles,TrTeleport,TrWorld,Mods};
inline constexpr unsigned PageCount=11;
inline const char* pageName(Page p) noexcept {
    static constexpr const char* n[]{"SET_DISPLAY","SET_FRAME_RATE","SET_WORLD","SET_CONTROLS","TOUCH","TR_TRAINER","TR_PLAYER","TR_VEHICLES",
                                     "TR_TELEPORT","TR_WORLD","MODS"};
    return n[unsigned(p)];
}
#endif
inline bool trainerPage(Page p) noexcept {return std::string_view(pageName(p)).substr(0,3)=="TR_";}
enum class Kind {Action,PageLink,Choice,Toggle};
// One row. value: a choice's index into its labels; a toggle's 0 / 1. help: what it does, or why it is disabled.
struct Row {
    std::string id,label,help;Kind kind=Kind::Action;std::vector<std::string> values;int value=0;bool enabled=true,restart=false;
    float fraction=-1.0f; // (a slider's position 0..1: the game's thin bar; negative for every other row)
    static Row action(std::string id,std::string label,std::string help){Row r;r.id=std::move(id);r.label=std::move(label);r.help=std::move(help);return r;}
    static Row toggle(std::string id,std::string label,bool on,bool enabled,bool restart,std::string help){
        Row r=action(std::move(id),std::move(label),std::move(help));r.kind=Kind::Toggle;r.values={"Off","On"};r.value=on?1:0;
        r.enabled=enabled;r.restart=restart;return r;}
    static Row choice(std::string id,std::string label,std::vector<std::string> values,int value,bool enabled,bool restart,std::string help){
        Row r=action(std::move(id),std::move(label),std::move(help));r.kind=Kind::Choice;r.values=std::move(values);r.value=value;
        r.enabled=enabled;r.restart=restart;return r;}
    // The value shown: a choice's label or On / Off; "" for actions.
    std::string shown() const {return value<0 || std::size_t(value)>=values.size()?std::string():values[std::size_t(value)];}
};
// A tab: its name and its sections with their names in the column.
struct Tab {std::string title;std::vector<Page> pages;std::vector<std::string> names;};
/** SPEC 3.14 (SideMenuRows.trainerTab): the Trainer tab's index, else 0. */
inline int trainerTab(const std::vector<Tab>& tabs) {for(std::size_t i=0;i<tabs.size();++i)if(tabs[i].title=="Trainer")return int(i);return 0;}
// What the player did. Change: row id and its new value; Action: row id; Opened / Closed; AlertShown; PageShown: the section shown.
struct Event {
    enum class Type {Opened,Closed,PageShown,Change,Action,AlertShown};
    Type type=Type::Opened;std::string id;int value=0;bool hasPage=false;Page page=Page::SetDisplay;
    std::string str() const { // (SideMenu.Event.toString: "PAGE MODS", "CHANGE scaler=1", "ALERT_SHOWN")
        static constexpr const char* names[]{"OPENED","CLOSED","PAGE","CHANGE","ACTION","ALERT_SHOWN"};
        std::string s=names[int(type)];
        if(!id.empty())s+=" "+id;
        if(type==Type::Change)s+="="+std::to_string(value);
        if(hasPage){s+=" ";s+=pageName(page);}
        return s;
    }
};
struct Rows {
    virtual ~Rows()=default;
    virtual std::vector<Tab> tabs() =0;
    virtual std::vector<Row> rows(Page page) =0;
    // A result line under the help (the trainer's last request), or "". Words starting "Done" are drawn in the good colour.
    virtual std::string status(Page) {return {};}
    // The info block at the top of the section's pane (why it is empty, the mods' state), or "".
    virtual std::string info(Page) {return {};}
    // The small lines at the header's right.
    virtual std::vector<std::string> header() {return {"In Game"};}
};
#if defined(RECOMP_DUO_EDITION)
inline const std::string Title="FivePocket"; // ios-duo (main 2026-10-25: header "FivePocket"; "Duo Edition" in the status lines)
#else
inline const std::string Title="FivePocket"; // (the menu header; the home-screen name is "FivePocket iOS") // (owner 2026-10-25: "FivePocket, FivePocket IOS, FivePocket Duo, and FivePocket Thor Edition")
#endif
inline const std::string AlertText="This setting change requires a restart.";

class Menu {
public:
    explicit Menu(Rows& source):source_(source) {}
    void view(float w,float h) noexcept {vw_=std::max(1.0f,w);vh_=std::max(1.0f,h);}
    void measure(Measure m) {if(m)measure_=std::move(m);}
    // Whether an on-screen control lies under a point (Pad::controlAt): a finger there is the control's, also inside the edge strip.
    void controls(std::function<bool(float,float)> at) {controls_=std::move(at);}
    bool isOpen() const noexcept {return open_;}
    bool alertShown() const noexcept {return alert_;}
    int tab() const noexcept {return tab_;}
    bool inPane() const noexcept {return inPane_;}
    Page page() {const auto ts=tabs();const auto& t=ts[std::size_t(tab_)];return t.pages[std::size_t(std::min(section_[std::size_t(tab_)],int(t.pages.size())-1))];}
    int highlighted() {return highlight_[unsigned(page())];}
    std::vector<Row> rows() {return source_.rows(page());}
    std::vector<Tab> tabs() {return source_.tabs();}
    std::vector<Event> take() {std::vector<Event> out;out.swap(events_);return out;}

    // ---------------------------------------------------------------------------------------------- opening and closing
    void openMenu() {
        if(open_)return;
        open_=true;alert_=false;after_=After::None;restartPending_=false;inPane_=false;
        if(tab_>=int(tabs().size()))tab_=0;
        add(Event::Type::Opened);
    }
    // Close (START, the Resume prompt, a swipe left, B at the top level): after a restart setting changed, the ALERT first.
    void close() {
        if(!open_)return;
        if(restartPending_){showAlert(After::Close,0);return;}
        open_=false;alert_=false;after_=After::None;fingerSet_=false;
        add(Event::Type::Closed);
    }
    // Back (B, the Back prompt): Continue on the ALERT; from the pane to the column; else close.
    void back() {
        if(!open_)return;
        if(alert_){dismissAlert();return;}
        if(inPane_ && !single()){inPane_=false;return;}
        close();
    }
    // TEST ONLY (private-trace/ios-sidemenu-cmd "alert": a screenshot of the ALERT without changing a saved setting): open, ALERT shown.
    void alertForTest() {openMenu();showAlert(After::None,0);}
    // Continue on the ALERT (a tap anywhere, A, B): then what led to it (closing, or the other tab).
    void dismissAlert() {
        if(!alert_)return;
        alert_=false;restartPending_=false;
        const After then=after_;after_=After::None;
        if(then==After::Close)close();else if(then==After::Tab)toTab(afterTab_);
    }

    // ---------------------------------------------------------------------------------------------- tabs and sections
    void prevTab() {if(open_ && !alert_)toTab(tab_-1);}
    void nextTab() {if(open_ && !alert_)toTab(tab_+1);}
    // Another tab (wrapping): after a restart setting changed on this one, the ALERT first.
    void toTab(int t) {
        const int n=int(tabs().size());if(!n)return;
        t=((t%n)+n)%n;if(t==tab_)return;
        if(restartPending_){showAlert(After::Tab,t);return;}
        tab_=t;inPane_=single();
        addPage(page());
    }

    // ---------------------------------------------------------------------------------------------- gamepad
    void up() {if(alert_)return;if(inPane_)step(-1);else toSection(section_[std::size_t(tab_)]-1);}
    void down() {if(alert_)return;if(inPane_)step(+1);else toSection(section_[std::size_t(tab_)]+1);}
    void left() {if(alert_ || !inPane_)return;const auto r=current();if(r && r->enabled && (r->kind==Kind::Choice || r->kind==Kind::Toggle))activate(*r,-1);}
    void right() {
        if(alert_)return;
        if(!inPane_){enterPane();return;}
        const auto r=current();if(r && r->enabled && (r->kind==Kind::Choice || r->kind==Kind::Toggle))activate(*r,+1);
    }
    void select() {
        if(alert_){dismissAlert();return;}
        if(!inPane_){enterPane();return;}
        const auto r=current();if(r && r->enabled)activate(*r,+1);
    }
    // The help strip's first line: the highlighted row's help, a disabled row's reason, a restart row's note.
    std::string description() {
        const auto r=inPane_?current():std::unique_ptr<Row>();
        if(!r)return {};
        if(!r->enabled)return r->help;
        return r->restart?r->help+" Applies the next time the game starts.":r->help;
    }

    // ---------------------------------------------------------------------------------------------- the drawn frame
    // View pixels per panel pixel: the panel fits the view (as the launcher's) and is centred.
#if defined(RECOMP_DUO_EDITION)
    // ios-duo (owner 2026-10-25: the Duo's menu "needs to be full screen"; menu-redesign's addendum: the same 1920 x 2924 frame in game,
    // over both halves): the frame as tall as the view (Frame::screen / panelHeight, which on the Duo also takes a portrait view); taps map
    // through the panel of the scene drawn last (the restart notice keeps 1920 x 1080).
    float unit() const noexcept {return std::min(vw_/float(ml::W),vh_/float(ml::H));}
    ml::Frame frame() const noexcept {return ml::Frame::screen(ml::panelHeight(vw_,vh_,unit()));}
#else
    float unit() const noexcept {return std::min(vw_/float(ml::W),vh_/float(ml::H));}
    const ml::Frame& frame() const noexcept {return ml::DEFAULT_FRAME;}
#endif
    // What the frame shows now, as the launcher's own page description.
    ms::Page pageOf() {
        ms::Page p;
        const auto ts=tabs();const auto& t=ts[std::size_t(tab_)];
        p.title=Title;
        for(const auto& s:source_.header())p.status.push_back({s,ms::TONE_NORMAL});
        for(const auto& x:ts)p.tabs.push_back(x.title);
        p.tab=tab_;
        if(t.pages.size()>1) {
            p.hasColumn=true;p.column=t.names;p.columnSelected=section_[std::size_t(tab_)];p.columnLit=true;
            for(const auto pg:t.pages)p.columnEnabled.push_back(!source_.rows(pg).empty() || !source_.info(pg).empty());
        }
        const Page pg=page();
        const auto info=source_.info(pg);
        if(!info.empty())p.info.push_back({info,ms::TONE_NORMAL});
        const auto rs=source_.rows(pg);
        for(const auto& r:rs) {
            ms::Row m;m.label=r.label;m.enabled=r.enabled;m.restart=r.restart;m.fraction=r.fraction;
            if(r.kind==Kind::Choice || r.kind==Kind::Toggle){m.value=r.shown();m.hasValue=true;m.arrows=true;m.halves=true;m.canLeft=m.canRight=r.values.size()>1;}
            p.rows.push_back(m);
        }
        p.rowFocused=inPane_ && !rs.empty()?std::min(highlight_[unsigned(pg)],int(rs.size())-1):-1;
        p.rowFirst=first_[unsigned(pg)];
        const auto help=description(),status=source_.status(pg);
        if(!help.empty())p.help.push_back({help,ms::TONE_NORMAL});
        if(!status.empty())p.help.push_back({status,status.rfind("Done",0)==0?ms::TONE_GOOD:ms::TONE_WARNING});
        p.touchIcons=!gamepad_; // (the strip with a gamepad, else the X and check icons: owner 2026-10-25)
        p.prompts.push_back({"Resume","START",ml::nav::MENU});
        p.prompts.push_back({"Back","B",ml::nav::BACK});
        const auto cur=current();
        if(inPane_?(cur && cur->enabled):!rs.empty())p.prompts.push_back({"Select","A",ml::nav::CONFIRM});
        return p;
    }
    // A gamepad is connected (the view tells it before each drawing): the prompt strip names its buttons; without one the prompts are the
    // X and check icons (owner 2026-10-25: "only on touch screen devices, and only when a gamepad isn't connected"; SideMenu.gamepad).
    void gamepad(bool connected) {gamepad_=connected;}
    bool gamepadShown() const {return gamepad_;}
    // Builds the frame to draw (and to hit-test the next taps against): the restart notice while the ALERT stands, else the menu.
    const Scene& scene() {
        if(alert_){shown_=ms::restartAlert(AlertText,measure_,!gamepad_);shownValid_=true;return shown_;}
        auto p=pageOf();
        first_[unsigned(page())]=ms::firstRow(p,measure_,frame());
        p.rowFirst=first_[unsigned(page())];
        shown_=ms::build(p,measure_,frame());shownValid_=true;
        return shown_;
    }

    // ---------------------------------------------------------------------------------------------- touches
    // A finger down at (x, y) in the view: true when the menu takes it (the edge strip, or anywhere while the menu is open).
    bool down(std::uintptr_t id,long long t,float x,float y) {
        if(open_){grab(id,t,x,y);return true;}
        if(x<=Edge*vw_ && !(controls_ && controls_(x,y))){grab(id,t,x,y);armed_=true;return true;}
        return false;
    }
    void move(std::uintptr_t id,long long t,float x,float y) {
        if(!fingerSet_ || id!=finger_)return;
        if(armed_) {
            const float dx=x-downX_,dy=std::fabs(y-downY_);
            if(t-downAt_<=OpenMs && dx>=OpenDx*vw_ && dx>2.0f*dy){armed_=false;fingerSet_=false;openMenu();}
            return;
        }
        if(!open_ || alert_)return;
        if(!moved_ && std::hypot(x-downX_,y-downY_)>TapSlop*std::min(vw_,vh_))moved_=true;
        if(moved_ && std::fabs(x-downX_)<std::fabs(y-downY_)) { // (a vertical drag moves the highlight, one row per row pitch)
            dragged_+=lastY_-y;
            const float pitch=float(ml::ROW_PITCH)*unit();
            while(std::fabs(dragged_)>=pitch){const int d=dragged_>0?1:-1;dragged_-=float(d)*pitch;if(!inPane_)enterPane();if(inPane_)step(d);}
        }
        lastY_=y;
    }
    void up(std::uintptr_t id,long long t,float x,float y) {
        (void)t;
        if(!fingerSet_ || id!=finger_)return;
        fingerSet_=false;
        if(armed_){armed_=false;return;} // (an edge touch that did not open: nothing)
        if(!open_)return;
        if(alert_){dismissAlert();return;}
        if(moved_){if(x-downX_<=-OpenDx*vw_ && std::fabs(x-downX_)>2.0f*std::fabs(y-downY_))close();return;} // (a swipe left closes)
        tap(x,y);
    }
    void cancel() noexcept {fingerSet_=false;armed_=false;}
    bool owns(std::uintptr_t id) const noexcept {return fingerSet_ && finger_==id;}
    // The panel point of a view point (the panel fitted to the view and centred).
    void toPanel(float x,float y,float& px,float& py) const noexcept {
#if defined(RECOMP_DUO_EDITION)
        const float pw=float(shown_.panelW),ph=float(shown_.panelH),u=unit(); // ios-duo: the panel of the scene drawn last
        px=(x-(vw_-pw*u)/2.0f)/u;py=(y-(vh_-ph*u)/2.0f)/u;
#else
        const float u=unit();const auto& f=frame();
        px=(x-(vw_-float(f.w)*u)/2.0f)/u;py=(y-(vh_-float(f.h)*u)/2.0f)/u;
#endif
    }
    // A tap at a view point: what the frame drawn last has there (Scene::hitAt).
    void tap(float x,float y) {
        if(!shownValid_)scene();
        float px,py;toPanel(x,y,px,py);
        hit(shown_.hitAt(px,py));
    }
    // A touch rectangle of the frame (layout::HIT_*).
    void hit(int id) {
        if(id<0 || !open_)return;
        if(alert_){dismissAlert();return;}
        if(id>=ml::HIT_PROMPT && id<ml::HIT_ALERT_BUTTON) {
            switch(id-ml::HIT_PROMPT){
            case ml::nav::BACK:back();return;case ml::nav::CONFIRM:select();return;case ml::nav::MENU:close();return;
            case ml::nav::PAGE_PREV:prevTab();return;case ml::nav::PAGE_NEXT:nextTab();return;default:return;}
        }
        if(id>=ml::HIT_TAB && id<ml::HIT_COLUMN){toTab(id-ml::HIT_TAB);return;}
        if(id>=ml::HIT_COLUMN && id<ml::HIT_ROW) {
            const int s=id-ml::HIT_COLUMN;
            if(s==section_[std::size_t(tab_)] && !inPane_)enterPane();else{toSection(s);inPane_=false;}
            return;
        }
        const bool leftHalf=id>=ml::HIT_ARROW_LEFT && id<ml::HIT_ARROW_RIGHT,rightHalf=id>=ml::HIT_ARROW_RIGHT && id<ml::HIT_PROMPT;
        const int i=leftHalf?id-ml::HIT_ARROW_LEFT:rightHalf?id-ml::HIT_ARROW_RIGHT:id-ml::HIT_ROW;
        const auto rs=rows();
        if(i<0 || i>=int(rs.size()))return;
        const bool wasHighlighted=inPane_ && highlighted()==i;
        inPane_=true;highlight_[unsigned(page())]=i;
        const auto& r=rs[std::size_t(i)];
        if(!r.enabled)return; // (highlighted: its reason shows in the help strip; nothing changes)
        // Two taps for a value (owner 2026-10-25, via menu-redesign, after one stray tap turned the Thor's lower screen off): a tap on a
        // value row that is not the highlighted one only highlights it; the left or right half of the highlighted row then steps it.
        // An action row (the trainer's) acts on one tap.
        if((r.kind==Kind::Choice || r.kind==Kind::Toggle) && !wasHighlighted)return;
        activate(r,leftHalf?-1:+1);
    }

private:
    enum class After {None,Close,Tab};
    void add(Event::Type t){Event e;e.type=t;events_.push_back(e);}
    void addPage(Page p){Event e;e.type=Event::Type::PageShown;e.hasPage=true;e.page=p;events_.push_back(e);}
    void showAlert(After then,int toTab_){alert_=true;after_=then;afterTab_=toTab_;add(Event::Type::AlertShown);}
    bool single() {const auto ts=tabs();return ts.empty() || ts[std::size_t(tab_)].pages.size()<=1;}
    void toSection(int s) {
        const auto ts=tabs();const auto& t=ts[std::size_t(tab_)];const int n=int(t.pages.size());
        s=((s%n)+n)%n;
        if(s==section_[std::size_t(tab_)])return;
        section_[std::size_t(tab_)]=s;
        addPage(page());
    }
    void enterPane() {if(!rows().empty())inPane_=true;}
    std::unique_ptr<Row> current() {const auto rs=rows();const int i=highlighted();return i>=0 && i<int(rs.size())?std::make_unique<Row>(rs[std::size_t(i)]):nullptr;}
    void step(int d) {const int n=int(rows().size());if(!n)return;highlight_[unsigned(page())]=((highlighted()+d)%n+n)%n;}
    void activate(const Row& r,int dir) {
        switch(r.kind) {
        case Kind::PageLink:break;
        case Kind::Action:{Event e;e.type=Event::Type::Action;e.id=r.id;events_.push_back(e);break;}
        case Kind::Toggle:change(r,r.value==1?0:1);break;
        case Kind::Choice:{const int n=int(r.values.size());if(!n)break;change(r,((r.value+dir)%n+n)%n);break;}
        }
    }
    void change(const Row& r,int v) {
        Event e;e.type=Event::Type::Change;e.id=r.id;e.value=v;events_.push_back(e);
        if(r.restart)restartPending_=true;
    }
    void grab(std::uintptr_t id,long long t,float x,float y){finger_=id;fingerSet_=true;downX_=x;downY_=lastY_=y;downAt_=t;moved_=false;dragged_=0;}

    Rows& source_;
    Measure measure_=[](const std::string& text,int size,int){return float(text.size())*float(size)*0.5f;};
    std::function<bool(float,float)> controls_;
    std::vector<Event> events_;
    bool open_=false,armed_=false,alert_=false,inPane_=false;
    int tab_=0;
    std::array<int,16> section_{};
    std::array<int,PageCount> highlight_{},first_{};
    bool restartPending_=false;
    After after_=After::None;int afterTab_=0;
    std::uintptr_t finger_=0;bool fingerSet_=false;float downX_=0,downY_=0,lastY_=0,dragged_=0;long long downAt_=0;bool moved_=false;
    float vw_=1,vh_=1;
    Scene shown_;bool shownValid_=false;bool gamepad_=true;
};

// ================================================================================================ iOS rows
// The launcher's fields (LauncherDisplayOptions.hpp: cap, scaler, sharpness, resolution, counter, frame generation, draw distance,
// traffic, shadow distance, screen) and their labels as the launcher shows them (LauncherCoordinator.mm).
enum Field : unsigned {Cap=0,Scaler=1,Sharpness=2,Resolution=3,Counter=4,FrameGen=5,DrawDistance=6,Traffic=7,Shadow=8,Screen=9};
inline constexpr unsigned ResolutionOriginal=3; // (ResolutionPercent[3] = 100)
// The RESTART fields (the readers read them at start, or SPEC 12.2 says so); the rest of the word is live.
inline bool restartField(unsigned field,unsigned startResolution) noexcept {
    return field==Resolution || field==DrawDistance || field==Traffic || (field==FrameGen && startResolution!=ResolutionOriginal);
}
// The live word's choices: the saved ones, with every restart field (and the Apple-unavailable scaler / sharpness) at its start value.
inline launcher::DisplayChoices liveChoices(const launcher::DisplayChoices& saved,const launcher::DisplayChoices& start) noexcept {
    launcher::DisplayChoices out=saved;
    for(unsigned f=0;f<launcher::ChoiceCounts.size();++f)
        if(f==Scaler || f==Sharpness || restartField(f,start.values[Resolution]))out.values[f]=start.values[f];
    return out;
}
// The touch controls' saved words (settings/touch-controls.txt; ios_touch_pad.h Pad::settings reads them).
struct TouchSettings {
    int mode=0;          // 0 Universal, 1 360 pad, 2 Off (mode=pad / mode=off)
    bool keep=false;     // Keep With A Controller (hide=0)
    bool layoutB=true;   // layout=a for A
    int size=100,opacity=80;
    bool operator==(const TouchSettings&) const=default;
};
inline std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;std::size_t i=0;
    while(i<text.size()) {
        while(i<text.size() && (text[i]==' ' || text[i]=='\n' || text[i]=='\r' || text[i]=='\t'))++i;
        const std::size_t s=i;while(i<text.size() && !(text[i]==' ' || text[i]=='\n' || text[i]=='\r' || text[i]=='\t'))++i;
        if(i>s)out.push_back(text.substr(s,i-s));
    }
    return out;
}
inline int number(std::string_view v) noexcept {
    if(v.empty() || v.size()>4)return -1;int n=0;for(char c:v){if(c<'0' || c>'9')return -1;n=n*10+(c-'0');}return n;
}
// As Pad::settings parses them (the same ranges: size 70-130, opacity 20-100).
inline TouchSettings parseTouch(std::string_view text) {
    TouchSettings s;
    for(const auto w:words(text)) {
        const auto eq=w.find('=');if(eq==std::string_view::npos || !eq)continue;
        const auto k=w.substr(0,eq),v=w.substr(eq+1);
        if(k=="mode")s.mode=v=="pad"?1:v=="off"?2:0;
        else if(k=="hide")s.keep=v=="0";
        else if(k=="layout")s.layoutB=v!="a";
        else if(k=="size"){const int n=number(v);if(n>=70 && n<=130)s.size=n;}
        else if(k=="opacity"){const int n=number(v);if(n>=20 && n<=100)s.opacity=n;}
    }
    return s;
}
// The file's text with these settings: the menu's five keys written again (only where they differ from the defaults), every other word
// (rswipe, wstyle, show, ...) kept as it was, in its order.
inline std::string touchText(std::string_view existing,const TouchSettings& s) {
    std::string out;
    const auto put=[&](std::string_view w){if(!out.empty())out+=' ';out+=w;};
    for(const auto w:words(existing)) {
        const auto eq=w.find('=');const auto k=eq==std::string_view::npos?w:w.substr(0,eq);
        if(k=="mode" || k=="hide" || k=="layout" || k=="size" || k=="opacity")continue;
        put(w);
    }
    if(s.mode==1)put("mode=pad");else if(s.mode==2)put("mode=off");
    if(s.keep)put("hide=0");
    if(!s.layoutB)put("layout=a");
    if(s.size!=100)put("size="+std::to_string(s.size));
    if(s.opacity!=80)put("opacity="+std::to_string(s.opacity));
    out+='\n';
    return out;
}
// The trainer as the menu sees it (recomp_trainer_v1 ... in the same binary; a fake on the host). SPEC 12.4.
struct Trainer {
    std::function<bool()> present;                       // the game build has the trainer
    std::function<unsigned()> state;                     // toggles in bits 0..6, enabled 1<<16, gameplay 1<<17, busy 1<<18
    std::function<int(int)> count;                       // trainer.h List: 0 vehicles, 1 landmarks, 2 weathers
    std::function<std::string(int,int)> label;
    std::function<std::string()> status;                 // recomp_trainer_status_v1: the last request's line, or ""
    std::function<std::string()> mods;                   // recomp_mods_status_v1: "enabled=.. found=.. applied=.. skipped=..", or ""
};
inline constexpr int ListVehicles=0,ListLandmarks=1,ListWeathers=2;
inline constexpr unsigned StateEnabled=1u<<16,StateGameplay=1u<<17;
inline constexpr std::array<const char*,7> ToggleLabels{"Invincible","Never Wanted","Infinite Ammo","Super Jump","Fast Run","Fast Swim","Freeze Time"};
inline constexpr const char* TextNotInBuild="Not in the installed game build.";
inline constexpr const char* TextNotInVersion="Not available in this version.";
inline constexpr const char* TextTrainerOff="The trainer is off. Turn it on under Trainer.";
inline std::string hourText(int h) {char b[8];std::snprintf(b,sizeof b,"%02d:00",h);return b;}
// The trainer's last result on its pages (the library's "seq=N ok|fail|blocked|dropped <action> [detail]"), in words.
inline std::string trainerStatusText(std::string_view raw) {
    std::vector<std::string> w;for(const auto x:words(raw))w.emplace_back(x);
    if(w.size()<3 || w[0].rfind("seq=",0)!=0)return {};
    const auto dashes=[](std::string s){for(auto& c:s)if(c=='-')c=' ';return s;};
    const std::string what=dashes(w[2]);std::string detail;
    for(std::size_t i=3;i<w.size();++i) {
        if(w[i].find('=')==std::string::npos){if(!detail.empty())detail+=' ';detail+=dashes(w[i]);}
        else if(w[i].rfind("blocked=",0)==0){if(!detail.empty())detail+=' ';detail+=dashes(w[i].substr(8));}
    }
    const std::string tail=detail.empty()?std::string():" ("+detail+")";
    if(w[1]=="ok")return "Done: "+what+".";
    if(w[1]=="fail")return "Not done: "+what+tail+".";
    if(w[1]=="blocked")return "Waiting: "+what+tail+".";
    if(w[1]=="dropped")return "Not done, the game was busy: "+what+tail+".";
    return {};
}
// The library's mods line ("enabled=1 found=3 applied=2 skipped=1") in words; "" without it.
inline std::string modsText(std::string_view raw) {
    if(raw.empty())return {};
    const auto nl=raw.find('\n');const auto first=raw.substr(0,nl);
    int enabled=-1,found=0,applied=0,skipped=0;
    for(const auto w:words(first)) {
        const auto eq=w.find('=');if(eq==std::string_view::npos || !eq)continue;
        const auto k=w.substr(0,eq),v=w.substr(eq+1);int n=0;bool ok=!v.empty();
        for(char c:v){if(c<'0' || c>'9'){ok=false;break;}n=n*10+(c-'0');}
        if(!ok)continue;
        if(k=="enabled")enabled=n;else if(k=="found")found=n;else if(k=="applied")applied=n;else if(k=="skipped")skipped=n;
    }
    if(enabled!=1)return {};
    if(!found)return "No mods in the mods folder.";
    return std::to_string(applied)+(applied==1?" mod":" mods")+" applied"+(skipped>0?", "+std::to_string(skipped)+" skipped":"")+".";
}

#if defined(RECOMP_DUO_EDITION)
// ios-duo: the Dual Screens rows' state (the Thor's SettingsSchema rows, the Duo's files): minimap 0 = both screens, 1 = the bottom
// screen only (settings/minimap.txt "both" / "bottom"; the Thor's Top Screen Only is the Duo's Lower Screen Off); lowerShown = the
// split (Off = full screen, duo-screen.txt, live); the wheels' and the phone's screens (second-screen.txt "wheel=main" ...); the phone's
// background (private-trace/diag-phone-pov: floor / close / wide / black, live). The first three apply when the game starts.
// topMode (2026-11-05, owner: "Make it toggleable"; Stretch "looks ugly": Fit first): the top screen's picture, 0 Fit / 1 Zoom /
// 2 Stretch / 3 4:3 (duo_state.h topPicture; settings/duo-top-screen.txt; live, except 4:3 on or off: a restart, the row says so).
// frame4x3: the 4:3 layout is on in this process (duo_state.h frame4x3).
struct DualScreens {int minimap=1;bool lowerShown=true,weaponMain=false,radioMain=false,characterMain=false,phoneMain=false;int phoneBg=0;int topMode=0;
    bool frame4x3=false;};
#endif
class IosRows : public Rows {
public:
    launcher::DisplayChoices saved{};      // the launcher's saved choices (or the original)
    launcher::DisplayChoices start{};      // what the game started with (the frozen word, or the original)
    std::array<std::uint64_t,10> allowed{}; // the launcher's capability mask per field
    bool settingsAvailable=false;          // the launcher's store could be read (else every setting row is disabled)
    TouchSettings touch{};
    bool touchShown=true;                  // the HIDE eye's state (this session)
    Trainer trainer{};                     // present == nullptr: no trainer in this build
#if defined(RECOMP_DUO_EDITION)
    DualScreens dual{};                    // ios-duo: the Dual Screens tab
#endif
    bool scriptModsPresent=false,scriptModsOn=false;
    int wanted=0,hour=12;                  // the trainer rows' chosen values (this session)

    // The tabs (menu-redesign: SETTINGS with the launcher's sections; the iPad has no low-delay input, so no Controls section).
    std::vector<Tab> tabs() override {
        return {{"Settings",{Page::SetDisplay,Page::SetFrameRate,Page::SetWorld},{"Display","Frame Rate","World"}},
#if defined(RECOMP_DUO_EDITION)
                {"Dual Screens",{Page::ScrLower,Page::ScrWheels,Page::ScrPhone},{"Lower Screen","Wheels","Phone"}}, // ios-duo (SideMenuRows.java)
#endif
                {"Touch",{Page::Touch},{"Touch Controls"}},
                {"Trainer",{Page::TrTrainer,Page::TrPlayer,Page::TrVehicles,Page::TrTeleport,Page::TrWorld},{"Trainer","Player","Vehicles","Teleport","World"}},
                {"Mods",{Page::Mods},{"Script Mods"}}};
    }
#if defined(RECOMP_DUO_EDITION)
    std::vector<std::string> header() override {return {"Duo Edition","In Game"};} // ios-duo
#else
    std::vector<std::string> header() override {return {"FivePocket iOS","In Game"};} // (2026-11-05, main: no "Private Build" in a public app)
#endif
    bool hasTrainer() const {return trainer.present && trainer.present();}
    bool trainerOn() const {return hasTrainer() && trainer.state && (trainer.state()&StateEnabled)!=0;}
    std::vector<Row> rows(Page page) override {
        std::vector<Row> l;
        switch(page) {
#if defined(RECOMP_DUO_EDITION) // ios-duo: the top screen's Stretch / Fit / Zoom (Dual Screens) replaces Screen's Fill / Fit
        case Page::SetDisplay:for(const unsigned f:{Scaler,Sharpness,Resolution})l.push_back(setting(f));break;
#else
        case Page::SetDisplay:for(const unsigned f:{Scaler,Sharpness,Resolution,Screen})l.push_back(setting(f));break;
#endif
        case Page::SetFrameRate:for(const unsigned f:{Cap,FrameGen,Counter})l.push_back(setting(f));break;
        case Page::SetWorld:for(const unsigned f:{Traffic,DrawDistance,Shadow})l.push_back(setting(f));break;
        case Page::SetControls:break;
        case Page::Touch:
            l.push_back(Row::toggle("touch:show","Show Touch Controls",touchShown,true,false,"Hides or shows the on-screen controls (the eye button does the same)."));
            l.push_back(Row::choice("touchControls","Touch Controls",{"Universal (changes in vehicles)","Xbox 360 pad","Off"},touch.mode,true,false,
                                    "On-screen buttons that change in vehicles, or a full 360 pad."));
            l.push_back(Row::choice("touchLayout","Touch Control Layout",{"B (default)","A"},touch.layoutB?0:1,true,false,
                                    "B: big buttons in the corners. A: the first layout, more buttons."));
            l.push_back(percentRow("touchSize","Touch Control Size",{85,100,115},touch.size,"The size of the on-screen buttons and sticks."));
            l.push_back(percentRow("touchOpacity","Touch Control Opacity",{60,80,100},touch.opacity,"How solid the on-screen controls are drawn."));
            l.push_back(Row::toggle("touchKeep","Keep Touch Controls With A Controller",touch.keep,true,false,"Off: hidden while a controller is used; a touch shows them."));
            break;
        case Page::TrTrainer:
            if(hasTrainer())l.push_back(Row::toggle("tr:on","Trainer",trainerOn(),true,false,"Turns the trainer on. Single player only; it acts only while you play."));
            else l.push_back(Row::toggle("tr:on","Trainer",false,false,false,TextNotInBuild));
            break;
        case Page::TrPlayer:if(trainerOn())playerRows(l);break;
        case Page::TrVehicles:if(trainerOn()){l.push_back(Row::action("tr:fix","Repair Vehicle","Repairs and cleans the vehicle you are in."));
            list(l,ListVehicles,"veh:","Spawns it beside you and puts you in it.");}break;
        case Page::TrTeleport:if(trainerOn()){l.push_back(Row::action("tr:waypoint","Teleport To Waypoint","Takes you to the map's waypoint."));
            list(l,ListLandmarks,"lm:","Takes you there.");}break;
        case Page::TrWorld:if(trainerOn())worldRows(l);break;
        case Page::Mods:
            l.push_back(Row::toggle("scriptMods","Script Mods",scriptModsOn,scriptModsPresent,true,
                                    scriptModsPresent?"Loads the mods in the game's mods folder.":TextNotInVersion));
            break;
#if defined(RECOMP_DUO_EDITION)
        // ios-duo: the Thor's rows and hints (SettingsSchema.java); restart = read when the game starts
        case Page::ScrLower:
            {const int top=std::clamp(dual.topMode,0,3);
             std::string hint=top==1?"The picture enlarged to fill the screen, its shape kept; the edges are cut off.":
                              top==2?"The whole picture stretched to fill the screen; no bars, nothing cut off.":
                              top==3?"The game's own 4:3 layout: menus and HUD placed for a 4:3 screen, a narrower view.":
                                     "The whole picture, its shape kept; bars where the screen's shape differs.";
             if((top==3)!=dual.frame4x3)hint+=top==3?" Restart the game to apply.":" Restart the game to leave the 4:3 layout.";
             l.push_back(Row::choice("scr:top","Top Screen",{"Fit","Zoom","Stretch","4:3"},top,true,false,hint));}
            l.push_back(Row::choice("scr:minimap","Minimap",{"Both Screens","Bottom Screen Only"},dual.minimap,true,true,
                                    dual.minimap==1?"The top screen leaves out the minimap; the lower screen shows it.":"The minimap on both screens."));
            l.push_back(Row::choice("scr:lower","Lower Screen",{"HUD","Off (Full Screen)"},dual.lowerShown?0:1,true,false,
                                    dual.lowerShown?"The minimap, wheels, phone and help text on the lower screen.":"Full screen: everything shows on the game's picture."));
            break;
        case Page::ScrWheels:
            l.push_back(Row::choice("scr:weapon","Weapon Wheel",{"Lower Screen","Main Screen"},dual.weaponMain?1:0,true,true,
                                    dual.weaponMain?"The game's own wheel on the main screen, as on other devices.":"Hold LB: shown on the lower screen; touch a weapon to pick it."));
            l.push_back(Row::choice("scr:radio","Radio Wheel",{"Lower Screen","Main Screen"},dual.radioMain?1:0,true,true,
                                    dual.radioMain?"The game's own radio wheel on the main screen, as on other devices.":"Hold D-pad left in a vehicle: shown on the lower screen; touch a station to pick it."));
            l.push_back(Row::choice("scr:character","Character Wheel",{"Lower Screen","Main Screen"},dual.characterMain?1:0,true,true,
                                    dual.characterMain?"The game's own character wheel on the main screen, as on other devices.":"Hold D-pad down: shown on the lower screen; touch a character to pick them."));
            break;
        case Page::ScrPhone:
            l.push_back(Row::choice("scr:phone","Phone",{"Lower Screen","Main Screen"},dual.phoneMain?1:0,true,true,
                                    dual.phoneMain?"The game's own phone on the main screen, as on other devices.":"The phone on the lower screen; swipe and tap it there."));
            l.push_back(Row::choice("scr:phonebg","Phone Background",{"Floor","Floor, Close Up","Floor, Wide","Black"},dual.phoneBg,true,false,
                                    dual.phoneBg==3?"Behind the phone: black.":"Behind the phone: the ground where you stand, as if you look down at it."));
            break;
#endif
        }
        return l;
    }
    std::string info(Page page) override {
        switch(page) {
        case Page::TrPlayer:case Page::TrVehicles:case Page::TrTeleport:case Page::TrWorld:
            if(!hasTrainer())return TextNotInBuild;
            return trainerOn()?std::string():TextTrainerOff;
        case Page::Mods:return trainer.mods?modsText(trainer.mods()):std::string();
        default:return {};
        }
    }
    std::string status(Page page) override {
        if(!trainerPage(page) || !trainer.status)return {};
        return trainerStatusText(trainer.status());
    }
    static const char* fieldId(unsigned f) noexcept {
        static constexpr const char* ids[]{"frameCap","scaler","fsrSharpness","renderResolution","fpsOverlay","frameGeneration",
                                           "drawDistance","traffic","shadowDistance","screen"};
        return f<10?ids[f]:"";
    }
    static int fieldOf(std::string_view id) noexcept {for(unsigned f=0;f<10;++f)if(id==fieldId(f))return int(f);return -1;}
    // The field's value for a Change event's index on its row (as last built); -1 for an unknown row or index.
    int settingValue(std::string_view id,int index) const {
        const int f=fieldOf(id);if(f<0)return -1;
        const auto& vs=shown_[unsigned(f)];
        return index>=0 && std::size_t(index)<vs.size()?int(vs[std::size_t(index)]):-1;
    }
    int percentValue(std::string_view id,int index) const {
        const auto& vs=id=="touchSize"?sizes_:opacities_;
        return index>=0 && std::size_t(index)<vs.size()?vs[std::size_t(index)]:-1;
    }
private:
    static const std::vector<std::string>& labels(unsigned f) {
        static const std::vector<std::string> l[10]{
            {"Off (standing 60 fps)","60 fps","40 fps","30 fps","24 fps","Automatic"},
            {"Unavailable on Apple","Unavailable on Apple","Unavailable on Apple","Unavailable on Apple"},
            std::vector<std::string>(41,"Unavailable on Apple"),
            {"50% (internal)","75% (internal)","88% (internal)","100% (internal)","115% (internal)","125% (internal)","150% (internal)"},
            {"Off","On"},{"Off","On"},{"Original","90%","80%","70%"},{"Original","75%","50%","25%"},{"Original","Near"},{"Fill","Fit"}};
        return l[f];
    }
    static const char* title(unsigned f) noexcept {
        static constexpr const char* t[]{"Frame Rate Cap","Scaler","FSR Sharpness","Render Resolution","Frame-Rate Counter","Frame Generation",
                                         "Draw Distance","Traffic Density","Shadow Distance","Screen"};
        return t[f];
    }
    // The launcher's one-line descriptions, shortened to the help strip (LauncherCoordinator.mm).
    static const char* help(unsigned f) noexcept {
        switch(f) {
        case Cap:return "Original: 30 fps, evenly paced. Off, 60 and 40 let the game run up to 60 fps.";
        case Scaler:case Sharpness:return "No Apple scaler or sharpness implementation.";
        case Resolution:return "The size the game draws at, against 720p.";
        case Counter:return "Shows the frame rate in the top left corner.";
        case FrameGen:return "Generated frames between real ones: a 30 fps game, a 60 fps display.";
        case DrawDistance:return "How far detailed buildings and props reach. Lower is faster while driving.";
        case Traffic:return "How many moving and parked cars spawn. Lower is faster while driving.";
        case Shadow:return "Near leaves out the farthest shadows. Cuts CPU work while driving.";
        case Screen:return "Fill covers the whole screen; Fit shows the whole picture with bars.";
        }
        return "";
    }
    Row setting(unsigned f) {
        const unsigned savedValue=saved.values[f];
        std::vector<unsigned> vs;
        if(f!=Scaler && f!=Sharpness)
            for(unsigned v=0;v<launcher::ChoiceCounts[f];++v)if(v==savedValue || (allowed[f]&(1ull<<v)))vs.push_back(v);
        if(vs.empty())vs.push_back(savedValue);
        std::vector<std::string> names;int at=0;
        for(std::size_t i=0;i<vs.size();++i){names.push_back(labels(f)[vs[i]]);if(vs[i]==savedValue)at=int(i);}
        shown_[f]=vs;
        const bool enabled=settingsAvailable && vs.size()>1;
        const std::string why=f==Scaler || f==Sharpness?help(f):settingsAvailable?TextNotInVersion:"The launcher's settings could not be read.";
        const bool restart=restartField(f,start.values[Resolution]);
        if(f==Counter || f==FrameGen)return Row::toggle(fieldId(f),title(f),savedValue!=0,enabled,restart,enabled?help(f):why);
        return Row::choice(fieldId(f),title(f),std::move(names),at,enabled,restart,enabled?help(f):why);
    }
    Row percentRow(const char* id,const char* label,std::vector<int> vs,int savedValue,const char* text) {
        if(std::find(vs.begin(),vs.end(),savedValue)==vs.end()){vs.push_back(savedValue);std::sort(vs.begin(),vs.end());}
        std::vector<std::string> names;int at=0;
        for(std::size_t i=0;i<vs.size();++i){names.push_back(std::to_string(vs[i])+" %");if(vs[i]==savedValue)at=int(i);}
        (std::string_view(id)=="touchSize"?sizes_:opacities_)=vs;
        return Row::choice(id,label,std::move(names),at,true,false,text);
    }
    void playerRows(std::vector<Row>& l) {
        const unsigned s=trainer.state();
        for(unsigned b=0;b<6;++b)l.push_back(Row::toggle("tr:t"+std::to_string(b),ToggleLabels[b],((s>>b)&1u)!=0,true,false,"Stays on until you turn it off."));
        l.push_back(Row::choice("tr:wanted","Wanted Level",{"0 Stars","1 Star","2 Stars","3 Stars","4 Stars","5 Stars"},wanted,true,false,"Sets your wanted level now."));
        l.push_back(Row::action("tr:heal","Heal And Armour","Full health and armour."));
        l.push_back(Row::action("tr:weapons","Give All Weapons","Every weapon, with ammo."));
    }
    void worldRows(std::vector<Row>& l) {
        std::vector<std::string> hours;for(int h=0;h<24;++h)hours.push_back(hourText(h));
        l.push_back(Row::choice("tr:time","Time Of Day",std::move(hours),hour,true,false,"Sets the clock to this hour now."));
        l.push_back(Row::toggle("tr:t6",ToggleLabels[6],((trainer.state()>>6)&1u)!=0,true,false,"Stays on until you turn it off."));
        l.push_back(Row::action("tr:clearwx","Clear Weather","Back to the game's own weather."));
        list(l,ListWeathers,"wx:","Sets the weather until you clear it.");
    }
    void list(std::vector<Row>& l,int which,const char* prefix,const char* text) {
        const int n=trainer.count?std::max(0,trainer.count(which)):0;
        for(int i=0;i<n;++i){auto lab=trainer.label?trainer.label(which,i):std::string();l.push_back(Row::action(prefix+std::to_string(i),lab.empty()?"?":lab,text));}
    }
    std::array<std::vector<unsigned>,10> shown_{};
    std::vector<int> sizes_{85,100,115},opacities_{60,80,100};
};

// ================================================================================================ effects
// trainer.h Action
inline constexpr int TrSync=0,TrToggle=1,TrWanted=2,TrHeal=3,TrGiveWeapons=4,TrSpawnVehicle=5,TrFixVehicle=6,TrTeleportWaypoint=7,
                     TrTeleportLandmark=8,TrSetTime=9,TrSetWeather=10,TrClearWeather=11,
                     TrOpenSaveMenu=12; // (library v716+ trainer.h OpenSaveMenu: the game's Save Game menu; the pad's SAVE button, SPEC 3.14)
struct Effects {
    std::function<bool(const launcher::DisplayChoices&)> saveDisplay; // the launcher's store (true: written), then the live word
    std::function<bool(const TouchSettings&)> saveTouch;              // touch-controls.txt, then the pad's words again
    std::function<void(bool)> touchShown;                             // the HIDE eye's state
    std::function<bool(bool)> scriptMods;                             // settings/mods.txt "scripts=on|off" (true: written)
    std::function<void(bool)> trainerEnabled;                         // settings/trainer.txt "trainer=on|off", then a Sync request
    std::function<int(int,int,int)> trainer;                          // recomp_trainer_v1; < 0: refused
#if defined(RECOMP_DUO_EDITION)
    std::function<bool(const DualScreens&)> saveDual;                 // ios-duo: the Dual Screens files (true: written)
#endif
};
inline void apply(IosRows& rows,const Effects& fx,const Event& e) {
    const auto tr=[&](int a,int a0,int a1){if(fx.trainer)fx.trainer(a,a0,a1);};
    if(e.type==Event::Type::Change) {
        const std::string& id=e.id;
        if(id=="touch:show"){rows.touchShown=e.value==1;if(fx.touchShown)fx.touchShown(rows.touchShown);return;}
        if(id=="tr:on"){if(fx.trainerEnabled)fx.trainerEnabled(e.value==1);return;}
        if(id=="tr:wanted"){rows.wanted=e.value;tr(TrWanted,e.value,0);return;}
        if(id=="tr:time"){rows.hour=e.value;tr(TrSetTime,e.value,0);return;} // (menu-redesign: Time Of Day is a value row)
        if(id.rfind("tr:t",0)==0){tr(TrToggle,number(std::string_view(id).substr(4)),e.value==1?1:0);return;}
        if(id=="scriptMods"){if(fx.scriptMods && fx.scriptMods(e.value==1))rows.scriptModsOn=e.value==1;return;}
#if defined(RECOMP_DUO_EDITION)
        if(id.rfind("scr:",0)==0) { // ios-duo
            DualScreens next=rows.dual;const int v=e.value;
            if(id=="scr:top")next.topMode=std::clamp(v,0,3);
            else if(id=="scr:minimap")next.minimap=std::clamp(v,0,1);
            else if(id=="scr:lower")next.lowerShown=v==0;
            else if(id=="scr:weapon")next.weaponMain=v==1;
            else if(id=="scr:radio")next.radioMain=v==1;
            else if(id=="scr:character")next.characterMain=v==1;
            else if(id=="scr:phone")next.phoneMain=v==1;
            else if(id=="scr:phonebg")next.phoneBg=std::clamp(v,0,3);
            else return;
            if(fx.saveDual && fx.saveDual(next))rows.dual=next;
            return;
        }
#endif
        if(id.rfind("touch",0)==0) {
            TouchSettings next=rows.touch;
            if(id=="touchControls")next.mode=std::clamp(e.value,0,2);
            else if(id=="touchLayout")next.layoutB=e.value==0;
            else if(id=="touchKeep")next.keep=e.value==1;
            else if(id=="touchSize" || id=="touchOpacity"){const int v=rows.percentValue(id,e.value);if(v<0)return;(id=="touchSize"?next.size:next.opacity)=v;}
            else return;
            if(fx.saveTouch && fx.saveTouch(next))rows.touch=next; // (not written: the rows keep the saved state)
            return;
        }
        const int f=IosRows::fieldOf(id),v=rows.settingValue(id,e.value);
        if(f<0 || v<0)return; // (not a setting row of this build)
        auto next=rows.saved;next.values[unsigned(f)]=std::uint8_t(v);
        if(fx.saveDisplay && fx.saveDisplay(next))rows.saved=next;
        return;
    }
    if(e.type!=Event::Type::Action)return;
    const std::string& id=e.id;
    if(id=="tr:heal"){tr(TrHeal,0,0);return;}
    if(id=="tr:weapons"){tr(TrGiveWeapons,0,0);return;}
    if(id=="tr:fix"){tr(TrFixVehicle,0,0);return;}
    if(id=="tr:waypoint"){tr(TrTeleportWaypoint,0,0);return;}
    if(id=="tr:clearwx"){tr(TrClearWeather,0,0);return;}
    const auto colon=id.find(':');if(colon==std::string::npos)return;
    const int i=number(std::string_view(id).substr(colon+1));if(i<0)return;
    const auto kind=std::string_view(id).substr(0,colon);
    if(kind=="veh")tr(TrSpawnVehicle,i,1); // (a1 = 1: a0 is an index into the trainer's vehicle list)
    else if(kind=="lm")tr(TrTeleportLandmark,i,0);
    else if(kind=="wx")tr(TrSetWeather,i,0);
}
}
