// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "apple/launcher_settings_store.h"
#include "apple/launcher_display_options.h"
#include <array>
#include <pthread.h>

namespace recomp::apple::launcher {
// The integrating host must supply its captured ORIGINAL configuration and an
// explicit consumer capability mask. Nothing here invents Android defaults,
// discovers a game root, authenticates a request, or starts/stops a game.
struct SettingsPolicy {
    DisplayChoices original;
    std::array<std::uint64_t,10> allowed;
    bool permits(const DisplayChoices& choices) const noexcept {
        if(!valid(choices))return false;
        for(unsigned i=0;i<ChoiceCounts.size();++i)
            if(!(allowed[i]&(1ull<<choices.values[i])))return false;
        return true;
    }
    bool validPolicy() const noexcept {
        if(!permits(original))return false;
        for(unsigned i=0;i<ChoiceCounts.size();++i)
            if(allowed[i]&~((1ull<<ChoiceCounts[i])-1))return false;
        return true;
    }
};
enum class SettingsOwnership { Disabled, Menu, Verifying, Preparing, Guest };

// Main-thread-only producer: draft -> explicit durable Save -> cold revalidation
// -> one-way frozen publication, called ONLY inside authenticated start admission.
// Verification/preparation callbacks remain owned by the integrating host.
class LauncherSettingsOwner {
public:
    LauncherSettingsOwner(int directory,const SettingsPolicy& policy):store_(directory),policy_(policy),draft_(policy.original) {
        if(!main() || !policy_.validPolicy() || !store_.ready())return;
        const auto read=store_.load();
        if(read.status!=LoadStatus::Missing && read.status!=LoadStatus::Loaded)return;
        if(read.value && !policy_.permits(read.value->choices))return;
        stored_=read.value;draft_=savedChoices();ownership_=SettingsOwnership::Menu;
    }
    SettingsOwnership ownership() const noexcept {return ownership_;}
    DisplayChoices choices() const noexcept {return draft_;}
    const SettingsPolicy& policy() const noexcept {return policy_;}
    std::optional<StoredDisplay> saved() const noexcept {return stored_;}
    bool dirty() const noexcept {return draft_!=savedChoices();}
    bool change(unsigned field,unsigned value) noexcept {
        if(!main() || ownership_!=SettingsOwnership::Menu || field>=ChoiceCounts.size() || value>=ChoiceCounts[field])return false;
#if !defined(APPLE_LAUNCHER_OWNER_MUTANT_CAPABILITY)
        if(!(policy_.allowed[field]&(1ull<<value)))return false;
#endif
        draft_.values[field]=std::uint8_t(value);return true;
    }
    bool discard() noexcept {
        if(!main() || ownership_!=SettingsOwnership::Menu)return false;
        draft_=savedChoices();return true;
    }
    SavedSettings save() {
        if(!main() || ownership_!=SettingsOwnership::Menu || !policy_.permits(draft_))
            return {SaveStatus::Invalid,std::nullopt,0};
        const auto result=store_.save(stored_,draft_);
        if(result.status==SaveStatus::Saved && result.value && result.value->choices==draft_) {
            stored_=result.value;
#if defined(APPLE_LAUNCHER_OWNER_MUTANT_EARLY)
            publishSavedLauncherDisplayOptions(*stored_);freezeLauncherDisplayOptions();
#endif
        } else if(result.status==SaveStatus::CommittedUncertain || result.status==SaveStatus::Conflict ||
                  result.status==SaveStatus::Invalid || result.status==SaveStatus::Saved) {
            ownership_=SettingsOwnership::Disabled;
        }
        return result;
    }
    bool beginVerification() noexcept {return begin(SettingsOwnership::Verifying,false);}
    bool beginPreparation() noexcept {return begin(SettingsOwnership::Preparing,true);}
    bool returnToMenu() noexcept {
        if(!main() || (ownership_!=SettingsOwnership::Verifying && ownership_!=SettingsOwnership::Preparing))return false;
        ownership_=SettingsOwnership::Menu;return true;
    }
    void invalidate() noexcept {if(main())ownership_=SettingsOwnership::Disabled;}
    bool commitGuest(bool diagnostic) {
        if(!main() || ownership_!=SettingsOwnership::Preparing || dirty())return false;
        const auto current=store_.load();
        if((current.status!=LoadStatus::Missing && current.status!=LoadStatus::Loaded)
#if !defined(APPLE_LAUNCHER_OWNER_MUTANT_STALE)
           || current.value!=stored_
#endif
           ) {ownership_=SettingsOwnership::Disabled;return false;}
        bool published=false;
        if(diagnostic || !stored_)published=freezeLegacyLauncherDisplayOptions();
        else published=publishSavedLauncherDisplayOptions(*stored_) && freezeLauncherDisplayOptions();
        if(!published) {ownership_=SettingsOwnership::Disabled;return false;}
        ownership_=SettingsOwnership::Guest;return true;
    }
private:
    static bool main() noexcept {return pthread_main_np()!=0;}
    DisplayChoices savedChoices() const noexcept {return stored_?stored_->choices:policy_.original;}
    bool begin(SettingsOwnership next,bool clean) noexcept {
        if(!main() || ownership_!=SettingsOwnership::Menu)return false;
#if !defined(APPLE_LAUNCHER_OWNER_MUTANT_DIRTY)
        if(clean && dirty())return false;
#else
        (void)clean;
#endif
        ownership_=next;return true;
    }
    LauncherSettingsStore store_;
    SettingsPolicy policy_;
    DisplayChoices draft_;
    std::optional<StoredDisplay> stored_;
    SettingsOwnership ownership_=SettingsOwnership::Disabled;
};
}
