#pragma once
#include <cstddef>

// Diagnostic storage only, called under the renderer's exclusive owner gate.
// Normal history must never consume the slot or bytes needed by the first
// capturable exception. This policy does not admit or complete GPU work.
class BoundIndexedCaptureBudget {
public:
    static constexpr unsigned RoutineRecords=128;
    static constexpr std::size_t TotalBytes=32u*1024*1024;
    static constexpr std::size_t FailureReserve=4u*1024*1024;
    static constexpr std::size_t RoutineBytes=TotalBytes-FailureReserve;

    std::size_t available(unsigned sequence,bool exception) const noexcept {
        if(!sequence)return 0;
        if(exception)return failureRecorded_?0:FailureReserve;
        if(sequence>RoutineRecords)return 0;
        return RoutineBytes-routineBytes_;
    }
    bool retain(unsigned sequence,bool exception,std::size_t bytes) noexcept {
        if(!bytes || bytes>available(sequence,exception))return false;
        if(exception){failureBytes_=bytes;failureRecorded_=true;}
        else routineBytes_+=bytes;
        return true;
    }
    std::size_t retainedBytes() const noexcept{return routineBytes_+failureBytes_;}
    std::size_t failureBytes() const noexcept{return failureBytes_;}
private:
    std::size_t routineBytes_=0,failureBytes_=0;
    bool failureRecorded_=false;
};
