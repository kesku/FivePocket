// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <stdexcept>
namespace recomp::apple::presenter_timing {
enum class Phase:unsigned{Admission,SourcePrepare,Acquire,Record,Submit,Fence,SourceCompare,DestinationCompare,Present,Idle,SourceSubmit,SourceFence,Count};
inline constexpr const char* names[]={"admission","source_prepare","acquire","record","submit","fence","src_compare","dst_compare","present","idle","src_submit","src_fence"};
using Reader=bool(*)(void*,std::uint64_t&) noexcept;
inline int policy(const char*p) noexcept {if(!p||!std::strcmp(p,"0"))return 0;if(!std::strcmp(p,"1"))return 1;return -1;}
inline bool enabled(){const auto p=policy(std::getenv("RECOMP_IOS_PRESENTER_TIMING"));if(p<0)throw std::invalid_argument("RECOMP_IOS_PRESENTER_TIMING must be 0 or 1");return p==1;}
struct Segment{Phase phase;std::uint64_t begin,end;bool valid;};
class Snapshot {
 Reader reader_=nullptr;void* context_=nullptr;std::array<Segment,64> segments_;unsigned count_=0;bool invalid_=false;
 bool read(std::uint64_t&v)noexcept{return reader_&&reader_(context_,v);}
public:
 std::uint32_t sequence=0;std::uint64_t begin=0,end=0,other=0,mask=0;std::array<std::uint64_t,unsigned(Phase::Count)> ns{};bool finished=false,valid=false;
 Snapshot(std::uint32_t seq,Reader r=nullptr,void*c=nullptr)noexcept:reader_(r),context_(c),sequence(seq){if(reader_&&!read(begin))invalid_=true;}
 bool active()const noexcept{return reader_!=nullptr;}
 template<class F> auto measure(Phase phase,F&&f) noexcept(noexcept(std::forward<F>(f)())) {
  if(!active())return std::forward<F>(f)();
  std::uint64_t a=0,b=0;bool va=read(a);
  if constexpr(std::is_void_v<std::invoke_result_t<F>>) {std::forward<F>(f)();const bool vb=read(b);add(phase,a,b,va&&vb);}
  else {auto result=std::forward<F>(f)();const bool vb=read(b);add(phase,a,b,va&&vb);return result;}
 }
 void add(Phase p,std::uint64_t a,std::uint64_t b,bool ok)noexcept{if(finished||count_==segments_.size()){invalid_=true;return;}segments_[count_++]={p,a,b,ok};}
 void finish()noexcept{
  if(finished)return;finished=true;if(!active())return;if(!read(end))invalid_=true;
  std::uint64_t previous=begin,total=0;
  for(unsigned i=0;i<count_;++i){const auto&s=segments_[i];const auto p=unsigned(s.phase);if(!s.valid||p>=unsigned(Phase::Count)||s.begin<previous||s.end<s.begin||s.end>end){invalid_=true;continue;}previous=s.end;const auto d=s.end-s.begin;if(d>UINT64_MAX-total||d>UINT64_MAX-ns[p]){invalid_=true;continue;}total+=d;ns[p]+=d;mask|=std::uint64_t(1)<<p;}
  valid=!invalid_&&end>=begin&&total<=end-begin;if(valid)other=end-begin-total;
 }
 void write(FILE*out,const char*prefix,const char*status)noexcept{
  if(!active())return;finish();char text[2048];int n=std::snprintf(text,sizeof(text),"%s timing=1 timing_clock=mct timing_status=%s timing_valid=%u timing_sequence=%u timing_mask=%llu begin_ns=%llu end_ns=%llu other_ns=%llu",prefix,status,unsigned(valid),sequence,(unsigned long long)mask,(unsigned long long)begin,(unsigned long long)end,(unsigned long long)other);
  if(n<0||size_t(n)>=sizeof(text))return;size_t used=size_t(n);
  for(unsigned p=0;p<unsigned(Phase::Count);++p){n=std::snprintf(text+used,sizeof(text)-used," %s_ns=%llu",names[p],(unsigned long long)ns[p]);if(n<0||size_t(n)>=sizeof(text)-used)return;used+=size_t(n);}
  if(used+1<sizeof(text)){text[used++]='\n';std::fwrite(text,1,used,out);}
 }
};
template<class F>auto measure(Snapshot*s,Phase p,F&&f)noexcept(noexcept(std::forward<F>(f)())){if(s)return s->measure(p,std::forward<F>(f));return std::forward<F>(f)();}
struct Partial {Snapshot&snapshot;~Partial(){if(snapshot.active()&&!snapshot.finished)snapshot.write(stderr,"APPLE_IOS_PRESENTER_TIMING_PARTIAL","failed-or-unreached");}};
}
