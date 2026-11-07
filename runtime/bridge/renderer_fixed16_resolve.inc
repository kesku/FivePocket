// Included after native fixed16 owners, under the renderer's Android guard.
// Runs under the existing exclusive, non-yielding gate. nullopt means this is
// not a native-owned fixed16 source; false is a real unsupported/failed resolve.
std::optional<bool> tryResolveFixed16Target(const PPCContext& ctx) {
    using namespace recomp::gpu;
    if(!inlineRegistry || (ctx.r4.u32&7u))return std::nullopt;
    const auto live=liveTargets.find(ctx.r3.u32);
    if(live==liveTargets.end() || !live->second.ownsPresentation)return std::nullopt;
    const auto state=inlineRegistry->copySerializedGuestHeap(ctx.r3.u32,13860);
    if(!state || state->size()!=13860)return std::nullopt;
    std::uint32_t object=0;for(unsigned i=0;i<4;++i)object=(object<<8)|(*state)[12816+i];
    const auto found=fixed16Targets.find({ctx.r3.u32,object});
    if(found==fixed16Targets.end())return std::nullopt;
    const auto reject=[&](const char* stage)->std::optional<bool> {
        rendererLog("RENDER_API_FIXED16_RESOLVE_REJECT object=%08x destination=%08x stage=%s completed=0\n",object,ctx.r6.u32,stage);
        return false;
    };
    auto& owner=found->second;
    if(!owner.valid || !owner.image || !owner.image->valid())return reject("source-owner");
    const auto& p=owner.image->plan();
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    const auto bindings=captureGta5BoundSurfaces(*state,heap,graphics,40);
    if(!bindings || !bindings->surfaces[0].header || *bindings->surfaces[0].header!=owner.header ||
       (p.depth && (!bindings->surfaces[4].header || *bindings->surfaces[4].header!=owner.depthHeader)) ||
       !nativeFixed16ResolveSourceMatches(p,*state,*bindings))return reject("source-state");
    const auto header=captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics);
    const auto description=header?describeGta5TextureHeader(*header):std::nullopt;
    const NativeOffscreenResolveRequest request{ctx.r1.u32,ctx.r4.u32,ctx.r5.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
    const auto args=captureNativeOffscreenResolveArguments(request,
        [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
    const auto plan=description?planNativeFixed16Resolve(p,request,args,*description):std::nullopt;
    if(!plan)return reject("copy-arguments-or-format");
    const auto address=textureStorageAddress(plan->storage.description,plan->storage.sourceBytes);
    const auto before=address?graphics(*address,plan->storage.sourceBytes):std::nullopt;
    const auto encoded=owner.image->encodedSamples();
    if(!before || !encoded)return reject("completed-source-or-owned-destination");
    const auto completed=packNativeFixed16Resolve(*plan,*encoded,*before);
    if(!completed)return reject("conversion-or-packing");
    // No clear in this contract, so the source and its serial stay untouched.
    // Recheck the binding/header connection before publishing the real pixels.
    if(heap(ctx.r3.u32,13860)!=state || captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics)!=header ||
       heap(object,40)!=bindings->surfaces[0].header ||
       (p.depth && heap(p.depth->object,40)!=bindings->surfaces[4].header))return reject("changed-inputs");
    if(!inlineRegistry->publishSerializedGraphicsData(*address,*before,*completed))return reject("publication");
    live->second.completedTextures[*address]={plan->storage.description,plan->storage.sourceBytes};++liveResolves;
    rendererLog("RENDER_API_FIXED16_RESOLVE object=%08x destination=%08x storage=%08x width=%u height=%u format=30 bytes=%zu source_serial=%llu policy=float32-scale-half-rtz published=1 display_valid=%u ring_advance=0\n",
        object,ctx.r6.u32,*address,p.width,p.height,completed->size(),static_cast<unsigned long long>(owner.serial),unsigned(live->second.valid));
    return true;
}
