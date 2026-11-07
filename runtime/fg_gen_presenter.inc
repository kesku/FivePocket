    // ---- framegen-1b (frame generation, stage 1b: the generator; fg_gen.h, fg_gen_core.glsl). Included INSIDE class Presenter
    // as its last members, so the members above keep their offsets. Our own code.
    // While files/private-trace/diag-fg-between exists (the renderer's window poll sets fg_pair.h's SwBetween and fg_gen.h's
    // SwGenBetween), the pair of stage 1a is shown with a MADE picture as its first picture: [made(k-1, k), real k]. The made
    // picture is drawn in front of the unchanged presenter passes by fgRecord's hook fgGenRecord():
    //   LOW (the 4 x 4 box of this real picture, kept for the next pair) -> GAIN (1x1) -> CLASSIFY (160x90) -> RESOLVE x 3 ->
    //   SYNTHESIS (into fgOut), then stage 1a's copy of the real picture into fgPrev.
    // Inputs per frame (fgGenFrame, called by the renderer on the thread that then calls presentGpu): the present image's view,
    // the frame's scene depth view and its camera (fg_gen.h FrameIn). A frame without usable inputs is shown alone (no pair); the first frame
    // after such a gap, and a frame after a camera cut, is shown as [real, the same picture again] and kept; then pictures
    // are made again. When made pictures do not keep their slot the slot keeper (fg_gen.h) stops the pairs altogether:
    // "RENDER_DIRECT_FG_GEN fallback=real-only why=..." and every decision is the 30 fps cap's until the cool-down is over.
    // Without diag-fg-between: fgGenAllows() is one load and a test, fgGenBefore() and fgGenRecord() one compare each, and
    // none of them is reached unless a diag-fg-pair-* file is present (stage 1a's own first test).
    // Repair after check c2 (2026-10-05): a frame's inputs are judged in fgGenAllows(), i.e. BEFORE the pair is planned, so a
    // refused frame is never planned as a timed pair: stage 1a counts it under skip=failed, lane A's pacer is told the frame
    // is untimed (pairsTimed(false)) and the frame is presented exactly as with the cap alone. (Before, the pair was planned
    // first and dropped afterwards: the pacer released the frame with the timed lead and trim, and it was then presented
    // without a desired time.) Only the generator's own objects are still made after the plan (they need stage 1a's
    // pictures): when that fails, that ONE frame is planned and then dropped; from the next frame on the refusal comes first.
    // Descriptor sets: four per swapchain image (LOW, GAIN, CLASSIFY, SYNTHESIS), rewritten at every use - presentLocked() has
    // waited for the previous pass on that image before it calls the recorder - and two fixed ones for the resolve rounds.
    // (framegen-cost-b: since framegen-cost the second of the four is GAIN COLUMNS's and GAIN has a fixed set; now the third
    // is TERMS's and CLASSIFY has a fixed set.)
    // Lifetime: as fgPrev / fgOut (made once, never destroyed before the process ends). 7.5 MB (two low-passed pictures,
    // two block maps, the gain texel) beside stage 1a's two pictures. (framegen-cost-b: and 14.7 MB for the terms: one RGBA32F texel per pixel.)
    // framegen-cost (2026-10-06), two changes, both only while diag-fg-between is on (every edit is marked framegen-cost):
    //   - the GAIN pass is drawn in two steps (fg_gen_core.glsl: GAIN COLUMNS into a 160x2 picture, then GAIN from it):
    //     one more picture (1,280 bytes), one more framebuffer, shader module and pipeline, one more fixed set. EXACT:
    //     the gain picture has the bytes it had;
    //   - one timed made picture in three (fg_gen.h PassEvery) carries six more timestamps, so that fg_us can be split by
    //     pass: "RENDER_DIRECT_FG_GEN pass_us=..." behind the counters line. The other made pictures are stamped as in
    //     v467; fg_us and what the slot keeper is given are v467's for every picture.
    // framegen-cost-b (2026-10-06, stage B1), only while diag-fg-between is on (every edit is marked framegen-cost-b):
    //   - the CLASSIFY pass is drawn in two steps (fg_gen_core.glsl: TERMS into a picture of three 32-bit floats per
    //     pixel, then CLASSIFY from it): one more picture (RGBA32F, 14.7 MB), render pass, framebuffer, shader module and
    //     pipeline, one more fixed set. EXACT: the block map has the bytes it had. A device that cannot draw into or
    //     sample that format gets no generator: "RENDER_DIRECT_FG_GEN failed=terms-format", then the cap alone;
    //   - a split picture carries seven stamps (one more, behind TERMS); the pass line has the field terms:.
public:
    // The renderer, just before presentGpu of the same frame (the render thread; read by fgBefore on that thread, under m).
    void fgGenFrame(const recomp::fgg::FrameIn& in) noexcept {fgIn=in;}
    // The presenter's switch word as stage 1a's poll left it (the renderer adds the generator's bits to it at the same poll).
    unsigned fgSwitchWord() const noexcept {return fg.switches();}
    // The renderer's 60-present window: the tuning files and the audit request (plain values of the render thread).
    void fgGenTune(float fail,unsigned lateMaxPct,unsigned auditToken) noexcept {fgTuneFail=fail;fgTuneLate=lateMaxPct;fgTuneAudit=auditToken;}
private:
    static_assert(recomp::fgg::PairBetween==recomp::fg::SwBetween,"the pair's in-between bit");
    struct FgTarget {Img img;VkFramebuffer framebuffer=VK_NULL_HANDLE;};
    recomp::fgg::SlotKeeper fgKeeper;recomp::fgg::Kept fgKeptFrame;recomp::fgg::Decision fgDecision;
    recomp::fgg::FrameIn fgIn;
    float fgTuneFail=recomp::fgg::K::Fail;unsigned fgTuneLate=recomp::fgg::K::LateMaxPct,fgTuneAudit=0,fgAuditSeen=0;
    recomp::fgg::Act fgGenAct=recomp::fgg::Act::Dry;
    std::array<FgTarget,2> fgLow,fgBlocks;FgTarget fgGain;VkFramebuffer fgOutFramebuffer=VK_NULL_HANDLE;
    VkSampler fgLinear=VK_NULL_HANDLE;VkRenderPass fgPassRead=VK_NULL_HANDLE,fgPassOut=VK_NULL_HANDLE;
    VkDescriptorSetLayout fgGenSetLayout=VK_NULL_HANDLE;VkPipelineLayout fgGenLayout=VK_NULL_HANDLE;VkDescriptorPool fgGenPool=VK_NULL_HANDLE;
    static constexpr unsigned FgShaders=5,FgMaxImages=16,FgSetsPerImage=4; // (shaders: low, gain, classify, resolve, synthesis)
    static constexpr unsigned FgGainCols=FgShaders; // framegen-cost: the sixth module, GAIN COLUMNS. Its module and pipeline are among this lane's members at the end, so v467's two arrays (and every member behind them) stay where they were
    static constexpr unsigned FgTerms=FgShaders+1;  // framegen-cost-b: the seventh module, TERMS (its handles: this lane's members at the very end)
    static constexpr VkFormat FgTermsFormat=VK_FORMAT_R32G32B32A32_SFLOAT; // framegen-cost-b: three 32-bit floats per texel, kept as they are
    std::array<VkShaderModule,FgShaders> fgGenModule{};std::array<VkPipeline,FgShaders> fgGenPipeline{};
    std::array<VkDescriptorSet,2> fgResolveSet{};std::array<std::array<VkDescriptorSet,FgSetsPerImage>,FgMaxImages> fgGenSet{};
    VkQueryPool fgGenTime=VK_NULL_HANDLE;unsigned fgGenTimedSlot=~0u;Buffer fgAuditBuffer;
    unsigned fgLowAt=0,fgGenImageIndex=~0u;
    bool fgGenBuilt=false,fgGenFailed=false,fgLowValid=false,fgGenAnnounced=false,fgAuditThis=false,fgGenWasOn=false,fgGenBlindSaid=false;
    std::uint64_t fgGenFrames=0,fgGenMadeN=0,fgGenKeepN=0,fgGenTimeNs=0,fgGenTimeN=0,fgGenTimeMissed=0,fgGenCallNs=0,fgGenFallbacks=0,fgGenAudits=0;
    std::array<std::uint64_t,unsigned(recomp::fgg::Why::Count)> fgGenWhy{};

    // The generator's objects, once. False: it cannot run in this run (logged; the keeper is disabled, the cap alone remains).
    [[gnu::noinline]] bool fgGenEnsure() noexcept {
        namespace fgg=recomp::fgg;
        if(fgGenBuilt)return !fgGenFailed;
        fgGenBuilt=true;
        const auto fail=[&](const char* what){fgGenFailed=true;RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN failed=%s",what);return false;};
        const auto t0=std::chrono::steady_clock::now();
        if(!vertex || !sampler || !fgOut.view || images.size()>FgMaxImages)return fail("presenter-objects");
        // framegen-cost-b: the terms' picture must hold three 32-bit floats per texel as they are: R32G32B32A32_SFLOAT, drawn
        // into and read with texelFetch (no filtering, no blending). Asked before anything is made: without it the
        // generator is refused whole (never a picture from terms that were rounded).
        {VkFormatProperties tf{};vkGetPhysicalDeviceFormatProperties(dev->physical,FgTermsFormat,&tf);
         constexpr VkFormatFeatureFlags need=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
         if((tf.optimalTilingFeatures&need)!=need)return fail("terms-format");}
        VkSamplerCreateInfo sc{};sc.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;sc.magFilter=sc.minFilter=VK_FILTER_LINEAR;sc.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU=sc.addressModeV=sc.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sc.maxLod=0;
        if(vkCreateSampler(dev->device,&sc,nullptr,&fgLinear)!=VK_SUCCESS){fgLinear=VK_NULL_HANDLE;return fail("sampler");}
        // two render passes of one RGBA8 picture: one leaves it for sampling, one (the made picture) in GENERAL as stage 1a keeps fgOut
        // (framegen-cost-b: and a third, the first one's twin for the terms' RGBA32F picture)
        for(unsigned k=0;k<3;++k) {
            VkAttachmentDescription at{};at.format=k==2?FgTermsFormat:VK_FORMAT_R8G8B8A8_UNORM;at.samples=VK_SAMPLE_COUNT_1_BIT;at.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            at.storeOp=VK_ATTACHMENT_STORE_OP_STORE;at.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            at.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at.finalLayout=k==1?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
            std::array<VkSubpassDependency,2> dep{};
            dep[0].srcSubpass=VK_SUBPASS_EXTERNAL;dep[0].dstSubpass=0;dep[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
            dep[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            dep[1].srcSubpass=0;dep[1].dstSubpass=VK_SUBPASS_EXTERNAL;dep[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dep[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
            dep[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;rp.attachmentCount=1;rp.pAttachments=&at;
            rp.subpassCount=1;rp.pSubpasses=&sp;rp.dependencyCount=2;rp.pDependencies=dep.data();
            VkRenderPass& out=k==2?fgPassTerms:k?fgPassOut:fgPassRead;
            if(vkCreateRenderPass(dev->device,&rp,nullptr,&out)!=VK_SUCCESS){out=VK_NULL_HANDLE;return fail("render-pass");}
        }
        const auto target=[&](FgTarget& t,std::uint32_t w,std::uint32_t h,VkImageUsageFlags more,VkFormat format=VK_FORMAT_R8G8B8A8_UNORM,VkRenderPass pass=VK_NULL_HANDLE) { // (framegen-cost-b: format, pass)
            VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;i.imageType=VK_IMAGE_TYPE_2D;i.format=format;
            i.extent={w,h,1};i.mipLevels=1;i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
            i.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|more;i.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            if(vkCreateImage(dev->device,&i,nullptr,&t.img.image)!=VK_SUCCESS){t.img.image=VK_NULL_HANDLE;return false;}
            VkMemoryRequirements r{};vkGetImageMemoryRequirements(dev->device,t.img.image,&r);
            const auto type=memoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if(!type)return false;
            VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=*type;
            if(vkAllocateMemory(dev->device,&a,nullptr,&t.img.memory)!=VK_SUCCESS){t.img.memory=VK_NULL_HANDLE;return false;}
            if(vkBindImageMemory(dev->device,t.img.image,t.img.memory,0)!=VK_SUCCESS)return false;
            VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=t.img.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
            v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            if(vkCreateImageView(dev->device,&v,nullptr,&t.img.view)!=VK_SUCCESS){t.img.view=VK_NULL_HANDLE;return false;}
            VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=pass?pass:fgPassRead;f.attachmentCount=1;f.pAttachments=&t.img.view;
            f.width=w;f.height=h;f.layers=1;
            return vkCreateFramebuffer(dev->device,&f,nullptr,&t.framebuffer)==VK_SUCCESS;
        };
        for(auto& t:fgLow)if(!target(t,fgW,fgH,0))return fail("low-picture");
        for(auto& t:fgBlocks)if(!target(t,(fgW/fgg::Block),(fgH/fgg::Block),VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return fail("block-map");
        if(!target(fgGain,1,1,VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return fail("gain-picture");
        // framegen-cost: the gain's column sums. RGBA8 like every other picture here (the one format the presenter already
        // renders into and samples; a column sum fits three of its bytes exactly, fg_gen_core.glsl)
        if(!target(fgGainCols,(fgW/fgg::Block),fgg::GainColsRows,0))return fail("gain-columns");
        // framegen-cost-b: CLASSIFY's per-pixel terms (the format asked for above; drawn with its own render pass)
        if(!target(fgTerms,fgW,fgH,0,FgTermsFormat,fgPassTerms))return fail("terms-picture");
        {VkFramebufferCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;f.renderPass=fgPassOut;f.attachmentCount=1;f.pAttachments=&fgOut.view;
         f.width=fgW;f.height=fgH;f.layers=1;
         if(vkCreateFramebuffer(dev->device,&f,nullptr,&fgOutFramebuffer)!=VK_SUCCESS){fgOutFramebuffer=VK_NULL_HANDLE;return fail("made-framebuffer");}}
        // one set layout (four pictures), one pipeline layout (the 96-byte push block), one pool
        std::array<VkDescriptorSetLayoutBinding,4> b{};
        for(unsigned k=0;k<4;++k){b[k].binding=k;b[k].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[k].descriptorCount=1;b[k].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
        VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=4;sl.pBindings=b.data();
        if(vkCreateDescriptorSetLayout(dev->device,&sl,nullptr,&fgGenSetLayout)!=VK_SUCCESS){fgGenSetLayout=VK_NULL_HANDLE;return fail("set-layout");}
        VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(fgg::Push)};
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pl.setLayoutCount=1;pl.pSetLayouts=&fgGenSetLayout;
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pr;
        if(vkCreatePipelineLayout(dev->device,&pl,nullptr,&fgGenLayout)!=VK_SUCCESS){fgGenLayout=VK_NULL_HANDLE;return fail("pipeline-layout");}
        constexpr unsigned sets=FgMaxImages*FgSetsPerImage+4; // (framegen-cost: + the GAIN pass's fixed set; framegen-cost-b: + the CLASSIFY pass's)
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4*sets};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=sets;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
        if(vkCreateDescriptorPool(dev->device,&dp,nullptr,&fgGenPool)!=VK_SUCCESS){fgGenPool=VK_NULL_HANDLE;return fail("set-pool");}
        const auto allocate=[&](VkDescriptorSet& out) {
            VkDescriptorSetAllocateInfo sa{};sa.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sa.descriptorPool=fgGenPool;sa.descriptorSetCount=1;sa.pSetLayouts=&fgGenSetLayout;
            return vkAllocateDescriptorSets(dev->device,&sa,&out)==VK_SUCCESS;
        };
        for(auto& s:fgResolveSet)if(!allocate(s))return fail("sets");
        for(auto& per:fgGenSet)for(auto& s:per)if(!allocate(s))return fail("sets");
        if(!allocate(fgGainSet))return fail("sets"); // framegen-cost
        if(!allocate(fgTermsSet))return fail("sets"); // framegen-cost-b
        for(unsigned k=0;k<2;++k)fgGenWrite(fgResolveSet[k],k?fgg::PassResolve10:fgg::PassResolve01); // (the two block maps: fixed)
        fgGenWrite(fgGainSet,fgg::PassGain); // framegen-cost: the column sums (fixed)
        fgGenWrite(fgTermsSet,fgg::PassClassify); // framegen-cost-b: the terms (fixed)
        // seven pipelines (framegen-cost: v467's five and GAIN COLUMNS; framegen-cost-b: and TERMS): makePassAndPipeline's fixed state, the presenter's one-triangle vertex shader
        const std::array<std::pair<const std::uint32_t*,std::size_t>,FgShaders+2> code{{{fgg::spirv::Low,sizeof fgg::spirv::Low},{fgg::spirv::Gain,sizeof fgg::spirv::Gain},
            {fgg::spirv::Classify,sizeof fgg::spirv::Classify},{fgg::spirv::Resolve,sizeof fgg::spirv::Resolve},{fgg::spirv::Synth,sizeof fgg::spirv::Synth},
            {fgg::spirv::Gaincols,sizeof fgg::spirv::Gaincols}, // framegen-cost: + gain columns
            {fgg::spirv::Terms,sizeof fgg::spirv::Terms}}};     // framegen-cost-b: + terms
        std::array<VkPipelineShaderStageCreateInfo,2> st{};
        st[0].sType=st[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT;st[0].module=vertex;st[0].pName="main";st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;st[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{};cbs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;cbs.attachmentCount=1;cbs.pAttachments=&ba;
        const std::array<VkDynamicState,2> dyn{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;ds.dynamicStateCount=2;ds.pDynamicStates=dyn.data();
        VkGraphicsPipelineCreateInfo gp{};gp.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gp.stageCount=2;gp.pStages=st.data();
        gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;
        gp.pColorBlendState=&cbs;gp.pDynamicState=&ds;gp.layout=fgGenLayout;gp.subpass=0;
        for(unsigned k=0;k<=FgTerms;++k) { // (framegen-cost: the sixth module's two handles; framegen-cost-b: the seventh's)
            VkShaderModule& module=k<FgShaders?fgGenModule[k]:k==FgTerms?fgTermsModule:fgGainColsModule;VkPipeline& made=k<FgShaders?fgGenPipeline[k]:k==FgTerms?fgTermsPipeline:fgGainColsPipeline;
            VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;sm.codeSize=code[k].second;sm.pCode=code[k].first;
            if(vkCreateShaderModule(dev->device,&sm,nullptr,&module)!=VK_SUCCESS){module=VK_NULL_HANDLE;return fail("shader-module");}
            st[1].module=module;gp.renderPass=k==FgShaders-1?fgPassOut:k==FgTerms?fgPassTerms:fgPassRead;
            if(vkCreateGraphicsPipelines(dev->device,VK_NULL_HANDLE,1,&gp,nullptr,&made)!=VK_SUCCESS){made=VK_NULL_HANDLE;return fail("pipeline");}
        }
        if(timePool) { // the passes' GPU time, when the queue family has timestamps (as the presenter's own pass times)
            VkQueryPoolCreateInfo qp{};qp.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;qp.queryType=VK_QUERY_TYPE_TIMESTAMP;qp.queryCount=2*MaxTimed;
            if(vkCreateQueryPool(dev->device,&qp,nullptr,&fgGenTime)!=VK_SUCCESS)fgGenTime=VK_NULL_HANDLE;
            // framegen-cost: the split pictures' stamps (fg_gen.h PassStamps per acquire slot); without it the passes are timed as in v467
            if(fgGenTime){qp.queryCount=fgg::PassStamps*MaxTimed;if(vkCreateQueryPool(dev->device,&qp,nullptr,&fgGenPassTime)!=VK_SUCCESS)fgGenPassTime=VK_NULL_HANDLE;}
        }
        // (framegen-cost: bytes counts the column sums' picture too; pass_timed / every: one timed made picture in `every` is split by pass)
        // (framegen-cost-b: bytes counts the terms' picture too; terms=: its size and format, the last field)
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN objects=1 pictures=%ux%u blocks=%ux%u bytes=%u timed=%u ms=%.1f pass_timed=%u every=%u terms=%ux%u:rgba32f",fgW,fgH,(fgW/fgg::Block),(fgH/fgg::Block),
            unsigned(2u*fgW*fgH*4u+2u*(fgW/fgg::Block)*(fgH/fgg::Block)*4u+4u+(fgW/fgg::Block)*fgg::GainColsRows*4u+fgW*fgH*fgg::TermsTexelBytes),unsigned(fgGenTime!=VK_NULL_HANDLE),
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count(),unsigned(fgGenPassTime!=VK_NULL_HANDLE),fgg::PassEvery,fgW,fgH);
        return true;
    }
    // What a role's picture is in this frame, and the layout it is read in.
    VkDescriptorImageInfo fgGenInfo(recomp::fgg::Role role,bool linear) const noexcept {
        namespace fgg=recomp::fgg;
        const auto view=[](std::uint64_t v){return reinterpret_cast<VkImageView>(v);};
        const VkSampler s=linear?fgLinear:sampler;
        switch(role) {
        case fgg::Role::Source:  return {s,view(fgIn.sourceView),VK_IMAGE_LAYOUT_GENERAL};
        case fgg::Role::Prev:    return {s,fgPrev.view,VK_IMAGE_LAYOUT_GENERAL};
        case fgg::Role::Depth:   return {s,view(fgIn.depthView),VK_IMAGE_LAYOUT_GENERAL};
        case fgg::Role::LowCur:  return {s,fgLow[fgLowAt].img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case fgg::Role::LowPrev: return {s,fgLow[fgLowAt^1u].img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case fgg::Role::Gain:    return {s,fgGain.img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case fgg::Role::Blocks0: return {s,fgBlocks[0].img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case fgg::Role::GainCols:return {s,fgGainCols.img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; // framegen-cost
        case fgg::Role::Terms:   return {s,fgTerms.img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};    // framegen-cost-b
        default:                 return {s,fgBlocks[1].img.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
    }
    // A pass's set: its four bindings by fg_gen.h's table (the LOW pass reads its picture through the LINEAR sampler).
    void fgGenWrite(VkDescriptorSet set,recomp::fgg::Pass pass) noexcept {
        std::array<VkDescriptorImageInfo,4> info{};std::array<VkWriteDescriptorSet,4> w{};
        for(unsigned k=0;k<4;++k) {
            info[k]=fgGenInfo(recomp::fgg::Bindings[pass][k],pass==recomp::fgg::PassLow && k==0);
            w[k].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w[k].dstSet=set;w[k].dstBinding=k;w[k].descriptorCount=1;
            w[k].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[k].pImageInfo=&info[k];
        }
        vkUpdateDescriptorSets(dev->device,4,w.data(),0,nullptr);
    }
    // One pass: the whole target from one triangle.
    void fgGenDraw(VkCommandBuffer cb,unsigned shader,VkFramebuffer framebuffer,std::uint32_t w,std::uint32_t h,VkDescriptorSet set) noexcept {
        VkRenderPassBeginInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;rp.renderPass=shader==FgShaders-1?fgPassOut:shader==FgTerms?fgPassTerms:fgPassRead;rp.framebuffer=framebuffer; // (framegen-cost-b: the terms' render pass)
        rp.renderArea={{0,0},{w,h}};
        vkCmdBeginRenderPass(cb,&rp,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,shader<FgShaders?fgGenPipeline[shader]:shader==FgTerms?fgTermsPipeline:fgGainColsPipeline); // (framegen-cost: the sixth pipeline; framegen-cost-b: the seventh)
        const VkViewport vp{0,0,float(w),float(h),0,1};const VkRect2D sc{{0,0},{w,h}};
        vkCmdSetViewport(cb,0,1,&vp);vkCmdSetScissor(cb,0,1,&sc);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,fgGenLayout,0,1,&set,0,nullptr);
        vkCmdDraw(cb,3,1,0,0);
        vkCmdEndRenderPass(cb);
    }
    // fgBefore, in the `usable` argument of the pair plan: may a pair be planned for this frame? Always, without the
    // generator's switch. With it: the slot keeper's word (its cool-down counted, its verdict taken and logged, here) AND
    // this frame's inputs: what the pair would be is decided here, before the plan (see the top: check c2).
    [[gnu::noinline]] bool fgGenAllows(std::uint32_t tag) noexcept {
        namespace fgg=recomp::fgg;
        // (fgPlan and fgGenAct are still the previous real frame's here: was it shown with a made picture in front of it?)
        const bool madeBefore=fgGenAct==fgg::Act::Generate && fgPlan.mode==recomp::fg::Between;
        fgGenAct=fgg::Act::Dry;
        if(!(fg.switches()&fgg::SwGenBetween)){if(fgGenWasOn)fgGenOff();return true;}
        fgGenWasOn=true;++fgGenFrames;
        if(!fgGenAnnounced) { // (the format ends with the capability marker the app's Settings screen scans the library for)
            fgGenAnnounced=true;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN generator=1 order=between needs=diag-pace-30 fail=%.1f late_max=%u cap=" RECOMP_FGG_CAPABILITY,
                double(fgTuneFail),fgTuneLate);
        }
        fgKeeper.lateMax(fgTuneLate);
        if(fgKeeper.frame())RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN fallback=ended retry=%u (pairs may be planned again)",fgKeeper.trips());
        // this frame's inputs (the size of the presented pictures is the run's: known without stage 1a's objects; the
        // generator's own objects are assumed until fgGenBefore has tried to make them)
        const rsb::Size fs=rsb::gpuSource();
        const bool sourceOk=fgg::sizeOk(fs.w,fs.h) && (!fgSet || (fs.w==fgW && fs.h==fgH)); // (ios-fg-native, Claude 2026-10-19: any size the block grid divides, up to 1280x720; the objects' size once made)
        fgg::Kept kept=fgKeptFrame;kept.valid=kept.valid && fgPrevValid && fgLowValid;
        fgDecision=fgg::decide(fg.switches(),&fgIn,tag,kept,!fgGenFailed,sourceOk,fs.w,fs.h);
        const bool usable=fgDecision.act!=fgg::Act::NoPair;
        fgKeeper.inputs(usable,madeBefore);
        fgg::SlotKeeper::Window seen;
        if(const auto trip=fgKeeper.verdict(&seen);trip!=fgg::SlotKeeper::Trip::None) {
            ++fgGenFallbacks;fgKeptFrame.valid=false;fgLowValid=false;
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN fallback=real-only why=%s made=%u shown=%u late=%u late_pct=%u late_max=%u lost=%u interrupted=%u gpu_us=%.0f call_us=%.0f trips=%u "
                "retry_in=%u final=%u (real frames are shown alone, as with the 30 fps cap)",fgg::SlotKeeper::text(trip),seen.made,seen.shown,seen.late,seen.latePct(),
                fgKeeper.lateMax(),seen.lost,seen.interrupted,seen.gpuMean(),seen.callMean(),fgKeeper.trips(),fgKeeper.cooldownLeft(),unsigned(fgKeeper.final()));
        }
        if(fgKeeper.blindWindows() && !fgGenBlindSaid) { // (once per switch-on: without display times the keeper cannot see late pictures)
            fgGenBlindSaid=true;const auto& bw=fgKeeper.blindWindow();
            RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN keeper=blind made=%u shown=%u (a full window brought fewer than half of its display times: lateness is NOT judged; "
                "lost pictures, GPU time and call time still are)",bw.made,bw.shown);
        }
        if(!fgKeeper.allows())return false;
        if(usable){fgGenAct=fgDecision.act;return true;}
        // no usable inputs: the real frame alone, by the cap's own path (no pair is planned for it)
        fgGenAct=fgg::Act::NoPair;++fgGenWhy[unsigned(fgDecision.why)];fgKeptFrame.valid=false;
        if(fgDecision.why==fgg::Why::Source || fgDecision.why==fgg::Why::Objects)fgGenNever(fs.w,fs.h);
        return false;
    }
    // Nothing a later frame can change (pictures of another size, objects that could not be made): the cap alone from now on.
    [[gnu::noinline]] void fgGenNever(unsigned w,unsigned h) noexcept {
        fgKeeper.disable();
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN fallback=real-only why=%s pictures=%ux%u final=1 (real frames are shown alone, as with the 30 fps cap)",
            recomp::fgg::text(fgDecision.why),w,h);
    }
    // The switch went away (or was never there): nothing stays planned or kept; the objects stay for the next time.
    [[gnu::noinline]] void fgGenOff() noexcept {
        if(!fgGenWasOn)return;
        fgGenLog(); // (before the keeper forgets: the line holds the last window and the blind windows of this switch-on)
        fgGenWasOn=false;fgGenAct=recomp::fgg::Act::Dry;fgKeptFrame.valid=false;fgLowValid=false;fgKeeper.reset();fgIn.tag=0;fgGenBlindSaid=false;
    }
    // fgBefore, when a pair is planned and stage 1a's objects exist: the generator's own objects (made here, once), and what
    // fgGenAllows() decided for this frame. False: no pair for it after all (the real frame alone): the objects could not be
    // made (once per run), or - never, as fgBefore is written - a frame was planned that fgGenAllows() had refused.
    [[gnu::noinline]] bool fgGenBefore(std::uint32_t tag) noexcept {
        namespace fgg=recomp::fgg;
        (void)tag;
        if(!(fg.switches()&fgg::SwGenBetween))return true;
        if(fgGenAct!=fgg::Act::NoPair && !fgGenEnsure()) {
            fgDecision.act=fgGenAct=fgg::Act::NoPair;fgDecision.why=fgg::Why::Objects;++fgGenWhy[unsigned(fgg::Why::Objects)];
            fgGenNever(unsigned(fgW),unsigned(fgH));
        }
        if(fgGenAct==fgg::Act::NoPair){fgKeptFrame.valid=false;return false;}
        if(fgGenAct!=fgg::Act::Generate)++fgGenWhy[unsigned(fgDecision.why)];
        if(fgGenAct==fgg::Act::Keep)fgPrevValid=false; // (stage 1a's own path for a first frame: [real, the same picture again], kept)
        fgAuditThis=fgGenAct==fgg::Act::Generate && fgTuneAudit!=fgAuditSeen;
        return true;
    }
    // fgRecord: the made picture into fgOut. True: the five passes were recorded. False: stage 1a's copy is recorded instead
    // (no generator switch; or this frame only keeps its picture - then its LOW pass was recorded here for the next pair).
    [[gnu::noinline]] bool fgGenRecord(VkCommandBuffer cb,bool between) noexcept {
        namespace fgg=recomp::fgg;
        if(fgGenAct==fgg::Act::Dry)return false;
        unsigned i=0;
        while(i<commands.size() && commands[i]!=cb)++i;
        if(i>=commands.size() || i>=FgMaxImages || fgGenFailed){fgKeptFrame.valid=false;fgLowValid=false;return false;}
        const bool make=fgGenAct==fgg::Act::Generate && between && fgLowValid;
        auto& sets=fgGenSet[i];
        // (the previous pass that used image i's sets has completed: presentLocked waited for it before recording)
        fgGenWrite(sets[0],fgg::PassLow);
        if(make){fgGenWrite(sets[1],fgg::PassGainCols);fgGenWrite(sets[2],fgg::PassTerms);fgGenWrite(sets[3],fgg::PassSynth);} // (framegen-cost: sets[1] is the GAIN COLUMNS pass's: the pictures GAIN read until v467; framegen-cost-b: sets[2] is the TERMS pass's: the pictures CLASSIFY read until v468)
        if(fgGenTime && fgGenTimedSlot<MaxTimed) { // the previous made picture's GPU time, if its pass has finished (never waited for)
            std::uint64_t t[2]{};
            if(vkGetQueryPoolResults(dev->device,fgGenTime,2*fgGenTimedSlot,2,sizeof t,t,sizeof t[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
                const double ns=double(((t[1]&tsMask)-(t[0]&tsMask))&tsMask)*tsNs;
                fgGenTimeNs+=std::uint64_t(ns);++fgGenTimeN;fgKeeper.gpu(ns/1e3);
                fgGenPassRead(t,ns/1e3); // framegen-cost: that picture's times by pass, or its total among the plain ones
            } else {++fgGenTimeMissed;if(fgGenSplitSlot)fgPass.notReady();} // (framegen-cost: a split picture that was not ready is counted, never read later or guessed)
            fgGenTimedSlot=~0u;fgGenSplitSlot=false;
        }
        // everything recorded or submitted before (the frame's own picture and depth, the last pair's copies) comes first
        VkMemoryBarrier mb{};mb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;mb.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);
        const bool timed=make && fgGenTime && nextAcquire<MaxTimed;
        if(timed){vkCmdResetQueryPool(cb,fgGenTime,2*nextAcquire,2);vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,fgGenTime,2*nextAcquire);}
        // framegen-cost: one timed made picture in fgg::PassEvery carries a stamp behind each step (fg_gen.h: what each difference holds)
        const bool split=timed && fgGenPassTime && fgg::splitDue(fgGenTimedCount);
        if(timed)++fgGenTimedCount;
        std::uint32_t stamp=fgg::PassStamps*nextAcquire;
        const auto mark=[&]{if(split)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,fgGenPassTime,stamp++);};
        if(split){vkCmdResetQueryPool(cb,fgGenPassTime,stamp,fgg::PassStamps);mark();} // (behind v467's first stamp: everything before has finished)
        const fgg::Push push=fgg::push(make?fgDecision.m:std::array<float,16>{},fgPlan.phase,fgTuneFail,(fg.switches()&fgg::SwShow)!=0,fgW,fgH);
        vkCmdPushConstants(cb,fgGenLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
        fgGenDraw(cb,0,fgLow[fgLowAt].framebuffer,fgW,fgH,sets[0]);
        mark(); // framegen-cost: behind LOW
        if(make) {
            fgGenDraw(cb,FgGainCols,fgGainCols.framebuffer,(fgW/fgg::Block),fgg::GainColsRows,sets[1]); // framegen-cost: the column sums of the two low-passed pictures
            mark();
            fgGenDraw(cb,1,fgGain.framebuffer,1,1,fgGainSet); // (framegen-cost: from the column sums; until v467 from the two pictures, with sets[1])
            mark();
            fgGenDraw(cb,FgTerms,fgTerms.framebuffer,fgW,fgH,sets[2]); // framegen-cost-b: per pixel what a turn of v468's CLASSIFY loop computed
            mark();
            fgGenDraw(cb,2,fgBlocks[0].framebuffer,(fgW/fgg::Block),(fgH/fgg::Block),fgTermsSet); // (framegen-cost-b: from the terms; until v468 from the two low-passed pictures, the depth and the gain, with sets[2])
            mark();
            for(const fgg::Pass round:fgg::ResolveRounds)
                fgGenDraw(cb,3,fgBlocks[round==fgg::PassResolve01?1:0].framebuffer,(fgW/fgg::Block),(fgH/fgg::Block),fgResolveSet[round==fgg::PassResolve01?0:1]);
            mark(); // (behind the third round; SYNTHESIS ends at v467's second stamp)
            fgGenDraw(cb,4,fgOutFramebuffer,fgW,fgH,sets[3]);
            if(timed){vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,fgGenTime,2*nextAcquire+1);fgGenTimedSlot=nextAcquire;fgGenSplitSlot=split;}
            if(fgAuditThis && !(fgAuditThis=fgGenAuditCopy(cb)))fgAuditSeen=fgTuneAudit; // (no buffer: this request is given up, not tried every frame)
            ++fgGenMadeN;
        } else {++fgGenKeepN;fgAuditThis=false;}
        // this frame is the next pair's previous one: its low-passed picture (just drawn), its camera; fgRecord copies its picture
        fgKeptFrame.valid=true;fgKeptFrame.cam=fgIn.cam;fgLowValid=true;fgLowAt^=1u;fgGenImageIndex=i;
        return make;
    }
    // fgPresentMade, after the made picture's call: the slot keeper's account of it, and a requested audit.
    [[gnu::noinline]] void fgGenMade(bool ok,std::uint64_t callNs) noexcept {
        namespace fgg=recomp::fgg;
        if(fgGenAct!=fgg::Act::Generate){if(fgGenAct==fgg::Act::Keep && !ok){fgKeptFrame.valid=false;fgLowValid=false;}return;}
        if(!ok){fgKeeper.lost();fgKeptFrame.valid=false;fgLowValid=false;fgAuditThis=false;return;}
        fgGenCallNs+=callNs;fgKeeper.call(double(callNs)/1e3);
        recomp::fg::Slot s{};
        if(fgPlan.timed)if(const auto f=recomp::fg::slotSource.load(std::memory_order_relaxed))f(currentTag,&s);
        fgKeeper.planned(currentTag,s.target,s.period);
        if(fgAuditThis){fgAuditThis=false;fgAuditSeen=fgTuneAudit;fgGenAuditRead();}
    }
    // fgAfter: the pair's REAL frame was not presented after its made picture took the slot (check c2: it counts as a lost
    // picture; without the generator's switch a compare and a return).
    [[gnu::noinline]] void fgGenRealLost() noexcept {if(fgGenAct==recomp::fgg::Act::Generate)fgKeeper.realLost();}
    // diag-fg-audit: the resolved block map and the gain texel of this made frame into a host buffer (57.6 KB + 4 bytes).
    bool fgGenAuditCopy(VkCommandBuffer cb) noexcept {
        namespace fgg=recomp::fgg;
        const VkDeviceSize mapBytes=VkDeviceSize((fgW/fgg::Block))*(fgH/fgg::Block)*4;
        if(!fgAuditBuffer.buffer && !makeBuffer(mapBytes+4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,fgAuditBuffer,true)){freeBuffer(fgAuditBuffer);return false;}
        std::array<VkImageMemoryBarrier,2> b{};
        for(unsigned k=0;k<2;++k){b[k].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b[k].image=k?fgGain.img.image:fgBlocks[1].img.image;
            b[k].oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b[k].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[k].srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b[k].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b[k].srcQueueFamilyIndex=b[k].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b[k].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
        // (after the synthesis pass, which read both; the next frame's passes start from UNDEFINED, so nothing is put back)
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b.data());
        VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={(fgW/fgg::Block),(fgH/fgg::Block),1};
        vkCmdCopyImageToBuffer(cb,fgBlocks[1].img.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fgAuditBuffer.buffer,1,&c);
        c.bufferOffset=mapBytes;c.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(cb,fgGain.img.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fgAuditBuffer.buffer,1,&c);
        VkMemoryBarrier h{};h.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;h.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;h.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&h,0,nullptr,0,nullptr);
        return true;
    }
    // The audited frame's pass is waited for (this is the one place the generator waits for the GPU: only on request, once
    // per request) and its block classes are counted on the CPU.
    void fgGenAuditRead() noexcept {
        namespace fgg=recomp::fgg;
        const unsigned i=fgGenImageIndex;
        if(i>=imageSlot.size() || imageSlot[i]<0 || !fgAuditBuffer.mapped || !waitAcquireSlot(unsigned(imageSlot[i])))return;
        const auto* p=static_cast<const std::uint8_t*>(fgAuditBuffer.mapped);
        const fgg::Shares s=fgg::shares(p,(fgW/fgg::Block)*(fgH/fgg::Block));const std::uint8_t* g=p+std::size_t((fgW/fgg::Block))*(fgH/fgg::Block)*4;
        ++fgGenAudits;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN audit=%llu blocks=%u camera_pct=%.1f zero_pct=%.1f none_pct=%.1f other=%u open=%u mean_cost=%.2f gain=%.4f phase=%.3f "
            "step_m=%.3f step_deg=%.3f fail=%.1f",(unsigned long long)fgGenAudits,s.blocks,s.pct(s.camera),s.pct(s.zero),s.pct(s.none),s.other,s.open,s.meanCost(),
            double(fgg::gainOf(g[0],g[1])),double(fgPlan.phase),fgDecision.step.metres,fgDecision.step.degrees,double(fgTuneFail));
    }
    // fgLog (every 256 real frames while a pair switch is on), and when the generator's switch goes away.
    [[gnu::noinline]] void fgGenLog() noexcept {
        namespace fgg=recomp::fgg;
        if(!fgGenFrames)return;
        const auto why=[&](fgg::Why w){return (unsigned long long)fgGenWhy[unsigned(w)];};
        const auto& kw=fgKeeper.window();
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN frames=%llu made=%llu kept_only=%llu nogen=no-frame:%llu,no-camera:%llu,orthonormal:%llu,lens:%llu,viewproj:%llu,no-depth:%llu,"
            "depth-size:%llu,source:%llu,objects:%llu,cut:%llu,no-previous:%llu,tonemap-draws:%llu fg_us=%.0f fg_n=%llu fg_unread=%llu call_ms=%.3f state=%s trips=%u retry_in=%u fallbacks=%llu "
            "window=made:%u,shown:%u,late:%u,lost:%u,interrupted:%u late_max=%u fail=%.1f audits=%llu failed=%u blind=%u",(unsigned long long)fgGenFrames,(unsigned long long)fgGenMadeN,
            (unsigned long long)fgGenKeepN,why(fgg::Why::NoFrame),why(fgg::Why::NoCamera),why(fgg::Why::Orthonormal),why(fgg::Why::Lens),why(fgg::Why::ViewProj),
            why(fgg::Why::NoDepth),why(fgg::Why::DepthSize),why(fgg::Why::Source),why(fgg::Why::Objects),why(fgg::Why::Cut),why(fgg::Why::NoPrevious),why(fgg::Why::TonemapDraws),
            fgGenTimeN?double(fgGenTimeNs)/1e3/double(fgGenTimeN):0.0,(unsigned long long)fgGenTimeN,(unsigned long long)fgGenTimeMissed,
            fgGenMadeN?double(fgGenCallNs)/1e6/double(fgGenMadeN):0.0,fgKeeper.final()?"off":fgKeeper.allows()?"run":"cool",fgKeeper.trips(),fgKeeper.cooldownLeft(),
            (unsigned long long)fgGenFallbacks,kw.made,kw.shown,kw.late,kw.lost,kw.interrupted,fgKeeper.lateMax(),double(fgTuneFail),(unsigned long long)fgGenAudits,unsigned(fgGenFailed),
            fgKeeper.blindWindows());
        fgGenFrames=fgGenMadeN=fgGenKeepN=fgGenTimeNs=fgGenTimeN=fgGenTimeMissed=fgGenCallNs=0;fgGenWhy.fill(0);
        fgGenPassLog(); // framegen-cost
    }
    // ---- framegen-cost (2026-10-06): the made pictures' GPU time by pass (fg_gen.h PassTimes says what each number holds).
    // fgGenRecord, when v467's two stamps of the previous made picture were read (t; totalUs = their difference): a split
    // picture's stamps (fg_gen.h PassStamps: seven since framegen-cost-b) are read with them, in one call that gives all or none.
    void fgGenPassRead(const std::uint64_t* t,double totalUs) noexcept {
        namespace fgg=recomp::fgg;
        if(!fgGenSplitSlot){fgPass.plain(totalUs);return;}
        std::uint64_t mid[fgg::PassStamps]{};
        if(vkGetQueryPoolResults(dev->device,fgGenPassTime,fgg::PassStamps*fgGenTimedSlot,fgg::PassStamps,sizeof mid,mid,sizeof mid[0],VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)
            fgPass.split(t[0],mid,t[1],tsMask,tsNs);
        else fgPass.notReady();
    }
    // Behind the counters line, when a made picture was timed since the last one. Microseconds, means over pass_n split
    // pictures: before = v467's first stamp -> everything submitted earlier has finished; low, gain_cols, gain_out,
    // terms, classify (framegen-cost-b: the two steps of what v468 printed as classify),
    // resolve (three rounds), synth = from the end of the step before to the end of this step; pass_sum = their
    // sum = those pictures' mean fg_us. pass_unread: split pictures whose stamps were not ready (left out); pass_odd: read
    // but refused (a part above one second). plain_us / plain_n: the pictures stamped as in v467 (the control for what the
    // extra stamps cost).
    [[gnu::noinline]] void fgGenPassLog() noexcept {
        namespace fgg=recomp::fgg;
        if(!fgPass.any())return;
        RECOMP_DIRECT_LOG("RENDER_DIRECT_FG_GEN pass_us=before:%.0f,low:%.0f,gain_cols:%.0f,gain_out:%.0f,terms:%.0f,classify:%.0f,resolve:%.0f,synth:%.0f pass_sum=%.0f pass_n=%llu pass_unread=%llu "
            "pass_odd=%llu plain_us=%.0f plain_n=%llu every=%u",fgPass.mean(fgg::PartBefore),fgPass.mean(fgg::PartLow),fgPass.mean(fgg::PartGainCols),fgPass.mean(fgg::PartGain),
            fgPass.mean(fgg::PartTerms),fgPass.mean(fgg::PartClassify),fgPass.mean(fgg::PartResolve),fgPass.mean(fgg::PartSynth),fgPass.sum(),(unsigned long long)fgPass.n,(unsigned long long)fgPass.unread,
            (unsigned long long)fgPass.odd,fgPass.plainMean(),(unsigned long long)fgPass.plainN,fgg::PassEvery);
        fgPass=fgg::PassTimes{};
    }
    // (framegen-cost: the lane's members, behind everything above, so that every member of v467 keeps its offset)
    FgTarget fgGainCols;VkDescriptorSet fgGainSet=VK_NULL_HANDLE;VkQueryPool fgGenPassTime=VK_NULL_HANDLE;
    VkShaderModule fgGainColsModule=VK_NULL_HANDLE;VkPipeline fgGainColsPipeline=VK_NULL_HANDLE;
    recomp::fgg::PassTimes fgPass;std::uint64_t fgGenTimedCount=0;bool fgGenSplitSlot=false;
    // (framegen-cost-b: this lane's members, behind everything above, so that every member of v468 keeps its offset)
    FgTarget fgTerms;VkDescriptorSet fgTermsSet=VK_NULL_HANDLE;VkRenderPass fgPassTerms=VK_NULL_HANDLE;
    VkShaderModule fgTermsModule=VK_NULL_HANDLE;VkPipeline fgTermsPipeline=VK_NULL_HANDLE;
