// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentIngress.hpp"
#include <span>
#include <string_view>

namespace recomp::apple::agentrun {
namespace ai=agentinput;
namespace ag=agentingress;
enum class Launch : std::uint8_t { Disabled,Enabled,Invalid };
inline Launch launch(std::span<const std::string_view> arguments,bool normalMode,bool measurement) noexcept {
    unsigned enable=0,input=0;
    for(auto argument:arguments) {
        enable+=argument=="--agent-authorize-run-v1";
        input+=argument=="--agent-input-v1";
    }
    if(!enable)return Launch::Disabled;
    return enable==1 && input==1 && normalMode && !measurement?Launch::Enabled:Launch::Invalid;
}
struct Identities {ai::Nonce run{},menu{},guest{};};
enum class Phase : std::uint8_t { Disabled,PendingMenu,Menu,Guest,Closed };

// Main-owner only. Identities come from the app's launch adapter, never the
// network. This grant authorizes a run/stage; it does not attest image review.
// Reads use the actual Record and source. No clock, image, ACK, stage reset,
// allocation, or automatic retry occurs here. Referents are not retained.
class Authorization final {
public:
    bool enable() noexcept {
        if(!owner() || phase_!=Phase::Disabled)return false;
        phase_=Phase::PendingMenu;return true;
    }
    template<class Source> bool prepare(const Identities& ids,Source& source) noexcept {
        if(!owner() || phase_!=Phase::PendingMenu || prepared_)return false;
        const auto status=source.agentStatus();
        if(!ai::validNonce(ids.run) || !ai::validNonce(ids.menu) || !ai::validNonce(ids.guest) ||
           ids.run==ids.menu || ids.run==ids.guest || ids.menu==ids.guest || !status.manualNeutral) {
            close();return false;
        }
        identities_=ids;ownerGeneration_=status.ownerGeneration;prepared_=true;return true;
    }
    template<class Source> bool enterMenu(agentenvironment::Record& record,Source& source) noexcept {
        if(!owner() || phase_!=Phase::PendingMenu || !prepared_)return false;
        ai::Environment e{};
        if(!record.read(e))return false;
        if(!checkOwner(source) || !e.active || e.stage!=ai::Stage::Menu || !e.stageGeneration) {
            close();return false;
        }
        grant_={identities_.run,e.stage,e.stageGeneration,{identities_.menu,1}};
        phase_=Phase::Menu;return true;
    }
    template<class Source> bool advanceGuest(agentenvironment::Record& record,Source& source,bool consumedPlay) noexcept {
        if(!owner() || phase_!=Phase::Menu)return false;
        ai::Environment e{};
        if(!record.read(e))return false;
        if(!consumedPlay || !checkOwner(source) || !e.active || e.stage!=ai::Stage::Guest ||
           grant_.stageGeneration==UINT64_MAX || e.stageGeneration!=grant_.stageGeneration+1) {
            close();return false;
        }
        grant_={identities_.run,e.stage,e.stageGeneration,{identities_.guest,2}};
        phase_=Phase::Guest;return true;
    }
    template<class Source> bool grant(agentenvironment::Record& record,Source& source,ag::Grant& out) noexcept {
        out={};
        if(!owner() || (phase_!=Phase::Menu && phase_!=Phase::Guest))return false;
        ai::Environment e{};
        if(!record.read(e))return false;
        if(!checkOwner(source) || !e.active || e.stage!=grant_.stage || e.stageGeneration!=grant_.stageGeneration) {
            close();return false;
        }
        out=grant_;return true;
    }
    template<class Source> bool checkOwner(Source& source) noexcept {
        if(!owner() || phase_==Phase::Closed)return false;
        if(phase_==Phase::Disabled)return true;
        const auto status=source.agentStatus();
        if(!prepared_ || !status.active || !status.manualNeutral || status.ownerGeneration!=ownerGeneration_) {
            close();return false;
        }
        return true;
    }
    bool close() noexcept {
        if(!owner())return false;
        if(phase_!=Phase::Disabled){phase_=Phase::Closed;grant_={};}
        return true;
    }
    Phase phase() const noexcept {return owner()?phase_:Phase::Closed;}
    ai::Nonce run() const noexcept {return owner() && prepared_?identities_.run:ai::Nonce{};}
private:
    bool owner() const noexcept {return std::this_thread::get_id()==owner_;}
    const std::thread::id owner_=std::this_thread::get_id();
    Phase phase_=Phase::Disabled;Identities identities_{};ag::Grant grant_{};
    std::uint64_t ownerGeneration_=0;bool prepared_=false;
};
}
