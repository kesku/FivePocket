// Included after the ABI22 owners, under the renderer's Android guard. Runs
// under the existing exclusive, non-yielding gate. nullopt means this is not a
// native-owned rectangle MRT source; false is a real unsupported/failed resolve.
//
// This is the counterpart of tryResolveFixed16Target for the ABI22 band. The
// ABI22 draw registered its own owner but had no resolve route, so the resolve
// fell through to the display-gated path and was refused with
// stage=display-owner-invalid — the display image being invalid is the DESIGNED
// state after offscreen work, because the inline gate clears live->valid on
// every inline draw and offscreen work does not resurrect it. A completed
// offscreen source has its own owner and must not inherit presentation validity.
std::optional<bool> tryResolveRectangleMrtTarget(const PPCContext& ctx) {
    using namespace recomp::gpu;
    if(!inlineRegistry)return std::nullopt;
    const auto live=liveTargets.find(ctx.r3.u32);
    if(live==liveTargets.end() || !live->second.ownsPresentation)return std::nullopt;
    const auto state=inlineRegistry->copySerializedGuestHeap(ctx.r3.u32,13860);
    if(!state || state->size()!=13860)return std::nullopt;
    const auto word=[&](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // The registry is keyed by colour0's object, exactly as the draw registered
    // it, regardless of which colour this resolve selects as its source.
    const auto key=std::make_pair(ctx.r3.u32,word(*state,12816));
    const auto found=rectangleMrtTargets.find(key);
    if(found==rectangleMrtTargets.end())return std::nullopt;
    const auto reject=[&](const char* stage)->std::optional<bool> {
        rendererLog("RENDER_API_RECTANGLE_MRT_RESOLVE_REJECT object=%08x destination=%08x flags=%08x stage=%s completed=0\n",
            key.second,ctx.r6.u32,ctx.r4.u32,stage);
        return false;
    };
    auto& owner=found->second;
    if(!owner.valid || !owner.image || !owner.image->valid())return reject("source-owner");
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    const auto bindings=captureGta5BoundSurfaces(*state,heap,graphics,40);
    if(!bindings)return reject("source-state");
    // The SELECTED band must still be bound with the exact header the draw
    // completed against. The other band may already be unbound: the game
    // resolves colour1, drops it, then resolves colour0 from the same completed
    // target, and that second resolve is as legitimate as the first. What is
    // never tolerated is a DIFFERENT object appearing in either slot, which
    // would mean this is no longer the content the owner recorded.
    const auto& sourcePlan=owner.image->plan();
    const auto selected=ctx.r4.u32&7u;
    if(selected>=2)return reject("resolve-source-slot");
    for(unsigned slot=0;slot<2;++slot) {
        const auto& s=bindings->surfaces[slot];
        const auto expected=sourcePlan.targets[slot].object;
        const auto bound=word(*state,12816+slot*4);
        if(s.object!=bound)return reject("source-state");
        if(slot==selected) {
            if(bound!=expected || !s.header || *s.header!=owner.headers[slot])return reject("source-state");
        } else if(bound && (bound!=expected || !s.header || *s.header!=owner.headers[slot])) {
            return reject("source-state");
        }
    }
    for(unsigned slot=2;slot<5;++slot)
        if(bindings->surfaces[slot].object || word(*state,12816+slot*4))return reject("source-state");
    const auto header=captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics);
    const auto description=header?describeGta5TextureHeader(*header):std::nullopt;
    if(!description)return reject("destination-header");
    const NativeOffscreenResolveRequest request{ctx.r1.u32,ctx.r4.u32,ctx.r5.u32,
        ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
    const auto args=captureNativeOffscreenResolveArguments(request,
        [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
    const char* planStage="not-evaluated";
    const auto plan=planNativeRectangleMrtResolve(sourcePlan,request,args,*description,&planStage);
    if(!plan)return reject(planStage);
    const auto address=textureStorageAddress(plan->storage.description,plan->storage.sourceBytes);
    const auto before=address?graphics(*address,plan->storage.sourceBytes):std::nullopt;
    const auto attachment=owner.image->readRgba(plan->sourceSlot);
    if(!before || !attachment)return reject("completed-source-or-owned-destination");
    const auto completed=packNativeRectangleMrtResolve(*plan,*attachment,*before);
    if(!completed)return reject("packing");
    // Recheck every binding this decision rests on before publishing pixels.
    if(heap(ctx.r3.u32,13860)!=state ||
       captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics)!=header)return reject("changed-inputs");
    // Recheck exactly the bands this decision rested on. An unbound sibling was
    // not part of the decision, so demanding its header back would refuse a
    // resolve for a reason that never entered the plan.
    for(unsigned slot=0;slot<2;++slot) {
        if(slot!=selected && !word(*state,12816+slot*4))continue;
        if(heap(sourcePlan.targets[slot].object,40)!=std::optional{owner.headers[slot]})
            return reject("changed-inputs");
    }
    if(!inlineRegistry->publishSerializedGraphicsData(*address,*before,*completed))return reject("publication");
    live->second.completedTextures[*address]={plan->storage.description,plan->storage.sourceBytes};++liveResolves;
    rendererLog("RENDER_API_RECTANGLE_MRT_RESOLVE object=%08x destination=%08x storage=%08x source_slot=%u "
        "width=%u height=%u format=%u bytes=%zu policy=byte-identical published=1 display_valid=%u ring_advance=0\n",
        key.second,ctx.r6.u32,*address,plan->sourceSlot,plan->width,plan->height,
        nativeRectangleMrtResolveDestinationFormat(sourcePlan.formats[plan->sourceSlot]),
        completed->size(),unsigned(live->second.valid));
    return true;
}
