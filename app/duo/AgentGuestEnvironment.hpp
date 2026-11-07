// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentEnvironment.hpp"
#include "AgentGuestDelivery.hpp"
#include <array>

namespace recomp::apple::agentenvironment {
namespace ad=agentdelivery;
// Multi-worker producer/main-thread consumer journal. Neither direction waits:
// a busy producer or full journal closes the automated episode instead of
// silently presenting an incomplete acknowledgement stream as complete.
class Acknowledgements final {
public:
    static constexpr std::size_t Capacity=32;
    bool push(const ad::Result& result) noexcept {
        if(result.verdict!=ad::Verdict::Confirmed || failed())return false;
        if(busy_.test_and_set(std::memory_order_acquire)) {
            failed_.store(true,std::memory_order_release);return false;
        }
        const bool accepted=size_<Capacity && !failed();
        if(accepted){records_[(head_+size_)%Capacity]=result;++size_;}
        else failed_.store(true,std::memory_order_release);
        busy_.clear(std::memory_order_release);return accepted;
    }
    bool pop(ad::Result& result) noexcept {
        result={};
        if(busy_.test_and_set(std::memory_order_acquire))return false;
        const bool available=size_!=0;
        if(available){result=records_[head_];head_=(head_+1)%Capacity;--size_;}
        busy_.clear(std::memory_order_release);return available;
    }
    bool failed() const noexcept {return failed_.load(std::memory_order_acquire);}
private:
    std::array<ad::Result,Capacity> records_{};
    std::size_t head_=0,size_=0;
    std::atomic_flag busy_=ATOMIC_FLAG_INIT;
    std::atomic<bool> failed_{false};
};
// Borrowed real source/record must outlive the joined engine registration.
// This is the final-copy boundary, not a registry substitute. Main registers
// copied in its actual engine and calls read from its actual V5 reader.
class Guest final {
public:
    template<class Source> Guest(Source& source,Record& record) noexcept
        :record_(record),adapter_(source,provide,this),identity_(newIdentity()){}
    Guest(const Guest&)=delete;
    Guest& operator=(const Guest&)=delete;
    bool takeAcknowledgement(ad::Result& result) noexcept {return acknowledgements_.pop(result);}
    bool acknowledgementsFailed() const noexcept {return acknowledgements_.failed();}
    int read(std::uint32_t user,recomp::input::GamepadState* state) noexcept {
        pending_={};
        const int result=adapter_.read(user,state);
        if(result==1 && identity_)pending_={this,identity_,user};
        return result;
    }
    static void copied(void* context,std::uint32_t user,std::uint32_t output,
        const std::uint8_t* bytes,std::uint32_t count) noexcept {
        const auto pending=pending_;pending_={};
        auto* self=static_cast<Guest*>(context);
        if(!self || self!=pending.owner || !pending.identity || pending.identity!=self->identity_ || user!=pending.user) {
            // A mismatched/duplicate callback consumes, never retains, the
            // adapter's old TLS sample for a later unrelated callback.
            ad::Adapter::delivered(nullptr,user,output,bytes,count);return;
        }
        if(!self->record_.guestCopied(user,output,bytes,count)) {
            // No recorded final-copy progress => no successful ACK. Clearing
            // with invalid bytes discards the exact pending sample once.
            ad::Adapter::delivered(&self->adapter_,user,output,nullptr,0);return;
        }
        // Record ALL valid actual copies (manual/no lease too) BEFORE the
        // adapter re-reads the coherent environment for its exact ACK check.
        ad::Adapter::delivered(&self->adapter_,user,output,bytes,count);
        (void)self->acknowledgements_.push(ad::Adapter::lastForCurrentThread());
    }
private:
    static bool provide(void* context,agentinput::Environment& e) noexcept {
        auto& self=*static_cast<Guest*>(context);
        if(!self.record_.read(e))return false;
        // Revoke the agent on the next real source read even if Main has not
        // drained the failure yet. Ordinary manual input still uses its source.
        if(self.acknowledgements_.failed())e.active=false;
        return true;
    }
    static std::uint64_t newIdentity() noexcept {
        static std::atomic<std::uint64_t> next{1};return ad::allocateIdentity(next);
    }
    struct Pending {Guest* owner=nullptr;std::uint64_t identity=0;std::uint32_t user=0;};
    Record& record_;
    ad::Adapter adapter_;
    Acknowledgements acknowledgements_;
    const std::uint64_t identity_;
    static thread_local Pending pending_;
};
inline thread_local Guest::Pending Guest::pending_{};
}
