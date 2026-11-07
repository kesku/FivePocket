#pragma once
// Attempt v16, DIAGNOSTIC BUILDS ONLY (RECOMP_DIAGNOSTIC_GENERIC_RENDER): one generic renderer for
// every draw, clear, resolve and present, using the offline XenosRecomp translations of the
// game's own shaders (tools/iter/generic_shader_pack.py, descriptor32 ABI). The user asked to see
// the frames the game renders "correct or wrong": render state is mapped from the Xenos register
// shadow as far as it goes and the rest is approximated (no culling, no MSAA, no EDRAM aliasing,
// 1x targets keyed by EDRAM base/format/pitch, resolves stay on the GPU). Never an installed path.
#include "device.h"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace recomp::gpu::generic {
using Reader=std::function<std::optional<std::vector<std::uint8_t>>(std::uint32_t,std::uint32_t)>;

// v80: the skinning palette constants (823BDB28's SET_SHADER_CONSTANTS, gta5_inline_constants.h) as the
// hook saw them before this draw; read by the census only.
struct SkinConstantsInfo {
    bool capture=true;                      // committed into the constants (diag-generic-no-skin-constants: no)
    std::uint64_t packets=0,committed=0,unreadable=0; // since boot (unreadable: + bad header / out of range)
    std::uint64_t drawsSince=~std::uint64_t{0};       // draws since the last packet was flushed (0: this draw)
    unsigned start=0,count=0;               // last packet: ALU registers [start, start + count)
    const std::vector<std::uint32_t>* words=nullptr;  // its data, count * 4 words (empty unless read)
    const char* last="none";                // its result (committed / observed / unreadable / ...)
};

// v214: an allocator whose value-initialization is default-initialization, for the index vectors the hooks fill in
// full right after resize() (bigEndianIndices / bigEndianIndicesRebased write every element): resize() no longer zeroes
// them first (memset ~1% of the render thread, v212 house profile). Only these vectors use it.
template<class T> struct DefaultInitAllocator : std::allocator<T> {
    template<class U> struct rebind {using other=DefaultInitAllocator<U>;};
    using std::allocator<T>::allocator;
    DefaultInitAllocator()=default;
    template<class U> DefaultInitAllocator(const DefaultInitAllocator<U>&) noexcept {}
    template<class U> void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {::new(static_cast<void*>(p)) U;}
    template<class U,class... A> void construct(U* p,A&&... a) {::new(static_cast<void*>(p)) U(std::forward<A>(a)...);}
};
using IndexVector=std::vector<std::uint32_t,DefaultInitAllocator<std::uint32_t>>;

struct DrawInput {
    const std::vector<std::uint8_t>* state=nullptr;      // device shadow, 13860 bytes
    std::optional<std::array<std::uint32_t,3>> scissor;  // PA_SC_WINDOW_OFFSET, SCISSOR_TL, SCISSOR_BR
    const std::array<std::uint32_t,1024>* vertexConstants=nullptr; // effective float constants (words)
    const std::array<std::uint32_t,1024>* pixelConstants=nullptr;
    std::function<std::optional<std::array<std::uint32_t,6>>(unsigned)> fetch; // submitted texture fetch
    const std::vector<std::uint8_t>* vertexIdentity=nullptr;  // VS microcode as created (pack key)
    const std::vector<std::uint8_t>* vertexEffective=nullptr; // VS microcode with patched vfetch
    const std::vector<std::vector<std::uint8_t>>* vertexAlternates=nullptr; // other stored variants
    const std::vector<std::uint8_t>* pixelCode=nullptr;
    unsigned primitive=0;                                // Xenos primitive type
    IndexVector indices;                                 // raw indices; empty = non-indexed (v214: IndexVector)
    bool indicesFinal=false;                             // inline draws: already relative to the vertices
    std::uint32_t baseVertex=0;                          // bound draws: added before the VGT min/max clamp
    unsigned vertexCount=0;                              // non-indexed count
    const std::vector<std::uint8_t>* inlineVertices=nullptr; // UP draws: every vfetch reads this
    unsigned inlineStride=0;
    const char* source=nullptr;                          // v56: census tag of the hook ("dv": DrawVertices)
    std::vector<std::uint32_t>* pixelSamples=nullptr;    // v56: when set, the pixel stage's texture base addresses
    const SkinConstantsInfo* skin=nullptr;               // v80: census of skinned draws (BLENDINDICES inputs)
    // v78: content versions of vertexIdentity / vertexEffective / pixelCode (0: unknown). Two draws passing the same
    // nonzero version for a field pass the same bytes there (the hooks' byte-checked shader cache generations), so the
    // renderer reuses its lookup of those bytes instead of hashing and comparing them again. Versions of the three
    // fields share one namespace (the caller keeps them distinct).
    std::uint64_t vertexIdentityVersion=0,vertexEffectiveVersion=0,pixelVersion=0;
    // v123: content versions of *vertexConstants / *pixelConstants (0: unknown). Two draws given the same nonzero version
    // for a bank give the same 1024 words (the hooks' Gta5DriverConstants::bankVersion of an in-place bank). The renderer
    // then binds the constants it copied into the ring for the earlier draw, and the pipelined capture shares its copy.
    std::uint64_t vertexConstantsVersion=0,pixelConstantsVersion=0;
    // v276: the bank each versioned array is (an identity, 0 unknown) and that bank's dirty mask (one bit per word changed
    // since the capture last consumed it; nullptr unknown). With both, the pipelined capture sends only the dirty words of a
    // bank it already sent from the same source (and clears the mask); otherwise it copies the bank as before.
    std::uint64_t vertexConstantsSource=0,pixelConstantsSource=0;
    std::array<std::uint64_t,16>* vertexConstantsDirty=nullptr;std::array<std::uint64_t,16>* pixelConstantsDirty=nullptr;
    // v78: the device shadow in place (13860 bytes, valid for the call) instead of `state`; either may be given.
    const std::uint8_t* stateBytes=nullptr;
    // v236: a bound-indexed draw while pipelined (deferIndexConversion()): the index buffer's validated guest bytes
    // (rawIndexCount big-endian entries of 4 bytes when rawIndex32, else 2) instead of `indices`; the renderer converts them
    // with hot::bigEndianIndices (the conversion the hook made) before draw(). Read by reference as vertex buffers are.
    // (v266: with the write-watch stream arena, a plain indexed draw's raw indices reach draw(), which may reuse the
    // converted list of an earlier frame while the watch reports the bytes unwritten.)
    const std::uint8_t* rawIndices=nullptr;std::uint32_t rawIndexCount=0;bool rawIndex32=false;
    // v111: the caller allows draw() to exchange `indices` and the vector `inlineVertices` points to with vectors of its
    // own (their contents are unspecified after the call; neither object may be const). The pipelined renderer then
    // takes them without copying.
    bool disposable=false;
};

struct ClearInput {
    const std::vector<std::uint8_t>* state=nullptr;
    std::optional<std::array<std::uint32_t,3>> scissor;
    std::uint32_t flags=0;              // bits 0-3 colour targets, 0x10 depth, 0x20 stencil
    std::array<std::int32_t,4> rect{};  // x1 y1 x2 y2 (already read from the guest)
    bool hasRect=false;
    std::array<float,4> color{};
    float depth=1;std::uint32_t stencil=0;
};

struct ResolveInput {
    const std::vector<std::uint8_t>* state=nullptr;
    // 82388A38 flags as 823C2738 consumes them: bits 0-2 source (0-3 colour, 4 depth), 4-6 sample
    // select, 0x100 clear the colour source, 0x200 clear depth/stencil (f1 + stack word 92), 0x2000
    // copy command "null" (clear only, no copy), bits 26-31 signed exponent bias (RB_COPY_DEST_INFO).
    std::uint32_t flags=0;
    std::optional<std::array<std::int32_t,4>> sourceRect;
    std::optional<std::array<std::int32_t,2>> destinationPoint;
    std::array<std::uint32_t,6> destinationFetch{}; // destination texture's fetch words
    std::array<float,4> clearColor{};
    unsigned mip=0,slice=0;             // v40: adapter r8 / r9 (destination level and slice)
    float clearDepth=1;std::uint32_t clearStencil=0; // v40: f1 and caller stack word 92 (RB_DEPTH_CLEAR)
    std::uint32_t parameters=0;         // v40: caller stack word 100 (logged only)
#ifdef RECOMP_THOR_EDITION
    std::uint32_t caller=0,caller2=0;   // v580 (census only): the guest return address of 82388A38's call and its caller's (G7 tile resolves) (ios-duo: hud3's radar resolve test reads it)
#endif /* RECOMP_THOR_EDITION */
};

struct Stats {
    std::uint64_t draws=0,drawRefused=0,clears=0,resolves=0,presents=0,pipelines=0,textures=0,
        missingVertexShader=0,missingPixelShader=0,unsupportedPrimitive=0,noVertexFormat=0,vteDisabled=0,
        textureFallback=0,flushes=0,rectLists=0;
};

// v111: the pipelined renderer. With the pipeline on (the device default; files/private-trace/diag-generic-sync-renderer
// restores v109, read every 30 presents and applied at a present), draw(), clear() and resolve() capture what they need
// (generic_renderer.cpp, "the pipelined renderer") and queue it for the renderer thread: they return true with *why
// "generic-draw-queued" / "generic-clear-queued" / "generic-resolve-queued"; the renderer counts its own refusals
// (RENDER_GENERIC_FRAME refused=, RENDER_GENERIC_NOTE draw-refused). present(), submitPending() and stats() first
// wait until everything queued before them has executed, then run as before on the calling thread; a census frame
// runs synchronously. Guest bytes a queued draw reads by reference (vertex buffers, textures) are read before the
// next guestReadBarrier() returns.
// Returns false when unavailable (no device or no pack); every later call then refuses.
bool initialize(ProbeDevice& device,const char* packPath);
bool available();
// Each returns true when the operation was recorded; *why names the first refusal otherwise.
bool draw(const DrawInput&,const Reader& data,const char** why);
// v236: whether a bound-indexed hook may pass DrawInput::rawIndices (pipelined, and diag-v236-baseline absent).
bool deferIndexConversion();
bool clear(const ClearInput&,const char** why);
bool resolve(const ResolveInput&,const char** why);
// Finishes the frame: submits, waits, and returns the RGBA8 1280x720 image of the texture at
// `address` (the front buffer's base) if a resolve produced it this frame or earlier.
std::optional<std::vector<std::uint8_t>> present(std::uint32_t address,unsigned width,unsigned height,const char** why);
// v225: hand the previous present image back for reuse by the next readback (no per-frame allocation).
void recycleFrame(std::vector<std::uint8_t>&& frame);
Stats stats();
// v96: whether the caller's display of the image present() returned succeeded (queued for the presenter
// thread, or shown synchronously). After 30 consecutive failures (the window/surface gone) present() stops
// copying frames to the CPU: it submits the frame's work and returns nothing (why "display-lost"), except
// one full present every 30 that probes whether the display is back; its present images are freed. The next
// success restores full presents. files/private-trace/diag-generic-no-display-lost keeps full presents.
void displayResult(bool shown);
// v158 (diag-generic-frame-interp): the frame halfway between the previous present's image and the one present() just
// returned (the same conversion), once; nullopt when interpolation is off or had no previous frame.
std::optional<std::vector<std::uint8_t>> takeInterpolatedFrame();
// v31: true when the pack holds a vertex shader for this microcode (canonical hash, as draw() looks
// it up). draw() tries the created code, then the patched code, then the other stored variants, so a
// caller may skip reading the other variants when either of the first two is known.
bool vertexShaderKnown(const std::vector<std::uint8_t>& code);
// v32: an optional in-place reader with the Reader's validation (nullptr where the Reader would
// refuse the whole range). Texture fingerprints validate a texture once through it instead of
// reading 32 words one registry lock at a time. Set after initialize().
using Viewer=std::function<const std::uint8_t*(std::uint32_t,std::uint32_t)>;
void setViewer(Viewer viewer);
// v86: the CPU rewrote guest bytes [address, address+bytes) in place (a texture unlock; virtual or physical
// address). Cached textures that read any of them are re-read at their next use, and the per-frame
// texture bindings are verified again. No effect with files/private-trace/diag-ui-no-unlock-invalidate.
void invalidateMemory(std::uint32_t address,std::uint32_t bytes);
// v86: the same for a texture unlock, given the texture object's fetch words (its base and mip bytes).
void invalidateTextureFetch(const std::array<std::uint32_t,6>& fetch);
// v75: publishes resolve results the CPU reads back (generic_resolve_writeback.h). The renderer calls it
// from present() only, so inside the present hook's executor gate: `physical` is the destination's
// physical base, `bytes` the span of its level 0; the writer reads the current guest bytes, passes
// them to `edit` (which returns false to leave memory untouched) and publishes the edited bytes.
enum class WriteStatus {Published,Unchanged,NoOwner,EditRefused,PublishRefused};
using Writer=std::function<WriteStatus(std::uint32_t physical,std::uint32_t bytes,
                                       const std::function<bool(std::vector<std::uint8_t>&)>& edit)>;
void setWriter(Writer writer);
// v40: one-frame census. Creating files/private-trace/diag-frame-census arms it (checked every 30
// presents; delete and re-create the file for another frame): the next frame logs every draw, clear,
// resolve and present (RENDER_CENSUS lines, tag RecompRenderer, and 60 KB chunk files under
// files/private-trace/census/) with probe pixels read back after each operation. censusNote records
// an operation the hooks skipped before it reached the renderer (no effect when no census runs).
bool censusActive();
void censusNote(const char* site,const char* detail);
// v79: the display gamma ramp D3D keeps at device+15408 after D3DDevice_SetGammaRamp (836AFAF8), as the
// 256 DC_LUT_30_COLOR words the next swap would write (post_fidelity.h packD3DRamp); `caller` is the
// guest return address (logged). Callable from any thread, before or after initialize(); present()
// maps R, G and B through it (files/private-trace/diag-generic-no-gamma-ramp: no mapping).
void setGammaRamp(const std::array<std::uint32_t,256>& packed,std::uint32_t caller);
// v78: submits the draws recorded since the last submission as one more command buffer of the current frame,
// without a fence and without waiting (the frame's last command buffer, at present, still carries the slot
// fence). Called after every guest ring submission handler: the native engine's queue fences (runtime
// command-storage coherency, tile interrupts) share this queue and wait for everything submitted before them,
// so work submitted right after a fence has until the next fence to run instead of all of it arriving at
// present. No effect when nothing new was recorded, a census runs, the renderer is unavailable, or
// files/private-trace/diag-generic-no-chunk-submit exists (v71: one submission per frame).
void submitPending();
// v111: returns once every draw, clear and resolve queued before the call has executed, so that every guest byte they
// read by reference has been read. The guest's ring submission handler calls it before the ring walk, i.e. before the
// fences behind those draws retire and the game may rewrite what they read. No effect with the pipeline off.
void guestReadBarrier();
// v183: runs `work` after every queued draw, clear and resolve captured before the call has executed (in queue order);
// at once when the pipeline is off. For guest-visible completion stores (fences) that must follow the renderer's reads.
bool queueGuestWork(std::function<void()> work);
}
