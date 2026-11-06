// Called under the exclusive inline gate and the enclosing physical mutation.
// A successful copy preserves invalid display state and ring state. A flag0x100
// profile then records its clear (RB_COLOR_CLEAR C0000000 from the original
// prefix) over the 8-row-rounded footprint; others keep the source.
std::optional<bool> tryResolveRgb7e3Float(const PPCContext& ctx,
        recomp::gpu::NativeEdram32Bands& physical) {
    using namespace recomp::gpu;
    if(!inlineRegistry)return std::nullopt;
    const auto live=liveTargets.find(ctx.r3.u32);
    if(live==liveTargets.end() || !live->second.ownsPresentation)return std::nullopt;
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    const auto arguments=[&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);};
    const auto capture=[&] {
        return captureGta5TileResolveBoundary(ctx.r1.u32,
            {ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
             static_cast<unsigned>(ctx.lr)},ctx.f1.u64,heap,graphics,arguments);
    };
    const auto before=capture();const auto plan=planNativeRgb7e3FloatResolve(before);
    if(!plan)return std::nullopt; // Unsupported profiles keep their old guards.
    const auto reject=[&](const char* stage)->std::optional<bool> {
        rendererLog("RENDER_API_RGB7E3_FLOAT_RESOLVE_REJECT object=%08x destination=%08x stage=%s published=0\n",
            plan->sourceObject,plan->destinationObject,stage);return false;
    };
    const auto bytes=plan->storage.sourceBytes,base=plan->destination.baseAddress;
    const auto physicalBase=base<0x20000000u?base:base-0xA0000000u;
    const auto allocation=inlineRegistry->stoppedGraphicsPhysicalOwnership(physicalBase,bytes);
    if(!allocation || !allocation->writable)return reject("writable-destination-owner");
    const auto address=textureStorageAddress(plan->destination,bytes);
    if(!address || std::uint64_t{allocation->virtualBase}+physicalBase-allocation->physicalBase!=*address)
        return reject("destination-alias");
    if(std::uint64_t{*address}<std::uint64_t{plan->destinationObject}+52 &&
       std::uint64_t{plan->destinationObject}<std::uint64_t{*address}+bytes)
        return reject("destination-header-overlap");
    const auto previous=graphics(*address,bytes);if(!previous)return reject("destination-bytes");
    const char* stage="not-packed";
    const bool published=publishNativeRgb7e3FloatResolve(before,physical,*previous,[&] {
        const auto current=inlineRegistry->stoppedGraphicsPhysicalOwnership(physicalBase,bytes);
        return current && current->writable && current->physicalBase==allocation->physicalBase &&
            current->virtualBase==allocation->virtualBase && current->byteCount==allocation->byteCount &&
            current->protection==allocation->protection && sameNativeRgb7e3FloatInputs(before,capture());
    },[&](const auto& completed) {
        return inlineRegistry->publishSerializedGraphicsData(*address,*previous,completed);
    },&stage);
    if(!published)return reject(stage);
    const auto serial=physical.nextSerial()-1;
    // After publication only: the pack above read the pre-clear words. If the
    // clear cannot be recorded the footprint becomes UNKNOWN instead (a refusal
    // there resets the whole owner), which stays fail-closed for the candidate.
    const bool cleared=!plan->clearsSource || recordNativeRgb7e3FloatResolveClear(physical,*plan);
    const bool footprintUnknown=!cleared;
    if(footprintUnknown)physical.invalidate(plan->base,plan->clearTiles);
    live->second.completedTextures[*address]={plan->destination,bytes};++liveResolves;
    rendererLog("RENDER_API_RGB7E3_FLOAT_RESOLVE object=%08x destination=%08x storage=%08x bytes=%u "
        "physical_source_serial=%llu published=1 display_valid=%u binary16=1 base=%u width=%u height=%u "
        "source_clear=%u clear_rows=%u clear_word=%08x clear_recorded=%u unknown_tiles=%u ring_advance=0\n",
        plan->sourceObject,plan->destinationObject,*address,bytes,
        static_cast<unsigned long long>(serial),unsigned(live->second.valid),plan->base,plan->width,plan->height,
        unsigned(plan->clearsSource),plan->clearsSource?nativeRgb7e3FloatResolveClearRows(plan->height):0u,
        plan->clearsSource?NativeRgb7e3FloatResolveClearWord:0u,unsigned(plan->clearsSource && cleared),
        footprintUnknown?plan->clearTiles:0u);
    return true;
}
