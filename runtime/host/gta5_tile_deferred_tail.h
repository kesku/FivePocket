#pragma once
#include "native_gpu_word_updates.h"
#include <optional>
#include <span>

namespace recomp::gpu {
// Original82309890's deferred post-scene tail. These two EVENT_WRITE_SHD words
// release command storage and complete resource fence IDs. Parsing never grants
// either effect. Caller must satisfy native rendering/resolve publication,
// derived-state invalidation and the optional WAIT_UNTIL before committing.
class Gta5TileDeferredTail final {
public:
    static constexpr unsigned Words=12,Bytes=48;
    static std::optional<Gta5TileDeferredTail> parse(unsigned physical,
            std::span<const std::uint8_t> bytes) noexcept {
        if(bytes.size()!=Bytes || physical%4 || std::uint64_t{physical}+Bytes>0x20000000)return std::nullopt;
        const auto word=[&](unsigned i) {
            const auto*p=bytes.data()+i*4;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        };
        const bool wait=word(2)==0x5C8 && word(3)==0x20000;
        if(word(0)!=0xC0003B00 || word(1)!=0x7FFF ||
           (!wait && (word(2)!=0xC0001000 || word(3)!=0x77777777)) ||
           word(4)!=0xC0025800 || word(5)!=3 || word(8)!=0xC0025800 || word(9)!=3 ||
           (word(6)&3)!=2 || (word(10)&3)!=2)return std::nullopt;
        const auto fence=word(10)&~3u,cursor=word(7);
        if(!fence || std::uint64_t{fence}+8>0x20000000 || word(6)!=fence+6 ||
           cursor<0xA0000000 || cursor>=0xC0000000 ||
           (std::uint64_t{fence}<std::uint64_t{physical}+Bytes && physical<std::uint64_t{fence}+8))return std::nullopt;
        Gta5TileDeferredTail result;result.fence_=fence;result.cursor_=cursor;
        result.value_=word(11);result.wait_=wait;
        return result;
    }
    unsigned fencePhysical() const noexcept {return fence_;}
    unsigned commandCursor() const noexcept {return cursor_;}
    unsigned completionValue() const noexcept {return value_;}
    bool requiresWaitUntil() const noexcept {return wait_;}
    // The caller resolves the CURRENT owned eight-byte fence allocation; no
    // physical-address masking or stale stopped ownership is inferred here.
    bool matchesProducedState(std::span<const std::uint8_t> state,unsigned fenceVirtual,
            unsigned resolvedFencePhysical) const noexcept {
        if(state.size()<15328 || resolvedFencePhysical!=fence_ || !fenceVirtual)return false;
        const auto word=[&](unsigned at) {
            const auto*p=state.data()+at;
            return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        };
        return word(11024)==fenceVirtual && word(11036)==std::uint32_t(value_+2) &&
            (word(13608)|(word(13612)&3))==cursor_;
    }
    NativeWordUpdatePlan stagingPlan() const noexcept {
        NativeWordUpdatePlan result;
        const NativeWordRequest requests[]{{4,cursor_,NativeWordPhase::AfterGraphics},
            {0,value_,NativeWordPhase::AfterGraphics}};
        (void)planNativeWordUpdates(requests,2,8,result);
        return result;
    }
private:
    unsigned fence_=0,cursor_=0,value_=0;bool wait_=false;
};
}
