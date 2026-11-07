#pragma once
#include "gta5_native_scene_commands.h"
#include "gta5_tile_extent_publication.h"

namespace recomp::gpu {
// One ordered scene IB, after the ORIGINAL producer's SELECT. Draw/copy
// adapters must execute their current bound inputs; returning true for a
// planned operation is not permitted. This receipt does not retire a ring,
// execute an interrupt tail or publish resource completion.
class Gta5NativeScenePass final {
public:
    struct Counts {
        unsigned commands=0,indexed=0,copies=0,zeroRaster=0,registerWrites=0;
        unsigned skipped=0,extentResets=0,extentWrites=0;
    };
    struct Extent {
        unsigned drawOffsetWords=0,eventOffsetWords=0,destinationPhysical=0;
    };
    Gta5NativeScenePass(const Gta5NativeScenePass&)=delete;
    Gta5NativeScenePass& operator=(const Gta5NativeScenePass&)=delete;
    Gta5NativeScenePass(Gta5NativeScenePass&&)=default;
    Gta5NativeScenePass& operator=(Gta5NativeScenePass&&)=default;
    const Counts& counts()const noexcept{return counts_;}
    unsigned failedOffset()const noexcept{return failedOffset_;}
    bool prefixExecuted()const noexcept{return prefixExecuted_;}
    bool extentsPublished()const noexcept{return extentsPublished_;}
    bool resourcesPublished()const noexcept{return resourcesPublished_;}
    const auto& extents()const noexcept{return extents_;}
    bool matchesExecutedBytes(std::span<const std::uint8_t> bytes)const noexcept {
        return prefixExecuted_ && !sourceBytes_.empty() && bytes.size()==sourceBytes_.size() &&
            std::equal(bytes.begin(),bytes.end(),sourceBytes_.begin());
    }

    // Current() checks the same submission/input ownership before and after
    // execution. The owning caller parks guest writers across this operation.
    // A failure can leave native/private staging modified, but grants no
    // completion receipt or permission to resume the original consumer.
    template<class Engine,class Target,class Current,class Draw,class Copy,class Write>
    static Gta5NativeScenePass execute(const Gta5NativeSceneCommands& plan,Engine& engine,Target& target,
            const Current& current,const Draw& draw,const Copy& copy,const Write& write) noexcept {
        Gta5NativeScenePass result;
        try {
            if(plan.commands.empty() || !current() || !engine.logicalVisibilityIdle())return result;
            unsigned end=0,nextDraw=0,nextCopy=0;
            std::optional<unsigned> raster;
            bool rasterExecuted=false,reset=false,extentWritten=false;
            for(const auto& c:plan.commands) {
                result.failedOffset_=c.offsetBytes;
                if(c.offsetBytes!=end || c.endBytes<=end || (c.endBytes%4))return result;
                end=c.endBytes;
                using Kind=Gta5NativeSceneCommands::Kind;
                // State packets have their own exact parsers and side effects.
                // nullopt is a required effect below, never successful opacity.
                if(const auto state=executeGta5NativeSceneStateCommand(c,engine,target)) {
                    if(!*state)return result;
                    ++result.counts_.commands;continue;
                }
                const auto& s=engine.stateBatchSnapshot();
                const DrawBinState bins{{unsigned(s.binMask),unsigned(s.binMask>>32),
                    unsigned(s.binSelect),unsigned(s.binSelect>>32)},s.knownBinWords};
                const auto selected=evaluateDrawBinPredicate(bins,c.predicated);
                if(selected!=DrawBinDecision::Pass && selected!=DrawBinDecision::Skip)return result;
                const bool run=selected==DrawBinDecision::Pass;
                if(!run)++result.counts_.skipped;
                switch(c.kind) {
                case Kind::Indexed:
                    if(c.index!=nextDraw++ || (c.value&~0x7Fu))return result;
                    raster=c.offsetBytes/4;rasterExecuted=false;extentWritten=false;
                    if(run) {
                        if(!draw(c.index,c.value))return result;
                        rasterExecuted=true;++result.counts_.indexed;
                    }
                    break;
                case Kind::Resolve:
                    if(c.index!=nextCopy++)return result;
                    if(run) {if(!copy(c.index))return result;++result.counts_.copies;}
                    break;
                case Kind::ZeroRaster:
                    raster=c.offsetBytes/4;rasterExecuted=false;extentWritten=false;
                    if(run) {
                        // Exact zero-count DMA packet admitted by the planner.
                        // Its surrounding EXT/cache/query packets still execute.
                        if(!engine.executeLogicalZeroRaster())return result;
                        rasterExecuted=true;++result.counts_.zeroRaster;
                    }
                    break;
                case Kind::RegisterWriteback:
                    if(run) {
                        const auto& operation=std::get<NativeTileRegisterWriteback>(c.operation);
                        if(!operation.execute(bins,[&](unsigned reg){return engine.retainedRegister(reg);},write))return result;
                        ++result.counts_.registerWrites;
                    }
                    break;
                case Kind::DeferredExtentReset:
                    if(run) {reset=true;raster.reset();rasterExecuted=false;extentWritten=false;++result.counts_.extentResets;}
                    break;
                case Kind::DeferredExtentWrite:
                    if(run) {
                        if(!reset || !raster || !rasterExecuted || extentWritten || (c.value&3)!=1)return result;
                        result.extents_.push_back({*raster,c.offsetBytes/4,c.value&~3u});
                        extentWritten=true;reset=false;++result.counts_.extentWrites;
                    }
                    break;
                default:return result;
                }
                ++result.counts_.commands;
            }
            if(!current() || !engine.logicalVisibilityIdle())return result;
            if(!plan.sourceBytes.empty() && plan.sourceBytes.size()!=end)return result;
            result.sourceBytes_=plan.sourceBytes;
            result.failedOffset_=UINT32_MAX;result.prefixExecuted_=true;
        } catch(...) {} // Caller must stop this submission without publishing success.
        return result;
    }

    // The conservative-XY policy is separately owned/validated. Every selected
    // first-pass EXT must be tied to an actually executed DMA (including zero
    // count) and the exact original destination before its real fence/publisher.
    // This supplies the producer proof previously left entirely to the caller.
    template<class Publish>
    bool publishExtents(Gta5TileExtentPublication& plan,const Publish& publish) {
        if(!prefixExecuted_ || extentAttempted_ || extents_.empty() ||
           plan.phase()!=Gta5TileExtentPublication::Phase::Prepared || plan.bindings().size()!=extents_.size())return false;
        extentAttempted_=true;
        for(unsigned i=0;i<extents_.size();++i) {
            const auto& a=extents_[i];const auto& b=plan.bindings()[i];
            if(a.drawOffsetWords!=b.drawOffsetWords || a.eventOffsetWords!=b.eventOffsetWords ||
               a.destinationPhysical!=b.destinationPhysical)return false;
        }
        try {
            if(!publish(plan) || plan.phase()!=Gta5TileExtentPublication::Phase::Complete)return false;
            extentsPublished_=true;return true;
        } catch(...) {return false;}
    }
    // Resolve/register writes are guest-visible effects of this submission,
    // not just private staging. Publish all current destinations atomically
    // against their expected original bytes before any ring acknowledgement.
    // The later original deferred fence/cursor remain separate operations.
    template<class Publish>
    bool publishResources(const Gta5NativeSceneResolves& plan,const Gta5NativeSceneResolveStorage& storage,
            const Publish& publish) {
        if(!prefixExecuted_ || resourcesAttempted_ || storage.original.size()!=plan.destinations.size() ||
           storage.current.size()!=plan.destinations.size() ||
           (plan.destinations.empty() && (counts_.copies || counts_.registerWrites)))return false;
        resourcesAttempted_=true;
        for(unsigned i=0;i<plan.destinations.size();++i)
            if(storage.original[i].size()!=plan.destinations[i].bytes || storage.current[i].size()!=plan.destinations[i].bytes)return false;
        try {resourcesPublished_=publish(plan,storage);return resourcesPublished_;}
        catch(...) {return false;}
    }
private:
    Gta5NativeScenePass()=default;
    Counts counts_{};
    std::vector<std::uint8_t> sourceBytes_;
    std::vector<Extent> extents_;
    unsigned failedOffset_=0;
    bool prefixExecuted_=false,extentAttempted_=false,extentsPublished_=false;
    bool resourcesAttempted_=false,resourcesPublished_=false;
};
}
