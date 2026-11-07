#pragma once
#include "native_graphics_state.h"
#include <functional>
#include <map>
#include <optional>
#include <span>

namespace recomp::gpu {
// Original823CCEB8 emits WAIT_UNTIL(0x20000), optionally followed by the
// E00/E40 pair when device byte11072 has bit0x40. Exact hardware cache subset
// is not established. The native policy orders the real queue and discards ALL
// derived bindings for invalidating profiles, preserving raw programs and native
// image contents. Also supports original824F9C98's exact coherency transaction
// and unpredicated CACHE_FLUSH events; neither is an already-satisfied no-op.
// Caller must own fresh input snapshots and non-aliased native output storage;
// this is not a guest-memory flush, general PM4 executor or ring receipt.
class NativeTileCacheBarrier final {
public:
    using Action=std::function<bool()>;
    using Journal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
    enum class Phase {Ready,Ordering,Evicting,Complete,Failed};
    enum class Kind {Wait,IdleCachePair,HostCoherency,CacheFlush};
    static std::optional<NativeTileCacheBarrier> parse(std::span<const std::uint8_t> bytes) noexcept {
        const auto exact=[&](const auto& expected) {
            if(bytes.size()!=sizeof(expected))return false;
            for(unsigned i=0;i<bytes.size();++i)if(bytes[i]!=expected[i])return false;
            return true;
        };
        constexpr std::uint8_t coherency[]{0,0,0xA,0x31,0,1,0,0,0,1,0xA,0x2F,0,0,0,0,0,0,0x10,0,
            0xC0,4,0x3C,0,0,0,0,3,0,0,0xA,0x31,0,0,0,0,0x80,0,0,0,0,0,0,8};
        constexpr std::uint8_t flush[]{0xC0,0,0x46,0,0,0,0,6};
        if(exact(coherency))return NativeTileCacheBarrier(Kind::HostCoherency);
        if(exact(flush))return NativeTileCacheBarrier(Kind::CacheFlush);
        constexpr std::uint8_t expected[]{0,0,5,0xC8,0,2,0,0,0,0,0xE,0,0,0,0,1,0,0,0xE,0x40,0,0,0,1};
        if(bytes.size()!=8 && bytes.size()!=24)return std::nullopt;
        for(unsigned i=0;i<bytes.size();++i)if(bytes[i]!=expected[i])return std::nullopt;
        return NativeTileCacheBarrier(bytes.size()==24?Kind::IdleCachePair:Kind::Wait);
    }
    Kind kind() const noexcept{return kind_;}
    bool cachePair() const noexcept{return kind_==Kind::IdleCachePair;}
    bool waitUntil() const noexcept{return kind_==Kind::Wait || cachePair();}
    bool hostCoherency() const noexcept{return kind_==Kind::HostCoherency;}
    bool cacheFlush() const noexcept{return kind_==Kind::CacheFlush;}
    bool execute(NativeGraphicsState& state,Journal& journal,const Action& fence,const Action& evict) {
        if(phase_!=Phase::Ready)return fail();
        phase_=Phase::Failed;
        if(!fence || ((cachePair() || hostCoherency()) && !evict) ||
           (cachePair() && state.experimentalIdleCachePairEpoch==UINT64_MAX))return false;
        const auto epoch=state.experimentalIdleCachePairEpoch;
        auto expected=journal;auto completed=journal;
        if(waitUntil())completed[0x5C8]={0x20000,UINT32_MAX};
        if(cachePair()){completed[0xE00]={1,UINT32_MAX};completed[0xE40]={1,UINT32_MAX};}
        if(cacheFlush())completed[0x21F9]={6,UINT32_MAX};
        if(hostCoherency()) {
            // Register writes initiate work: reference WriteRegister(A31)
            // sets BUSY even though the incoming payload has bit31 clear.
            // Failure leaves this request dirty, never a completed status.
            expected[0xA31]={0x80010000,UINT32_MAX};expected[0xA2F]={0,UINT32_MAX};expected[0xA30]={0x1000,UINT32_MAX};
            completed=expected;completed[0xA31]={0,UINT32_MAX};
            auto staged=expected;journal.swap(staged);
        }
        try {
            phase_=Phase::Ordering;
            if(!fence() || phase_!=Phase::Ordering || state.experimentalIdleCachePairEpoch!=epoch || journal!=expected)return fail();
            if(cachePair() || hostCoherency()) {
                phase_=Phase::Evicting;
                if(!evict() || phase_!=Phase::Evicting || state.experimentalIdleCachePairEpoch!=epoch || journal!=expected)return fail();
                if(cachePair())++state.experimentalIdleCachePairEpoch;
            }
            journal.swap(completed);phase_=Phase::Complete;return true;
        } catch(...) {fail();throw;}
    }
    Phase phase() const noexcept{return phase_;}
private:
    explicit NativeTileCacheBarrier(Kind kind):kind_(kind){}
    bool fail() noexcept{phase_=Phase::Failed;return false;}
    Kind kind_;
    Phase phase_=Phase::Ready;
};
}
