#pragma once
#include "native_color_chain_resources.h"
#include "native_color_chain_commands.h"
#include "native_color_chain_inputs.h"
#include "native_color_chain_readback.h"
#include "native_color_edram_snapshot.h"
#include "native_color_chain_program.h"
#include "native_color_chain_capture.h"
#include "native_color_batch_capture.h"
#include "native_color_execution_result.h"
#include "native_deferred_color_sync_progress.h"
#include "xenos_draw_bin_predicate.h"
#include "native_tiled_copy_publication.h"
#include <cstdio>
#include <cstdlib>

namespace recomp::gpu {
#if defined(RECOMP_NATIVE_COLOR_FAULT_TESTING)
// Available ONLY in the standalone negative-test target, never VulkanEngine or
// the title trace. Deliberately corrupt a CPU-visible staging guard AFTER a real
// fence. The normal audit, not a manufactured failure result, must reject it.
inline int nativeColorAuditFaultStage=-1;
#endif
// Synchronous execution of the explicit bounded native profile, NOT a guest
// command interpreter. Caller has already validated module identity/ABI and
// device capabilities, source lifetimes, predicates/state and ordering policy.
// No PM4 wait/cache register, guest event, guest memory or RPTR is changed here.
// Shader framing alone is not module identity or SPIR-V instruction validation.
//
// Like the existing native backend/probes, queue-submit or fence failure is
// fail-stop (_Exit(4)): never destroy resources potentially still in use, invent
// completion, or publish partial output. This is not Android UI recovery.
// Rejection/audit failure returns false with caller output unchanged. All source
// packing and private resource creation precede work; output is owned before
// those resources are destroyed. There is NO expected-pixel argument.
inline bool executeNativeColorChain(ProbeDevice& device,
        const NativeColorChainResources<true,true,true>::Code& code,
        const NativeColorChainResources<true,true,true>::DepthCode& depthCode,
        const NativeColorChainBindings& bindings,std::uint32_t destinationBase,
        const NativeWordUpdatePlan& finalEvents,NativeColorChainExecutionResult& output,
        NativeColorCoordinateProfile coordinates=NativeColorCoordinateProfile::DiagnosticPositive,
        bool trackOwnedSync=false,NativeDeferredColorSyncProgress* deferredProgress=nullptr,
        std::vector<std::uint8_t>* capturedInitialHalf=nullptr) {
    using Resources=NativeColorChainResources<true,true,true>;
    using P=Resources::Profile;
    ColorCopyPlan copyPlan;
    if(!device.device || !device.physical || !device.queue || !validNativeColorChainEvents(finalEvents) ||
       !validNativeColorCoordinateProfile(coordinates) ||
       (trackOwnedSync && (coordinates!=NativeColorCoordinateProfile::Padded1280W1 || finalEvents.count!=2)) ||
       (deferredProgress && (trackOwnedSync || !deferredProgress->readyForStages() || finalEvents.count ||
           coordinates!=NativeColorCoordinateProfile::Padded1280W1)) ||
       !P::validModuleFraming(code) || !P::validModuleFraming(depthCode) ||
       !planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,destinationBase,0x2D00500,0x1000300,copyPlan))return false;
    try {
        PackedNativeColorChainInputs input;if(!packNativeColorChainInputs(bindings,input))return false;
        const auto events=finalEvents;
        NativeWordUpdatePlan noEvents;noEvents.bufferBytes=P::EventBytes;
        Resources r(device.device);r.depth.source=std::move(input.depth);
        r.colorViewportMode=coordinates==NativeColorCoordinateProfile::Padded1280W1?
            FragmentViewportMode::Color1280ShaderHalfPixel:FragmentViewportMode::DiagnosticPositive;
        const P::BufferSizes sizes{unsigned(input.pairs[0].size()),unsigned(input.pairs[1].size()),
            P::OutputBytes,P::BackingBytes,P::CopyBytes,P::EventBytes};
        if(!r.setup(device,code,sizes,&depthCode))return false;
        for(unsigned i=0;i<2;++i)std::memcpy(r.mapped[i],input.pairs[i].data(),input.pairs[i].size());
        NativeColorChainExecutionResult candidate;
        std::vector<std::uint8_t> initialHalf;
        NativeColorSyncProgress timeline;
        // Copy in only a freshly completed leading fence. Output progress is
        // failure-atomic, and never reconstructed from a final fence count.
        auto prefix=deferredProgress?*deferredProgress:NativeDeferredColorSyncProgress{};
        for(unsigned step=0;step<4;++step){
            if(trackOwnedSync && ((step && !timeline.depthWaitSatisfied()) || !timeline.begin(step)))return false;
            if(deferredProgress && ((step && !prefix.depthWaitSatisfied()) || !prefix.begin(step)))return false;
            for(unsigned i=2;i<6;++i)std::memset(r.mapped[i],0x5A,sizes[i]);
            const auto& stageEvents=step==3?events:noEvents;
            if(!recordNativeColorChainStage<true,true,true>(r,step,stageEvents))return false;
            VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
            if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
               vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){std::fflush(stdout);std::_Exit(4);}
            ++candidate.nativeFences;r.planes.ready=true;r.imageReady=true;
#if defined(RECOMP_NATIVE_COLOR_FAULT_TESTING)
            if(nativeColorAuditFaultStage==static_cast<int>(step)){
                static_cast<std::uint8_t*>(r.mapped[4])[0]^=1;
                std::printf("NATIVE_COLOR_FAULT_INJECTION after_real_fence=%u staging_guard_changed=1 guest_writes=0\n",candidate.nativeFences);
            }
#endif
            if(!r.depth.unchanged())return false;
            for(unsigned i=0;i<2;++i)if(std::memcmp(r.mapped[i],input.pairs[i].data(),input.pairs[i].size()))return false;
            if(!auditNativeColorChainReadback(static_cast<const std::uint8_t*>(r.mapped[2]),P::OutputBytes,
                    static_cast<const std::uint8_t*>(r.mapped[3]),P::BackingBytes,
                    static_cast<const std::uint8_t*>(r.mapped[4]),P::CopyBytes))return false;
            std::array<std::uint8_t,40> expectedEvents;expectedEvents.fill(0x5A);
            for(std::size_t i=0;i<stageEvents.count;++i){const auto& w=stageEvents.words[i];
                std::memcpy(expectedEvents.data()+w.byteOffset,w.bigEndianBytes.data(),4);}
            if(std::memcmp(r.mapped[5],expectedEvents.data(),expectedEvents.size()))return false;
            if(step==0 && capturedInitialHalf) {
                const auto* pixels=static_cast<const std::uint8_t*>(r.mapped[2])+16;
                initialHalf.assign(pixels,pixels+1280*720*8);
            }
            // A raw request literal, queue submission, or fence alone is not
            // enough. Advance only after real fence AND all visibility/source/
            // output audits. No guest register, event or RPTR is written here.
            if(trackOwnedSync && !timeline.finishAuditedStage(step))return false;
            if(deferredProgress && !prefix.finishAuditedStage(step))return false;
        }
        if(trackOwnedSync){if(!timeline.complete())return false;candidate.auditedTimeline=timeline;}
        if(deferredProgress && !prefix.complete())return false;
        candidate.edram=NativeColorEdramSnapshot::capture(static_cast<const std::uint8_t*>(r.mapped[2]),P::OutputBytes,
            static_cast<const std::uint8_t*>(r.mapped[3]),P::BackingBytes,static_cast<const std::uint8_t*>(r.mapped[4]),P::CopyBytes);
        if(!candidate.edram)return false;
        candidate.copy=NativeTiledCopyPublication::capture(copyPlan,static_cast<const std::uint8_t*>(r.mapped[4]),P::CopyBytes);
        if(!candidate.copy)return false;
        std::memcpy(candidate.eventBytes.data(),r.mapped[5],candidate.eventBytes.size());
        output=std::move(candidate);
        if(deferredProgress)*deferredProgress=prefix;
        if(capturedInitialHalf)*capturedInitialHalf=std::move(initialHalf);
        return true;
    } catch (...) {return false;}
}
inline bool executeMatchedNativeColorChain(ProbeDevice& device,const NativeColorChainProgram& program,
        const NativeColorChainProgram::Identities& active,const std::array<DrawBinPredicate,3>& predicates,
        const NativeColorChainBindings& bindings,
        std::uint32_t destinationBase,const NativeWordUpdatePlan& events,NativeColorChainExecutionResult& output,
        bool trackOwnedSync=false,NativeDeferredColorSyncProgress* deferredProgress=nullptr,
        std::vector<std::uint8_t>* capturedInitialHalf=nullptr){
    if(!program.matches(active))return false;
    // This fixed profile requires all three raster stages. A known skipped draw
    // is not an error in PM4 generally, but needs a different native sequence.
    // Predicates are explicit command-order state supplied by the caller; this
    // helper does not assume the retained state survived intervening writes.
    for(const auto& p:predicates)
        if(evaluateDrawBinPredicate(p.state,p.predicated)!=DrawBinDecision::Pass)return false;
    if(program.coordinates()==NativeColorCoordinateProfile::Padded1280W1){
        NativeDepthClearGeometry depthGeometry;
        if(!planNativeDepthClearGeometry(active[0],bindings.depth,depthGeometry))return false;
        for(unsigned role=0;role<2;++role){NativeColorVertexGeometry geometry;
            if(!planNativeColorVertexGeometry(active[1+role*2],bindings.pairs[role],role,geometry))return false;}
    }
    return executeNativeColorChain(device,program.code(),program.depthCode(),bindings,destinationBase,events,output,program.coordinates(),trackOwnedSync,deferredProgress,capturedInitialHalf);
}
// Own the command-selected sources across packing/execution. This overload does
// not expand the capture's admission scope: whole-batch effects/ownership still
// belong to the future live caller, and output is not guest publication.
inline bool executeCapturedNativeColorChain(ProbeDevice& device,const NativeColorChainProgram& program,
        const NativeColorChainCapture& capture,std::uint32_t destinationBase,
        const NativeWordUpdatePlan& events,NativeColorChainExecutionResult& output,bool trackOwnedSync=false,
        NativeDeferredColorSyncProgress* deferredProgress=nullptr,
        std::vector<std::uint8_t>* capturedInitialHalf=nullptr) {
    NativeColorChainBindings bindings;
    if (capture.coordinates()!=program.coordinates() || !capture.bindings(bindings)) return false;
    return executeMatchedNativeColorChain(device,program,capture.active(),capture.predicates(),bindings,destinationBase,events,output,trackOwnedSync,deferredProgress,capturedInitialHalf);
}
// Use the same immutable frontend object for sources, copy destination and event
// requests. There are no independent destination/event overrides to mix with a
// different batch. This executes the bounded DIAGNOSTIC native profile only:
// pending guest WAIT/cache/register effects, lifecycle leases and publication
// still belong to a future live transaction. No guest pointer is accepted here.
inline bool executeCapturedNativeColorBatch(ProbeDevice& device,const NativeColorChainProgram& program,
        const NativeColorBatchCapture& capture,NativeColorChainExecutionResult& output) {
    if(capture.controls().rootWords[2]!=capture.commands().commands.words().size() ||
        // The later profile adds a leading idle/setup obligation. Its metadata
        // capture is supported, but this startup executor has not fulfilled it.
        capture.synchronization().phase!=NativeColorBatchPhase::Startup ||
        program.coordinates()!=NativeColorCoordinateProfile::Padded1280W1)return false;
    return executeCapturedNativeColorChain(device,program,capture.color(),capture.copy().copy.destinationBase,
        capture.events().staging,output,true);
}
}
