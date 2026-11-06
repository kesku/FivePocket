// No display-image fallback, saved capture, fabricated clear or ring advance.
// The caller owns the same non-yielding gate and physical mutation throughout.
std::optional<bool> tryResolveRaw10Color(const PPCContext& ctx,
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
    const auto before=capture();
    const auto plan=planNativeRaw10ColorResolve(before);
    if(!plan)return std::nullopt; // Other profiles retain their existing routes.
    const auto reject=[&](const char* stage)->std::optional<bool> {
        rendererLog("RENDER_API_RAW10_RESOLVE_REJECT object=%08x destination=%08x stage=%s published=0\n",
            plan->sourceObject,plan->destinationObject,stage);return false;
    };
    // The planner admitted exactly the physical namespace or its A0000000
    // alias. Do not mask arbitrary virtual addresses into physical ownership.
    const auto base=plan->destination.baseAddress;
    const auto physicalBase=base<0x20000000u?base:base-0xA0000000u;
    const auto allocation=inlineRegistry->stoppedGraphicsPhysicalOwnership(physicalBase,plan->Bytes);
    if(!allocation || !allocation->writable)return reject("writable-destination-owner");
    const auto address=textureStorageAddress(plan->destination,plan->Bytes);
    if(!address || std::uint64_t{allocation->virtualBase}+physicalBase-allocation->physicalBase!=*address)
        return reject("destination-alias");
    // Texture pixels must not overwrite the resource header that described
    // them. Heap headers live in a disjoint virtual namespace.
    if(std::uint64_t{*address}<std::uint64_t{plan->destinationObject}+52 &&
       std::uint64_t{plan->destinationObject}<std::uint64_t{*address}+plan->Bytes)
        return reject("destination-header-overlap");
    const auto previous=graphics(*address,plan->Bytes);
    if(!previous)return reject("destination-bytes");
    const char* stage="not-packed";
    const bool published=publishNativeRaw10ColorResolve(before,physical,*previous,[&] {
        const auto current=inlineRegistry->stoppedGraphicsPhysicalOwnership(physicalBase,plan->Bytes);
        return current && current->writable && current->physicalBase==allocation->physicalBase &&
            current->virtualBase==allocation->virtualBase && current->byteCount==allocation->byteCount &&
            current->protection==allocation->protection && sameNativeRaw10ColorInputs(before,capture());
    },[&](const auto& completed) {
        return inlineRegistry->publishSerializedGraphicsData(*address,*previous,completed);
    },&stage);
    if(!published)return reject(stage);
    live->second.completedTextures[*address]={plan->destination,plan->Bytes};++liveResolves;
    rendererLog("RENDER_API_RAW10_RESOLVE object=%08x destination=%08x storage=%08x bytes=%u "
        "physical_source_serial=%llu published=1 display_valid=%u source_clear=0 ring_advance=0 edram64_model=xenia40x16\n",
        plan->sourceObject,plan->destinationObject,*address,plan->Bytes,
        static_cast<unsigned long long>(physical.nextSerial()-1),unsigned(live->second.valid));
    return true;
}
