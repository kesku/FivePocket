#pragma once
#include "native_graphics_state.h"
#include <functional>
#include <optional>
#include <span>

namespace recomp::gpu {
// Original823C2738 emits canonical INVALIDATE_STATE(0x100) before loading the
// copy vertex program. Evicting ALL derived bindings is a conservative superset
// of that selection, not permission to clear raw registers/programs or images.
// This operation neither executes IM_LOAD nor completes a scene or ring entry.
class NativeTileStateInvalidation final {
public:
    using Action=std::function<bool()>;
    enum class Phase {Ready,Ordering,Evicting,Complete,Failed};
    static std::optional<NativeTileStateInvalidation> parse(std::span<const std::uint8_t> bytes) noexcept {
        constexpr std::uint8_t expected[]{0xC0,0,0x3B,0,0,0,1,0};
        if(bytes.size()!=sizeof(expected))return std::nullopt;
        for(unsigned i=0;i<sizeof(expected);++i)if(bytes[i]!=expected[i])return std::nullopt;
        return NativeTileStateInvalidation{};
    }
    bool execute(NativeGraphicsState& state,const Action& fence,const Action& evictAllDerived) {
        if(phase_!=Phase::Ready)return fail();
        phase_=Phase::Failed;
        if(!fence || !evictAllDerived || state.invalidationEpoch==UINT64_MAX)return false;
        const auto before=state.invalidationEpoch;
        try {
            phase_=Phase::Ordering;
            if(!fence() || phase_!=Phase::Ordering || state.invalidationEpoch!=before)return fail();
            phase_=Phase::Evicting;
            if(!evictAllDerived() || phase_!=Phase::Evicting || state.invalidationEpoch!=before)return fail();
            ++state.invalidationEpoch;phase_=Phase::Complete;return true;
        } catch(...) {fail();throw;}
    }
    Phase phase() const noexcept{return phase_;}
private:
    NativeTileStateInvalidation()=default;
    bool fail() noexcept{phase_=Phase::Failed;return false;}
    Phase phase_=Phase::Ready;
};
}
