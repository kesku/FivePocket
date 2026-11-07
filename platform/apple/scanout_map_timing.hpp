// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdint>
#include <utility>
namespace scanout_map_timing {
struct Counts {std::uint64_t calls=0,ok=0,fail=0,ns=0,invalid=0;};
struct Snapshot {Counts total,interval;unsigned width=0,height=0;bool dimensionsConsistent=true;bool intervalConsistent=true;};
class Stats {
    Counts total_{},interval_{};unsigned width_=0,height_=0;bool dimensionsConsistent_=true;
public:
    static constexpr std::uint64_t Cadence=120;
    bool record(bool ok,bool valid,std::uint64_t ns,unsigned width,unsigned height) noexcept {
        if(!interval_.calls){width_=width;height_=height;dimensionsConsistent_=width!=0&&height!=0;}
        else dimensionsConsistent_&=width_==width&&height_==height;
        for(auto* c:{&total_,&interval_}){++c->calls;if(ok)++c->ok;else ++c->fail;if(valid)c->ns+=ns;else ++c->invalid;}
        return interval_.calls==Cadence;
    }
    Snapshot snapshot() const noexcept {return {total_,interval_,width_,height_,dimensionsConsistent_,dimensionsConsistent_&&!interval_.invalid&&interval_.calls==interval_.ok+interval_.fail};}
    void resetInterval() noexcept {interval_={};width_=height_=0;dimensionsConsistent_=true;}
};
template<class Clock=std::chrono::steady_clock,class Map,class Report>
bool measure(Stats& stats,unsigned width,unsigned height,Map&& map,Report&& report) {
    const auto begin=Clock::now();
    const bool mapped=std::forward<Map>(map)();
    const auto end=Clock::now();
    const bool valid=end>=begin;
    const auto elapsed=valid?std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count():0;
    if(stats.record(mapped,valid,static_cast<std::uint64_t>(elapsed),width,height)) {
        const auto snapshot=stats.snapshot();stats.resetInterval();
        std::forward<Report>(report)(snapshot);
    }
    return mapped;
}
}
