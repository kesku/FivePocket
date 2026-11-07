// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>

struct RecompIosGpuFrameV1;

namespace recomp::apple {
namespace performance { class DisplayRecording; }
namespace detail {
// Main-thread serialized owner lifetime, shared with host late-delivery tests.
// The notification block retains only weak_ptr. Invalidate BEFORE unregistering
// observers: removal need not cancel a delivery already queued by Foundation.
template<class Owner> class IosPresenterObserverGate final {
public:
    explicit IosPresenterObserverGate(Owner* owner) noexcept : owner_(owner) {}
    void invalidate() noexcept {
#if !defined(APPLE_IOS_PRESENTER_MUTANT_OBSERVER_LIFETIME)
        owner_=nullptr;
#endif
    }
    template<class Callback>
    static bool deliver(const std::weak_ptr<IosPresenterObserverGate>& weak, Callback&& callback) {
        auto locked=weak.lock();
        if (!locked || !locked->owner_) return false;
        callback(*locked->owner_);
        return true;
    }
private:
    Owner* owner_;
};

// The same gate runs in the UIKit consumer and in host-only V4 contract tests.
// No GPU or UIKit behavior is simulated by this gate.
class IosFrameContract final {
public:
    enum class Admission { Ready, Malformed, Closed, Sequence, Unavailable };
    enum class Completion { Rejected, Accepted, FatalAfterAcceptance };
    static constexpr std::uint32_t Width=1280, Height=720, Bytes=Width*Height*4;
    Admission begin(std::uint32_t sequence, const std::uint8_t* pixels,
                    std::uint32_t bytes, std::uint32_t width, std::uint32_t height,
                    bool active, bool attached, bool exactDrawable) noexcept {
        if (closed_) return Admission::Closed;
#if !defined(APPLE_IOS_PRESENTER_MUTANT_SHAPE)
        if (!pixels || bytes!=Bytes || width!=Width || height!=Height) return Admission::Malformed;
#else
        (void)pixels;(void)bytes;(void)width;(void)height;
#endif
#if !defined(APPLE_IOS_PRESENTER_MUTANT_SEQUENCE)
        if (pending_ || sequence!=accepted_ || sequence==UINT32_MAX) {
            close();return Admission::Sequence;
        }
#else
        (void)sequence;
#endif
#if !defined(APPLE_IOS_PRESENTER_MUTANT_LIFECYCLE)
        if (!active || !attached || !exactDrawable) {close();return Admission::Unavailable;}
#else
        (void)active;(void)attached;(void)exactDrawable;
#endif
        pending_=true;
        return Admission::Ready;
    }
    // acceptedByQueue includes VK_SUBOPTIMAL_KHR. Once accepted, a later error
    // cannot be reported as callback rejection: the process must terminate.
    Completion finish(bool acceptedByQueue, bool laterFailure) noexcept {
        if (!acceptedByQueue) {close();return Completion::Rejected;}
#if !defined(APPLE_IOS_PRESENTER_MUTANT_ACCEPTANCE)
        if (!pending_ || closed_ || laterFailure) {
            close();return Completion::FatalAfterAcceptance;
        }
#else
        (void)laterFailure;
#endif
        pending_=false;++accepted_;return Completion::Accepted;
    }
    void close() noexcept {closed_=true;pending_=false;}
    bool healthy() const noexcept {return !closed_;}
    std::uint32_t count() const noexcept {return accepted_;}
private:
    bool closed_=false, pending_=false;
    std::uint32_t accepted_=0;
};
}

// Diagnostic synchronous V4 consumer. All calls and destruction MUST be on the
// main thread; an off-thread call exits4. The caller must keep the main run loop
// pumping and dispatch the producer callback synchronously (never retain its
// borrowed pixel pointer). nativeView() is a borrowed UIView*, bridge it in
// Objective-C++, attach it to an active UIWindowScene, and lay it out first.
// The view letterboxes a fixed1280x720 CAMetalLayer; no source pixels are scaled
// by Vulkan. UIKit may scale the layer for the device's actual point size.
// true proves source+destination readback and accepted QueuePresent only, not
// physical display/vblank. Any lifecycle loss closes the stream permanently:
// no GPU work in background, no frame drop, auto-resume or swapchain recovery.
// The host must coordinate stopping/joining the old producer without blocking
// the main thread before destroying this object or creating a new session.
class IosFramePresenter final {
public:
    explicit IosFramePresenter(bool managedLifecycle=false);
    IosFramePresenter(bool managedLifecycle,std::shared_ptr<performance::DisplayRecording> recording);
    // Optional recorder selected before any UIKit layer/swapchain creation.
    // Detach only: does not close the game frame contract or change GPU work.
    void stopDisplayObservation();
    ~IosFramePresenter();
    IosFramePresenter(const IosFramePresenter&)=delete;
    IosFramePresenter& operator=(const IosFramePresenter&)=delete;
    void* nativeView() const noexcept;
    bool present(std::uint32_t sequence, const std::uint8_t* rgba,
                 std::uint32_t bytes, std::uint32_t width, std::uint32_t height);
    // Directly usable as RecompPrivateTraceCallbacksV4::orderedRgba; user=this.
    static int callback(void* user, std::uint32_t sequence, const std::uint8_t* rgba,
                        std::uint32_t bytes, std::uint32_t width, std::uint32_t height) noexcept;
    // Opt-in shared-device path. Any nonaccepted result is terminal to caller.
    int presentGpu(const RecompIosGpuFrameV1&);
    void closeGpu(bool terminal);
    void close();
    // Optional foreground lifecycle managed by the app's all-queue gate.
    // Suspend preserves ordering/resources; resume requires the same healthy,
    // active, attached exact drawable. Neither method submits GPU work.
    bool suspend();
    bool resume();
    // Transient UIKit availability only; does not mask a poisoned contract or
    // bad pixel/sequence/drawable format. Main can defer before begin().
    bool activeAndAttached() const noexcept;
    bool healthy() const noexcept;
    std::uint32_t count() const noexcept;
    std::uint32_t uploadCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
