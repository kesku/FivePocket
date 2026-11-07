// SPDX-License-Identifier: GPL-3.0-or-later
// Native fixed16 surface owners. Included after the renderer's memory/engine
// owners and logging. All operations below run inside the caller's inline gate.
#if (defined(__ANDROID__) || defined(__APPLE__))
struct Fixed16Target {
    std::unique_ptr<recomp::gpu::NativeFixed16ClearTarget> image;
    std::vector<std::uint8_t> header,depthHeader;
    bool valid=false;
    std::uint64_t serial=0;
};
std::map<std::pair<unsigned,unsigned>,Fixed16Target> fixed16Targets;
struct StoppedFixed16Target {
    unsigned device=0;
    recomp::gpu::Gta5Fixed16ClearPlan plan;
    std::vector<std::uint8_t> header,depthHeader,encoded;
    std::uint64_t serial=0;
    bool ownerValid=false,headersCurrent=false;
};
std::vector<StoppedFixed16Target> stoppedFixed16Targets;
bool stoppedFixed16CaptureFailed=false;

template<class Plan>
bool surfacePlanIntersects(const Plan& p,unsigned base,unsigned tiles) {
    const auto overlap=[&](unsigned b,unsigned n) {
        return n && tiles && std::uint64_t{b}<std::uint64_t{base}+tiles && std::uint64_t{base}<std::uint64_t{b}+n;
    };
    if(overlap(p.baseTile,p.tileCount))return true;
    if constexpr(requires{p.depth;})if(p.depth && overlap(p.depth->baseTile,p.depth->tileCount))return true;
    return false;
}
template<class A,class B>
bool surfacePlansOverlap(const A& a,const B& b) {
    if(surfacePlanIntersects(a,b.baseTile,b.tileCount))return true;
    if constexpr(requires{b.depth;})if(b.depth && surfacePlanIntersects(a,b.depth->baseTile,b.depth->tileCount))return true;
    return false;
}
template<class Plan>
void invalidateFixed16Overlaps(const Plan& plan,const std::optional<std::pair<unsigned,unsigned>>& except={}) {
    for(auto& [key,owner]:fixed16Targets)if(owner.image && (!except || key!=*except) && surfacePlansOverlap(owner.image->plan(),plan))owner.valid=false;
}
bool clearFixed16Target(unsigned device,const recomp::gpu::Gta5ClearCapture& captured,
        recomp::gpu::NativeEdram32Mutation& physicalMutation) {
    using namespace recomp::gpu;
    const auto audit=auditGta5Fixed16Clear(captured);const auto& plan=audit.plan;
    const auto reject=[&](const char* stage) {
        rendererLog("RENDER_API_FIXED16_CLEAR_REJECT object=%08x stage=%s issues=%08x completed=0\n",plan.object,stage,audit.issues);
        return false;
    };
    if(audit.issues)return reject("captured-request");
    if(!inlineRegistry || !inlineGraphics || device!=captured.call.device || !captured.state || !captured.surfaces[0].header)return reject("owner");
    const auto live=liveTargets.find(device);
    if(live==liveTargets.end() || !live->second.ownsPresentation || plan.object==live->second.object ||
       surfacePlanIntersects(plan,720,720))return reject("display-alias");
    const auto current=[&] {
        const auto actual=captureGta5ClearCall(captured.call,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
        if(actual.state!=captured.state || actual.rectangle!=captured.rectangle || actual.color!=captured.color)return false;
        for(unsigned i=0;i<5;++i)if(actual.surfaces[i].object!=captured.surfaces[i].object ||
            actual.surfaces[i].tileCache!=captured.surfaces[i].tileCache || actual.surfaces[i].header!=captured.surfaces[i].header)return false;
        return true;
    };
    if(!current())return reject("changed-inputs");
    const auto key=std::make_pair(device,plan.object);
    if(!fixed16Targets.count(key) && fixed16Targets.size()>=16)return reject("target-capacity");
    auto& owner=fixed16Targets[key];owner.valid=false;
    if(!owner.image || owner.image->plan()!=plan)owner.image=inlineGraphics->createFixed16ClearTarget(plan);
    if(!owner.image)return reject("native-storage");
    // Ownership changes BEFORE any GPU mutation. A failed clear cannot leave
    // an overlapping old color/depth view available for a subsequent consumer.
    invalidateFixed16Overlaps(plan,key);
    for(auto& [k,old]:clearedColor8Targets)if(old.image && surfacePlansOverlap(old.image->plan(),plan))old.valid=false;
    for(auto& [k,old]:offscreenTargets)if(old.image && surfacePlansOverlap(old.image->plan(),plan))old.valid=false;
    const auto depth=plan.depth?std::optional{plan.depth->value}:std::nullopt;
    const char* physicalStage="physical-footprint";
    if(!executeNativeEdram32Fixed16Write(physicalMutation,plan,[&] {
        physicalStage="gpu-clear";if(!owner.image->clear(plan.color,depth))return false;
        physicalStage="changed-inputs-after-gpu";return current();
    }))return reject(physicalStage);
    owner.header=*captured.surfaces[0].header;
    owner.depthHeader=captured.surfaces[4].header.value_or(std::vector<std::uint8_t>{});
    owner.valid=true;++owner.serial;
    rendererLog("RENDER_API_FIXED16_CLEAR object=%08x width=%u height=%u samples=%u format=%u packed=%08x depth=%u serial=%llu display_valid=%u completed=1 ring_advance=0\n",
        plan.object,plan.width,plan.height,plan.samples,plan.format,plan.color,unsigned(plan.depth.has_value()),
        static_cast<unsigned long long>(owner.serial),unsigned(live->second.valid));
    return true;
}
bool drawFixed16Target(unsigned device,const recomp::gpu::NativeTranslatedDrawProgram& program,
        const recomp::gpu::Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,
        const recomp::gpu::Gta5BoundSurfaceCapture& bindings,
        recomp::gpu::NativeEdram32Mutation& physicalMutation) {
    using namespace recomp::gpu;
    const auto object=bindings.surfaces[0].object;const auto key=std::make_pair(device,object);
    const auto reject=[&](const char* stage) {
        rendererLog("RENDER_API_FIXED16_DRAW_REJECT device=%08x object=%08x stage=%s completed=0\n",device,object,stage);
        return false;
    };
    if(!inlineRegistry || !inlineGraphics || device!=draw.device)return reject("execution-owner");
    const bool rectangle=validateNativeFixed16RectangleProgram(program);
    if(rectangle && !indices.empty())return reject("rectangle-has-indices");
    const auto live=liveTargets.find(device);auto found=fixed16Targets.find(key);
    if(rectangle && live!=liveTargets.end() && live->second.ownsPresentation &&
       (found==fixed16Targets.end() || !found->second.valid)) {
        // A new resource object may name the same eDRAM storage. Transfer ONE
        // current completed owner; do not allocate/clear a replacement or revive
        // an invalid view. All this runs under the existing exclusive gate.
        auto prior=fixed16Targets.end();std::optional<Gta5Fixed16ClearPlan> alias;
        for(auto it=fixed16Targets.begin();it!=fixed16Targets.end();++it) {
            const auto& old=it->second;
            if(it->first.first!=device || !old.valid || !old.image || !old.image->valid())continue;
            const auto candidate=planNativeFixed16ColorAlias(old.image->plan(),old.header,old.depthHeader,bindings);
            if(!candidate || !planNativeFixed16RectangleState(draw,program,*candidate,bindings))continue;
            if(prior!=fixed16Targets.end())return reject("ambiguous-alias-owner");
            prior=it;alias=candidate;
        }
        if(prior==fixed16Targets.end() || !alias)return reject("no-current-alias-owner");
        auto& old=prior->second;
        if(object==live->second.object || surfacePlanIntersects(*alias,720,720) ||
           inlineRegistry->copySerializedGuestHeap(prior->first.second,40)!=std::optional{old.header} ||
           inlineRegistry->copySerializedGuestHeap(alias->depth->object,40)!=std::optional{old.depthHeader} ||
           inlineRegistry->copySerializedGuestHeap(object,40)!=bindings.surfaces[0].header ||
           inlineRegistry->copySerializedGuestHeap(device,13860)!=std::optional{draw.deviceState})return reject("changed-alias-inputs");
        if(found==fixed16Targets.end() && fixed16Targets.size()>=16)return reject("target-capacity");
        if(!old.image->retagSameStorage(*alias))return reject("native-storage-retag");
        Fixed16Target transferred=std::move(old);old.valid=false;
        transferred.header=*bindings.surfaces[0].header;
        fixed16Targets.insert_or_assign(key,std::move(transferred));found=fixed16Targets.find(key);
        rendererLog("RENDER_API_FIXED16_ALIAS from=%08x object=%08x color_tile=%u depth_tile=%u storage_preserved=1 clear=0 draw_completed=0\n",
            prior->first.second,object,alias->baseTile,alias->depth->baseTile);
    }
    if(live==liveTargets.end() || !live->second.ownsPresentation || found==fixed16Targets.end())return reject("target-owner");
    auto& owner=found->second;
    if(!owner.valid || !owner.image || !owner.image->valid() || owner.serial==UINT64_MAX)return reject("target-content");
    const auto& plan=owner.image->plan();
    if(object==live->second.object || surfacePlanIntersects(plan,720,720))return reject("display-alias");
    if(!bindings.surfaces[0].header || *bindings.surfaces[0].header!=owner.header ||
       !bindings.surfaces[4].header || *bindings.surfaces[4].header!=owner.depthHeader)return reject("owner-headers");
    if(!(rectangle?planNativeFixed16RectangleState(draw,program,plan,bindings):
                   planNativeFixed16DrawState(draw,program,plan,bindings)))return reject("draw-state");
    std::vector<std::pair<unsigned,NativeMipTextureCapture>> textures;
    std::vector<NativeMipTextureUse> mipUses;
    std::vector<std::pair<std::uint32_t,LiveTarget::TextureUse>> baseUses;
    const auto slots=nativeTranslatedTextureSlots(program);
    const auto submitted=driverTextures.find(device);std::size_t textureBudget=64u*1024*1024;
    if(!slots.empty() && submitted==driverTextures.end())return reject("submitted-textures");
    for(auto slot:slots) {
        const auto fetch=submitted->second.fetch(slot);
        auto texture=fetch?captureNativeMipTextureStorage(*fetch,
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},textureBudget):std::nullopt;
        if(!texture || !planNativeGeometrySampler2D(texture->fetch))return reject("texture-planes-owner");
        textureBudget-=texture->base.size()+texture->mips.size();
        if(!texture->mips.empty()) {
            const auto use=mipTextureUse(texture->fetch);if(!use)return reject("mip-allocation-owner");
            mipUses.push_back(*use);
        } else {
            const auto desc=XenosTextureDescription::decode(texture->fetch);
            const auto address=desc?textureStorageAddress(*desc,texture->base.size()):std::nullopt;
            if(!address)return reject("base-allocation-owner");
            baseUses.emplace_back(*address,LiveTarget::TextureUse{*desc,static_cast<std::uint32_t>(texture->base.size())});
        }
        textures.emplace_back(slot,std::move(*texture));
    }
    const auto current=[&] {
        const auto state=inlineRegistry->copySerializedGuestHeap(device,static_cast<unsigned>(draw.deviceState.size()));
        if(!state || *state!=draw.deviceState)return false;
        const auto actual=captureGta5BoundSurfaces(*state,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},40);
        if(!actual)return false;
        for(unsigned i=0;i<5;++i)if(actual->surfaces[i].object!=bindings.surfaces[i].object ||
            actual->surfaces[i].tileCacheObject!=bindings.surfaces[i].tileCacheObject ||
            actual->surfaces[i].header!=bindings.surfaces[i].header)return false;
        for(const auto& [slot,texture]:textures)
            if(submitted->second.fetch(slot)!=std::optional{texture.fetch})return false;
        return true;
    };
    if(!current())return reject("changed-inputs");
    // The caller holds the real non-yielding owner gate. Invalidate aliases
    // before GPU writes; an offscreen draw never resurrects the display owner.
    owner.valid=false;invalidateFixed16Overlaps(plan,key);
    for(auto& [k,old]:clearedColor8Targets)if(old.image && surfacePlansOverlap(old.image->plan(),plan))old.valid=false;
    for(auto& [k,old]:offscreenTargets)if(old.image && surfacePlansOverlap(old.image->plan(),plan))old.valid=false;
    const char* physicalStage="physical-footprint";
    if(!executeNativeEdram32Fixed16Write(physicalMutation,plan,[&] {
        physicalStage="gpu-draw";
        if(!(rectangle?inlineGraphics->executeFixed16Rectangle(*owner.image,program,draw,bindings,textures):
            inlineGraphics->executeFixed16BoundGeometry(*owner.image,program,draw,indices,bindings,textures)))return false;
        physicalStage="changed-inputs-after-gpu";return current();
    }))return reject(physicalStage);
    owner.valid=true;++owner.serial;
    // Record only fenced source consumption. An offscreen draw does not
    // resurrect presentation or retire unrelated guest ring submissions.
    for(const auto& use:mipUses)live->second.completedMipTextures[use.baseAddress]=use;
    for(const auto& use:baseUses)live->second.completedTextures[use.first]=use.second;
    rendererLog("RENDER_API_FIXED16_DRAW object=%08x vertices=%u indices=%zu serial=%llu display_valid=%u completed=1 ring_advance=0 abi=%u textures=%zu\n",
        object,draw.vertexCount,indices.size(),static_cast<unsigned long long>(owner.serial),unsigned(live->second.valid),program.abiVersion,textures.size());
    return true;
}
void preserveStoppedFixed16Targets() noexcept {
    // All workers have joined; readback was already fenced by the native clear.
    // Preserve bytes before unbind destroys GPU owners. Invalidated views are
    // labeled historical and never reauthorized by this diagnostic copy.
    try {
        std::size_t remaining=16*1024*1024;
        for(const auto& [key,owner]:fixed16Targets)if(owner.image) {
            StoppedFixed16Target s;s.device=key.first;s.plan=owner.image->plan();
            s.header=owner.header;s.depthHeader=owner.depthHeader;s.serial=owner.serial;s.ownerValid=owner.valid;
            if(inlineRegistry) {
                const auto current=inlineRegistry->copySerializedGuestHeap(key.second,s.header.size());
                s.headersCurrent=current && *current==s.header;
                if(s.plan.depth) {
                    const auto z=inlineRegistry->copySerializedGuestHeap(s.plan.depth->object,s.depthHeader.size());
                    s.headersCurrent=s.headersCurrent && z && *z==s.depthHeader;
                }
            }
            if(std::uint64_t{s.plan.width}*s.plan.height*4<=remaining)if(auto bytes=owner.image->encodedSamples()) {
                remaining-=bytes->size();s.encoded=std::move(*bytes);
            }
            stoppedFixed16Targets.push_back(std::move(s));
        }
    } catch(...) {stoppedFixed16CaptureFailed=true;}
}
template<class Save>
bool saveStoppedFixed16Targets(const std::string& directory,const Save& save) {
    if(stoppedFixed16Targets.empty() && !stoppedFixed16CaptureFailed)return true;
    std::ofstream meta(std::filesystem::path(directory)/"fixed16-targets.json");
    meta<<"{\"version\":1,\"capture_failed\":"<<(stoppedFixed16CaptureFailed?"true":"false")
        <<",\"gameplay_verified\":false,\"sample_encoding\":\"little-endian-rg-signed-fixed16-scale32\",\"targets\":[";
    bool ok=true;
    for(unsigned i=0;i<stoppedFixed16Targets.size();++i) {
        const auto& s=stoppedFixed16Targets[i];const auto& p=s.plan;if(i)meta<<',';
        meta<<"{\"index\":"<<i<<",\"device\":"<<s.device<<",\"object\":"<<p.object<<",\"width\":"<<p.width
            <<",\"height\":"<<p.height<<",\"pitch\":"<<p.pitch<<",\"format\":"<<p.format<<",\"samples\":"<<p.samples
            <<",\"serial\":"<<s.serial<<",\"owner_valid\":"<<(s.ownerValid?"true":"false")
            <<",\"headers_current\":"<<(s.headersCurrent?"true":"false")<<",\"encoded_bytes\":"<<s.encoded.size()
            <<",\"chunk_bytes\":65536,\"subsequent_draw_execution_proven\":false}";
        const auto prefix="fixed16-"+std::to_string(i)+"-";
        ok=save(prefix+"surface-header.bin",s.header) && ok;
        if(!s.depthHeader.empty())ok=save(prefix+"depth-header.bin",s.depthHeader) && ok;
        if(!s.encoded.empty())ok=save(prefix+"color-samples.bin",s.encoded) && ok;
    }
    meta<<"]}";return bool(meta) && ok;
}
#endif
