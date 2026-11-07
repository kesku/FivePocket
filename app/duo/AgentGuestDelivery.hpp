// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "FullTouchInput.hpp"
#include <atomic>
#include <cstring>
#include <limits>

namespace recomp::apple::agentdelivery {
namespace ai=agentinput;
namespace ft=fulltouch;
// Constructor identity never wraps/reuses. Exhaustion disables this adapter;
// it is not a clock, guest frame or an authorization revision.
inline std::uint64_t allocateIdentity(std::atomic<std::uint64_t>& next) noexcept {
    auto value=next.load(std::memory_order_relaxed);
    while(value && value!=std::numeric_limits<std::uint64_t>::max()) {
        if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
    }
    return 0;
}
enum class Verdict {NoSample,WrongInstance,WrongUser,InvalidOutput,WrongBytes,
    EnvironmentUnavailable,Rejected,Confirmed};
struct Result {
    Verdict verdict=Verdict::NoSample;
    ai::Result acknowledgement{ai::Reason::NoLease};
    ai::Token token{};
    std::uint32_t output=0;
    // The same coherent environment used by confirmAgent, not a later timer
    // read. These identify input consumption, never a presented image.
    std::uint64_t progress=0,acknowledgedAtMs=0;
};
// Main's environment provider MUST supply authoritative, coherent stage,
// monotonic clock, consumer progress and fresh observation state. It is called
// from a guest worker: bounded/noexcept, no UI, I/O, blocking or registry query.
// This class invents no progress, freshness, owner inactivity or authorization.
class Adapter final {
public:
    using Environment=bool(*)(void*,ai::Environment&) noexcept;
    template<class InputSource> Adapter(InputSource& input,Environment environment,void* context) noexcept
        :input_(source(input)),environment_(environment),context_(context),identity_(newIdentity()) {}
    Adapter(const Adapter&)=delete;
    Adapter& operator=(const Adapter&)=delete;
    // Default-off: missing provider preserves ordinary manual snapshot. A
    // failed enabled provider reports disconnected, never returns held agent
    // state with fabricated freshness or stops the import with reader error.
    int read(std::uint32_t user,recomp::input::GamepadState* state) noexcept {
        pending_={};last_={};
        if(!environment_ || !identity_)return input_.snapshot(user,state);
        ai::Environment e{};
        if(!environment_(context_,e)) {if(state)*state={};return state?0:-1;}
        ft::AgentReceipt sample{};
        const int result=input_.sampleAgent(user,state,e,&sample);
        if(result==1 && sample.consumed.available)pending_={this,identity_,user,sample,true};
        return result;
    }
    // Signature is assignable to the dedicated versioned C observer. Main
    // registers THIS object only in its actual loaded engine; no static helper.
    // Same-thread synchronous registry callback consumes a sample ONCE. A null
    // output, capability/presence query or failed reader cannot fabricate one.
    static void delivered(void* context,std::uint32_t user,std::uint32_t output,
        const std::uint8_t* bytes,std::uint32_t count) noexcept {
        const auto sample=pending_;pending_={};last_={};
        if(!sample.valid)return;
        const auto* self=static_cast<const Adapter*>(context);
        if(!self || sample.owner!=self || sample.identity!=self->identity_) {
            last_.verdict=Verdict::WrongInstance;return;
        }
        if(user!=sample.user) {last_.verdict=Verdict::WrongUser;return;}
        if(!output || (output&3)) {last_.verdict=Verdict::InvalidOutput;return;}
        const auto expected=recomp::input::encodeGamepadState(sample.receipt.native);
        if(!bytes || count!=expected.size() || std::memcmp(bytes,expected.data(),expected.size())!=0) {
            last_.verdict=Verdict::WrongBytes;return;
        }
        ai::Environment e{};
        if(!self->environment_ || !self->environment_(self->context_,e)) {
            last_.verdict=Verdict::EnvironmentUnavailable;return;
        }
        const auto result=self->input_.confirmAgent(sample.receipt,sample.receipt.native,e);
        last_={result?Verdict::Confirmed:Verdict::Rejected,result,sample.receipt.consumed.token,output,
            e.progressRevision,e.nowMs};
    }
    // Diagnostic accessor for the matching consumer thread only. Not a log,
    // screenshot, source freshness or whole-game response acknowledgement.
    static Result lastForCurrentThread() noexcept {return last_;}
private:
    // Type-erased borrowed source: either the actual Gamepad owned by Main or
    // its Input seam. No heap allocation, second controller handler or mutable
    // internal-Input accessor. Source must outlive the joined observer episode.
    struct Source {
        void* context;
        int(*snapshotFn)(void*,std::uint32_t,recomp::input::GamepadState*) noexcept;
        int(*sampleFn)(void*,std::uint32_t,recomp::input::GamepadState*,ai::Environment,ft::AgentReceipt*) noexcept;
        ai::Result(*confirmFn)(void*,const ft::AgentReceipt&,const recomp::input::GamepadState&,ai::Environment) noexcept;
        int snapshot(std::uint32_t user,recomp::input::GamepadState* state)const noexcept {return snapshotFn(context,user,state);}
        int sampleAgent(std::uint32_t user,recomp::input::GamepadState* state,ai::Environment e,ft::AgentReceipt* receipt)const noexcept {
            return sampleFn(context,user,state,e,receipt);
        }
        ai::Result confirmAgent(const ft::AgentReceipt& sample,const recomp::input::GamepadState& state,ai::Environment e)const noexcept {
            return confirmFn(context,sample,state,e);
        }
    };
    template<class T> static Source source(T& input) noexcept {
        return {&input,
            [](void* context,std::uint32_t user,recomp::input::GamepadState* state) noexcept {return static_cast<T*>(context)->snapshot(user,state);},
            [](void* context,std::uint32_t user,recomp::input::GamepadState* state,ai::Environment e,ft::AgentReceipt* receipt) noexcept {
                return static_cast<T*>(context)->sampleAgent(user,state,e,receipt);},
            [](void* context,const ft::AgentReceipt& sample,const recomp::input::GamepadState& state,ai::Environment e) noexcept {
                return static_cast<T*>(context)->confirmAgent(sample,state,e);}};
    }
    struct Pending {const Adapter* owner=nullptr;std::uint64_t identity=0;
        std::uint32_t user=0;ft::AgentReceipt receipt{};bool valid=false;};
    static std::uint64_t newIdentity() noexcept {
        static std::atomic<std::uint64_t> next{1};return allocateIdentity(next);
    }
    const Source input_;
    const Environment environment_;
    void* const context_;
    const std::uint64_t identity_;
    static thread_local Pending pending_;
    static thread_local Result last_;
};
inline thread_local Adapter::Pending Adapter::pending_{};
inline thread_local Result Adapter::last_{};
}
