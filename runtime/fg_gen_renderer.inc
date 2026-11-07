    // ---- framegen-1b (frame generation, stage 1b: the generator; fg_gen.h): the renderer's part. Included INSIDE class
    // Renderer as its last members (after fg_burst_renderer.inc), so the members above keep their offsets. Our own code.
    // What the renderer gives the presenter's generator for every real frame while files/private-trace/diag-fg-between exists:
    // the present image's view, the frame's camera (the tonemap draw's pixel c8-c15 and c130) and the view of the frame's
    // scene depth (a depth target of pitch 1280 that this frame drew into: the main depth target, FEASIBILITY.md 1.2; the
    // rule is at fgGenPresent). Nothing is recorded into the frame's commands, no game object is changed, no view is created.
    // The hooks ride on lane fg-burst's (fg_burst_renderer.inc: three one-line additions there), so that draw() and present()
    // are not touched at all:
    //   fgGenWindow()   from fgbWindow(), the 60-present switch poll: the switch files, the tuning files, the presenter's word
    //   fgGenTonemap()  from fgbTonemap(), i.e. inside the block of the renderer's own RENDER_POSTFX log line. As the capture
    //                   does while armed, the generator makes that block run for the tonemap draw of EVERY frame by rewinding
    //                   postfxLogged[0] at every present: while diag-fg-between exists the renderer logs one
    //                   "RENDER_POSTFX name=tonemap" line per frame (about 390 characters). That is the price of not adding an
    //                   instruction to draw(); the one piece of renderer state written is postfxLogged[0]
    //   fgGenPresent()  from fgbPresent(), after the blit into the present image: this frame's inputs to the presenter
    // Switch files (files/private-trace/, polled at the 60-present window):
    //   diag-fg-between    the generator, in-between order (what the app's Settings row writes under the 30 fps cap)
    //   diag-fg-show       refused blocks red, blocks that did not move blue
    //   diag-fg-audit      one made frame per window is audited (its block classes counted: RENDER_DIRECT_FG_GEN audit=)
    //   diag-fg-fail       text: the refusal threshold, 4 .. 32 (default 10)
    //   diag-fg-late-max   text: the slot keeper's limit in percent, 5 .. 95 (default 40)
    bool fgGenOn=false;                 // the cached flag every hook tests first
    bool fgGenSeen=false;               // diag-fg-between was seen in this run (the window line is logged from then on; check c2)
    unsigned fgGenFile=0,fgGenAuditToken=0,fgGenDepthMoves=0;
    recomp::fgg::Camera fgGenCamera;std::uint64_t fgGenCameraInterval=0,fgGenCameraFrame=0;unsigned fgGenCameraDraws=0;
    std::uint64_t fgGenTonemapBase=0;   // postfxDraws[0] (the renderer's count of tonemap draws) at the previous present
    std::array<std::uint32_t,48> fgGenVariantFrames{}; // fixer2: frames whose camera came from tonemap postfx#(20 + k), since the last window line
    std::uint64_t fgGenDepthKey=~0ull;  // the depth target that has the role of the scene depth (fgGenDepthOf; ~0: none yet)
    std::uint64_t fgGenDepthClock=0;    // the renderer's write clock at the previous present with the generator on
    // ios-fg-scaled (Claude 2026-10-17, owner: "make the generator accept scaled pictures"): with a render scale and stage A
    // (diag-rs-stage-a: the renderer hands the presenter a 1280x720 present image), the scene depth is still at render size.
    // The generator's shaders read depth with texelFetch at the PICTURE's pixel coordinates, so it gets a 1280x720 copy:
    // the picture's region of the scaled depth, blitted with NEAREST (depth aspect). Refused (no depth) when the device cannot
    // blit the format. Never used at scale 1.
    Image fgGenDepthUp{};int fgGenDepthUpOk=-1; // (-1: not yet decided; 0: refused; 1: usable)
    std::uint64_t fgGenDepthUpBlits=0;

    static bool fgGenHas(const char* name) noexcept {
        char path[160];std::snprintf(path,sizeof path,RECOMP_APP_PATH("/private-trace/%s"),name);
        return access(path,F_OK)==0;
    }
    // A tuning file's text (null when absent or unreadable).
    static const char* fgGenText(const char* name,char* out,std::size_t cap) noexcept {
#if defined(__APPLE__)
        // (Claude 2026-10-20, iOS test runs) RECOMP_IOS_FG_TEXT="name=value;name=value": the text of these tuning files
        // (diag-fg-fail, diag-fg-late-max), which the iOS shell cannot write; the file is read otherwise
        if(const char* e=std::getenv("RECOMP_IOS_FG_TEXT")) {
            const std::size_t nl=std::strlen(name);
            for(const char* p=e;*p;) {
                const char* end=std::strchr(p,';');const std::size_t len=end?std::size_t(end-p):std::strlen(p);
                if(len>nl+1 && !std::strncmp(p,name,nl) && p[nl]=='='){const std::size_t v=std::min(len-nl-1,cap-1);std::memcpy(out,p+nl+1,v);out[v]=0;return out;}
                if(!end)break;p=end+1;
            }
        }
#endif
        char path[160];std::snprintf(path,sizeof path,RECOMP_APP_PATH("/private-trace/%s"),name);
        FILE* f=std::fopen(path,"r");
        if(!f)return nullptr;
        const std::size_t n=std::fread(out,1,cap-1,f);std::fclose(f);out[n]=0;
        return out;
    }
    void fgGenEveryFrame() noexcept {postfxLogged[0]=std::uint64_t{0}-300;} // (frame >= postfxLogged[0] + 300 holds for any frame: the tonemap block runs)
    // ---- hook: the 60-present switch poll (after fgWindow(), which has just written stage 1a's bits of the presenter's word)
    [[gnu::noinline]] void fgGenWindow() noexcept {
        namespace fgg=recomp::fgg;
        // (a path that ends with the switch's name: the app's Settings screen looks for exactly this string in the library)
        bool between=access(RECOMP_APP_PATH("/private-trace/diag-fg-between"),F_OK)==0;
#if defined(__APPLE__)
        between=between || recomp::apple::launcher::bridge::frameGeneration(); // (Claude, 2026-10-18: the launcher's Frame Generation)
#endif
        const bool show=between && fgGenHas("diag-fg-show");
        const unsigned window=fgGenFile;
        fgGenFile=(between?fgg::SwGenBetween:0u)|(show?fgg::SwShow:0u);
        auto& presenter=recomp::gpu::direct::presenter();
        // (absent now and at the last poll: the presenter's word is stage 1a's own, untouched)
        if(fgGenFile || window)presenter.fgSwitches(fgg::switchWord(presenter.fgSwitchWord(),between,show));
        if(between) {
            char text[32];
            const float fail=fgg::tuned(fgGenText("diag-fg-fail",text,sizeof text),fgg::K::FailLo,fgg::K::FailHi,fgg::K::Fail);
            const float late=fgg::tuned(fgGenText("diag-fg-late-max",text,sizeof text),float(fgg::K::LateLo),float(fgg::K::LateHi),float(fgg::K::LateMaxPct));
            if(fgGenHas("diag-fg-audit"))++fgGenAuditToken;
            presenter.fgGenTune(fail,unsigned(late),fgGenAuditToken);
            if(!fgGenOn){fgGenEveryFrame();fgGenDepthKey=~0ull;fgGenDepthClock=writeClock;fgGenDepthMoves=0;fgGenTonemapBase=postfxDraws[0];}
            fgGenSeen=true;
        }
        fgGenOn=between;
        // (check c2: no line while the file has never been there, as lane fg-burst's own poll: a run without the switch logs
        // what v465 logs. From the first poll that saw it: one line per window, also after it is removed.)
        if(!fgGenSeen)return;
        const unsigned w=window&fgGenFile;
        GLOG("RENDER_GENERIC_V210Z fg_between=%u fg_show=%u",unsigned((w&fgg::SwGenBetween)!=0),unsigned((w&fgg::SwShow)!=0));
        { // fixer2: which tonemap variants gave this window's cameras (only when another than postfx#20p did; "one" = diag-fg-tonemap-one)
            std::string v;bool other=false;
            for(unsigned k=0;k<fgGenVariantFrames.size();++k)if(fgGenVariantFrames[k]){char b[24];std::snprintf(b,sizeof b,"%s%u:%u",v.empty()?"":",",20+k,fgGenVariantFrames[k]);v+=b;other=other || k;}
            if(other)GLOG("RENDER_DIAG_FG_TONEMAP variants=%s%s",v.c_str(),post::tonemapOneOnly.load(std::memory_order_relaxed)?" one":"");
            fgGenVariantFrames={};
        }
    }
    static std::uint64_t fgGenDepthOf(const Target& t) noexcept {
        return (std::uint64_t(t.base)<<40)|(std::uint64_t(t.xenosFormat)<<32)|(std::uint64_t(t.image.width)<<16)|t.image.height;
    }
    // ---- hook: draw(), where the renderer logs RENDER_POSTFX for a postfx shader's draw: the tonemap draw's camera constants
    // (the frame's FIRST tonemap draw; how many the frame had is the renderer's own count, taken at the present).
    [[gnu::noinline]] void fgGenTonemap(std::uint64_t psHash,const DrawInput& in) noexcept {
        // fixer2 (2026-10-22): any member of the tonemap family (post_fidelity.h TonemapVariants), not postfx#20p alone
        if(!fgGenOn || post::postfxShader(psHash)!=&post::PostfxShaders[0] || !in.pixelConstants)return;
        if(fgGenCameraInterval!=presentCount){fgGenCameraInterval=presentCount;fgGenCameraDraws=0;}
        if(!fgGenCameraDraws++) {
            recomp::fgg::takeCamera(fgGenCamera,*in.pixelConstants);fgGenCameraFrame=frame;
            const unsigned v=post::tonemapVariant(psHash);if(v>=20 && v<20+fgGenVariantFrames.size())++fgGenVariantFrames[v-20]; // (the window line)
        }
    }
    // ios-fg-scaled: the picture's region of a scaled depth target into fgGenDepthUp (1280x720, the target's format), NEAREST.
    [[gnu::noinline]] bool fgGenDepthUpscale(const Target& t) noexcept {
        namespace fgg=recomp::fgg;
        if(fgGenDepthUpOk==0)return false;
        if(fgGenDepthUpOk<0) {
            VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(d.physical,t.image.format,&p);
            const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            const bool can=(p.optimalTilingFeatures&need)==need;
            const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
            const bool made=can && makeImage(fgg::Width,fgg::Height,1,t.image.format,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                              (t.image.format==VK_FORMAT_D32_SFLOAT_S8_UINT || t.image.format==VK_FORMAT_D24_UNORM_S8_UINT)?
                                              VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT),
                                              false,identity,fgGenDepthUp); // (the view: depth only - makeImage's rule)
            fgGenDepthUpOk=made?1:0;
            GLOG("RENDER_IOS_FG_SCALED depth_up=%u blit_features=%u format=%d k=%d",unsigned(made),unsigned(can),int(t.image.format),int(t.image.k));
            if(!made)return false;
            endPass(__LINE__);toGeneral(fgGenDepthUp);
        }
        endPass(__LINE__);barrier();
        VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};b.dstSubresource=b.srcSubresource;
        b.srcOffsets[1]={std::int32_t(rscale::extent(fgg::Width,t.image.k)),std::int32_t(rscale::extent(fgg::Height,t.image.k)),1};
        b.dstOffsets[1]={std::int32_t(fgg::Width),std::int32_t(fgg::Height),1};
        vkCmdBlitImage(cmd,t.image.image,VK_IMAGE_LAYOUT_GENERAL,fgGenDepthUp.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_NEAREST);
        barrier();++fgGenDepthUpBlits;
        return true;
    }
    // ---- hook: present(), after the blit into the present image: this frame's inputs for the presenter's generator.
    [[gnu::noinline]] void fgGenPresent(const Image& presentImage) noexcept {
        namespace fgg=recomp::fgg;
        fgg::FrameIn in;in.tag=presentTag;in.frame=fgGenCameraFrame;
        // the picture: the game's 1280x720 UNORM present image (not the sRGB image of diag-gamma-on, the mosaic, a census frame)
        // ios-fg-native (Claude 2026-10-19): under stage B the presenter receives the picture at the render size (rsbW x rsbH); the
        // generator then works at that size (when the block grid divides it), with the depth target of the same scale as it is
        const bool native=rsbW && rsK!=rscale::One && fgg::sizeOk(rsbW,rsbH);
        // (under stage B the present image is made at the render size itself: rsbPresentW/H, host texels, k = 16)
        const bool picture=presentImage.view && presentImage.width==(native?rsbW:fgg::Width) && presentImage.height==(native?rsbH:fgg::Height) &&
            presentImage.format==VK_FORMAT_R8G8B8A8_UNORM && !v369Mosaic && !census.active && (rsK==rscale::One || !rsbW || native); // (ios-fg-scaled: stage A's 1280x720 present image)
        if(picture)in.sourceView=reinterpret_cast<std::uint64_t>(presentImage.view);
        // the camera: taken while this frame was rendered (presentCount was advanced at the top of this present), and only
        // from a frame with exactly one tonemap draw (the renderer's own count; the hook sees a frame's first draw only)
        in.tonemapDraws=std::uint32_t(postfxDraws[0]-fgGenTonemapBase);fgGenTonemapBase=postfxDraws[0];
        if(!in.tonemapDraws)++iosNoTonemapPs[iosLastFullPs]; // (ios-notonemap diagnosis: which full-picture shader ended such a frame)
        iosLastFullPs=0;
        const bool taken=fgGenCameraDraws && fgGenCameraInterval+1==presentCount;
        if(taken)in.cam=fgGenCamera;
        in.why=fgg::cameraVerdict(in.tonemapDraws,taken,fgGenCamera);
        // the scene depth, read as it is. Nothing here can see what a target HOLDS; what can be checked is checked (check c2):
        //   candidates  depth targets of pitch 1280, not scaled, that THIS frame drew into (a write stamp newer than the
        //               previous present's: a target the frame did not touch holds another frame's depth and is never handed);
        //   the role    the candidate that had it at the previous present keeps it, so a later pass of the frame that draws
        //               into another pitch-1280 depth target (a phone, a 3D menu) does not take it away; otherwise the newest.
        // No candidate: no depth is handed and the frame gets no made picture (nogen=no-depth). Every change of the role is
        // logged with the number of candidates and of changes since the switch came on (DEVICE-PLAN.md G1: one line, 0:17:1280).
        const Target* best=nullptr;const Target* same=nullptr;unsigned candidates=0;
        for(const auto& [key,t]:targets) {
            if(!t.depth || t.pitch!=fgg::Width || !t.image.image || !t.image.view || t.written<=fgGenDepthClock)continue;
            if(native?t.image.k!=rsK:(t.image.k!=rscale::One && (rsbW || t.image.k!=rsK)))continue; // (ios-fg-scaled: a scaled target under stage A; ios-fg-native: the render scale's)
            ++candidates;
            if(fgGenDepthOf(t)==fgGenDepthKey)same=&t;
            if(!best || t.written>best->written)best=&t;
        }
        if(same)best=same;
        fgGenDepthClock=writeClock;
        if(best && !native && best->image.k!=rscale::One && !fgGenDepthUpscale(*best))best=nullptr; // ios-fg-scaled: no usable 1280x720 copy -> no depth
        if(best) {
            const bool up=!native && best->image.k!=rscale::One;
            in.depthView=reinterpret_cast<std::uint64_t>(up?fgGenDepthUp.view:best->image.view);
            in.depthW=native?rsHostW(best->image):best->image.width;in.depthH=native?rscale::extent(best->image.height,best->image.k):best->image.height; // (ios-fg-native: host texels)
            if(const std::uint64_t key=fgGenDepthOf(*best);key!=fgGenDepthKey) { // (once, and whenever another target takes the role)
                if(fgGenDepthKey!=~0ull)++fgGenDepthMoves;
                fgGenDepthKey=key;
                GLOG("RENDER_DIAG_FG_GEN depth=%u:%u:%u size=%ux%u vkformat=%d pending_clears=%zu residue=%u tonemap_draws=%u camera=%s candidates=%u moves=%u",best->base,best->xenosFormat,best->pitch,
                     best->image.width,best->image.height,int(best->image.format),best->pendingClears.size(),unsigned(best->residuePending),in.tonemapDraws,fgg::text(in.why),candidates,fgGenDepthMoves);
            }
        }
        recomp::gpu::direct::presenter().fgGenFrame(in);
        fgGenCameraDraws=0;
        fgGenEveryFrame(); // the next frame's tonemap draw reaches fgGenTonemap()
    }
