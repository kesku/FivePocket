// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <mutex>
#include <utility>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <cstdio>
#endif

namespace iosrendererqos {
inline constexpr std::uint32_t rendererToken=0x6408e000u;
enum class Event { Configured, Bound, BindRejected, IdentityFailed, Requested, Started,
                   StartFailed, Ended, EndFailed, Unbound, Closed };
struct Record {
    Event event;
    std::uint64_t threadId=0;
    int requestedClass=0, relativePriority=0, error=0;
};

#if defined(__APPLE__)
struct PublicApi {
    using Thread=pthread_t;
    using Handle=pthread_override_t;
    static Thread current() noexcept { return pthread_self(); }
    static std::uint64_t threadId(Thread thread) noexcept {
        std::uint64_t id=0;
        return pthread_threadid_np(thread,&id)==0?id:0;
    }
    static int requested(Thread thread,int& qos,int& relative) noexcept {
        qos_class_t value=QOS_CLASS_UNSPECIFIED;
        const int error=pthread_get_qos_class_np(thread,&value,&relative);
        qos=static_cast<int>(value);
        return error;
    }
    static Handle start(Thread thread) noexcept {
        return pthread_override_qos_class_start_np(thread,QOS_CLASS_USER_INTERACTIVE,0);
    }
    static int end(Handle handle) noexcept { return pthread_override_qos_class_end_np(handle); }
    static void report(const Record& r) noexcept {
        const char* names[]={"configured","bound","bind_rejected","identity_failed","requested",
            "started","start_failed","ended","end_failed","unbound","closed"};
        // requested_class is the baseline request, NOT effective QoS or CPU placement.
        std::fprintf(stderr,"APPLE_IOS_RENDERER_QOS event=%s thread_id=%llu requested_class=%d relative=%d error=%d\n",
            names[static_cast<unsigned>(r.event)],static_cast<unsigned long long>(r.threadId),
            r.requestedClass,r.relativePriority,r.error);
    }
};
#endif

// Api methods must be nonthrowing; report must not reenter this controller.
// Controller must outlive every Binding (the integration owns it for the session).
// Only the actual renderer guest task owns a Binding. Main drives transitions, never a draw hook.
template<class Api> class Controller {
public:
    using Thread=typename Api::Thread;
    using Handle=typename Api::Handle;
    struct Status { bool configured,desired,bound,active,failed,closed; std::uint64_t threadId; };
    class Binding {
        friend class Controller;
        Controller* owner_=nullptr;
        std::uint64_t generation_=0;
        Binding(Controller* owner,std::uint64_t generation):owner_(owner),generation_(generation){}
    public:
        Binding()=default;
        Binding(const Binding&)=delete;
        Binding& operator=(const Binding&)=delete;
        Binding(Binding&& other) noexcept
            :owner_(std::exchange(other.owner_,nullptr)),generation_(other.generation_){}
        Binding& operator=(Binding&& other) noexcept {
            if(this!=&other){reset();owner_=std::exchange(other.owner_,nullptr);generation_=other.generation_;}
            return *this;
        }
        ~Binding(){reset();}
        explicit operator bool() const noexcept {return owner_!=nullptr;}
        void reset() noexcept {
            if(auto* owner=std::exchange(owner_,nullptr))owner->unbind(generation_);
        }
    };
    Controller()=default;
    Controller(const Controller&)=delete;
    Controller& operator=(const Controller&)=delete;
    ~Controller(){close();}

    // Explicit opt-in. Disabling releases an active override without altering desired state.
    void configure(bool enabled=true) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_ || configured_==enabled)return;
        configured_=enabled; emit(Event::Configured); reconcile();
    }
    void setDesired(bool on) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_ || desired_==on)return;
        desired_=on;reconcile();
    }
    void poll(bool rootFlag) noexcept {setDesired(rootFlag);}
    Binding bind(std::uint32_t token) noexcept {
        if(token!=rendererToken)return {};
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_ || !configured_)return {};
        if(bound_){emit(Event::BindRejected);return {};}
        const Thread thread=Api::current();
        const std::uint64_t id=Api::threadId(thread);
        if(!id){emit(Event::IdentityFailed);return {};}
        thread_=thread;threadId_=id;bound_=true;++generation_;
        emit(Event::Bound);reconcile();
        return Binding(this,generation_);
    }
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_)return;
        closed_=true;desired_=false;endOverride();emit(Event::Closed);
    }
    Status status() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return {configured_,desired_,bound_,handle_!=Handle{},failed_,closed_,threadId_};
    }
private:
    mutable std::mutex mutex_;
    bool configured_=false,desired_=false,bound_=false,failed_=false,closed_=false;
    Thread thread_{};
    Handle handle_{};
    std::uint64_t threadId_=0,generation_=0;
    int requestedClass_=0,relativePriority_=0;
    void emit(Event event,int error=0) noexcept {
        Api::report({event,threadId_,requestedClass_,relativePriority_,error});
    }
    void reconcile() noexcept { // mutex held throughout API calls, including cross-thread close.
        if(!configured_ || !desired_ || closed_ || !bound_){endOverride();return;}
        if(handle_!=Handle{} || failed_)return;
        const int error=Api::requested(thread_,requestedClass_,relativePriority_);
        emit(Event::Requested,error);
        if(error){failed_=true;emit(Event::StartFailed,error);return;}
        handle_=Api::start(thread_);
        if(handle_==Handle{}){failed_=true;emit(Event::StartFailed,-1);return;}
        emit(Event::Started);
    }
    void endOverride() noexcept {
        if(handle_==Handle{})return;
        // Consume once even on error: retrying an ambiguously consumed native handle is unsafe.
        const Handle handle=std::exchange(handle_,Handle{});
        const int error=Api::end(handle);
        if(error){failed_=true;emit(Event::EndFailed,error);}else emit(Event::Ended);
    }
    void unbind(std::uint64_t generation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!bound_ || generation!=generation_)return;
        endOverride();emit(Event::Unbound);bound_=false;thread_=Thread{};threadId_=0;
    }
};
#if defined(__APPLE__)
// The runtime's task bindings can outlive a stopped view; retain the owner.
inline Controller<PublicApi>& experiment() {
    static auto* value=new Controller<PublicApi>;
    return *value;
}
#endif
} // namespace iosrendererqos
