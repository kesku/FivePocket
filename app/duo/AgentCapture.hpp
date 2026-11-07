// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AgentIngress.hpp"
#include <cstring>
#include <memory>
#include <thread>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace recomp::apple::agentcapture {
namespace ai=agentinput;
namespace ao=agentobservation;
namespace ingress=agentingress;

enum class Pixels : std::uint8_t { MenuWindowPremultipliedRgba8, AcceptedGuestRgba8 };
struct Publication {
    ingress::Observation observation{};
    Pixels pixels=Pixels::MenuWindowPremultipliedRgba8;
};
enum class Reason : std::uint8_t { None,Thread,Busy,Closed,Source,Menu,Shape,Clock,Image,Publication,Cancelled };
struct Result {
    Reason reason=Reason::None;
    explicit operator bool() const noexcept {return reason==Reason::None;}
};

#if defined(__APPLE__)
inline bool nativeSha256(void*,const std::uint8_t* bytes,std::size_t count,ao::Digest& out) noexcept {
    if(!bytes || !count || count>ao::Ledger::MaxBytes)return false;
    return CC_SHA256(bytes,static_cast<CC_LONG>(count),out.data())!=nullptr;
}
#endif

// Construct on Main before accepting requests. One pixel buffer allocates once;
// acceptedGuest performs no allocation, UI, file I/O or asynchronous work.
// Referents outlive this controller. Only the actual accepted Main callback may
// call acceptedGuest. Its progress is input-copy progress AT capture, never a
// claim that the asynchronously produced frame incorporated those inputs.
//
// The transport copies a publication into its own storage before off-main work.
// Cancel after consuming/abandoning a request; a second request never silently
// replaces an image awaiting review. This class never authorizes, attests or ACKs.
template<class Dispatcher> class Controller final {
public:
    using Clock=ao::Ledger::Clock;
    Controller(ao::Ledger& ledger,Dispatcher& dispatcher,std::size_t capacity,
               Clock clock,void* context)
        :ledger_(ledger),dispatcher_(dispatcher),capacity_(capacity),clock_(clock),context_(context) {
        if(!capacity || capacity>ao::Ledger::MaxBytes || !clock){phase_=Phase::Closed;return;}
        pixels_=std::make_unique<std::uint8_t[]>(capacity);
    }
    Controller(const Controller&)=delete;
    Controller& operator=(const Controller&)=delete;

    // Menu supplies the actual neutral source consumer and pixel producer.
    // The native adapter below checks window/scene/epoch identity on each call.
    template<class Menu> Result captureMenu(Menu& menu) noexcept {
        Scope scope(*this);if(!scope)return {scope.reason};
        if(phase_!=Phase::Idle)return {Reason::Busy};
        ai::Environment before{};
        if(!dispatcher_.read(before) || before.stage!=ai::Stage::Menu || !before.stageGeneration ||
           !before.active || !before.manualNeutral)return {Reason::Source};
        if(!menu.valid() || !menu.consume())return {Reason::Menu};
        ai::Environment sampled{};
        if(cancelled_ || !source(ai::Stage::Menu,sampled) || !sameOwner(before,sampled))
            return abort(Reason::Source);
        const auto capturedAt=clock_(context_);
        if(!capturedAt)return abort(Reason::Clock);
        std::uint32_t width=0,height=0;
        if(!menu.draw(pixels_.get(),capacity_,width,height) || !menu.valid())return abort(Reason::Menu);
        std::size_t count=0;
        if(!shape(width,height,count))return abort(Reason::Shape);
        ai::Environment after{};
        if(cancelled_ || !source(ai::Stage::Menu,after) || !sameOwner(sampled,after) ||
           after.progressRevision!=sampled.progressRevision)return abort(Reason::Source);
        return publish(sampled,0,capturedAt,pixels_.get(),count,width,height,Pixels::MenuWindowPremultipliedRgba8);
    }

    Result armGuest() noexcept {
        Scope scope(*this);if(!scope)return {scope.reason};
        if(phase_!=Phase::Idle)return {Reason::Busy};
        if(!source(ai::Stage::Guest,ticket_))return {Reason::Source};
        phase_=Phase::Armed;return {};
    }
    Result acceptedGuest(std::uint32_t sequence,const std::uint8_t* bytes,std::uint32_t count,
                         std::uint32_t width,std::uint32_t height) noexcept {
        Scope scope(*this);if(!scope)return {scope.reason};
        if(phase_!=Phase::Armed)return {Reason::Busy};
        // Capture the next actual accepted callback after arm, not a numerical
        // sequence comparison. Preserve its sequence (including zero/wrap).
        if(!bytes || width!=1280 || height!=720 ||
           count!=1280u*720u*4u || count>capacity_)return abort(Reason::Shape);
        ai::Environment sampled{};
        if(!source(ai::Stage::Guest,sampled) || !sameOwner(ticket_,sampled))return abort(Reason::Source);
        const auto capturedAt=clock_(context_);
        if(!capturedAt)return abort(Reason::Clock);
        return publish(sampled,sequence,capturedAt,bytes,count,width,height,Pixels::AcceptedGuestRgba8);
    }

    Result copyPublication(std::uint8_t* destination,std::size_t capacity,Publication& out) const noexcept {
        out={};
        if(std::this_thread::get_id()!=owner_)return {Reason::Thread};
        if(entered_)return {Reason::Busy};
        if(phase_!=Phase::Published)return {Reason::Publication};
        const auto count=publication_.observation.metadata.byteCount;
        if(!destination || capacity<count)return {Reason::Shape};
        std::memcpy(destination,pixels_.get(),count);out=publication_;return {};
    }
    Result cancel() noexcept {
        if(std::this_thread::get_id()!=owner_)return {Reason::Thread};
        // UIKit may reenter lifecycle code. Invalidate the outer capture even
        // though resetting the dispatcher must wait until it returns.
        if(entered_){cancelled_=true;return {Reason::Busy};}
        if(phase_==Phase::Closed)return {Reason::Closed};
        return abort(Reason::None);
    }
private:
    enum class Phase : std::uint8_t { Idle,Armed,Published,Closed };
    struct Scope {
        Controller& self;Reason reason=Reason::None;
        explicit Scope(Controller& value) noexcept:self(value) {
            if(std::this_thread::get_id()!=self.owner_)reason=Reason::Thread;
            else if(self.entered_)reason=Reason::Busy;
            else if(self.phase_==Phase::Closed)reason=Reason::Closed;
            else {self.entered_=true;self.cancelled_=false;}
        }
        ~Scope(){if(reason==Reason::None)self.entered_=false;}
        explicit operator bool() const noexcept {return reason==Reason::None;}
    };
    bool source(ai::Stage stage,ai::Environment& e) noexcept {
        return bool(dispatcher_.read(e)) && e.stage==stage && e.stageGeneration && e.active && e.manualNeutral &&
            e.progressRevision && e.nowMs && e.sourceSampleMs && e.sourceSampleMs<=e.nowMs &&
            e.nowMs-e.sourceSampleMs<=ai::SourceFreshMs;
    }
    static bool sameOwner(const ai::Environment& a,const ai::Environment& b) noexcept {
        return a.stage==b.stage && a.stageGeneration==b.stageGeneration &&
            a.ownerActivityGeneration==b.ownerActivityGeneration;
    }
    bool shape(std::uint32_t width,std::uint32_t height,std::size_t& count) const noexcept {
        if(!width || !height || width>capacity_/4)return false;
        const auto row=std::size_t(width)*4;
        if(height>capacity_/row)return false;
        count=row*height;return true;
    }
    Result abort(Reason reason) noexcept {
        publication_={};ticket_={};cancelled_=false;
        phase_=dispatcher_.reset()?Phase::Idle:Phase::Closed;
        return {phase_==Phase::Closed?Reason::Closed:reason};
    }
    Result publish(const ai::Environment& sampled,std::uint32_t sequence,std::uint64_t capturedAt,
                   const std::uint8_t* bytes,std::size_t count,std::uint32_t width,std::uint32_t height,Pixels pixels) noexcept {
        ao::Reference reference{};
        if(!ledger_.capture(sampled,sequence,capturedAt,bytes,count,width,height,reference))return abort(Reason::Image);
        ai::Environment after{};
        if(cancelled_ || !source(sampled.stage,after) || !sameOwner(sampled,after) ||
           (sampled.stage==ai::Stage::Menu && after.progressRevision!=sampled.progressRevision))return abort(Reason::Cancelled);
        ingress::Observation observation{};
        // Ledger owns the raster before this copy overwrites scratch storage.
        // Published blocks further capture until cancel invalidates the export.
        if(!dispatcher_.publish(reference,pixels_.get(),capacity_,observation))return abort(Reason::Publication);
        if(cancelled_)return abort(Reason::Cancelled);
        publication_={observation,pixels};phase_=Phase::Published;return {};
    }
    ao::Ledger& ledger_;Dispatcher& dispatcher_;
    const std::size_t capacity_;const Clock clock_;void* const context_;
    const std::thread::id owner_=std::this_thread::get_id();
    std::unique_ptr<std::uint8_t[]> pixels_;
    ai::Environment ticket_{};Publication publication_{};
    Phase phase_=Phase::Idle;
    bool entered_=false,cancelled_=false;
};
}

#if defined(__OBJC__)
#import "LauncherInputBridge.h"
#import "LauncherController.h"
#import <UIKit/UIKit.h>
#include <cmath>

namespace recomp::apple::agentcapture {
// An app-window raster, including UIKit presentations, not physical scanout.
// Full-size premultiplied sRGB RGBA8; no silent crop/downscale/extended range.
class UIKitMenu final {
public:
    UIKitMenu(PortLauncherInput* input,PortLauncherController* launcher,UIWindow* window)
        :input_(input),launcher_(launcher),window_(window),scene_(window.windowScene),
         root_(window.rootViewController),bounds_(window.bounds),scale_(window.windowScene.screen.scale),epoch_(launcher.inputEpoch) {}
    bool valid() const noexcept {
      @try {
        if(!NSThread.isMainThread || !input_ || !launcher_ || !window_ || !scene_ || !root_ ||
           !input_.active || !launcher_.isViewLoaded || launcher_.inputClosed || launcher_.hostBusy || launcher_.inputEpoch!=epoch_ ||
           window_.windowScene!=scene_ || window_.rootViewController!=root_ ||
           scene_.activationState!=UISceneActivationStateForegroundActive ||
           UIApplication.sharedApplication.applicationState!=UIApplicationStateActive ||
           !CGRectEqualToRect(window_.bounds,bounds_) || scene_.screen.scale!=scale_ ||
           launcher_.view.window!=window_ || !std::isfinite(scale_) || scale_<=0)return false;
        for(UIView* view=launcher_.view;view;view=view.superview)
            if(view.hidden || view.alpha<=0)return false;
        return true;
      } @catch(NSException*) {return false;}
    }
    bool consume() noexcept {
        @try {return [input_ consumeIdleAppSource];} @catch(NSException*) {return false;}
    }
    bool draw(std::uint8_t* bytes,std::size_t capacity,std::uint32_t& width,std::uint32_t& height) noexcept {
        width=height=0;if(!valid() || !bytes)return false;
        const double w=std::ceil(bounds_.size.width*scale_),h=std::ceil(bounds_.size.height*scale_);
        if(!std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0 || w>UINT32_MAX || h>UINT32_MAX ||
           w>capacity/4)return false;
        const auto row=std::size_t(w)*4;
        if(h>capacity/row)return false;
        const auto count=row*std::size_t(h);std::memset(bytes,0,count);
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        if(!color)return false;
        CGContextRef context=CGBitmapContextCreate(bytes,std::size_t(w),std::size_t(h),8,row,color,
            CGBitmapInfo(kCGBitmapByteOrder32Big)|CGBitmapInfo(kCGImageAlphaPremultipliedLast));
        CGColorSpaceRelease(color);if(!context)return false;
        CGContextTranslateCTM(context,0,h);CGContextScaleCTM(context,scale_,-scale_);
        __block BOOL complete=NO;
        UIGraphicsPushContext(context);
        @try {complete=[window_ drawViewHierarchyInRect:CGRectMake(0,0,bounds_.size.width,bounds_.size.height) afterScreenUpdates:YES];}
        @catch(NSException*) {complete=NO;}
        @finally {UIGraphicsPopContext();CGContextRelease(context);}
        if(!complete || !valid())return false;
        width=std::uint32_t(w);height=std::uint32_t(h);return true;
    }
private:
    PortLauncherInput* __strong input_;PortLauncherController* __strong launcher_;
    UIWindow* __strong window_;UIWindowScene* __strong scene_;UIViewController* __strong root_;
    CGRect bounds_;CGFloat scale_;NSUInteger epoch_;
};
}
#endif
