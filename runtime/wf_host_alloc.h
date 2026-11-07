#pragma once
// worker-fixed (2026-10-04): a host-memory heap for the renderer's frame-slot command pools, handed to vkCreateCommandPool as its
// VkAllocationCallbacks (diag-wf-host-alloc). Why: the Vulkan worker's profiles (prof-v442-1 house/outdoor, milestone-v429-1 drive)
// spend 4.3-5.3% of the thread in libc calloc called by the Adreno driver while recording (vkCmdBeginRenderPass 2.2-2.9%,
// vkEndCommandBuffer 0.7-0.8, vkCmdEndRenderPass 0.7-0.8, vkCmdPipelineBarrier 0.45-0.54, vkCmdDraw 0.2-0.25: 0.70 ms per house
// frame, 0.97 per driving frame), mostly scudo's bookkeeping (allocate(), the per-thread cache refills and its atomics), ~25% the
// zeroing; the frees run at vkResetCommandPool on the pool-reset helper. Vulkan sources a pooled object's host memory from its
// pool's allocator (vkCmd* recording memory: the command pool's), so a driver that honours pAllocator moves those allocations here.
// Nothing GPU-visible changes: the same commands with the same arguments are recorded; only where the driver's host-side bookkeeping
// lives. Whether this driver uses the callbacks at all is measured on the device (RENDER_DIAG_WF allocs per frame; 0 = it ignores
// them and the switch is inert).
//
// The heap: size classes (16-byte steps to 256, then 4 per power of two to 64 KB) carved from 256 KB slabs, a free list per class,
// a 16-byte header before every payload (class, offset back to the block start, requested size). Larger requests (and any request
// once the slabs reach SlabCap) come from malloc with the header in front. Every allocation is zero-filled (the driver called calloc
// before; a conformant driver cannot assume that, but zeroing keeps any such assumption harmless). One spin lock per heap: the pool's
// callbacks are normally called by one thread at a time (pools are externally synchronized), but frees may come from the pool-reset
// helper or a driver thread. Slabs are kept until the heap is destroyed (the renderer's pools live for the process).
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace recomp::gpu::wf {

struct HeapCounters { // (relaxed atomics: read by the window log on the worker while a helper thread may free)
    std::atomic<std::uint64_t> allocs{0},reallocs{0},frees{0},large{0},bytes{0},slabBytes{0},failures{0},aligned{0};
};

class PoolHeap {
public:
    static constexpr std::size_t HeaderBytes=16;
    static constexpr std::size_t SlabBytes=256u<<10;
    static constexpr std::size_t SlabCap=std::size_t{64}<<20;   // per heap; beyond it new blocks come from malloc
    static constexpr std::uint32_t Magic=0x57464831u;           // "WFH1"
    static constexpr std::uint16_t LargeClass=0xFFFF;
#if defined(RECOMP_WF_TEST_SMALL_SLABS) // host tests: tiny slabs and cap (slab turnover, the malloc fallback)
    static constexpr std::size_t TestSlab=4096,TestCap=64u<<10;
#endif
    PoolHeap() {
        unsigned n=0;
        for(std::size_t c=16;c<=256;c+=16)caps_[n++]=c;
        for(std::size_t p=256;p<65536;p*=2)for(unsigned q=1;q<=4;++q)caps_[n++]=p+p*q/4;
        classes_=n; // 16 + 8*4 = 48 (largest 65536)
        free_.assign(classes_,nullptr);
    }
    ~PoolHeap(){for(void* s:slabs_)std::free(s);}
    PoolHeap(const PoolHeap&)=delete;PoolHeap& operator=(const PoolHeap&)=delete;

    VkAllocationCallbacks callbacks() {
        VkAllocationCallbacks c{};c.pUserData=this;c.pfnAllocation=&vkAllocation;c.pfnReallocation=&vkReallocation;c.pfnFree=&vkFree;
        return c;
    }
    void* allocate(std::size_t size,std::size_t alignment) {
        Lock l(lock_);
        void* p=allocateLocked(size?size:1,alignment<16?16:alignment);
        if(p){counters.allocs.fetch_add(1,std::memory_order_relaxed);counters.bytes.fetch_add(size,std::memory_order_relaxed);}
        else counters.failures.fetch_add(1,std::memory_order_relaxed);
        return p;
    }
    void* reallocate(void* original,std::size_t size,std::size_t alignment) {
        if(!original)return allocate(size,alignment);
        if(!size){release(original);return nullptr;}
        counters.reallocs.fetch_add(1,std::memory_order_relaxed);
        const std::size_t align=alignment<16?16:alignment;
        Lock l(lock_);
        Header* h=header(original);
        const std::size_t capacity=payloadCapacity(original,h),old=h->size;
#if !defined(RECOMP_WF_MUTANT_REALLOC_GROW_IN_PLACE) // test mutant: a larger request kept in the same block
        if(size<=capacity && !(reinterpret_cast<std::uintptr_t>(original)&(align-1)))
#else
        if(!(reinterpret_cast<std::uintptr_t>(original)&(align-1)))
#endif
        {
            if(size>old)std::memset(static_cast<char*>(original)+old,0,size-old);
            h->size=std::uint32_t(size<0xFFFFFFFFu?size:0xFFFFFFFFu);
            return original;
        }
        void* p=allocateLocked(size,align);
        if(!p){counters.failures.fetch_add(1,std::memory_order_relaxed);return nullptr;} // (the original stays valid, as the spec asks)
#if defined(RECOMP_WF_MUTANT_REALLOC_SHORT_COPY) // test mutant: one byte too few preserved
        std::memcpy(p,original,(old<size?old:size)-1);
#else
        std::memcpy(p,original,old<size?old:size);
#endif
        releaseLocked(original);
        return p;
    }
    void release(void* p) {
        if(!p)return;
        Lock l(lock_);
        releaseLocked(p);
    }
    HeapCounters counters;
    std::size_t slabBytes() const {return slabBytes_;}

private:
    struct Header {std::uint32_t magic;std::uint16_t cls;std::uint16_t pad;std::uint32_t back;std::uint32_t size;};
    static_assert(sizeof(Header)==HeaderBytes);
    struct Lock { // a test-and-test-and-set spin lock (uncontended: one exchange)
        std::atomic<bool>& f;
        explicit Lock(std::atomic<bool>& x):f(x){
            for(unsigned k=0;f.exchange(true,std::memory_order_acquire);++k)
                while(f.load(std::memory_order_relaxed)){if(k>64)std::this_thread::yield();++k;}
        }
        ~Lock(){f.store(false,std::memory_order_release);}
    };
    static Header* header(void* payload){return reinterpret_cast<Header*>(static_cast<char*>(payload)-HeaderBytes);}
    std::size_t payloadCapacity(void* payload,Header* h) const {
        if(h->cls==LargeClass)return h->size; // (large blocks are exactly what was asked)
        const char* block=reinterpret_cast<char*>(h)-h->back;
        return std::size_t(block+HeaderBytes+caps_[h->cls]-static_cast<char*>(payload));
    }
    unsigned classFor(std::size_t need) const { // the smallest class holding `need` payload bytes, or classes_ if none
        if(need<=256)return unsigned((need+15)/16)-1;
        unsigned lo=16,hi=classes_;
        while(lo<hi){const unsigned m=(lo+hi)/2;if(caps_[m]>=need)hi=m;else lo=m+1;}
        return lo;
    }
    void* allocateLocked(std::size_t size,std::size_t align) {
        // a block of class c holds HeaderBytes + caps_[c]; the payload starts HeaderBytes after the block start (16-aligned), or later
        // for larger alignments (need = size + align - 16 then leaves room to move the payload up to an aligned address)
        const std::size_t need=align<=16?size:size+align-16;
        const unsigned c=need<=caps_[classes_-1]?classFor(need):classes_;
        char* block=nullptr;
        if(c<classes_) {
            if(void* f=free_[c]){free_[c]=*static_cast<void**>(f);block=static_cast<char*>(f);}
            else block=carve(HeaderBytes+caps_[c]);
        }
        if(!block) { // large, or the slabs are at their cap
            const std::size_t total=HeaderBytes+size+align;
            char* raw=static_cast<char*>(std::malloc(total));
            if(!raw)return nullptr;
            char* payload=reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(raw)+HeaderBytes+align-1)&~std::uintptr_t(align-1));
            Header* h=header(payload);h->magic=Magic;h->cls=LargeClass;h->pad=0;
            h->back=std::uint32_t(reinterpret_cast<char*>(h)-raw);h->size=std::uint32_t(size);
            counters.large.fetch_add(1,std::memory_order_relaxed);
            std::memset(payload,0,size);
            return payload;
        }
        char* payload=block+HeaderBytes;
#if !defined(RECOMP_WF_MUTANT_NO_ALIGN) // test mutant: alignments above 16 ignored
        if(align>16){payload=reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(payload)+align-1)&~std::uintptr_t(align-1));counters.aligned.fetch_add(1,std::memory_order_relaxed);}
#endif
        Header* h=header(payload);h->magic=Magic;h->cls=std::uint16_t(c);h->pad=0;
        h->back=std::uint32_t(reinterpret_cast<char*>(h)-block);h->size=std::uint32_t(size<0xFFFFFFFFu?size:0xFFFFFFFFu);
#if !defined(RECOMP_WF_MUTANT_NO_ZERO) // test mutant: a recycled block keeps its old bytes
        std::memset(payload,0,size);
#endif
        return payload;
    }
    void releaseLocked(void* payload) {
        counters.frees.fetch_add(1,std::memory_order_relaxed);
        Header* h=header(payload);
        if(h->magic!=Magic)std::abort(); // (not ours: a driver freeing foreign memory through these callbacks would corrupt the heap)
        h->magic=0;
        char* block=reinterpret_cast<char*>(h)-h->back;
        if(h->cls==LargeClass){std::free(block);return;}
        const unsigned c=h->cls;
#if defined(RECOMP_WF_MUTANT_WRONG_CLASS) // test mutant: a freed block filed one class up (a later allocation overruns it)
        const unsigned to=c+1<classes_?c+1:c;
#else
        const unsigned to=c;
#endif
        *reinterpret_cast<void**>(block)=free_[to];free_[to]=block;
    }
    char* carve(std::size_t bytes) {
#if defined(RECOMP_WF_TEST_SMALL_SLABS)
        const std::size_t slab=bytes>TestSlab?bytes:TestSlab,cap=TestCap;
#else
        const std::size_t slab=SlabBytes,cap=SlabCap;
#endif
        if(!bump_ || std::size_t(end_-bump_)<bytes) {
            if(slabBytes_+slab>cap)return nullptr;
            void* s=nullptr;
            if(posix_memalign(&s,64,slab)!=0 || !s)return nullptr;
            slabs_.push_back(s);slabBytes_+=slab;counters.slabBytes.store(slabBytes_,std::memory_order_relaxed);
            bump_=static_cast<char*>(s);end_=bump_+slab;
        }
        char* b=bump_;bump_+=bytes;return b;
    }
    static void* VKAPI_PTR vkAllocation(void* u,std::size_t size,std::size_t alignment,VkSystemAllocationScope){
        return static_cast<PoolHeap*>(u)->allocate(size,alignment);}
    static void* VKAPI_PTR vkReallocation(void* u,void* original,std::size_t size,std::size_t alignment,VkSystemAllocationScope){
        return static_cast<PoolHeap*>(u)->reallocate(original,size,alignment);}
    static void VKAPI_PTR vkFree(void* u,void* p){static_cast<PoolHeap*>(u)->release(p);}

    std::size_t caps_[64]{};unsigned classes_=0;
    std::vector<void*> free_;std::vector<void*> slabs_;
    char* bump_=nullptr;char* end_=nullptr;std::size_t slabBytes_=0;
    std::atomic<bool> lock_{false};
};

} // namespace recomp::gpu::wf
