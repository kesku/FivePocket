// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
namespace renderwait {
inline bool requested(){const auto*s=std::getenv("RECOMP_IOS_RENDER_WAIT_TIMING");if(!s||!std::strcmp(s,"0"))return false;if(!std::strcmp(s,"1"))return true;throw std::invalid_argument("RECOMP_IOS_RENDER_WAIT_TIMING must be 0 or 1");}
inline std::uint64_t now()noexcept{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
enum Phase:unsigned {Admission,QueueMutex,SubmitDriver,FenceWait,PostFence,Readback,PhaseCount};
struct Identity {std::uint64_t frame=0,command=0,fence=0;std::uint32_t slot=0;};
struct Stats {std::uint64_t count=0,ns=0,peak=0,begin=0,end=0,command=0,fence=0;};
struct Store {
 static constexpr unsigned Capacity=256;
 Identity recording{};std::uint64_t submittedCommand=0;std::array<Stats,PhaseCount> phases{};unsigned events=0;std::uint64_t overflow=0,invalid=0;bool reported=false;
 void reset(Identity id)noexcept{*this=Store{};recording=id;}
 void add(Phase phase,Identity id,std::uint64_t begin,std::uint64_t end)noexcept{
  if(events++>=Capacity){++overflow;return;}
  if(phase>=PhaseCount||!id.frame||id.frame!=recording.frame||id.slot!=recording.slot||end<begin){++invalid;return;}
  auto&s=phases[phase];const auto n=end-begin;if(s.ns>UINT64_MAX-n||s.count==UINT64_MAX){++invalid;return;}
  ++s.count;s.ns+=n;if(n>=s.peak){s.peak=n;s.begin=begin;s.end=end;s.command=id.command;s.fence=id.fence;}
 }
};
// The completed CPU readback must be this exact recorded/fenced slot. Source
// sequence/generation come from the actual descriptor, never inferred from time.
inline bool sourceJoined(const Store&s,const Identity* completed,std::uint64_t sourceFrame,std::uint64_t presents,
 std::uint64_t sourceGeneration,std::uint64_t currentGeneration,bool fenceComplete,bool sync)noexcept {
 if(!completed||!sync||!fenceComplete||!sourceFrame||sourceFrame!=presents||!sourceGeneration||sourceGeneration!=currentGeneration||s.invalid||s.overflow)return false;
 const auto&f=s.phases[FenceWait];const auto&r=s.phases[Readback];
 return completed->frame==s.recording.frame&&completed->slot==s.recording.slot&&completed->command&&completed->command==s.submittedCommand&&
  completed->fence&&completed->fence==s.recording.fence&&f.count==1&&r.count==1&&
  f.command==completed->command&&r.command==completed->command&&f.fence==completed->fence&&r.fence==completed->fence;
}
// Exact original path when disabled, including no clock reads. Enabled path
// preserves admission -> shared queue mutex -> native ticket -> driver order.
// Timings are recorded only after releasing the queue mutex.
template<class Original,class Admit,class Note,class Submit,class Clock>
auto submit(bool enabled,Store&store,Identity id,std::mutex&mutex,Original original,Admit admit,Note note,Submit driver,Clock clock,int denied){
 if(!enabled)return original();
 const auto t0=clock();auto operation=admit();const auto t1=clock();
 if(!operation){store.add(Admission,id,t0,t1);return denied;}
 std::uint64_t t2=0,t3=0,t4=0;decltype(driver()) result;
 {std::lock_guard lock(mutex);t2=clock();note();t3=clock();result=driver();t4=clock();}
 store.add(Admission,id,t0,t1);store.add(QueueMutex,id,t1,t2);store.add(SubmitDriver,id,t3,t4);return result;
}
template<class Clock=decltype(&now)>class Span final {
 Store*store_;Identity id_;Phase phase_;Clock clock_;std::uint64_t start_=0;
public:
 Span(bool enabled,Store&store,Identity id,Phase phase,Clock clock=&now)noexcept:store_(enabled?&store:nullptr),id_(id),phase_(phase),clock_(clock){if(store_)start_=clock_();}
 ~Span(){if(store_)store_->add(phase_,id_,start_,clock_());}
 Span(const Span&)=delete;Span&operator=(const Span&)=delete;
};
struct Reporter {
 unsigned slow=0,total=0;static constexpr unsigned SlowCap=32,TotalCap=128;
 bool select(Store&store)noexcept{
  if(store.reported||total>=TotalCap)return false;
  const bool periodic=store.recording.frame&&store.recording.frame%60==0;
  const bool large=store.phases[FenceWait].ns>=50'000'000;
  if(!periodic&&(!large||slow>=SlowCap))return false;
  store.reported=true;++total;if(!periodic)++slow;return true;
 }
};
}
