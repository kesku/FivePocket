// Exclusive inline gate and enclosing physical mutation: the completed R32
// owner is independent of the invalid presentation image. Pure copy only.
std::optional<bool> tryResolveR32Float(const PPCContext& ctx,
        const recomp::gpu::NativeEdram32Bands& physical) {
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
    const auto before=capture();const auto plan=planNativeR32FloatResolve(before);
    if(!plan)return std::nullopt;
    const auto reject=[&](const char* stage)->std::optional<bool> {
        rendererLog("RENDER_API_R32_FLOAT_RESOLVE_REJECT object=%08x destination=%08x stage=%s published=0\n",
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
    const bool published=publishNativeR32FloatResolve(before,physical,*previous,[&] {
        const auto current=inlineRegistry->stoppedGraphicsPhysicalOwnership(physicalBase,bytes);
        return current && current->writable && current->physicalBase==allocation->physicalBase &&
            current->virtualBase==allocation->virtualBase && current->byteCount==allocation->byteCount &&
            current->protection==allocation->protection && sameNativeR32FloatInputs(before,capture());
    },[&](const auto& completed) {
        return inlineRegistry->publishSerializedGraphicsData(*address,*previous,completed);
    },&stage);
    if(!published)return reject(stage);
    live->second.completedTextures[*address]={plan->destination,bytes};++liveResolves;
    rendererLog("RENDER_API_R32_FLOAT_RESOLVE object=%08x destination=%08x storage=%08x bytes=%u "
        "physical_source_serial=%llu published=1 display_valid=%u bitwise=1 source_clear=0 ring_advance=0 "
        "source_base=%u width=%u height=%u\n",
        plan->sourceObject,plan->destinationObject,*address,bytes,
        static_cast<unsigned long long>(physical.nextSerial()-1),unsigned(live->second.valid),
        plan->Base,plan->Width,plan->Height);
    return true;
}
