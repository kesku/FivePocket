// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ios_gpu_frame_bridge.h"
#include "three_source_mailbox.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif
namespace recomp::ios_async_shared {
// Internal only. Queued is neither public Accepted nor pixel completion.
enum class QueueResult:int {Terminal=-1,Queued=2};
inline bool requested(){const char*s=std::getenv("RECOMP_IOS_ASYNC_SHARED_PRESENT");if(!s||!std::strcmp(s,"0"))return false;if(!std::strcmp(s,"1"))return true;throw std::invalid_argument("RECOMP_IOS_ASYNC_SHARED_PRESENT must be 0 or 1");}
inline bool enabled=false; // immutable before renderer construction
inline std::atomic<bool> adopted{false};
using Mailbox=sharedsource::ThreeSourceMailbox;
using Enqueue=std::function<QueueResult(std::shared_ptr<Mailbox>,Mailbox::Producer&&,RecompIosGpuFrameV1,std::vector<std::uint8_t>&&)>;
// This object never owns/joins the worker. A Main-held snapshot can safely be
// last owner after shutdown; closed admission prevents captured callback use.
class Control final {
 std::mutex mutex_;std::condition_variable cv_;bool closed_=false;unsigned inFlight_=0;Enqueue enqueue_;
public:
 explicit Control(Enqueue enqueue):enqueue_(std::move(enqueue)){}
 QueueResult submit(std::shared_ptr<Mailbox> box,Mailbox::Producer&& producer,RecompIosGpuFrameV1 frame,std::vector<std::uint8_t>&&raw){
  {std::lock_guard lock(mutex_);if(closed_)return QueueResult::Terminal;++inFlight_;}
  QueueResult result=QueueResult::Terminal;
  try{result=enqueue_(std::move(box),std::move(producer),frame,std::move(raw));}catch(...){}
  {std::lock_guard lock(mutex_);--inFlight_;cv_.notify_all();}
  return result;
 }
 void close()noexcept{std::lock_guard lock(mutex_);closed_=true;cv_.notify_all();}
 void drain(){std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return !inFlight_;});}
};
// libc++ in this SDK has no atomic<shared_ptr> specialization. Only pointer
// publication uses this tiny mutex; callbacks/drain never run while held.
class ControlSlot final {
 std::mutex mutex_;std::shared_ptr<Control> value_;
public:
 std::shared_ptr<Control> load(){std::lock_guard lock(mutex_);return value_;}
 void store(std::shared_ptr<Control> p){std::lock_guard lock(mutex_);value_=std::move(p);}
 std::shared_ptr<Control> exchange(std::shared_ptr<Control> p){std::lock_guard lock(mutex_);value_.swap(p);return p;}
};
inline ControlSlot control;
inline QueueResult enqueueFrame(std::shared_ptr<Mailbox>box,Mailbox::Producer&&producer,RecompIosGpuFrameV1 frame,std::vector<std::uint8_t>&&raw){auto c=control.load();if(!c)return QueueResult::Terminal;return c->submit(std::move(box),std::move(producer),frame,std::move(raw));}
using RetireSources=bool(*)();
inline std::atomic<RetireSources> retireSources{nullptr};
inline std::atomic<bool> stopRequested{false};
inline void requestStop()noexcept{stopRequested.store(true,std::memory_order_release);if(auto c=control.load())c->close();}
struct Prepared final {
 Mailbox::Token token{};std::uint64_t sourceGeneration=0;RecompIosGpuFrameV1 frame{};std::vector<std::uint8_t> mapped;
};
// One mailbox per runtime device. Metadata is immutable while Pending/Active;
// a producer can replace only the descriptor of its new Producing slot.
class Dispatcher final {
 std::mutex mutex_;std::condition_variable cv_;bool closed_=false,wake_=false,consumerActive_=false;
 std::atomic<std::uint64_t> submitted_{0},completed_{0},cancelled_{0},failed_{0},publishedWhileActive_{0};
 std::shared_ptr<Mailbox> mailbox_;std::array<std::optional<Prepared>,3> prepared_;
 std::function<bool(RecompIosGpuFrameV1&,const std::vector<std::uint8_t>&,std::vector<std::uint8_t>&)> prepare_;
 std::function<int(RecompIosGpuFrameV1&)> present_;std::function<void()> terminal_;
 std::function<void(const RecompIosGpuFrameV1&,Mailbox::Token,bool,bool)> observe_;
 const std::thread::id owner_=std::this_thread::get_id();std::thread worker_;
 static bool same(Mailbox::Token a,Mailbox::Token b){return a.slot==b.slot&&a.epoch==b.epoch&&a.generation==b.generation;}
 static bool matches(const sharedsource::Resource&r,const RecompIosGpuFrameV1&f){return f.structureBytes==sizeof(f)&&f.version==1&&!f.reserved0&&!f.reserved1&&!f.reservedSequence&&f.sourceGeneration&&f.sourceFrame&&f.instance&&f.physicalDevice&&f.device&&f.queue&&f.sourceFenceComplete==1&&f.format==VK_FORMAT_R8G8B8A8_UNORM&&f.layout==VK_IMAGE_LAYOUT_GENERAL&&((f.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))==(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))&&f.width==1280&&f.height==720&&r.image==reinterpret_cast<std::uintptr_t>(f.image)&&r.view==reinterpret_cast<std::uintptr_t>(f.view)&&r.epoch==f.deviceEpoch&&r.width==f.width&&r.height==f.height;}
 void run()noexcept {
  for(;;){std::shared_ptr<Mailbox> box;
   {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return closed_||wake_;});if(closed_)return;if(stopRequested.load(std::memory_order_acquire)){lock.unlock();requestClose();return;}wake_=false;box=mailbox_;}
   if(!box)continue;
   auto active=box->take();if(!active)continue;
   bool ok=false;Prepared p;
   try {
    const auto token=active->token();const auto id=active->identity(token);const auto resource=active->resource();
    {std::lock_guard lock(mutex_);consumerActive_=true;if(token.slot>=prepared_.size()||!prepared_[token.slot]||!same(prepared_[token.slot]->token,token))throw std::logic_error("missing prepared source");p=std::move(*prepared_[token.slot]);prepared_[token.slot].reset();}
    if(id&&id->guestFrame==p.frame.sourceFrame&&p.sourceGeneration==p.frame.sourceGeneration&&matches(resource,p.frame)&&p.mapped.size()==std::uint64_t(resource.width)*resource.height*4) {
     p.frame.mappedRgba=p.mapped.data();p.frame.mappedBytes=p.mapped.size();
     ok=box->complete(std::move(*active),[&](auto r,auto t){return matches(r,p.frame)&&same(t,token)&&[&]{
       {std::lock_guard lock(mutex_);if(closed_||stopRequested.load(std::memory_order_acquire))return false;}
       return present_(p.frame)==RECOMP_IOS_GPU_ACCEPTED;
      }();});
    }
   }catch(...){ok=false;}
   // If not completed, dropping Active quarantines it. No CPU fallback or retry.
   if(!ok){
    bool cancelled;{std::lock_guard lock(mutex_);cancelled=closed_||stopRequested.load(std::memory_order_acquire);}
    requestClose();if(cancelled)++cancelled_;else ++failed_;
    if(observe_)try{observe_(p.frame,p.token,false,cancelled);}catch(...){}
    {std::lock_guard lock(mutex_);consumerActive_=false;}
    if(cancelled)return;try{terminal_();}catch(...){}return;
   }
   ++completed_;
   if(observe_)try{observe_(p.frame,p.token,true,false);}catch(...){}
   {std::lock_guard lock(mutex_);consumerActive_=false;}
   if(stopRequested.load(std::memory_order_acquire)){requestClose();return;}
   // Publications during the callback set wake_. The latest Pending owns its
   // own metadata and remains untouched by this completed Active slot.
  }
 }
public:
 struct Counts{std::uint64_t submitted,completed,cancelled,failed,publishedWhileActive,replaced,quarantined;};
 Counts counts() {std::shared_ptr<Mailbox>box;{std::lock_guard lock(mutex_);box=mailbox_;}auto c=box?box->counts():Mailbox::Counts{};return {submitted_.load(),completed_.load(),cancelled_.load(),failed_.load(),publishedWhileActive_.load(),c.replaced,c.quarantined};}
 using Prepare=decltype(prepare_);
 Dispatcher(Prepare prepare,std::function<int(RecompIosGpuFrameV1&)> present,std::function<void()> terminal,
  std::function<void(const RecompIosGpuFrameV1&,Mailbox::Token,bool,bool)> observe={})
 :prepare_(std::move(prepare)),present_(std::move(present)),terminal_(std::move(terminal)),observe_(std::move(observe)),worker_([this]{run();}){}
 Dispatcher(const Dispatcher&)=delete;Dispatcher&operator=(const Dispatcher&)=delete;
 QueueResult submit(std::shared_ptr<Mailbox>box,Mailbox::Producer&&producer,RecompIosGpuFrameV1 frame,std::vector<std::uint8_t>&&raw){
  // Runs on producer BEFORE publication: map/LUT may need producer-owned native
  // state. Worker performs no mapNativeScanout or registry/native-gate access.
  if(!box||!producer)return QueueResult::Terminal;
  const auto token=producer.token();Prepared p;p.token=token;p.sourceGeneration=frame.sourceGeneration;p.frame=frame;
  try {
   if(frame.sourceFenceComplete!=1||!matches(producer.resource(),frame)||raw.size()!=std::uint64_t(frame.width)*frame.height*4||!prepare_(p.frame,raw,p.mapped))return QueueResult::Terminal;
   auto proof=box->proveSource(producer,std::move(raw),[&](auto r,auto t,const auto&bytes){return same(t,token)&&matches(r,frame)&&frame.sourceFenceComplete==1&&bytes.size()==std::uint64_t(frame.width)*frame.height*4;});
   if(!proof)return QueueResult::Terminal;
   std::lock_guard lock(mutex_);
   if(closed_||stopRequested.load(std::memory_order_acquire)||(mailbox_&&mailbox_!=box)||token.slot>=prepared_.size())return QueueResult::Terminal;
   mailbox_=box;prepared_[token.slot]=std::move(p);
   if(!box->publish(std::move(producer),std::move(*proof),frame.sourceFrame)){prepared_[token.slot].reset();return QueueResult::Terminal;}
   ++submitted_;if(consumerActive_)++publishedWhileActive_;
   wake_=true;cv_.notify_one();return QueueResult::Queued;
  }catch(...){return QueueResult::Terminal;}
 }
 // Safe on Main: stop admission/cancel Pending, no join and no display mutex.
 // Active remains owned until the in-flight synchronous app callback returns.
 void requestClose()noexcept {std::shared_ptr<Mailbox>box;{std::lock_guard lock(mutex_);closed_=true;box=mailbox_;cv_.notify_all();}if(box)box->close();}
 // Only after executor joins, off Main, outside RendererMutex/native/source gate.
 void closeAndJoin(){
  if(worker_.joinable()){
   if(owner_!=std::this_thread::get_id() || worker_.get_id()==std::this_thread::get_id())std::terminate();
#if defined(__APPLE__)
   if(pthread_main_np())std::terminate();
#endif
  }
  requestClose();if(worker_.joinable())worker_.join();
 }
 ~Dispatcher(){closeAndJoin();}
};
}
