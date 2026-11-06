#pragma once
#include "gta5_native_tile_submission_capture.h"
#include "gta5_tile_command_interrupt.h"
#include "native_tile_cache_barrier.h"

namespace recomp::gpu {
// Original EndTiling's two initial queue entries, not its scene draws. Completion
// here means the actual cache packets and source1 interrupt ran. It does NOT
// retire RPTR, complete the worker's later scene, or authorize presentation.
class Gta5NativeTileInitialQueue final {
public:
    static std::optional<Gta5NativeTileInitialQueue> prepare(const Gta5NativeTileSubmissionCapture& q) {
        if(q.phase()!=Gta5NativeTileSubmissionCapture::Phase::Sealed || q.submissions().size()!=2)return std::nullopt;
        const auto& first=q.submissions()[0];const auto& last=q.submissions()[1];
        for(const auto& s:q.submissions())if(s.ringWords.size()!=3 || s.indirects.size()!=1 ||
            s.ringWords[0]!=0xC0013F00 || s.ringWords[1]!=s.indirects[0].physical ||
            std::uint64_t{s.ringWords[2]}*4!=s.indirects[0].bytes.size())return std::nullopt;
        const auto cache=NativeTileCacheBarrier::parse(first.indirects[0].bytes);
        const auto irq=Gta5TileCommandInterrupt::parse(last.indirects[0].physical,last.indirects[0].bytes);
        if(!cache || !cache->cachePair() || !irq || first.after!=last.before)return std::nullopt;
        const auto overlap=[](unsigned a,unsigned n,unsigned b,unsigned m) {
            return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
        };
        const auto& a=first.indirects[0];const auto& b=last.indirects[0];
        if(overlap(a.physical,a.bytes.size(),b.physical,b.bytes.size()))return std::nullopt;
        for(const auto& [physical,size]:std::array<std::pair<unsigned,unsigned>,2>{{
            {irq->scratchPhysical(),32},{irq->callbackContext()-0xC0000000,4}}}) {
            if(overlap(physical,size,a.physical,a.bytes.size()) || overlap(physical,size,b.physical,b.bytes.size()) ||
               overlap(physical,size,q.ringVirtual()-0xA0000000,q.ringBytes()) ||
               overlap(physical,size,q.readVirtual()-0xA0000000,4))return std::nullopt;
        }
        return Gta5NativeTileInitialQueue(q,*cache,*irq);
    }
    template<class Engine,class Target,class Interrupt>
    bool execute(Engine& engine,Target& target,const Gta5ResourceReader& heap,
            const Gta5ResourceReader& graphics,const Interrupt& interrupt) {
        if(attempted_)return false;attempted_=true;
        if(!queue_.matchesCurrent(heap,graphics) || !engine.executeLogicalCacheBarrier(cache_,target))return false;
        cacheCompleted_=true;
        if(!queue_.matchesCurrent(heap,graphics) || !interrupt(queue_.device(),irq_))return false;
        interruptCompleted_=true;
        complete_=queue_.matchesCurrent(heap,graphics);return complete_;
    }
    bool attempted()const noexcept{return attempted_;}
    bool cacheCompleted()const noexcept{return cacheCompleted_;}
    bool interruptCompleted()const noexcept{return interruptCompleted_;}
    bool packetsCompleted()const noexcept{return complete_;}
    const Gta5TileCommandInterrupt& interrupt()const noexcept{return irq_;}
    const Gta5NativeTileSubmissionCapture& queue()const noexcept{return queue_;}
private:
    Gta5NativeTileInitialQueue(const Gta5NativeTileSubmissionCapture& q,const NativeTileCacheBarrier& c,const Gta5TileCommandInterrupt& i)
        :queue_(q),cache_(c),irq_(i){}
    Gta5NativeTileSubmissionCapture queue_;NativeTileCacheBarrier cache_;Gta5TileCommandInterrupt irq_;
    bool attempted_=false,cacheCompleted_=false,interruptCompleted_=false,complete_=false;
};
}
