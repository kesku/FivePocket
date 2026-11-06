#pragma once
#include "gta5_native_tile_submission_capture.h"
#include "gta5_native_scene_pass.h"
#include "gta5_tile_scene_continuation.h"

namespace recomp::gpu {
// First ordinary-worker scene only: SELECT + scene IB with its selected IRQ
// tail. The second scene/deferred tail needs its own consumer. Completion here
// requires exact executed prefix bytes, published bounds, then real tail/IRQ
// execution while the original bounds consumer is still parked. It is NOT RPTR
// publication and does not claim the whole frame or game has completed.
class Gta5NativeTileFirstScene final {
public:
    static std::optional<Gta5NativeTileFirstScene> prepare(
            const Gta5NativeTileSubmissionCapture& q,const DrawBinState& bins) {
        if(q.phase()!=Gta5NativeTileSubmissionCapture::Phase::Sealed || q.submissions().size()!=1)return std::nullopt;
        const auto& s=q.submissions().front();
        if(s.ringWords.size()!=5 || s.indirects.size()!=1 || s.ringWords[0]!=0xC0006200 ||
           s.ringWords[2]!=0xC0013F00 || s.ringWords[3]!=s.indirects[0].physical ||
           std::uint64_t{s.ringWords[4]}*4!=s.indirects[0].bytes.size() ||
           !(bins.knownWords&4) || bins.words[2]!=s.ringWords[1])return std::nullopt;
        const auto& ib=s.indirects.front();
        if(ib.bytes.size()<=Gta5TileSceneContinuation::Bytes)return std::nullopt;
        const auto prefix=ib.bytes.size()-Gta5TileSceneContinuation::Bytes;
        const auto tail=Gta5TileSceneContinuation::parse(ib.physical+prefix,
            std::span<const std::uint8_t>(ib.bytes).subspan(prefix));
        if(!tail)return std::nullopt;
        const auto selection=tail->select(bins);
        if(!selection || !selection->interruptRequired)return std::nullopt;
        const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m) {
            return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
        };
        for(const auto& [at,n]:std::array<std::pair<unsigned,unsigned>,2>{{
                {tail->request().scratchPhysical(),32},{tail->request().callbackContext()-0xC0000000,4}}})
            if(overlaps(at,n,ib.physical,ib.bytes.size()) || overlaps(at,n,q.ringVirtual()-0xA0000000,q.ringBytes()) ||
               overlaps(at,n,q.readVirtual()-0xA0000000,4))return std::nullopt;
        return Gta5NativeTileFirstScene(q,*tail,prefix);
    }
    // Bounds describe the executed drawing prefix, never the appended IRQ.
    // prepare() already validated the complete tail and retained it as a
    // separate completion obligation. Both live and diagnostic callers use
    // this same split so a full-IB parser call cannot silently diverge.
    std::optional<Gta5TileExtentPublication> prepareExtentPublication(
            const Gta5TileExtentRange& table,std::span<const std::array<std::uint32_t,4>> tiles) const noexcept {
        if(attempted_)return std::nullopt;
        const auto& ib=queue_.submissions().front().indirects.front();
        return Gta5TileExtentPublication::prepare(
            {ib.physical+0xA0000000,ib.physical,std::span<const std::uint8_t>(ib.bytes).first(prefixBytes_)},table,tiles);
    }
    template<class ExecuteTail>
    bool complete(const Gta5NativeScenePass& pass,const Gta5ResourceReader& heap,
            const Gta5ResourceReader& graphics,const ExecuteTail& execute) {
        if(attempted_)return false;attempted_=true;
        const auto& ib=queue_.submissions().front().indirects.front();
        if(!pass.resourcesPublished() || !pass.extentsPublished() || !pass.matchesExecutedBytes(
            std::span<const std::uint8_t>(ib.bytes).first(prefixBytes_)) || !queue_.matchesCurrent(heap,graphics))return false;
        // execute MUST finish the actual continuation and original source1
        // handler, and return the issuing owner before waking the consumer.
        if(!execute(queue_.device(),tail_) || !queue_.matchesCurrent(heap,graphics))return false;
        completed_=true;return true;
    }
    bool packetsCompleted()const noexcept{return completed_;}
    const auto& queue()const noexcept{return queue_;}
    const auto& interrupt()const noexcept{return tail_.request();}
private:
    Gta5NativeTileFirstScene(const Gta5NativeTileSubmissionCapture& q,const Gta5TileSceneContinuation& t,unsigned n)
        :queue_(q),tail_(t),prefixBytes_(n){}
    Gta5NativeTileSubmissionCapture queue_;Gta5TileSceneContinuation tail_;
    unsigned prefixBytes_=0;bool attempted_=false,completed_=false;
};
}
