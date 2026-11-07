// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace iosrendererasync {
enum class Marker {SyncRenderer,SyncPresent,SubmitProxy,SyncReadback};
struct Configuration { bool requested; int forcedMode; bool queuedPresent; bool shared; };
inline bool parse(const char* value,const char* name) {
    if(!value || std::strcmp(value,"0")==0)return false;
    if(std::strcmp(value,"1")==0)return true;
    throw std::invalid_argument(name);
}
// Startup only. The existing V177 mailbox is unchanged; queued1 deliberately
// delivers a completed CPU frame and its resolve writebacks one present later.
// Every rejection check precedes mutation and the existing mode callback.
template<class SetMode,class MarkerPresent>
Configuration configure(SetMode&& setMode,MarkerPresent&& present) {
    const bool requested=parse(std::getenv("RECOMP_IOS_RENDERER_ASYNC"),"invalid RECOMP_IOS_RENDERER_ASYNC");
    const bool shared=parse(std::getenv("RECOMP_IOS_SHARED_DEVICE_PRESENT"),"invalid RECOMP_IOS_SHARED_DEVICE_PRESENT");
    const bool queued=parse(std::getenv("RECOMP_IOS_QUEUED_PRESENT"),"invalid RECOMP_IOS_QUEUED_PRESENT");
    const char* expected=queued?"1":"0";
    const char* existing=std::getenv("RECOMP_TEST_QUEUED_PRESENT");
    if(existing && std::strcmp(existing,expected)!=0)
        throw std::invalid_argument("RECOMP_TEST_QUEUED_PRESENT conflicts with requested mode");
    const bool combined=requested && shared;
    if(combined && present(Marker::SubmitProxy))
        throw std::invalid_argument("async shared qualification requires direct worker submit");
    if(combined && !present(Marker::SyncReadback))
        throw std::invalid_argument("async shared qualification requires diag-generic-sync");
    if(queued && (!requested || shared))throw std::invalid_argument("queued present requires async1 shared0");
    if(requested && present(Marker::SyncRenderer))
        throw std::invalid_argument("diag-generic-sync-renderer forbids async renderer");
    if(queued && present(Marker::SyncPresent))
        throw std::invalid_argument("diag-generic-sync-present forbids queued present");
    if(queued && present(Marker::SubmitProxy))
        throw std::invalid_argument("diag-generic-submit-proxy forbids queued present");
    if(queued && !present(Marker::SyncReadback))
        throw std::invalid_argument("queued present qualification requires diag-generic-sync");
    if(::setenv("RECOMP_TEST_QUEUED_PRESENT",expected,1)!=0)
        throw std::runtime_error("queued present override failed");
    const char* actual=std::getenv("RECOMP_TEST_QUEUED_PRESENT");
    if(!actual || std::strcmp(actual,expected)!=0)
        throw std::runtime_error("queued present override readback failed");
    const int mode=requested?1:0;
    std::forward<SetMode>(setMode)(mode);
    return {requested,mode,queued,shared};
}
}
