#ifdef RECOMP_THOR_EDITION
// ios-duo (2026-10-24): two texts in one file: the Thor Edition's v850 (RECOMP_THOR_EDITION: the Duo) and the regular iOS build's 238e.
// thor-maptap (v658, Thor edition only: included under RECOMP_THOR_EDITION as the LAST members of Renderer, after thor-wheel's): the
// renderer's half of map_tap.h. draw() marks a frame that drew the MAP tab's map window (v659: on every draw, one compare of the
// scissor's x first); present() hands that to the macro and, while an episode runs, keeps a copy of the last frame without the MAP tab
// (the game's own pause blur, or the last game frame) and puts it in place of every MAP-tab frame. One vkCmdCopyImage of the present
// image per frame of an episode (~0.5 s), nothing otherwise. RENDER_MAPTAP lines: one per episode (its times from the request).
Image mapTapHeld;bool mapTapHeldValid=false;bool mapTapFailed=false;
std::uint64_t mapTapCopies=0,mapTapRestores=0;
[[gnu::noinline]] void mapTapPresent(const Image& shown,unsigned pw,unsigned ph) {mapTapPresentInner(shown,pw,ph);mapTapMirror(shown,pw,ph);povPresent(shown,pw,ph);thorFpsDraw(shown,pw,ph);} // (v824: povPresent; v826: thorFpsDraw)
void mapTapPresentInner(const Image& shown,unsigned pw,unsigned ph) {
    namespace mt=recomp::gpu::maptap;
    const unsigned seen=mt::seenDrawn().exchange(0u,std::memory_order_relaxed);
    const bool tab=(seen&mt::SeenTab)!=0;
    const std::int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto f=mt::machine().present(now,seen);
    if(f.ended) {
        const auto& s=f.summary;
        GLOG("RENDER_MAPTAP %s n=%llu mode=%s end=%s start_up_ms=%lld start_frames=%llu tab_ms=%lld release_ms=%lld tab_to_release=%llu last_release_ms=%lld "
             "retries=%u done_ms=%lld tab_frames=%u held=%u early=%u blur_ms=%lld full_seen=%u copies=%llu restores=%llu",mt::Marker,(unsigned long long)s.n,mt::modeName(s.mode),mt::endName(s.end),
             (long long)s.startUpMs,(unsigned long long)s.startPresents,(long long)s.tabMs,(long long)s.firstReleaseMs,(unsigned long long)s.tabToRelease,
             (long long)s.lastReleaseMs,s.retries,(long long)s.doneMs,s.tabFrames,s.held,unsigned(s.early),(long long)s.blurMs,unsigned(s.fullSeen),(unsigned long long)mapTapCopies,(unsigned long long)mapTapRestores);
        if(s.mode==mt::Mode::Waypoint)GLOG("RENDER_MAPTAP waypoint n=%llu panned=%u tap=%d,%d xf=%.6g,%.6g,%.6g,%.6g pan_frames=%u left_px=%.1f end=%s",(unsigned long long)s.n,
            unsigned(s.panned),s.tap[0],s.tap[1],double(s.xf[0]),double(s.xf[1]),double(s.xf[2]),double(s.xf[3]),s.panFrames,double(s.panLeft),mt::endName(s.end));
    }
    // RC1.1: Resume on the game's pause state only (touch_context.h IS_PAUSE_MENU_ACTIVE, or the pause map the map tap opened); Loading
    // for a character switch (v685) and for death / arrest until the respawn has control (loading_layout.h RespawnState); a pause wins.
    const auto* base=recomp::gpu::hud::guestBase().load(std::memory_order_acquire);
    const int screens=base?recomp::touchctx::screens(recomp::touchctx::GuestMemory{base}):0;
    const bool pauseMenu=(screens&recomp::touchctx::Frontend)!=0,pauseMap=mt::machine().mapShown();
    mt::machine().setPauseMenu(pauseMenu); // (v811: a lower-screen tap acts only on the map)
    const auto pick=mapTapLoading(now,mt::pausedFrame(seen),base,(screens&recomp::touchctx::ControlOff)==0,pauseMenu,pauseMap);
    mapTapResume(pick.resume,pick.pill,shown,pw,ph,f.hold); // v661 / v663 / v665: the lower screen's Resume Game (v811: the pill on the map only)
    if(!f.active){mapTapHeldValid=false;return;}
    if(mapTapFailed || !shown.image || census.active)return;
    if(f.hold) {
        if(!mapTapHeldValid)return; // (no frame without the tab yet: this one is shown)
        hazards({mapTapHeld.image},{shown.image});
        VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={pw,ph,1};
        vkCmdCopyImage(cmd,mapTapHeld.image,VK_IMAGE_LAYOUT_GENERAL,shown.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
        afterWrite();++mapTapRestores;
        return;
    }
    if(tab)return; // (a MAP-tab frame that is not hidden: hide=0, or after the episode's end)
    if(!mapTapHeld.image || mapTapHeld.width!=pw || mapTapHeld.height!=ph || mapTapHeld.format!=shown.format) {
        if(mapTapHeld.image)destroyLater(mapTapHeld);
        mapTapHeld={};mapTapHeldValid=false;
        const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
        if(!makeImage(pw,ph,1,shown.format,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,false,identity,mapTapHeld)) {
            mapTapHeld={};mapTapFailed=true;GLOG("RENDER_MAPTAP held-image failed %ux%u: MAP-tab frames are shown for the rest of the run",pw,ph);return;
        }
        toGeneral(mapTapHeld);
    }
    hazards({shown.image},{mapTapHeld.image});
    VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={pw,ph,1};
    vkCmdCopyImage(cmd,shown.image,VK_IMAGE_LAYOUT_GENERAL,mapTapHeld.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    afterWrite();mapTapHeldValid=true;++mapTapCopies;
}
// v661 / v663 (Resume Game, owner 2026-10-24): while the game shows a pause screen (map_tap.h pausedFrame: the pause blur without the
// tonemap; no wheel button held) the lower screen shows the Resume Game row (resume_screen.inc). v663: from the first such frame to the
// first frame without (no gate), and no colour sampling (the owner's fixed #A3A6AA, resume_layout.h).
// v667: no colour of its own any more: the lower screen mirrors the main picture (resume_screen.inc); pressed = the library is closing
// the pause screen (its Close episode, the lower screen's tap). mapTapLayerSeen stays a no-op hook (the draw hook's call).
std::uint64_t mapTapResumeFrames=0;bool mapTapResumeWas=false;
// thor-char (v685, owner 2026-10-24: "when transitioning we should get a screen very similar to the resume game screen but that says
// loading"): while the game runs a player switch (hud_game_state.h switchInProgress: the game's own is_player_switch_in_progress state,
// read from guest memory once per present) the lower screen shows the Resume screen's look with "Loading" and a spinner
// (loading_layout.h), from the first such frame to the first frame without (the HUD then returns by the usual rules); at most
// LoadingMaxMs per switch (then the usual lower screen even if the flag stays). Taps do nothing meanwhile (map_tap.h setLoading).
// v683 on the device (run dq1fbrnn 15:25:41-15:26:01): the switch's blurred frames passed pausedFrame and showed the Resume pill.
// v687 (device v685, run ti0p0ys4: the flag fell at 15:40:56.47, control returned at 57.7 and the switch's last blurred frames until 58.34
// passed pausedFrame: the Resume pill for 14 frames): Loading stayed while those frames looked paused (<= 4 s).
// v689 (owner on v687: "the loading should continue until the character is fully loaded in (it goes black first)"; device v687 run
// 37nenq59: the flag fell with control returning (ctl 1 at 15:53:57.9 / 15:54:35.0), then the lower screen was BLACK - the HUD hidden
// until the game drew its minimap again): after the flag falls Loading HOLDS until the game's own minimap is replayed again (hud4Serial
// advances: the game's HUD is back, so the lower screen's HUD picture is current) or the player presses START (a real pause then gets
// its Resume screen) or LoadingHoldMaxMs. The hold replaces v687's paused-frame tail (it covers those frames too).
// (the rule: loading_layout.h LoadingState, host-tested)
recomp::gpu::maptap::LoadingState mapTapLoadingState;bool mapTapLoadingWas=false,mapTapLoadingHold=false;std::uint64_t mapTapLoadingFrames=0;
// v693: the background variant (files/private-trace/diag-loading-bg, read once a second while the screen could show)
recomp::gpu::maptap::LoadingBg mapTapLoadingBg=recomp::gpu::maptap::DefaultBg;std::int64_t mapTapLoadingBgAt=-(std::int64_t(1)<<40);
void mapTapLoadingBgRead(std::int64_t now) noexcept {
    if(now-mapTapLoadingBgAt<1000)return;
    mapTapLoadingBgAt=now;char t[16]{};std::size_t n=0;
    if(FILE* f=std::fopen(RECOMP_APP_PATH("/private-trace/diag-loading-bg"),"rb")){n=std::fread(t,1,sizeof t-1,f);std::fclose(f);}
    const auto b=recomp::gpu::maptap::parseLoadingBg(t,n);
    if(b!=mapTapLoadingBg)GLOG("RENDER_LOADING bg=%s",b==recomp::gpu::maptap::LoadingBg::Blur?"blur":b==recomp::gpu::maptap::LoadingBg::Pause?"pause":"hybrid");
    mapTapLoadingBg=b;
}
recomp::gpu::maptap::RespawnState mapTapRespawnState;bool mapTapRespawnWas=false;
recomp::gpu::maptap::LowerPick mapTapLoading(std::int64_t now,bool paused,const std::uint8_t* base,bool controlOn,bool pauseMenu,bool pauseMap) noexcept {
    // (v691: paused = the frame looks like a pause screen - the switch hold's end rule only)
    const bool raw=base && recomp::gpu::hud::switchInProgress(recomp::gpu::hud::GuestMemory{base});
    auto& S=mapTapLoadingState;
    const auto o=S.step(raw,now,hud4Serial,recomp::gpu::maptap::machine().lastStartPressMs(),paused);
    if(o.rose)GLOG("RENDER_LOADING switch=1 episode=%llu",(unsigned long long)S.episodes);
    if(o.fell)GLOG("RENDER_LOADING switch=0 episode=%llu ms=%lld frames=%llu hold=1",(unsigned long long)S.episodes,(long long)(now-S.since),(unsigned long long)mapTapLoadingFrames);
    if(S.why)GLOG("RENDER_LOADING hold=0 why=%s hold_ms=%lld",S.why,(long long)(now-S.end));
    // RC1.1: death / arrest
    const auto fate=base?recomp::gpu::hud::readPlayerFate(recomp::gpu::hud::GuestMemory{base}):recomp::gpu::hud::PlayerFate{};
    const bool rraw=fate.dead || fate.arrested;
    auto& R=mapTapRespawnState;
    const auto r=R.step(rraw,controlOn,now,hud4Serial);
    if(r.rose)GLOG("RENDER_LOADING respawn=1 episode=%llu dead=%u arrested=%u",(unsigned long long)R.episodes,unsigned(fate.dead),unsigned(fate.arrested));
    if(r.fell)GLOG("RENDER_LOADING respawn=0 episode=%llu ms=%lld hold=1",(unsigned long long)R.episodes,(long long)(now-R.since));
    if(R.why)GLOG("RENDER_LOADING respawn_hold=0 why=%s hold_ms=%lld",R.why,(long long)(now-R.end));
    const auto pick=recomp::gpu::maptap::lowerPick(pauseMenu,pauseMap,o.show,r.show);
    const bool show=pick.loading && !census.active;
    if(show){++mapTapLoadingFrames;mapTapLoadingBgRead(now);}
    mapTapLoadingWas=raw || rraw;mapTapLoadingHold=S.hold || R.hold;mapTapRespawnWas=rraw;
    recomp::gpu::maptap::machine().setLoading(o.on || r.on);
    recomp::gpu::hud2::screen().publishLoading(show,recomp::gpu::maptap::tinted(mapTapLoadingBg));
    return pick;
}
void mapTapLayerSeen(const Image&) noexcept {}
// v677 (device v675: the lower screen stayed on Resume through the game's own unpause fade, ~1.3 s after the tap: the pause blur is drawn
// until the fade ends): a close (the lower screen's tap, or the player's START during a pause) shows the pressed pill for ResumePressedMs,
// then the HUD at once (its last minimap counts as current until the game's next one); the Resume screen stays off until the pause ends.
static constexpr std::int64_t ResumePressedMs=100;
std::int64_t mapTapSeenClose=-1,mapTapPressedUntil=0;bool mapTapClosing=false,mapTapResumePill=false;
void mapTapResume(bool paused,bool pill,const Image&,unsigned,unsigned,bool) noexcept {
    const std::int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::int64_t lc=recomp::gpu::maptap::machine().lastCloseMs();
    if(lc!=mapTapSeenClose){mapTapSeenClose=lc;if(paused){mapTapClosing=true;mapTapPressedUntil=lc+ResumePressedMs;}}
    if(!paused)mapTapClosing=false;
    const bool pressed=pill && mapTapClosing && now<mapTapPressedUntil,show=paused && (!mapTapClosing || pressed || !pill);
    if(show)++mapTapResumeFrames;
    if(show!=mapTapResumeWas || pill!=mapTapResumePill){mapTapResumeWas=show;mapTapResumePill=pill;GLOG("RENDER_MAPTAP resume=%u pill=%u frames=%llu paused=%u closing=%u",
        unsigned(show),unsigned(pill),(unsigned long long)mapTapResumeFrames,unsigned(paused),unsigned(mapTapClosing));}
    recomp::gpu::hud2::screen().publishResume(show,pressed,pill);
}
// v667 (main: "the mirrored backdrop must NOT include the fps overlay"): while paused, the present image as the main screen will show it
// (after the map-tap hold, BEFORE the fps overlay is copied in) is the mirror's source (the lower screen's Resume pass samples the result).
// v675 (owner on v673: "why is the map duplicated on the bottom screen?" - the colour should match, not a second map): the mirror is a
// heavy blur of the frame, like the PS5 home backdrop: the present image (before the fps overlay) halved six times with linear blits to
// 20x11 texels (each texel the mean of 64x64 of the frame; v675 preview at 40x22 still showed the floor plan's outline), then stretched
// back up level by level to 320x180 (linear: a smooth spread, no
// blocks); the lower screen samples that 320x180 image bilinearly, cover-fit. Colours and light follow the main screen frame by frame,
// no line, blip or letter survives. Ten small blits per paused frame (the first reads 1280x720), nothing otherwise.
// thor-char (v693): two more levels (10x5, 5x2) for the Loading screen's heavy blur; the up path starts at the level the screen wants
// (the Resume screen: 20x11 as before; Loading: 5x2 for blur / hybrid, 80x45 for pause - loading_layout.h LoadingBg).
static constexpr unsigned MirrorLevels=9; // 640x360, 320x180, 160x90, 80x45, 40x22, 20x11, 10x5, 5x2 (and 1280x720 is the source)
static constexpr unsigned MirrorResumeLast=5,MirrorHeavyLast=7,MirrorPauseLast=4; // (pause: 40x22, the game's own pause blur in its pause shots thor-edition/resume/main.png, own-main.png)
std::array<Image,MirrorLevels> mapTapMirrorLevels{};bool mapTapMirrorFailed=false;std::uint64_t mapTapMirrorFrames=0;
void mapTapMirror(const Image& shown,unsigned pw,unsigned ph) noexcept {
    if(!(mapTapResumeWas || mapTapLoadingWas || mapTapLoadingHold) || mapTapMirrorFailed || !shown.image || census.active || !pw || !ph)return; // (thor-char v685: + Loading)
    try {
        unsigned w[MirrorLevels],h[MirrorLevels];
        for(unsigned k=0;k<MirrorLevels-1;++k){w[k]=std::max(1u,pw>>(k+1));h[k]=std::max(1u,ph>>(k+1));}
        const unsigned last=mapTapResumeWas?MirrorResumeLast:recomp::gpu::maptap::heavy(mapTapLoadingBg)?MirrorHeavyLast:MirrorPauseLast; // (v693)
        for(unsigned k=0;k<=last;++k) {
            Image& im=mapTapMirrorLevels[k];
            if(!im.image || im.width!=w[k] || im.height!=h[k] || im.format!=shown.format) {
                if(im.image)destroyLater(im);
                im={};
                const VkComponentMapping id{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
                if(!makeImage(w[k],h[k],1,shown.format,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT,false,id,im)){im={};mapTapMirrorFailed=true;GLOG("RENDER_MAPTAP mirror-image failed %ux%u",w[k],h[k]);return;}
                toGeneral(im);
            }
        }
        const auto blit=[&](VkImage src,unsigned sw,unsigned sh,VkImage dst,unsigned dw,unsigned dh) {
            hazards({src},{dst});
            VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            b.srcOffsets[1]={std::int32_t(sw),std::int32_t(sh),1};b.dstOffsets[1]={std::int32_t(dw),std::int32_t(dh),1};
            vkCmdBlitImage(cmd,src,VK_IMAGE_LAYOUT_GENERAL,dst,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
            barrier();
        };
        blit(shown.image,pw,ph,mapTapMirrorLevels[0].image,w[0],h[0]);                      // down: 1280 -> 640 -> ... -> the start level
        for(unsigned k=1;k<=last;++k)blit(mapTapMirrorLevels[k-1].image,w[k-1],h[k-1],mapTapMirrorLevels[k].image,w[k],h[k]);
        for(unsigned k=last;k>1;--k)blit(mapTapMirrorLevels[k].image,w[k],h[k],mapTapMirrorLevels[k-1].image,w[k-1],h[k-1]); // up: 20 -> 40 -> ... -> 320
        afterWrite();++mapTapMirrorFrames;
        recomp::gpu::hud2::screen().mirrorSource(mapTapMirrorLevels[1].view,w[1],h[1]); // (320x180: each up blit stretches over the whole next level)
    } catch(...) {}
}

// v826 (thor-radio, item 6): the frame-rate counter as v713's game-style label ("60 FPS", white, black shadow, no box; the real rate small
// and grey under frame generation) at the main screen's top left. Rebuilt when the meter's numbers change; its opaque runs copied from
// the ring (one copy command). files/private-trace/diag-fps-detail = v309's strip at the top right instead (generic_renderer.cpp).
recomp::gpu::generic::v713::Label thorFpsLabel;double thorFpsShown[2]{-1,-1};bool thorFpsDetail=false;std::int64_t thorFpsCheckMs=-(std::int64_t(1)<<40);
std::vector<VkBufferImageCopy> thorFpsRegions;
static constexpr std::int32_t ThorFpsX=12,ThorFpsY=8; // (1280-wide present image: the main screen's top left)
void thorFpsDraw(const Image& shown,unsigned pw,unsigned ph) noexcept {
#if defined(RECOMP_DUO_EDITION)
    (void)shown;(void)pw;(void)ph;return; // ios-duo: the iOS 238e base draws the game-style label and serves recomp_fps_v1 itself (generic_renderer.cpp)
#else
    namespace g=recomp::gpu::generic;
    const std::int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now-thorFpsCheckMs>=1000){thorFpsCheckMs=now;thorFpsDetail=access(RECOMP_APP_PATH("/private-trace/diag-fps-detail"),F_OK)==0;}
    g::v309::thorLabelOn().store(fpsOverlay && !thorFpsDetail,std::memory_order_relaxed);
    // v832 (android-qol's v714): the rates for the app's label (recomp_fps_v1); while the app reads them it draws the label, the library none
    g::v714::fpsShown.store(float(fpsMeter.shownFps),std::memory_order_relaxed);g::v714::fpsReal.store(float(fpsMeter.fps),std::memory_order_relaxed);
    g::v714::fpsMs.store(float(fpsMeter.ms),std::memory_order_relaxed);g::v714::fpsOn.store(fpsOverlay?1:0,std::memory_order_relaxed);
    if(!thorFpsDetail && g::v714::appDrawing(g::v714::nowMs(),g::v714::fpsAppMs.load(std::memory_order_relaxed)))return;
    if(!fpsOverlay || thorFpsDetail || census.active || census.armed || !shown.image || !(fpsMeter.fps>0))return;
    try {
        if(fpsMeter.fps!=thorFpsShown[0] || fpsMeter.shownFps!=thorFpsShown[1]){thorFpsShown[0]=fpsMeter.fps;thorFpsShown[1]=fpsMeter.shownFps;thorFpsLabel=g::v713::render(fpsMeter.fps,fpsMeter.shownFps);}
        const unsigned w=thorFpsLabel.width,h=thorFpsLabel.height;
        if(thorFpsLabel.rgba.empty() || w+ThorFpsX+8>pw || h+ThorFpsY+8>ph)return;
        const auto at=allocate(thorFpsLabel.rgba.size());if(!at)return;
        std::memcpy(static_cast<std::uint8_t*>(ring.mapped)+*at,thorFpsLabel.rgba.data(),thorFpsLabel.rgba.size());
        thorFpsRegions.clear();
        for(const auto& r:thorFpsLabel.runs) {
            VkBufferImageCopy c{};c.bufferOffset=*at+(VkDeviceSize(r.y)*w+r.x)*4;c.bufferRowLength=w;c.bufferImageHeight=h;
            c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageOffset={ThorFpsX+r.x,ThorFpsY+r.y,0};c.imageExtent={r.w,1,1};
            thorFpsRegions.push_back(c);
        }
        if(thorFpsRegions.empty())return;
        hazards({},{shown.image});
        vkCmdCopyBufferToImage(cmd,ring.buffer,shown.image,VK_IMAGE_LAYOUT_GENERAL,std::uint32_t(thorFpsRegions.size()),thorFpsRegions.data());
        afterWrite();
    } catch(...) {}
#endif
}
#else /* RECOMP_THOR_EDITION */
// thor-maptap (v658, Thor edition only: included under RECOMP_THOR_EDITION as the LAST members of Renderer, after thor-wheel's): the
// renderer's half of map_tap.h. draw() marks a frame that drew the MAP tab's map window (v659: on every draw, one compare of the
// scissor's x first); present() hands that to the macro and, while an episode runs, keeps a copy of the last frame without the MAP tab
// (the game's own pause blur, or the last game frame) and puts it in place of every MAP-tab frame. One vkCmdCopyImage of the present
// image per frame of an episode (~0.5 s), nothing otherwise. RENDER_MAPTAP lines: one per episode (its times from the request).
// qol-touch (v700): also included under RECOMP_TOUCH_MAPTAP (the single-screen QoL edition, no RECOMP_THOR_EDITION): the macro and the hold
// only, through mapTapPresentTouch / mapTapDrawSeen; the lower screen's Resume / Loading / mirror below stay Thor-only.
Image mapTapHeld;bool mapTapHeldValid=false;bool mapTapFailed=false;
std::uint64_t mapTapCopies=0,mapTapRestores=0;
#ifdef RECOMP_THOR_EDITION
[[gnu::noinline]] void mapTapPresent(const Image& shown,unsigned pw,unsigned ph) {mapTapPresentInner(shown,pw,ph);mapTapMirror(shown,pw,ph);}
#else /* RECOMP_TOUCH_MAPTAP (qol-touch v700, the single-screen edition): the macro and the MAP-tab hold only; no lower screen */
// present(): one call, no branch there (its PGO record); the diag-mosaic view is left alone, as the Thor's `if(!mosaic)`.
// v717 / iPad 239 (the phone pad): once a present, the phone is up while one of its screen quads was seen in the last 8 frames (the
// Thor's PhoneTracker rule); while it is up the trainer's safe point reads the call natives (recomp_phone_poll).
bool qolPhoneSeen=false;unsigned qolPhoneLeft=0;
[[gnu::noinline]] void qolPhoneFrame() noexcept {
    qolPhoneLeft=qolPhoneSeen?8u:(qolPhoneLeft?qolPhoneLeft-1:0u);qolPhoneSeen=false;
    const int bits=qolPhoneLeft>0?1:0;
    recomp_phone_poll.store(bits!=0,std::memory_order_relaxed);
    if(bits!=recomp_phone_bits.load(std::memory_order_relaxed)){recomp_phone_bits.store(bits,std::memory_order_relaxed);GLOG("RENDER_QOL_PHONE up=%d",bits);}
}
[[gnu::noinline]] void mapTapPresentTouch(bool mosaic,const Image& shown,unsigned pw,unsigned ph) noexcept {
    qolPhoneFrame(); // (v717 / iPad 239: the phone pad's signal, once a present)
    if(mosaic)return;
    try {mapTapPresentInner(shown,pw,ph);} catch(...) {}
}
// draw(): the Thor's draw-hook block (generic_renderer.cpp, RECOMP_THOR_EDITION) out of line: what this frame drew for the macro, and the
// pause map's projection for the waypoint tap.
[[gnu::noinline]] void mapTapDrawSeen(std::int32_t x0,std::int32_t y0,std::int32_t x1,std::int32_t y1,std::uint32_t passWidth,std::uint64_t psHash,
                                      const DrawInput& in) noexcept {
    namespace mt=recomp::gpu::maptap;
    const unsigned seen=(mt::tabScissor(x0,y0,x1,y1,passWidth)?mt::SeenTab:0u)| // (v703: +-4, no exact x0: the iPad's window is 1 px off)
                        (x0==1 && mt::fullScissor(x0,y0,x1,y1,passWidth)?mt::SeenFull:0u)|(psHash==mt::BlurPs?mt::SeenBlur:0u)|
                        (psHash==mt::TonemapPs?mt::SeenPost:0u);
    if(seen)mt::seenDrawn().fetch_or(seen,std::memory_order_relaxed);
    if((psHash==mt::MapTilePs || psHash==mt::MapVectorPs) && (seen&mt::SeenFull) && in.vertexConstants) {
        const auto* w=in.vertexConstants->data();
        mt::machine().noteXform(regFloat(w[8*4+0]),regFloat(w[9*4+1]),regFloat(w[11*4+0]),regFloat(w[11*4+1]));
    }
}
#endif /* RECOMP_THOR_EDITION */
void mapTapPresentInner(const Image& shown,unsigned pw,unsigned ph) {
    namespace mt=recomp::gpu::maptap;
    const unsigned seen=mt::seenDrawn().exchange(0u,std::memory_order_relaxed);
    const bool tab=(seen&mt::SeenTab)!=0;
    const std::int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto f=mt::machine().present(now,seen);
    if(f.ended) {
        const auto& s=f.summary;
        GLOG("RENDER_MAPTAP %s n=%llu mode=%s end=%s start_up_ms=%lld start_frames=%llu tab_ms=%lld release_ms=%lld tab_to_release=%llu last_release_ms=%lld "
             "retries=%u done_ms=%lld tab_frames=%u held=%u early=%u blur_ms=%lld full_seen=%u copies=%llu restores=%llu",mt::Marker,(unsigned long long)s.n,mt::modeName(s.mode),mt::endName(s.end),
             (long long)s.startUpMs,(unsigned long long)s.startPresents,(long long)s.tabMs,(long long)s.firstReleaseMs,(unsigned long long)s.tabToRelease,
             (long long)s.lastReleaseMs,s.retries,(long long)s.doneMs,s.tabFrames,s.held,unsigned(s.early),(long long)s.blurMs,unsigned(s.fullSeen),(unsigned long long)mapTapCopies,(unsigned long long)mapTapRestores);
        if(s.mode==mt::Mode::Waypoint)GLOG("RENDER_MAPTAP waypoint n=%llu panned=%u tap=%d,%d xf=%.6g,%.6g,%.6g,%.6g pan_frames=%u left_px=%.1f end=%s",(unsigned long long)s.n,
            unsigned(s.panned),s.tap[0],s.tap[1],double(s.xf[0]),double(s.xf[1]),double(s.xf[2]),double(s.xf[3]),s.panFrames,double(s.panLeft),mt::endName(s.end));
    }
#ifdef RECOMP_THOR_EDITION
    const bool switching=mapTapLoading(now,mt::pausedFrame(seen)); // thor-char (v685): a character switch = the Loading screen, never the Resume one (its blur looks paused)
    mapTapResume(mt::pausedFrame(seen) && !mt::machine().wheelRecent(now) && !switching,shown,pw,ph,f.hold); // v661 / v663 / v665: the lower screen's Resume Game
#endif /* RECOMP_THOR_EDITION */
    if(!f.active){mapTapHeldValid=false;return;}
    if(mapTapFailed || !shown.image || census.active)return;
    if(f.hold) {
        if(!mapTapHeldValid)return; // (no frame without the tab yet: this one is shown)
        hazards({mapTapHeld.image},{shown.image});
        VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={pw,ph,1};
        vkCmdCopyImage(cmd,mapTapHeld.image,VK_IMAGE_LAYOUT_GENERAL,shown.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
        afterWrite();++mapTapRestores;
        return;
    }
    if(tab)return; // (a MAP-tab frame that is not hidden: hide=0, or after the episode's end)
    if(!mapTapHeld.image || mapTapHeld.width!=pw || mapTapHeld.height!=ph || mapTapHeld.format!=shown.format) {
        if(mapTapHeld.image)destroyLater(mapTapHeld);
        mapTapHeld={};mapTapHeldValid=false;
        const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
        if(!makeImage(pw,ph,1,shown.format,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,false,identity,mapTapHeld)) {
            mapTapHeld={};mapTapFailed=true;GLOG("RENDER_MAPTAP held-image failed %ux%u: MAP-tab frames are shown for the rest of the run",pw,ph);return;
        }
        toGeneral(mapTapHeld);
    }
    hazards({shown.image},{mapTapHeld.image});
    VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={pw,ph,1};
    vkCmdCopyImage(cmd,shown.image,VK_IMAGE_LAYOUT_GENERAL,mapTapHeld.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    afterWrite();mapTapHeldValid=true;++mapTapCopies;
}
#ifdef RECOMP_THOR_EDITION // (the lower screen's Resume / Loading screens and its mirror: Thor only)
// v661 / v663 (Resume Game, owner 2026-10-24): while the game shows a pause screen (map_tap.h pausedFrame: the pause blur without the
// tonemap; no wheel button held) the lower screen shows the Resume Game row (resume_screen.inc). v663: from the first such frame to the
// first frame without (no gate), and no colour sampling (the owner's fixed #A3A6AA, resume_layout.h).
// v667: no colour of its own any more: the lower screen mirrors the main picture (resume_screen.inc); pressed = the library is closing
// the pause screen (its Close episode, the lower screen's tap). mapTapLayerSeen stays a no-op hook (the draw hook's call).
std::uint64_t mapTapResumeFrames=0;bool mapTapResumeWas=false;
// thor-char (v685, owner 2026-10-24: "when transitioning we should get a screen very similar to the resume game screen but that says
// loading"): while the game runs a player switch (hud_game_state.h switchInProgress: the game's own is_player_switch_in_progress state,
// read from guest memory once per present) the lower screen shows the Resume screen's look with "Loading" and a spinner
// (loading_layout.h), from the first such frame to the first frame without (the HUD then returns by the usual rules); at most
// LoadingMaxMs per switch (then the usual lower screen even if the flag stays). Taps do nothing meanwhile (map_tap.h setLoading).
// v683 on the device (run dq1fbrnn 15:25:41-15:26:01): the switch's blurred frames passed pausedFrame and showed the Resume pill.
// v687 (device v685, run ti0p0ys4: the flag fell at 15:40:56.47, control returned at 57.7 and the switch's last blurred frames until 58.34
// passed pausedFrame: the Resume pill for 14 frames): after the flag falls, the Loading screen stays while the frames still look
// paused, at most LoadingTailMs (a real pause menu cannot open in that time: the player has no control yet).
static constexpr std::int64_t LoadingMaxMs=60000,LoadingTailMs=4000;
bool mapTapLoadingWas=false,mapTapLoadingTail=false;std::int64_t mapTapLoadingSince=0,mapTapLoadingEnd=-(std::int64_t(1)<<40);
std::uint64_t mapTapLoadingFrames=0,mapTapLoadingEpisodes=0,mapTapLoadingTailFrames=0;
bool mapTapLoading(std::int64_t now,bool paused) noexcept {
    const auto* base=recomp::gpu::hud::guestBase().load(std::memory_order_acquire);
    const bool raw=base && recomp::gpu::hud::switchInProgress(recomp::gpu::hud::GuestMemory{base});
    if(!raw && mapTapLoadingWas && !mapTapLoadingTail){mapTapLoadingEnd=now;}
    const bool tail=!raw && paused && now-mapTapLoadingEnd<LoadingTailMs && mapTapLoadingEpisodes;
    if(tail)++mapTapLoadingTailFrames;
    if(tail!=mapTapLoadingTail)GLOG("RENDER_LOADING tail=%u tail_frames=%llu",unsigned(tail),(unsigned long long)mapTapLoadingTailFrames);
    mapTapLoadingTail=tail;
    const bool flag=raw || tail;
    if(raw && !mapTapLoadingWas){mapTapLoadingSince=now;++mapTapLoadingEpisodes;}
    const bool show=flag && now-mapTapLoadingSince<LoadingMaxMs && !census.active;
    if(show)++mapTapLoadingFrames;
    if(raw!=mapTapLoadingWas)GLOG("RENDER_LOADING switch=%u episode=%llu ms=%lld frames=%llu",unsigned(raw),(unsigned long long)mapTapLoadingEpisodes,
        (long long)(raw?0:now-mapTapLoadingSince),(unsigned long long)mapTapLoadingFrames);
    mapTapLoadingWas=raw;
    recomp::gpu::maptap::machine().setLoading(flag);
    recomp::gpu::hud2::screen().publishLoading(show);
    return flag;
}
void mapTapLayerSeen(const Image&) noexcept {}
// v677 (device v675: the lower screen stayed on Resume through the game's own unpause fade, ~1.3 s after the tap: the pause blur is drawn
// until the fade ends): a close (the lower screen's tap, or the player's START during a pause) shows the pressed pill for ResumePressedMs,
// then the HUD at once (its last minimap counts as current until the game's next one); the Resume screen stays off until the pause ends.
static constexpr std::int64_t ResumePressedMs=100;
std::int64_t mapTapSeenClose=-1,mapTapPressedUntil=0;bool mapTapClosing=false;
void mapTapResume(bool paused,const Image&,unsigned,unsigned,bool) noexcept {
    const std::int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::int64_t lc=recomp::gpu::maptap::machine().lastCloseMs();
    if(lc!=mapTapSeenClose){mapTapSeenClose=lc;if(paused){mapTapClosing=true;mapTapPressedUntil=lc+ResumePressedMs;}}
    if(!paused)mapTapClosing=false;
    const bool pressed=mapTapClosing && now<mapTapPressedUntil,show=paused && (!mapTapClosing || pressed);
    if(show)++mapTapResumeFrames;
    if(show!=mapTapResumeWas){mapTapResumeWas=show;GLOG("RENDER_MAPTAP resume=%u frames=%llu paused=%u closing=%u",unsigned(show),(unsigned long long)mapTapResumeFrames,
        unsigned(paused),unsigned(mapTapClosing));}
    recomp::gpu::hud2::screen().publishResume(show,pressed);
}
// v667 (main: "the mirrored backdrop must NOT include the fps overlay"): while paused, the present image as the main screen will show it
// (after the map-tap hold, BEFORE the fps overlay is copied in) is the mirror's source (the lower screen's Resume pass samples the result).
// v675 (owner on v673: "why is the map duplicated on the bottom screen?" - the colour should match, not a second map): the mirror is a
// heavy blur of the frame, like the PS5 home backdrop: the present image (before the fps overlay) halved six times with linear blits to
// 20x11 texels (each texel the mean of 64x64 of the frame; v675 preview at 40x22 still showed the floor plan's outline), then stretched
// back up level by level to 320x180 (linear: a smooth spread, no
// blocks); the lower screen samples that 320x180 image bilinearly, cover-fit. Colours and light follow the main screen frame by frame,
// no line, blip or letter survives. Ten small blits per paused frame (the first reads 1280x720), nothing otherwise.
static constexpr unsigned MirrorLevels=7; // 640x360, 320x180, 160x90, 80x45, 40x22, 20x11 (and 1280x720 is the source)
std::array<Image,MirrorLevels> mapTapMirrorLevels{};bool mapTapMirrorFailed=false;std::uint64_t mapTapMirrorFrames=0;
void mapTapMirror(const Image& shown,unsigned pw,unsigned ph) noexcept {
    if(!(mapTapResumeWas || mapTapLoadingWas || mapTapLoadingTail) || mapTapMirrorFailed || !shown.image || census.active || !pw || !ph)return; // (thor-char v685: + Loading)
    try {
        unsigned w[MirrorLevels],h[MirrorLevels];
        for(unsigned k=0;k<MirrorLevels-1;++k){w[k]=std::max(1u,pw>>(k+1));h[k]=std::max(1u,ph>>(k+1));}
        const unsigned last=MirrorLevels-2; // (index of 20x11 at 1280x720)
        for(unsigned k=0;k<=last;++k) {
            Image& im=mapTapMirrorLevels[k];
            if(!im.image || im.width!=w[k] || im.height!=h[k] || im.format!=shown.format) {
                if(im.image)destroyLater(im);
                im={};
                const VkComponentMapping id{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
                if(!makeImage(w[k],h[k],1,shown.format,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT,false,id,im)){im={};mapTapMirrorFailed=true;GLOG("RENDER_MAPTAP mirror-image failed %ux%u",w[k],h[k]);return;}
                toGeneral(im);
            }
        }
        const auto blit=[&](VkImage src,unsigned sw,unsigned sh,VkImage dst,unsigned dw,unsigned dh) {
            hazards({src},{dst});
            VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            b.srcOffsets[1]={std::int32_t(sw),std::int32_t(sh),1};b.dstOffsets[1]={std::int32_t(dw),std::int32_t(dh),1};
            vkCmdBlitImage(cmd,src,VK_IMAGE_LAYOUT_GENERAL,dst,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
            barrier();
        };
        blit(shown.image,pw,ph,mapTapMirrorLevels[0].image,w[0],h[0]);                      // down: 1280 -> 640 -> ... -> 40
        for(unsigned k=1;k<=last;++k)blit(mapTapMirrorLevels[k-1].image,w[k-1],h[k-1],mapTapMirrorLevels[k].image,w[k],h[k]);
        for(unsigned k=last;k>1;--k)blit(mapTapMirrorLevels[k].image,w[k],h[k],mapTapMirrorLevels[k-1].image,w[k-1],h[k-1]); // up: 20 -> 40 -> ... -> 320
        afterWrite();++mapTapMirrorFrames;
        recomp::gpu::hud2::screen().mirrorSource(mapTapMirrorLevels[1].view,w[1],h[1]); // (320x180: each up blit stretches over the whole next level)
    } catch(...) {}
}
#endif /* RECOMP_THOR_EDITION */
#endif /* RECOMP_THOR_EDITION */
