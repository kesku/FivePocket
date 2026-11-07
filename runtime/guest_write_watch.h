#pragma once
// Attempt v258 (goal step 4): exact write watching of guest physical memory, so the renderer can keep what it derived from
// guest bytes (converted vertex streams, verified textures) across frames instead of re-reading them every frame.
//
// A watched page is read-only in every host view of the physical memory (the 0xA0000000 and 0xC0000000 aliases map the
// same shared memory). The first write to it, from any thread, faults; the handler bumps the page's generation and makes
// it writable in every view, and the write retries. A range's stamp is the sum of its pages' generations: generations only
// grow, so an equal stamp later means no byte of the range was written in between. The caller reads the bytes after
// watch() returned (after the protection): a write before the protection is in what it reads, a write after it faults. The handler's unprotect and watch()'s protect of one page are serialized by a per-page stripe
// lock (otherwise a handler's "writable" could land after a new "read-only" and hide later writes). Nothing that holds a
// stripe lock writes guest memory, so the handler never waits for itself.
//
// Kernel writers do not fault (a syscall writing into a read-only page fails with EFAULT): code that passes guest memory to
// a syscall calls written() for the range first. Pages written in recent frames are "dynamic": watch() returns Dynamic for
// ranges that touch one (no protection change), so a buffer the game rewrites every frame costs no faults; such a page is
// watched again after QuietFrames frames without a fault.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace recomp::gpu::watch {
// (The page size is the system's, read at install: 4 KB on the device, 16 KB on Apple silicon hosts.)
constexpr std::uint64_t Dynamic=~0ull; // watch(): a page of the range was written recently (not watched)
constexpr unsigned MaxViews=4,Stripes=4096,QuietFrames=8;

class WriteWatch {
public:
    struct Counters {std::atomic<std::uint64_t> faults{0},protects{0},protectCalls{0},dynamicRefusals{0},kernelWrites{0},chained{0};};
    // views: host addresses where the physical bytes [0, physicalBytes) are mapped (page aligned, read/write).
    bool install(std::uint8_t* const* views,unsigned viewCount,std::uint64_t physicalBytes) {
        const long pageSize=sysconf(_SC_PAGESIZE);
        if(pageSize<4096 || (pageSize&(pageSize-1)))return false;
        shift_=unsigned(__builtin_ctzl(static_cast<unsigned long>(pageSize)));pageBytes_=std::uint32_t(pageSize);
        if(installed_ || !viewCount || viewCount>MaxViews || !physicalBytes || physicalBytes%pageBytes_ || physicalBytes>(1ull<<32))return false;
        for(unsigned v=0;v<viewCount;++v){if(!views[v] || reinterpret_cast<std::uintptr_t>(views[v])%pageBytes_)return false;views_[v]=views[v];}
        viewCount_=viewCount;bytes_=physicalBytes;pages_=std::uint32_t(physicalBytes>>shift_);
        gen_.reset(new std::atomic<std::uint32_t>[pages_]());protected_.reset(new std::atomic<std::uint8_t>[pages_]());
        lastWrite_.reset(new std::atomic<std::uint32_t>[pages_]());
        for(std::uint32_t p=0;p<pages_;++p){gen_[p].store(1,std::memory_order_relaxed);protected_[p].store(0,std::memory_order_relaxed);
            lastWrite_[p].store(0,std::memory_order_relaxed);}
        vkw4InitRegions(physicalBytes); // vk-worker-4 (a call only: install()'s control flow unchanged)
        active().store(this,std::memory_order_release);
        struct sigaction a{};a.sa_sigaction=&onSignal;a.sa_flags=SA_SIGINFO;sigemptyset(&a.sa_mask);
        if(sigaction(SIGSEGV,&a,&previousSegv_)!=0 || sigaction(SIGBUS,&a,&previousBus_)!=0){active().store(nullptr);return false;}
        installed_=true;return true;
    }
    bool installed() const noexcept {return installed_;}
    // Frame boundary (the renderer's present): ages the dynamic marks.
    void nextFrame() noexcept {frame_.fetch_add(1,std::memory_order_relaxed);}
    // Protects the pages of [physical, physical+bytes) and returns their stamp, or Dynamic (nothing changed).
    std::uint64_t watch(std::uint32_t physical,std::uint32_t bytes) {
        if(!installed_ || !bytes || std::uint64_t{physical}+bytes>bytes_)return Dynamic;
        const std::uint32_t first=physical>>shift_,last=std::uint32_t((std::uint64_t{physical}+bytes-1)>>shift_);
        const auto now=frame_.load(std::memory_order_relaxed);
        for(std::uint32_t p=first;p<=last;++p){const auto w=lastWrite_[p].load(std::memory_order_relaxed);
            if(w && now-w<QuietFrames){counters.dynamicRefusals.fetch_add(1,std::memory_order_relaxed);return Dynamic;}}
        std::uint64_t sum=0;
        for(std::uint32_t p=first;p<=last;) {
            // one mprotect per view for each run of unprotected pages (each under its pages' stripe locks, taken in order)
            if(std::uint32_t g;readProtected(p,g)){sum+=g;++p;continue;}
            std::uint32_t end=p+1,g;
            while(end<=last && !readProtected(end,g) && end-p<64)++end;
            for(std::uint32_t q=p;q<end;++q)lock(q);
            bool ok=true;
            for(unsigned v=0;v<viewCount_;++v)
                ok=ok && mprotect(views_[v]+(std::size_t{p}<<shift_),std::size_t{end-p}<<shift_,PROT_READ)==0;
            counters.protectCalls.fetch_add(viewCount_,std::memory_order_relaxed);
            for(std::uint32_t q=p;q<end;++q) {
                if(ok){protected_[q].store(1,std::memory_order_release);counters.protects.fetch_add(1,std::memory_order_relaxed);}
                sum+=gen_[q].load(std::memory_order_acquire);
            }
            for(std::uint32_t q=end;q-->p;)unlock(q);
            if(!ok){unprotectRange(p,end);return Dynamic;}
            p=end;
        }
        return sum;
    }
    // The stamp now (no protection change); Dynamic when a page of the range is not protected.
    std::uint64_t stamp(std::uint32_t physical,std::uint32_t bytes) const noexcept {
        if(!installed_ || !bytes || std::uint64_t{physical}+bytes>bytes_)return Dynamic;
        const std::uint32_t first=physical>>shift_,last=std::uint32_t((std::uint64_t{physical}+bytes-1)>>shift_);
        std::uint64_t sum=0;
        for(std::uint32_t p=first;p<=last;++p){std::uint32_t g;if(!readProtected(p,g))return Dynamic;sum+=g;}
        return sum;
    }
    // A writer that does not fault (a syscall) is about to write [physical, physical+bytes): its pages count as written now.
    void written(std::uint32_t physical,std::uint32_t bytes) noexcept {
        if(!installed_ || !bytes || std::uint64_t{physical}+bytes>bytes_)return;
        const std::uint32_t first=physical>>shift_,last=std::uint32_t((std::uint64_t{physical}+bytes-1)>>shift_);
        for(std::uint32_t p=first;p<=last;++p)(void)touch(p,false);
        counters.kernelWrites.fetch_add(1,std::memory_order_relaxed);
    }
    // Host address -> physical offset, for callers holding a host pointer into one of the views.
    bool physicalOf(const void* host,std::uint32_t& physical) const noexcept {
        const auto a=reinterpret_cast<std::uintptr_t>(host);
        for(unsigned v=0;v<viewCount_;++v){const auto b=reinterpret_cast<std::uintptr_t>(views_[v]);
            if(a>=b && a-b<bytes_){physical=std::uint32_t(a-b);return true;}}
        return false;
    }
    Counters counters;
    // ---- vk-worker-4 (diag-vkw4-watch; see vk_worker4_fast.h): per 64 KB region of physical memory (or per page if larger), the
    // number of touch() calls that changed one of its pages (bumped after the page's flag, generation and last-write frame) and
    // the watch frame at the latest one. Maintained whether the switch is on or not; read by the renderer's watch proofs only.
    // The sum of the generations of the regions covering [physical, physical + bytes) (acquire: what a counted touch changed is
    // visible to the reads that follow).
    std::uint64_t regionSum(std::uint32_t physical,std::uint32_t bytes) const noexcept {
        if(!regions_ || !bytes || std::uint64_t{physical}+bytes>bytes_)return ~0ull;
        const std::uint32_t first=physical>>regionShift_,last=std::uint32_t((std::uint64_t{physical}+bytes-1)>>regionShift_);
        std::uint64_t sum=0;
        for(std::uint32_t r=first;r<=last;++r)sum+=regionGen_[r].load(std::memory_order_acquire);
        return sum;
    }
    // No region of the range was touched in frame `now` (the watch's current frame; 0 = never touched: frames start at 1). A page
    // touched in an even frame f stores lastWrite = f+1, which watch()'s test `now-w < QuietFrames` (unsigned) passes in frame f and
    // fails in the 8 frames after: the only way a page can turn dynamic later without another touch.
    bool regionsQuiet(std::uint32_t physical,std::uint32_t bytes,std::uint32_t now) const noexcept {
        if(!regions_ || !bytes || std::uint64_t{physical}+bytes>bytes_)return false;
        const std::uint32_t first=physical>>regionShift_,last=std::uint32_t((std::uint64_t{physical}+bytes-1)>>regionShift_);
        for(std::uint32_t r=first;r<=last;++r)if(regionFrame_[r].load(std::memory_order_acquire)>=now)return false;
        return true;
    }
    std::uint32_t frameNow() const noexcept {return frame_.load(std::memory_order_relaxed);}
    static std::atomic<WriteWatch*>& active() noexcept {static std::atomic<WriteWatch*> w{nullptr};return w;}
private:
    // The page's generation when it is protected and no write is under way (else false): generation, flag, generation.
    bool readProtected(std::uint32_t p,std::uint32_t& generation) const noexcept {
        const auto g1=gen_[p].load(std::memory_order_acquire);
        if(!protected_[p].load(std::memory_order_acquire))return false;
        const auto g2=gen_[p].load(std::memory_order_acquire);
#if defined(RECOMP_V258_MUTANT_UNCHECKED_READ) // test mutant: v258's first reader (flag, then one generation read)
        (void)g1;generation=g2;return true;
#endif
        if(g1!=g2)return false;
        generation=g1;return true;
    }
#if defined(RECOMP_V258_MUTANT_NO_LOCK) // test mutant: the handler's unprotect and watch()'s protect unserialized
    void lock(std::uint32_t) noexcept {}
    void unlock(std::uint32_t) noexcept {}
#else
    void lock(std::uint32_t p) noexcept {auto& l=locks_[p%Stripes];while(l.exchange(1,std::memory_order_acquire))while(l.load(std::memory_order_relaxed)){}}
    void unlock(std::uint32_t p) noexcept {locks_[p%Stripes].store(0,std::memory_order_release);}
#endif
    void unprotectRange(std::uint32_t p,std::uint32_t end) noexcept {for(std::uint32_t q=p;q<end;++q)(void)touch(q,true);} // (a view may be read-only)
    // The page counts as written: generation +1, writable in every view, marked dynamic for QuietFrames frames.
    bool touch(std::uint32_t p,bool fault) noexcept {
        lock(p);
        // The flag is cleared before the generation grows: a reader that sees the flag set on both sides of an unchanged
        // generation (readProtected) read a stamp from before this write (see the class comment).
        const bool wasProtected=protected_[p].exchange(0,std::memory_order_acq_rel);
#if defined(RECOMP_V258_MUTANT_NO_FAULT_GENERATION) // test mutant: a faulting write leaves the generation
        if(!fault)
#endif
        gen_[p].fetch_add(1,std::memory_order_acq_rel);
        lastWrite_[p].store(frame_.load(std::memory_order_relaxed)|1u,std::memory_order_relaxed);
        vkw4Touched(p); // vk-worker-4: after every change of this page's state (a call only: touch()'s control flow unchanged)
        bool ok=true;
        if(wasProtected || fault)
            for(unsigned v=0;v<viewCount_;++v)ok=mprotect(views_[v]+(std::size_t{p}<<shift_),pageBytes_,PROT_READ|PROT_WRITE)==0 && ok;
        unlock(p);
        return ok;
    }
    // vk-worker-4: the region arrays (install(), before the handler is active)
    [[gnu::noinline]] void vkw4InitRegions(std::uint64_t physicalBytes) noexcept {
        regionShift_=shift_>16u?shift_:16u;
        regions_=std::uint32_t(((physicalBytes-1)>>regionShift_)+1);
        regionGen_.reset(new std::atomic<std::uint64_t>[regions_]());regionFrame_.reset(new std::atomic<std::uint32_t>[regions_]());
    }
    // vk-worker-4: page p's state just changed (touch(), under its stripe lock): its region's frame (max), then its generation (release)
#if defined(RECOMP_VKW4_MUTANT_NO_REGION_BUMP) // test mutant: touch() leaves the region generation (a proof survives the write)
    [[gnu::noinline]] void vkw4Touched(std::uint32_t) noexcept {}
#else
    [[gnu::noinline]] void vkw4Touched(std::uint32_t p) noexcept {
        if(!regions_)return;
        const std::uint32_t r=std::uint32_t((std::uint64_t{p}<<shift_)>>regionShift_);
        // (the latest frame of any touch of the region: a maximum, so a handler that read an older frame and stores late cannot
        // hide a newer touch; this load of frame_ is not older than the one touch() stored as the page's last write)
        const std::uint32_t f=frame_.load(std::memory_order_relaxed);
        std::uint32_t t=regionFrame_[r].load(std::memory_order_relaxed);
        while(t<f && !regionFrame_[r].compare_exchange_weak(t,f,std::memory_order_relaxed,std::memory_order_relaxed)){}
        regionGen_[r].fetch_add(1,std::memory_order_release);
    }
#endif
    bool handle(void* address) noexcept {
        std::uint32_t physical=0;
        if(!physicalOf(address,physical))return false;
        counters.faults.fetch_add(1,std::memory_order_relaxed);
        return touch(physical>>shift_,true); // (false: still read-only; the previous handler decides, no fault loop)
    }
    static void onSignal(int sig,siginfo_t* info,void* context) {
        auto* w=active().load(std::memory_order_acquire);
        if(w && info && w->handle(info->si_addr))return; // the faulting write retries on a now writable page
        const struct sigaction* previous=w?(sig==SIGBUS?&w->previousBus_:&w->previousSegv_):nullptr;
        if(w)w->counters.chained.fetch_add(1,std::memory_order_relaxed);
        if(previous && (previous->sa_flags&SA_SIGINFO) && previous->sa_sigaction){previous->sa_sigaction(sig,info,context);return;}
        if(previous && previous->sa_handler!=SIG_DFL && previous->sa_handler!=SIG_IGN){previous->sa_handler(sig);return;}
        // (Claude 2026-10-22, iPad c492) a fault this watch does not own ends in raise(), as before; the crash report then shows
        // EXC_CRASH/SIGSEGV with no faulting stack (the raise erases it). Before raising, one line names the fault: signal,
        // code, address, pc, lr and the frame-pointer chain of the faulting context (async-signal-safe: write(2) of a stack
        // buffer). The default disposition is set first, so a bad frame in the walk ends the process as the raise would.
        signal(sig,SIG_DFL);
        {
            char line[640];std::size_t n=0;
            auto put=[&](const char* t){while(*t && n<sizeof line-1)line[n++]=*t++;};
            auto hex=[&](std::uint64_t v){char b[17];int i=16;b[i]=0;do{b[--i]="0123456789abcdef"[v&15];v>>=4;}while(v && i);put(b+i);};
            std::uint64_t pc=0,lr=0,fp=0;
#if defined(__APPLE__) && defined(__aarch64__)
            if(context){const auto* uc=static_cast<const ucontext_t*>(context);if(uc->uc_mcontext){pc=uc->uc_mcontext->__ss.__pc;lr=uc->uc_mcontext->__ss.__lr;fp=uc->uc_mcontext->__ss.__fp;}}
#endif
            put("GUEST_WRITE_WATCH_UNOWNED_FAULT sig=");hex(std::uint64_t(sig));put(" code=");hex(info?std::uint64_t(info->si_code):0);
            put(" addr=");hex(info?reinterpret_cast<std::uint64_t>(info->si_addr):0);put(" pc=");hex(pc);put(" lr=");hex(lr);put(" fp_chain=");
            for(int k=0;k<24 && fp && !(fp&15);++k) { // (fp, return address) pairs; stops at a null, misaligned or non-increasing frame
                const std::uint64_t next=reinterpret_cast<const std::uint64_t*>(fp)[0],ret=reinterpret_cast<const std::uint64_t*>(fp)[1];
                hex(ret);put(",");if(next<=fp)break;fp=next;
            }
            put("\n");(void)::write(2,line,n);
        }
        raise(sig);
    }
    bool installed_=false;unsigned shift_=12;std::uint32_t pageBytes_=4096;
    std::uint8_t* views_[MaxViews]{};unsigned viewCount_=0;std::uint64_t bytes_=0;std::uint32_t pages_=0;
    std::unique_ptr<std::atomic<std::uint32_t>[]> gen_,lastWrite_;std::unique_ptr<std::atomic<std::uint8_t>[]> protected_;
    std::atomic<std::uint8_t> locks_[Stripes]{};
    std::atomic<std::uint32_t> frame_{1};
    struct sigaction previousSegv_{},previousBus_{};
    // vk-worker-4 (last: the members above keep their offsets)
    std::unique_ptr<std::atomic<std::uint64_t>[]> regionGen_;std::unique_ptr<std::atomic<std::uint32_t>[]> regionFrame_;
    unsigned regionShift_=16;std::uint32_t regions_=0;
};
}
