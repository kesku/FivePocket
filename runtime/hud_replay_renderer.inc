// hud4 (v597): the second screen's minimap, replayed from the game's own operations (hud_replay.h says which and why). Included
// as the LAST members of Renderer (the members above keep their offsets). Everything here is out of line and noexcept; the
// existing functions gain only calls: execute() (one call after a draw item's draw(), one after a clear item's clear(), one
// before the present item's present()), v585Radar (one call), target() / boundUncached() (four argument helpers below, no new
// branch in either), rsTargetK (its body: the replay's scale while a replay runs).
//
// Copies (hud4Draw / hud4Clear, on the renderer thread right after the operation executed): the draw's state ranges, both
// constant banks (shared while their version stays), its fetch constants, scissor words, shader code (one copy per code version),
// indices, inline vertices, and its guest spans (copied up to Hud4SpanCopy bytes each; larger ones that lie in guest memory stay
// in place: static map data; a span the capture itself copied is always copied). A queued packet's state is a delta against a
// running state and its banks and spans are recycled, so nothing of the packet is kept.
//
// Replay (hud4Present, before the present item's present(), only when the Collector saw the radar pass, its resolve and the
// composite): the radar list into the shadow radar target, then the box list into the shadow overlay target, both through draw()
// with target() redirected (hud4Target: the four expected surfaces to the shadows, anything else to a scratch target and the
// replay counted as broken) and the scale hud4K. Clears are recorded here through ensurePass + rsClearAttachments (clear() would
// also clear the EDRAM aliases of the REAL surfaces). For the box list, the resolved radar texture's entry in `resolved` shows the
// shadow radar for the duration (its image fields swapped, resolveEpoch bumped before and after: every binding is verified
// again), and each operation gets a window offset that moves the box to the shadow's origin. The shadow targets are never in
// `targets` (no alias clear, residue or memo of a real surface can reach them). After the replay: the redirect off, the memo
// generations bumped, the viewer restored; the overlay is handed to the second screen (hud2::screen().publishReplica).
struct Hud4Op {
    bool clear=false;
    std::vector<std::uint8_t> state;              // draws: the three state ranges (zero elsewhere); clears: the whole state
    std::shared_ptr<const std::array<std::uint32_t,1024>> vc,pc;std::uint64_t vcVersion=0,pcVersion=0;
    std::array<std::array<std::uint32_t,6>,32> fetch{};std::uint32_t fetchMask=0;
    std::optional<std::array<std::uint32_t,3>> scissor;
    std::shared_ptr<const std::vector<std::uint8_t>> identity,effective,pixel;
    std::shared_ptr<const std::vector<std::vector<std::uint8_t>>> alternates;
    std::uint64_t identityVersion=0,effectiveVersion=0,pixelVersion=0;
    unsigned primitive=0;IndexVector indices;bool indicesFinal=false;std::uint32_t baseVertex=0;unsigned vertexCount=0;
    std::vector<std::uint8_t> inlineVertices;bool haveInline=false;unsigned inlineStride=0;const char* source=nullptr;
    std::vector<AsyncSpan> spans;std::vector<std::vector<std::uint8_t>> spanBytes;
    ClearInput clearIn;hudrep::Rect rect{};
};
struct Hud4List {std::vector<Hud4Op> ops;unsigned n=0;
    Hud4Op& next(){if(n==ops.size())ops.emplace_back();return ops[n++];}};
static constexpr std::array<std::pair<unsigned,unsigned>,3> Hud4StateRanges{{{1152,1928},{10112,10864},{12816,12836}}};
static constexpr unsigned Hud4SurfaceInfo=10368; // RB_SURFACE_INFO (0x2000) in the draw state (= gta5DrawRegisterOffset(0x2000): hud4_test)
static constexpr std::uint64_t Hud4SpanCopy=1u<<20,Hud4FrameBudget=48ull<<20;
hudrep::Collector hud4C;Hud4List hud4Radar,hud4Overlay;std::uint64_t hud4FrameBytes=0;
std::shared_ptr<const std::array<std::uint32_t,1024>> hud4LastVc,hud4LastPc;std::uint64_t hud4LastVcVersion=0,hud4LastPcVersion=0;
std::unordered_map<std::uint64_t,std::shared_ptr<const std::vector<std::uint8_t>>> hud4Code; // code version -> copy
// the redirect (only while hud4On)
struct Hud4Redirect {unsigned base=0,format=0,pitch=0;bool depth=false;Target* to=nullptr;};
std::array<Hud4Redirect,2> hud4Map{};bool hud4On=false;int hud4K=16;unsigned hud4W=0,hud4H=0;bool hud4Unexpected=false;
Target hud4RadarColor,hud4RadarDepth,hud4OverlayColor,hud4OverlayDepth,hud4Scratch;
// v638 (look=game): the main frame's pixels under the box this frame (captured before the game's composite); its host size; whether
// this frame captured it; counters (RENDER_HUD12 lines)
Image hud12Backdrop{};std::uint32_t hud12BdW=0,hud12BdH=0;bool hud12Captured=false;std::uint64_t hud12Captures=0,hud12Published=0;bool hud12Logged=false;
std::vector<Image> hud4Retired; // published overlay images replaced by larger ones: kept (the second screen may still sample them)
int hud4Scale=0;                // the replay's sixteenths, chosen at the first replay from the second screen's size and the box
Resolved hud4Swap;std::array<unsigned,4> hud4SwapView{9,9,9,9}; // the shadow radar as a resolve destination (fields swapped in)
Viewer hud4SavedViewer;Viewer hud4Viewer;Reader hud4Reader;
VkQueryPool hud4Pool=VK_NULL_HANDLE;unsigned hud4PoolSlot=0;std::array<bool,4> hud4PoolPending{};
std::uint64_t hud4Serial=0;
struct Hud4Stats {std::uint64_t replays=0,skipped=0,broken=0,refused=0,draws=0,clears=0,copiedKb=0,inPlaceKb=0,cpuNs=0,cpuNsMax=0,gpuNs=0,gpuN=0,
    gpuNsMax=0,ocq=0,budget=0,unexpected=0,noResolved=0,exceptions=0,radarOps=0,overlayOps=0;const char* firstRefusal=nullptr;} hud4Stats;
bool hud4Logged=false;
// hud5 (Thor edition, v604): the game's wanted stars (hud_replay.h StarCollector) replayed into the overlay's star band, the
// "Bottom screen only" skip, and one read-back of the first star frame that measures the row (the crop shown).
unsigned hud7BoxW=0,hud7BoxH=0;int hud7BoxK=0; // v623: the overlay's box + star part (grow only, its own: not the colour shadow's size)
hudrep::StarCollector hud5S;Hud4List hud5Stars;hudrep::Rect hud5Crop=hudrep::StarCropDefault;bool hud5Measured=false,hud5Logged=false;
Buffer hud5Probe;bool hud5ProbePending=false;std::uint64_t hud5Presents=0,hud5ProbeAt=0;unsigned hud5ProbeBytes=0,hud5ProbeW=0,hud5ProbeH=0;
hudrep::Rect hud5ProbeHost{};int hud5ProbeK=16;
unsigned hud5FrameStars=0; // (this frame's star operations, StarCollector::present)
// hud7 (v620): the map's rows added above and below (chosen with the scale), the copies made this frame (every Nth frame), the counter
std::int32_t hud8Shift=0,hud8NdcShift=0;bool hud8Probed=false,hud8ProbePending=false;Buffer hud8Probe;std::uint64_t hud8ProbeAt=0;
unsigned hud8ProbeW=0,hud8ProbeH=0;std::array<std::uint8_t,4> hud8Clear{};bool hud8ClearKnown=false; // hud8b (v626)
bool hud11Frame=false; // (hud11 v635: this replay drew the frame band)
// hud15 (v695 -> v804, thor-radio): this replay drew the flash band; flashes shown / refused (v806 Thor RC1: the RENDER_HUD15 / HUD16
// diagnostics of v695-v804 removed).
bool hud15Flash=false;std::uint64_t hud15Shown=0,hud15Refused=0;
// The op's own vertices (the Scaleform fill layout at vertex fetch 95, 8-byte stride) in a copied / in-place span or inline; null if none.
const std::uint8_t* hud15Vertices(const Hud4Op& op) const noexcept {
    if(op.clear || op.state.size()<1920 || !op.vertexCount)return nullptr;
    const auto be=[](const std::uint8_t* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|std::uint32_t(p[3]);};
    const std::uint32_t f0=be(op.state.data()+1152+8*95);
    if((f0&3u)!=3u)return nullptr;
    const std::uint64_t pa=f0&0x1FFFFFFCu,need=(std::uint64_t(op.baseVertex)+op.vertexCount)*8u;
    for(const auto& sp:op.spans){const std::uint64_t sa=sp.address&0x1FFFFFFFu;
        if(sp.host && sa<=pa && pa+need<=sa+sp.bytes)return sp.host+(pa-sa)+std::size_t(op.baseVertex)*8u;}
    if(op.haveInline && op.inlineVertices.size()>=need)return op.inlineVertices.data()+std::size_t(op.baseVertex)*8u;
    return nullptr;
}
// hud15 (v804 / v809): a box draw drawn as the flash - the damage glow (hudrep::flashTexture on its counts and texture fetches) or the
// wanted flash (hudrep::wantedRect on its own vertices).
bool hud15IsFlash(const Hud4Op& op) const noexcept {
    if(op.clear || op.indices.size()!=hudrep::FlashIndices || op.vertexCount!=hudrep::FlashVertices)return false;
    unsigned textures=0,w=0,h=0;
    for(unsigned k=0;k<32;++k)if((op.fetchMask>>k)&1u){++textures;if(const auto d=XenosTextureDescription::decode(op.fetch[k])){w=d->width;h=d->height;}}
    if(textures)return hudrep::flashTexture(op.indices.size(),op.vertexCount,textures,w,h);
    return hudrep::wantedRect(hud15Vertices(op),op.indices.size(),op.vertexCount);
}
std::int32_t hud8Rows=135;bool hud7Copy=true;std::uint64_t hud7Frames=0; // (hud8 v625: hud8Rows = the rows shown from row 0)
std::uint64_t hud7Skipped=0;bool hud7Logged=false;
// v649 (hud14; owner 2026-10-23 "the gps bar looks janky ... fix that black transparent bar that has the meter count on it"): the lower
// screen shows the box layer OVER the map by the layer's alpha (hud_screen.frag kind 4 flag 1). The game's alpha writes are not coverage
// (its back buffer's alpha is never shown): the GPS strip's distance text ("1.21mi") left alpha 0 inside its quads, and the map showed
// through as a grey box in the dark strip (device frame thor-edition/gps-lower-1.png: (190,193,198) = the map inside the text quads,
// (15,15,16) in the strip). While the box layer is replayed, a draw whose colour blend is "over" (dst factor ONE_MINUS_SRC_ALPHA) gets
// the alpha equation ONE, ONE_MINUS_SRC_ALPHA, ADD: alpha accumulates as coverage. The main frame's draws are never changed.
bool hud14Cover=false;std::unordered_set<std::uint64_t> hud14Seen;std::uint64_t hud14Changed=0;
void hud14Blend(std::array<std::uint32_t,4>& blend,std::uint64_t ps,unsigned colorMask) noexcept {
    for(unsigned i=0;i<4;++i) {
        const std::uint32_t w=blend[i];
        const bool over=((w>>8)&31u)==7u; // colour dst = ONE_MINUS_SRC_ALPHA
        const std::uint32_t n=over?((w&0x0000FFFFu)|(1u<<16)|(0u<<21)|(7u<<24)):w;
        if(i==0 && hud14Seen.size()<64 && hud14Seen.insert(ps^(std::uint64_t(w)<<20)^colorMask).second)
            GLOG("RENDER_HUD14 ps=%016llx blend=%08x -> %08x cm=%04x",(unsigned long long)ps,w,n,colorMask);
        if(n!=w){blend[i]=n;++hud14Changed;}
    }
}
struct Hud5Stats {std::uint64_t starFrames=0,starOps=0,skippedDraws=0,skippedClears=0,starBroken=0;} hud5Stats;
// v614 (thor-wheel's fix of v612's device run l2ytja7n: RENDER_HUD4 broken = unexpected = every frame): the scene draws hundreds of 1212-index
// meshes on pitch-1280 G-buffer surfaces (384:0:1280 with three more targets; every Thor census), the same index count as the star head.
// StarCollector took the frame's last such mesh as the head at wanted 0, and replaying it named a surface outside the overlay's redirect
// (hud4Target -> unexpected -> the whole replay broken; window-offset -1120,112 = the star band's in the log). A star is drawn on the
// main frame's colour surface, the one the composite names: once a replay knew it (hud5Main), other surfaces' draws are not stars; and
// hud5ReplayStars draws nothing (no stars this frame, the box still replayed) unless every star operation names the redirected surfaces.
struct Hud5Key {unsigned pitch=0,color=0,format=0;bool known=false;} hud5Main;

// ---- the helpers target() / boundUncached() call (identity while no replay runs)
[[gnu::noinline]] Target& hud4Target(Target& real,unsigned base,unsigned format,unsigned pitch,bool depth) noexcept {
    if(!hud4On)return real;
#if defined(RECOMP_HUD4_MUTANT_REAL) // test mutant: the replay draws into the real surfaces
    return real;
#endif
    for(const auto& m:hud4Map)if(m.to && m.base==base && m.format==format && m.pitch==pitch && m.depth==depth)return *m.to;
    hud4Unexpected=true;return hud4Scratch; // (an operation that names another surface draws into the scratch, never a real one)
}
[[gnu::noinline]] unsigned hud4TargetHeight(unsigned height) const noexcept {return hud4On?std::min(height,hud4H):height;}
[[gnu::noinline]] unsigned hud4TargetWidth(unsigned pitch) const noexcept {return hud4On?hud4W:pitch;}
[[gnu::noinline]] unsigned hud4BoundWidth(unsigned pitch) const noexcept {return hud4On?hud4W:pitch;}

// ---- copies (renderer thread, after the operation executed)
bool hud4Wanted() const noexcept {return recomp::gpu::hud2::screen().replicaWanted() && !census.active && !census.armed;}
std::shared_ptr<const std::vector<std::uint8_t>> hud4CodeCopy(const std::vector<std::uint8_t>* code,std::uint64_t version) {
    if(!code)return nullptr;
    if(version) {
        if(hud4Code.size()>512)hud4Code.clear();
        auto& e=hud4Code[version];
        if(!e || e->size()!=code->size())e=std::make_shared<const std::vector<std::uint8_t>>(*code);
        return e;
    }
    return std::make_shared<const std::vector<std::uint8_t>>(*code);
}
std::shared_ptr<const std::array<std::uint32_t,1024>> hud4Bank(const std::array<std::uint32_t,1024>* bank,std::uint64_t version,
                                                                std::shared_ptr<const std::array<std::uint32_t,1024>>& last,std::uint64_t& lastVersion) {
    if(!bank)return nullptr;
    if(version && version==lastVersion && last)return last;
    last=std::make_shared<const std::array<std::uint32_t,1024>>(*bank);lastVersion=version;
    return last;
}
// The spans: copied (up to Hud4SpanCopy each, always when the capture owned them), else kept in place (guest memory).
bool hud4CopySpans(Hud4Op& op,const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) {
    op.spans.clear();
    std::size_t used=0;
    for(const auto& s:spans) {
        if(!s.host){op.spans.push_back(s);continue;}
        bool own=false;for(const auto& v:owned)if(s.host>=v.data() && s.host<v.data()+v.size()){own=true;break;}
        if(s.bytes<=Hud4SpanCopy || own) {
            if(hud4FrameBytes+s.bytes>Hud4FrameBudget){++hud4Stats.budget;return false;}
            if(used==op.spanBytes.size())op.spanBytes.emplace_back();
            auto& b=op.spanBytes[used++];b.assign(s.host,s.host+s.bytes);
            op.spans.push_back({s.address,s.bytes,b.data()});
            hud4FrameBytes+=s.bytes;hud4Stats.copiedKb+=s.bytes/1024;
        } else {op.spans.push_back(s);hud4Stats.inPlaceKb+=s.bytes/1024;}
    }
    return true;
}
[[gnu::noinline]] void hud4Draw(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
                                const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) noexcept {
    if(!in.stateBytes || !hud4Wanted())return;
    // (the common case first: RB_SURFACE_INFO's pitch at its fixed state offset; a main-frame draw outside the box phases returns here)
    const unsigned pitch=be32(in.stateBytes+Hud4SurfaceInfo)&0x3FFFu;
    if(pitch==hudrep::MainPitch)hud5StarDraw(in,fetch,fetchMask,spans,owned); // hud5 (v604): the wanted stars (a count compare when idle)
    if(pitch!=hudrep::RadarPitch && (pitch!=hudrep::MainPitch || (hud4C.phase!=hudrep::Phase::Resolved && hud4C.phase!=hudrep::Phase::Box)))return;
    if(!in.vertexIdentity || !in.vertexConstants || !in.pixelConstants)return;
    try {
        const StateView st{in.stateBytes,13860};
        const auto s=hudrep::surfaces(reg(st,0x2000),reg(st,0x2001),reg(st,0x2002));
        hudrep::Keep keep=hudrep::Keep::No;
        const auto rect=hudrep::drawScissor(in.scissor);
        if(hudrep::radarSurface(s))keep=hud4C.radarOp(false);
        else if(s.pitch==hudrep::MainPitch) {
            bool samples=false;
            if(hud4C.phase==hudrep::Phase::Resolved)for(unsigned k=0;k<32;++k)if((fetchMask>>k)&1u)
                if(const auto d=XenosTextureDescription::decode(fetch[k]);d && physicalAddress(d->baseAddress)==hud4C.radarAddress)samples=true;
            keep=hud4C.mainOp(rect,samples);
        }
        if(keep==hudrep::Keep::No)return;
        if(!hud7Copy)return; // (hud7 v620: a frame the lower screen does not replay: classified, not copied)
        if(!hud4CopyDraw((keep==hudrep::Keep::Radar?hud4Radar:hud4Overlay).next(),in,fetch,fetchMask,spans,owned,rect))hud4C.fail();
    } catch(...) {hud4C.fail();++hud4Stats.exceptions;}
}
// hud4's copy of one kept draw (hud5 v604: shared with the star copies).
bool hud4CopyDraw(Hud4Op& op,const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
                  const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned,const hudrep::Rect& rect) {
    {
        op.clear=false;op.rect=rect;
        op.state.assign(13860,0);for(const auto& [b,e]:Hud4StateRanges)std::memcpy(op.state.data()+b,in.stateBytes+b,e-b);
        op.vc=hud4Bank(in.vertexConstants,in.vertexConstantsVersion,hud4LastVc,hud4LastVcVersion);op.vcVersion=in.vertexConstantsVersion;
        op.pc=hud4Bank(in.pixelConstants,in.pixelConstantsVersion,hud4LastPc,hud4LastPcVersion);op.pcVersion=in.pixelConstantsVersion;
        op.fetch=fetch;op.fetchMask=fetchMask;op.scissor=in.scissor;
        op.identity=hud4CodeCopy(in.vertexIdentity,in.vertexIdentityVersion);
        // (draw() compares the effective code's address with the identity's: the same vector stays the same copy)
        op.effective=in.vertexEffective==in.vertexIdentity?op.identity:hud4CodeCopy(in.vertexEffective,in.vertexEffectiveVersion);
        op.pixel=hud4CodeCopy(in.pixelCode,in.pixelVersion);
        op.alternates=in.vertexAlternates?std::make_shared<const std::vector<std::vector<std::uint8_t>>>(*in.vertexAlternates):nullptr;
        op.identityVersion=in.vertexIdentityVersion;op.effectiveVersion=in.vertexEffectiveVersion;op.pixelVersion=in.pixelVersion;
        op.primitive=in.primitive;op.indicesFinal=in.indicesFinal;op.baseVertex=in.baseVertex;op.vertexCount=in.vertexCount;
        if(in.rawIndices && in.rawIndexCount) { // (a raw index list: converted here, as execute() does for draws that do not keep them raw)
            op.indices.resize(in.rawIndexCount);recomp::gpu::hot::bigEndianIndices(in.rawIndices,in.rawIndexCount,in.rawIndex32,op.indices.data());
        } else op.indices.assign(in.indices.begin(),in.indices.end());
        op.haveInline=in.inlineVertices!=nullptr;if(op.haveInline)op.inlineVertices=*in.inlineVertices;else op.inlineVertices.clear();
        op.inlineStride=in.inlineStride;op.source=in.source;
        return hud4CopySpans(op,spans,owned); // (false: over the frame's copy budget)
    }
}
[[gnu::noinline]] void hud4Clear(const ClearInput& in) noexcept {
    if(!hud4Wanted() || !in.state || in.state->size()<13860)return;
    try {
        const StateView st{*in.state};
        const auto s=hudrep::surfaces(reg(st,0x2000),reg(st,0x2001),reg(st,0x2002));
        float vp[4];for(unsigned i=0;i<4;++i){const auto v=field(st,13024+4*i);std::memcpy(&vp[i],&v,4);}
        const hudrep::Rect sc{std::int32_t(field(st,13052)),std::int32_t(field(st,13056)),std::int32_t(field(st,13060)),std::int32_t(field(st,13064))};
        const auto rect=hudrep::clearRect(vp,in.hasRect?std::optional<std::array<std::int32_t,4>>(in.rect):std::nullopt,!legacyClears && field(st,12288)!=0,sc);
        hudrep::Keep keep=hudrep::Keep::No;
        if(hudrep::radarSurface(s))keep=hud4C.radarOp((in.flags&0xFu) && rect.x0<=0 && rect.y0<=0 && rect.x1>=256 && rect.y1>=256);
        else if(s.pitch==hudrep::MainPitch){hud5S.other();keep=hud4C.mainOp(rect,false);} // (hud5: a clear ends a star run)
        if(keep==hudrep::Keep::No || !hud7Copy)return; // (hud7: copies only on replay frames)
        auto& op=(keep==hudrep::Keep::Radar?hud4Radar:hud4Overlay).next();
        op.clear=true;op.rect=rect;op.state=*in.state;op.clearIn=in;op.clearIn.state=nullptr; // (pointed at op.state when replayed)
        op.spans.clear();op.vc=op.pc=nullptr;op.identity=op.effective=op.pixel=nullptr;op.alternates=nullptr;
    } catch(...) {hud4C.fail();++hud4Stats.exceptions;}
}
// ---- hud5 (v604)
// The stars: a main-frame draw that StarCollector keeps (the five star shapes, then the lit stars' pairs) is copied like a box draw.
void hud5StarDraw(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask,
                  const std::vector<AsyncSpan>& spans,const std::vector<std::vector<std::uint8_t>>& owned) noexcept {
    const unsigned indices=in.rawIndices && in.rawIndexCount?in.rawIndexCount:unsigned(in.indices.size());
    if(!hud5S.open && indices!=hudrep::StarHeadIndices)return; // (every other draw while no run is open: this compare only)
    if(hud5Main.known) { // v614: another surface's draw is neither a star nor the end of a star run
        const StateView st{in.stateBytes,13860};const auto ci=reg(st,0x2001); // (RB_COLOR_INFO)
        if((ci&0xFFFu)!=hud5Main.color || targetFormat((ci>>16)&15u)!=hud5Main.format)return;
    }
    try {
        if(hud5S.draw(indices,in.primitive,hudrep::drawScissor(in.scissor))!=hudrep::StarKeep::Star)return;
        if(hud5S.ops==1)hud5Stars.n=0; // (a run starts: a second one in a frame replaces the first)
        if(!hud7Copy)return; // (hud7: copies only on replay frames)
        if(!in.vertexIdentity || !in.vertexConstants || !in.pixelConstants || hud5Stars.n>=hudrep::StarMaxOps){hud5S.overflow=true;return;}
        if(!hud4CopyDraw(hud5Stars.next(),in,fetch,fetchMask,spans,owned,hudrep::StarRegion))hud5S.overflow=true;
    } catch(...) {hud5S.overflow=true;++hud4Stats.exceptions;}
}
// "Bottom screen only" (files/settings/minimap.txt "bottom", hud2::Screen::mapMode): the operations hud4 keeps for the box are not
// drawn on the main frame. Asked before the operation executes; Collector::peekMain answers what mainOp will decide after it.
bool hud5Bottom() const noexcept {return recomp::gpu::hud2::screen().mapMode()==hudrep::MapMode::Bottom && hud4Wanted();}
bool hud12LookGame() const noexcept {return recomp::gpu::hud2::screen().lookGame() && hud4Wanted();}
// v638 (look=game): when this main-frame draw is the box's composite (the first draw sampling the resolved radar into a new box), copy the
// main frame's colour under backdropRegion(box) - the world the game composites the box over - into hud12Backdrop (host texels 1:1).
[[gnu::noinline]] void hud12Capture(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask) noexcept {
    try {
        bool samples=false;
        for(unsigned k=0;k<32;++k)if((fetchMask>>k)&1u)
            if(const auto d=XenosTextureDescription::decode(fetch[k]);d && physicalAddress(d->baseAddress)==hud4C.radarAddress)samples=true;
        const auto box=hudrep::drawScissor(in.scissor);
        if(!samples || hud4C.peekMain(box,true)!=hudrep::Keep::Overlay)return;
        const StateView st{in.stateBytes,13860};
        const unsigned pitch=reg(st,0x2000)&0x3FFFu,ci=reg(st,0x2001);
        const auto it=targets.find({ci&0xFFFu,targetFormat((ci>>16)&15),pitch,false});
        if(it==targets.end() || !it->second.image.image)return;
        const Image& src=it->second.image;
        const auto se=recomp::gpu::hud2::screen().extentHint();
        const auto g=hudrep::backdropRegion(box,se.width,se.height);
        const std::int32_t x0=hudrep::edge(g.x0,int(src.k)),y0=hudrep::edge(g.y0,int(src.k));
        const std::int32_t x1=std::min<std::int32_t>(hudrep::edge(g.x1,int(src.k)),std::int32_t(rsHostW(src))),y1=std::min<std::int32_t>(hudrep::edge(g.y1,int(src.k)),std::int32_t(rsHostH(src)));
        if(x1-x0<8 || y1-y0<8)return;
        const std::uint32_t w=std::uint32_t(x1-x0),h=std::uint32_t(y1-y0);
        if(!hud12Backdrop.image || hud12Backdrop.width!=w || hud12Backdrop.height!=h || hud12Backdrop.format!=src.format) {
            if(hud12Backdrop.image){endPass(__LINE__);destroyLater(hud12Backdrop);hud12Backdrop={};}
            const VkComponentMapping id{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
            if(!makeImage(w,h,1,src.format,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT,false,id,hud12Backdrop))
                {hud12Backdrop={};return;}
            endPass(__LINE__);toGeneral(hud12Backdrop);
        }
        endPass(__LINE__);hazards({src.image},{hud12Backdrop.image});barrier();
        VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        b.srcOffsets[0]={x0,y0,0};b.srcOffsets[1]={x1,y1,1};b.dstOffsets[1]={std::int32_t(w),std::int32_t(h),1};
        vkCmdBlitImage(cmd,src.image,VK_IMAGE_LAYOUT_GENERAL,hud12Backdrop.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_NEAREST);
        hazards({},{hud12Backdrop.image});afterWrite();
        hud12BdW=w;hud12BdH=h;hud12Captured=true;++hud12Captures;
    } catch(...) {}
}
[[gnu::noinline]] bool hud5SkipDraw(const DrawInput& in,const std::array<std::array<std::uint32_t,6>,32>& fetch,std::uint32_t fetchMask) noexcept {
    if(!in.stateBytes || (hud4C.phase!=hudrep::Phase::Resolved && hud4C.phase!=hudrep::Phase::Box))return false; // (the common case)
    if(hud4C.phase==hudrep::Phase::Resolved && (be32(in.stateBytes+Hud4SurfaceInfo)&0x3FFFu)==hudrep::MainPitch && hud12LookGame())
        hud12Capture(in,fetch,fetchMask); // (v638 look=game: before the composite draws, the world under the box)
    if((be32(in.stateBytes+Hud4SurfaceInfo)&0x3FFFu)!=hudrep::MainPitch || !hud5Bottom())return false;
    if(!in.vertexIdentity || !in.vertexConstants || !in.pixelConstants)return false; // (hud4Draw would not copy it: drawn)
    try {
        bool samples=false;
        if(hud4C.phase==hudrep::Phase::Resolved)for(unsigned k=0;k<32;++k)if((fetchMask>>k)&1u)
            if(const auto d=XenosTextureDescription::decode(fetch[k]);d && physicalAddress(d->baseAddress)==hud4C.radarAddress)samples=true;
        if(hud4C.peekMain(hudrep::drawScissor(in.scissor),samples)!=hudrep::Keep::Overlay)return false;
        ++hud5Stats.skippedDraws;return true;
    } catch(...) {return false;}
}
[[gnu::noinline]] bool hud5SkipClear(const ClearInput& in) noexcept {
    if(hud4C.phase!=hudrep::Phase::Box || !in.state || in.state->size()<13860 || !hud5Bottom())return false; // (a clear opens no box)
    try {
        const StateView st{*in.state};
        if((reg(st,0x2000)&0x3FFFu)!=hudrep::MainPitch)return false;
        float vp[4];for(unsigned i=0;i<4;++i){const auto v=field(st,13024+4*i);std::memcpy(&vp[i],&v,4);}
        const hudrep::Rect sc{std::int32_t(field(st,13052)),std::int32_t(field(st,13056)),std::int32_t(field(st,13060)),std::int32_t(field(st,13064))};
        const auto rect=hudrep::clearRect(vp,in.hasRect?std::optional<std::array<std::int32_t,4>>(in.rect):std::nullopt,!legacyClears && field(st,12288)!=0,sc);
        if(hud4C.peekMain(rect,false)!=hudrep::Keep::Overlay)return false;
        ++hud5Stats.skippedClears;return true;
    } catch(...) {return false;}
}
// The star band of the overlay: cleared with the box (hud4Replay's blank clear covers the whole overlay), then the star operations
// with StarRegion as their scissor, moved by whole tiles to (0, top). Called inside hud4Replay's redirect (main-frame surfaces).
bool hud5ReplayStars(std::uint32_t top) {
    for(unsigned i=0;i<hud5Stars.n;++i) { // v614: every star operation on the redirected surfaces, else no stars this frame (nothing drawn)
        const auto k=hud4Keys(hud5Stars.ops[i].state);
        if(k.pitch!=hud4Map[0].pitch || k.colorBase!=hud4Map[0].base || k.colorFormat!=hud4Map[0].format || k.depthBase!=hud4Map[1].base ||
           k.depthFormat!=hud4Map[1].format)return false;
    }
    for(unsigned i=0;i<hud5Stars.n;++i) {
        auto& op=hud5Stars.ops[i];
        op.scissor=std::array<std::uint32_t,3>{0u,std::uint32_t(hudrep::StarRegion.x0)|(std::uint32_t(hudrep::StarRegion.y0)<<16),
                                               std::uint32_t(hudrep::StarRegion.x1)|(std::uint32_t(hudrep::StarRegion.y1)<<16)};
        if(!hud4Op(op,hudrep::starOffsetX(),hudrep::starOffsetY(top)))return false;
    }
    return true;
}
// hud8b (v626): draw()'s call: while the radar list is replayed, every position moves hud8NdcShift rows down (clip space: rows / ys
// in NDC for a viewport-transformed draw, rows for a screen-space one); 0 = unchanged.
[[gnu::noinline]] void hud8ShiftHalfPixel(std::array<float,2>& halfPixel,float ys,bool screenSpace) const noexcept {
    if(!hud8NdcShift)return;
    halfPixel[1]+=hudrep::ndcShift(float(hud8NdcShift),ys,screenSpace);
}
// hud8 (v625): one radar-pass operation; those the game confines to the map rectangle or to the box region (0,0)-(190,135) get the taller
// region's scissor (rows 0..hud8Rows: the tiles the game submitted beyond), the water clear of the map rectangle covers it too. Nothing is
// moved: rows < 0 are outside the game's viewport (the clip volume drops them; v623's band).
bool hud7RadarOp(Hud4Op& op) {
    // (hud8b v626) every rectangle moves hud8Shift rows down with the positions (hud8ShiftHalfPixel); the map rectangle's and the box
    // region's scissor / clear become the tall region: rows -hud8Shift..135 of the game's target = 0..135+hud8Shift of the shadow
    const std::int32_t sh=hud8Shift;
    const hudrep::Rect tall{hudrep::RadarRegion.x0,0,hudrep::RadarRegion.x1,hudrep::RadarRegion.y1+sh};
    const auto down=[sh](hudrep::Rect r){r.y0+=sh;r.y1+=sh;return r;};
    if(op.clear) {
        if(op.rect==hudrep::RadarMap){op.rect=tall;
            if(!hud8ClearKnown){for(unsigned c=0;c<4;++c)hud8Clear[c]=std::uint8_t(std::lround(std::clamp(op.clearIn.color[c],0.0f,1.0f)*255.0f));hud8ClearKnown=true;}}
        else if(op.rect.y0<=0 && op.rect.y1>=256)op.rect.y1+=sh; // (the pass's full clear: still from row 0)
        else op.rect=down(op.rect);
    } else {
        if(!op.scissor)op.scissor=std::array<std::uint32_t,3>{0u,0u,512u|(512u<<16)};
        if((*op.scissor)[1]>>31)return false; // (a scissor without the window offset: not in the radar pass's census shape)
        const auto r=hudrep::drawScissor(op.scissor);
        const auto n=(r==hudrep::RadarMap || r==hudrep::RadarRegion)?tall:down(r);
        op.scissor=std::array<std::uint32_t,3>{(*op.scissor)[0],std::uint32_t(std::max(0,n.x0))|(std::uint32_t(std::clamp(n.y0,0,0x7FFF))<<16),
                                               std::uint32_t(std::max(0,n.x1))|(std::uint32_t(std::clamp(n.y1,0,0x7FFF))<<16)};
        if(hudrep::offset15((*op.scissor)[0]) || hudrep::offset15((*op.scissor)[0]>>16))return false; // (the game's own window offset: 0 here)
    }
    hud8NdcShift=sh;
    const bool ok=hud4Op(op,0,0);
    hud8NdcShift=0;
    return ok;
}
// hud8b: one read-back of the shadow's top rows (the rows above the game's target row 0 after the move) at the first replay; a later
// present counts how many of them hold the water clear's colour (RENDER_HUD8 top_rows: a high fraction = no tiles there).
void hud8ProbeRecord(int k) {
    if(hud8Probed || hud8ProbePending || !hud8Shift || !hud4RadarColor.image.image || hud4RadarColor.image.format!=VK_FORMAT_R8G8B8A8_UNORM)return;
    const std::int32_t x0=hudrep::edge(hudrep::RadarMap.x0,k),x1=hudrep::edge(hudrep::RadarMap.x1,k),y1=hudrep::edge(hud8Shift,k);
    const unsigned w=unsigned(std::min<std::int32_t>(x1,std::int32_t(rsHostW(hud4RadarColor.image)))-x0),h=unsigned(std::min<std::int32_t>(y1,std::int32_t(rsHostH(hud4RadarColor.image))));
    if(!w || !h){hud8Probed=true;return;}
    if(!hud8Probe.buffer && !makeBuffer(VkDeviceSize(w)*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,hud8Probe)){hud8Probed=true;return;}
    endPass(__LINE__);hazards({hud4RadarColor.image.image},{});barrier();
    VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageOffset={x0,0,0};c.imageExtent={w,h,1};
    vkCmdCopyImageToBuffer(cmd,hud4RadarColor.image.image,VK_IMAGE_LAYOUT_GENERAL,hud8Probe.buffer,1,&c);
    barrier();hud8ProbePending=true;hud8ProbeAt=hud5Presents;hud8ProbeW=w;hud8ProbeH=h;
}
void hud8ProbeRead() noexcept {
    if(!hud8ProbePending || hud5Presents<hud8ProbeAt+6)return;
    hud8ProbePending=false;hud8Probed=true;
    const auto* p=static_cast<const std::uint8_t*>(hud8Probe.mapped);std::uint64_t clear=0,empty=0,n=std::uint64_t(hud8ProbeW)*hud8ProbeH;
    for(std::uint64_t i=0;i<n;++i) {
        const std::uint8_t* t=p+i*4;bool same=hud8ClearKnown;
        for(unsigned c=0;c<3 && same;++c)same=std::abs(int(t[c])-int(hud8Clear[c]))<=2;
        clear+=same;empty+=t[3]==0;
    }
    GLOG("RENDER_HUD8 top_rows=%d texels=%llu clear_colour=%.3f transparent=%.3f (rows above the game's target row 0, moved into the shadow)",hud8Shift,
         (unsigned long long)n,n?double(clear)/double(n):0.0,n?double(empty)/double(n):0.0);
}
// One read-back of the first frame with stars: the star band's texels; a later present measures the row (alpha > 1/255), maps it to
// guest pixels (one pixel of margin) and makes it the crop the second screen shows. RENDER_HUD5 stars_bbox logs it.
void hud5ProbeRecord(const Target& overlay,std::uint32_t top,int k) {
    if(hud5Measured || hud5ProbePending || !overlay.image.image)return;
    const unsigned bytes=overlay.image.format==VK_FORMAT_R16G16B16A16_SFLOAT?8u:overlay.image.format==VK_FORMAT_R8G8B8A8_UNORM?4u:0u;
    if(!bytes){hud5Measured=true;GLOG("RENDER_HUD5 stars_bbox=none why=format format=%d",int(overlay.image.format));return;}
    const auto hb=hudrep::starHostCrop(hudrep::StarRegion,top,k);
    const unsigned w=unsigned(std::max(0,std::min<std::int32_t>(hb.x1,std::int32_t(rsHostW(overlay.image)))-hb.x0));
    const unsigned h=unsigned(std::max(0,std::min<std::int32_t>(hb.y1,std::int32_t(rsHostH(overlay.image)))-hb.y0));
    if(!w || !h){hud5Measured=true;return;}
    if(!hud5Probe.buffer && !makeBuffer(VkDeviceSize(w)*h*bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,hud5Probe)){hud5Measured=true;return;}
    if(hud5Probe.size<VkDeviceSize(w)*h*bytes){hud5Measured=true;return;}
    std::memset(hud5Probe.mapped,0xFF,std::size_t(w)*h*bytes); // (the sentinel: all-ones halves are NaN, which the replay never writes)
    endPass(__LINE__);hazards({overlay.image.image},{});barrier();
    VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageOffset={hb.x0,hb.y0,0};c.imageExtent={w,h,1};
    vkCmdCopyImageToBuffer(cmd,overlay.image.image,VK_IMAGE_LAYOUT_GENERAL,hud5Probe.buffer,1,&c);
    barrier();
    hud5ProbePending=true;hud5ProbeAt=hud5Presents;hud5ProbeBytes=bytes;hud5ProbeW=w;hud5ProbeH=h;hud5ProbeHost=hb;hud5ProbeK=k;
}
void hud5ProbeRead() noexcept {
    if(!hud5ProbePending || hud5Presents<hud5ProbeAt+6)return; // (several frames later: its command buffer has finished)
    const auto* p=static_cast<const std::uint8_t*>(hud5Probe.mapped);
    const unsigned w=hud5ProbeW,h=hud5ProbeH,b=hud5ProbeBytes;
    std::int32_t x0=std::int32_t(w),y0=std::int32_t(h),x1=-1,y1=-1;bool unwritten=false;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const std::uint8_t* t=p+(std::size_t(y)*w+x)*b;
        bool on;
        if(b==8){std::uint16_t a;std::memcpy(&a,t+6,2);std::uint64_t all;std::memcpy(&all,t,8);if(all==~0ull){unwritten=true;continue;}
            on=!(a&0x8000u) && a>0x1C04u;} // (a half above 1/255; the sign bit set = not positive)
        else on=t[3]>1; // (RGBA8: no sentinel can be told from white; the six presents' wait alone)
        if(on){x0=std::min<std::int32_t>(x0,x);y0=std::min<std::int32_t>(y0,y);x1=std::max<std::int32_t>(x1,x);y1=std::max<std::int32_t>(y1,y);}
    }
    if(unwritten && hud5Presents<hud5ProbeAt+120)return; // (not finished yet: asked again at the next present)
    hud5ProbePending=false;hud5Measured=true;
    if(unwritten || x1<0){GLOG("RENDER_HUD5 stars_bbox=none why=%s",unwritten?"unwritten":"empty");return;}
    // host texels of the band -> guest pixels (edge rule inverted, outward), one pixel of margin, inside StarRegion
    const int k=hud5ProbeK;
    const auto guest=[k](std::int32_t hostAbs,bool hi){return hi?std::int32_t((std::int64_t(hostAbs)*16+k-1)/k):std::int32_t((std::int64_t(hostAbs)*16)/k);};
    const std::int32_t top=hud5ProbeHost.y0; // (the band's first host row = StarRegion.y0's)
    hudrep::Rect g{hudrep::StarRegion.x0+guest(hud5ProbeHost.x0+x0,false)-guest(hud5ProbeHost.x0,false)-1,
                   hudrep::StarRegion.y0+guest(top+y0,false)-guest(top,false)-1,
                   hudrep::StarRegion.x0+guest(hud5ProbeHost.x0+x1+1,true)-guest(hud5ProbeHost.x0,false)+1,
                   hudrep::StarRegion.y0+guest(top+y1+1,true)-guest(top,false)+1};
    g.x0=std::max(g.x0,hudrep::StarRegion.x0);g.y0=std::max(g.y0,hudrep::StarRegion.y0);g.x1=std::min(g.x1,hudrep::StarRegion.x1);g.y1=std::min(g.y1,hudrep::StarRegion.y1);
    if(!g.empty())hud5Crop=g;
    GLOG("RENDER_HUD5 stars_bbox host=%d,%d..%d,%d of %ux%u guest=%d,%d..%d,%d k=%d",x0,y0,x1+1,y1+1,w,h,hud5Crop.x0,hud5Crop.y0,hud5Crop.x1,hud5Crop.y1,k);
}
// v585Radar: the radar resolve passed its checks (hud4C keeps the address the composite will sample).
[[gnu::noinline]] void hud4Resolved(std::uint32_t physical) noexcept {if(hud4Wanted())hud4C.radarResolved(physical);}

// ---- the replay
// A shadow target of guest size w x h at scale k in `format`: made (or remade when any of these differ); false = none.
bool hud4Shadow(Target& t,unsigned w,unsigned h,int k,VkFormat format,bool depth,unsigned base,unsigned xenosFormat,unsigned pitch,bool published) {
    if(t.image.image && t.image.width==w && t.image.height==h && t.image.k==k && t.image.format==format) {
        t.base=base;t.xenosFormat=xenosFormat;t.pitch=pitch;return true;
    }
    if(t.image.image) {
        dropFramebuffers(t.image.view);
        if(published)hud4Retired.push_back(t.image);else destroyLater(t.image); // (a published overlay: the second screen may sample it)
        t.image={};
    }
    t=Target{};
    const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
    const auto usage=VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
        (depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
    const auto aspect=depth?VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
    if(!makeScaled(k,w,h,1,format,usage,aspect,false,identity,t.image)){t.image={};return false;}
    endPass(__LINE__);toGeneral(t.image);hazards({},{t.image.image});
    if(depth){const VkClearDepthStencilValue v{1.0f,0};const VkImageSubresourceRange r{aspect,0,1,0,1};vkCmdClearDepthStencilImage(cmd,t.image.image,VK_IMAGE_LAYOUT_GENERAL,&v,1,&r);}
    else {const VkClearColorValue v{{0,0,0,0}};const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,t.image.image,VK_IMAGE_LAYOUT_GENERAL,&v,1,&r);}
    afterWrite();
    t.depth=depth;t.xenosFormat=xenosFormat;t.pitch=pitch;t.base=base;
    return true;
}
// The surface keys an operation's state names (as boundUncached builds them for target()).
struct Hud4Keys {unsigned pitch=0,colorBase=0,colorFormat=0,depthBase=0,depthFormat=0;};
Hud4Keys hud4Keys(const std::vector<std::uint8_t>& state) const {
    const StateView st{state};const auto ci=reg(st,0x2001),di=reg(st,0x2002);
    return {reg(st,0x2000)&0x3FFFu,ci&0xFFFu,targetFormat((ci>>16)&15),di&0xFFFu,16+((di>>16)&1)};
}
VkFormat hud4RealFormat(unsigned base,unsigned format,unsigned pitch,bool depth,VkFormat fallback) const {
    const auto it=targets.find({base,format,pitch,depth});
    return it!=targets.end() && it->second.image.image?it->second.image.format:fallback;
}
void hud4Redirect(const Hud4Keys& k,Target& color,Target& depth,unsigned w,unsigned h) {
    endPass(__LINE__);
    hud4Map[0]={k.colorBase,k.colorFormat,k.pitch,false,&color};hud4Map[1]={k.depthBase,k.depthFormat,k.pitch,true,&depth};
    hud4On=true;hud4W=w;hud4H=h;
#if !defined(RECOMP_HUD4_MUTANT_MEMO) // test mutant: bound()'s memo kept (it answers with the real surfaces of the same registers)
    ++targetsGeneration;boundMemo.valid=false; // (bound()'s memo may hold the real targets for the same registers)
#endif
}
void hud4RedirectOff() noexcept {
    if(!hud4On)return;
    try{endPass(__LINE__);}catch(...){}
    hud4On=false;hud4Map={};
#if !defined(RECOMP_HUD4_MUTANT_MEMO_OFF) // test mutant: bound()'s memo kept after the replay (it answers with a shadow)
    ++targetsGeneration;boundMemo.valid=false;
#endif
}
// One kept operation into the redirected targets; (ox, oy): the window offset that moves the box to the shadow's origin.
bool hud4Op(Hud4Op& op,std::int32_t ox,std::int32_t oy,bool absoluteScissor=false) { // (hud7: absoluteScissor keeps TL bit 31 set)
    if(op.clear) {
        ClearInput in=op.clearIn;in.state=&op.state;const StateView st{op.state};
        bool color=false,depth=false;
        for(unsigned i=0;i<4;++i)if((in.flags&(1u<<i)) && (legacyClears || field(st,12816+4*i))){if(i)return false;color=true;}
        depth=(in.flags&0x30)!=0 && (legacyClears || field(st,12832));
        hudrep::Rect r=op.rect;r.x0+=ox;r.x1+=ox;r.y0+=oy;r.y1+=oy;
        r.x0=std::clamp<std::int32_t>(r.x0,0,std::int32_t(hud4W));r.y0=std::clamp<std::int32_t>(r.y0,0,std::int32_t(hud4H));
        r.x1=std::clamp<std::int32_t>(r.x1,r.x0,std::int32_t(hud4W));r.y1=std::clamp<std::int32_t>(r.y1,r.y0,std::int32_t(hud4H));
        if(r.empty() || (!color && !depth))return true;
        Bound b;if(color)b.t[0]=hud4Map[0].to;if(depth)b.t[4]=hud4Map[1].to;
        b.count=(color?1u:0u)+(depth?1u:0u);b.width=hud4W;b.height=hud4H;b.k=hud4K;
        for(const auto* t:b.t)if(t)b.height=std::min(b.height,t->image.height);
        if(!ensurePass(b))return false;
        if(b.t[4] && !depthWriteAhead(b))return false;
        VkClearAttachment atts[2]{};std::uint32_t n=0;
        if(color){atts[n].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;atts[n].colorAttachment=0;for(unsigned c=0;c<4;++c)atts[n].clearValue.color.float32[c]=in.color[c];++n;}
        if(depth) {
            atts[n].aspectMask=((in.flags&0x10)?VK_IMAGE_ASPECT_DEPTH_BIT:0)|((in.flags&0x20)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
            atts[n].clearValue.depthStencil={std::clamp(in.depth,0.0f,1.0f),in.stencil&0xFF};
            if(dfFloat && (in.flags&0x10))atts[n].clearValue.depthStencil.depth=dfClearDepth(b.t[4],in.depth);
            ++n;
        }
        const VkClearRect rect{{{r.x0,r.y0},{std::uint32_t(r.x1-r.x0),std::uint32_t(r.y1-r.y0)}},0,1};
        rsClearAttachments(cmd,n,atts,1,&rect);
        for(const auto* t:b.t)noteWrite(t,r.x0,r.y0,r.x1,r.y1);
        ++hud4Stats.clears;return true;
    }
    std::optional<std::array<std::uint32_t,3>> scissor=op.scissor;
    if(ox || oy) {
        if(!scissor || hudrep::offset15((*scissor)[0]) || hudrep::offset15((*scissor)[0]>>16))return false; // (the game's own offset is 0 here)
        (*scissor)[0]=hudrep::offsetWord(ox,oy);if(!absoluteScissor)(*scissor)[1]&=0x7FFFFFFFu; // (the scissor moves with the offset)
        if(const auto at=gta5DrawRegisterOffset(0x2205)) { // the vertices too (PA_SU_SC_MODE_CNTL bit 16)
            auto* p=op.state.data()+*at;const std::uint32_t v=be32(p)|(1u<<16);p[0]=std::uint8_t(v>>24);p[1]=std::uint8_t(v>>16);p[2]=std::uint8_t(v>>8);p[3]=std::uint8_t(v);
        } else return false;
    }
    DrawInput in;
    in.stateBytes=op.state.data();in.scissor=scissor;in.vertexConstants=op.vc.get();in.pixelConstants=op.pc.get();
    in.vertexConstantsVersion=op.vcVersion;in.pixelConstantsVersion=op.pcVersion;
    in.fetch=[&op](unsigned slot)->std::optional<std::array<std::uint32_t,6>> {
        if(slot<32 && ((op.fetchMask>>slot)&1u))return op.fetch[slot];
        return std::nullopt;};
    in.vertexIdentity=op.identity.get();in.vertexEffective=op.effective.get();in.vertexAlternates=op.alternates.get();in.pixelCode=op.pixel.get();
    in.vertexIdentityVersion=op.identityVersion;in.vertexEffectiveVersion=op.effectiveVersion;in.pixelVersion=op.pixelVersion;
    in.primitive=op.primitive;in.indices.swap(op.indices);in.indicesFinal=op.indicesFinal;in.baseVertex=op.baseVertex;in.vertexCount=op.vertexCount;
    in.inlineVertices=op.haveInline?&op.inlineVertices:nullptr;in.inlineStride=op.inlineStride;in.source=op.source;
    asyncSpans=&op.spans;
    const char* why=nullptr;bool ok=false;
    try {ok=draw(in,hud4Reader,&why);} catch(...) {in.indices.swap(op.indices);asyncSpans=nullptr;throw;}
    in.indices.swap(op.indices);asyncSpans=nullptr;
    ++hud4Stats.draws;
    if(!ok){++hud4Stats.refused;if(!hud4Stats.firstRefusal)hud4Stats.firstRefusal=why?why:"?";}
    return true;
}
// The resolved radar's entry shows the shadow radar (swapped in) / its own image again (swapped back: the same call).
void hud4SwapResolved(Resolved& r) noexcept {
    std::swap(r.image,hud4Swap.image);std::swap(r.width,hud4Swap.width);std::swap(r.height,hud4Swap.height);std::swap(r.view,hud4Swap.view);
    std::swap(r.shapeViews,hud4Swap.shapeViews);std::swap(r.remaps,hud4Swap.remaps);std::swap(r.packedValid,hud4Swap.packedValid);
    std::swap(r.levelsWritten,hud4Swap.levelsWritten);std::swap(r.mode,hud4Swap.mode);std::swap(r.bakedExp,hud4Swap.bakedExp);
    std::swap(r.storage,hud4Swap.storage);std::swap(r.layers,hud4Swap.layers);std::swap(r.levels,hud4Swap.levels);std::swap(r.cube,hud4Swap.cube);
    ++resolveEpoch;++v363ResolvedSetGen; // (every binding of a resolved texture is verified again; the key set's memos are rebuilt)
}
// hud4Swap: the shadow radar as the real entry's twin (its swizzle view made once per view tuple).
bool hud4PrepareSwap(const Resolved& real) {
    if(hud4SwapView!=real.view || !hud4Swap.image.view || hud4Swap.image.image!=hud4RadarColor.image.image) {
        if(hud4Swap.image.view && hud4Swap.image.view!=hud4RadarColor.image.view)viewLater(hud4Swap.image.view);
        for(auto& v:hud4Swap.shapeViews)if(v.owned)viewLater(v.image.view);
        hud4Swap.shapeViews.clear();
        Image im=hud4RadarColor.image;im.memory=VK_NULL_HANDLE;im.bytes=0;im.view=VK_NULL_HANDLE;
        VkImageViewCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;c.image=im.image;c.viewType=VK_IMAGE_VIEW_TYPE_2D;c.format=im.format;
        c.components={component(real.view[0]),component(real.view[1]),component(real.view[2]),component(real.view[3])};
        c.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(!makeView(c,im.view))return false;
        hud4Swap.image=im;hud4SwapView=real.view;
    }
    hud4Swap.width=real.width;hud4Swap.height=real.height;hud4Swap.view=real.view;hud4Swap.remaps.clear();hud4Swap.packedValid=false;
    hud4Swap.levelsWritten=1;hud4Swap.mode=0;hud4Swap.bakedExp=0;hud4Swap.storage=false;hud4Swap.layers=1;hud4Swap.levels=1;hud4Swap.cube=false;
    return true;
}
void hud4Replay() {
    const auto t0=Clock::now();
    if(!hud4Viewer)hud4Init();
    if(ocq::engine().counting()){++hud4Stats.ocq;return;} // (an open occlusion interval would count the replay's draws)
    if(!begin()){++hud4Stats.skipped;return;}
    const auto& radar0=hud4Radar.ops[0];const auto& comp=hud4Overlay.ops[0];
    const auto rk=hud4Keys(radar0.state),mk=hud4Keys(comp.state);
    hud5Main={mk.pitch,mk.colorBase,mk.colorFormat,true}; // v614: the main frame's colour surface (stars are drawn there)
    const auto it=resolved.find(hud4C.radarAddress);
    if(it==resolved.end() || !it->second.image.image || it->second.mode || it->second.levels!=1 || it->second.layers!=1 || it->second.cube ||
       it->second.image.k!=rscale::One || it->second.width<16 || it->second.height<16){++hud4Stats.noResolved;return;}
    Resolved& real=it->second;
    // geometry: the radar pass's reach, the box's overlay, the scale (chosen once)
    // (2026-11-03, the real Duo: the map stayed 1240x1080-sized in a 2007x1428 lower screen) the scale was chosen once, from the hint's
    // 1240x1080 default before the second screen's swapchain existed; now it is chosen again whenever the hint changes (the shadow images
    // follow k: hud4Shadow re-creates them for a new k, hud7BoxK resets).
    {static std::uint32_t hud4HintW=0,hud4HintH=0;const auto e=recomp::gpu::hud2::screen().extentHint();
     if(!hud4Scale || e.width!=hud4HintW || e.height!=hud4HintH){hud4HintW=e.width;hud4HintH=e.height;
        hud4Scale=hudrep::scaleFor13(e.width,e.height);hud8Rows=hudrep::shownRows(e.height,hud4Scale);
        hud8Shift=hudrep::radarShift(e.height,hud4Scale);}} // (hud13 v638: the box aligned, its bottom at the screen's bottom: every row above it from above row 0) // (first: the rows set the reach below; hud8 v625: the box region's width, rows from 0)
    std::int32_t reachX=0,reachY=0;
    for(unsigned i=0;i<hud4Radar.n;++i){const auto& op=hud4Radar.ops[i];const auto& r=op.rect;
        if(op.clear && r.x0<=0 && r.y0<=0 && r.x1>=256 && r.y1>=256)continue; // (v623: the pass's full clear is clipped to the shadow; it sets no reach)
        reachX=std::max(reachX,std::min(r.x1,std::int32_t(real.width)));reachY=std::max(reachY,std::min(r.y1,std::int32_t(real.height)));}
    reachX=std::max(reachX,hudrep::RadarRegion.x1);reachY=std::max(reachY,hudrep::RadarRegion.y1)+hud8Shift; // (hud8b: the box region moved down)
    {const auto e11=recomp::gpu::hud2::screen().extentHint();reachY=std::max(reachY,hudrep::hud11Reach(e11.height,hud4Scale,hud8Shift));} // (hud11 v635: the map below the box)
    const unsigned rw=hudrep::radarArea(reachX,real.width),rh=hudrep::radarArea(reachY,real.height);
    const auto box=hud4C.box;const auto ov=hudrep::overlayFor(box);
    if(!rw || !rh || !ov.w || !ov.h || ov.w>1024 || ov.h>1024){++hud4Stats.skipped;return;}
    const int k=hud4Scale;
    // shadows (formats of the real surfaces, so that every pipeline is the one the game's draws use)
    const VkFormat rcf=real.image.format,rdf=hud4RealFormat(rk.depthBase,rk.depthFormat,rk.pitch,true,depthFormat);
    const VkFormat ocf=hud4RealFormat(mk.colorBase,mk.colorFormat,mk.pitch,false,colorFormat(mk.colorFormat));
    const VkFormat odf=hud4RealFormat(mk.depthBase,mk.depthFormat,mk.pitch,true,depthFormat);
    // (hud5: the overlay also holds the star band, rows ov.h .. ov.h + StarRegion's height)
    // v623 (thor-wheel; v622 low-memory kills): the box + star part grows only by itself. v620 took max(.., the colour shadow's size), but
    // the colour shadow is owC x ohC (this part + the map band), so every replay added the band again: a new, larger overlay per replay,
    // the old one kept in hud4Retired (a published image is never freed) - about one leaked image per frame.
    if(hud7BoxK!=k){hud7BoxK=k;hud7BoxW=hud7BoxH=0;}
    hud7BoxW=std::max(hud7BoxW,hudrep::overlayWidthWithStars(ov));hud7BoxH=std::max(hud7BoxH,(hudrep::Hud11FrameBand || hudrep::Hud15FlashBand)?hudrep::overlayHeightWithFrame(ov):hudrep::overlayHeightWithStars(ov)); // (hud11 v635: + the frame band; hud12: none)
    const unsigned ow=hud7BoxW,oh=hud7BoxH; // (grow only)
    const std::uint32_t starTop=hudrep::starBandTop(ov);bool starsOk=false;
    // hud7 (v620): the map band below the star band holds the lower screen's map (blitted from the radar shadow): screen-sized
    const auto se=recomp::gpu::hud2::screen().extentHint();
    const std::uint32_t bandTop=std::uint32_t(hudrep::edge(std::int32_t(oh),k));
    const unsigned owC=std::max(ow,hudrep::roundUp16(std::int32_t((std::int64_t(se.width)*16+k-1)/k)+1));
    const unsigned ohC0=oh+hudrep::roundUp16(std::int32_t((std::int64_t(se.height)*16+k-1)/k)+1);
    // v638 (look=game, only then): room below the map band for this frame's backdrop (its host texels at this scale's guest rows)
    const bool backdrop=hud12LookGame() && hud12Captured && hud12Backdrop.image;
    const unsigned ohC=backdrop?ohC0+hudrep::roundUp16(std::int32_t((std::int64_t(hud12BdH)*16+k-1)/k)+1):ohC0;
    if(!hud4Shadow(hud4RadarColor,rw,rh,k,rcf,false,rk.colorBase,rk.colorFormat,rk.pitch,false) || // (hud7: the drawn area only)
       !hud4Shadow(hud4RadarDepth,rw,rh,k,rdf,true,rk.depthBase,rk.depthFormat,rk.pitch,false) ||
       !hud4Shadow(hud4OverlayColor,std::max(owC,hud4OverlayColor.image.image && hud4OverlayColor.image.k==k?hud4OverlayColor.image.width:0u),
                   std::max(ohC,hud4OverlayColor.image.image && hud4OverlayColor.image.k==k?hud4OverlayColor.image.height:0u),k,ocf,false,mk.colorBase,mk.colorFormat,mk.pitch,true) ||
       !hud4Shadow(hud4OverlayDepth,ow,oh,k,odf,true,mk.depthBase,mk.depthFormat,mk.pitch,false)){++hud4Stats.skipped;return;}
    if(!hud4Pool){VkQueryPoolCreateInfo q{};q.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=8;
        if(vkCreateQueryPool(d.device,&q,nullptr,&hud4Pool)!=VK_SUCCESS)hud4Pool=VK_NULL_HANDLE;}
    // GPU time of the earlier replays (not waited for: a result not ready yet stays pending)
    if(hud4Pool)for(unsigned s=0;s<4;++s)if(hud4PoolPending[s]) {
        std::uint64_t t[2]{};
        if(vkGetQueryPoolResults(d.device,hud4Pool,2*s,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            const auto ns=std::uint64_t(double(((t[1]&timestampMask)-(t[0]&timestampMask))&timestampMask)*timestampPeriod);
            hud4Stats.gpuNs+=ns;hud4Stats.gpuNsMax=std::max(hud4Stats.gpuNsMax,ns);++hud4Stats.gpuN;hud4PoolPending[s]=false;
        }
    }
    const unsigned slot=hud4PoolSlot++%4;
    const bool timed=hud4Pool && !hud4PoolPending[slot] && timestampPeriod>0;
    endPass(__LINE__);
    if(timed){vkCmdResetQueryPool(cmd,hud4Pool,2*slot,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,hud4Pool,2*slot);}
    hud4SavedViewer=viewer;viewer=hud4Viewer;hud4K=k;hud4Unexpected=false;
    bool swapped=false;
    const auto restore=[&]{
        hud4RedirectOff();
#if !defined(RECOMP_HUD4_MUTANT_NO_SWAP_BACK) // test mutant: the resolved radar's entry keeps showing the shadow
        if(swapped){hud4SwapResolved(real);swapped=false;}
#endif
        viewer=hud4SavedViewer;asyncSpans=nullptr;
    };
    bool ok=true;
    try {
        // 1. the radar pass moved down hud8Shift rows in clip space, its map rectangle and box region drawn taller (hud8b v626)
        hud4Redirect(rk,hud4RadarColor,hud4RadarDepth,rw,rh);
        for(unsigned i=0;i<hud4Radar.n && ok;++i)ok=hud7RadarOp(hud4Radar.ops[i]);
        // 2. the box: the overlay cleared (no world under it), the box's draws WITHOUT the composite (op 0: the lower screen shows the radar
        //    texture itself at full strength; the main frame's composite is not changed)
        hud4RedirectOff();
        hud4Redirect(mk,hud4OverlayColor,hud4OverlayDepth,ow,oh);
        {Hud4Op blank;blank.clear=true;blank.state=comp.state;blank.clearIn.flags=0x31;blank.clearIn.depth=1.0f;blank.clearIn.stencil=0;
         blank.rect={-ov.ox,-ov.oy,std::int32_t(ow)-ov.ox,std::int32_t(oh)-ov.oy};
         const StateView st{blank.state};
         if(!(legacyClears || field(st,12816)) || !(legacyClears || field(st,12832))) { // (the slots the clear needs: set on the copy)
             const auto put=[&](unsigned at,std::uint32_t v){auto* p=blank.state.data()+at;p[0]=std::uint8_t(v>>24);p[1]=std::uint8_t(v>>16);p[2]=std::uint8_t(v>>8);p[3]=std::uint8_t(v);};
             put(12816,1);put(12832,1);
         }
         ok=hud4Op(blank,ov.ox,ov.oy);}
        hudrep::FrameFilter frame; // (hud9 v631: the frame's fill and ring are not drawn in the box layer)
        unsigned ringAt=0; // (hud11 v635: the op that drew the frame's ring; the ops 1..ringAt = the game's frame, in its order)
        unsigned maskAt=0,slotAt=0; // (hud15: the stencil mask and the fill slot after it)
        std::array<unsigned,4> flashAt{};unsigned flashes=0; // (hud15 v698: the overlays that look like the damage flash, in order)
        hud14Cover=true; // v649: the box layer's alpha accumulates as coverage
        for(unsigned i=1;i<hud4Overlay.n && ok;++i) {
            const auto& op=hud4Overlay.ops[i];
            const bool hadMask=frame.mask,hadSlot=frame.slot;
            const auto role=frame.role(op.clear?0:op.indices.size(),op.clear);
            if(!hadMask && frame.mask)maskAt=i;
            if(!hadSlot && frame.slot)slotAt=i;
            if(role==hudrep::BoxRole::Keep && hudrep::Hud15FlashBand && flashes<flashAt.size() && hud15IsFlash(op)){flashAt[flashes++]=i;continue;} // (drawn alone below)
            if(role==hudrep::BoxRole::Drop){if(frame.ring && !ringAt)ringAt=i;continue;}
            ok=hud4Op(hud4Overlay.ops[i],ov.ox,ov.oy);
        }
        // hud15 (v695/v698): the flash alone in the flash band (the frame band: frameBandTop rows below the box layer), with what it tests:
        // the box's ops up to its stencil mask (the depth/stencil clear, the mask), at the same place moved down; the lower screen
        // stretches the band's map rectangle to its map area. A refusal here leaves the box layer as drawn and shows no flash.
        hud15Flash=false;
        if(hudrep::Hud15FlashBand && ok && flashes && maskAt && maskAt<flashAt[0]) {
            const std::int32_t fy=std::int32_t(hudrep::frameBandTop(ov));
            bool fok=true;
            for(unsigned i=1;i<=maskAt && fok;++i)fok=hud4Op(hud4Overlay.ops[i],ov.ox,ov.oy+fy);
            for(unsigned f=0;f<flashes && fok;++f)fok=hud4Op(hud4Overlay.ops[flashAt[f]],ov.ox,ov.oy+fy);
            hud15Flash=fok;if(fok)++hud15Shown;else ++hud15Refused;
        }
        hud14Cover=false;
        // hud11 (v635): the game's frame again, alone, in the frame band below the star band: the box's ops up to and including the ring
        // (its clear and first fill set what the frame's draws test), at the same place moved down frameBandTop rows
        hud11Frame=false;
        if(hudrep::Hud11FrameBand && ok && ringAt && frame.fill) { // (hud12 v637: off - the lower screen draws its own thin frame)
            const std::int32_t fy=std::int32_t(hudrep::frameBandTop(ov));
            for(unsigned i=1;i<=ringAt && ok;++i)ok=hud4Op(hud4Overlay.ops[i],ov.ox,ov.oy+fy);
            hud11Frame=ok;
        }
        // 3. hud5 (v604): the game's star draws into the band below the box (cleared above with the whole overlay)
        if(ok && hud5FrameStars && hud5Stars.n==hud5FrameStars)starsOk=hud5ReplayStars(starTop);
        restore();
    } catch(...) {hud14Cover=false;restore();++hud4Stats.exceptions;ok=false;}
    if(timed){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,hud4Pool,2*slot+1);hud4PoolPending[slot]=true;}
    if(hud4Scratch.image.image){dropFramebuffers(hud4Scratch.image.view);destroyLater(hud4Scratch.image);hud4Scratch=Target{};}
    if(hud4Unexpected){++hud4Stats.unexpected;ok=false;}
    const auto ns=nsSince(t0);hud4Stats.cpuNs+=ns;hud4Stats.cpuNsMax=std::max(hud4Stats.cpuNsMax,ns);
    hud4Stats.radarOps+=hud4Radar.n;hud4Stats.overlayOps+=hud4Overlay.n;
    if(!ok){++hud4Stats.broken;return;}
    ++hud4Stats.replays;
    if(hud5FrameStars){++hud5Stats.starFrames;hud5Stats.starOps+=hud5FrameStars;if(!starsOk)++hud5Stats.starBroken;}
    if(starsOk)hud5ProbeRecord(hud4OverlayColor,starTop,k);
    hud8ProbeRecord(k); // hud8b
    // hud7: the screen's crop of the radar's (extended) map into the overlay's map band, 1:1 (nearest): the map at full strength
    const auto mc=hudrep::mapCropBottom(se.width,se.height,k,hud8Shift); // (hud13 v638: the box's bottom = the screen's bottom, aligned)
    const std::int32_t cx0=mc.cx0,cy0=mc.cy0,bw=mc.w,bh=mc.h;
    if(bw<=0 || bh<=0 || std::uint32_t(bw)>rsHostW(hud4OverlayColor.image) || bandTop+std::uint32_t(bh)>rsHostH(hud4OverlayColor.image) ||
       std::uint32_t(cx0+bw)>rsHostW(hud4RadarColor.image) || std::uint32_t(cy0+bh)>rsHostH(hud4RadarColor.image)){++hud4Stats.skipped;return;}
    endPass(__LINE__);hazards({hud4RadarColor.image.image},{hud4OverlayColor.image.image});barrier();
    {VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
     b.srcOffsets[0]={cx0,cy0,0};b.srcOffsets[1]={cx0+bw,cy0+bh,1};b.dstOffsets[0]={0,std::int32_t(bandTop),0};b.dstOffsets[1]={bw,std::int32_t(bandTop)+bh,1};
     vkCmdBlitImage(cmd,hud4RadarColor.image.image,VK_IMAGE_LAYOUT_GENERAL,hud4OverlayColor.image.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_NEAREST);}
    hazards({},{hud4OverlayColor.image.image});afterWrite();
    // v638 (look=game): this frame's backdrop below the map band, 1:1
    bool backdropOk=false;const std::uint32_t bdTop=bandTop+std::uint32_t(bh);
    if(backdrop && hud12BdW<=rsHostW(hud4OverlayColor.image) && bdTop+hud12BdH<=rsHostH(hud4OverlayColor.image)) {
        endPass(__LINE__);hazards({hud12Backdrop.image},{hud4OverlayColor.image.image});barrier();
        VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        b.srcOffsets[1]={std::int32_t(hud12BdW),std::int32_t(hud12BdH),1};b.dstOffsets[0]={0,std::int32_t(bdTop),0};
        b.dstOffsets[1]={std::int32_t(hud12BdW),std::int32_t(bdTop+hud12BdH),1};
        vkCmdBlitImage(cmd,hud12Backdrop.image,VK_IMAGE_LAYOUT_GENERAL,hud4OverlayColor.image.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_NEAREST);
        hazards({},{hud4OverlayColor.image.image});afterWrite();backdropOk=true;++hud12Published;
        if(!hud12Logged){hud12Logged=true;GLOG("RENDER_HUD12 look=game first=1 backdrop=%ux%u at 0,%u captures=%llu",hud12BdW,hud12BdH,bdTop,(unsigned long long)hud12Captures);}
    }
    hud12Captured=false;
    const auto hb=hudrep::hostBox(box,ov,k);
    recomp::gpu::hud2::Replica rep;
    rep.image=hud4OverlayColor.image.image;rep.view=hud4OverlayColor.image.view;rep.format=hud4OverlayColor.image.format;
    rep.width=rsHostW(hud4OverlayColor.image);rep.height=rsHostH(hud4OverlayColor.image);
    rep.box[0]=float(hb.x0);rep.box[1]=float(hb.y0);rep.box[2]=float(hb.x1);rep.box[3]=float(hb.y1);rep.k=k;rep.serial=++hud4Serial;
    { // hud5: the stars of this frame (none drawn: none shown) and the crop shown, in the overlay's host texels
        const auto sc=hudrep::starHostCrop(hud5Crop,starTop,k);
        // hud7: the map band (screen-sized texels)
        rep.map[0]=0;rep.map[1]=float(bandTop);rep.map[2]=float(bw);rep.map[3]=float(bandTop+std::uint32_t(bh));
        // hud8: the box layer = the whole box (the radar region's place on the main frame: (box.x0+1, box.y0+1) + 190 x 135), in place on the map
        const auto bl=hudrep::hostRect(hudrep::Rect{box.x0+1,box.y0+1,box.x0+1+hudrep::RadarRegion.x1,box.y0+1+hudrep::RadarRegion.y1},ov,k);
        rep.interior[0]=float(bl.x0);rep.interior[1]=float(bl.y0);rep.interior[2]=float(bl.x0+mc.bw);rep.interior[3]=float(bl.y0+mc.bh);
        rep.interiorAt[0]=float(mc.atX);rep.interiorAt[1]=float(mc.atY);
        rep.bars[0]=rep.bars[1]=rep.bars[2]=rep.bars[3]=0; // (hud8: the bars are drawn in place, inside the box layer)
        if(backdropOk){rep.backdrop[0]=0;rep.backdrop[1]=float(bdTop);rep.backdrop[2]=float(hud12BdW);rep.backdrop[3]=float(bdTop+hud12BdH);} // v638
        // hud11 (v635): the frame band (the radar region's texels, frameBandTop rows below the box layer); none = no frame shown
        if(hud11Frame){const auto fb=hudrep::hostRect(hudrep::Rect{box.x0+1,box.y0+1+std::int32_t(hudrep::frameBandTop(ov)),box.x0+1+hudrep::RadarRegion.x1,
                                                                   box.y0+1+std::int32_t(hudrep::frameBandTop(ov))+hudrep::RadarRegion.y1},ov,k);
            rep.frame[0]=float(fb.x0);rep.frame[1]=float(fb.y0);rep.frame[2]=float(fb.x0+mc.bw);rep.frame[3]=float(fb.y0+mc.bh);}
        // hud15 (v695): the flash band's map rectangle (the part the stencil mask lets the flash cover), in host texels; none = no flash
        if(hud15Flash){const std::int32_t fy=std::int32_t(hudrep::frameBandTop(ov));
            const auto fm=hudrep::hostRect(hudrep::Rect{box.x0+1+hudrep::RadarMap.x0,box.y0+1+fy+hudrep::RadarMap.y0,box.x0+1+hudrep::RadarMap.x1,
                                                        box.y0+1+fy+hudrep::RadarMap.y1},ov,k);
            rep.flash[0]=float(fm.x0);rep.flash[1]=float(fm.y0);rep.flash[2]=float(fm.x1);rep.flash[3]=float(fm.y1);}
        rep.stars=starsOk;rep.starBox[0]=float(sc.x0);rep.starBox[1]=float(sc.y0);rep.starBox[2]=float(sc.x1);rep.starBox[3]=float(sc.y1);
        if(starsOk && !hud5Logged){hud5Logged=true;GLOG("RENDER_HUD5 stars first=1 ops=%u top=%u crop=%d,%d..%d,%d host=%d,%d..%d,%d k=%d",hud5FrameStars,starTop,
            hud5Crop.x0,hud5Crop.y0,hud5Crop.x1,hud5Crop.y1,sc.x0,sc.y0,sc.x1,sc.y1,k);}
    }
    recomp::gpu::hud2::screen().publishReplica(rep);
    if(!hud7Logged){hud7Logged=true;GLOG("RENDER_HUD7 first=1 k=%d shift=%d radar_shadow=%ux%u overlay=%ux%u band_top=%u crop=%d,%d+%dx%d interior_at=%d,%d rate=%u",
        k,hud8Shift,rsHostW(hud4RadarColor.image),rsHostH(hud4RadarColor.image),rsHostW(hud4OverlayColor.image),rsHostH(hud4OverlayColor.image),bandTop,cx0,cy0,bw,bh,
        mc.atX,mc.atY,recomp::gpu::hud2::screen().replayRate());}
    if(!hud4Logged){hud4Logged=true;
        GLOG("RENDER_HUD4 first=1 k=%d radar=%ux%u@%08x area=%ux%u box=%d,%d..%d,%d overlay=%ux%u offset=%d,%d host_box=%d,%d..%d,%d formats=%d/%d/%d/%d "
             "radar_ops=%u box_ops=%u",k,real.width,real.height,hud4C.radarAddress,rw,rh,box.x0,box.y0,box.x1,box.y1,ow,oh,ov.ox,ov.oy,hb.x0,hb.y0,hb.x1,hb.y1,
             int(rcf),int(rdf),int(ocf),int(odf),hud4Radar.n,hud4Overlay.n);}
}
[[gnu::noinline]] void hud4Present() noexcept {
    const bool replay=hud4C.present() && hud7Copy; // (hud7 v620: only frames whose operations were copied)
    hud8ProbeRead(); // hud8b
    if(!hud7Copy)++hud7Skipped;
    hud7Copy=(++hud7Frames%std::max(1u,recomp::gpu::hud2::screen().replayRate()))==0; // (the next frame: copied and replayed or not)
    hud5FrameStars=hud5S.present();++hud5Presents;hud5ProbeRead(); // hud5 (v604)
    try {
        if(replay && hud4Wanted() && hud4Radar.n && hud4Overlay.n)hud4Replay();
    } catch(...) {++hud4Stats.exceptions;}
    hud4Radar.n=hud4Overlay.n=hud5Stars.n=0;hud4FrameBytes=0;hud4LastVc=hud4LastPc=nullptr;hud4LastVcVersion=hud4LastPcVersion=0;
    const auto& s=hud4Stats;
    if(s.replays+s.broken+s.skipped && !((s.replays+s.broken+s.skipped)%256) && replay) {
        const double n=double(std::max<std::uint64_t>(1,s.replays+s.broken));
        GLOG("RENDER_HUD4 replays=%llu broken=%llu skipped=%llu refused=%llu first_refusal=%s draws=%llu clears=%llu radar_ops=%.1f box_ops=%.1f cpu_us=%.1f cpu_us_max=%.1f "
             "gpu_us=%.1f gpu_us_max=%.1f copied_kb=%.0f in_place_kb=%.0f ocq=%llu budget=%llu unexpected=%llu no_resolved=%llu exceptions=%llu k=%d "
             "star_frames=%llu star_ops=%llu star_broken=%llu skipped_draws=%llu skipped_clears=%llu mode=%u",
             (unsigned long long)s.replays,(unsigned long long)s.broken,(unsigned long long)s.skipped,(unsigned long long)s.refused,s.firstRefusal?s.firstRefusal:"-",
             (unsigned long long)s.draws,(unsigned long long)s.clears,double(s.radarOps)/n,double(s.overlayOps)/n,double(s.cpuNs)/1e3/n,double(s.cpuNsMax)/1e3,
             s.gpuN?double(s.gpuNs)/1e3/double(s.gpuN):0.0,double(s.gpuNsMax)/1e3,double(s.copiedKb)/n,double(s.inPlaceKb)/n,(unsigned long long)s.ocq,
             (unsigned long long)s.budget,(unsigned long long)s.unexpected,(unsigned long long)s.noResolved,(unsigned long long)s.exceptions,hud4Scale,
             (unsigned long long)hud5Stats.starFrames,(unsigned long long)hud5Stats.starOps,(unsigned long long)hud5Stats.starBroken,
             (unsigned long long)hud5Stats.skippedDraws,(unsigned long long)hud5Stats.skippedClears,unsigned(recomp::gpu::hud2::screen().mapMode()));
        hud4Stats.cpuNsMax=hud4Stats.gpuNsMax=0;
    }
}
void hud4Init() {
    hud4Viewer=[this](std::uint32_t a,std::uint32_t n){return asyncView(a,n);};
    hud4Reader=[this](std::uint32_t a,std::uint32_t n)->std::optional<std::vector<std::uint8_t>> {
        if(const auto* v=asyncView(a,n))return std::vector<std::uint8_t>(v,v+n);
        return std::nullopt;};
}
