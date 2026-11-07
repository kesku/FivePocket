// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>

namespace recomp::apple::launcher {
// Behavioural counterpart of Android ShellNav. No file, controller handler,
// guest start or setting consumer is owned by this pure navigation model.
enum class Page:int { Play,Settings,Controller,About };
enum class Command:int { None,Up,Down,Left,Right,Confirm,Back,PreviousPage,NextPage,Menu,Alternate };
enum class Answer:int { Nothing,Moved,Tab,Entered,Activate,PaneCommand,Ask };
enum class Level:int { Strip,Column,Pane };
class Navigation {
public:
    static constexpr int Pages=4;
    Page page() const noexcept {return static_cast<Page>(tab_);}
    Level level() const noexcept {return onStrip_?Level::Strip:inPane_[tab_]?Level::Pane:Level::Column;}
    int column(Page page) const noexcept {return valid(page)?column_[index(page)]:-1;}
    bool inPane(Page page) const noexcept {return valid(page) && inPane_[index(page)];}
    bool wentToStrip() const noexcept {return wentToStrip_;}
    bool asking() const noexcept {return pendingTab_>=0 || pendingStrip_;}
    void setColumnCount(Page page,int count) noexcept {
        if(!valid(page))return;
        const auto i=index(page);columnCount_[i]=std::max(1,count);
        column_[i]=std::min(column_[i],columnCount_[i]-1);
    }
    void place(Page page,int item,bool pane) noexcept {
        if(!valid(page))return;
        const auto i=index(page);column_[i]=std::clamp(item,0,columnCount_[i]-1);inPane_[i]=pane;
    }
    void intoPane(Page page,int item) noexcept {place(page,item,true);if(valid(page) && index(page)==tab_)onStrip_=false;}
    Answer command(Command command,bool repeat,bool paneEnterable,bool unsaved) noexcept {
        const bool before=onStrip_;const auto answer=handle(command,repeat,paneEnterable,unsaved);
        wentToStrip_=onStrip_ && !before;return answer;
    }
    Answer tapTab(Page target,bool unsaved) noexcept {
        wentToStrip_=false;
        if(!valid(target))return Answer::Nothing;
        if(index(target)==tab_) {
            if(!onStrip_)return Answer::Nothing;
            onStrip_=false;return Answer::Moved;
        }
        const auto answer=changeTab(index(target),unsaved);
        if(answer==Answer::Tab)onStrip_=false;
        return answer;
    }
    Answer tapColumn(int item,bool paneEnterable) noexcept {
        wentToStrip_=false;
        if(item<0 || item>=columnCount_[tab_])return Answer::Nothing;
        if(level()==Level::Column && column_[tab_]==item) {
            if(!paneEnterable)return Answer::Activate;
            inPane_[tab_]=true;return Answer::Entered;
        }
        onStrip_=false;inPane_[tab_]=false;column_[tab_]=item;return Answer::Moved;
    }
    Answer leaveGranted() noexcept {
        const auto target=pendingTab_;const bool strip=pendingStrip_;leaveCancelled();wentToStrip_=false;
        if(target>=0) {tab_=target;return Answer::Tab;}
        if(strip) {wentToStrip_=true;onStrip_=true;return Answer::Moved;}
        return Answer::Nothing;
    }
    void leaveCancelled() noexcept {pendingTab_=-1;pendingStrip_=false;}
    static int route(Page page,Command command,bool settingsCapturing,bool settingsReady,bool playAsking) noexcept {
        if(page==Page::Settings && (settingsCapturing || (settingsReady && (command==Command::Menu || command==Command::Alternate))))return 1;
        if(page==Page::Play && playAsking && command==Command::Back)return 2;
        return 0;
    }
    static bool endsHold(Answer answer,Command command) noexcept {return answer==Answer::Tab && !direction(command);}
private:
    static int index(Page page) noexcept {return static_cast<int>(page);}
    static bool valid(Page page) noexcept {return index(page)>=0 && index(page)<Pages;}
    static bool direction(Command command) noexcept {return command>=Command::Up && command<=Command::Right;}
    static int wrapped(int target,bool repeat) noexcept {
        if(target>=0 && target<Pages)return target;
#if defined(APPLE_LAUNCHER_NAV_MUTANT_REPEAT_WRAP)
        (void)repeat;return (target+Pages)%Pages;
#else
        return repeat?-1:(target+Pages)%Pages;
#endif
    }
    Answer changeTab(int target,bool unsaved) noexcept {
        if(target<0 || target>=Pages || target==tab_)return Answer::Nothing;
#if !defined(APPLE_LAUNCHER_NAV_MUTANT_LOSE_DIRTY)
        if(tab_==index(Page::Settings) && unsaved) {pendingTab_=target;pendingStrip_=false;return Answer::Ask;}
#else
        (void)unsaved;
#endif
        tab_=target;return Answer::Tab;
    }
    Answer handle(Command command,bool repeat,bool paneEnterable,bool unsaved) noexcept {
#if defined(APPLE_LAUNCHER_NAV_MUTANT_LOSE_HOLD)
        const bool stillHeld=false;
#else
        const bool stillHeld=repeat && command==enteredBy_;
#endif
        if(!repeat && direction(command))enteredBy_=Command::None;
        if(command==Command::PreviousPage)return changeTab(wrapped(tab_-1,repeat),unsaved);
        if(command==Command::NextPage)return changeTab(wrapped(tab_+1,repeat),unsaved);
        if(onStrip_) {
            if(stillHeld)return Answer::Nothing;
            if(command==Command::Left)return changeTab(wrapped(tab_-1,repeat),unsaved);
            if(command==Command::Right)return changeTab(wrapped(tab_+1,repeat),unsaved);
            if(command==Command::Down || command==Command::Confirm) {
                onStrip_=false;if(!paneEnterable)inPane_[tab_]=false;return Answer::Moved;
            }
            return Answer::Nothing;
        }
        if(!paneEnterable)inPane_[tab_]=false;
        if(stillHeld)return Answer::Nothing;
        if(inPane_[tab_]) {
            if(command!=Command::Back)return Answer::PaneCommand;
            inPane_[tab_]=false;return Answer::Moved;
        }
        switch(command) {
        case Command::Up:
            if(column_[tab_]>0) {--column_[tab_];return Answer::Moved;}
            if(repeat)return Answer::Nothing;
            onStrip_=true;return Answer::Moved;
        case Command::Down:
            if(column_[tab_]<columnCount_[tab_]-1) {++column_[tab_];return Answer::Moved;}
            if(repeat || columnCount_[tab_]<2)return Answer::Nothing;
            column_[tab_]=0;return Answer::Moved;
        case Command::Confirm:case Command::Right:
            if(paneEnterable) {inPane_[tab_]=true;if(command==Command::Right)enteredBy_=command;return Answer::Entered;}
            return command==Command::Confirm?Answer::Activate:Answer::Nothing;
        case Command::Back:
            if(tab_==index(Page::Settings) && unsaved) {pendingTab_=-1;pendingStrip_=true;return Answer::Ask;}
            onStrip_=true;return Answer::Moved;
        default:return Answer::Nothing;
        }
    }
    int tab_=0;
    bool onStrip_=false;
    std::array<int,Pages> column_{},columnCount_{1,1,1,1};
    std::array<bool,Pages> inPane_{};
    int pendingTab_=-1;
    bool pendingStrip_=false,wentToStrip_=false;
    Command enteredBy_=Command::None;
};
}
