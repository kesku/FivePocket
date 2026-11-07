// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <string_view>
namespace recomp::apple {
// Explicit diagnostic accessory. Connecting is not a button press or gameplay.
class VirtualControllerStart final {
    bool requested_=false,invalid_=false,pending_=false,attempted_=false,connected_=false,closed_=false;
public:
    explicit VirtualControllerStart(std::span<const std::string_view> args={}) noexcept {
        unsigned count=0;for(auto arg:args)if(arg=="--virtual-controller")++count;
        requested_=count==1;invalid_=count>1;
    }
    bool begin(bool active,bool terminal) noexcept {
        if(!requested_ || invalid_ || attempted_ || closed_ || !active || terminal)return false;
        attempted_=pending_=true;return true;
    }
    bool finish(bool success,bool active,bool terminal) noexcept {
        if(!pending_ || closed_)return false;
        pending_=false;connected_=success && active && !terminal;return connected_;
    }
    bool ready() const noexcept {
#if defined(APPLE_PORT_MUTANT_VIRTUAL_READY)
        return !closed_;
#else
        return !closed_ && !invalid_ && (!requested_ || connected_);
#endif
    }
    void close() noexcept {closed_=true;connected_=false;pending_=false;}
};
}
