// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "private_gamepad_observer_v1.h"
#include <dlfcn.h>

namespace recomp::apple::agentobserver {
enum class Result { Ok, Occupied, MissingEntry, MissingExport, UnresolvedImage,
    DifferentImage, Unresolved, InvalidOwner, InstallRefused, ClearRefused };

// App-side metadata ONLY: never links/creates a second engine observer. Main
// passes the address of the V5 entry it actually calls. iOS statically links
// that engine into the app, so there is no private-DSO dlopen handle to reuse.
// Image equality rejects another loaded engine; it does not prove object/link
// uniqueness within one image. The production artifact still needs that audit.
class Registration final {
public:
    using Setter=int(*)(RecompPrivateGamepadObserverV1,void*) noexcept;
    Registration()=default;
    Registration(const Registration&)=delete;
    Registration& operator=(const Registration&)=delete;
    Result resolve(const void* actualEntry) noexcept {
        if(installed_)return Result::Occupied;
        setter_=nullptr; // A failed new lookup cannot retain an old capability.
        Dl_info entry{},observer{};
        if(!actualEntry)return Result::MissingEntry;
        if(!dladdr(actualEntry,&entry) || !entry.dli_fbase)return Result::UnresolvedImage;
        void* symbol=dlsym(RTLD_DEFAULT,"recomp_private_trace_gamepad_observer_v1");
        if(!symbol)return Result::MissingExport;
        if(!dladdr(symbol,&observer) || !observer.dli_fbase)return Result::UnresolvedImage;
        if(entry.dli_fbase!=observer.dli_fbase)return Result::DifferentImage;
        setter_=reinterpret_cast<Setter>(symbol);
        return Result::Ok;
    }
    Result install(RecompPrivateGamepadObserverV1 callback,void* owner) noexcept {
        if(installed_)return Result::Occupied;
        if(!setter_)return Result::Unresolved;
        if(!callback || !owner)return Result::InvalidOwner;
        if(setter_(callback,owner)!=0)return Result::InstallRefused;
        installed_=true;
        return Result::Ok;
    }
    Result clear() noexcept {
        if(!setter_)return Result::Unresolved;
        if(!installed_)return Result::Ok;
        if(setter_(nullptr,nullptr)!=0)return Result::ClearRefused;
        installed_=false;
        return Result::Ok;
    }
    bool installed() const noexcept {return installed_;}
    // No automatic removal/unload on destruction, timeout, scene transition or
    // core exception. The borrowed owner MUST stay alive until clear succeeds
    // after verified normal joined completion (the existing immortal Session
    // is suitable). Failed clear retains installed metadata and ownership.
private:
    Setter setter_=nullptr;
    bool installed_=false;
};
}
