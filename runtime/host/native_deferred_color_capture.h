#pragma once
#include "native_deferred_color_sync.h"
#include "native_color_chain_capture.h"

namespace recomp::gpu {
// Stopped request/input admission for the combined submission. NOT a live
// memory lease, backend plan, fence receipt, publication or retirement token.
// The current reader must enforce allocation ownership with all writers joined.
// Whole ring/WB/identifier/lifecycle aliases still need a separate live gate.
class NativeDeferredColorCapture final {
public:
    NativeDeferredColorCapture(const NativeDeferredColorCapture&)=delete;
    NativeDeferredColorCapture& operator=(const NativeDeferredColorCapture&)=delete;
    NativeDeferredColorCapture(NativeDeferredColorCapture&&) noexcept=default;
    NativeDeferredColorCapture& operator=(NativeDeferredColorCapture&&) noexcept=default;
    static std::optional<NativeDeferredColorCapture> create(const NativeDeferredSwapSubmission& frame,
            const DrawBinState& bins,const NativeColorChainProgram& program,const Pm4PhysicalReader& reader,
            unsigned* rejection=nullptr) noexcept {
        const auto stage=[&](unsigned value){if(rejection)*rejection=value;};stage(1);
        if(!reader || program.coordinates()!=NativeColorCoordinateProfile::Padded1280W1)return std::nullopt;
        try{
            NativeDeferredColorCapture candidate(frame,bins);DrawCommandError error;
            const auto protectedRange=[&](unsigned address,unsigned count){
                const auto overlaps=[&](unsigned base,unsigned bytes){return address<std::uint64_t{base}+bytes && base<std::uint64_t{address}+count;};
                if(overlaps(frame.physicalAddress(),static_cast<unsigned>(frame.prefix().words().size()*4)+NativeDeferredSwapWrapper::Bytes) ||
                    overlaps(frame.swap().command().identity().imagePhysical,NativeSwapImagePlan::Capacity) ||
                    overlaps(frame.swap().scratchPhysical(),32))return true;
                for(const auto& event:frame.swap().events())if(overlaps(event.physicalAddress,4))return true;
                return false;
            };
            std::size_t sourceBytes=0;
            const Pm4PhysicalReader bounded=[&](unsigned address,unsigned count)->std::optional<std::vector<std::uint8_t>>{
                if(!count || (address|count)&3 || std::uint64_t{address}+count>0x20000000 ||
                    count>2*1024*1024-sourceBytes || protectedRange(address,count))return std::nullopt;
                sourceBytes+=count;return reader(address,count);
            };
            stage(2);if(!planDrawCommandCandidates(frame.prefix(),bins,bounded,candidate.plan_,error))return std::nullopt;
            stage(3);if(!planNativeDeferredColorSync(frame,candidate.plan_,candidate.sync_))return std::nullopt;
            stage(4);if(!planNativeColorCopyRequest(candidate.plan_,3,bounded,candidate.copy_))return std::nullopt;
            stage(5);if(candidate.copy_.copy.destinationBase!=frame.swap().command().identity().imagePhysical)return std::nullopt;
            stage(6);
            candidate.color_=NativeColorChainCapture::create(candidate.plan_,{0,1,2},program,bounded);
            if(!candidate.color_)return std::nullopt;
            candidate.sourceBytes_=static_cast<unsigned>(sourceBytes);stage(0);return candidate;
        }catch(...){return std::nullopt;}
    }
    const NativeDeferredSwapSubmission& submission() const noexcept{return frame_;}
    const DrawBinState& initialBins() const noexcept{return initialBins_;}
    const DrawCommandPlan& commands() const noexcept{return plan_;}
    const NativeDeferredColorSyncRequests& synchronization() const noexcept{return sync_;}
    const NativeColorCopyRequest& copy() const noexcept{return copy_;}
    const NativeColorChainCapture& color() const noexcept{return *color_;}
    unsigned sourceBytes() const noexcept{return sourceBytes_;}
private:
    NativeDeferredColorCapture(const NativeDeferredSwapSubmission& frame,const DrawBinState& bins):frame_(frame),initialBins_(bins){}
    NativeDeferredSwapSubmission frame_;
    DrawBinState initialBins_;
    DrawCommandPlan plan_;
    NativeDeferredColorSyncRequests sync_;
    NativeColorCopyRequest copy_;
    std::optional<NativeColorChainCapture> color_;
    unsigned sourceBytes_=0;
};
}
