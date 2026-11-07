// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
namespace iosasyncworkerqos {
inline bool parse(const char* value) {
    if(!value || !std::strcmp(value,"0"))return false;
    if(!std::strcmp(value,"1"))return true;
    throw std::invalid_argument("RECOMP_IOS_ASYNC_WORKER_QOS requires exact0 or1");
}
inline bool startupPolicy(const char* qos,const char* async,const char* queued) {
    const bool enabled=parse(qos);
    if(enabled && (!async || std::strcmp(async,"1") || !queued || std::strcmp(queued,"0")))
        throw std::invalid_argument("async worker QoS requires async1 and queued0");
    return enabled;
}
struct Record {const char* event;std::uint64_t threadId;int requestedClass,relative,targetClass,error;};
// All Api operations are nonthrowing. Only the native Pipeline entry binds;
// owner/deadline close may run on another thread, and the owner outlives both.
template<class Api> class Controller {
public:
    using Handle=typename Api::Handle;
    bool configure(bool enabled) noexcept {
        std::lock_guard lock(mutex_);
        if(closed_)return false;
        enabled_=enabled;return enabled_;
    }
    bool enter() noexcept {
        std::lock_guard lock(mutex_);
        if(!enabled_ || closed_ || bound_)return false;
        const auto thread=Api::current();const auto tid=Api::threadId(thread);
        if(!tid){emit("identity_failed",-1);return false;}
        threadId_=tid;const int error=Api::requested(thread,requestedClass_,relative_);
        emit("requested",error);
        if(error){emit("start_failed",error);return false;}
        handle_=Api::start(thread);
        if(handle_==Handle{}){emit("start_failed",-1);return false;}
        bound_=true;emit("started",0);return true;
    }
    void leave() noexcept {
        std::lock_guard lock(mutex_);
        if(!bound_)return;
        end();bound_=false;emit("unbound",0);
    }
    void close() noexcept {
        std::lock_guard lock(mutex_);
        if(closed_)return;
        closed_=true;end();emit("closed",0);
    }
private:
    std::mutex mutex_;bool enabled_=false,closed_=false,bound_=false;
    Handle handle_{};std::uint64_t threadId_=0;int requestedClass_=0,relative_=0;
    void emit(const char* event,int error) noexcept {Api::report({event,threadId_,requestedClass_,relative_,Api::targetClass(),error});}
    void end() noexcept {
        if(handle_==Handle{})return;
        const auto handle=handle_;handle_=Handle{};
        const int error=Api::end(handle);emit(error?"end_failed":"ended",error);
    }
};
#if defined(__APPLE__)
struct PublicApi {
    using Handle=pthread_override_t;
    static pthread_t current() noexcept{return pthread_self();}
    static std::uint64_t threadId(pthread_t t) noexcept{std::uint64_t id=0;return pthread_threadid_np(t,&id)==0?id:0;}
    static int targetClass() noexcept{return int(QOS_CLASS_USER_INITIATED);}
    static int requested(pthread_t t,int& qos,int& rel) noexcept {qos_class_t q=QOS_CLASS_UNSPECIFIED;int e=pthread_get_qos_class_np(t,&q,&rel);qos=int(q);return e;}
    static Handle start(pthread_t t) noexcept{return pthread_override_qos_class_start_np(t,QOS_CLASS_USER_INITIATED,0);}
    static int end(Handle h) noexcept{return pthread_override_qos_class_end_np(h);}
    static void report(const Record&r) noexcept{std::fprintf(stderr,"APPLE_IOS_ASYNC_WORKER_QOS event=%s thread_id=%llu requested_class=%d relative=%d target_class=%d error=%d\n",r.event,(unsigned long long)r.threadId,r.requestedClass,r.relative,r.targetClass,r.error);}
};
inline Controller<PublicApi>& controller(){static auto* p=new Controller<PublicApi>;return *p;}
inline bool configureFromEnvironment(){return controller().configure(startupPolicy(std::getenv("RECOMP_IOS_ASYNC_WORKER_QOS"),std::getenv("RECOMP_IOS_RENDERER_ASYNC"),std::getenv("RECOMP_IOS_QUEUED_PRESENT")));}
inline bool enter(void* context) noexcept{return static_cast<Controller<PublicApi>*>(context)->enter();}
inline void leave(void* context) noexcept{static_cast<Controller<PublicApi>*>(context)->leave();}
inline void close() noexcept{controller().close();}
#endif
}
