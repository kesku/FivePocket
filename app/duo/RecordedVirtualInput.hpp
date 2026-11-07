// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "RecordedInputPolicy.hpp"
#include "RecordedAimPolicy.hpp"
#include "BoundedCommandFile.hpp"
#include "apple/gamepad_input.h"
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>
namespace recomp::apple {
// Development-only, explicit opt-in; local app-container commands, no network.
// Owner must inspect a current screenshot before each fresh, bounded command.
class RecordedVirtualInput final {
    GCVirtualController* controller_;
    AppleGamepadInput* input_;
    std::unique_ptr<RecordedDpadLease> dpad_;
    int directory_=-1;
    std::string nonce_;
    RecordedInputPolicy policy_;
    RecordedAimPolicy aim_;
    bool aimEnabled_=false;
    std::uint32_t aimPublished_=0,aimCommand_=0;
    std::mutex aimMutex_;
    bool aimPendingAck_=false;
    RecordedAimState aimExpected_{};
    std::uint32_t aimAckGeneration_=0,aimAckCommand_=0,aimAckLease_=0;
    bool holding_=false,closed_=false;
    std::uint32_t command_=0;
    std::chrono::steady_clock::time_point releaseAt_;
    static std::uint64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static const char* eventName(AimEvent event) {
        switch(event) {
        case AimEvent::Begin:return "begin";case AimEvent::Move:return "move";
        case AimEvent::Fire:return "fire";case AimEvent::Settle:return "settle";
        case AimEvent::End:return "end";case AimEvent::Expired:return "expired";
        case AimEvent::Closed:return "closed";default:return "none";
        }
    }
    bool aimAcknowledged() {
        std::lock_guard lock(aimMutex_);return !aimPendingAck_;
    }
    // One publisher owns both protocols. Never create a second controller or
    // D-pad lease. Guest acknowledgement is from the real callback, not from
    // merely setting a native property or from a host copy result.
    bool publishAim(std::uint64_t now) {
        if(aimPublished_==aim_.generation())return true;
        const auto state=aim_.state();
        RecordedInputCommand c;c.axes={0,0,state.rightX,state.rightY,state.leftTrigger,state.rightTrigger};
        std::lock_guard lock(aimMutex_);
        // Drop fire before aim on every release; native callbacks may expose
        // intermediate setters. Never briefly publish un-aimed fire.
        if(!state.rightTrigger)[controller_ setValue:0.0 forButtonElement:GCInputRightTrigger];
        if(!apply(c))return false;
        aimExpected_=state;aimPendingAck_=true;
        aimAckGeneration_=aim_.generation();aimAckCommand_=aimCommand_;aimAckLease_=aim_.lease();
        aimPublished_=aim_.generation();
        std::fprintf(stderr,"APPLE_IOS_AIM_STATE command=%u lease=%u generation=%u event=%s x=%d y=%d lt=%d rt=%d monotonic_ms=%llu deadline_ms=%llu physical_input=0\n",
            aimCommand_,aim_.lease(),aimPublished_,eventName(aim_.event()),state.rightX,state.rightY,state.leftTrigger,state.rightTrigger,
            (unsigned long long)now,(unsigned long long)aim_.deadline());
        return true;
    }
    bool readCommand(const std::string& filename,std::string& text) {
        std::array<char,513> bytes;std::size_t count=0;
        if(!readBoundedCommandFile(directory_,filename.c_str(),bytes,count))return false;
        text.assign(bytes.data(),count);return true;
    }
    void applyNative(const RecordedInputCommand& c) {
        NSArray<NSString*>* names=@[GCInputButtonA,GCInputButtonB,GCInputButtonX,GCInputButtonY,GCInputLeftShoulder,GCInputRightShoulder];
        constexpr unsigned masks[]{0x1000,0x2000,0x4000,0x8000,0x100,0x200};
        for(unsigned n=0;n<6;++n)[controller_ setValue:(c.buttons&masks[n])?1.0:0.0 forButtonElement:names[n]];
        [controller_ setPosition:CGPointMake(c.axes[0]/1000.0,c.axes[1]/1000.0) forDirectionPadElement:GCInputLeftThumbstick];
        [controller_ setPosition:CGPointMake(c.axes[2]/1000.0,c.axes[3]/1000.0) forDirectionPadElement:GCInputRightThumbstick];
        [controller_ setValue:c.axes[4]/1000.0 forButtonElement:GCInputLeftTrigger];
        [controller_ setValue:c.axes[5]/1000.0 forButtonElement:GCInputRightTrigger];
    }
    bool apply(const RecordedInputCommand& c) {
        // The real virtual profile supplies every other control. Only its
        // explicitly leased D-pad is supplemented, never a different owner.
        if(!dpad_ || !dpad_->set(std::uint16_t(c.buttons&0x000fu)))return false;
        applyNative(c);return true;
    }
public:
    RecordedVirtualInput(GCVirtualController* controller,AppleGamepadInput* input,bool aimEnabled=false):
        controller_(controller),input_(input),aimEnabled_(aimEnabled) {
        if(input_)dpad_=input_->recordedDpad((__bridge void*)controller_);
        if(!dpad_ || !dpad_->active()) {
            closed_=true;policy_.close();
            std::fputs("APPLE_IOS_INPUT_REFUSED reason=dpad_owner physical_input=0\n",stderr);
            return;
        }
        nonce_=[[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString] UTF8String];
        const std::string library=[NSHomeDirectory() stringByAppendingPathComponent:@"Library"].UTF8String;
        const int parent=open(library.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(parent<0)return;
        if(mkdirat(parent,"PortInput",0700)!=0 && errno!=EEXIST){::close(parent);return;}
        const int root=openat(parent,"PortInput",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);::close(parent);if(root<0)return;
        if(mkdirat(root,nonce_.c_str(),0700)==0)directory_=openat(root,nonce_.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ::close(root);
        std::fprintf(stderr,"APPLE_IOS_INPUT_CHANNEL ready=%u session=%s relative=Library/PortInput/%s physical_input=0\n",
            unsigned(directory_>=0),nonce_.c_str(),nonce_.c_str());
        if(aimEnabled_)std::fprintf(stderr,"APPLE_IOS_AIM_CHANNEL ready=%u session=%s opt_in=1 max_lease_ms=30000\n",
            unsigned(directory_>=0),nonce_.c_str());
    }
    ~RecordedVirtualInput(){close();if(directory_>=0)::close(directory_);}
    bool neutral() const {
        recomp::input::GamepadState s{};
        return !holding_ && !aim_.active() && !closed_ && directory_>=0 && dpad_ && dpad_->active() && input_->snapshot(0,&s)==1 && !s.buttons &&
            !s.leftX && !s.leftY && !s.rightX && !s.rightY && !s.leftTrigger && !s.rightTrigger;
    }
    void close() {
        if(closed_)return;closed_=true;policy_.close();
        if(aimEnabled_ && aim_.generation()) {
            aim_.close();
            if(!publishAim(nowMs())) {
                // D-pad ownership loss cannot prevent releasing the controls
                // on OUR virtual controller. This is not a guest ACK.
                [controller_ setValue:0.0 forButtonElement:GCInputRightTrigger];
                applyNative({});
                std::fputs("APPLE_IOS_AIM_RELEASE native_only=1 guest_ack_unproven=1 reason=owner_lost\n",stderr);
            }
        }
        if(holding_){applyNative({});holding_=false;std::fprintf(stderr,"APPLE_IOS_INPUT_RELEASE command=%u reason=closed physical_input=0\n",command_);}
        if(dpad_)dpad_->close();
    }
    // Capture BEFORE reading the actual guest snapshot; comparing after that
    // read rejects a state fetched across a newer publication boundary.
    std::uint32_t guestReadToken() {
        if(!aimEnabled_)return 0;
        std::lock_guard lock(aimMutex_);return aimAckGeneration_;
    }
    // Called after logging that actual snapshot, never with a synthesized state.
    void observeGuest(std::uint32_t token,int connected,const recomp::input::GamepadState& s) {
        if(!aimEnabled_ || connected!=1)return;
        std::lock_guard lock(aimMutex_);
        if(!token || token!=aimAckGeneration_ || !aimPendingAck_ || s.buttons || s.leftX || s.leftY ||
           s.rightX!=gamepad::stick(aimExpected_.rightX/1000.0f) ||
           s.rightY!=gamepad::stick(aimExpected_.rightY/1000.0f) ||
           s.leftTrigger!=gamepad::trigger(aimExpected_.leftTrigger/1000.0f) ||
           s.rightTrigger!=gamepad::trigger(aimExpected_.rightTrigger/1000.0f))return;
        aimPendingAck_=false;
        std::fprintf(stderr,"APPLE_IOS_AIM_ACK generation=%u command=%u lease=%u packet=%u x=%d y=%d lt=%u rt=%u monotonic_ms=%llu\n",
            aimAckGeneration_,aimAckCommand_,aimAckLease_,s.packet,s.rightX,s.rightY,s.leftTrigger,s.rightTrigger,
            (unsigned long long)nowMs());
    }
    void tick(unsigned frame,bool active,bool terminal) {
        if(!active || terminal){close();return;}
        if(closed_ || directory_<0)return;
        if(!dpad_ || !dpad_->active()) {
            std::fputs("APPLE_IOS_INPUT_REFUSED reason=dpad_owner_lost physical_input=0\n",stderr);
            close();return;
        }
        if(aimEnabled_) {
            const auto now=nowMs();
            const bool connected=controller_.controller && !controller_.controller.snapshot;
            // Publish settle/expiry BEFORE admitting anything else so a later
            // action cannot erase evidence of the prior pulse's release.
            aim_.tick(now,active,connected);
            if(!publishAim(now) || aim_.closed()){close();return;}
            std::string bytes;
            if(readCommand("aim-"+std::to_string(aim_.next())+".txt",bytes)) {
                RecordedAimCommand c;
                if(parseRecordedAim(bytes,nonce_,c)) {
                    const bool release=c.operation==AimOperation::End;
                    // Zero is already safe: retain its pending generation so
                    // End cannot overwrite the only guest acknowledgement.
                    if(release && !aimAcknowledged() && aim_.state()==RecordedAimState{})return;
                    if(holding_ || (!release && !aimAcknowledged()) ||
                       !aim_.admit(c,std::uint64_t(std::time(nullptr)),now,frame,active,connected,neutral())) {
                        std::fprintf(stderr,"APPLE_IOS_AIM_REJECT command=%u lease=%u channel_closed=1\n",c.sequence,c.lease);
                        close();return; // Never leave a rejected shot queued for later.
                    }
                    aimCommand_=c.sequence;
                    std::fprintf(stderr,"APPLE_IOS_AIM_APPLY command=%u lease=%u operation=%s duration_ms=%u x=%d y=%d deadline_ms=%llu observed_frame=%u\n",
                        c.sequence,c.lease,eventName(aim_.event()),c.durationMs,c.x,c.y,(unsigned long long)aim_.deadline(),frame);
                    if(!publishAim(now)){close();return;}
                }
            }
            if(aim_.active() || !aimAcknowledged())return;
        }
        if(holding_) {
            if(std::chrono::steady_clock::now()>=releaseAt_) {
                if(!apply({})){close();return;}
                holding_=false;policy_.release();
                std::fprintf(stderr,"APPLE_IOS_INPUT_RELEASE command=%u reason=duration physical_input=0\n",command_);
            }
            return;
        }
        std::string bytes;if(!readCommand(std::to_string(policy_.next())+".txt",bytes))return;
        RecordedInputCommand c;if(!parseRecordedInput(bytes,nonce_,c))return;
        recomp::input::GamepadState state{};
        const bool connected=controller_.controller && !controller_.controller.snapshot && input_->snapshot(0,&state)==1;
        if(!policy_.admit(c,std::uint64_t(std::time(nullptr)),frame,active,connected))return;
        if(!apply(c)){close();return;}
        command_=c.sequence;holding_=true;releaseAt_=std::chrono::steady_clock::now()+std::chrono::milliseconds(c.holdMs);
        std::fprintf(stderr,"APPLE_IOS_INPUT_APPLY command=%u buttons=%04x axes=%d,%d,%d,%d,%d,%d hold_ms=%u observed_frame=%u physical_input=0\n",
            c.sequence,c.buttons,c.axes[0],c.axes[1],c.axes[2],c.axes[3],c.axes[4],c.axes[5],c.holdMs,frame);
    }
};
}
