#pragma once
// Attempt v111 (change 1 of docs/fable/PERF_PLAN.md): the queue between the guest threads that call the generic
// renderer (producers: the D3D hooks) and the renderer thread (one consumer, "the worker") that executes what they
// captured. Header-only and renderer-agnostic so that host tests drive it directly (tests/async_queue_test.cpp).
//
// Contract (the renderer relies on each point; the stress test checks each one):
//  1. FIFO: items execute on the worker one at a time, in push order (one global order over every producer).
//  2. Bounded: push() of a bounded item waits while `depth` bounded items are queued or executing; unbounded items
//     (small control items that must keep their place in the order, e.g. texture unlocks from other threads) never
//     wait, so a producer that is not the render thread can never be blocked by the worker.
//  3. Drain: drain() returns only after every item pushed before the call (by any thread) has finished executing.
//     The renderer drains before it reads or publishes anything the worker produced (present, submitPending), and at
//     every guest ring submission before the fences it carries retire (guestReadBarrier): an item's guest-memory
//     reads therefore happen before the game can learn that the GPU is done with that memory.
//  4. Proxy: the worker never waits on a producer except through runOnWaiter(): the work is run by a producer that is
//     itself blocked in push() or drain() (or by the next one to block). The renderer uses it for vkQueueSubmit, which
//     must stay on the guest threads that submitted in v109 (the VkQueue is shared with the native engine, whose
//     submissions are serialized with those threads by their permit domain, not by any lock the worker could take).
//     Every producer-side wait services the proxy, so a worker blocked in runOnWaiter() cannot deadlock a waiter.
//  5. Wakeups: a producer signals the worker only when it sleeps; the worker spins `spin` before sleeping (the render
//     thread's gap between draws is ~20 us), and signals producers only when one waits for what just happened.
// Nothing here allocates per item: items are pointers the caller owns (the renderer recycles its packets).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#if defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__)
#include <pthread.h>
#endif

// v151 host tests: the default drain spin of a Pipeline built without one (the renderer always passes its own).
#if !defined(RECOMP_ASYNC_QUEUE_DEFAULT_DRAIN_SPIN_NS)
#define RECOMP_ASYNC_QUEUE_DEFAULT_DRAIN_SPIN_NS 0
#endif
#if defined(__ANDROID__)
#include <unistd.h>
extern "C" void recomp_register_stop_yield_thread(int tid); // v241 (guest_cooperative_executor.cpp)
#endif
namespace recomp::gpu::generic::async {
using Clock=std::chrono::steady_clock;
inline std::uint64_t nanos(Clock::time_point a,Clock::time_point b){return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());}
// A CPU hint for a spin-wait iteration (no system call).
inline void relax() {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

struct Item {void* payload=nullptr;unsigned kind=0;bool bounded=true;std::uint64_t seq=0;};
// v210 A/B (files/private-trace/diag-generic-v210-baseline, applied by the renderer every 60 presents): v208's busy time,
// two clock reads around every item, instead of v209's busy periods.
inline std::atomic<bool> perItemTiming{false};

// Counters since the last takeStats() (all under the queue mutex; the worker's busy time is its own).
struct Stats {
    std::uint64_t pushed=0,executed=0,stalls=0,stallNs=0,drains=0,drainNs=0,drainsIdle=0,proxies=0,proxyNs=0,
        busyNs=0,wakes=0,sleeps=0,maxDepth=0,depthSum=0,depthSamples=0;
    std::uint64_t drainSpins=0,drainSpinHits=0; // v151: drains that spun before sleeping, and those done while spinning
};

struct WorkerScopeHooks {void* context=nullptr;bool(*enter)(void*) noexcept=nullptr;void(*leave)(void*) noexcept=nullptr;};

class Pipeline {
public:
    using Execute=std::function<void(const Item&)>;
    // depth: bounded items in flight at most (queued + executing); spin: how long an idle worker polls before sleeping.
    // drainSpin (v151): how long drain() polls (unlocked) for its target before it sleeps; 0 = sleep at once (v150).
    Pipeline(Execute execute,unsigned depth,std::chrono::nanoseconds spin,const char* threadName="recomp-render-w",
             std::chrono::nanoseconds drainSpin=std::chrono::nanoseconds{RECOMP_ASYNC_QUEUE_DEFAULT_DRAIN_SPIN_NS},
             WorkerScopeHooks workerScope={})
        :execute_(std::move(execute)),depth_(std::max(1u,depth)),spin_(spin),drainSpin_(drainSpin) {
        worker_=std::thread([this,threadName,workerScope]{
#if defined(__ANDROID__) || defined(__linux__)
            pthread_setname_np(pthread_self(),threadName);
#elif defined(__APPLE__)
            pthread_setname_np(threadName);
#endif
            onWorker()=true;
            struct Scope {WorkerScopeHooks hooks;bool entered;~Scope(){if(entered && hooks.leave)hooks.leave(hooks.context);}};
            Scope scope{workerScope,workerScope.enter && workerScope.leave && workerScope.enter(workerScope.context)};
#if defined(__ANDROID__)
            recomp_register_stop_yield_thread(int(gettid())); // v241
#endif
            run();});
    }
    Pipeline(const Pipeline&)=delete;Pipeline& operator=(const Pipeline&)=delete;
    ~Pipeline(){stop();}
    unsigned depth()const{return depth_;}
    std::chrono::nanoseconds spin()const{return spin_;}
    std::chrono::nanoseconds drainSpin()const{return drainSpin_;} // v151
    // True on the worker thread (any pipeline's).
    static bool& onWorker(){static thread_local bool flag=false;return flag;}
    // v331: diag-v331-baseline (set by the renderer's window poll) = one item per lock, as before.
    static std::atomic<bool>& batchBaseline(){static std::atomic<bool> flag{false};return flag;}
    // submit-early (package Q): a hook the worker calls when the queue stayed empty for the whole spin, just before it sleeps,
    // without the queue mutex (producers keep pushing meanwhile). It returns true when it did work (its time then counts as
    // busy). It must never BLOCK on a lock a producer may hold while that producer waits for the worker (in push() on a full
    // queue, or in drain()): the renderer's hook only try-locks rendererMutex (the queue lock it then takes is the one every
    // worker item already takes, v177). Unset (the default): v419's loop. No item runs while it runs, so FIFO and the drain
    // contract are unchanged.
    using IdleHook=bool(*)(void*);
    void setIdleHook(IdleHook hook,void* context) {
        idleContext_.store(context,std::memory_order_relaxed);idleHook_.store(hook,std::memory_order_release);
    }
    // vk-worker-3 (diag-vkw3-prefetch): a hook the worker calls, without the mutex, just before it executes each item, with the
    // items that follow that item in the queue order (at most two: the rest of its batch, then the items that were still queued
    // when the batch was taken, remembered under the mutex). The renderer prefetches their packets. Every one of those items
    // was pushed (and its payload written) before the batch was taken, and none runs before the item about to execute; the hook
    // must only READ what an item owns and must not change anything any item reads. Unset (the default): v427's loop.
    using AheadHook=void(*)(void*,const Item* next,unsigned count);
    void setAheadHook(AheadHook hook,void* context) {
        aheadContext_.store(context,std::memory_order_relaxed);aheadHook_.store(hook,std::memory_order_release);
    }

    // Queues `item` (its seq is assigned here). A bounded item waits for room first (servicing the proxy meanwhile).
    // Returns the item's sequence number (1, 2, ...), 0 when the pipeline is stopping (the item was not queued).
    std::uint64_t push(Item item) {
        std::unique_lock lock(m_);
        if(stopping_)return 0;
        if(item.bounded && boundedInFlight_>=depth_) {
            const auto t0=Clock::now();++stats_.stalls;
            ++waiters_;attention_.fetch_add(1,std::memory_order_seq_cst); // (v331: the worker publishes per item while set)
            while(!stopping_ && boundedInFlight_>=depth_)if(!serviceProxy(lock))producers_.wait(lock);
            --waiters_;attention_.fetch_sub(1,std::memory_order_seq_cst);
            stats_.stallNs+=nanos(t0,Clock::now());
            if(stopping_)return 0;
        }
        item.seq=++pushedSeq_;
        if(item.bounded)++boundedInFlight_;
        queue_.push_back(item);
        ++stats_.pushed;
        const std::uint64_t depth=queue_.size()+(busy_?1:0);
        stats_.maxDepth=std::max(stats_.maxDepth,depth);stats_.depthSum+=depth;++stats_.depthSamples;
        pushedAtomic_.store(pushedSeq_,std::memory_order_release);
        if(workerSleeping_){workerSleeping_=false;++stats_.wakes;lock.unlock();worker_cv_.notify_one();}
        return item.seq;
    }
    // Waits until every item pushed before this call finished executing (the proxy is serviced meanwhile).
    // Returns the nanoseconds waited. The worker must not call it (it would wait for itself).
    std::uint64_t drain(){return drainTo(0);}
    // v177: waits until the items up to sequence `upTo` (push()'s return value) finished executing; later items keep
    // running. 0: every item pushed before the call (drain()). Same waiting, spinning and proxy service as drain().
    std::uint64_t executed() const noexcept {return executedAtomic_.load(std::memory_order_acquire);} // v211
    std::uint64_t pushedSeq() const noexcept {return pushedAtomic_.load(std::memory_order_acquire);} // occlusion: the last pushed item's seq
    std::uint64_t drainTo(std::uint64_t upTo) {
        if(onWorker())return 0; // (the worker never waits for itself)
        std::unique_lock lock(m_);
        ++stats_.drains;
        const std::uint64_t target=upTo?std::min(upTo,pushedSeq_):pushedSeq_;
        if(doneThrough()>=target && !proxy_){++stats_.drainsIdle;return 0;}
        const auto t0=Clock::now();
        ++waiters_;drainTargets_.push_back(target);attention_.fetch_add(1,std::memory_order_seq_cst); // (v331)
        if(drainSpin_.count()>0 && doneThrough()<target && !stoppedWorker_ && !proxy_) {
            // v151: registered as a waiter first (a proxy posted now names this drain), then poll the worker's progress
            // unlocked; a posted proxy ends the spin so the loop below runs it (contract 4). Only latency changes: the
            // loop below re-checks everything under the mutex.
            ++stats_.drainSpins;
            lock.unlock();
            const auto until=Clock::now()+drainSpin_;
            for(unsigned k=1;executedAtomic_.load(std::memory_order_acquire)<target &&
                    !proxyPosted_.load(std::memory_order_acquire);++k) {
                relax();
                if(!(k&63) && Clock::now()>=until)break;
            }
            lock.lock();
            if(doneThrough()>=target)++stats_.drainSpinHits;
#if defined(RECOMP_ASYNC_QUEUE_MUTANT_SPIN_NO_RECHECK)
            // Test mutant: a spin that timed out counts as done (drain() returns before its items ran).
            drainTargets_.erase(std::find(drainTargets_.begin(),drainTargets_.end(),target));--waiters_;attention_.fetch_sub(1,std::memory_order_seq_cst);return 0;
#endif
        }
        while(doneThrough()<target && !stoppedWorker_)if(!serviceProxy(lock))producers_.wait(lock);
        drainTargets_.erase(std::find(drainTargets_.begin(),drainTargets_.end(),target));
        --waiters_;attention_.fetch_sub(1,std::memory_order_seq_cst);
        const auto ns=nanos(t0,Clock::now());stats_.drainNs+=ns;
        return ns;
    }
    // Worker only: runs `work` on a producer blocked in push()/drain() (or the next one to block) and returns once it
    // ran. False (and `work` not run) when the pipeline stops first.
    bool runOnWaiter(const std::function<void()>& work) {
        std::unique_lock lock(m_);
        if(stopping_ && !waiters_)return false;
        Proxy p{&work};
        proxy_=&p;++stats_.proxies;proxyPosted_.store(true,std::memory_order_release);
        const auto t0=Clock::now();
        producers_.notify_all();
        while(!p.done && !(stopping_ && !waiters_))worker_cv_.wait(lock);
        if(proxy_==&p)proxy_=nullptr;
        proxyPosted_.store(proxy_!=nullptr,std::memory_order_release);
        stats_.proxyNs+=nanos(t0,Clock::now());
        return p.done;
    }
    // Runs the worker's posted proxy now if there is one (a producer waiting for something else, e.g. a lock the worker
    // holds while it waits in runOnWaiter). True when one ran.
    bool servicePending(){std::unique_lock lock(m_);return serviceProxy(lock);}
    Stats takeStats(){std::lock_guard lock(m_);Stats s=stats_;s.busyNs=busyNs_.exchange(0,std::memory_order_relaxed);stats_=Stats{};return s;}
    // Items pushed and not yet finished (queued + executing).
    std::uint64_t inFlight(){std::lock_guard lock(m_);return pushedSeq_-executedSeq_;}
    // Drains, then stops and joins the worker. Items pushed after stop() began are refused (push returns 0).
    void stop() {
        {std::unique_lock lock(m_);if(stopped_)return;}
        drain();
        {std::lock_guard lock(m_);stopping_=true;stoppingHint_.store(true,std::memory_order_relaxed);workerSleeping_=false;}
        worker_cv_.notify_all();producers_.notify_all();
        if(worker_.joinable())worker_.join();
        std::lock_guard lock(m_);stopped_=true;
    }
private:
    struct Proxy {const std::function<void()>* work=nullptr;bool done=false;};
    // Runs a posted proxy (the mutex is released while it runs). True when one ran.
    bool serviceProxy(std::unique_lock<std::mutex>& lock) {
#if defined(RECOMP_ASYNC_QUEUE_MUTANT_NO_PROXY_SERVICE)
        (void)lock;return false; // Test mutant: waiters never run the worker's proxy (a blocked worker deadlocks drain()).
#endif
        if(!proxy_ || proxy_->done || proxyRunning_)return false;
        Proxy* p=proxy_;proxyRunning_=true;
        lock.unlock();
        (*p->work)();
        lock.lock();
        proxyRunning_=false;p->done=true;proxy_=nullptr;proxyPosted_.store(false,std::memory_order_release);
        worker_cv_.notify_all();
        return true;
    }
    void run() {
        std::unique_lock lock(m_);
        // v209: busy time per busy period (from the first item after an empty queue until the queue is empty again, and
        // flushed every 64 items so a saturated worker still reports per window), not two clock reads per item (4.9% of
        // the worker's samples were clock reads while driving, v207). It now includes the queue's own lock/unlock between
        // items (a few hundred ns per item at most).
        bool inBusy=false;Clock::time_point busyStart{};unsigned busyItems=0;
        const auto endBusy=[&]{if(inBusy){busyNs_.fetch_add(nanos(busyStart,Clock::now()),std::memory_order_relaxed);inBusy=false;}};
        for(;;) {
            if(queue_.empty() && !stopping_) {
                endBusy();
                // Spin (unlocked) for the next push before sleeping; a push during the spin needs no signal.
                const auto seen=pushedSeq_;
                lock.unlock();
                const auto until=Clock::now()+spin_;
                for(unsigned k=0;pushedAtomic_.load(std::memory_order_acquire)==seen;++k) {
                    relax();
                    if(!(k&63) && Clock::now()>=until)break;
                }
                if(const IdleHook hook=idleHook_.load(std::memory_order_acquire);
                   hook && !stoppingHint_.load(std::memory_order_relaxed) && pushedAtomic_.load(std::memory_order_acquire)==seen) {
                    const auto t0=Clock::now(); // submit-early: the queue stayed empty for the whole spin
                    if(hook(idleContext_.load(std::memory_order_relaxed)))busyNs_.fetch_add(nanos(t0,Clock::now()),std::memory_order_relaxed);
                }
                lock.lock();
                while(queue_.empty() && !stopping_) {
                    workerSleeping_=true;++stats_.sleeps;
                    worker_cv_.wait(lock);
                }
                workerSleeping_=false;
            }
            if(queue_.empty() && stopping_)break;
            // v331: up to BatchMax queued items per lock (diag-v331-baseline / batchBaseline: one, as before). Each item's
            // completion is stored in executedAtomic_ as soon as it ran (drains count it: doneThrough); the locked
            // bookkeeping (executedSeq_, the bounded count, wakes) is published at the batch's end, or after each item while
            // a producer waits (attention_). A missed hint only delays a waiter to the batch's end (always published).
            const unsigned n=batchBaseline().load(std::memory_order_relaxed)?1u:unsigned(std::min<std::size_t>(queue_.size(),BatchMax));
            for(unsigned b=0;b<n;++b){batch_[b]=queue_.front();queue_.pop_front();}
            aheadPeek(); // vk-worker-3 (under the mutex; a call only: this loop's control flow and PGO record unchanged)
            unsigned published=0;
            busy_=true;
            lock.unlock();
            for(unsigned b=0;b<n;++b) {
            const Item item=batch_[b];
            const bool itemTimed=perItemTiming.load(std::memory_order_relaxed); // v210 A/B
            const auto t0=itemTimed?Clock::now():Clock::time_point{};
            if(itemTimed)endBusy();
            else if(!inBusy){busyStart=Clock::now();inBusy=true;busyItems=0;}
            else if(!(++busyItems&63)){const auto now=Clock::now();busyNs_.fetch_add(nanos(busyStart,now),std::memory_order_relaxed);busyStart=now;}
#if defined(RECOMP_ASYNC_QUEUE_MUTANT_EARLY_COMPLETE)
            // Test mutant: the item counts as executed before it runs (drain() can return too early).
            {std::lock_guard l(m_);executedSeq_=item.seq;executedAtomic_.store(item.seq,std::memory_order_release);producers_.notify_all();}
#endif
#if defined(RECOMP_SE_MUTANT_HOOK_EVERY_ITEM) // test mutant: the idle hook also runs between queued items (a busy queue)
            if(const IdleHook hook=idleHook_.load(std::memory_order_acquire))hook(idleContext_.load(std::memory_order_relaxed));
#endif
            aheadCall(b,n); // vk-worker-3 (unlocked; a call only)
            execute_(item);
            if(itemTimed)busyNs_.fetch_add(nanos(t0,Clock::now()),std::memory_order_relaxed);
            executedAtomic_.store(item.seq,std::memory_order_seq_cst); // v151 (v331: per item, before the locked publish)
            if(b+1<n && attention_.load(std::memory_order_seq_cst)){lock.lock();publishLocked(published,b+1);lock.unlock();}
            }
            lock.lock();
            busy_=false;
            publishLocked(published,n);
        }
        endBusy();
        stoppedWorker_=true;
        producers_.notify_all();
    }
    // v331: the locked bookkeeping of batch_[published, upTo) (under m_), then the wakes the item-at-a-time loop did.
    void publishLocked(unsigned& published,unsigned upTo) {
        bool wake=false;
        for(;published<upTo;++published) {
            const Item& item=batch_[published];
            executedSeq_=item.seq;++stats_.executed;
            const bool freed=item.bounded && boundedInFlight_-- >= depth_;
            wake=wake || (freed && waiters_);
        }
        for(const auto t:drainTargets_)wake=wake || t<=executedSeq_;
        if(wake)producers_.notify_all();
    }
    // vk-worker-3: under m_, right after a batch was taken: the first two items still queued (they run right after the batch).
    [[gnu::noinline]] void aheadPeek() noexcept {
        peekCount_=0;
        if(!aheadHook_.load(std::memory_order_relaxed))return;
#if defined(RECOMP_VKW3_TEST_NO_PEEK) // host tests: the hook sees only the rest of each batch
        return;
#endif
        for(std::size_t k=0;k<queue_.size() && k<peek_.size();++k)peek_[peekCount_++]=queue_[k];
    }
    // vk-worker-3: without m_, before batch_[b] executes: the hook with the (up to two) items that follow it.
    [[gnu::noinline]] void aheadCall(unsigned b,unsigned n) noexcept {
        const AheadHook hook=aheadHook_.load(std::memory_order_acquire);
        if(!hook)return;
        Item next[2];unsigned c=0;
        for(unsigned k=b+1;k<n && c<2;++k)next[c++]=batch_[k];
        for(unsigned k=0;k<peekCount_ && c<2;++k)next[c++]=peek_[k];
        hook(aheadContext_.load(std::memory_order_relaxed),next,c);
    }
    // Every item up to this sequence has finished executing (the locked count, or the worker's newer per-item store).
    std::uint64_t doneThrough() const noexcept {return std::max(executedSeq_,executedAtomic_.load(std::memory_order_seq_cst));}
    static constexpr unsigned BatchMax=8;
    std::array<Item,BatchMax> batch_{}; // (worker only)
    std::atomic<unsigned> attention_{0}; // v331: producers waiting in push()/drain() (the worker then publishes per item)
    Execute execute_;
    const unsigned depth_;const std::chrono::nanoseconds spin_,drainSpin_;
    alignas(64) std::mutex m_; // v292: own line start (the members below are written under it)
    std::condition_variable worker_cv_,producers_;
    std::deque<Item> queue_;
    std::uint64_t pushedSeq_=0,executedSeq_=0;unsigned boundedInFlight_=0;
    std::atomic<std::uint64_t> pushedAtomic_{0};
    std::atomic<std::uint64_t> executedAtomic_{0}; // v151: executedSeq_ mirror (release after each item)
    std::atomic<bool> proxyPosted_{false};         // v151: proxy_ != nullptr mirror (ends a drain's spin)
    std::atomic<IdleHook> idleHook_{nullptr};std::atomic<void*> idleContext_{nullptr}; // submit-early (setIdleHook)
    std::atomic<bool> stoppingHint_{false};        // submit-early: stopping_ for the unlocked hook check
    alignas(64) std::atomic<std::uint64_t> busyNs_{0}; // v292: the worker's unlocked counter, off the locked fields' lines
    bool busy_=false,workerSleeping_=false,stopping_=false,stoppedWorker_=false,stopped_=false,proxyRunning_=false;
    unsigned waiters_=0;std::deque<std::uint64_t> drainTargets_;
    Proxy* proxy_=nullptr;
    Stats stats_;
    std::thread worker_;
    // vk-worker-3 (last: the members above keep v427's offsets)
    std::array<Item,2> peek_{};unsigned peekCount_=0; // vk-worker-3 (worker only: written under m_ by aheadPeek, read by aheadCall)
    std::atomic<AheadHook> aheadHook_{nullptr};std::atomic<void*> aheadContext_{nullptr}; // vk-worker-3 (setAheadHook)
};
}
