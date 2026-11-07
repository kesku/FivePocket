// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MeasurementLaunch.hpp"
#include "apple/measurement_session.h"
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <stdexcept>

namespace recomp::apple {
namespace measurement_setup {
struct Descriptor {
    int value=-1;
    explicit Descriptor(int fd):value(fd) {}
    ~Descriptor() {if(value>=0)::close(value);}
    Descriptor(const Descriptor&)=delete;
    int release() {const int fd=value;value=-1;return fd;}
};
struct FileCloser {void operator()(FILE* file) const {if(file)std::fclose(file);}};
using File=std::unique_ptr<FILE,FileCloser>;
inline File output(int directory,const char* name) {
    Descriptor fd(openat(directory,name,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600));
    if(fd.value<0)throw std::runtime_error("measurement output creation failed; no retry/overwrite");
    FILE* file=fdopen(fd.value,"wb");
    if(!file)throw std::runtime_error("measurement stream setup failed; partial output preserved");
    fd.release();return File(file);
}
// The descriptor is borrowed. Keeping directory selection outside this function
// lets host tests exercise the real output path in a test-owned directory.
inline std::unique_ptr<performance::MeasurementSession> prepareAt(
        int libraryDirectory,const std::vector<std::string_view>& arguments,
        std::shared_ptr<performance::DisplayRecording> recording={},
        std::shared_ptr<performance::DisplayFollowupControl> followup={}) {
    const auto launch=measurementLaunch(arguments);
    if(launch.request==MeasurementRequest::Disabled) {
        if(recording)throw std::runtime_error("unexpected display recorder without explicit measurement");
        return {};
    }
    if(launch.request!=MeasurementRequest::Enabled)throw std::runtime_error("invalid explicit measurement request");
    if(libraryDirectory<0 || (mkdirat(libraryDirectory,"PortMeasurements",0700)!=0 && errno!=EEXIST))
        throw std::runtime_error("measurement root unavailable");
    Descriptor root(openat(libraryDirectory,"PortMeasurements",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(root.value<0 || mkdirat(root.value,launch.id.c_str(),0700)!=0)
        throw std::runtime_error("measurement ID exists or cannot be created; not overwritten");
    Descriptor directory(openat(root.value,launch.id.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(directory.value<0)throw std::runtime_error("measurement directory unavailable");
    auto frames=output(directory.value,"frames.jsonl"),counters=output(directory.value,"counters.jsonl");
    auto display=recording?output(directory.value,"display.jsonl"):File{};
    // operator new allocates before evaluating release() arguments; constructor
    // assumes stream ownership even when validation/allocation then throws.
    auto result=std::unique_ptr<performance::MeasurementSession>(new performance::MeasurementSession(
        frames.release(),counters.release(),display.release(),std::move(recording),launch.id.c_str(),"other",30,false,
        [](void*,performance::MeasurementSession::Status state) noexcept {
            std::fprintf(stderr,"APPLE_MEASUREMENT_END output_ok=%u frames_clean=%u samples=%llu physical_unproven=1\n",
                unsigned(state.outputSucceeded),unsigned(state.frameRecordsClean),(unsigned long long)state.samples);
        }));
    if(followup) {
        auto extra=output(directory.value,"display-followup.jsonl");
        result->setDisplayFollowup(extra.release(),std::move(followup));
    }
    std::fprintf(stderr,"APPLE_MEASUREMENT_START id=%s relative=Library/PortMeasurements/%s purpose=exploratory target_not_effective_cap=30\n",
        launch.id.c_str(),launch.id.c_str());
    return result;
}
}
// Before guest start only; no default activation or mutation of game arguments.
// Creates a new private diagnostic directory, never removes/reuses any path.
inline std::unique_ptr<performance::MeasurementSession> preparePerformanceMeasurement(
        std::shared_ptr<performance::DisplayRecording> recording={},
        std::shared_ptr<performance::DisplayFollowupControl> followup={}) {
    std::vector<std::string_view> arguments;
    for(NSString* arg in NSProcessInfo.processInfo.arguments)arguments.emplace_back(arg.UTF8String);
    const auto launch=measurementLaunch(arguments);
    if(launch.request==MeasurementRequest::Disabled) {
        if(recording)throw std::runtime_error("unexpected display recorder without explicit measurement");
        return {};
    }
    if(launch.request!=MeasurementRequest::Enabled)throw std::runtime_error("invalid explicit measurement request");
    const std::string library=[NSHomeDirectory() stringByAppendingPathComponent:@"Library"].UTF8String;
    measurement_setup::Descriptor parent(open(library.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    return measurement_setup::prepareAt(parent.value,arguments,std::move(recording),std::move(followup));
}
}
