// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace resolve_identity {
inline bool parse(const char* v) {
 if(!v || std::strcmp(v,"0")==0)return false;
 if(std::strcmp(v,"1")==0)return true;
 throw std::invalid_argument("RECOMP_IOS_RESOLVE_IDENTITY must be 0 or 1");
}
struct Identity {std::uint64_t frame=0,command=0;std::uint32_t slot=0,chunk=0;};
inline bool same(Identity a,Identity b) noexcept {return a.frame==b.frame && a.command==b.command && a.slot==b.slot && a.chunk==b.chunk;}
struct Event {
 Identity begin{},end{};std::uint64_t source=0,destination=0,ns=0;
 std::uint32_t gap=0,srcW=0,srcH=0,dstW=0,dstH=0,cw=0,ch=0,mip=0,slice=0,mode=0,branch=0;
 std::int32_t srcFormat=0,dstFormat=0,sx=0,sy=0,dx=0,dy=0,exponent=0;
 bool complete=false,clockValid=false,joinValid=false;
};
inline bool gapJoined(const Event& e,std::uint32_t passCount) noexcept {return e.joinValid && e.gap>0 && e.gap<passCount;}
struct Store {
 static constexpr std::size_t Capacity=128;
 std::array<Event,Capacity> events{};
 std::uint64_t epoch=0,overflow=0,abandoned=0,late=0;
 std::uint32_t used=0;Identity recording{};
 void reset(Identity id) noexcept {
  for(std::uint32_t i=0;i<used;++i)if(!events[i].complete)++abandoned;
  used=0;overflow=0;recording=id;++epoch;
 }
};
template<class Clock,class Current> class Observation {
 Store* store_=nullptr;std::uint64_t epoch_=0,start_=0;std::uint32_t index_=0;
 Clock clock_;Current current_;bool finished_=false;
public:
 Observation(bool enabled,Store& s,std::uint32_t gap,Clock clock,Current current):clock_(std::move(clock)),current_(std::move(current)) {
  if(!enabled)return;
  if(s.used==Store::Capacity){++s.overflow;return;}
  store_=&s;epoch_=s.epoch;index_=s.used++;s.events[index_]={};
  auto& e=s.events[index_];e.begin=current_();e.gap=gap;start_=clock_();
 }
 Observation(const Observation&)=delete;
 ~Observation() noexcept {finish(false);}
 Event* event() noexcept {return store_ && store_->epoch==epoch_?&store_->events[index_]:nullptr;}
 void finish(bool complete) noexcept {
  if(finished_)return;finished_=true;if(!store_)return;
  if(store_->epoch!=epoch_){++store_->late;return;}
  auto& e=store_->events[index_];e.end=current_();const auto end=clock_();
  e.complete=complete;e.clockValid=end>=start_;if(e.clockValid)e.ns=end-start_;
  e.joinValid=same(e.begin,e.end) && e.begin.slot==store_->recording.slot && e.begin.frame==store_->recording.frame;
 }
};
}
