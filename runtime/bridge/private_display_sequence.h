#pragma once
#include "private_trace_api.h"
#include <cstdint>
#include <cstdlib>

namespace recomp::private_trace {
// Serialized caller only. Invalid ordering/shape is not a submitted attempt;
// a submitted rejection is terminal. A callback cannot advance while in flight.
class DisplaySequence final {
public:
    // Keep the sequence monotonic without a short diagnostic frame cap. The
    // reserved final value prevents wraparound and accidental replay of zero.
    static constexpr std::uint32_t Limit=UINT32_MAX;
    std::uint32_t next()const noexcept{return next_;}
    bool begin(std::uint32_t sequence)noexcept{
        if(failed_ || busy_ || sequence!=next_ || sequence>=Limit)return false;
        busy_=true;++next_;return true;
    }
    bool finish(bool accepted)noexcept{
        if(!busy_)return false;
        busy_=false;failed_=!accepted;return accepted;
    }
private:
    std::uint32_t next_=0;
    bool busy_=false,failed_=false;
};

// Own the callback configuration for the entire synchronous trace. The legacy
// internal pipeline does not need to know about window sequence numbering.
class DisplayCallbacksV4 final {
public:
    explicit DisplayCallbacksV4(const RecompPrivateTraceCallbacksV4& callbacks):callbacks_(callbacks){}
    int consume(const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height)noexcept{
        if(!rgba || bytes!=1280U*720U*4U || width!=1280 || height!=720)return 0;
        const auto sequence=sequence_.next();
        if(!sequence_.begin(sequence))return 0;
        const int result=callbacks_.orderedRgba(callbacks_.user,sequence,rgba,bytes,width,height);
        if(result!=0 && result!=1)std::_Exit(4);
        return sequence_.finish(result==1)?1:0;
    }
private:
    const RecompPrivateTraceCallbacksV4 callbacks_;
    DisplaySequence sequence_;
};
}
