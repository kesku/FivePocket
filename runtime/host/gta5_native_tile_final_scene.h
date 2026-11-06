#pragma once
#include "gta5_native_tile_submission_capture.h"
#include "gta5_native_scene_pass.h"
#include "gta5_tile_scene_continuation.h"
#include "gta5_tile_deferred_tail.h"

namespace recomp::gpu {
// Second scene + following deferred submission. The original worker emits
// both in one cycle. Execute the skipped-IRQ tail before the later SELECT;
// append that actual submission afterward. Neither recording it nor staging
// output can authorize RPTR. All resource bytes, tail effects and final
// cursor/completion writes must really finish first.
class Gta5NativeTileFinalScene final {
public:
    enum class Phase {Ready,SceneComplete,DeferredReady,Complete,Failed};
    static std::optional<Gta5NativeTileFinalScene> prepare(
            const Gta5NativeTileSubmissionCapture& q,const DrawBinState& bins) {
        if(q.phase()!=Gta5NativeTileSubmissionCapture::Phase::Sealed || q.submissions().size()!=1)return std::nullopt;
        const auto& s=q.submissions().front();
        if(!indirect(s) || !(bins.knownWords&4) || bins.words[2]!=s.ringWords[1])return std::nullopt;
        const auto& ib=s.indirects.front();
        if(ib.bytes.size()<=Gta5TileSceneContinuation::Bytes)return std::nullopt;
        const auto prefix=ib.bytes.size()-Gta5TileSceneContinuation::Bytes;
        const auto tail=Gta5TileSceneContinuation::parse(ib.physical+prefix,
            std::span<const std::uint8_t>(ib.bytes).subspan(prefix));
        if(!tail)return std::nullopt;
        const auto selected=tail->select(bins);
        if(!selected || selected->interruptRequired || selected->operationCount!=5)return std::nullopt;
        // Reject aliases BEFORE scratch writes or any original consumer runs,
        // not merely when the later deferred submission is appended.
        for(const auto& [at,n]:std::array<std::pair<unsigned,unsigned>,2>{{
                {tail->request().scratchPhysical(),32},{tail->request().callbackContext()-0xC0000000,4}}})
            if(overlaps(at,n,ib.physical,ib.bytes.size()) ||
               overlaps(at,n,q.ringVirtual()-0xA0000000,q.ringBytes()) ||
               overlaps(at,n,q.readVirtual()-0xA0000000,4))return std::nullopt;
        return Gta5NativeTileFinalScene(q,*tail,prefix);
    }
    template<class ExecuteTail>
    bool completeScene(const Gta5NativeScenePass& pass,const Gta5ResourceReader& heap,
            const Gta5ResourceReader& graphics,const ExecuteTail& execute) {
        if(phase_!=Phase::Ready)return false;phase_=Phase::Failed;
        const auto& ib=queue_.submissions().front().indirects.front();
        if(!pass.resourcesPublished() || !pass.extents().empty() ||
           !pass.matchesExecutedBytes(std::span<const std::uint8_t>(ib.bytes).first(prefixBytes_)) ||
           !queue_.matchesCurrent(heap,graphics) || !execute(tail_) || !queue_.matchesCurrent(heap,graphics))return false;
        phase_=Phase::SceneComplete;return true;
    }
    bool appendDeferred(const Gta5NativeTileSubmissionCapture& all,const Gta5ResourceReader& heap,
            const Gta5ResourceReader& graphics) {
        if(phase_!=Phase::SceneComplete)return false;phase_=Phase::Failed;
        if(all.phase()!=Gta5NativeTileSubmissionCapture::Phase::Sealed || all.submissions().size()!=2 ||
           all.device()!=queue_.device() || all.ringVirtual()!=queue_.ringVirtual() || all.ringBytes()!=queue_.ringBytes() ||
           all.readVirtual()!=queue_.readVirtual() || all.entryReadWord()!=queue_.entryReadWord())return false;
        const auto& prior=queue_.submissions().front();const auto& first=all.submissions().front();
        if(!indirect(first) || first.before!=prior.before || first.after!=prior.after || first.ringWords!=prior.ringWords ||
           first.indirects.front().bytes!=prior.indirects.front().bytes)return false;
        const auto& last=all.submissions().back();
        if(!indirect(last) || last.before!=first.after || last.ringWords[1]!=UINT32_MAX)return false;
        const auto& ib=last.indirects.front();const auto deferred=Gta5TileDeferredTail::parse(ib.physical,ib.bytes);
        if(!deferred || overlaps(first.indirects.front().physical,first.indirects.front().bytes.size(),ib.physical,ib.bytes.size()) ||
           !all.matchesCurrent(heap,graphics))return false;
        for(const auto& [at,n]:std::array<std::pair<unsigned,unsigned>,3>{{
                {tail_.request().scratchPhysical(),32},{tail_.request().callbackContext()-0xC0000000,4},
                {deferred->fencePhysical(),8}}}) {
            for(const auto& s:all.submissions())if(overlaps(at,n,s.indirects.front().physical,s.indirects.front().bytes.size()))return false;
            if(overlaps(at,n,all.ringVirtual()-0xA0000000,all.ringBytes()) ||
               overlaps(at,n,all.readVirtual()-0xA0000000,4))return false;
        }
        queue_=all;deferred_=*deferred;phase_=Phase::DeferredReady;return true;
    }
    template<class Execute>
    bool completeDeferred(const Gta5ResourceReader& heap,const Gta5ResourceReader& graphics,const Execute& execute) {
        if(phase_!=Phase::DeferredReady)return false;phase_=Phase::Failed;
        const auto state=heap(queue_.device(),21944);
        if(!state || !deferred_ || queue_.readVirtual()<60 ||
           !deferred_->matchesProducedState(*state,queue_.readVirtual()-60,deferred_->fencePhysical()) ||
           deferred_->fencePhysical()!=queue_.readVirtual()-60-0xA0000000 || !queue_.matchesCurrent(heap,graphics) ||
           !execute(*deferred_) || !queue_.matchesCurrent(heap,graphics))return false;
        const auto completed=graphics(deferred_->fencePhysical(),8);
        if(!completed || completed->size()!=8 || word(*completed,0)!=deferred_->completionValue() ||
           word(*completed,4)!=deferred_->commandCursor())return false;
        phase_=Phase::Complete;return true;
    }
    bool packetsCompleted()const noexcept{return phase_==Phase::Complete;}
    Phase phase()const noexcept{return phase_;}
    const auto& queue()const noexcept{return queue_;}
    const auto& interrupt()const noexcept{return tail_.request();}
    const auto& deferred()const noexcept{return deferred_;}
private:
    using Submission=Gta5NativeTileSubmissionCapture::Submission;
    static bool indirect(const Submission& s) {
        return s.ringWords.size()==5 && s.indirects.size()==1 && s.ringWords[0]==0xC0006200 &&
            s.ringWords[2]==0xC0013F00 && s.ringWords[3]==s.indirects.front().physical &&
            std::uint64_t{s.ringWords[4]}*4==s.indirects.front().bytes.size();
    }
    static bool overlaps(unsigned a,unsigned n,unsigned b,unsigned m) {
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
    }
    static unsigned word(std::span<const std::uint8_t> b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    Gta5NativeTileFinalScene(const Gta5NativeTileSubmissionCapture& q,const Gta5TileSceneContinuation& t,unsigned n)
        :queue_(q),tail_(t),prefixBytes_(n){}
    Gta5NativeTileSubmissionCapture queue_;Gta5TileSceneContinuation tail_;
    std::optional<Gta5TileDeferredTail> deferred_;
    unsigned prefixBytes_=0;Phase phase_=Phase::Ready;
};
}
