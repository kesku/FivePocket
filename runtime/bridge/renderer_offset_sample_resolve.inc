// Included after the ABI23 owner, under the renderer's Android guard. Runs under
// the existing exclusive, non-yielding gate. nullopt means this is not a
// native-owned offset sample source; false is a real unsupported/failed resolve.
//
// The counterpart of tryResolveRectangleMrtTarget for the ABI23 band, and it
// exists for the same reason: that band registered its own owner but had no
// resolve route, so the resolve fell through to the display-gated path and was
// refused with display-owner-invalid. The display image being invalid is the
// DESIGNED state after offscreen work, because the inline gate clears
// live->valid on every inline draw and offscreen work does not resurrect it.
std::optional<bool> tryResolveOffsetSampleTarget(const PPCContext& ctx) {
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
    const auto key=std::make_pair(ctx.r3.u32,word(*state,12816));
    const auto found=offsetSampleTargets.find(key);
    if(found==offsetSampleTargets.end())return std::nullopt;
    // The flags word's vocabulary is the guest's own, read out of sub_823C2738
    // (which sub_82388A38 forwards r3/r4/r5 to unchanged). That function tests
    // exactly these fields of it and nothing else:
    //
    //   bits 4-6   copy_sample_select      bits 8,9  colour/depth clear enable
    //   bits 13,14,15  three single-bit controls
    //   bits 26-31 a six-bit field, rotated out at 823C2738+367 and assembled
    //              with one more bit into a destination descriptor
    //
    // Bits 0-3, 7, 10-12 and 16-25 are never read there, so copy_src_select is
    // NOT taken from this word by this path. Breaking the fields out here costs
    // one log line and turns the next refusal into an answer rather than a
    // guess; the destination's own decoded format is reported with them.
    const auto reject=[&](const char* stage,const XenosTextureDescription* destination=nullptr)->std::optional<bool> {
        const auto f=ctx.r4.u32;
        unsigned destFormat=64,destWidth=0,destHeight=0,destInteger=2,destEndian=64;
        std::array<unsigned,4> destSign{9,9,9,9};
        if(destination) {
            if(const auto d=XenosTextureDescription::decode(destination->fetch)) {
                destFormat=d->format;destWidth=d->width;destHeight=d->height;
                destSign=d->sign;destInteger=unsigned(d->integerComponents);
                destEndian=destination->fetch[1]&3u;
            }
        }
        rendererLog("RENDER_API_OFFSET_SAMPLE_RESOLVE_REJECT object=%08x destination=%08x flags=%08x "
            "sample_select=%u colour_clear=%u depth_clear=%u bit13=%u bit14=%u bit15=%u field26_31=%u "
            "unread_bits=%08x destination_format=%u destination_extent=%ux%u "
            // The destination's own numeric selects decide whether the copy is
            // a bias or a range remap: a signed destination would be read back
            // exactly as the target stores it, an unsigned one would not.
            "destination_sign=%u,%u,%u,%u destination_integer=%u destination_endian=%u "
            "source_format=%u stage=%s completed=0\n",
            key.second,ctx.r6.u32,f,(f>>4)&7u,(f>>8)&1u,(f>>9)&1u,(f>>13)&1u,(f>>14)&1u,(f>>15)&1u,
            (f>>26)&63u,f&0x03ff1c8fu,destFormat,destWidth,destHeight,
            destSign[0],destSign[1],destSign[2],destSign[3],destInteger,destEndian,
            found->second.image?found->second.image->plan().format:64u,stage);
        return false;
    };
    auto& owner=found->second;
    if(!owner.valid || !owner.image || !owner.image->valid())return reject("source-owner");
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    const auto bindings=captureGta5BoundSurfaces(*state,heap,graphics,40);
    if(!bindings)return reject("source-state");
    // The single written band must still be bound with the exact header the draw
    // completed against, and nothing else may have appeared in the other slots.
    const auto& sourcePlan=owner.image->plan();
    const auto& s=bindings->surfaces[0];
    if(s.object!=key.second || key.second!=sourcePlan.object ||
       !s.header || *s.header!=owner.header)return reject("source-state");
    for(unsigned slot=1;slot<5;++slot)
        if(bindings->surfaces[slot].object || word(*state,12816+slot*4))return reject("source-state");
    const auto header=captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics);
    const auto description=header?describeGta5TextureHeader(*header):std::nullopt;
    if(!description)return reject("destination-header");
    const XenosTextureDescription* const reported=&*description;
    const NativeOffscreenResolveRequest request{ctx.r1.u32,ctx.r4.u32,ctx.r5.u32,
        ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
    const auto args=captureNativeOffscreenResolveArguments(request,
        [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
    const char* planStage="not-evaluated";
    const auto plan=planNativeOffsetSampleResolve(sourcePlan,request,args,*description,&planStage);
    if(!plan)return reject(planStage,reported);
    const auto address=textureStorageAddress(plan->storage.description,plan->storage.sourceBytes);
    const auto before=address?graphics(*address,plan->storage.sourceBytes):std::nullopt;
    const auto attachment=owner.image->readRgba();
    if(!before || !attachment)return reject("completed-source-or-owned-destination");
    const auto completed=packNativeOffsetSampleResolve(*plan,*attachment,*before);
    if(!completed)return reject("packing");
    // Recheck every binding this decision rests on before publishing pixels.
    if(heap(ctx.r3.u32,13860)!=state ||
       captureNativeResolveTextureHeader(ctx.r6.u32,heap,graphics)!=header ||
       heap(sourcePlan.object,40)!=std::optional{owner.header})return reject("changed-inputs");
    if(!inlineRegistry->publishSerializedGraphicsData(*address,*before,*completed))return reject("publication");
    live->second.completedTextures[*address]={plan->storage.description,plan->storage.sourceBytes};++liveResolves;
    rendererLog("RENDER_API_OFFSET_SAMPLE_RESOLVE object=%08x destination=%08x storage=%08x "
        "width=%u height=%u format=%u bytes=%zu policy=%s published=1 display_valid=%u ring_advance=0\n",
        key.second,ctx.r6.u32,*address,plan->width,plan->height,
        // The destination's OWN format. Deriving it from the source instead
        // reports 0 for the red-channel route, whose source has no equivalent
        // destination format -- that is the whole reason it converts.
        plan->storage.description.format,
        completed->size(),
        plan->copy==recomp::gpu::NativeOffsetSampleResolveCopy::RedChannelToR8?"red-channel-to-r8":
        plan->copy==recomp::gpu::NativeOffsetSampleResolveCopy::SnormToUnorm16?
            "snorm-to-unorm16":"byte-identical",
        unsigned(live->second.valid));
    return true;
}
