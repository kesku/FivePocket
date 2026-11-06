#pragma once
#include "gta5_tile_deferred_tail.h"
#include "native_graphics_state.h"
#include <functional>
#include <map>

namespace recomp::gpu {
// Executes original82309890's exact tail after the native scene's resolves.
// The caller retains command/output/fence ownership and excludes guest writers.
// Invalidate must discard derived bindings (not raw registers or image contents).
// Publish must atomically validate ALL outputs, then store textures, cursor and
// completion ID in that order. This does not complete omitted scene PM4 or RPTR.
class NativeTileDeferredTail final {
public:
    using Journal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
    using Fence=std::function<bool()>;
    using Invalidate=std::function<bool(std::uint32_t)>;
    using Publish=std::function<bool(const NativeWordUpdatePlan&)>;
    enum class Phase {Ready,Ordering,Invalidating,Publishing,Complete,Failed};
    explicit NativeTileDeferredTail(const Gta5TileDeferredTail& plan):plan_(plan) {}
    bool execute(NativeGraphicsState& state,Journal& journal,const Fence& fence,
            const Invalidate& invalidate,const Publish& publish) {
        if(phase_!=Phase::Ready)return fail();
        phase_=Phase::Failed;
        if(!fence || !invalidate || !publish || !plan_.fencePhysical() || state.invalidationEpoch==UINT64_MAX)return false;
        auto prepared=journal;
        // VGT_EVENT_INITIATOR is0x21F9 on Xenos, NOT the desktop Radeon index.
        prepared.try_emplace(0x21F9,std::make_pair(0u,0u));
        if(plan_.requiresWaitUntil())prepared.try_emplace(0x5C8,std::make_pair(0u,0u));
        journal.swap(prepared);
        const auto updates=plan_.stagingPlan();
        try {
            // Conservative queue fence also covers the NOP profile's two
            // shader-done events. Never publish based only on a CPU counter.
            phase_=Phase::Ordering;
            if(!fence() || phase_!=Phase::Ordering)return fail();
            ++nativeFences_;
            phase_=Phase::Invalidating;
            if(!invalidate(0x7FFF) || phase_!=Phase::Invalidating)return fail();
            ++state.invalidationEpoch;
            if(plan_.requiresWaitUntil())journal.find(0x5C8)->second={0x20000,UINT32_MAX};
            // Both original EVENT_WRITE_SHD packets have initiator3. Exclusive
            // publication makes their identical retained register writes one
            // observable final value; cursor still precedes completion memory.
            journal.find(0x21F9)->second={3,UINT32_MAX};
            phase_=Phase::Publishing;
            if(!publish(updates) || phase_!=Phase::Publishing)return fail();
            phase_=Phase::Complete;return true;
        } catch(...) {fail();throw;}
    }
    Phase phase() const noexcept{return phase_;}
    unsigned nativeFences() const noexcept{return nativeFences_;}
private:
    bool fail() noexcept{phase_=Phase::Failed;return false;}
    Gta5TileDeferredTail plan_;
    Phase phase_=Phase::Ready;
    unsigned nativeFences_=0;
};
}
