// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MetalCapturePolicy.hpp"
#include "BoundedCommandFile.hpp"
#include "apple/storage_space.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>
namespace recomp::apple {
// Explicit developer opt-in only. No gameplay changes, capture at startup,
// uploads, automatic retry, arbitrary paths or indefinite capture requests.
// Main-thread serialized like the presenter. The time limit is checked by the
// UI timer; a blocked main/GPU call can delay stop. External supervisor remains
// necessary. Free-space checks are snapshots, not reservations/byte quotas.
class BoundedMetalCapture final {
    int directory_=-1;
    std::string nonce_,path_;
    MetalCapturePolicy policy_;
    bool ownsCapture_=false;
    static std::uint64_t nowMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();}
public:
    BoundedMetalCapture() {
        nonce_=[[[NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""] lowercaseString] UTF8String];
        const std::string library=[NSHomeDirectory() stringByAppendingPathComponent:@"Library"].UTF8String;
        path_=library+"/PortCapture/"+nonce_;
        const int parent=open(library.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(parent<0)return;
        if(mkdirat(parent,"PortCapture",0700)!=0 && errno!=EEXIST){::close(parent);return;}
        const int root=openat(parent,"PortCapture",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);::close(parent);if(root<0)return;
        if(mkdirat(root,nonce_.c_str(),0700)==0)directory_=openat(root,nonce_.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ::close(root);
        std::fprintf(stderr,"APPLE_IOS_CAPTURE_CHANNEL ready=%u session=%s relative=Library/PortCapture/%s one_shot=1 max_frames=2 deadline_ms=2000\n",
            unsigned(directory_>=0),nonce_.c_str(),nonce_.c_str());
    }
    ~BoundedMetalCapture(){close("destruction",0);if(directory_>=0)::close(directory_);}
    void close(const char* reason,unsigned frame) {
        policy_.close();
        if(!ownsCapture_)return;ownsCapture_=false;
        [MTLCaptureManager.sharedCaptureManager stopCapture];
        std::fprintf(stderr,"APPLE_IOS_CAPTURE_STOP reason=%s frame=%u relative=Library/PortCapture/%s/scene.gputrace contents_unverified=1\n",
            reason,frame,nonce_.c_str());
    }
    void tick(unsigned frame,bool foreground,bool terminal,bool inputNeutral) {
        if(!foreground || terminal){close("lifecycle",frame);return;}
        if(directory_<0)return;
        if(policy_.active()) {
            const auto space=storageSpace(path_.c_str());
            const auto available=space.valid?space.availableBytes:0;
            if(policy_.stopDue(frame,nowMs(),foreground,available))close("frame_time_or_space_bound",frame);
            return;
        }
        if(policy_.used())return;
        std::array<char,129> bytes;std::size_t count=0;
        if(!readBoundedCommandFile(directory_,"capture.txt",bytes,count))return;
        MetalCaptureRequest request;
        if(!parseMetalCapture(std::string_view(bytes.data(),count),nonce_,request))return;
        const auto space=storageSpace(path_.c_str());
        const auto available=space.valid?space.availableBytes:0;
        if(!policy_.admit(request,std::uint64_t(std::time(nullptr)),frame,nowMs(),foreground,inputNeutral,available))return;
        auto manager=MTLCaptureManager.sharedCaptureManager;
        const bool supported=[manager supportsDestination:MTLCaptureDestinationGPUTraceDocument];
        const std::string trace=path_+"/scene.gputrace";struct stat existing{};
        const bool absent=lstat(trace.c_str(),&existing)!=0 && errno==ENOENT;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!supported || manager.isCapturing || !absent || !device) {
            std::fprintf(stderr,"APPLE_IOS_CAPTURE_REFUSED supported=%u busy=%u absent=%u device=%u\n",
                unsigned(supported),unsigned(manager.isCapturing),unsigned(absent),unsigned(device!=nil));
            policy_.close();return;
        }
        MTLCaptureDescriptor* descriptor=[MTLCaptureDescriptor new];
        descriptor.captureObject=device;descriptor.destination=MTLCaptureDestinationGPUTraceDocument;
        descriptor.outputURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:trace.c_str()]];
        NSError* error=nil;ownsCapture_=[manager startCaptureWithDescriptor:descriptor error:&error];
        std::fprintf(stderr,"APPLE_IOS_CAPTURE_START success=%u frame=%u error=%ld available_bytes=%llu device=%s scene_queue_unverified=1\n",
            unsigned(ownsCapture_),frame,long(error.code),(unsigned long long)available,device.name.UTF8String);
        if(!ownsCapture_)policy_.close();
    }
};
}
