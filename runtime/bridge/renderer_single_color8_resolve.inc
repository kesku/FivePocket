// Included inside the existing Android renderer namespace. ONE caller-owned
// exclusive gate/physical transaction; no query, fence or ring completion.
std::optional<bool> tryResolveSingleColor8Target(const PPCContext& ctx,
        recomp::gpu::NativeEdram32Mutation& mutation) {
    using namespace recomp::gpu;
    if(!inlineRegistry)return std::nullopt;
    const auto display=liveTargets.find(ctx.r3.u32);
    if(display==liveTargets.end() || !display->second.ownsPresentation)return std::nullopt;
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    const auto arguments=[&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);};
    const auto state=heap(ctx.r3.u32,13860);
    if(!state || state->size()!=13860)return std::nullopt;
    const auto bindings=captureGta5BoundSurfaces(*state,heap,graphics,40);
    if(!bindings)return std::nullopt;
    const auto object=bindings->surfaces[0].object;
    const auto found=clearedColor8Targets.find({ctx.r3.u32,object});
    if(found==clearedColor8Targets.end() || !found->second.image)return std::nullopt;
    auto& owner=found->second;const auto& source=owner.image->plan();
    if(source.samples!=1 || source.format!=0 || source.depth)return std::nullopt;
    const auto reject=[&](const char* stage)->std::optional<bool> {
        rendererLog("RENDER_API_SINGLE_COLOR8_RESOLVE_REJECT source=%08x destination=%08x stage=%s display_valid=%u completed=0\n",
            object,ctx.r6.u32,stage,unsigned(display->second.valid));
        return false;
    };
    const auto header=captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics);
    const auto description=header?describeGta5TextureHeader(*header):std::nullopt;
    if(!description)return reject("destination-header");
    const NativeOffscreenResolveRequest request{ctx.r1.u32,ctx.r4.u32,ctx.r5.u32,
        ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
    const auto args=captureNativeOffscreenResolveArguments(request,arguments);
    const auto serial=owner.contentSerial;
    const auto plan=planNativeSingleColor8Resolve(source,display->second.ownsPresentation,display->second.object,
        owner.valid && serial && owner.image->isUniformClear(),*state,*bindings,owner.header,*description,request,args);
    if(!plan)return reject("source-or-request-profile");
    const auto address=textureStorageAddress(plan->destination.storage.description,plan->destination.storage.sourceBytes);
    const auto before=address?graphics(*address,plan->destination.storage.sourceBytes):std::nullopt;
    const auto rgba=owner.image->readUniformResolved();
    if(!before || !rgba)return reject("owned-destination-or-fenced-source");
    const auto current=[&] {
        if(!owner.valid || owner.contentSerial!=serial || !owner.image->isUniformClear() ||
           heap(ctx.r3.u32,13860)!=state || heap(object,40)!=std::optional{owner.header} ||
           captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics)!=header)return false;
        const auto now=captureNativeOffscreenResolveArguments(request,arguments);
        return now.rectangle==args.rectangle && now.point==args.point && now.color==args.color &&
            now.stack92==args.stack92 && now.stack100==args.stack100;
    };
    const char* why="not-executed";
    const bool completed=executeNativeSingleColor8Resolve(*plan,*rgba,*before,mutation,current,
        [&](const auto& pixels){return inlineRegistry->publishSerializedGraphicsData(*address,*before,pixels);},&why);
    if(!completed)return reject(why);
    display->second.completedTextures[*address]={plan->destination.storage.description,plan->destination.storage.sourceBytes};
    ++liveResolves;
    rendererLog("RENDER_API_SINGLE_COLOR8_RESOLVE source=%08x destination=%08x storage=%08x width=%u height=%u bytes=%u "
        "completed=1 display_valid=%u stage=%s query_result=0 ring_advance=0\n",
        object,ctx.r6.u32,*address,source.width,source.height,plan->destination.storage.sourceBytes,unsigned(display->second.valid),why);
    return true;
}
