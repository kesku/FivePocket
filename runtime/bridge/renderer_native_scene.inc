// SPDX-License-Identifier: GPL-3.0-or-later
// Live EndTiling connection. Included after the live recording/input owners.
// No private fixture files, fixed ring indices, authored pixels or fake fences.
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
struct LiveNativeScene {
    enum class Phase {FirstSelect,FirstSubmit,FirstRendered,FirstRetired,SecondSubmit,SecondRendered,DeferredSubmit,FinalRetired};
    unsigned device=0,issuer=0,worker=0;
    Phase phase=Phase::FirstSelect;
    std::optional<recomp::gpu::Gta5NativeTileSubmissionCapture> capture;
    std::optional<recomp::gpu::Gta5NativeTileFirstScene> first;
    std::optional<recomp::gpu::Gta5NativeTileFinalScene> final;
    std::array<std::optional<recomp::gpu::Gta5NativeScenePass>,2> passes;
};
std::optional<LiveNativeScene> liveNativeScene;

unsigned liveSceneWord(const std::vector<std::uint8_t>& b,unsigned at) {
    if(at>b.size() || b.size()-at<4)throw std::runtime_error("Live scene word outside owned bytes");
    return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
}
bool liveSceneOverlap(unsigned a,std::size_t n,unsigned b,std::size_t m) {
    return n && m && std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
}
const BoundIndexedSnapshot* liveSceneDraw(unsigned index) {
    if(!pendingNativeSceneInputs || index>=pendingNativeSceneInputs->draws.size())return nullptr;
    const auto sequence=pendingNativeSceneInputs->draws[index].sequence;
    const auto found=std::find_if(boundIndexedSnapshots.begin(),boundIndexedSnapshots.end(),
        [&](const auto& d){return d.geometryRecordingOnly && d.sequence==sequence;});
    return found==boundIndexedSnapshots.end()?nullptr:&*found;
}
bool liveSceneClearPrefix() {
    // This captured path has all clears before the command-stream geometry.
    // Do not hoist an unknown later clear across a draw or resolve.
    return recomp::gpu::validGta5NativeSceneClearPrefix(tileRecordingDiagnostic.clears(),tileRecordingDiagnostic.events());
}
bool liveSceneInputsCurrent(const recomp::gpu::Gta5NativeTileSubmissionCapture& q) {
    if(!inlineRegistry || !liveNativeScene || !pendingNativeSceneInputs || !pendingNativeSceneResolves ||
       !pendingNativeSceneStorage || inlineRegistry->snapshotNativeInterruptDeviceRoot(q.device())!=std::optional{true})return false;
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto gfx=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    if(!q.matchesCurrent(heap,gfx))return false;
    std::vector<recomp::gpu::Gta5NativeSceneDrawSource> sources;
    for(unsigned i=0;i<pendingNativeSceneInputs->draws.size();++i) {
        const auto* s=liveSceneDraw(i);
        if(!s || !s->geometry || !s->surfaces || !s->recordedScissor || !s->originalEmissionReturned)return false;
        sources.push_back({s->sequence,s->orderedConstants,s->originalEmissionReturned,&*s->geometry,&*s->surfaces,
            &s->vertexCode,&s->pixelCode,&*s->recordedScissor,&s->mipTextures});
    }
    const auto check=recomp::gpu::checkGta5NativeSceneOwnedInputs(sources,*pendingNativeSceneResolves,*pendingNativeSceneStorage,heap,gfx);
    if(!check)rendererLog("RENDER_LIVE_SCENE_INPUT_REJECT issue=%u index=%u address=%08x\n",unsigned(check.issue),check.index,check.address);
    return bool(check);
}
bool publishLiveSceneResources(const recomp::gpu::Gta5NativeTileSubmissionCapture& q,
        const recomp::gpu::Gta5TileCommandInterrupt& irq,
        const recomp::gpu::Gta5NativeSceneResolves& plan,const recomp::gpu::Gta5NativeSceneResolveStorage& storage) {
    if(!liveSceneInputsCurrent(q) || plan.destinations.size()!=storage.original.size() ||
       plan.destinations.size()!=storage.current.size() || q.readVirtual()<60)return false;
    std::vector<recomp::bootstrap::GuestImportRegistry::SerializedGraphicsUpdate> updates;
    const auto protectedRange=[&](unsigned a,std::size_t n) {
        if(liveSceneOverlap(a,n,q.ringVirtual(),q.ringBytes()) || liveSceneOverlap(a,n,q.readVirtual()-60,64) ||
           liveSceneOverlap(a,n,irq.scratchPhysical()+0xA0000000,32) ||
           liveSceneOverlap(a,n,irq.callbackContext()-0x20000000,4))return true;
        for(const auto& s:q.submissions())for(const auto& ib:s.indirects)
            if(liveSceneOverlap(a,n,ib.physical+0xA0000000,ib.bytes.size()))return true;
        unsigned firstExtent=UINT32_MAX,lastExtent=0;
        for(const auto& c:pendingNativeSceneCommands->commands)
            if(c.kind==recomp::gpu::Gta5NativeSceneCommands::Kind::DeferredExtentWrite) {
                const auto physical=c.value&~3u;
                if(physical<8 || physical>0x1FFFFFEF)return true;
                firstExtent=std::min(firstExtent,physical-8);lastExtent=std::max(lastExtent,physical+16);
            }
        // Include the original block's link/header and terminal word, not
        // only its XY payload, before publishing any resource bytes.
        if(lastExtent && liveSceneOverlap(a,n,firstExtent+0xA0000000,lastExtent-firstExtent))return true;
        for(unsigned i=0;i<pendingNativeSceneInputs->draws.size();++i) {
            const auto* s=liveSceneDraw(i);if(!s || !s->geometry)return true;
            const auto& g=*s->geometry;
            if(liveSceneOverlap(a,n,g.draw.vertexAddress+0xA0000000,g.draw.vertices.size()) ||
               liveSceneOverlap(a,n,g.indexPhysicalAddress+0xA0000000,g.rawIndices.size()*2))return true;
        }
        return false;
    };
    for(unsigned i=0;i<plan.destinations.size();++i) {
        const auto& d=plan.destinations[i];
        if(protectedRange(d.address,d.bytes))return false;
        updates.push_back({d.address,&storage.original[i],&storage.current[i]});
    }
    return inlineRegistry->publishSerializedGraphicsBatch(updates.data(),updates.size());
}
bool executeLiveScenePrefix(unsigned bin,const recomp::gpu::Gta5NativeTileSubmissionCapture& q,
        const recomp::gpu::Gta5TileCommandInterrupt& irq) {
    using namespace recomp::gpu;
    if(!inlineGraphics || !pendingNativeSceneTarget || !pendingNativeSceneCommands || !liveSceneClearPrefix() ||
       !liveSceneInputsCurrent(q) || bin>1 || liveNativeScene->passes[bin] || q.submissions().empty())return false;
    const auto& ib=q.submissions().front().indirects.front();
    const auto size=pendingNativeSceneCommands->sourceBytes.size();
    if(ib.bytes.size()!=size+Gta5TileSceneContinuation::Bytes)return false;
    auto plan=prepareGta5NativeSceneReplayCommands(pendingNativeSceneInputs->queryCommands,*pendingNativeSceneResolves,
        std::span<const std::uint8_t>(ib.bytes).first(size),2,bin!=0);
    if(!plan || (bin && (!liveNativeScene->passes[0] || !liveNativeScene->passes[0]->extentsPublished())))return false;
    const auto active=bin?0xCu:0x80000003u;
    const auto& bins=inlineGraphics->stateBatchSnapshot();
    if(!(bins.knownBinWords&4) || unsigned(bins.binSelect)!=active)return false;
    const auto& clears=tileRecordingDiagnostic.clears();
    if(clears.empty())return false;
    NativeEdram32Mutation physicalMutation(liveEdram32[q.device()]);
    const auto physicalPlans=prepareNativeSceneColorAliases(physicalMutation,clears.front(),bin);
    if(!physicalPlans)return false;
    if(bin && !pendingNativeSceneTarget->carryCapturedTile(clears.front(),0,bin))return false;
    for(const auto& clear:clears)
        if((liveSceneWord(*clear.state,13084)&active) && !pendingNativeSceneTarget->clearCapturedTile(clear,bin))return false;
    auto pass=Gta5NativeScenePass::execute(*plan,*inlineGraphics,*pendingNativeSceneTarget,
        [&]{return liveSceneInputsCurrent(q);},
        [&](unsigned i,unsigned control) {
            const auto* s=liveSceneDraw(i);if(!s || !s->geometry || !s->recordedScissor)return false;
            const auto& prepared=pendingNativeSceneInputs->draws.at(i);
            if(prepared.programIndex>=nativeLogicalGeometryPrograms.size() ||
               gta5DrawRegister(s->geometry->draw,0x2293)!=std::optional{control})return false;
            const NativeClippedFanProgram* query=nullptr;
            if(control&1) {
                if(prepared.queryProgramIndex>=nativeLogicalGeometryQueryPrograms.size())return false;
                query=&nativeLogicalGeometryQueryPrograms[prepared.queryProgramIndex];
            }
            return inlineGraphics->executeLogicalGeometry(*pendingNativeSceneTarget,nativeLogicalGeometryPrograms[prepared.programIndex],
                s->geometry->draw,s->geometry->effectiveIndices,prepared.surfaces,bin,*s->recordedScissor,s->mipTextures,nullptr,query);
        },[&](unsigned i){return executeGta5NativeSceneResolve(*pendingNativeSceneResolves,i,bin,*pendingNativeSceneTarget,*pendingNativeSceneStorage);},
        [&](unsigned physical,const std::array<std::uint8_t,4>& bytes) {
            unsigned matches=0;
            for(unsigned i=0;i<pendingNativeSceneResolves->destinations.size();++i) {
                const auto& d=pendingNativeSceneResolves->destinations[i];
                if(physical>=d.address-0xA0000000 && std::uint64_t{physical}+4<=std::uint64_t{d.address}-0xA0000000+d.bytes) {
                    auto& output=pendingNativeSceneStorage->current[i];
                    std::copy(bytes.begin(),bytes.end(),output.begin()+physical-(d.address-0xA0000000));++matches;
                }
            }
            return matches==1;
        });
    if(!pass.prefixExecuted()) {
        rendererLog("RENDER_LIVE_SCENE_PREFIX_FAILED bin=%u offset=%u commands=%u no_completion=1\n",bin,pass.failedOffset(),pass.counts().commands);
        return false;
    }
    if(!stageCompletedNativeSceneColors(physicalMutation,*physicalPlans,[&](unsigned slot) {
        return pendingNativeSceneTarget->readCapturedColorTile(clears.front(),bin,slot);
    })) {
        rendererLog("RENDER_LIVE_SCENE_COLOR_ALIAS_REJECT bin=%u physical_published=0\n",bin);
        return false;
    }
    if(!pass.publishResources(*pendingNativeSceneResolves,*pendingNativeSceneStorage,
        [&](const auto& resources,const auto& storage){return publishLiveSceneResources(q,irq,resources,storage);}))return false;
    pendingNativeSceneStorage->original=pendingNativeSceneStorage->current;
    if(!liveSceneInputsCurrent(q))return false;
    physicalMutation.commit();
    rendererLog("RENDER_LIVE_SCENE_COLOR_ALIAS bin=%u rows=%u known_tiles=%u same_target_fenced=1 physical_published=1\n",
        bin,(*physicalPlans)[0].physical.height,liveEdram32[q.device()].knownTiles());
    rendererLog("RENDER_LIVE_SCENE_PREFIX bin=%u draws=%u copies=%u register_writes=%u resources_published=1 ring_retired=0\n",
        bin,pass.counts().indexed,pass.counts().copies,pass.counts().registerWrites);
    liveNativeScene->passes[bin]=std::move(pass);return true;
}
bool publishLiveSceneBounds() {
    using namespace recomp::gpu;
    const auto reject=[](const char* stage){rendererLog("RENDER_LIVE_SCENE_BOUNDS_REJECT stage=%s bounds_published=0\n",stage);return false;};
    if(!liveNativeScene || !liveNativeScene->first || !liveNativeScene->passes[0])return reject("scene-owner");
    const auto& q=liveNativeScene->first->queue();
    const auto& extents=liveNativeScene->passes[0]->extents();
    if(extents.empty() || extents.front().destinationPhysical<8 || !liveSceneInputsCurrent(q))return reject("current-inputs");
    const auto tablePhysical=extents.front().destinationPhysical-8,tableAddress=tablePhysical+0xA0000000;
    const auto header=inlineRegistry->copySerializedGraphicsData(tableAddress,4);if(!header)return reject("table-header-owner");
    const auto link=liveSceneWord(*header,0);
    if(link<0xC0000000 || link>=0xE0000000 || link-0xC0000000<=tablePhysical ||
       std::uint64_t{link-0xC0000000}-tablePhysical+4>65536)return reject("table-link");
    const auto table=inlineRegistry->copySerializedGraphicsData(tableAddress,link-0xC0000000-tablePhysical+4);
    const auto state=inlineRegistry->copySerializedGuestHeap(q.device(),21944);if(!table || !state)return reject("table-device-owner");
    const auto count=liveSceneWord(*state,11200);if(count!=2)return reject("tile-count");
    std::vector<std::array<unsigned,4>> rectangles;
    for(unsigned i=0;i<count;++i) {
        std::array<unsigned,4> r{};for(unsigned c=0;c<4;++c)r[c]=liveSceneWord(*state,11204+i*16+c*4);rectangles.push_back(r);
    }
    auto extent=liveNativeScene->first->prepareExtentPublication({tableAddress,tablePhysical,*table},rectangles);
    if(!extent)return reject("drawing-prefix-bindings");
    const bool published=liveNativeScene->passes[0]->publishExtents(*extent,[&](auto& plan) {
        return inlineGraphics->publishTileExtentBounds(plan,
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
            [&](auto a,const auto& before,const auto& after){return inlineRegistry->publishSerializedGraphicsData(a,before,after);});
    });
    if(!published)return reject("executed-bounds-publication");
    rendererLog("RENDER_LIVE_SCENE_BOUNDS events=%zu current_table_published=1 irq_tail_pending=1 ring_retired=0\n",extent->bindings().size());
    return true;
}
bool beginLiveSceneSelect(unsigned device,const std::function<void()>& original) {
    if(!liveNativeScene)return false;
    auto& s=*liveNativeScene;using Phase=LiveNativeScene::Phase;
    if(device!=s.device || !originalRecordingGate || !nativeSceneScheduler.currentToken)
        throw std::runtime_error("Live scene SELECT has no current owner");
    bool done=false;
    const bool gated=originalRecordingGate([&] {
        const auto token=nativeSceneScheduler.currentToken();
        if(!token || token==s.issuer || (s.worker && s.worker!=token))return;
        if(s.phase==Phase::FirstSelect || s.phase==Phase::FirstRetired) {
            s.capture=recomp::gpu::Gta5NativeTileSubmissionCapture::begin(device,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
            if(!s.capture)return;
            s.worker=token;original();
            s.phase=s.phase==Phase::FirstSelect?Phase::FirstSubmit:Phase::SecondSubmit;done=true;
        } else if(s.phase==Phase::SecondRendered) {
            original();s.phase=Phase::DeferredSubmit;done=true;
        }
    });
    if(!gated || !done)throw std::runtime_error("Live scene SELECT order/ownership refused");
    return true;
}
bool submitLiveNativeScene(unsigned device,const std::function<void()>& original) {
    if(!liveNativeScene)return false;
    auto& s=*liveNativeScene;using Phase=LiveNativeScene::Phase;using namespace recomp::gpu;
    if(device!=s.device || !s.capture || !originalRecordingGate || !nativeSceneScheduler.currentToken ||
       nativeSceneScheduler.currentToken()!=s.worker ||
       (s.phase!=Phase::FirstSubmit && s.phase!=Phase::SecondSubmit && s.phase!=Phase::DeferredSubmit))
        throw std::runtime_error("Live scene submission order/owner refused");
    bool done=false;
    const bool gated=originalRecordingGate([&] {
        const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
        const auto gfx=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
        if(activeSceneQueue || activeEndQueue)return;
        activeSceneQueue=&*s.capture;
        {struct Clear{~Clear(){activeSceneQueue=nullptr;}} clear;original();}
        auto q=*s.capture;if(!q.seal(heap,gfx) || q.submissions().empty())return;
        const auto& last=q.submissions().back();if(last.ringWords.size()!=5 || last.ringWords[0]!=0xC0006200)return;
        std::array<std::uint8_t,8> bytes{};
        for(unsigned i=0;i<2;++i)for(unsigned b=0;b<4;++b)bytes[i*4+b]=last.ringWords[i]>>(24-b*8);
        const auto select=NativeTileBinCommand::parse(bytes);
        if(!select || !inlineGraphics->executeLogicalBinCommand(*select))return;
        const auto& state=inlineGraphics->stateBatchSnapshot();
        const DrawBinState bins{{unsigned(state.binMask),unsigned(state.binMask>>32),unsigned(state.binSelect),unsigned(state.binSelect>>32)},state.knownBinWords};
        if(s.phase==Phase::FirstSubmit) {
            s.first=Gta5NativeTileFirstScene::prepare(q,bins);
            if(!s.first || !executeLiveScenePrefix(0,q,s.first->interrupt()) || !publishLiveSceneBounds())return;
            s.phase=Phase::FirstRendered;done=true;
        } else if(s.phase==Phase::SecondSubmit) {
            s.final=Gta5NativeTileFinalScene::prepare(q,bins);
            if(!s.final || !executeLiveScenePrefix(1,q,s.final->interrupt()))return;
            const auto memory=[&](const auto& op) {
                const auto& irq=s.final->interrupt();
                if(op.kind!=Gta5TileCommandInterrupt::Kind::ScratchWrite || op.target>=8 ||
                   std::find(irq.operations().begin(),irq.operations().end(),op)==irq.operations().end())return false;
                const auto a=irq.scratchPhysical()+0xA0000000+4*op.target;
                const auto before=gfx(a,4);if(!before)return false;
                std::vector<std::uint8_t> after(4);for(unsigned b=0;b<4;++b)after[b]=op.value>>(24-8*b);
                return inlineRegistry->publishSerializedGraphicsData(a,*before,after);
            };
            if(!s.final->completeScene(*s.passes[1],heap,gfx,[&](const auto& tail){
                return inlineGraphics->executeTileSceneContinuation(tail,memory,{});}))return;
            s.phase=Phase::SecondRendered;done=true;
        } else {
            if(!s.final || !s.final->appendDeferred(q,heap,gfx))return;
            const auto before=gfx(s.final->deferred()->fencePhysical(),8);if(!before)return;
            if(!s.final->completeDeferred(heap,gfx,[&](const auto& tail) {
                return inlineGraphics->executeTileDeferredTail(tail,*pendingNativeSceneTarget,[&](const auto& actual) {
                    // The exact original plan orders cursor then completion.
                    const auto expected=tail.stagingPlan();
                    if(actual.count!=expected.count || actual.bufferBytes!=expected.bufferBytes)return false;
                    for(unsigned i=0;i<actual.count;++i)
                        if(actual.words[i].byteOffset!=expected.words[i].byteOffset || actual.words[i].phase!=expected.words[i].phase ||
                           actual.words[i].bigEndianBytes!=expected.words[i].bigEndianBytes)return false;
                    std::vector<std::uint8_t> cursor(4),value(4);
                    for(unsigned b=0;b<4;++b){cursor[b]=tail.commandCursor()>>(24-8*b);value[b]=tail.completionValue()>>(24-8*b);}
                    const std::vector<std::uint8_t> oldValue(before->begin(),before->begin()+4),oldCursor(before->begin()+4,before->end());
                    const auto address=tail.fencePhysical()+0xA0000000;
                    const std::array<recomp::bootstrap::GuestImportRegistry::SerializedGraphicsUpdate,2> writes{{
                        {address+4,&oldCursor,&cursor},{address,&oldValue,&value}}};
                    return inlineRegistry->publishSerializedGraphicsBatch(writes);
                });
            }))return;
            done=true;
        }
    });
    if(!gated || !done)throw std::runtime_error("Live native scene effects failed; no ring completion");
    if(s.phase==Phase::DeferredSubmit) {
        if(!inlineRegistry->retireNativeTileFinalScene(*s.final,originalRecordingGate))
            throw std::runtime_error("Live final scene ring retirement refused");
        s.phase=Phase::FinalRetired;
        rendererLog("RENDER_LIVE_FINAL_SCENE_RING read=%u write=%u resources_and_deferred_completed=1\n",
            s.final->queue().entryReadWord(),s.final->queue().lastWriteWord());
    }
    return true;
}
bool completeLiveNativeScene() {
    using Phase=LiveNativeScene::Phase;
    if(!liveNativeScene || !nativeSceneScheduler.wait || !nativeSceneScheduler.parked || !nativeSceneContinuation)return false;
    auto& s=*liveNativeScene;
    const auto originalWorkerParked=[&]{return s.worker && nativeSceneScheduler.parked(s.worker) &&
        inlineRegistry->nativeTileSceneWaitingWorker(s.device)==std::optional{s.worker};};
    if(!nativeSceneScheduler.wait([&]{return s.phase==Phase::FirstRendered && originalWorkerParked();}) ||
       !s.first || !s.passes[0])return false;
    const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
    const auto gfx=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
    // Rendering ran on the ordinary worker, then this owner yielded until it
    // parked. Recheck CURRENT resources after that scheduling interval too.
    if(!liveSceneInputsCurrent(s.first->queue()) ||
       !s.first->complete(*s.passes[0],heap,gfx,nativeSceneContinuation) ||
       !inlineRegistry->retireNativeTileFirstScene(*s.first,originalRecordingGate))return false;
    s.phase=Phase::FirstRetired;
    rendererLog("RENDER_LIVE_FIRST_SCENE_RING read=%u write=%u original_worker_parked=1\n",
        s.first->queue().entryReadWord(),s.first->queue().lastWriteWord());
    if(!nativeSceneScheduler.wait([&]{return s.phase==Phase::FinalRetired && originalWorkerParked();}))return false;
    bool drained=false;
    if(!inlineGate([&]{
        const auto state=heap(s.device,21944);const auto lock=inlineRegistry->nativeTileLockScopeStatus();
        drained=state && liveSceneWord(*state,11152)==0 && liveSceneWord(*state,11156)==0 &&
            lock.returned && lock.balanced && !lock.failed && !lock.held && lock.worker==s.worker;
    }) || !drained)return false;
    return true;
}
#endif
