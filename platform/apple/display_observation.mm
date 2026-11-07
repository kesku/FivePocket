// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple/display_observation.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace recomp::apple::performance {
namespace { constexpr std::uint64_t Closed=std::uint64_t{1}<<63; }
struct DisplayRecording::Impl {
    struct Slot {
        // 0: empty, 1: admitted, 2: callback claimed, 3: completed.
        std::atomic<unsigned> state{0};
        std::uint64_t drawableId=0;
        double hostSeconds=0;
    };
    const std::uint32_t capacity;
    std::unique_ptr<Slot[]> slots;
    std::atomic<std::uint64_t> admission{0},completed{0},rejected{0},layerEpoch{0};
    std::atomic<bool> overflow{false},duplicate{false},invalid{false},identity{false};
    explicit Impl(std::uint32_t size):capacity(size),slots(std::make_unique<Slot[]>(size)) {}
};
DisplayRecording::DisplayRecording(std::uint32_t capacity) {
    if(!capacity || capacity>65536)throw std::invalid_argument("display recording capacity must be1..65536");
    impl_=std::make_unique<Impl>(capacity);
}
DisplayRecording::~DisplayRecording()=default;
bool DisplayRecording::acquire(std::uint64_t id,std::uint32_t& slot) noexcept {
    auto& s=*impl_;
    auto value=s.admission.load(std::memory_order_acquire);
    for(;;) {
        if(value&Closed)return false;
        if(value>=s.capacity) {s.overflow.store(true,std::memory_order_release);return false;}
        if(s.admission.compare_exchange_weak(value,value+1,std::memory_order_acq_rel,std::memory_order_acquire))break;
    }
    slot=static_cast<std::uint32_t>(value);
    auto& row=s.slots[slot];row.drawableId=id;
    row.state.store(1,std::memory_order_release);
    return true;
}
void DisplayRecording::presented(std::uint32_t slot,std::uint64_t id,double seconds) noexcept {
    auto& s=*impl_;
    if(slot>=s.capacity) {s.invalid.store(true,std::memory_order_release);return;}
    auto& row=s.slots[slot];unsigned expected=1;
    if(!row.state.compare_exchange_strong(expected,2,std::memory_order_acq_rel)) {
#if !defined(APPLE_DISPLAY_MUTANT_IGNORE_DUPLICATE)
        s.duplicate.store(true,std::memory_order_release);
#endif
        return;
    }
    if(id!=row.drawableId)s.identity.store(true,std::memory_order_release);
    row.hostSeconds=seconds;
#if !defined(APPLE_DISPLAY_MUTANT_ACCEPT_ZERO)
    if(!std::isfinite(seconds) || seconds<=0)s.invalid.store(true,std::memory_order_release);
#endif
    row.state.store(3,std::memory_order_release);
    s.completed.fetch_add(1,std::memory_order_release);
}
void DisplayRecording::close() noexcept {impl_->admission.fetch_or(Closed,std::memory_order_acq_rel);}
void DisplayRecording::registrationRejected() noexcept {
#if !defined(APPLE_DISPLAY_MUTANT_REJECTION)
    impl_->rejected.fetch_add(1,std::memory_order_release);
#endif
}
bool DisplayRecording::bindLayerEpoch(std::uint64_t epoch) noexcept {
    if(!epoch || (impl_->admission.load(std::memory_order_acquire)&Closed))return false;
    std::uint64_t expected=0;
    if(impl_->layerEpoch.compare_exchange_strong(expected,epoch,std::memory_order_acq_rel) || expected==epoch)return true;
#if defined(APPLE_DISPLAY_MUTANT_LAYER_EPOCH)
    return true;
#else
    impl_->identity.store(true,std::memory_order_release);return false;
#endif
}
DisplaySnapshot DisplayRecording::snapshot() const {
    auto& s=*impl_;DisplaySnapshot out;
    auto admitted=s.admission.load(std::memory_order_acquire);
    out.closed=bool(admitted&Closed);out.acquired=admitted&~Closed;
    out.completed=s.completed.load(std::memory_order_acquire);
    out.rejectedRegistrations=s.rejected.load(std::memory_order_acquire);
    out.layerEpoch=s.layerEpoch.load(std::memory_order_acquire);
    out.overflow=s.overflow.load(std::memory_order_acquire);
    out.duplicateCallback=s.duplicate.load(std::memory_order_acquire);
    out.invalidTime=s.invalid.load(std::memory_order_acquire);
    out.invalidIdentity=s.identity.load(std::memory_order_acquire);
    for(std::uint64_t i=0;i<out.acquired;++i) {
        const auto& row=s.slots[i];
        if(row.state.load(std::memory_order_acquire)==3)
            out.samples.push_back({i,row.drawableId,row.hostSeconds});
    }
    std::vector<std::uint64_t> identities;identities.reserve(out.samples.size());
    for(const auto& row:out.samples)identities.push_back(row.drawableId);
    std::sort(identities.begin(),identities.end());
    if(std::adjacent_find(identities.begin(),identities.end())!=identities.end())out.invalidIdentity=true;
    return out;
}
bool observeDisplayDrawable(void* opaque,const std::shared_ptr<DisplayRecording>& recording) noexcept {
    if(!recording)return false;
    id<MTLDrawable> drawable=(__bridge id<MTLDrawable>)opaque;
    if(!drawable || ![drawable respondsToSelector:@selector(addPresentedHandler:)] ||
       ![drawable respondsToSelector:@selector(presentedTime)] ||
       ![drawable respondsToSelector:@selector(drawableID)]) {
        recording->registrationRejected();return false;
    }
    std::uint32_t slot=0;
    if(!recording->acquire(static_cast<std::uint64_t>(drawable.drawableID),slot)) {
        recording->registrationRejected();return false;
    }
    const auto owned=recording;
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
#if defined(APPLE_DISPLAY_MUTANT_CALLBACK_TIME)
        owned->presented(slot,presented.drawableID,1.0); // NOT presentation time
#else
        owned->presented(slot,presented.drawableID,presented.presentedTime);
#endif
    }];
    return true;
}
}

@interface RecompObservedMetalLayer : CAMetalLayer {
    std::shared_ptr<recomp::apple::performance::DisplayRecording> recording_;
    std::uint64_t epoch_;
}
- (BOOL)setRecording:(std::shared_ptr<recomp::apple::performance::DisplayRecording>)recording;
@end
@implementation RecompObservedMetalLayer
- (BOOL)setRecording:(std::shared_ptr<recomp::apple::performance::DisplayRecording>)recording {
    static std::atomic<std::uint64_t> nextEpoch{1};
    if(!epoch_)epoch_=nextEpoch.fetch_add(1,std::memory_order_relaxed);
    if(recording && !recording->bindLayerEpoch(epoch_))return NO;
    auto previous=std::atomic_load_explicit(&recording_,std::memory_order_acquire);
    if(previous && previous!=recording)previous->close();
    std::atomic_store_explicit(&recording_,std::move(recording),std::memory_order_release);
    return YES;
}
- (id<CAMetalDrawable>)nextDrawable {
    id<CAMetalDrawable> drawable=[super nextDrawable];
    auto recording=std::atomic_load_explicit(&recording_,std::memory_order_acquire);
    if(drawable && recording)
        (void)recomp::apple::performance::observeDisplayDrawable((__bridge void*)drawable,recording);
    return drawable;
}
@end
namespace recomp::apple::performance {
void* observedMetalLayerClass() noexcept {return (__bridge void*)RecompObservedMetalLayer.class;}
bool configureDisplayLayer(void* opaque,std::shared_ptr<DisplayRecording> recording) noexcept {
    id layer=(__bridge id)opaque;
    if(![NSThread isMainThread] || ![layer isKindOfClass:RecompObservedMetalLayer.class])return false;
    return [(RecompObservedMetalLayer*)layer setRecording:std::move(recording)];
}
}
