// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "three_source_mailbox.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <exception>
#include <stdexcept>
#include <type_traits>
namespace recomp::ios_async_shared {
template<class T>std::uint64_t handleBits(T h)noexcept{if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(h);else return static_cast<std::uint64_t>(h);}
// Declare before leases and locks: their destructors run before terminal action.
// Default-off exceptions propagate unchanged; production fatal callback never returns.
template<class Enabled,class Fatal>class AdoptedUnwindGuard {
 Enabled enabled_;Fatal fatal_;int initial_=std::uncaught_exceptions();
public:
 AdoptedUnwindGuard(Enabled enabled,Fatal fatal):enabled_(enabled),fatal_(fatal){}
 ~AdoptedUnwindGuard()noexcept{if(std::uncaught_exceptions()>initial_ && enabled_())fatal_();}
};
// Renderer-owned allocations. The mailbox never destroys them; retirement is
// explicit only after dispatcher drain and a successful actual device drain.
template<class Image>class SourcePool {
 std::array<Image,3> images_{};std::shared_ptr<sharedsource::ThreeSourceMailbox> mailbox_;
 std::uint32_t width_=0,height_=0;std::uint64_t epoch_=0;
public:
 template<class Make,class Destroy>void ensure(std::uint32_t w,std::uint32_t h,std::uint64_t epoch,Make make,Destroy destroy){
  if(mailbox_){if(w!=width_||h!=height_||epoch!=epoch_)throw std::runtime_error("async source pool shape/epoch changed");return;}
  if(!w||!h||!epoch)throw std::runtime_error("async source pool invalid shape/epoch");
  unsigned made=0;
  try{
   for(;made<3;++made)if(!make(images_[made],w,h))throw std::runtime_error("async source image allocation failed");
   std::array<sharedsource::Resource,3> rs;
   for(unsigned i=0;i<3;++i)rs[i]={handleBits(images_[i].image),handleBits(images_[i].view),handleBits(images_[i].memory),epoch,w,h};
   mailbox_=std::make_shared<sharedsource::ThreeSourceMailbox>(rs);width_=w;height_=h;epoch_=epoch;
  }catch(...){for(auto&i:images_){if(handleBits(i.image)||handleBits(i.view)||handleBits(i.memory))destroy(i);i={};}throw;}
 }
 std::shared_ptr<sharedsource::ThreeSourceMailbox> mailbox()const{return mailbox_;}
 Image& select(const sharedsource::ThreeSourceMailbox::Producer&lease){
  const auto t=lease.token();if(!mailbox_||t.slot>=3||t.epoch!=epoch_)throw std::runtime_error("async source lease rejected");auto&r=images_[t.slot];const auto proof=lease.resource();
  if(proof.image!=handleBits(r.image)||proof.view!=handleBits(r.view)||proof.memory!=handleBits(r.memory))throw std::runtime_error("async source resource mismatch");return r;
 }
 std::uint64_t bytes()const noexcept{std::uint64_t n=0;for(auto&i:images_)n+=i.bytes;return n;}
 template<class Drain,class Destroy>bool retire(Drain drain,Destroy destroy){
  if(!mailbox_)return true;mailbox_->close();auto ready=mailbox_->retire(epoch_,[&](auto const&,auto){return drain();});if(!ready)return false;
  for(auto&i:images_){destroy(i);i={};}mailbox_.reset();return true;
 }
};
}
