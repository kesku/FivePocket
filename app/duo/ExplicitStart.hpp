// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <string_view>

namespace recomp::apple {
// Diagnostic opt-in, not a default launch mode. Same preparation handler as UI.
class ExplicitStart final {
    bool requested_=false,consumed_=false;
public:
    explicit ExplicitStart(std::span<const std::string_view> arguments={}) noexcept {
        unsigned count=0;
        for(auto arg:arguments)if(arg=="--start-recorded-test")++count;
        requested_=count==1;
    }
    bool consume(bool active,bool terminal) noexcept {
        if(!requested_ || consumed_ || !active || terminal)return false;
#if !defined(APPLE_PORT_MUTANT_REPEAT_START)
        consumed_=true;
#endif
        return true;
    }
};
}
