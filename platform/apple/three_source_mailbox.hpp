// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
namespace sharedsource {
// Owns three resource descriptors, but performs no Vulkan calls or allocation.
// A later adapter must move actual allocations with these handles and implement
// the verification callbacks from real fence/readback/drain results.
struct Resource {std::uint64_t image=0,view=0,memory=0,epoch=0;std::uint32_t width=0,height=0;};
class ThreeSourceMailbox {
public:
 enum class Phase {Free,Producing,Pending,Active,Quarantined,Retired};
 struct Token {std::uint32_t slot=0;std::uint64_t epoch=0,generation=0;};
 struct FrameIdentity {Token token;std::uint64_t guestFrame=0;};
 struct Counts {std::uint64_t published=0,replaced=0,accepted=0,quarantined=0;};
private:
 struct Slot {Resource resource;Phase phase=Phase::Free;std::uint64_t generation=0,frame=0;bool submitted=false,sourceComplete=false,live=false;std::vector<std::uint8_t> raw;};
 struct State {std::mutex mutex;std::array<Slot,3> slots;std::optional<unsigned> pending,active,producing;bool closed=false,retired=false;std::uint64_t lastFrame=0;Counts counts;};
 std::shared_ptr<State> state_;
 static bool matches(const State&s,Token t,Phase p){return t.slot<3 && s.slots[t.slot].resource.epoch==t.epoch && s.slots[t.slot].generation==t.generation && s.slots[t.slot].phase==p;}
 static void quarantine(State&s,Slot&slot){slot.phase=Phase::Quarantined;slot.live=false;++s.counts.quarantined;}
 static void abandon(const std::shared_ptr<State>&s,Token t,Phase kind) noexcept {
  if(!s)return;std::lock_guard lock(s->mutex);if(!matches(*s,t,kind))return;auto&slot=s->slots[t.slot];
  if(kind==Phase::Producing){s->producing.reset();if(!slot.submitted || slot.sourceComplete){slot.phase=Phase::Free;slot.live=false;}else quarantine(*s,slot);}
  else if(kind==Phase::Active){s->active.reset();quarantine(*s,slot);}
 }
public:
 template<Phase Kind>class Lease {
  friend class ThreeSourceMailbox;std::shared_ptr<State> state_;Token token_;
  Lease(std::shared_ptr<State>s,Token t):state_(std::move(s)),token_(t){}
  void consume()noexcept{state_.reset();}
 public:
  Lease()=default;Lease(const Lease&)=delete;Lease&operator=(const Lease&)=delete;
  Lease(Lease&&o)noexcept:state_(std::move(o.state_)),token_(o.token_){}
  Lease&operator=(Lease&&o)noexcept{if(this!=&o){abandon(state_,token_,Kind);state_=std::move(o.state_);token_=o.token_;}return *this;}
  ~Lease(){abandon(state_,token_,Kind);}
  explicit operator bool()const noexcept{return bool(state_);}Token token()const noexcept{return token_;}
  Resource resource()const{if(!state_)throw std::logic_error("consumed lease");std::lock_guard lock(state_->mutex);if(!matches(*state_,token_,Kind))throw std::logic_error("stale lease");return state_->slots[token_.slot].resource;}
  std::optional<FrameIdentity> identity(Token expected)const requires(Kind==Phase::Active) {
   if(!state_)return {};std::lock_guard lock(state_->mutex);
   if(expected.slot!=token_.slot || expected.epoch!=token_.epoch || expected.generation!=token_.generation || !matches(*state_,token_,Kind))return {};
   return FrameIdentity{token_,state_->slots[token_.slot].frame};
  }
  // Valid only while this Active lease remains owned and unconsumed. Producer
  // and mailbox operations cannot mutate this slot during that lifetime.
  const std::vector<std::uint8_t>& raw()const requires(Kind==Phase::Active){if(!state_)throw std::logic_error("consumed lease");std::lock_guard lock(state_->mutex);if(!matches(*state_,token_,Kind))throw std::logic_error("stale lease");return state_->slots[token_.slot].raw;}
 };
 using Producer=Lease<Phase::Producing>;using Active=Lease<Phase::Active>;
 class SourceProof {
  friend class ThreeSourceMailbox;std::shared_ptr<State>state_;Token token_;std::vector<std::uint8_t>raw_;
  SourceProof(std::shared_ptr<State>s,Token t,std::vector<std::uint8_t>&&raw):state_(std::move(s)),token_(t),raw_(std::move(raw)){}
 public:SourceProof(const SourceProof&)=delete;SourceProof&operator=(const SourceProof&)=delete;SourceProof(SourceProof&&)=default;SourceProof&operator=(SourceProof&&)=default;
 };
 explicit ThreeSourceMailbox(std::array<Resource,3>resources,std::uint64_t generationSeed=0):state_(std::make_shared<State>()) {
  const auto r=resources[0];if(!r.epoch||!r.width||!r.height||std::uint64_t(r.width)*r.height>std::numeric_limits<std::size_t>::max()/4)throw std::invalid_argument("shape/epoch");
  for(unsigned i=0;i<3;++i){auto x=resources[i];if(!x.image||!x.view||!x.memory||x.epoch!=r.epoch||x.width!=r.width||x.height!=r.height)throw std::invalid_argument("three compatible resources required");for(unsigned j=0;j<i;++j)if(x.image==resources[j].image||x.view==resources[j].view||x.memory==resources[j].memory)throw std::invalid_argument("resource alias");state_->slots[i].resource=x;state_->slots[i].generation=generationSeed;}
 }
 ThreeSourceMailbox(const ThreeSourceMailbox&)=delete;ThreeSourceMailbox&operator=(const ThreeSourceMailbox&)=delete;
 ~ThreeSourceMailbox(){close();} // No driver destruction, especially after uncertainty.
 std::optional<Producer> begin(std::uint64_t epoch,std::uint32_t width,std::uint32_t height){
  auto s=state_;std::lock_guard lock(s->mutex);if(s->closed||s->producing)return {};
  for(unsigned i=0;i<3;++i){auto&x=s->slots[i];if(x.phase!=Phase::Free)continue;if(x.resource.epoch!=epoch||x.resource.width!=width||x.resource.height!=height)return {};if(x.generation==UINT64_MAX){s->closed=true;return {};}++x.generation;x.phase=Phase::Producing;x.live=true;x.submitted=x.sourceComplete=false;s->producing=i;return Producer(s,{i,epoch,x.generation});}return {};
 }
 bool markSubmitted(Producer&p){auto s=state_;std::lock_guard lock(s->mutex);if(p.state_!=s||!matches(*s,p.token_,Phase::Producing)||s->closed)return false;auto&x=s->slots[p.token_.slot];if(x.submitted||x.sourceComplete)return false;x.submitted=true;return true;}
 template<class Verify>std::optional<SourceProof> proveSource(Producer&p,std::vector<std::uint8_t> raw,Verify verify){
  auto s=state_;Resource r;{std::lock_guard lock(s->mutex);if(p.state_!=s||!matches(*s,p.token_,Phase::Producing)||!s->slots[p.token_.slot].submitted||s->closed)return {};r=s->slots[p.token_.slot].resource;if(raw.size()!=std::uint64_t(r.width)*r.height*4)return {};}
  bool ok=false;try{ok=verify(r,p.token_,static_cast<const std::vector<std::uint8_t>&>(raw));}catch(...){ok=false;}
  std::lock_guard lock(s->mutex);if(!matches(*s,p.token_,Phase::Producing))return {};auto&x=s->slots[p.token_.slot];if(!ok){quarantine(*s,x);s->producing.reset();p.consume();return {};}x.sourceComplete=true;return SourceProof(s,p.token_,std::move(raw));
 }
 bool publish(Producer&&p,SourceProof&&proof,std::uint64_t frame){
  auto s=state_;std::vector<std::uint8_t>discard;{std::lock_guard lock(s->mutex);if(s->closed||p.state_!=s||proof.state_!=s||!matches(*s,p.token_,Phase::Producing)||p.token_.slot!=proof.token_.slot||p.token_.generation!=proof.token_.generation||p.token_.epoch!=proof.token_.epoch)return false;auto&x=s->slots[p.token_.slot];const auto bytes=std::uint64_t(x.resource.width)*x.resource.height*4;if(!x.sourceComplete||!frame||frame<=s->lastFrame||proof.raw_.size()!=bytes)return false;
   if(s->pending){auto&old=s->slots[*s->pending];if(old.phase!=Phase::Pending||old.live||!old.sourceComplete)throw std::logic_error("pending invariant");old.phase=Phase::Free;discard.swap(old.raw);++s->counts.replaced;}
   x.raw=std::move(proof.raw_);x.frame=frame;x.phase=Phase::Pending;x.live=false;s->pending=p.token_.slot;s->producing.reset();s->lastFrame=frame;++s->counts.published;p.consume();proof.state_.reset();}return true;
 }
 std::optional<Active> take(){auto s=state_;std::lock_guard lock(s->mutex);if(s->closed||s->active||!s->pending)return {};unsigned i=*s->pending;auto&x=s->slots[i];if(x.phase!=Phase::Pending||!x.sourceComplete)throw std::logic_error("pending invariant");x.phase=Phase::Active;x.live=true;s->pending.reset();s->active=i;return Active(s,{i,x.resource.epoch,x.generation});}
 template<class Verify>bool complete(Active&&a,Verify verify){
  auto s=state_;Resource r;{std::lock_guard lock(s->mutex);if(a.state_!=s||!matches(*s,a.token_,Phase::Active))return false;r=s->slots[a.token_.slot].resource;}
  bool ok=false;try{ok=verify(r,a.token_);}catch(...){ok=false;}
  std::vector<std::uint8_t>discard;{std::lock_guard lock(s->mutex);if(!matches(*s,a.token_,Phase::Active))return false;auto&x=s->slots[a.token_.slot];s->active.reset();if(ok){x.phase=Phase::Free;x.live=false;discard.swap(x.raw);++s->counts.accepted;}else quarantine(*s,x);a.consume();}return ok;
 }
 void close()noexcept{auto s=state_;std::vector<std::uint8_t>discard;{std::lock_guard lock(s->mutex);s->closed=true;if(s->pending){auto&x=s->slots[*s->pending];x.phase=Phase::Free;discard.swap(x.raw);s->pending.reset();}}}
 // Caller verifies device drain after admission is closed and no CPU lease can
 // still use the resources. An unsuccessful/throwing verifier retires nothing.
 template<class Verify>std::optional<std::array<Resource,3>> retire(std::uint64_t epoch,Verify verify){
  auto s=state_;std::array<Resource,3>rs;{std::lock_guard lock(s->mutex);if(!s->closed||s->retired||s->active||s->producing||s->slots[0].resource.epoch!=epoch)return {};for(unsigned i=0;i<3;++i){if(s->slots[i].live)return {};rs[i]=s->slots[i].resource;}}
  bool ok=false;try{ok=verify(rs,epoch);}catch(...){ok=false;}if(!ok)return {};
  std::lock_guard lock(s->mutex);if(s->retired||s->active||s->producing)return {};for(auto&x:s->slots)x.phase=Phase::Retired;s->retired=true;return rs;
 }
 std::array<Phase,3> phases()const{auto s=state_;std::lock_guard lock(s->mutex);return {s->slots[0].phase,s->slots[1].phase,s->slots[2].phase};}
 Counts counts()const{std::lock_guard lock(state_->mutex);return state_->counts;}
};
}
