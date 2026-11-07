// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace recomp::apple::performance {
// A one-shot recording window, not the guest's lifetime. The owner and its
// notification context must outlive all callback attempts, including attempts
// after close. notify must only enqueue work; never flush or wait on this path.
// Closing excludes new recordings without excluding/altering guest callbacks.
class MeasurementLifetime {
public:
    using Notify = void (*)(void*) noexcept;
    // inFlight also counts briefly refused post-close attempts. drained is the
    // monotonic proof that every ADMITTED storage user has returned; after that
    // new attempts cannot touch storage, even when inFlight is momentarily >0.
    struct Status { bool closed,drained,loss;std::uint64_t inFlight; };
    class Lease {
    public:
        Lease() noexcept=default;
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
        Lease(Lease&& other) noexcept:owner_(std::exchange(other.owner_,nullptr)) {}
        Lease& operator=(Lease&&)=delete;
        ~Lease() {if(owner_)owner_->leave(false);}
        explicit operator bool() const noexcept {return owner_!=nullptr;}
        // All accesses to recording storage must precede release. Exceptions
        // abandon the lease (loss=true); a second finish never runs the action.
        template<class Action> bool finish(Action&& action) {
            if(!owner_)return false;
            try {action();}
            catch(...) {auto* owner=std::exchange(owner_,nullptr);owner->leave(false);throw;}
            auto* owner=std::exchange(owner_,nullptr);owner->leave(true);return true;
        }
    private:
        friend class MeasurementLifetime;
        explicit Lease(MeasurementLifetime* owner) noexcept:owner_(owner) {}
        MeasurementLifetime* owner_=nullptr;
    };
    explicit MeasurementLifetime(Notify notify,void* context) noexcept:notify_(notify),context_(context) {}
    MeasurementLifetime(const MeasurementLifetime&)=delete;
    MeasurementLifetime& operator=(const MeasurementLifetime&)=delete;
    Lease tryAcquire() noexcept {
        // Count even a refused attempt until its decision is published. This
        // avoids an acquire/close race publishing loss after the drain. The
        // finite callback stack/thread count cannot approach the 63-bit field.
        const auto before=state_.fetch_add(1,std::memory_order_acq_rel);
        if(before&Closed) {leave(true);return {};}
        if((before&Count)>=MaxInFlight) {
            loss_.store(true,std::memory_order_release);leave(true);return {};
        }
        return Lease(this);
    }
    bool close() noexcept {
        const auto before=state_.fetch_or(Closed,std::memory_order_acq_rel);
        if(before&Closed)return false;
#if defined(APPLE_MEASUREMENT_MUTANT_EARLY_DRAIN)
        notifyOnce(); // deliberately allows export while a writer owns storage
#else
        if(!(before&Count))notifyOnce();
#endif
        return true;
    }
    Status status() const noexcept {
        const auto state=state_.load(std::memory_order_acquire);
        return {bool(state&Closed),notified_.load(std::memory_order_acquire),
            loss_.load(std::memory_order_acquire),state&Count};
    }
private:
    static constexpr std::uint64_t MaxInFlight=1024;
    static constexpr std::uint64_t Closed=std::uint64_t(1)<<63,Count=Closed-1;
    void leave(bool completed) noexcept {
#if !defined(APPLE_MEASUREMENT_MUTANT_HIDE_ABANDON)
        if(!completed)loss_.store(true,std::memory_order_release);
#else
        (void)completed;
#endif
        const auto before=state_.fetch_sub(1,std::memory_order_acq_rel);
        if((before&Closed) && (before&Count)==1)notifyOnce();
    }
    void notifyOnce() noexcept {
        if(!notified_.exchange(true,std::memory_order_acq_rel) && notify_)notify_(context_);
    }
    std::atomic<std::uint64_t> state_{0};
    std::atomic<bool> loss_{false},notified_{false};
    const Notify notify_;
    void* const context_;
};
}
