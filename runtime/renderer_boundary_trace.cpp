// Build-time opt-in: experimental GTA V API renderer with original-driver
// fallback. Live clear/draw/resolve execute on Vulkan; resolve publishes real
// pixels to owned guest textures. Native Present takes over after display setup.
// Addresses below were traced in the user's GTA V executable, not inherited
// from the GTA IV/Sonic hooks in SanRecomp/gpu/video.cpp. Original CPU behavior
// and return registers are retained until native presentation takes ownership.
// Inline draw inputs are copied only inside the executor's exclusive gate.
#include "prime_swap_v245.h" // v245
#include "pace_caps.h" // pace-caps (lane A): diag-pace-* (paceHold)
#include "fast_tls_v362.h" // v362: the render thread's thread_locals without a TLS resolver call
#include "diag_log_sample.h" // v218
#include "vulkan_queue_lock_all.h" // v177: every VkQueue call in this file holds the shared queue lock
#include <set>
#include <map>
#include <string>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "ppc_config.h"
#include "ppc_context.h"
#include "renderer_boundary_trace.h"
namespace recomp::bootstrap { const char* exchangeGuestHostActivity(const char* label) noexcept; } // v226 (executor)
namespace { struct HostActivityScope { const char* p; explicit HostActivityScope(const char* l) noexcept : p(recomp::bootstrap::exchangeGuestHostActivity(l)) {}
    ~HostActivityScope() { recomp::bootstrap::exchangeGuestHostActivity(p); } }; } // v226 DIAGNOSTIC
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
#include "diag_show_frames.h"
#endif
#include "occlusion_queries.h" // occlusion package: the Issue hook's ZPD markers (diag-oc-queries)
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
#include "generic_renderer.h"
#include "span_move.h" // span-move (diag-sm-worker): recomp_sm_snapshot_source
#include "generic_texture_decode.h" // v75: tex::fromHalf for the exposure-state log
#include "post_fidelity.h" // v79
#include "bound_null_pixel.h" // white-floor: bound indexed draws without a pixel shader (the opaque shadow casters)
#include <atomic>
extern "C" std::atomic<int> recomp_diag_loading_screen_shutdown;
#endif
#include "bound_indexed_capture_budget.h"
#include "guest_import_registry.h"
#include "native_tile_lock_call.h"
#include "native_completed_scene_target.h"
#include "xenos_texture_description.h"
#include "xenos_straight_line_shader_identity.h"
#include "gta5_shader_resource.h"
#include "gta5_vertex_shader_binding.h"
#include "gta5_shader_patch_capture.h"
#include "native_color_chain_program.h"
#include "gta5_inline_draw.h"
#include "gta5_stopped_inline_draw.h"
#include "native_expanded16_texture_capture.h"
#include "gta5_stopped_surface_evidence.h"
#include "gta5_stopped_query_evidence.h"
#include "gta5_query_command_trace.h"
#include "gta5_pending_query_neutral_fan.h"
#include "gta5_bound_indexed_draw.h"
#include "gta5_startup_draw_inputs.h"
#include "gta5_driver_constants.h"
#include "gta5_inline_constants.h" // v80
#include "render_hot_path.h"
#include "render_trims_v366.h" // v366 (d, e)
#include "loading_handover.h" // loading-race: script render targets before the takeover
#include "gta5_draw_vertices.h"
namespace recomp::bootstrap { std::uint32_t currentGuestThreadToken() noexcept; } // v56 (guest_parallel_wake.h)
#include "gta5_driver_scissor.h"
#include "gta5_driver_scissor_direct.h" // v278
#include "gta5_tile_recording_diagnostic.h"
#include "gta5_tile_resolve_capture.h"
#include "native_resolve_failure_capture.h"
#include "native_fixed16_resolve.h"
#include "gta5_recorded_depth_resolve.h"
#include "gta5_end_tiling_capture.h"
#include "gta5_native_scene_inputs.h"
#include "gta5_native_scene_resolves.h"
#include "gta5_native_scene_owned_inputs.h"
#include "gta5_native_scene_commands.h"
#include "gta5_native_tile_submission_capture.h"
#include "gta5_native_tile_initial_queue.h"
#include "gta5_native_tile_first_scene.h"
#include "gta5_native_tile_final_scene.h"
#include "gta5_draw_state.h"
#include "gta5_color_clear.h"
#include "gta5_clear_capture.h"
#include "gta5_render_queue_inventory.h"
#include "gta5_color8_clear_plan.h"
#include "native_color8_clear_execution.h"
#include "native_single_color8_resolve.h"
#include "gta5_bound_surface_capture.h"
#include "native_color_chain_capture.h"
#include "native_translated_draw.h"
#include "native_inline_indices.h"
#include "native_offscreen_resolve.h"
#include "native_offscreen_resolve_request.h"
#include "native_color8_resolve.h"
#include "native_cube_base_capture.h"
#include "native_raw10_texture_capture.h"
#if __has_include("native_filter_weights_shader_pack.h")
#include "native_filter_weights_shader_pack.h"
#endif
#if __has_include("native_expanded16_max_shader_pack.h")
#include "native_expanded16_max_shader_pack.h"
#endif
#if __has_include("native_expanded16_luma_shader_pack.h")
#include "native_expanded16_luma_shader_pack.h"
#endif
#if __has_include("native_r32_scalar_shader_pack.h")
#include "native_r32_scalar_shader_pack.h"
#endif
#if __has_include("native_exposure_scalar_shader_pack.h")
#include "native_exposure_scalar_shader_pack.h"
#endif
#if __has_include("native_bright_pass_shader_pack.h")
#include "native_bright_pass_shader_pack.h"
#endif
#if __has_include("native_expanded16_copy_shader_pack.h")
#include "native_expanded16_copy_shader_pack.h"
#endif
#if __has_include("native_postfx_blur_shader_pack.h")
#include "native_postfx_blur_shader_pack.h"
#endif
#if __has_include("native_postfx_vertical_blur_shader_pack.h")
#include "native_postfx_vertical_blur_shader_pack.h"
#endif
#if __has_include("native_postfx_upsample_shader_pack.h")
#include "native_postfx_upsample_shader_pack.h"
#endif
#include "native_stopped_physical32_capture.h"
#include "native_mip_texture_capture.h"
#include "native_preserved_sample_token.h"
#include "native_stencil_tile_batch.h"
#include "native_primary_clear.h"
#include "native_stencil_clear.h"
#include "native_edram32_bands.h"
#if __has_include("native_query_neutral_fan_shader_pack.h")
#include "native_query_neutral_fan_shader_pack.h"
#endif
#include "native_fixed16_edram.h"
#include "native_raw10_color_resolve.h"
#include "native_rgb7e3_float_resolve.h"
#include "native_r32_float_resolve.h"
#include "native_exposure_resolve.h"
#if __has_include("native_additional_shader_pack.h")
#include "native_additional_shader_pack.h"
#endif
#if __has_include("native_offscreen_shader_pack.h")
#include "native_offscreen_shader_pack.h"
#endif
#if __has_include("native_offset_sample_shader_pack.h")
#include "native_offset_sample_shader_pack.h"
#include "native_offset_sample_resolve.h"
#define RECOMP_HAVE_OFFSET_SAMPLE 1
#endif
#if __has_include("native_offset_sample2_shader_pack.h")
#include "native_offset_sample2_shader_pack.h"
#define RECOMP_HAVE_OFFSET_SAMPLE2 1
#endif
#if __has_include("native_tonemap_shader_pack.h")
#include "native_tonemap_shader_pack.h"
#define RECOMP_HAVE_TONEMAP 1
#endif
#if __has_include("native_tonemap2_shader_pack.h")
#include "native_tonemap2_shader_pack.h"
#define RECOMP_HAVE_TONEMAP2 1
#endif
#if __has_include("native_tonemap3_shader_pack.h")
#include "native_tonemap3_shader_pack.h"
#define RECOMP_HAVE_TONEMAP3 1
#endif
#if __has_include("native_sampled_stencil_shader_pack.h")
#include "native_sampled_stencil_shader_pack.h"
#endif
#if __has_include("native_stencil_only_shader_pack.h")
#include "native_stencil_only_shader_pack.h"
#include "native_stencil_only_plan.h"
#if (defined(__ANDROID__) || defined(__APPLE__))
#include "native_stencil_pipeline_options.h"
#endif
#endif
#if __has_include("native_rectangle_mrt_shader_pack.h")
#include "native_rectangle_mrt_shader_pack.h"
#include "native_rectangle_mrt_target.h"
#include "native_rectangle_mrt_resolve.h"
#define RECOMP_HAVE_RECTANGLE_MRT 1
#endif
#if __has_include("native_depth_only_shader_pack.h")
#include "native_depth_only_shader_pack.h"
#include "native_inert_draw_plan.h"
#include "native_read_only_rectangle_plan.h"
#endif
#if __has_include("native_composite_shader_pack.h")
#include "native_composite_shader_pack.h"
#endif
#if __has_include("native_screen_shader_pack.h")
#include "native_screen_shader_pack.h"
#endif
#if __has_include("native_screen_strip_shader_pack.h")
#include "native_screen_strip_shader_pack.h"
#endif
#if __has_include("native_world_strip_shader_pack.h")
#include "native_world_strip_shader_pack.h"
#include "native_world_strip_capture.h"
#endif
#if __has_include("native_clipped_indexed_shader_pack.h")
#include "native_clipped_indexed_shader_pack.h"
#endif
#if __has_include("native_indexed_rgb7e3_shader_pack.h")
#include "native_indexed_rgb7e3_shader_pack.h"
#endif
#if __has_include("native_indexed_zero_depth_shader_pack.h")
#include "native_indexed_zero_depth_shader_pack.h"
#endif
#if __has_include("native_indexed_blend_depth_shader_pack.h")
#include "native_indexed_blend_depth_shader_pack.h"
#endif
#if __has_include("native_indexed_lod_shader_pack.h")
#include "native_indexed_lod_shader_pack.h"
#endif
#if __has_include("native_clipped_scene_fan_shader_pack.h")
#include "native_clipped_scene_fan_shader_pack.h"
#endif
#if __has_include("native_stencil_reject_identity.h")
#include "native_stencil_reject_identity.h"
#endif
#if __has_include("native_conditional_stencil_reject_identity.h")
#include "native_conditional_stencil_reject_identity.h"
#endif
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <chrono>
#include "diag_timeline.h" // v159
#include <filesystem>
#include <fstream>
#include "gta5_driver_textures.h"
#include "xenos_shader_texture_slots.h"
#include "gta5_submitted_texture_pages.h"
#if (defined(__ANDROID__) || defined(__APPLE__))
#include <android/log.h>
#include <dlfcn.h>
#include <unwind.h>
#include "vulkan_engine.h"
#if __has_include("native_clipped_fan_shader_pack.h")
#include "native_clipped_fan_shader_pack.h"
#endif
#if __has_include("native_gamma_fan_shader_pack.h")
#include "native_gamma_fan_shader_pack.h"
#endif
#if __has_include("native_gamma_mip_fan_shader_pack.h")
#include "native_gamma_mip_fan_shader_pack.h"
#endif
#if __has_include("native_gamma_snapshot_module.h")
#include "native_gamma_snapshot_module.h"
#endif
#if __has_include("native_gamma_quad_shader_pack.h")
#include "native_gamma_quad_shader_pack.h"
#endif
#if __has_include("native_cube_rectangle_shader_pack.h")
#include "native_cube_rectangle_shader_pack.h"
#endif
#if __has_include("native_lod_rectangle_shader_pack.h")
#include "native_lod_rectangle_shader_pack.h"
#endif
#if __has_include("native_predicated_lod_rectangle_shader_pack.h")
#include "native_predicated_lod_rectangle_shader_pack.h"
#endif
#if __has_include("native_retail_lod5_rectangle_shader_pack.h")
#include "native_retail_lod5_rectangle_shader_pack.h"
#endif
#if __has_include("native_depth_snapshot_module.h")
#include "native_depth_snapshot_module.h"
#endif
#if __has_include("native_bound_geometry_shader_pack.h")
#include "native_bound_geometry_shader_pack.h"
#endif
#if __has_include("native_fixed16_draw_shader_pack.h")
#include "native_fixed16_draw_shader_pack.h"
#endif
#if __has_include("native_fixed16_rectangle_shader_pack.h")
#include "native_fixed16_rectangle_shader_pack.h"
#endif
#if __has_include("native_logical_geometry_shader_pack.h")
#include "native_logical_geometry_shader_pack.h"
#endif
#if __has_include("native_persistent_quad_shader_pack.h")
#include "native_persistent_quad_shader_pack.h"

#endif
#endif

namespace {
constexpr unsigned CaptureLimit = 16;
std::array<std::uint32_t, CaptureLimit> presentedTextures{};
std::array<std::uint32_t, CaptureLimit> pixelShaders{};
struct VertexPatch { std::uint32_t object=0,code=0,variant=0; bool returned=false; };
std::array<VertexPatch, CaptureLimit> vertexPatches{};
std::map<std::uint32_t,recomp::gpu::Gta5VertexShaderBinding> selectedVertexShaders;
constexpr std::size_t ShaderPatchHistoryLimit=16;
std::array<std::optional<recomp::gpu::Gta5ShaderPatchCapture>,ShaderPatchHistoryLimit> shaderPatchHistory;
std::array<std::uint64_t,ShaderPatchHistoryLimit> shaderPatchSequences{};
std::uint64_t shaderPatchAttempts=0;
std::atomic<unsigned> shaderPatchCaptureFailures{0};
recomp::bootstrap::GuestImportRegistry* inlineRegistry=nullptr;
// (Claude 2026-10-21, ported from the Fold's v571 index_view.h, CROSS-PORT-LOG 22:25): the registry's graphics-data view refuses
// an address that is not a multiple of 4. A draw of 16-bit indices with an ODD start index begins 2 bytes past such a boundary, so
// every such draw was skipped whole ("index-data"; on the Fold 22 vfx_decal draws a frame outdoors). The view is taken from the
// 4-byte boundary at or below the address (the same allocation) and the pointer advanced by the difference; an aligned address
// reads exactly as before.
[[gnu::noinline]] const std::uint8_t* graphicsIndexView(std::uint32_t address,std::uint32_t bytes) noexcept {
    const std::uint32_t pad=address&3u;
    if(!pad)return inlineRegistry->viewSerializedGraphicsData(address,bytes);
    if(bytes>0xFFFFFFFFu-pad)return nullptr;
    const std::uint8_t* p=inlineRegistry->viewSerializedGraphicsData(address-pad,bytes+pad);
    if(p) { // (evidence on the device: the odd-start index ranges now served; first 3, then every 1000th)
        static std::atomic<std::uint64_t> served{0};const auto n=++served;
        if(n<=3 || !(n%1000))__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_INDEX_VIEW_ODD n=%llu address=%08x bytes=%u pad=%u\n",(unsigned long long)n,address,bytes,pad); // (the iOS console takes the log shim, not stderr)
    }
    return p?p+pad:nullptr;
}
const recomp::gpu::NativeColorChainProgram* inlinePrograms=nullptr;
std::function<bool(const std::function<void()>&)> inlineGate;
// v213: gate bodies passed by reference: a std::function holding std::ref(body) fits the function's inline buffer, so no
// heap allocation per gate call (a [&] body with more than ~3 captures was copied to the heap on every call: malloc/free
// ~3% of the render thread, v212 house profile). The same body runs under the same gate.
// v213 A/B: files/private-trace/diag-generic-v213-baseline (read at most once a second) restores v212 behaviour for the
// v213 render-thread cuts: gate bodies copied into the std::function, scissor shapes logged, the scalar constant commit.
bool v213Baseline() {
    static std::atomic<long long> checkedAt{0};static std::atomic<bool> on{false};
    unsigned& calls=::recomp::v362::tls<::recomp::v362::Slot::GateCalls,unsigned>([]() noexcept -> unsigned& {static thread_local unsigned calls=0;return calls;});
    if(calls++&1023)return on.load(std::memory_order_relaxed); // (the clock is read once per 1024 calls)
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now-checkedAt.load(std::memory_order_relaxed)>=1000) {
        checkedAt.store(now,std::memory_order_relaxed);
        const bool b=access(RECOMP_APP_PATH("/private-trace/diag-generic-v213-baseline"),F_OK)==0;
        on.store(b,std::memory_order_relaxed);recomp::gpu::scalarConstantCommit.store(b,std::memory_order_relaxed);
    }
    return on.load(std::memory_order_relaxed);
}
template<class F> bool gateRef(F&& body) {
    if(v213Baseline())return inlineGate(std::function<void()>(body)); // (v212: the body copied into the function)
    return inlineGate(std::function<void()>(std::ref(body)));
}
std::function<bool(const std::function<void()>&)> originalRecordingGate;
std::function<bool(const std::function<void()>&)> originalEndRecordingGate;
std::function<bool(unsigned,const recomp::gpu::Gta5TileCommandInterrupt&)> nativeSceneInterrupt;
std::function<bool(unsigned,const recomp::gpu::Gta5TileSceneContinuation&)> nativeSceneContinuation;
RendererSceneScheduler nativeSceneScheduler;
recomp::gpu::Gta5NativeTileSubmissionCapture* activeSceneQueue=nullptr;
std::optional<recomp::gpu::Gta5NativeTileInitialQueue> stoppedInitialTileQueue;
bool stoppedInitialRingRetired=false;
recomp::gpu::Gta5NativeTileSubmissionCapture* activeEndQueue=nullptr;
std::optional<recomp::gpu::Gta5NativeTileSubmissionCapture> stoppedEndQueue;
std::optional<std::vector<std::uint8_t>> stoppedEndCpuState;
bool stoppedEndCpuReturned=false;
std::optional<std::array<std::uint32_t,4>> endRestoredScissor;
struct PendingInline {
    std::uint32_t device=0;
    unsigned primitive=0,count=0,stride=0;
    bool nativeAllocation=false;
    unsigned indexCount=0,indexFormat=0,minVertex=0;
};
thread_local PendingInline pendingInline;
[[gnu::always_inline]] inline PendingInline& pendingInlineRef() noexcept {
    return ::recomp::v362::tls<::recomp::v362::Slot::PendingInline,PendingInline>([]() noexcept -> PendingInline& {return pendingInline;});
}
thread_local unsigned ragePresentDepth=0;
thread_local std::uint32_t lockingTexture=0;
std::atomic<std::uint32_t> nativeUiDevice{0};
std::atomic<unsigned> textureBindingDiagnostics{0};
constexpr std::uint32_t NativeInlineBytes=256*1024;
struct NativeInlineBuffer {std::uint32_t address=0;bool busy=false;std::uint32_t indexAddress=0;};
std::map<std::uint32_t,NativeInlineBuffer> nativeInlineBuffers;
struct RagePresentScope {
    bool active;
    explicit RagePresentScope(bool enabled):active(enabled){if(active)++ragePresentDepth;}
    ~RagePresentScope(){if(active)--ragePresentDepth;}
};
// Capture only the command-copy caller, not every persistent shader patch
// during normal UI frames. Keep the latest bounded history so the failed batch
// isn't displaced by boot-time activity. This never changes guest execution.
// v254: the history (a diagnostic for failure reports, never read by guest execution) is opt-in: two gated copies of the
// patched microcode per call were 0.89 ms/frame of the render thread in R outdoors (v250 phase profile) against 0.09 ms for
// the original patch. files/private-trace/diag-v254-shader-patch-history restores the capture (polled by the renderer).
extern "C" std::atomic<bool> recomp_v254_patch_history;
std::optional<std::uint64_t> beginCommandShaderPatch(const PPCContext& ctx) noexcept {
    if(!recomp_v254_patch_history.load(std::memory_order_relaxed))return std::nullopt;
    if(ctx.lr!=0x823BDD60 || !nativeUiDevice.load() || !inlineRegistry || !inlineGate)return std::nullopt;
    std::optional<std::uint64_t> ticket;
    try {
        if(!gateRef([&] {
            const auto sequence=++shaderPatchAttempts;
            const auto slot=(sequence-1)%ShaderPatchHistoryLimit;
            shaderPatchHistory[slot].reset();shaderPatchSequences[slot]=sequence;
            const std::array<std::uint32_t,5> args{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32};
            auto snapshot=recomp::gpu::captureGta5ShaderPatchBefore(args,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
            if(!snapshot){++shaderPatchCaptureFailures;return;}
            shaderPatchHistory[slot]=std::move(snapshot);ticket=sequence;
        }))++shaderPatchCaptureFailures;
    } catch(...) {++shaderPatchCaptureFailures;}
    return ticket;
}
void endCommandShaderPatch(const std::optional<std::uint64_t>& ticket) noexcept {
    if(!ticket || !inlineRegistry || !inlineGate)return;
    try {
        if(!gateRef([&] {
            const auto slot=(*ticket-1)%ShaderPatchHistoryLimit;
            if(shaderPatchSequences[slot]!=*ticket || !shaderPatchHistory[slot]) {
                ++shaderPatchCaptureFailures;return;
            }
            if(!recomp::gpu::captureGta5ShaderPatchAfter(*shaderPatchHistory[slot],
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);}))
                ++shaderPatchCaptureFailures;
        }))++shaderPatchCaptureFailures;
    } catch(...) {++shaderPatchCaptureFailures;}
}
std::array<std::optional<recomp::gpu::Gta5InlineDraw>,CaptureLimit> inlineDraws;
unsigned inlineDrawCount=0;
struct StoppedColorSamples {
    unsigned object=0,width=0,height=0,samples=0,format=0;
    std::uint64_t serial=0;
    std::vector<std::uint8_t> rgba;
    unsigned depthObject=0;
    bool depthUniform=false;
    const char* provenance="unavailable";
    std::vector<std::uint8_t> depth;
};
struct UnsupportedInline {
    PendingInline pending;
    std::uint32_t vertexObject=0,pixelObject=0;
    std::vector<std::uint8_t> vertexCode,pixelCode,vertexResource,pixelResource,deviceState;
    std::optional<recomp::gpu::Gta5InlineDraw> draw;
    std::uint32_t textureCaptured=0,textureUnavailable=0;
    recomp::gpu::Gta5SubmittedTextureSnapshot submittedTextures{};
    recomp::gpu::Gta5SubmittedTexturePages submittedTexturePages;
    std::vector<recomp::gpu::Gta5InlineDraw::Texture> raw10Textures;
    std::vector<recomp::gpu::Gta5InlineDraw::Texture> expanded16Textures;
    std::optional<recomp::gpu::NativeStoppedPhysical32Capture> physicalAtStop;
    std::vector<std::pair<unsigned,recomp::gpu::NativeMipTextureCapture>> mipTextures;
    std::uint32_t indexAddress=0,indexWords=0;
    std::vector<std::uint8_t> indices;
    std::optional<recomp::gpu::Gta5BoundSurfaceCapture> surfaces;
    recomp::gpu::Gta5StoppedSurfaceEvidence surfaceEvidence;
    std::optional<recomp::gpu::Gta5StoppedQueryEvidence> queryEvidence;
    std::optional<recomp::gpu::Gta5QueryCommandTrace> queryCommands;
    StoppedColorSamples colorSamples;
};
std::optional<UnsupportedInline> firstUnsupportedInline;
std::optional<recomp::gpu::Gta5QueryCommandTrace> latestQueryCommandTrace;
// Owned diagnostic bytes survive unbind and a subsequent BEGIN. Never a lease
// or execution receipt; reset only when a new renderer generation is bound.
std::optional<recomp::gpu::Gta5QueryCommandTrace> retainedQueryEndTrace;
// Original indexed draws still emit PM4, while their upload wrappers already
// use native state. Own draw-time evidence before a later flush loses that
// connection. This is diagnostic-only: no skipped draws or fabricated retirements.
enum class BoundIndexedCapturePhase { Prepared, Returned, Exception };
struct BoundIndexedSnapshot {
    BoundIndexedCapturePhase capturePhase=BoundIndexedCapturePhase::Prepared;
    std::optional<std::uint32_t> pixelTextureSlots;
    std::uint32_t sequence=0,caller=0,device=0;
    recomp::gpu::Gta5BoundIndexedRequest request;
    recomp::gpu::DrawBinState nativeBins;
    std::optional<recomp::gpu::Gta5BoundIndexedStorage> geometry;
    std::optional<recomp::gpu::Gta5BoundSurfaceCapture> surfaces;
    std::vector<std::uint8_t> deviceState,vertexCode,pixelCode,indexHeader,vertexHeader;
    std::array<std::uint32_t,1024> vertexConstants{},pixelConstants{};
    recomp::gpu::Gta5SubmittedTextureSnapshot textures{};
    std::vector<std::pair<unsigned,recomp::gpu::NativeMipTextureCapture>> mipTextures;
    recomp::gpu::Gta5SubmittedTexturePages texturePages;
    bool orderedConstants=false,geometryRecordingOnly=false;
    std::uint32_t observationPc=0;
    std::optional<std::array<std::uint32_t,4>> recordedScissor;
    std::optional<recomp::gpu::Gta5RecordedIndexedEmissionWindow> recordingWindow;
    bool originalEmissionReturned=false;
};
std::vector<BoundIndexedSnapshot> boundIndexedSnapshots;
BoundIndexedCaptureBudget boundIndexedBudget;
std::atomic<unsigned> boundIndexedAttempts{0},boundIndexedFailures{0},boundIndexedDropped{0};
struct UnsupportedResolve {
    std::array<std::uint32_t,10> arguments{};
    std::uint32_t stack=0,source=0;
    std::uint64_t f1=0;
    std::vector<std::uint8_t> state,sourceHeader,destinationHeader,stack92,stack100;
    recomp::gpu::NativeOffscreenResolveArguments pointed;
    recomp::gpu::NativeOffscreenDrawPlan sourcePlan;
    std::vector<std::uint8_t> sourceRgba,destinationStorage;
    std::vector<std::uint8_t> sourceResourceTail;
    unsigned sourceSamples=1,sourceColorFormat=0;
    bool sourceClearOnly=false;
    std::optional<recomp::gpu::Gta5BoundSurfaceCapture> surfaces;
    std::uint32_t issues=0;
    bool graphicsHeader=false,clearCompleted=false;
    StoppedColorSamples colorSamples;
};
std::optional<UnsupportedResolve> firstUnsupportedResolve;
std::optional<recomp::gpu::NativeResolveFailureCapture> firstNativeResolveFailure;
std::atomic<unsigned> nativeResolveFailureCaptureFailures{0};
std::optional<recomp::gpu::Gta5ClearCapture> firstUnsupportedClear;
std::optional<recomp::gpu::Gta5QueueInventory> firstUnsupportedQueue;
struct UnsupportedScissor {
    std::uint32_t device=0,caller=0;
    std::array<std::uint32_t,4> rectangle{};
    std::vector<std::uint8_t> state;
    std::optional<recomp::gpu::Gta5DriverScissorPlan> plan;
    std::optional<recomp::gpu::Gta5BoundSurfaceCapture> bindings;
};
std::optional<UnsupportedScissor> firstUnsupportedScissor;
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
// Build-time diagnostic ONLY. CPU recording is distinct from GPU completion.
// All draw/resolve/submit/wait/present boundaries below stop while this is armed.
recomp::gpu::Gta5TileRecordingDiagnostic tileRecordingDiagnostic;
std::atomic<std::uint32_t> tileRecordingDevice{0};
std::atomic<const char*> tileRecordingStop{nullptr};
std::optional<recomp::gpu::Gta5TileResolveCapture> stoppedTileResolve;
std::optional<recomp::gpu::Gta5EndTilingCapture> stoppedTileEnd;
thread_local unsigned preparedTileIndexedSequence=0;
thread_local unsigned originalResolveRecordingDevice=0;
[[gnu::always_inline]] inline unsigned& preparedTileIndexedSequenceRef() noexcept {
    return ::recomp::v362::tls<::recomp::v362::Slot::PreparedIndexed,unsigned>([]() noexcept -> unsigned& {return preparedTileIndexedSequence;});
}
[[gnu::always_inline]] inline unsigned& originalResolveRecordingDeviceRef() noexcept {
    return ::recomp::v362::tls<::recomp::v362::Slot::ResolveDevice,unsigned>([]() noexcept -> unsigned& {return originalResolveRecordingDevice;});
}
struct RecordedTileResolve {
    recomp::gpu::Gta5TileResolveCapture before;
    recomp::gpu::RecordedDepthResolveAdmission plan;
    std::optional<recomp::gpu::RecordedDepthResolveOutput> output;
    bool originalReturned=false;
};
std::vector<RecordedTileResolve> recordedTileResolves;
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
// Prepared from this live recording at EndTiling, never loaded from saved test
// files. Preparation is distinct from rendering and guest completion.
std::optional<recomp::gpu::Gta5NativeSceneInputs> pendingNativeSceneInputs;
std::optional<recomp::gpu::Gta5NativeSceneResolves> pendingNativeSceneResolves;
std::optional<recomp::gpu::Gta5NativeSceneResolveStorage> pendingNativeSceneStorage;
std::optional<recomp::gpu::Gta5NativeSceneCommands> pendingNativeSceneCommands;
#endif
#endif
std::map<std::uint32_t,recomp::gpu::Gta5DriverConstants> driverConstants;
std::map<std::uint32_t,recomp::gpu::Gta5DriverTextures> driverTextures;
bool driverConstantsValid=true;
// v80: the skinning palette (823BDB28, SET_SHADER_CONSTANTS in the command buffer) and the instanced
// draw's LOAD_ALU_CONSTANT (836B4920) past the shadow: gta5_inline_constants.h. Used inside inlineGate
// (its reads go through the registry's views); inlineConstantsMutex guards it for the present line.
recomp::gpu::InlineConstantsTracker inlineConstants;
std::mutex inlineConstantsMutex;
// v362 (exact): inlineConstants.pending(), mirrored under inlineConstantsMutex after every call that can change it (reserve,
// flush, gpuLoad: the tracker's only writers of its pending flag). flushInlineConstants reads it first and returns without the
// mutex when nothing is pending: the answer the locked test gives at that point of the mutex's order (the mirror is written
// inside the same critical sections, so a load that misses a reservation is ordered before that reservation's section, where
// the locked test would also have found nothing pending). v349 profiles: this mutex 0.8-0.9% of the render thread, taken by
// every draw's constants (the release of an otherwise idle mutex waits for the draw's preceding stores to drain).
std::atomic<bool> inlineConstantsPending{false};
void noteInlineConstantsPendingLocked(){inlineConstantsPending.store(inlineConstants.pending(),std::memory_order_release);}
std::map<std::uint32_t,std::optional<std::array<std::uint32_t,3>>> driverScissors;
#if (defined(__ANDROID__) || defined(__APPLE__))
recomp::gpu::VulkanEngine* inlineGraphics=nullptr;
std::unique_ptr<recomp::gpu::NativeLogicalTileTarget> pendingNativeSceneTarget;
recomp::gpu::NativeCompletedSceneTarget<recomp::gpu::NativeLogicalTileTarget,recomp::gpu::VulkanEngine> completedNativeSceneTarget;
struct LiveTarget {
    std::unique_ptr<recomp::gpu::NativeApiDrawTarget> image;
    std::uint32_t object=0;
    bool valid=false;
    bool ownsPresentation=false;
    std::uint64_t contentSerial=0,resolvedSerial=0;
    std::uint32_t presentCount=0;
    std::uint32_t initialVBlank=0;
    std::chrono::steady_clock::time_point presentationEpoch;
    // Storage ranges consumed by actual completed native submissions. Fetch
    // addresses are canonicalized through the registry's current owner, not
    // through an assumed Xbox physical-address mask.
    struct TextureUse {recomp::gpu::XenosTextureDescription description;std::uint32_t bytes;};
    std::map<std::uint32_t,TextureUse> completedTextures;
    std::map<std::uint32_t,recomp::gpu::NativeMipTextureUse> completedMipTextures;
};
std::map<std::uint32_t,LiveTarget> liveTargets;
// Actual fenced 32bpp physical storage, not a cache of a previous scene image.
// All mutation entrypoints detach/reset it BEFORE a submission; unsupported
// writer profiles conservatively lose coverage instead of preserving stale data.
std::map<std::uint32_t,recomp::gpu::NativeEdram32Bands> liveEdram32;
struct OffscreenTarget {
    std::unique_ptr<recomp::gpu::NativeOffscreenDrawTarget> image;
    std::vector<std::uint8_t> header;
    bool valid=false;
};
std::map<std::pair<std::uint32_t,std::uint32_t>,OffscreenTarget> offscreenTargets;
// ABI22 rectangle MRT writes TWO attachments from one pass, so its target is
// tracked as one entry keyed by its FIRST target object, carrying both headers.
struct RectangleMrtTargetEntry {
    std::unique_ptr<recomp::gpu::NativeRectangleMrtTarget> image;
    std::array<std::vector<std::uint8_t>,2> headers;
    bool valid=false;
};
std::map<std::pair<std::uint32_t,std::uint32_t>,RectangleMrtTargetEntry> rectangleMrtTargets;
struct OffsetSampleTargetEntry {
    std::unique_ptr<recomp::gpu::NativeOffscreenDrawTarget> image;
    std::vector<std::uint8_t> header;
    bool valid=false;
};
std::map<std::pair<std::uint32_t,std::uint32_t>,OffsetSampleTargetEntry> offsetSampleTargets;
struct ClearedColor8Target {
    std::unique_ptr<recomp::gpu::NativeColor8ClearTarget> image;
    std::vector<std::uint8_t> header;
    bool valid=false;
    std::uint64_t contentSerial=0;
};
std::map<std::pair<std::uint32_t,std::uint32_t>,ClearedColor8Target> clearedColor8Targets;
#if __has_include("native_gamma_snapshot_module.h")
StoppedColorSamples captureCompletedColorSamples(std::uint32_t device,std::uint32_t object,
        const recomp::gpu::NativePreservedSampleToken& preserved={}) {
    StoppedColorSamples result;
    const auto found=clearedColor8Targets.find({device,object});
    if(found==clearedColor8Targets.end() || !found->second.image)return result;
    const auto& target=found->second;const auto& p=target.image->plan();
    if(!target.valid && !preserved.matches(target.image.get(),object,target.contentSerial,
            target.image->diagnosticMutationSerial()))return result;
    // The image also independently rejects snapshots after an incomplete
    // persistent write. This fallback never revalidates an owner, increments
    // completion serials or publishes pixels to the guest.
    if(!target.image->borrowSamplesForReadOnlyDiagnostic())return result;
    result.object=object;result.width=p.width;result.height=p.height;result.samples=p.samples;result.format=p.format;result.serial=target.contentSerial;
    if(const auto pixels=target.image->snapshotEncodedSamples(nativeGammaSnapshotCode)) {
        result.rgba=*pixels;result.provenance=target.valid?"completed-owner":"preserved-before-rejected-draw";
    }
    if(p.depth){result.depthObject=p.depth->object;result.depthUniform=target.image->isDepthStencilUniform();}
#if __has_include("native_depth_snapshot_module.h")
    if(p.depth)if(const auto pixels=target.image->snapshotDepthSamples(nativeDepthSnapshotCode))result.depth=*pixels;
#endif
    return result;
}
#endif
std::optional<std::vector<std::uint8_t>> latestLiveHalf;
std::uint32_t latestLiveDevice=0;
unsigned liveClears=0,liveDraws=0,liveResolves=0,livePresents=0;
std::array<unsigned,3> completedTranslatedTextures{};
std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)> inlinePresent;
struct ResolvedTexture {std::uint32_t address=0;std::vector<std::uint8_t> bytes;};
std::map<std::uint32_t,ResolvedTexture> liveResolvedTextures;
#endif
// Attempt v43 (bound by bindRendererDiagnosticV43 from trace_runner.cpp, cleared with inlinePresent):
// asyncPresent queues a generic frame for the dedicated presenter thread (false: not queued, display
// synchronously); lockGateAdmits says whether the lock gate would admit the calling task right now
// (1 yes, 0 no, -1 unknown), with runNativeLockedGate's own refusal checks.
std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)> asyncPresent;
std::function<int()> lockGateAdmits;
// v158 (frame interpolation): queues (interpolated frame now, real frame after `delay`) for the presenter thread.
std::function<bool(const std::vector<std::uint8_t>&,const std::vector<std::uint8_t>&,unsigned,unsigned,std::chrono::microseconds)> asyncPresentPair;
// v43: the devices whose live target owns presentation (LiveTarget::ownsPresentation). Written only where
// that flag is set (inside a gate body, the one writer) and cleared with liveTargets at bind/unbind, so it
// holds exactly the devices for which ownsNativeRendering's gated lookup answers true. A fifth owner
// (never seen; the game has one device) switches the mirror off and the gated lookup decides again.
std::array<std::atomic<std::uint32_t>,4> presentationOwners{};
std::atomic<bool> presentationOwnersOverflow{false};
void notePresentationOwner(std::uint32_t device) {
    if(!device){presentationOwnersOverflow.store(true,std::memory_order_release);return;}
    for(auto& slot:presentationOwners) {
        const auto current=slot.load(std::memory_order_acquire);
        if(current==device)return;
        if(!current){slot.store(device,std::memory_order_release);return;}
    }
    presentationOwnersOverflow.store(true,std::memory_order_release);
}
void clearPresentationOwners() {
    for(auto& slot:presentationOwners)slot.store(0,std::memory_order_release);
    presentationOwnersOverflow.store(false,std::memory_order_release);
}
// v43 counters, logged as RENDER_DIAG_HOT every 256 generic presents.
struct HotCounters {
    std::atomic<std::uint64_t> ownerUngated{0},ownerGated{0};
    std::uint64_t bankInPlace=0,bankAssembled=0,bankShadowOnly=0,asyncPresents=0,syncPresents=0,
        scissorLines=0,scissorLinesSuppressed=0,scissorDirect=0,textureSpans=0,textureSlotReads=0,constantUploads=0,literalLoads=0,
        vertexSelectionsKept=0,vertexSelectionsRead=0;
} hot;
// v43: the bytes copySerializedGuestHeap(address,bytes) would return (its validation, one copy), into a
// reused buffer instead of a new vector.
bool copyGuestHeapInto(std::uint32_t address,std::uint32_t bytes,std::vector<std::uint8_t>& out) {
    const auto* p=inlineRegistry->viewSerializedGuestHeap(address,bytes);
    if(!p)return false;
    out.resize(bytes);std::memcpy(out.data(),p,bytes);return true;
}

template<class... Args>
void rendererLog(const char* format, Args... args) noexcept {
#if (defined(__ANDROID__) || defined(__APPLE__))
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", format, args...);
#else
    std::fprintf(stderr, format, args...);
#endif
}
// quiet-log (diag_log_sample.h): rendererLog for the high-rate line types nothing reads; not logged while
// files/private-trace/diag-quiet-log exists. rendererLog's shape with recomp::diag::quietLog (out of line, may throw like
// __android_log_print) in place of __android_log_print: the call sites keep their CFG (and IR-PGO records).
template<class... Args>
void rendererLogQuiet(const char* format, Args... args) noexcept {
#if (defined(__ANDROID__) || defined(__APPLE__))
    ::recomp::diag::quietLog(ANDROID_LOG_INFO, "RecompRenderer", format, args...);
#else
    if(!recomp::diag::quietSkip())std::fprintf(stderr, format, args...);
#endif
}
// quiet-log: rendererLog at a v218-sampled site (recomp::diag::quietLogSampled: while quiet every 16th, `keep` always).
template<class... Args>
void rendererLogSampled(int keep, const char* format, Args... args) noexcept {
#if (defined(__ANDROID__) || defined(__APPLE__))
    ::recomp::diag::quietLogSampled(keep, ANDROID_LOG_INFO, "RecompRenderer", format, args...);
#else
    if(keep || !recomp::diag::quietSkip())std::fprintf(stderr, format, args...);
#endif
}

void stopPendingTileRecording(const char* boundary) {
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(const auto device=tileRecordingDevice.load()) {
        const char* absent=nullptr;tileRecordingStop.compare_exchange_strong(absent,boundary);
        rendererLog("RENDER_TILE_RECORDING_STOP device=%08x boundary=%s gpu_completed=0 guest_published=0\n",device,boundary);
        throw std::runtime_error("Capture-only tiled recording stopped before GPU completion");
    }
#else
    (void)boundary;
#endif
}

void record(const char* name, unsigned call, const char* phase,
            const PPCContext& ctx) noexcept {
    if (call >= CaptureLimit) return;
    rendererLog(
        "RENDER_BOUNDARY %s call=%u %s lr=%08x sp=%08x "
        "r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x "
        "r9=%08x r10=%08x f1=%.9g\n",
        name, call + 1, phase, static_cast<std::uint32_t>(ctx.lr), ctx.r1.u32,
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
        ctx.r9.u32, ctx.r10.u32, ctx.f1.f64);
}
#if (defined(__ANDROID__) || defined(__APPLE__))
// Caller holds inlineGate: every other guest writer is parked and this caller
// performs no guest writes while inspecting the allocation's current owner.
std::optional<std::uint32_t> textureStorageAddress(
        const recomp::gpu::XenosTextureDescription& d,std::uint32_t bytes) {
    if(d.baseAddress>=0x20000000)return d.baseAddress;
    const auto owner=inlineRegistry->stoppedGraphicsPhysicalOwnership(d.baseAddress,bytes);
    if(!owner)return std::nullopt;
    return owner->virtualBase+(d.baseAddress-owner->physicalBase);
}
std::optional<recomp::gpu::NativeMipTextureUse> mipTextureUse(const std::array<std::uint32_t,6>& fetch) {
    return recomp::gpu::describeNativeMipTextureUse(fetch,[](auto address,auto bytes) {
        recomp::gpu::XenosTextureDescription view;view.baseAddress=address;
        return textureStorageAddress(view,bytes);
    });
}
void recordCompletedTextures(LiveTarget& live,const recomp::gpu::Gta5InlineDraw& draw) {
    for(const auto& texture:draw.textures) {
        if(texture.literalZero)continue;
        const auto d=recomp::gpu::XenosTextureDescription::decode(texture.fetch);
        const auto address=d?textureStorageAddress(*d,texture.source.size()):std::nullopt;
        if(!address)throw std::runtime_error("Completed native texture lost its allocation owner");
        live.completedTextures[*address]={*d,static_cast<std::uint32_t>(texture.source.size())};
    }
}
#endif
#include "renderer_fixed16.inc"
#if (defined(__ANDROID__) || defined(__APPLE__))
#include "renderer_fixed16_resolve.inc"
#include "renderer_raw10_resolve.inc"
#include "renderer_rgb7e3_float_resolve.inc"
#include "renderer_r32_float_resolve.inc"
#include "renderer_exposure_resolve.inc"
#include "renderer_single_color8_resolve.inc"
#if defined(RECOMP_HAVE_RECTANGLE_MRT)
#include "renderer_rectangle_mrt_resolve.inc"
#endif
#if defined(RECOMP_HAVE_OFFSET_SAMPLE)
#include "renderer_offset_sample_resolve.inc"
#endif
#endif

#if (defined(__ANDROID__) || defined(__APPLE__))
void invalidatePhysicalNativeViews(std::uint32_t device,unsigned base,unsigned tiles) {
    for(auto& [key,owner]:offscreenTargets)if(key.first==device && owner.image &&
        surfacePlanIntersects(owner.image->plan(),base,tiles))owner.valid=false;
    for(auto& [key,owner]:offsetSampleTargets)if(key.first==device && owner.image &&
        surfacePlanIntersects(owner.image->plan(),base,tiles))owner.valid=false;
    for(auto& [key,owner]:rectangleMrtTargets)if(key.first==device && owner.image)
        for(const auto& target:owner.image->plan().targets)
            if(surfacePlanIntersects(target,base,tiles))owner.valid=false;
    for(auto& [key,owner]:clearedColor8Targets)if(key.first==device && owner.image &&
        surfacePlanIntersects(owner.image->plan(),base,tiles))owner.valid=false;
    for(auto& [key,owner]:fixed16Targets)if(key.first==device && owner.image &&
        surfacePlanIntersects(owner.image->plan(),base,tiles))owner.valid=false;
    // Current presentation contract is pitch1280, 1280x720, base720.
    if(base<1440 && std::uint64_t{base}+tiles>720) {
        const auto found=liveTargets.find(device);if(found!=liveTargets.end())found->second.valid=false;
    }
    // Retained scene images remain only for maintenance/resource retirement.
    // ABI25 below never reuses those old pixels as current physical depth.
}
#endif


// ===== DIAGNOSTIC SKIP BUILD ONLY (generated by build_diagnostic_skip_v664.py) =====
// Every skip below is work that did NOT happen. Output after the first skip is
// wrong by construction; this build only inventories the remaining classes and
// measures how far the title's CPU flow advances. Never a qualified library.
std::uint64_t diagnosticCodeIdentity(const std::optional<std::vector<std::uint8_t>>& code) {
    std::uint64_t h=1469598103934665603ull;
    if(code)for(auto b:*code){h^=b;h*=1099511628211ull;}
    return h;
}
enum DiagnosticCounter {DiagDraws,DiagResolves,DiagClears,DiagIndexed,DiagWaits,DiagDrains,DiagInvalidPresents,DiagPresents,DiagCount};
unsigned long long diagnosticCounts[DiagCount]{};
std::vector<std::uint8_t> diagnosticLastFrame;
// First sighting of a key, then every power-of-two repeat.
unsigned diagnosticSeen(std::map<std::string,unsigned>& seen,const std::string& key) {
    auto& n=seen[key];++n;return (n&(n-1))==0?n:0;
}
void diagnosticSummary(const char* why) {
    rendererLog("RENDER_DIAG_SUMMARY why=%s presents=%llu invalid_presents=%llu skipped_draws=%llu skipped_resolves=%llu "
        "skipped_clears=%llu skipped_indexed=%llu forced_waits=%llu forced_drains=%llu diagnostic_build=1\n",why,
        diagnosticCounts[DiagPresents],diagnosticCounts[DiagInvalidPresents],diagnosticCounts[DiagDraws],
        diagnosticCounts[DiagResolves],diagnosticCounts[DiagClears],diagnosticCounts[DiagIndexed],
        diagnosticCounts[DiagWaits],diagnosticCounts[DiagDrains]);
}
void recordDiagnosticPresent() {
    if(!(++diagnosticCounts[DiagPresents]%120))diagnosticSummary("present");
}
void recordDiagnosticSkippedDraw(unsigned line,const char* reason,const std::optional<std::vector<std::uint8_t>>& vertexCode,
        const std::optional<std::vector<std::uint8_t>>& pixelCode,unsigned primitive,unsigned vertices,unsigned stride,
        unsigned indices,std::uint32_t target,std::uint32_t colourMask,std::uint32_t depthControl) {
    static std::map<std::string,unsigned> seen;
    ++diagnosticCounts[DiagDraws];
    const auto vs=diagnosticCodeIdentity(vertexCode),ps=diagnosticCodeIdentity(pixelCode);
    char key[96];std::snprintf(key,sizeof key,"%u/%016llx/%016llx",line,(unsigned long long)vs,(unsigned long long)ps);
    if(const auto n=diagnosticSeen(seen,key))
        rendererLog("RENDER_DIAG_SKIP_DRAW line=%u vs=%016llx ps=%016llx vs_bytes=%zu ps_bytes=%zu primitive=%u vertices=%u "
            "stride=%u indices=%u target=%08x colour_mask=%08x depth_control=%08x seen=%u distinct=%zu total=%llu reason=\"%s\"\n",
            line,(unsigned long long)vs,(unsigned long long)ps,vertexCode?vertexCode->size():0,pixelCode?pixelCode->size():0,
            primitive,vertices,stride,indices,target,colourMask,depthControl,n,seen.size(),diagnosticCounts[DiagDraws],reason);
}
void recordDiagnosticSkippedResolve(const PPCContext& ctx) {
    static std::map<std::string,unsigned> seen;
    ++diagnosticCounts[DiagResolves];
    unsigned width=0,height=0,format=0,address=0;std::uint32_t source=0;
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(inlineRegistry && inlineGate)gateRef([&] {
        if(const auto header=inlineRegistry->copySerializedGuestHeap(ctx.r6.u32,52))
            if(const auto d=recomp::gpu::describeGta5TextureHeader(*header))
                {width=d->width;height=d->height;format=d->format;address=d->baseAddress;}
        if(const auto s=inlineRegistry->copySerializedGuestHeap(ctx.r3.u32+12816,4);s && s->size()==4)
            source=(std::uint32_t{(*s)[0]}<<24)|(std::uint32_t{(*s)[1]}<<16)|(std::uint32_t{(*s)[2]}<<8)|(*s)[3];
    });
#endif
    char key[96];std::snprintf(key,sizeof key,"%08x/%08x/%ux%u/%u/%08x",static_cast<unsigned>(ctx.lr),ctx.r4.u32,width,height,format,source);
    if(const auto n=diagnosticSeen(seen,key))
        rendererLog("RENDER_DIAG_SKIP_RESOLVE caller=%08x flags=%08x r5=%08x destination_object=%08x r7=%08x r8=%08x r9=%08x r10=%08x "
            "destination=%08x size=%ux%u format=%u source_object=%08x seen=%u distinct=%zu total=%llu\n",
            static_cast<unsigned>(ctx.lr),ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
            address,width,height,format,source,n,seen.size(),diagnosticCounts[DiagResolves]);
}
void recordDiagnosticSkippedClear(std::uint32_t device,std::uint32_t flags,std::uint32_t rect,std::uint32_t color,std::uint32_t caller) {
    static std::map<std::string,unsigned> seen;
    ++diagnosticCounts[DiagClears];
    std::uint32_t object=0,depth=0;
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(inlineRegistry && inlineGate)gateRef([&] {
        const auto w=[&](std::uint32_t a) {
            const auto s=inlineRegistry->copySerializedGuestHeap(a,4);
            return s && s->size()==4?(std::uint32_t{(*s)[0]}<<24)|(std::uint32_t{(*s)[1]}<<16)|(std::uint32_t{(*s)[2]}<<8)|(*s)[3]:0u;
        };
        object=w(device+12816);depth=w(device+12832);
    });
#endif
    char key[64];std::snprintf(key,sizeof key,"%08x/%08x/%08x",caller,flags,object);
    if(const auto n=diagnosticSeen(seen,key))
        rendererLog("RENDER_DIAG_SKIP_CLEAR caller=%08x flags=%08x rect=%08x color=%08x colour_object=%08x depth_object=%08x seen=%u distinct=%zu total=%llu\n",
            caller,flags,rect,color,object,depth,n,seen.size(),diagnosticCounts[DiagClears]);
}
void recordDiagnosticSkippedIndexed(std::uint32_t device,const char* stage) {
    static std::map<std::string,unsigned> seen;
    ++diagnosticCounts[DiagIndexed];
    if(const auto n=diagnosticSeen(seen,stage))
        rendererLog("RENDER_DIAG_SKIP_INDEXED device=%08x stage=%s seen=%u distinct=%zu total=%llu\n",
            device,stage,n,seen.size(),diagnosticCounts[DiagIndexed]);
}
void recordDiagnosticForcedWait(std::uint32_t object) {
    static std::map<std::string,unsigned> seen;
    ++diagnosticCounts[DiagWaits];
    char key[16];std::snprintf(key,sizeof key,"%08x",object);
    if(const auto n=diagnosticSeen(seen,key))
        rendererLogQuiet("RENDER_DIAG_FORCED_RESOURCE_WAIT object=%08x seen=%u distinct=%zu total=%llu\n",object,n,seen.size(),diagnosticCounts[DiagWaits]);
}
void recordDiagnosticForcedDrain(std::uint32_t device) {
    if(const auto n=++diagnosticCounts[DiagDrains];(n&(n-1))==0)
        rendererLog("RENDER_DIAG_FORCED_DRAIN device=%08x total=%llu\n",device,n);
}
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
void genericTakeOver(const char* why);
#endif
void recordDiagnosticInvalidPresent(std::uint32_t object,unsigned valid,unsigned resolved) {
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    genericTakeOver("native-present-invalid");
#endif
    if(const auto n=++diagnosticCounts[DiagInvalidPresents];(n&(n-1))==0)
        rendererLog("RENDER_DIAG_INVALID_PRESENT object=%08x display_valid=%u resolved_texture=%u total=%llu\n",object,valid,resolved,n);
}
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
// First sighting of each (site, reason), then every power-of-two repeat.
void diagShowFramesReason(const char* site,const char* why) {
    static std::map<std::string,unsigned> seen;
    const std::string key=std::string(site)+"/"+why;
    if(const auto n=diagnosticSeen(seen,key))
        rendererLog("RENDER_DIAG_SHOW_FRAMES site=%s reason=\"%s\" seen=%u distinct=%zu\n",site,why,n,seen.size());
}
#endif
// ===== end DIAGNOSTIC SKIP helpers =====
// v80: where the bytes of an inline constant packet or a LOAD_ALU_CONSTANT source may be read, in order:
// the physical translation of the CPU address (836B4920's and D3D's: & 0x1FFFFFFF, + 0x1000 in the
// 0xE0000000 range) through the graphics-data view (what the GPU reads), the virtual address through
// the heap view, the address as graphics data. Inside inlineGate only.
const std::uint8_t* inlineConstantsCandidate(unsigned k,std::uint32_t address,std::uint32_t bytes) {
    if(!inlineRegistry)return nullptr;
    switch(k) {
    case 0: return inlineRegistry->viewSerializedGraphicsData((address&0x1FFFFFFFu)+(((address>>20)+512u)&0x1000u),bytes);
    case 1: return inlineRegistry->viewSerializedGuestHeap(address,bytes);
    case 2: return inlineRegistry->viewSerializedGraphicsData(address,bytes);
    default: return nullptr;
    }
}
const std::uint8_t* inlineConstantsView(std::uint32_t address,std::uint32_t bytes) { // LOAD_ALU_CONSTANT: the first readable
    for(unsigned k=0;k<3;++k)if(const auto* p=inlineConstantsCandidate(k,address,bytes))return p;
    return nullptr;
}
// v80: commits `device`'s pending skinning packet before anything reads or changes its constants. The
// packet is read where its header (written by 823BDB28 right before the data) is found; a readable
// candidate without it is reported as a bad header.
void flushInlineConstants(std::uint32_t device) {
    if(recomp::v362::trimsOn() && !inlineConstantsPending.load(std::memory_order_acquire))return; // v362
    std::lock_guard lock(inlineConstantsMutex);
    if(!inlineConstants.pending() || inlineConstants.pendingDevice()!=device)return;
    const auto packet=inlineConstants.pendingPacket();
    const auto view=[&](std::uint32_t address,std::uint32_t bytes)->const std::uint8_t* {
        const std::uint8_t* firstReadable=nullptr;
        for(unsigned k=0;k<3;++k) {
            const auto* p=inlineConstantsCandidate(k,address,bytes);
            if(!p)continue;
            if(!firstReadable)firstReadable=p;
            if(recomp::gpu::setShaderConstantsHeaderMatches(recomp::gpu::hot::be32(p),recomp::gpu::hot::be32(p+4),packet.bank,packet.start,packet.count))return p;
        }
        return firstReadable;
    };
    inlineConstants.flush(device,view,driverConstants[device]);
    noteInlineConstantsPendingLocked(); // v362
}
// v80: files/private-trace/diag-generic-no-skin-constants (v71: the skinning palette is not committed and
// no draw is skipped for it) and diag-generic-no-gpu-load-constants (v71: LOAD_ALU_CONSTANT not committed),
// checked at the first packet or load and every 256th after.
// v330a: switch files on the hook paths are re-read once per 60 presents (genericPresentHook advances switchEpoch), not every
// 256 / 1024 / 4096 calls: an access() is tens of us on Android and those checks ran ~10 times per frame on the render thread
// (v323 outdoor profile: __faccessat inside the bound-indexed and inline draw gates, sub_823BDB28 ~0.4%). A switch takes
// effect at the next 60-present window (the A/B windows' own boundary).
std::atomic<unsigned> switchEpoch{1};
inline bool switchEpochChanged(unsigned& seen){const unsigned e=switchEpoch.load(std::memory_order_relaxed);if(e==seen)return false;seen=e;return true;}
void inlineConstantsSwitches() {
    static std::atomic<unsigned> seenEpoch{0};static std::atomic<unsigned> counter{0};
    {const unsigned e=switchEpoch.load(std::memory_order_relaxed);if(seenEpoch.load(std::memory_order_relaxed)==e)return;seenEpoch.store(e,std::memory_order_relaxed);}
    const unsigned checks=counter.fetch_add(1,std::memory_order_relaxed)+1;
    const auto has=[](const char* name){
        char path[160];std::snprintf(path,sizeof path,RECOMP_APP_PATH("/private-trace/%s"),name);
        return access(path,F_OK)==0;};
    const bool skin=!has("diag-generic-no-skin-constants"),gpu=!has("diag-generic-no-gpu-load-constants");
    std::lock_guard lock(inlineConstantsMutex);
    if(checks==1 || skin!=inlineConstants.captureSkin || gpu!=inlineConstants.captureGpuLoads)
        rendererLog("RENDER_DIAG_SKIN_CONSTANTS_MODE capture=%u gpu_capture=%u\n",unsigned(skin),unsigned(gpu));
    inlineConstants.captureSkin=skin;inlineConstants.captureGpuLoads=gpu;
}
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
// v80: the per-frame RENDER_DIAG_SKIN_CONSTANTS line (the first 16 frames, then every 60th).
void finishInlineConstantsFrame() {
    static unsigned long long frames=0;static unsigned logged=0;
    std::lock_guard lock(inlineConstantsMutex);
    ++frames;
    const bool print=logged<16 || !(frames%60);
    const auto line=inlineConstants.frameLine(frames); // resets the frame counts either way
    if(print){++logged;rendererLog("%s\n",line.c_str());}
}
// v80: the census view of the skinning constants for the draw about to be recorded, and whether it
// must be skipped (its packet was not committed: fail closed instead of v71's stale bone rows).
bool genericSkinConstants(recomp::gpu::generic::SkinConstantsInfo& info) {
    std::lock_guard lock(inlineConstantsMutex);
    const auto& t=inlineConstants;
    info.capture=t.captureSkin;info.packets=t.total().packets;info.committed=t.total().committed;
    info.unreadable=t.total().unreadable+t.total().badHeader+t.total().outOfRange;
    info.drawsSince=t.drawsSince();info.start=t.last().bank*256+t.last().start;info.count=t.last().count;
    info.words=&t.lastWords();info.last=recomp::gpu::inlineConstantsResultName(t.lastResult());
    return inlineConstants.drawSkipped();
}
#endif
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
// v56: DrawVertices accounting (the 823CE5A0 hook and the present hook, under drawVerticesMutex), and the
// per-frame line (the first 16 frames, then every 60th, zero counts included).
recomp::gpu::DrawVerticesFrame drawVerticesFrame;
std::mutex drawVerticesMutex; // the frame record: callers may be in other permit domains
bool drawVerticesRoutingEnabled=true;
void finishDrawVerticesFrame() {
    static unsigned long long frames=0;static unsigned logged=0;
    std::lock_guard lock(drawVerticesMutex);
    ++frames;
    if(logged<16 || !(frames%60)){++logged;rendererLog("%s\n",drawVerticesFrame.line(frames,drawVerticesRoutingEnabled).c_str());}
    drawVerticesFrame=recomp::gpu::DrawVerticesFrame{};
}
// Attempt v16: every draw/clear/resolve/present after the native presentation takeover goes through
// generic_renderer (XenosRecomp translations of the game's shaders). Refusals are skipped draws.
extern "C" std::atomic<bool> recomp_v312_release __attribute__((weak)); // v312 (guest_cooperative_executor.cpp)
void genericNote(const char* site,const char* why) {
#if defined(RECOMP_RELEASE_PROFILE)
    (void)site;(void)why;return; // v312: the release profile keeps no per-call result counts
#endif
    if(&::recomp_v312_release && ::recomp_v312_release.load(std::memory_order_relaxed))return; // v312 (diag-v312-release)
    static std::map<std::string,unsigned> seen;
    // v34: per-draw callers pass string literals; after the first sighting of a (site, why) pointer pair,
    // count through a cached pointer to the same string-keyed counter (same counts and log lines, no
    // std::string construction and map search per draw).
    struct Key {const char* site;const char* why;bool operator==(const Key&)const=default;};
    struct Hash {std::size_t operator()(const Key& k)const noexcept{
        return std::hash<const void*>{}(k.site)*31u^std::hash<const void*>{}(k.why);}};
    static std::unordered_map<Key,unsigned*,Hash> counters;
    auto& counter=counters[Key{site,why}];
    if(!counter)counter=&seen[std::string(site)+"/"+(why?why:"?")];
    const auto n=++*counter;
    if((n&(n-1))==0)
        rendererLog("RENDER_GENERIC site=%s result=%s seen=%u distinct=%zu\n",site,why?why:"?",n,seen.size());
}
// Attempt v17: the native paths keep every screen they already draw (logo, legal and loading
// screens); the generic renderer takes over at gameplay start: the first present the native
// paths cannot complete, or the script's shutdown_loading_screen, whichever comes first.
std::atomic<bool> genericTakeover{false};
// v75: resolve writeback (generic_resolve_writeback.h). generic::present() calls it inside the present
// hook's gate: the destination's owner in the physical arena, its current bytes, the renderer's edit,
// then the registry's validated publication (unchanged bytes are not rewritten).
recomp::gpu::generic::WriteStatus genericWriteBack(std::uint32_t physical,std::uint32_t bytes,
        const std::function<bool(std::vector<std::uint8_t>&)>& edit) {
    using S=recomp::gpu::generic::WriteStatus;
#if (defined(__ANDROID__) || defined(__APPLE__))
    recomp::gpu::XenosTextureDescription d;d.baseAddress=physical;
    const auto address=textureStorageAddress(d,bytes);
    if(!address)return S::NoOwner;
    const auto before=inlineRegistry->copySerializedGraphicsData(*address,bytes);
    if(!before || before->size()!=bytes)return S::NoOwner;
    auto after=*before;
    if(!edit(after))return S::EditRefused;
    if(after==*before)return S::Unchanged;
    return inlineRegistry->publishSerializedGraphicsData(*address,*before,after)?S::Published:S::PublishRefused;
#else
    (void)physical;(void)bytes;(void)edit;return S::NoOwner;
#endif
}
// v75: the CPU side of GTA V's HDR pre-exposure (see generic_resolve_writeback.h), read inside the present
// gate every 120 presents and in census frames: the double-buffered exposure block at 839B77F0 (+0 w
// luminance, +8 y adapted exposure in stops, +16 x = 2^y, +24 z, +32 e = s * 2^-y, +40 buffer index), s at
// 8399D050, the CPU copy of globalScalars3 at 83803290 (expected 1/e, e, 1/e, e), the three exposure
// texel pointers at 83954EC0 with the frame counter at 839589B8 that picks one, and the 8 bytes each
// points at (the four big-endian halves sub_8238CE00 unpacks). Guest data is big-endian.
void genericExposureState(const char* when) {
    const auto title=[](std::uint32_t a,std::uint32_t n){return inlineRegistry->copySerializedTitleImage(a,n);};
    const auto be32=[](const std::vector<std::uint8_t>& b,std::size_t at){
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto f32=[&](const std::vector<std::uint8_t>& b,std::size_t at){const auto u=be32(b,at);float f;std::memcpy(&f,&u,4);return f;};
    const auto block=title(0x839B77F0u,48),scale=title(0x8399D050u,4),gs3=title(0x83803290u,16),ptrs=title(0x83954EC0u,12),counter=title(0x839589B8u,4);
    if(!block || block->size()!=48 || !scale || scale->size()!=4 || !gs3 || gs3->size()!=16 || !ptrs || ptrs->size()!=12 || !counter || counter->size()!=4) {
        rendererLog("RENDER_GENERIC_EXPOSURE_STATE when=%s unreadable\n",when);return;
    }
    std::string texels;
    for(unsigned i=0;i<3;++i) {
        const auto ptr=be32(*ptrs,4*i);char b[96];
        const auto t=ptr?inlineRegistry->copySerializedGraphicsData(ptr,8):std::nullopt;
        if(t && t->size()==8) {
            float h[4];for(unsigned k=0;k<4;++k)h[k]=recomp::gpu::generic::tex::fromHalf(std::uint16_t(((*t)[2*k]<<8)|(*t)[2*k+1]));
            std::snprintf(b,sizeof b," t%u=%08x:%g,%g,%g,%g",i,ptr,h[0],h[1],h[2],h[3]);
        } else std::snprintf(b,sizeof b," t%u=%08x:unreadable",i,ptr);
        texels+=b;
    }
    rendererLog("RENDER_GENERIC_EXPOSURE_STATE when=%s index=%u y=%g,%g e=%g,%g x=%g,%g w=%g,%g s=%g gs3=%g,%g,%g,%g frame=%u next=%u%s\n",
        when,be32(*block,40),f32(*block,8),f32(*block,12),f32(*block,32),f32(*block,36),f32(*block,16),f32(*block,20),f32(*block,0),f32(*block,4),
        f32(*scale,0),f32(*gs3,0),f32(*gs3,4),f32(*gs3,8),f32(*gs3,12),be32(*counter,0),(be32(*counter,0)+2)%3,texels.c_str());
}
void genericTakeOver(const char* why) {
    if(!genericTakeover.exchange(true))rendererLog("RENDER_GENERIC_TAKEOVER reason=%s\n",why);
}
}
// span-move (diag-sm-worker; span_move.h): the registry snapshot source for the renderer's worker-side span validation, for the
// viewer's registry (inlineRegistry, read at each call). Asked for by the renderer; a new function only (every existing function of
// this file keeps its CFG and PGO record).
extern "C" const std::atomic<std::uint64_t>* recomp_registry_generation_word(const void* registry) noexcept;
extern "C" bool recomp_sm_snapshot_source(recomp::gpu::generic::SnapshotSource* out) noexcept {
    if(!out || !inlineRegistry)return false;
    const auto* word=recomp_registry_generation_word(inlineRegistry);
    if(!word)return false; // (no fast-path slot: every draw is planned inline, as v442)
    out->word=word;
    out->current=[](std::uint64_t w) noexcept {return recomp::bootstrap::registryGraphicsSnapshotFor(inlineRegistry,w);};
    out->view=&recomp::bootstrap::registrySnapshotGraphicsView;
    return true;
}
namespace {
bool genericReady() {
    if(!genericTakeover.load(std::memory_order_acquire)) {
        if(!recomp_diag_loading_screen_shutdown.load(std::memory_order_acquire))return false;
        genericTakeOver("shutdown_loading_screen");
    }
    static int state=0; // 0 untried, 1 ready, -1 unavailable
    if(state==0 && inlineGraphics)if(auto* device=inlineGraphics->diagnosticDevice()) {
        // v41: its own pack (set-5 volume shaders, cloth/ped/steep shaders), so builds without v41's
        // renderer never load it; the v35 pack when it is absent.
        const char* v41=RECOMP_APP_PATH("/private-trace/generic-shaders-v41.bin");
        const char* v35=RECOMP_APP_PATH("/private-trace/generic-shaders.bin");
        std::FILE* probe=std::fopen(v41,"rb");if(probe)std::fclose(probe);
        state=recomp::gpu::generic::initialize(*device,probe?v41:v35)?1:-1;
    }
    if(state==1) { // v32: once; the viewer keeps copySerializedGraphicsData's validation.
        static bool viewerSet=false;
        if(!viewerSet){viewerSet=true;recomp::gpu::generic::setViewer([](std::uint32_t a,std::uint32_t n){return inlineRegistry->viewSerializedGraphicsData(a,n);});
            recomp::gpu::generic::setWriter(genericWriteBack);} // v75
    }
    return state==1;
}
recomp::gpu::generic::Reader genericDataReader() {
    return [](std::uint32_t a,std::uint32_t n){return inlineRegistry->copySerializedGraphicsData(a,n);};
}
std::uint32_t genericBe32(const std::vector<std::uint8_t>& b,unsigned at) {
    return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];
}
// The original vertex code for the selected variant (the pack key) plus the patched copy.
// v43: collected by render_hot_path.h (collectShaderCode), the v35 selection over in-place reads: the views
// carry copySerializedGuestHeap's/copySerializedGraphicsData's validation, header blocks and descriptor pairs
// are still copied once per call (fixed buffers instead of vectors), and the microcode is kept across draws
// and reused only while the current guest bytes equal it in full (else refreshed). The patched copy is the
// selected binding's own bytes (v35 copied them per draw). Pointers are valid inside this gate body.
using GenericShaderCode=recomp::gpu::hot::ShaderCodeResult;
recomp::gpu::hot::ShaderCodeState genericShaderState;
recomp::gpu::hot::ContentVersions genericEffectiveVersions; // v78
// v366d (exact; diag-v366-render-trims2, recomp_v366_render_trims2 in generic_renderer.cpp): the selected binding's content version
// without comparing its bytes again. genericEffectiveVersions.version(key, bytes) compares the binding's whole patched copy with
// the bytes it last saw for (device, vertex object) on every draw (v349 drive profile: memcmp 0.57% + version 0.2% of the render
// thread). A binding's bytes change only through selectedVertexShaders' writers below (observeNativeVertexShader's select,
// invalidateNativeVertexShader's erase, installation's clear), each of which advances vertexSelectionSerial (all under the gate,
// as this). So a draw whose key, binding vector and serial equal those of the last version() call, with no trim of the versions
// since, gets exactly that call's answer: the entry still holds those bytes and that version (version() is the only writer of its
// key's entry), and the bytes compared would be the same bytes. Counted as version()'s hit (ContentVersions::hits).
extern "C" std::atomic<bool> recomp_v366_render_trims2;
std::uint64_t vertexSelectionSerial=1; // (under the gate) advanced by every write of a vertex shader binding
recomp::v366::EffectiveVersionMemo effectiveVersionMemo; // (under the gate) the last version() call's question and answer (render_trims_v366.h)
constexpr std::size_t GenericShaderCacheLimit=8192; // distinct (address, length) ranges; the pack has 3642 shaders
GenericShaderCode genericShaderCode(std::uint32_t device,std::uint32_t vs,std::uint32_t ps) {
    genericShaderState.trim(GenericShaderCacheLimit); // before any pointer of this call is handed out
    const std::vector<std::uint8_t>* effective=nullptr;unsigned variant=0;
    const auto found=selectedVertexShaders.find(device);
    if(found!=selectedVertexShaders.end())if(const auto* selected=found->second.forObject(vs)){effective=&selected->code;variant=selected->variant;}
    // v47 (merge of v40 into v43): shader objects outside the guest heap (title image, physical memory) fall back
    // to the title-image and graphics readers, each with its own validation (v40); fallback bytes are held in a
    // per-call pool so every pointer stays valid for this call.
    static thread_local std::array<std::vector<std::uint8_t>,32> fallbackPool;
    unsigned& fallbackUsed=::recomp::v362::tls<::recomp::v362::Slot::ShaderFallback,unsigned>([]() noexcept -> unsigned& {static thread_local unsigned fallbackUsed;return fallbackUsed;});
    fallbackUsed=0;
    const auto heap=[&fallbackUsed](std::uint32_t a,std::uint32_t n)->const std::uint8_t* {
        if(const auto* p=inlineRegistry->viewSerializedGuestHeap(a,n))return p;
        if(a>=0x20000000)if(const auto* p=inlineRegistry->viewSerializedGraphicsData(a,n))return p;
        if(fallbackUsed>=fallbackPool.size())return nullptr;
        auto b=inlineRegistry->copySerializedTitleImage(a,n);
        if(!b || b->size()!=n)return nullptr;
        auto& slot=fallbackPool[fallbackUsed++];slot=std::move(*b);return slot.data();
    };
    const auto data=[](std::uint32_t a,std::uint32_t n){return inlineRegistry->viewSerializedGraphicsData(a,n);};
    auto code=recomp::gpu::hot::collectShaderCode(vs,ps,effective,variant,heap,data,
        [](const std::vector<std::uint8_t>& code){return recomp::gpu::generic::vertexShaderKnown(code);},genericShaderState);
    // v78: a content version for the selected binding's patched copy (rewritten in place by every re-selection), by
    // (device, vertex object): same bytes as last time, same version.
    if(code.effective){genericEffectiveVersions.trim(GenericShaderCacheLimit);
        code.effectiveVersion=recomp::v366::effectiveVersion(genericEffectiveVersions,effectiveVersionMemo, // v366d
            recomp_v366_render_trims2.load(std::memory_order_relaxed),vertexSelectionSerial,(std::uint64_t{device}<<32)|vs,code.effective);
    }
    return code;
}
// v78: the code versions and the in-place device shadow of a generic draw (DrawInput v78 fields). identity/effective
// fall back to each other exactly as the callers pick the vectors. diag-generic-copy-state: v71's shadow copy.
// v335 (exact): the validated view of a device's shadow [device, device+13860), reused for the device-relative reads of the hook
// bodies while the registry's generation word (generation << 16 | open windows) is unchanged and no window is open: the snapshot
// rule fastValidatedView applies, and a range inside a valid heap range is valid (the bytes are the same guest memory). A changed
// word, an open window or a range outside the shadow takes the ordinary view. Per thread (any thread may run a hook body).
// diag-v335-baseline = every read through viewSerializedGuestHeap, as before.
extern "C" const std::atomic<std::uint64_t>* recomp_registry_generation_word(const void* registry) noexcept;
bool v335Baseline() {
    static unsigned seen=0;static bool on=false;
    if(switchEpochChanged(seen))on=access(RECOMP_APP_PATH("/private-trace/diag-v335-baseline"),F_OK)==0;
    return on;
}
struct DeviceShadowView {std::uint32_t device=0;std::uint64_t word=~0ull;const std::uint8_t* base=nullptr;};
const std::uint8_t* deviceRange(std::uint32_t device,std::uint32_t address,std::uint32_t bytes) {
    static const std::atomic<std::uint64_t>* const generationWord=inlineRegistry?recomp_registry_generation_word(inlineRegistry):nullptr;
    if(!generationWord || !device || address<device || std::uint64_t{address}+bytes>std::uint64_t{device}+13860 || v335Baseline())
        return inlineRegistry->viewSerializedGuestHeap(address,bytes);
    const auto word=generationWord->load(std::memory_order_acquire);
    if(word&0xFFFFu)return inlineRegistry->viewSerializedGuestHeap(address,bytes); // a window is open: as fastValidatedView
    auto& cache=::recomp::v362::tls<::recomp::v362::Slot::ShadowView,DeviceShadowView>([]() noexcept -> DeviceShadowView& {static thread_local DeviceShadowView cache;return cache;});
    if(cache.device!=device || cache.word!=word || !cache.base) {
        cache.base=inlineRegistry->viewSerializedGuestHeap(device,13860);cache.device=device;cache.word=word;
        if(!cache.base)return inlineRegistry->viewSerializedGuestHeap(address,bytes);
    }
    return cache.base+(address-device);
}
bool v329bBaseline() { // v329b: diag-v329b-baseline (checked every 4096th call)
    static unsigned seen=0;static bool on=false; // (v330a: per switch epoch)
    if(switchEpochChanged(seen))on=access(RECOMP_APP_PATH("/private-trace/diag-v329b-baseline"),F_OK)==0;
    return on;
}
bool v329cBaseline() { // v329c: diag-v329c-baseline (checked every 4096th call)
    static unsigned seen=0;static bool on=false; // (v330a: per switch epoch)
    if(switchEpochChanged(seen))on=access(RECOMP_APP_PATH("/private-trace/diag-v329c-baseline"),F_OK)==0;
    return on;
}
bool v323Baseline() { // v323: diag-v323-baseline = a new vector per inline draw's vertex copy (checked every 1024th inline draw)
    static unsigned seen=0;static bool on=false; // (v330a: per switch epoch)
    if(switchEpochChanged(seen))on=access(RECOMP_APP_PATH("/private-trace/diag-v323-baseline"),F_OK)==0;
    return on;
}
bool v281Baseline() { // v281: diag-v281-baseline (checked every 1024th inline draw)
    static unsigned seen=0;static bool on=false; // (v330a: per switch epoch)
    if(switchEpochChanged(seen))on=access(RECOMP_APP_PATH("/private-trace/diag-v281-baseline"),F_OK)==0;
    return on;
}
bool genericStateInPlace() {
    static unsigned checks=0,seen=0;static bool inPlace=true;
    if(switchEpochChanged(seen) && ++checks) { // (v330a: per switch epoch; checks counts the reads, 1 = the first)
        const bool copy=access(RECOMP_APP_PATH("/private-trace/diag-generic-copy-state"),F_OK)==0;
        if(copy==inPlace || checks==1)rendererLog("RENDER_GENERIC_V78_HOOKS state_in_place=%u code_versions=1\n",unsigned(!copy));
        inPlace=!copy;
    }
    return inPlace;
}
void genericCodeVersions(recomp::gpu::generic::DrawInput& in,const GenericShaderCode& code) {
    in.vertexIdentityVersion=code.identity?code.identityGeneration:code.effectiveVersion;
    in.vertexEffectiveVersion=code.effective?code.effectiveVersion:code.identityGeneration;
    in.pixelVersion=code.pixel?code.pixelGeneration:0;
}
// v43: per-draw buffers reused across draws (touched only inside gate bodies, one draw at a time).
struct GenericDrawScratch {
    std::vector<std::uint8_t> state;            // the device shadow (13,860 bytes)
    std::array<std::uint32_t,1024> vertexConstants{},pixelConstants{};
    recomp::gpu::generic::IndexVector indices; // (v214)
    std::vector<std::uint8_t> inlineVertices; // v323: the inline draw's vertex copy, reused (the packet swaps its previous buffer back)
    recomp::gpu::generic::IndexVector v366Indices; // v366f: genericDrawVertices' index buffer, reused (the packet swaps its previous one back)
    std::uint64_t vertexVersion=0,pixelVersion=0; // v123: genericDrawConstants' banks' content versions (0: assembled/shadow)
    std::uint64_t vertexSource=0,pixelSource=0;   // v276: an in-place bank's Gta5DriverConstants identity and dirty mask (0 / nullptr: none)
    std::array<std::uint64_t,16>* vertexDirty=nullptr;std::array<std::uint64_t,16>* pixelDirty=nullptr;
    void constantsInto(recomp::gpu::generic::DrawInput& in)const { // v123 versions + v276 sources and dirty masks
        in.vertexConstantsVersion=vertexVersion;in.pixelConstantsVersion=pixelVersion;
        in.vertexConstantsSource=vertexSource;in.pixelConstantsSource=pixelSource;
        in.vertexConstantsDirty=vertexDirty;in.pixelConstantsDirty=pixelDirty;
    }
};
// v362 (fast_tls_v362.h): a thread_local with a destructor costs two resolver calls per access (its TLS guard first)
GenericDrawScratch& genericDrawScratch(){
    return ::recomp::v362::tls<::recomp::v362::Slot::DrawScratch,GenericDrawScratch>([]() noexcept -> GenericDrawScratch& {static thread_local GenericDrawScratch s;return s;});
}
// A draw's float constants: the device shadow's banks (+1920 vertex, +6016 pixel) with the submitted
// constants applied over them, the v35 decode + Gta5DriverConstants::apply; the shadow's alone when the
// submitted constants are unavailable. A fully submitted bank is used in place.
std::pair<const std::array<std::uint32_t,1024>*,const std::array<std::uint32_t,1024>*> genericDrawConstants(
        std::uint32_t device,const std::uint8_t* state,GenericDrawScratch& s) {
    flushInlineConstants(device); // v80
    const auto constants=driverConstants.find(device);
    if(driverConstantsValid && constants!=driverConstants.end()) {
        const auto* v=&constants->second.effectiveBank(0,state+1920,s.vertexConstants);
        const auto* p=&constants->second.effectiveBank(1,state+6016,s.pixelConstants);
        hot.bankInPlace+=unsigned(v!=&s.vertexConstants)+unsigned(p!=&s.pixelConstants);
        hot.bankAssembled+=unsigned(v==&s.vertexConstants)+unsigned(p==&s.pixelConstants);
        // v123: an in-place bank's content version (DrawInput::vertexConstantsVersion); an assembled one has none.
        s.vertexVersion=v!=&s.vertexConstants?constants->second.bankVersion(0):0;
        s.pixelVersion=p!=&s.pixelConstants?constants->second.bankVersion(1):0;
        s.vertexSource=s.vertexVersion?constants->second.sourceId():0;s.vertexDirty=s.vertexVersion?constants->second.dirtyMask(0):nullptr; // v276
        s.pixelSource=s.pixelVersion?constants->second.sourceId():0;s.pixelDirty=s.pixelVersion?constants->second.dirtyMask(1):nullptr;
        return {v,p};
    }
    recomp::gpu::Gta5DriverConstants::decodeBank(state+1920,s.vertexConstants);
    recomp::gpu::Gta5DriverConstants::decodeBank(state+6016,s.pixelConstants);
    hot.bankShadowOnly+=2;s.vertexVersion=s.pixelVersion=0; // v123
    s.vertexSource=s.pixelSource=0;s.vertexDirty=s.pixelDirty=nullptr; // v276
    return {&s.vertexConstants,&s.pixelConstants};
}
#endif

bool captureInline(std::uint32_t device,std::uint32_t stack,std::uint32_t link,
        std::uint32_t r20,std::uint32_t r21) {
    auto& pendingSlot=pendingInlineRef(); // v362
    if(!inlineRegistry || !inlinePrograms || !inlineGate || device!=pendingSlot.device)return false;
    const auto pending=pendingSlot;pendingSlot={};
    const auto index=inlineDrawCount++;
    std::optional<recomp::gpu::Gta5InlineDraw> captured;
    int vsRole=-1,psRole=-1;bool additionalDraw=false,nativeDrawCompleted=false;
    const auto gate=gateRef([&] {
#if (defined(__ANDROID__) || defined(__APPLE__))
        recomp::gpu::NativeEdram32Mutation physicalMutation(liveEdram32[device]);
        recomp::gpu::NativePreservedSampleToken preservedSamples;
        LiveTarget* live=nullptr;
        const auto activeTarget=liveTargets.find(device);
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
        // v15 show-frames: draw into the presentation image even after an earlier skip left it
        // invalid, so the frame on screen is the one the game is rendering now.
        if(activeTarget!=liveTargets.end() && (activeTarget->second.valid || activeTarget->second.image)) {
#else
        if(activeTarget!=liveTargets.end() && activeTarget->second.valid) {
#endif
            live=&activeTarget->second;live->valid=false;
        }
#endif
        const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
        const auto data=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
        // Only surface headers may use the current worker's ownership-checked
        // stack reader. Textures/shader bytes retain their graphics-only reader.
        const auto surfaceArgument=[&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);};
        const recomp::gpu::Gta5TextureFetchReader submittedFetch=[&](unsigned slot) {
            const auto it=driverTextures.find(device);
            return it==driverTextures.end()?std::optional<std::array<std::uint32_t,6>>{}:it->second.fetch(slot);
        };
        // v43: the heap copy's validation in place (one word read, no 4-byte vector per call).
        const auto word=[&](std::uint32_t a)->std::optional<std::uint32_t> {
            const auto* p=deviceRange(device,a,4);if(!p)return std::nullopt; // (v335)
            return recomp::gpu::hot::be32(p);
        };
        const auto vs=word(device+13072),ps=word(device+13068);
        if(pending.nativeAllocation) {
            const auto buffer=nativeInlineBuffers.find(device);
            if(buffer==nativeInlineBuffers.end() || !buffer->second.busy ||
               word(device+13848)!=buffer->second.address ||
               (pending.indexCount && word(device+13852)!=buffer->second.indexAddress))
                throw std::runtime_error("Native inline buffer no longer matches its BeginVertices owner");
        }
        if(!vs || !ps)return;
        const auto match=[&](unsigned stage,const auto& bytes) {
            if(bytes)for(unsigned i=0;i<inlinePrograms->identities().size();++i) {
                const auto& key=inlinePrograms->identities()[i];
                if(key.stage==stage && key.bigEndianCode==*bytes)return static_cast<int>(i);
            }
            return -1;
        };
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
        // v43: on the generic inline path the two roles below only feed the sampled RENDER_INLINE_INPUT
        // line after the gate; read the codes for them only when that line is written.
        const bool genericInline=pending.nativeAllocation && genericReady();
        const bool rolesNeeded=!genericInline || index<4096 || !(index&1023);
#else
        const bool rolesNeeded=true;
#endif
        std::optional<std::vector<std::uint8_t>> pixelCode,vertexCode;
        if(rolesNeeded) {
        const auto pixel=recomp::gpu::describeGta5ShaderCode(*ps,1,0,word);
        if(pixel){pixelCode=data(pixel->virtualAddress,pixel->byteCount);psRole=match(1,pixelCode);}
        // Cached rebinds do not call the patch routine. Match the exact
        // selected version, copied at the original shader-load boundary.
        const auto found=selectedVertexShaders.find(device);
        if(found!=selectedVertexShaders.end())if(const auto* selected=found->second.forObject(*vs)) {
            vertexCode=selected->code;vsRole=match(0,vertexCode);
        }
        }
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
        if(genericInline) {
            additionalDraw=true;nativeDrawCompleted=true; // a refused generic draw is a skipped draw
            // v20: own capture (captureGta5InlineDraw caps inline vertex data at 256 KB).
            captured.reset();
            // v281: the device state read in place through the validated heap view (the bound-indexed hook's rule;
            // diag-generic-copy-state: the copy), the constants pointed at genericDrawConstants' banks with their sources
            // and dirty masks (the v276 delta chain), and no Gta5InlineDraw (two zeroed 4 KB constant arrays moved per draw).
            // v280 uncapped outdoor render profile: these copies and the capture's whole-bank copies of inline draws ~2.5% of
            // the render thread. diag-v281-baseline: v280's copies (below).
            if(!v281Baseline()) {
            const bool inPlace=genericStateInPlace();
            std::optional<std::vector<std::uint8_t>> stateCopy;const std::uint8_t* stateBytes=nullptr;
            if(inPlace)stateBytes=deviceRange(device,device,13860); // (v335)
            else if((stateCopy=heap(device,13860)) && stateCopy->size()==13860)stateBytes=stateCopy->data();
            std::optional<std::vector<std::uint8_t>> inlineVertices;
            std::vector<std::uint8_t>* inlineBuffer=nullptr; // v323: the same validated bytes, copied into a reused buffer
            if(stateBytes) {
                const auto field=[&](unsigned at){return recomp::gpu::hot::be32(stateBytes+at);};
                const std::uint64_t bytes=std::uint64_t{pending.count}*pending.stride;
                const auto address=field(13848);
                if(pending.count && pending.stride && bytes<=32ull*1024*1024 && std::uint64_t{field(13856)}*4>=bytes && address) {
                    if(!v323Baseline()) {
                        if(const auto* raw=inlineRegistry->viewSerializedGraphicsData(address,static_cast<std::uint32_t>(bytes))) {
                            auto& b=genericDrawScratch().inlineVertices;b.assign(raw,raw+bytes);inlineBuffer=&b;
                        }
                    } else if(auto vertices=data(address,static_cast<std::uint32_t>(bytes));vertices && vertices->size()==bytes)inlineVertices=std::move(vertices);
                }
            }
            const bool haveCapture=inlineBuffer || inlineVertices.has_value();
            // v43: the shadow's constants with the submitted ones over them in one pass (v35 decoded all
            // 2048 words, then applied every submitted one over them); the same values.
            recomp::gpu::generic::SkinConstantsInfo skinInfo; // v80
            bool skinSkip=false;
            const std::array<std::uint32_t,1024>* inlineVc=nullptr;const std::array<std::uint32_t,1024>* inlinePc=nullptr;
            if(haveCapture) { // (the banks in place, or genericDrawScratch's assembled copies: valid until the draw below)
                auto& s=genericDrawScratch();
                std::tie(inlineVc,inlinePc)=genericDrawConstants(device,stateBytes,s);
                skinSkip=genericSkinConstants(skinInfo);
            }
            const char* why="capture";
            const auto code=genericShaderCode(device,*vs,*ps);
            // v41: missing pixel code no longer skips the draw: the renderer draws it with no fragment
            // stage when it writes no colour (depth-only mode or a zero colour mask), else refuses it
            // ("pixel-shader-unavailable"). v35 skipped every such draw as "shader-code".
            if(haveCapture && !code.pixel)genericNote("inline-draw-pixel",code.pixelWhy?code.pixelWhy:"?");
            // v48b: files/private-trace/diag-generic-skip-null-ps restores v47 (skip draws without pixel code),
            // checked at the first inline draw and every 1024th after (device bisection of the v51 livelock).
            static unsigned nullPixelCheck=0,nullPixelSeen=0;static bool skipNullPixel=false;
            if(switchEpochChanged(nullPixelSeen) && ++nullPixelCheck) { // (v330a)
                const bool skip=access(RECOMP_APP_PATH("/private-trace/diag-generic-skip-null-ps"),F_OK)==0;
                if(skip!=skipNullPixel || nullPixelCheck==1)genericNote("inline-null-ps-mode",skip?"skipped-v47":"drawn-v41");
                skipNullPixel=skip;
            }
            if(haveCapture && (code.identity || code.effective) && (code.pixel || !skipNullPixel)) {
                recomp::gpu::generic::DrawInput in;
                if(inPlace)in.stateBytes=stateBytes;else in.state=&*stateCopy;
                in.scissor=driverScissors[device];
                in.vertexConstants=inlineVc;in.pixelConstants=inlinePc;
                genericDrawScratch().constantsInto(in); // (v281: the banks themselves: sources and dirty masks as the bound hooks)
                in.fetch=[&](unsigned slot){return submittedFetch(slot);};
                in.vertexIdentity=code.identity?code.identity:code.effective;
                in.vertexEffective=code.effective?code.effective:code.identity;
                in.vertexAlternates=code.alternates;
                in.pixelCode=code.pixel;in.primitive=pending.primitive;in.vertexCount=pending.count;
                in.inlineVertices=inlineBuffer?inlineBuffer:&*inlineVertices;in.inlineStride=pending.stride;
                genericCodeVersions(in,code); // v78
                bool indicesOk=true;
                auto& scratch=genericDrawScratch();
                in.indices.swap(scratch.indices); // v43: reused buffer, returned below
                struct Return {recomp::gpu::generic::IndexVector& to;recomp::gpu::generic::IndexVector& from;~Return(){to.swap(from);}} give{scratch.indices,in.indices};
                if(pending.indexCount) {
                    const unsigned size=pending.indexFormat==1?2:4;
                    const auto address=word(device+13852);
                    // v43: the copy's validation in place; converted straight from the guest bytes.
                    const auto* raw=address?graphicsIndexView(*address,pending.indexCount*size):nullptr; // (index view, odd 16-bit start)
                    if(raw) {
                        in.indices.resize(pending.indexCount);
                        recomp::gpu::hot::bigEndianIndicesRebased(raw,pending.indexCount,size==4,pending.minVertex,in.indices.data());
                        in.indicesFinal=true;
                    } else {indicesOk=false;why="inline-indices";}
                } else in.indices.clear();
                in.skin=&skinInfo; // v80
                in.disposable=true; // v111: the indices and the inline vertices are this call's (taken, not copied, when pipelined)
                if(skinSkip){indicesOk=false;why="skin-constants";}
                if(indicesOk)recomp::gpu::generic::draw(in,genericDataReader(),&why);
            } else if(haveCapture)why="shader-code";
            if(!code.identity)genericNote("inline-draw-identity",code.effective?"original-code-unreadable":"no-vertex-code");
            genericNote("inline-draw",why);
            // v48: only skips before the renderer (v41 draws a missing pixel shader as depth-only; the
            // renderer logs its own refusal when that draw writes colour).
            if((!haveCapture || !(code.identity || code.effective) || std::strcmp(why,"inline-indices")==0) &&
               recomp::gpu::generic::censusActive()) {
                char detail[192];std::snprintf(detail,sizeof detail,"why=%s vs=%08x ps=%08x prim=%u count=%u stride=%u indices=%u identity=%u effective=%u pixel=%u captured=%u",
                    why,*vs,*ps,pending.primitive,pending.count,pending.stride,pending.indexCount,unsigned(code.identity!=nullptr),
                    unsigned(code.effective!=nullptr),unsigned(code.pixel!=nullptr),unsigned(haveCapture));
                recomp::gpu::generic::censusNote("inline-draw",detail);
            }
            return;
            }
            if(auto snapshot=heap(device,13860);snapshot && snapshot->size()==13860) {
                const auto field=[&](unsigned at){return genericBe32(*snapshot,at);};
                const std::uint64_t bytes=std::uint64_t{pending.count}*pending.stride;
                const auto address=field(13848);
                if(pending.count && pending.stride && bytes<=32ull*1024*1024 && std::uint64_t{field(13856)}*4>=bytes && address)
                    if(auto vertices=data(address,static_cast<std::uint32_t>(bytes));vertices && vertices->size()==bytes) {
                        recomp::gpu::Gta5InlineDraw draw;draw.device=device;draw.primitive=pending.primitive;
                        draw.vertexCount=pending.count;draw.stride=pending.stride;draw.vertexAddress=address;
                        draw.vertexShader=field(13072);draw.pixelShader=field(13068);draw.vertices=std::move(*vertices);
                        draw.deviceState=std::move(*snapshot);
                        captured=std::move(draw);
                    }
            }
            // v43: the shadow's constants with the submitted ones over them in one pass (v35 decoded all
            // 2048 words, then applied every submitted one over them); the same values.
            recomp::gpu::generic::SkinConstantsInfo skinInfo; // v80
            bool skinSkip=false;
            if(captured) {
                auto& s=genericDrawScratch();
                const auto [v,p]=genericDrawConstants(device,captured->deviceState.data(),s);
                captured->vertexConstants=*v;captured->pixelConstants=*p;
                skinSkip=genericSkinConstants(skinInfo);
            }
            const char* why="capture";
            const auto code=genericShaderCode(device,*vs,*ps);
            // v41: missing pixel code no longer skips the draw: the renderer draws it with no fragment
            // stage when it writes no colour (depth-only mode or a zero colour mask), else refuses it
            // ("pixel-shader-unavailable"). v35 skipped every such draw as "shader-code".
            if(captured && !code.pixel)genericNote("inline-draw-pixel",code.pixelWhy?code.pixelWhy:"?");
            // v48b: files/private-trace/diag-generic-skip-null-ps restores v47 (skip draws without pixel code),
            // checked at the first inline draw and every 1024th after (device bisection of the v51 livelock).
            static unsigned nullPixelCheck=0,nullPixelSeen=0;static bool skipNullPixel=false;
            if(switchEpochChanged(nullPixelSeen) && ++nullPixelCheck) { // (v330a)
                const bool skip=access(RECOMP_APP_PATH("/private-trace/diag-generic-skip-null-ps"),F_OK)==0;
                if(skip!=skipNullPixel || nullPixelCheck==1)genericNote("inline-null-ps-mode",skip?"skipped-v47":"drawn-v41");
                skipNullPixel=skip;
            }
            if(captured && (code.identity || code.effective) && (code.pixel || !skipNullPixel)) {
                recomp::gpu::generic::DrawInput in;
                in.state=&captured->deviceState;in.scissor=driverScissors[device];
                in.vertexConstants=&captured->vertexConstants;in.pixelConstants=&captured->pixelConstants;
                {const auto& sc=genericDrawScratch();in.vertexConstantsVersion=sc.vertexVersion;in.pixelConstantsVersion=sc.pixelVersion;} // v123 (copied as is)
                // (v276: no source / dirty mask: the draw reads captured's copies, not the banks the masks describe)
                in.fetch=[&](unsigned slot){return submittedFetch(slot);};
                in.vertexIdentity=code.identity?code.identity:code.effective;
                in.vertexEffective=code.effective?code.effective:code.identity;
                in.vertexAlternates=code.alternates;
                in.pixelCode=code.pixel;in.primitive=pending.primitive;in.vertexCount=pending.count;
                in.inlineVertices=&captured->vertices;in.inlineStride=pending.stride;
                genericCodeVersions(in,code); // v78
                bool indicesOk=true;
                auto& scratch=genericDrawScratch();
                in.indices.swap(scratch.indices); // v43: reused buffer, returned below
                struct Return {recomp::gpu::generic::IndexVector& to;recomp::gpu::generic::IndexVector& from;~Return(){to.swap(from);}} give{scratch.indices,in.indices};
                if(pending.indexCount) {
                    const unsigned size=pending.indexFormat==1?2:4;
                    const auto address=word(device+13852);
                    // v43: the copy's validation in place; converted straight from the guest bytes.
                    const auto* raw=address?graphicsIndexView(*address,pending.indexCount*size):nullptr; // (index view, odd 16-bit start)
                    if(raw) {
                        in.indices.resize(pending.indexCount);
                        recomp::gpu::hot::bigEndianIndicesRebased(raw,pending.indexCount,size==4,pending.minVertex,in.indices.data());
                        in.indicesFinal=true;
                    } else {indicesOk=false;why="inline-indices";}
                } else in.indices.clear();
                in.skin=&skinInfo; // v80
                in.disposable=true; // v111: the indices and captured->vertices are this call's (taken, not copied, when pipelined)
                if(skinSkip){indicesOk=false;why="skin-constants";}
                if(indicesOk)recomp::gpu::generic::draw(in,genericDataReader(),&why);
            } else if(captured)why="shader-code";
            if(!code.identity)genericNote("inline-draw-identity",code.effective?"original-code-unreadable":"no-vertex-code");
            genericNote("inline-draw",why);
            // v48: only skips before the renderer (v41 draws a missing pixel shader as depth-only; the
            // renderer logs its own refusal when that draw writes colour).
            if((!captured || !(code.identity || code.effective) || std::strcmp(why,"inline-indices")==0) &&
               recomp::gpu::generic::censusActive()) {
                char detail[192];std::snprintf(detail,sizeof detail,"why=%s vs=%08x ps=%08x prim=%u count=%u stride=%u indices=%u identity=%u effective=%u pixel=%u captured=%u",
                    why,*vs,*ps,pending.primitive,pending.count,pending.stride,pending.indexCount,unsigned(code.identity!=nullptr),
                    unsigned(code.effective!=nullptr),unsigned(code.pixel!=nullptr),unsigned(captured.has_value()));
                recomp::gpu::generic::censusNote("inline-draw",detail);
            }
            return;
        }
#endif
        const recomp::gpu::NativeTranslatedDrawProgram* additional=nullptr;
#if __has_include("native_additional_shader_pack.h")
        if(vertexCode && pixelCode)for(const auto& p:nativeAdditionalPrograms)
            if(p.indexed==bool(pending.indexCount) && p.vertexIdentity==*vertexCode && p.pixelIdentity==*pixelCode){additional=&p;break;}
#endif
#if __has_include("native_offscreen_shader_pack.h")
        if(!additional && vertexCode && pixelCode) {
            const auto& p=nativeOffscreenProgram;
            if(p.indexed==bool(pending.indexCount) && p.vertexIdentity==*vertexCode && p.pixelIdentity==*pixelCode)additional=&p;
        }
#endif
        const auto preserveStoppedDraw=[&] {
            if(firstUnsupportedInline)return;
            firstUnsupportedInline.emplace();auto& stopped=*firstUnsupportedInline;
            stopped.pending=pending;stopped.vertexObject=*vs;stopped.pixelObject=*ps;
            stopped.submittedTextures=recomp::gpu::captureGta5SubmittedTextures(submittedFetch);
            if(vertexCode)stopped.vertexCode=*vertexCode;
            if(pixelCode)stopped.pixelCode=*pixelCode;
            if(auto b=heap(*vs,1024))stopped.vertexResource=std::move(*b);
            if(auto b=heap(*ps,1024))stopped.pixelResource=std::move(*b);
            if(auto b=heap(device,13860))stopped.deviceState=std::move(*b);
#if (defined(__ANDROID__) || defined(__APPLE__))
            stopped.physicalAtStop=recomp::gpu::captureNativeStoppedPhysical32(
                physicalMutation.candidate(),stopped.deviceState);
#endif
            stopped.surfaceEvidence=recomp::gpu::captureGta5StoppedSurfaceEvidence(
                stopped.deviceState,stack,link,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
            stopped.queryEvidence=recomp::gpu::captureGta5StoppedQueryEvidence(
                device,r20,r21,stopped.surfaceEvidence,
                [&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);},heap,data);
            if(stopped.queryEvidence && latestQueryCommandTrace) {
                stopped.queryCommands=latestQueryCommandTrace;
                stopped.queryCommands->stopped(*stopped.queryEvidence,stopped.deviceState,data);
            }
            stopped.surfaces=recomp::gpu::captureGta5BoundSurfaces(stopped.deviceState,heap,
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},40);
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_gamma_snapshot_module.h")
            if(stopped.surfaces)stopped.colorSamples=captureCompletedColorSamples(device,stopped.surfaces->surfaces[0].object,preservedSamples);
#endif
            if(pending.indexCount) {
                stopped.indexAddress=word(device+13852).value_or(0);
                stopped.indexWords=word(device+13860).value_or(0);
                const auto bytes=std::uint64_t{pending.indexCount}*((pending.indexFormat&4)?4:2);
                if(bytes<=NativeInlineBytes && (bytes+3)/4==stopped.indexWords) {
                    if(auto b=data(stopped.indexAddress,static_cast<std::uint32_t>(bytes)))
                        stopped.indices=std::move(*b);
                }
                rendererLog("RENDER_STOPPED_INDEXED count=%u format=%u min_vertex=%u address=%08x words=%u bytes=%zu\n",
                    pending.indexCount,pending.indexFormat,pending.minVertex,stopped.indexAddress,
                    stopped.indexWords,stopped.indices.size());
            }
            const auto ownedData=[&](auto a,auto n) {
                auto bytes=data(a,n);
                if(!bytes)rendererLog("RENDER_STOPPED_INPUT unavailable_address=%08x bytes=%u\n",a,n);
                return bytes;
            };
            stopped.draw=recomp::gpu::captureGta5StoppedInlineDraw(device,pending.primitive,
                pending.count,pending.stride,{},heap,ownedData);
            rendererLog("RENDER_STOPPED_INPUT vertices=%u stride=%u address=%08x words=%u captured=%u constants=%u\n",
                pending.count,pending.stride,word(device+13848).value_or(0),word(device+13856).value_or(0),
                unsigned(stopped.draw.has_value()),unsigned(driverConstantsValid && driverConstants.count(device)));
            if(!stopped.draw)return;
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(driverConstantsValid && constants!=driverConstants.end())constants->second.apply(*stopped.draw);
            stopped.draw->scissorRegisters=driverScissors[device];
            // Diagnostic residency only; actual execution still selects slots
            // from the effective shader, and requires their full owned data.
            std::size_t total=0;std::uint32_t mipCaptured=0,raw10Captured=0,expanded16Captured=0;
            for(unsigned slot=0;slot<32;++slot) {
                const auto texture=recomp::gpu::captureGta5StoppedInlineDraw(device,pending.primitive,
                    pending.count,pending.stride,{slot},heap,data,submittedFetch);
                if(!texture) {
                    // Diagnostics need all six actual owned faces, not just
                    // the first64KiB page prefix. This does not admit the draw.
                    const auto fetch=submittedFetch(slot);
                    auto cube=fetch?recomp::gpu::captureNativeCubeBase(slot,*fetch,data):std::nullopt;
                    if(cube && total+cube->source.size()<=64u*1024*1024) {
                        total+=cube->source.size();stopped.draw->textures.push_back(std::move(*cube));
                        stopped.textureCaptured|=1u<<slot;continue;
                    }
                    // Preserve full recognized mip planes too. Separate from
                    // single-level draw textures: residency is NOT evidence of
                    // initialization, sampling, or successful draw admission.
                    const auto description=fetch?recomp::gpu::XenosTextureDescription::decode(*fetch):std::nullopt;
                    auto mip=description && description->mipMax?
                        recomp::gpu::captureNativeMipTexture(*fetch,data,64u*1024*1024-total):std::nullopt;
                    if(mip) {
                        total+=mip->base.size()+mip->mips.size();mipCaptured|=1u<<slot;
                        stopped.mipTextures.emplace_back(slot,std::move(*mip));continue;
                    }
                    // Full recognized raw10 storage, separately labelled as
                    // diagnostic bytes. Never insert this unsupported format
                    // into a native draw or mark its sampling as available.
                    auto raw10=fetch?recomp::gpu::captureNativeRaw10Base(
                        slot,*fetch,data,64u*1024*1024-total):std::nullopt;
                    if(raw10) {
                        total+=raw10->source.size();raw10Captured|=1u<<slot;
                        stopped.raw10Textures.push_back(std::move(*raw10));
                    }
                    // The completed7e3 copy is consumed as format29 EXPAND.
                    // Preserve its full owned storage, but retain unavailable
                    // sampling and the original refusal. No draw admission.
                    auto expanded=fetch?recomp::gpu::captureNativeExpanded16Base(
                        slot,*fetch,data,64u*1024*1024-total):std::nullopt;
                    if(expanded) {
                        total+=expanded->source.size();expanded16Captured|=1u<<slot;
                        stopped.expanded16Textures.push_back(std::move(*expanded));
                    }
                    stopped.textureUnavailable|=1u<<slot;continue;
                }
                if(total+texture->textures[0].source.size()>64u*1024*1024) {
                    stopped.textureUnavailable|=1u<<slot;continue;
                }
                total+=texture->textures[0].source.size();
                stopped.draw->textures.push_back(texture->textures[0]);stopped.textureCaptured|=1u<<slot;
            }
            // Preserve owned storage evidence for unsupported layouts too.
            // Bounded pages do NOT admit texture sampling or prove full mips.
            stopped.submittedTexturePages=recomp::gpu::captureGta5SubmittedTexturePages(
                stopped.submittedTextures,stopped.textureCaptured|mipCaptured|raw10Captured|expanded16Captured,data);
        };

        // DIAGNOSTIC SKIP BUILD ONLY: log, then treat the refused draw as done
        // (EndVertices releases its buffer) and keep a previously valid display.
        const auto diagnosticSkipDraw=[&](unsigned line,const char* reason) {
            recordDiagnosticSkippedDraw(line,reason,vertexCode,pixelCode,pending.primitive,pending.count,pending.stride,
                pending.indexCount,word(device+12816).value_or(0),word(device+10460).value_or(0),word(device+10548).value_or(0));
            nativeDrawCompleted=true;
#if (defined(__ANDROID__) || defined(__APPLE__))
            if(live)live->valid=true;
#endif
        };
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
        if(tileRecordingDevice.load()) {
            preserveStoppedDraw();stopPendingTileRecording("inline-draw");
        }
#endif
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_fixed16_rectangle_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeFixed16RectangleProgram.vertexIdentity==*vertexCode && nativeFixed16RectangleProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            nativeDrawCompleted=captured && bindings &&
                drawFixed16Target(device,nativeFixed16RectangleProgram,*captured,{},*bindings,physicalMutation);
            if(!nativeDrawCompleted) {
                diagnosticSkipDraw(835u,"Fixed16 rectangle color/depth draw did not complete");
            }
            ++liveDraws;return;
        }
#endif
#if (defined(__ANDROID__) || defined(__APPLE__))
        #if __has_include("native_query_neutral_fan_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           queryNeutralFanProgram.vertexIdentity==*vertexCode && queryNeutralFanProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto image=[&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);};
            const auto caller=captured?recomp::gpu::captureGta5StoppedSurfaceEvidence(captured->deviceState,stack,link,surfaceArgument):
                recomp::gpu::Gta5StoppedSurfaceEvidence{};
            const auto query=captured?recomp::gpu::captureGta5StoppedQueryEvidence(device,r20,r21,caller,image,heap,data):std::nullopt;
            const auto preserved=captured && query?recomp::gpu::Gta5PendingQueryNeutralFan::capture(*captured,*query,data):std::nullopt;
            const char* why="neutral-fan-current-query-bytes-unavailable";
            const auto proof=inlineGraphics && captured && preserved?
                inlineGraphics->executeQueryNeutralFan(queryNeutralFanProgram,*captured,*vertexCode,*pixelCode,&why):std::nullopt;
            nativeDrawCompleted=proof && preserved->unchanged(*captured,image,heap,data);
            if(nativeDrawCompleted) {
                physicalMutation.commit();if(live)live->valid=true;++liveDraws;
            } else if(proof)why="neutral-fan-pending-bytes-changed";
            rendererLog("RENDER_API_QUERY_NEUTRAL_FAN vertices=%u captured=%u completed=%u plane=%u stage=%s "
                "target_writes=0 query_result=0 query_events_consumed=0 fence_advance=0 ring_advance=0\n",
                pending.count,unsigned(captured.has_value()),unsigned(nativeDrawCompleted),proof?proof->plane:UINT32_MAX,why);
            if(!nativeDrawCompleted){diagnosticSkipDraw(866u,"Native query-neutral fan proof failed; query events remain pending");}
            return;
        }
        #endif
        #ifdef RECOMP_POST_SAMPLED_STENCIL_REJECTION
        if(!pending.indexCount && vertexCode && pixelCode &&
           *vertexCode==nativePostSampledStencilIdentity.vertex && *pixelCode==nativePostSampledStencilIdentity.pixel) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            if(captured)captured->scissorRegisters=driverScissors[device];
            const auto bindings=captured?recomp::gpu::captureGta5ArgumentBoundSurfaces(
                captured->deviceState,heap,data,surfaceArgument):std::nullopt;
            const bool queryIdle=inlineGraphics && inlineGraphics->logicalVisibilityIdle();
            const char* stage="capture-unavailable";
            recomp::gpu::NativePostSampledStencilProof proof;
            const bool proved=captured && bindings && recomp::gpu::proveNativePostSampledStencilRejection(
                *captured,*bindings,nativePostSampledStencilIdentity,*vertexCode,*pixelCode,false,queryIdle,
                physicalMutation.candidate(),proof,&stage);
            nativeDrawCompleted=recomp::gpu::finishNativeReadOnlyDraw(proved,physicalMutation,live?&live->valid:nullptr);
            if(nativeDrawCompleted)++liveDraws;
            rendererLog("RENDER_API_POST_SAMPLED_STENCIL_REJECT captured=%u query_owner_idle=%u examined=%u passing=%u "
                "owner_next_serial=%llu completed=%u stage=%s executed=0 guest_published=0 physical_content_changed=0 query_result=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(queryIdle),proof.examined,proof.passing,
                static_cast<unsigned long long>(proof.ownerNextSerial),unsigned(nativeDrawCompleted),stage);
            if(!nativeDrawCompleted){diagnosticSkipDraw(890u,"Post-sampled stencil proof failed; actual passing draw remains unimplemented");}
            return;
        }
        #endif
        #if __has_include("native_stencil_reject_identity.h") || __has_include("native_conditional_stencil_reject_identity.h")
        const recomp::gpu::NativeStencilRejectedRectangleIdentity* rejectedIdentity=nullptr;
        if(vertexCode && pixelCode) {
            #if __has_include("native_stencil_reject_identity.h")
            if(*vertexCode==nativeStencilRejectIdentity.vertex && *pixelCode==nativeStencilRejectIdentity.pixel)
                rejectedIdentity=&nativeStencilRejectIdentity;
            #endif
            #if __has_include("native_conditional_stencil_reject_identity.h")
            if(*vertexCode==nativeConditionalStencilRejectIdentity.vertex && *pixelCode==nativeConditionalStencilRejectIdentity.pixel)
                rejectedIdentity=&nativeConditionalStencilRejectIdentity;
            #endif
        }
        if(!pending.indexCount && rejectedIdentity) {
            // No saved stencil image participates: the proof consumes this
            // device's current fenced physical owner while holding the gate.
            // One passing/unknown sample requires an actual ABI27 renderer.
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            if(captured)captured->scissorRegisters=driverScissors[device];
            const auto bindings=captured?recomp::gpu::captureGta5ArgumentBoundSurfaces(
                captured->deviceState,heap,data,surfaceArgument):std::nullopt;
            const bool queryIdle=inlineGraphics && inlineGraphics->logicalVisibilityIdle();
            const char* stage="capture-unavailable";
            recomp::gpu::NativeStencilRejectedRectangleProof proof;
            const bool proved=captured && bindings && recomp::gpu::proveNativeStencilRejectedRectangle(
                *captured,*bindings,*rejectedIdentity,*vertexCode,*pixelCode,false,queryIdle,
                physicalMutation.candidate(),proof,&stage);
            nativeDrawCompleted=recomp::gpu::finishNativeReadOnlyDraw(proved,physicalMutation,live?&live->valid:nullptr);
            if(nativeDrawCompleted)++liveDraws;
            rendererLog("RENDER_API_STENCIL_REJECT_RECTANGLE captured=%u query_owner_idle=%u examined=%u passing=%u "
                "owner_next_serial=%llu completed=%u stage=%s executed=0 guest_published=0 physical_content_changed=0 profile=%u\n",
                unsigned(captured.has_value()),unsigned(queryIdle),proof.examined,proof.passing,
                static_cast<unsigned long long>(proof.ownerNextSerial),unsigned(nativeDrawCompleted),stage,
                unsigned(rejectedIdentity->profile));
            if(!nativeDrawCompleted){diagnosticSkipDraw(928u,"Native stencil rejection proof failed; actual colour draw remains unimplemented");}
            return;
        }
        #endif
        #if __has_include("native_clipped_scene_fan_shader_pack.h")
        const recomp::gpu::NativeClippedFanProgram* selectedSceneFan=nullptr;
        auto sceneFanProfile=recomp::gpu::NativeSceneFanProfile::Textured684;
        if(!pending.indexCount && vertexCode && pixelCode &&
           sceneFanProgram.vertexIdentity==*vertexCode && sceneFanProgram.pixelIdentity==*pixelCode)
            selectedSceneFan=&sceneFanProgram;
        #ifdef RECOMP_SCENE_FAN_SECONDARY_PROGRAM
        if(!pending.indexCount && vertexCode && pixelCode &&
           sceneFanSecondaryProgram.vertexIdentity==*vertexCode && sceneFanSecondaryProgram.pixelIdentity==*pixelCode) {
            selectedSceneFan=&sceneFanSecondaryProgram;
            sceneFanProfile=recomp::gpu::NativeSceneFanProfile::Untextured708;
        }
        #endif
        if(selectedSceneFan) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bound=captured?recomp::gpu::captureGta5ArgumentBoundSurfaces(
                captured->deviceState,heap,data,surfaceArgument):std::nullopt;
            const char* why="scene-fan-inputs-unavailable";
            const auto proof=inlineGraphics && captured && bound?
                inlineGraphics->executeClippedSceneFan(*selectedSceneFan,*captured,*bound,*vertexCode,*pixelCode,
                    physicalMutation,&why,sceneFanProfile):std::nullopt;
            nativeDrawCompleted=proof.has_value();
            if(nativeDrawCompleted) {
                if(live)live->valid=true; // Restore ONLY the previously valid, untouched view.
                ++liveDraws;
            }
            rendererLog("RENDER_API_SCENE_FAN_EXCLUDED vertices=%u stride=%u captured=%u completed=%u "
                "plane=%u stage=%s target_writes=0 sampled_textures=0 query_result=0 ring_advance=0 profile=%u\n",
                pending.count,pending.stride,unsigned(captured.has_value()),unsigned(nativeDrawCompleted),
                proof.value_or(UINT32_MAX),why,unsigned(sceneFanProfile));
            if(!nativeDrawCompleted) {
                diagnosticSkipDraw(968u,"Native scene fan current exclusion failed; original inputs preserved");
            }
            return;
        }
        #endif
        #if __has_include("native_clipped_fan_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeClippedFanProgram.vertexIdentity==*vertexCode && nativeClippedFanProgram.pixelIdentity==*pixelCode) {
            // Ordinary complete-primitive rejection, not a replacement image or
            // successful empty submit. Every original position executes on GPU;
            // Either all W are conservatively negative, or a common homogeneous
            // XY outside plane and >=2px scissor separation are required. The
            // latter proof remains linear when W crosses zero.
            // Compiler rejected VS/PS memory exports and other side
            // effects. A visible primitive requires the separately validated
            // mip/gamma writer below; otherwise preserve its inputs and stop.
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const bool state=captured && recomp::gpu::nativeClippedFanState(*captured,nativeClippedFanProgram.stride);
            const auto plane=live && inlineGraphics && state?
                inlineGraphics->executeClippedFan(nativeClippedFanProgram,*captured):std::nullopt;
            nativeDrawCompleted=plane.has_value();
            if(nativeDrawCompleted){live->valid=true;++liveDraws;}
            rendererLog("RENDER_API_CLIPPED_FAN vertices=%u stride=%u captured=%u state=%u completed=%u plane=%u target_writes=0\n",
                pending.count,pending.stride,unsigned(captured.has_value()),unsigned(state),unsigned(nativeDrawCompleted),plane.value_or(UINT32_MAX));
            #if __has_include("native_gamma_mip_fan_shader_pack.h")
            if(!nativeDrawCompleted && captured && nativeGammaMipFanProgram.draw.vertexIdentity==*vertexCode &&
               nativeGammaMipFanProgram.draw.pixelIdentity==*pixelCode) {
                const auto fetch=submittedFetch(7);
                const auto texture=fetch?recomp::gpu::captureNativeMipTexture(*fetch,data):std::nullopt;
                const auto use=texture?mipTextureUse(texture->fetch):std::nullopt;
                const auto bindings=recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40);
                const auto object=bindings?bindings->surfaces[0].object:0;
                const auto key=std::make_pair(device,object);const auto found=clearedColor8Targets.find(key);
                bool profile=false;std::uint64_t serial=0;
                if(live && inlineGraphics && texture && use && bindings && found!=clearedColor8Targets.end()) {
                    auto& target=found->second;
                    profile=target.valid && target.image && bindings->surfaces[0].header &&
                        *bindings->surfaces[0].header==target.header && object!=live->object &&
                        !recomp::gpu::color8ClearIntersects(target.image->plan(),720,720) &&
                        recomp::gpu::nativeGammaFanState(*captured,nativeGammaMipFanProgram.draw,target.image->plan(),*bindings,&*texture);
                    if(profile) {
                        const auto& plan=target.image->plan();
                        preservedSamples={target.image.get(),object,target.contentSerial,target.image->diagnosticMutationSerial()};
                        target.valid=false;
                        for(auto& [otherKey,other]:clearedColor8Targets)if(other.image && otherKey!=key &&
                            recomp::gpu::color8ClearPlansOverlap(other.image->plan(),plan))other.valid=false;
                        for(auto& [otherKey,other]:offscreenTargets)if(other.image &&
                            recomp::gpu::color8ClearIntersects(plan,other.image->plan().baseTile,other.image->plan().tileCount))other.valid=false;
                        invalidateFixed16Overlaps(plan);
                        nativeDrawCompleted=inlineGraphics->executeGammaFan(*target.image,nativeGammaMipFanProgram,*captured,*bindings,&*texture);
                        if(nativeDrawCompleted) {
                            target.valid=true;serial=++target.contentSerial;live->valid=true;++liveDraws;
                            live->completedMipTextures[use->baseAddress]=*use;
                        }
                    }
                }
                rendererLog("RENDER_API_GAMMA_MIP_FAN object=%08x vertices=%u captured_texture=%u owned_planes=%u profile=%u completed=%u serial=%llu base_bytes=%zu mip_bytes=%zu guest_published=0\n",
                    object,pending.count,unsigned(texture.has_value()),unsigned(use.has_value()),unsigned(profile),unsigned(nativeDrawCompleted),
                    static_cast<unsigned long long>(serial),texture?texture->base.size():0,texture?texture->mips.size():0);
            }
            #endif
            if(!nativeDrawCompleted){diagnosticSkipDraw(1034u,"Native fan clipping or mip/gamma draw requirements failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_gamma_fan_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeGammaFanProgram.draw.vertexIdentity==*vertexCode && nativeGammaFanProgram.draw.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            // The same side-effect-free solid shader may be behind another
            // cubemap face. Prove clipping before requiring a convex visible
            // fan or touching its color/depth target; preserve original pixels.
            const auto clipped=live && inlineGraphics && captured?
                inlineGraphics->executeClippedFan(nativeGammaFanProgram.positions,*captured):std::nullopt;
            if(clipped) {
                nativeDrawCompleted=true;live->valid=true;++liveDraws;
                rendererLog("RENDER_API_GAMMA_FAN_CLIPPED vertices=%u plane=%u target_writes=0 live_ready=%u\n",pending.count,*clipped,unsigned(live->valid));
                return;
            }
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            const auto object=bindings?bindings->surfaces[0].object:0;
            const auto key=std::make_pair(device,object);const auto found=clearedColor8Targets.find(key);
            bool state=false;std::uint64_t serial=0;
            if(live && inlineGraphics && captured && bindings && found!=clearedColor8Targets.end()) {
                auto& target=found->second;
                state=target.valid && target.image && bindings->surfaces[0].header &&
                    *bindings->surfaces[0].header==target.header && object!=live->object &&
                    !recomp::gpu::color8ClearIntersects(target.image->plan(),720,720) &&
                    recomp::gpu::nativeGammaFanState(*captured,nativeGammaFanProgram.draw,target.image->plan(),*bindings);
                if(state) {
                    const auto& plan=target.image->plan();
                    preservedSamples={target.image.get(),object,target.contentSerial,target.image->diagnosticMutationSerial()};
                    target.valid=false;
                    for(auto& [otherKey,other]:clearedColor8Targets)if(other.image && otherKey!=key &&
                        recomp::gpu::color8ClearPlansOverlap(other.image->plan(),plan))other.valid=false;
                    for(auto& [otherKey,other]:offscreenTargets)if(other.image &&
                        recomp::gpu::color8ClearIntersects(plan,other.image->plan().baseTile,other.image->plan().tileCount))other.valid=false;
                    invalidateFixed16Overlaps(plan);
                    nativeDrawCompleted=inlineGraphics->executeGammaFan(*target.image,nativeGammaFanProgram,*captured,*bindings);
                    if(nativeDrawCompleted){target.valid=true;serial=++target.contentSerial;live->valid=true;++liveDraws;}
                }
            }
            rendererLog("RENDER_API_GAMMA_FAN object=%08x vertices=%u stride=%u state=%u completed=%u serial=%llu samples=2 source_clamped=1 guest_published=0\n",
                object,pending.count,pending.stride,unsigned(state),unsigned(nativeDrawCompleted),static_cast<unsigned long long>(serial));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1082u,"Native gamma fan state/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_gamma_quad_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeGammaQuadProgram.draw.vertexIdentity==*vertexCode && nativeGammaQuadProgram.draw.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                recomp::gpu::nativeTranslatedTextureSlots(nativeGammaQuadProgram.draw),heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            // Exact original side-effect-free shader pair; all original GPU
            // positions must complete. Clip only when EVERY quad is proven
            // absent. Do not change target owners/content, publish pixels or
            // record texture consumption for a draw with no surviving samples.
            const auto clipped=live && inlineGraphics && captured?
                inlineGraphics->executeClippedGammaQuads(nativeGammaQuadProgram,*captured):std::nullopt;
            if(clipped) {
                nativeDrawCompleted=true;live->valid=true;++liveDraws;
                rendererLog("RENDER_API_GAMMA_QUADS_CLIPPED vertices=%u behind=%u scissor=%u degenerate=%u target_writes=0 live_ready=%u\n",
                    pending.count,clipped->behind,clipped->scissorExcluded,clipped->degenerate,unsigned(live->valid));
                return;
            }
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            const auto object=bindings?bindings->surfaces[0].object:0;
            const auto key=std::make_pair(device,object);const auto found=clearedColor8Targets.find(key);
            const recomp::gpu::NativeTranslatedDrawProgram* persistentIndexed=nullptr;
#if __has_include("native_persistent_quad_shader_pack.h")
            persistentIndexed=&nativePersistentQuadProgram;
#endif
            bool state=false;std::uint64_t serial=0;
            if(live && inlineGraphics && captured && bindings && found!=clearedColor8Targets.end()) {
                auto& target=found->second;
                state=target.valid && target.image && (target.image->isDepthStencilUniform() || persistentIndexed) && bindings->surfaces[0].header &&
                    *bindings->surfaces[0].header==target.header && object!=live->object &&
                    !recomp::gpu::color8ClearIntersects(target.image->plan(),720,720) &&
                    recomp::gpu::nativeGammaQuadState(*captured,nativeGammaQuadProgram.draw,target.image->plan(),*bindings);
                if(state) {
                    const auto& plan=target.image->plan();
                    preservedSamples={target.image.get(),object,target.contentSerial,target.image->diagnosticMutationSerial()};
                    target.valid=false;
                    for(auto& [otherKey,other]:clearedColor8Targets)if(other.image && otherKey!=key &&
                        recomp::gpu::color8ClearPlansOverlap(other.image->plan(),plan))other.valid=false;
                    for(auto& [otherKey,other]:offscreenTargets)if(other.image &&
                        recomp::gpu::color8ClearIntersects(plan,other.image->plan().baseTile,other.image->plan().tileCount))other.valid=false;
                    invalidateFixed16Overlaps(plan);
                    nativeDrawCompleted=inlineGraphics->executeGammaQuads(*target.image,nativeGammaQuadProgram,*captured,*bindings,persistentIndexed);
                    if(nativeDrawCompleted){target.valid=true;serial=++target.contentSerial;live->valid=true;recordCompletedTextures(*live,*captured);++liveDraws;}
                }
            }
            rendererLog("RENDER_API_GAMMA_QUADS object=%08x vertices=%u stride=%u state=%u completed=%u serial=%llu samples=2 depth_truncate=1 guest_published=0\n",
                object,pending.count,pending.stride,unsigned(state),unsigned(nativeDrawCompleted),static_cast<unsigned long long>(serial));
            if(!nativeDrawCompleted) {
                const auto* target=found!=clearedColor8Targets.end()?&found->second:nullptr;
                const bool image=target && target->image;
                rendererLog("RENDER_GAMMA_QUAD_REQUIREMENTS live=%u graphics=%u capture=%u bindings=%u owner=%u owner_valid=%u image=%u depth_uniform=%u header_match=%u state_profile=%u\n",
                    unsigned(live!=nullptr),unsigned(inlineGraphics!=nullptr),unsigned(captured.has_value()),unsigned(bindings.has_value()),
                    unsigned(target!=nullptr),unsigned(target && target->valid),unsigned(image),unsigned(image && target->image->isDepthStencilUniform()),
                    unsigned(target && bindings && bindings->surfaces[0].header && *bindings->surfaces[0].header==target->header),
                    unsigned(image && captured && bindings && recomp::gpu::nativeGammaQuadState(*captured,nativeGammaQuadProgram.draw,target->image->plan(),*bindings)));
                diagnosticSkipDraw(1145u,"Native gamma quad state/coverage/depth/draw failed; original inputs preserved");
            }
            return;
        }
        #endif
        #if __has_include("native_sampled_stencil_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeSampledStencilProgram.vertexIdentity==*vertexCode && nativeSampledStencilProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            const auto capture=[&]() {
                auto result=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                    {1},heap,data,submittedFetch);
                flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
                if(result && driverConstantsValid && constants!=driverConstants.end()) {
                    constants->second.apply(*result);result->scissorRegisters=driverScissors[device];
                } else result.reset();
                return result;
            };
            captured=capture();
            const auto bindings=captured?recomp::gpu::captureGta5ArgumentBoundSurfaces(captured->deviceState,heap,data,surfaceArgument):std::nullopt;
            const char* stage="capture-unavailable";
            const auto plan=captured && bindings?recomp::gpu::planNativeSampledStencil(*captured,nativeSampledStencilProgram,*bindings,&stage):std::nullopt;
            // live is non-null only when the presentation owner was valid on
            // entry. Base0..719 depth writes are disjoint from its720..1439.
            const bool displayPreviouslyValid=live!=nullptr;
            if(plan && inlineGraphics) {
                invalidatePhysicalNativeViews(device,plan->target.depthBase,720);
                nativeDrawCompleted=inlineGraphics->executeSampledStencil(nativeSampledStencilProgram,*captured,*bindings,
                    *vertexCode,*pixelCode,physicalMutation,[&] {
                        const auto now=capture();
                        const auto bound=now?recomp::gpu::captureGta5ArgumentBoundSurfaces(now->deviceState,heap,data,surfaceArgument):std::nullopt;
                        return now && bound && recomp::gpu::sameNativeSampledStencilInputs(*captured,*bindings,*now,*bound);
                    },&stage);
                if(nativeDrawCompleted){if(live)live->valid=displayPreviouslyValid;++liveDraws;}
            }
            rendererLog("RENDER_API_SAMPLED_STENCIL vertices=%u slot=1 stencil_mask=10 alpha_le_zero=1 state=%u completed=%u stage=%s display_valid=%u query_result=0 ring_advance=0\n",
                pending.count,unsigned(plan.has_value()),unsigned(nativeDrawCompleted),stage,unsigned(live && live->valid));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1182u,"Native sampled stencil state/texture/physical draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_stencil_only_shader_pack.h")
        // ABI25: direct, single-sample stencil writes consume CURRENT physical
        // depth, not retained earlier tiled scene pixels. Missing coverage fails.
        if(!pending.indexCount && vertexCode && !pixelCode &&
           nativeStencilOnlyProgram.vertexIdentity==*vertexCode) {
            additionalDraw=true;
            // The absent pixel stage is opt-in at the capture, and this is the
            // only band entitled to ask for it.
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                {},heap,data,{},true);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5ArgumentBoundSurfaces(captured->deviceState,heap,data,surfaceArgument):std::nullopt;
            const char* planWhy="not-evaluated";
            const auto plan=captured && bindings?
                recomp::gpu::planNativeStencilOnlyDraw(*captured,nativeStencilOnlyProgram,*bindings,&planWhy):std::nullopt;
            const char* stage=plan?"planned":planWhy;
            unsigned tiles=0,drawn=0;
            if(plan && inlineGraphics) {
                const auto viewport=recomp::gpu::nativeTranslatedViewport(*captured);
                const auto scissor=recomp::gpu::nativeTranslatedScissor(*captured);
                std::vector<std::uint8_t> input(8208+std::uint64_t{captured->vertexCount}*captured->stride,0);
                const auto word=[&](unsigned at,std::uint32_t value) {
                    for(unsigned b=0;b<4;++b)input[at+b]=static_cast<std::uint8_t>(value>>(24-8*b));
                };
                word(0,captured->vertexCount);word(4,0);word(8,0);word(12,0);
                for(unsigned i=0;i<1024;++i){word(16+4*i,captured->vertexConstants[i]);word(4112+4*i,captured->pixelConstants[i]);}
                std::memcpy(input.data()+8208,captured->vertices.data(),captured->vertices.size());
                if(!viewport || !scissor)stage="viewport-scissor";
                else {
                    stage="physical-depth-incomplete";
                    nativeDrawCompleted=recomp::gpu::executeNativeEdram32Stencil(physicalMutation,
                        plan->depthBase,plan->pitch,plan->width,plan->height,
                        [&](const auto& before)->std::optional<std::vector<std::uint32_t>> {
                            tiles=(plan->pitch/80)*(plan->height/16);
                            invalidatePhysicalNativeViews(device,plan->depthBase,tiles);
                            stage="direct-depth-import";
                            auto target=inlineGraphics->createDirectDepthTarget(*plan,before);
                            if(!target)return std::nullopt;
                            stage="direct-stencil-draw";
                            if(!inlineGraphics->executeDirectStencil(*target,nativeStencilOnlyProgram,input,
                                    captured->vertexCount,*viewport,*scissor))return std::nullopt;
                            drawn=1;stage="direct-depth-readback";
                            return target->readWords();
                        });
                    if(nativeDrawCompleted){stage="physical-stencil-published";++liveDraws;}
                }
            }
            rendererLog("RENDER_API_STENCIL_ONLY vertices=%u stride=%u colour=%08x depth=%08x width=%u height=%u "
                "depth_base=%u stencil_write_mask=%u tiles=%u drawn=%u state=%u completed=%u stage=%s guest_published=0\n",
                pending.count,pending.stride,plan?plan->colorObject:0,plan?plan->depthObject:0,
                plan?plan->width:0,plan?plan->height:0,plan?plan->depthBase:0,
                plan?plan->state.front.writeMask:0,tiles,drawn,unsigned(plan.has_value()),
                unsigned(nativeDrawCompleted),stage);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1242u,"Native stencil only state/target/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_offset_sample_shader_pack.h")
        // ABI23: a nonindexed rectangle list writing ONE colour target and
        // sampling ONE slot four times at a constant texel offset -- the
        // half-resolution downsample. The declared slot is selected out of the
        // nine textures the game leaves bound; `filter` is logged so the
        // sampled constant's own filter fields are visible in one line.
        // The pyramid uses two pairs: a four-tap one with two interpolators and
        // an eight-tap one with four. They share every predicate, so the branch
        // selects the matching program and runs a single path.
        const recomp::gpu::NativeOffsetSampleProgram* offsetSampleProgram=nullptr;
        if(vertexCode && pixelCode) {
            if(nativeOffsetSampleProgram.draw.vertexIdentity==*vertexCode &&
               nativeOffsetSampleProgram.draw.pixelIdentity==*pixelCode)
                offsetSampleProgram=&nativeOffsetSampleProgram;
            #if defined(RECOMP_HAVE_OFFSET_SAMPLE2)
            else if(nativeOffsetSample2Program.draw.vertexIdentity==*vertexCode &&
                    nativeOffsetSample2Program.draw.pixelIdentity==*pixelCode)
                offsetSampleProgram=&nativeOffsetSample2Program;
            #endif
            #if defined(RECOMP_HAVE_TONEMAP)
            // ABI24: the same draw, a richer pair. It joins the selection here
            // rather than getting a branch of its own precisely because every
            // predicate below already holds for it.
            else if(nativeTonemapProgram.draw.vertexIdentity==*vertexCode &&
                    nativeTonemapProgram.draw.pixelIdentity==*pixelCode)
                offsetSampleProgram=&nativeTonemapProgram;
            #endif
            #if defined(RECOMP_HAVE_TONEMAP2)
            // The tonemap pass's second pair: three interpolators instead of
            // two, a different sampled slot, and the same target.
            else if(nativeTonemap2Program.draw.vertexIdentity==*vertexCode &&
                    nativeTonemap2Program.draw.pixelIdentity==*pixelCode)
                offsetSampleProgram=&nativeTonemap2Program;
            #endif
            #if defined(RECOMP_HAVE_TONEMAP3)
            // Its third is an ABI23 pair, not an ABI24 one: it needs none of
            // the tonemap arithmetic and writes an ordinary RGBA8 target, so it
            // must not carry the k_16_16 encode scale. The disjoint target
            // formats caught that -- an ABI24 pack of it was refused on device
            // at target-colour-format with target_format=0.
            else if(nativeTonemap3Program.draw.vertexIdentity==*vertexCode &&
                    nativeTonemap3Program.draw.pixelIdentity==*pixelCode)
                offsetSampleProgram=&nativeTonemap3Program;
            #endif
        }
        if(!pending.indexCount && offsetSampleProgram) {
            const auto& offsetSample=*offsetSampleProgram;
            additionalDraw=true;
            // Capture only the slot this pair actually samples. The game leaves
            // nine textures bound here; asking for all of them would fail on the
            // ones whose fetch constants are unavailable and lose the draw.
            // No submittedFetch: this band's sampled slot is NOT in the driver's
            // uploaded set -- the device recorded it in the unavailable mask --
            // so its fetch constant is read from the guest's own block in device
            // state, which is where the game wrote it. Passing the driver reader
            // would fail the whole capture on a slot the driver never submitted.
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                recomp::gpu::nativeTranslatedTextureSlots(offsetSample.draw),heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            const char* planWhy="not-evaluated";
            const auto plan=captured && bindings?
                recomp::gpu::planNativeOffsetSampleDraw(*captured,offsetSample.draw,*bindings,&planWhy):std::nullopt;
            const char* stage=plan?"planned":planWhy;
            unsigned filter=0,slotFormat=0,targetColour=0,targetFormat=0;
            if(captured)for(const auto& texture:captured->textures)
                if(texture.slot==offsetSample.draw.textureSlot) {
                    filter=(texture.fetch[3]>>19)&63;slotFormat=texture.fetch[1]&63;
                }
            // Report the target's own colour word even when a predicate refuses.
            // The stopped-draw capture does not preserve surface headers, so this
            // line is the only place the value is observable.
            if(bindings && bindings->surfaces[0].header && bindings->surfaces[0].header->size()==40) {
                const auto& h=*bindings->surfaces[0].header;
                targetColour=(std::uint32_t{h[28]}<<24)|(std::uint32_t{h[29]}<<16)|
                    (std::uint32_t{h[30]}<<8)|h[31];
                targetFormat=(targetColour>>16)&0xFu;
            }
            if(plan && inlineGraphics) {
                const auto key=std::make_pair(device,plan->object);
                if(!(offsetSampleTargets.count(key) || offsetSampleTargets.size()<8))stage="target-capacity";
                else {
                    auto& entry=offsetSampleTargets[key];entry.valid=false;
                    if(!entry.image || entry.image->plan()!=*plan)
                        entry.image=inlineGraphics->createOffscreenDrawTarget(*plan);
                    invalidatePhysicalNativeViews(device,plan->baseTile,plan->tileCount);
                    const bool rawColour=plan->format==0 || plan->format==14 || plan->format==7;
                    if(!entry.image)stage="target-image-create";
                    else nativeDrawCompleted=(rawColour || physicalMutation.candidate().invalidate(plan->baseTile,plan->tileCount)) &&
                        inlineGraphics->executeOffsetSample(
                        *entry.image,offsetSample,*captured,*bindings,&stage);
                    if(nativeDrawCompleted && rawColour) {
                        const auto bytes=entry.image->readRgba();
                        if(!bytes || !recomp::gpu::recordCompletedNativeColorDraw(physicalMutation,*plan,*bytes,
                            recomp::gpu::NativeEdram64Model::Xenia40x16)) {
                            nativeDrawCompleted=false;stage="raw-colour-physical-publication";
                        }
                    }
                    if(nativeDrawCompleted) {
                        physicalMutation.commit();
                        entry.header=*bindings->surfaces[0].header;entry.valid=true;
                        recordCompletedTextures(liveTargets[device],*captured);++liveDraws;
                    }
                }
            }
            rendererLog("RENDER_API_OFFSET_SAMPLE object=%08x width=%u height=%u base=%u tiles=%u "
                "slot=%u filter=%u source_format=%u target_colour=%08x target_format=%u "
                "bound_textures=%zu state=%u completed=%u stage=%s guest_published=0 edram64_model=xenia40x16\n",
                plan?plan->object:0,plan?plan->width:0,plan?plan->height:0,
                plan?plan->baseTile:0,plan?plan->tileCount:0,
                offsetSample.draw.textureSlot,filter,slotFormat,targetColour,targetFormat,
                captured?captured->textures.size():0,unsigned(plan.has_value()),
                unsigned(nativeDrawCompleted),stage);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1362u,"Native offset sample state/coverage/ownership/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_exposure_scalar_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeExposureScalarProgram.vertexIdentity==*vertexCode && nativeExposureScalarProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // The exact original vertex graph proved its texture read dead.
            // Capture only the used pixel source; never supply a fake sample.
            captured=recomp::gpu::captureNativeR32ScalarDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            // Plan establishes the alias footprint only; engine checks live
            // query/interrupt ownership separately before GPU execution.
            const auto exposurePlan=captured && bindings?recomp::gpu::planNativeExposureScalarDraw(
                *captured,*bindings,nativeExposureScalarProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            if(exposurePlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,exposurePlan->target.baseTile,exposurePlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executeExposureScalar(nativeExposureScalarProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_EXPOSURE_SCALAR captured=%u completed=%u stage=%s width=1 height=1 "
                "format=7 base=%u base_known=%u footprint_tiles=2 written_words=%u "
                "display_valid=0 guest_published=0 ring_advance=0\n",unsigned(captured.has_value()),
                unsigned(nativeDrawCompleted),stage,exposurePlan?exposurePlan->target.baseTile:0u,
                unsigned(exposurePlan.has_value()),nativeDrawCompleted?2u:0u);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1396u,"Native exposure state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_expanded16_copy_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeExpanded16CopyProgram.vertexIdentity==*vertexCode && nativeExpanded16CopyProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // PostFX pass71: slot5 is the resolved bright-pass output (EXPAND).
            captured=recomp::gpu::captureNativeExpanded16CopyDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            const auto copyPlan=captured && bindings?recomp::gpu::planNativeExpanded16CopyDraw(
                *captured,*bindings,nativeExpanded16CopyProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            if(copyPlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,copyPlan->target.baseTile,copyPlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executeExpanded16Copy(nativeExpanded16CopyProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_BLOOM_DOWNSAMPLE captured=%u completed=%u stage=%s width=%u height=%u "
                "format=3 base=%u base_known=%u tiles=%u display_valid=0 guest_published=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                copyPlan?copyPlan->target.width:0u,copyPlan?copyPlan->target.height:0u,
                copyPlan?copyPlan->target.baseTile:0u,unsigned(copyPlan.has_value()),
                copyPlan?copyPlan->target.tileCount:0u);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1428u,"Native bloom downsample state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_postfx_blur_shader_pack.h")
        // Same shared post VS as ABI41: dispatch is on the exact pixel identity,
        // so neither route can shadow the other.
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativePostfxBlurProgram.vertexIdentity==*vertexCode && nativePostfxBlurProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // PostFX pass81: slot5 is the resolved T2 bloom texture (EXPAND).
            captured=recomp::gpu::captureNativePostfxBlurDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            const auto blurPlan=captured && bindings?recomp::gpu::planNativePostfxBlurDraw(
                *captured,*bindings,nativePostfxBlurProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            if(blurPlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,blurPlan->target.baseTile,blurPlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executePostfxBlur(nativePostfxBlurProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_BLOOM_BLUR captured=%u completed=%u stage=%s width=%u height=%u "
                "format=3 base=%u base_known=%u tiles=%u display_valid=0 guest_published=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                blurPlan?blurPlan->target.width:0u,blurPlan?blurPlan->target.height:0u,
                blurPlan?blurPlan->target.baseTile:0u,unsigned(blurPlan.has_value()),
                blurPlan?blurPlan->target.tileCount:0u);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1462u,"Native bloom blur state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_postfx_vertical_blur_shader_pack.h")
        // ABI43 pass82 (vertical mirror of pass81): same shared VS, state, rows and
        // consumer; dispatch on its own exact pixel identity.
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativePostfxVerticalBlurProgram.vertexIdentity==*vertexCode && nativePostfxVerticalBlurProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // PostFX pass82: slot5 is the resolved T3 bloom texture (EXPAND).
            captured=recomp::gpu::captureNativePostfxBlurDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            const auto blurPlan=captured && bindings?recomp::gpu::planNativePostfxBlurDraw(
                *captured,*bindings,nativePostfxVerticalBlurProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            if(blurPlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,blurPlan->target.baseTile,blurPlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executePostfxBlur(nativePostfxVerticalBlurProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_BLOOM_VERTICAL_BLUR captured=%u completed=%u stage=%s width=%u height=%u "
                "format=3 base=%u base_known=%u tiles=%u display_valid=0 guest_published=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                blurPlan?blurPlan->target.width:0u,blurPlan?blurPlan->target.height:0u,
                blurPlan?blurPlan->target.baseTile:0u,unsigned(blurPlan.has_value()),
                blurPlan?blurPlan->target.tileCount:0u);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1496u,"Native bloom vertical blur state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_postfx_upsample_shader_pack.h")
        // ABI44 pass83 (additive upsample T2 80x45 -> T1 160x90): same shared VS.
        // Its 60-byte pixel shader has ABI41's size, so dispatch is on the exact
        // pixel identity (never size); neither route can shadow the other.
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativePostfxUpsampleProgram.vertexIdentity==*vertexCode && nativePostfxUpsampleProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // PostFX pass83: slot5 is the resolved T2 bloom texture (EXPAND); the
            // ONE/ONE add reads T1's physical words (its resolve's recorded clear).
            captured=recomp::gpu::captureNativePostfxUpsampleDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            const auto upsamplePlan=captured && bindings?recomp::gpu::planNativePostfxUpsampleDraw(
                *captured,*bindings,nativePostfxUpsampleProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            // Diagnostic only (no admission): whether the destination words were known on entry.
            const bool destinationKnown=upsamplePlan && physicalMutation.candidate().readKnownColorRows(
                upsamplePlan->target.baseTile,upsamplePlan->target.pitch,upsamplePlan->target.width,upsamplePlan->target.height).has_value();
            if(upsamplePlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,upsamplePlan->target.baseTile,upsamplePlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executePostfxUpsample(nativePostfxUpsampleProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_BLOOM_UPSAMPLE_ADD captured=%u completed=%u stage=%s width=%u height=%u "
                "format=3 base=%u base_known=%u tiles=%u blend=add destination_known=%u "
                "display_valid=0 guest_published=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                upsamplePlan?upsamplePlan->target.width:0u,upsamplePlan?upsamplePlan->target.height:0u,
                upsamplePlan?upsamplePlan->target.baseTile:0u,unsigned(upsamplePlan.has_value()),
                upsamplePlan?upsamplePlan->target.tileCount:0u,unsigned(destinationKnown));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1536u,"Native bloom additive upsample state/resources/destination/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_bright_pass_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeBrightPassProgram.vertexIdentity==*vertexCode && nativeBrightPassProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // Slots1/17: the exposure copy texel; slot10: the 640x360 scene.
            // The vertex texel read is the same fetch, evaluated per fragment.
            captured=recomp::gpu::captureNativeBrightPassDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            // Plan establishes the physical footprint only; the engine checks
            // live query/interrupt ownership separately before GPU execution.
            const auto brightPlan=captured && bindings?recomp::gpu::planNativeBrightPassDraw(
                *captured,*bindings,nativeBrightPassProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            if(brightPlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,brightPlan->target.baseTile,brightPlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executeBrightPass(nativeBrightPassProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_BRIGHT_PASS captured=%u completed=%u stage=%s width=320 height=180 "
                "format=3 base=%u base_known=%u tiles=%u display_valid=0 guest_published=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                brightPlan?brightPlan->target.baseTile:0u,unsigned(brightPlan.has_value()),
                brightPlan?brightPlan->target.tileCount:0u);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1570u,"Native bright pass state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_r32_scalar_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeR32ScalarProgram.vertexIdentity==*vertexCode && nativeR32ScalarProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureNativeR32ScalarDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            // Layout validation determines the physical cache range only.
            // executeR32Scalar independently checks the actual query/interrupt
            // owner before executing; true here never grants GPU admission.
            const auto scalarPlan=captured && bindings?recomp::gpu::planNativeR32ScalarDraw(
                *captured,*bindings,nativeR32ScalarProgram,*vertexCode,*pixelCode,false,true,&stage):std::nullopt;
            if(scalarPlan && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,scalarPlan->target.baseTile,scalarPlan->target.tileCount);
                nativeDrawCompleted=inlineGraphics->executeR32Scalar(nativeR32ScalarProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            const auto scalarScissor=captured?recomp::gpu::nativeTranslatedScissor(*captured):std::nullopt;
            rendererLog("RENDER_API_R32_SCALAR captured=%u completed=%u stage=%s width=%u height=%u "
                "format=14 base=%u base_known=%u tiles=1 physical_r32=%u padding_preserved=%u "
                "display_valid=0 guest_published=0 ring_advance=0\n",unsigned(captured.has_value()),
                unsigned(nativeDrawCompleted),stage,scalarScissor?(*scalarScissor)[2]:0u,scalarScissor?(*scalarScissor)[3]:0u,
                scalarPlan?scalarPlan->target.baseTile:0u,unsigned(scalarPlan.has_value()),
                unsigned(nativeDrawCompleted),unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1605u,"Native R32 scalar state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_expanded16_luma_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeExpanded16LumaProgram.vertexIdentity==*vertexCode && nativeExpanded16LumaProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureNativeExpanded16LumaDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            if(captured && bindings && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,1440,4);
                nativeDrawCompleted=inlineGraphics->executeExpanded16Luma(nativeExpanded16LumaProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_EXPANDED16_LUMA captured=%u completed=%u stage=%s width=64 height=64 "
                "format=14 base=1440 tiles=4 physical_r32=%u padding_preserved=%u "
                "display_valid=0 guest_published=0 ring_advance=0\n",unsigned(captured.has_value()),
                unsigned(nativeDrawCompleted),stage,unsigned(nativeDrawCompleted),unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1632u,"Native expanded16 luma state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_expanded16_max_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeExpanded16MaxProgram.vertexIdentity==*vertexCode && nativeExpanded16MaxProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureNativeExpanded16MaxDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-or-presentation-owner-unavailable";
            if(captured && bindings && inlineGraphics && activeTarget!=liveTargets.end() && activeTarget->second.ownsPresentation) {
                invalidatePhysicalNativeViews(device,720,48);
                nativeDrawCompleted=inlineGraphics->executeExpanded16Max(nativeExpanded16MaxProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(activeTarget->second,*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_EXPANDED16_MAX captured=%u completed=%u stage=%s width=320 height=180 "
                "format=3 base=720 tiles=48 physical_rgba7e3=%u padding_preserved=%u "
                "display_valid=0 guest_published=0 ring_advance=0\n",unsigned(captured.has_value()),
                unsigned(nativeDrawCompleted),stage,unsigned(nativeDrawCompleted),unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1659u,"Native expanded16 max state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_filter_weights_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeFilterWeightsProgram.vertexIdentity==*vertexCode && nativeFilterWeightsProgram.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureNativeFilterWeightsDraw(device,pending.primitive,pending.count,pending.stride,
                heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-unavailable";
            if(captured && bindings && inlineGraphics) {
                // Whole physical tiles have cached views, but only the actual
                // 640x360 words are replaced. The executor preserves padding
                // bytes AND their knownness in the candidate physical owner.
                invalidatePhysicalNativeViews(device,720,184);
                nativeDrawCompleted=inlineGraphics->executeFilterWeights(nativeFilterWeightsProgram,
                    *captured,*bindings,*vertexCode,*pixelCode,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(liveTargets[device],*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_FILTER_WEIGHTS captured=%u completed=%u stage=%s width=640 height=360 "
                "format=3 base=720 tiles=184 physical_rgba7e3=%u padding_preserved=%u "
                "display_valid=0 guest_published=0 ring_advance=0\n",
                unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                unsigned(nativeDrawCompleted),unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1690u,"Native filter-weights state/resources/coverage/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_screen_strip_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeScreenStripProgram.draw.vertexIdentity==*vertexCode &&
           nativeScreenStripProgram.draw.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                recomp::gpu::nativeTranslatedTextureSlots(nativeScreenStripProgram.draw),heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const char* stage="capture-unavailable";
            if(captured && bindings && inlineGraphics) {
                // Exact planner/consumer pins colour tiles720..1439. Cached
                // images of those bytes cease to be current BEFORE submission.
                // The production consumer independently requires a complete
                // current physical owner and proves grid coverage on the GPU.
                invalidatePhysicalNativeViews(device,720,720);
                nativeDrawCompleted=inlineGraphics->executeScreenStrip(
                    nativeScreenStripProgram,*captured,*bindings,physicalMutation,&stage);
                if(nativeDrawCompleted){recordCompletedTextures(liveTargets[device],*captured);++liveDraws;}
            }
            rendererLog("RENDER_API_SCREEN_STRIP vertices=%u stride=%u captured=%u completed=%u stage=%s "
                "physical_rgb7e3=%u preserved_depth_stencil_alpha=%u display_valid=0 guest_published=0 ring_advance=0\n",
                pending.count,pending.stride,unsigned(captured.has_value()),unsigned(nativeDrawCompleted),stage,
                unsigned(nativeDrawCompleted),unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1722u,"Native screen strip state/coverage/physical ownership failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_world_strip_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeWorldStripProgram.draw.vertexIdentity==*vertexCode &&
           nativeWorldStripProgram.draw.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            // Capture the mip chain separately, never flatten it through the
            // ordinary single-level inline texture reader.
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(
                captured->deviceState,surfaceArgument,data,40):std::nullopt;
            const auto textures=recomp::gpu::captureNativeWorldStripTextures(submittedFetch,data);
            std::vector<recomp::gpu::NativeMipTextureUse> mipUses;
            std::vector<std::pair<std::uint32_t,LiveTarget::TextureUse>> baseUses;
            bool owned=bool(textures);const char* stage="capture-unavailable";
            if(textures)for(const auto& t:*textures) {
                if(!t.mips.empty()) {
                    const auto use=mipTextureUse(t.fetch);
                    if(!use){owned=false;break;}mipUses.push_back(*use);
                } else {
                    const auto desc=recomp::gpu::XenosTextureDescription::decode(t.fetch);
                    const auto address=desc?textureStorageAddress(*desc,t.base.size()):std::nullopt;
                    if(!address){owned=false;break;}
                    baseUses.emplace_back(*address,LiveTarget::TextureUse{*desc,static_cast<std::uint32_t>(t.base.size())});
                }
            }
            if(!owned)stage="texture-allocation-owner";
            if(captured && bindings && owned && inlineGraphics) {
                invalidatePhysicalNativeViews(device,0,1440);
                nativeDrawCompleted=inlineGraphics->executeWorldStrip(
                    nativeWorldStripProgram,*captured,*bindings,*textures,physicalMutation,&stage);
                if(nativeDrawCompleted) {
                    auto& target=liveTargets[device];
                    for(const auto& use:mipUses)target.completedMipTextures[use.baseAddress]=use;
                    for(const auto& use:baseUses)target.completedTextures[use.first]=use.second;
                    ++liveDraws;
                }
            }
            rendererLog("RENDER_API_WORLD_STRIP vertices=%u stride=%u captured=%u textures=%u completed=%u stage=%s "
                "physical_rgb7e3_depth_stencil=%u display_valid=0 guest_published=0 ring_advance=0\n",
                pending.count,pending.stride,unsigned(captured.has_value()),unsigned(owned),unsigned(nativeDrawCompleted),stage,
                unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(1771u,"Native world strip state/resources/physical ownership failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_rectangle_mrt_shader_pack.h")
        // ABI22: a nonindexed rectangle list writing TWO colour attachments.
        // Its targets come from the LIVE registers, so unlike the composite it
        // does not consult the tile cache; the plan re-proves that from this
        // draw's own state and names the predicate that refuses.
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeRectangleMrtProgram.draw.vertexIdentity==*vertexCode &&
           nativeRectangleMrtProgram.draw.pixelIdentity==*pixelCode) {
            additionalDraw=true;
#if defined(RECOMP_DIAGNOSTIC_FAST_FRAMES)
            // Attempt v7 frame-rate mode (diagnostic builds only). This pass belongs to the 3D scene behind
            // the loading screen, whose inputs come from skipped draws in a diagnostic build; it cost ~42% of
            // the render thread (v5 plateau profile), which gates every main-thread frame. Skip it exactly
            // like a failed native draw.
            diagnosticSkipDraw(1839u,"Fast-frames diagnostic: rectangle MRT pass skipped; original inputs preserved");
            return;
#endif
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                recomp::gpu::nativeTranslatedTextureSlots(nativeRectangleMrtProgram.draw),heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            const char* planWhy="not-evaluated";
            const auto plan=captured && bindings?
                recomp::gpu::planNativeRectangleMrtDraw(*captured,nativeRectangleMrtProgram.draw,*bindings,&planWhy):std::nullopt;
            const char* mrtStage=plan?"planned":planWhy;
            if(plan && inlineGraphics) {
                const auto key=std::make_pair(device,plan->targets[0].object);
                if(!(rectangleMrtTargets.count(key) || rectangleMrtTargets.size()<8))mrtStage="target-capacity";
                else {
                    auto& entry=rectangleMrtTargets[key];entry.valid=false;
                    if(!entry.image || entry.image->plan()!=*plan)
                        entry.image=inlineGraphics->createRectangleMrtTarget(*plan);
                    // BOTH outputs, with their format-sized physical footprints.
                    bool ranges=true;
                    for(const auto& target:plan->targets) {
                        invalidatePhysicalNativeViews(device,target.baseTile,target.tileCount);
                        // RGBA16F explicitly uses the reference emulator's
                        //40x16 model. Publish only actual fenced bytes; this
                        // does not claim the hardware permutation is measured.
                        if(target.format!=14 && target.format!=0 && target.format!=7)
                            ranges=physicalMutation.candidate().invalidate(target.baseTile,target.tileCount) && ranges;
                    }
                    if(!entry.image)mrtStage="target-image-create";
                    else nativeDrawCompleted=ranges && inlineGraphics->executeRectangleMrt(
                        *entry.image,nativeRectangleMrtProgram,*captured,*bindings,&mrtStage);
                    if(nativeDrawCompleted)for(unsigned slot=0;slot<2;++slot) {
                        const auto& target=plan->targets[slot];
                        if(target.format!=14 && target.format!=0 && target.format!=7)continue;
                        const auto bytes=entry.image->readRgba(slot);
                        if(!bytes || !recomp::gpu::recordCompletedNativeColorDraw(physicalMutation,target,*bytes,
                            recomp::gpu::NativeEdram64Model::Xenia40x16)) {
                            nativeDrawCompleted=false;mrtStage="raw-colour-physical-publication";break;
                        }
                    }
                    if(nativeDrawCompleted) {
                        physicalMutation.commit();
                        for(unsigned i=0;i<2;++i)entry.headers[i]=*bindings->surfaces[i].header;
                        entry.valid=true;
                        recordCompletedTextures(liveTargets[device],*captured);++liveDraws;
                    }
                }
            }
            rendererLog("RENDER_API_RECTANGLE_MRT target0=%08x target1=%08x width=%u height=%u base0=%u base1=%u "
                "tiles=%u state=%u completed=%u stage=%s guest_published=0 edram64_model=xenia40x16\n",
                plan?plan->targets[0].object:0,plan?plan->targets[1].object:0,
                plan?plan->targets[0].width:0,plan?plan->targets[0].height:0,
                plan?plan->targets[0].baseTile:0,plan?plan->targets[1].baseTile:0,
                plan?plan->targets[0].tileCount:0,unsigned(plan.has_value()),
                unsigned(nativeDrawCompleted),mrtStage);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1839u,"Native rectangle MRT state/coverage/ownership/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_screen_shader_pack.h")
        // The later no-pixel use of this exact position-only vertex program is
        // a distinct contract, not an ABI26 colour draw or the old ABI21 case.
        // Its current fixed-function state must prove every output unchanged,
        // including exclusion of query participation under this engine owner.
        if(!pending.indexCount && vertexCode && !pixelCode && *ps==0 &&
           nativeScreenSpaceProgram.draw.vertexIdentity==*vertexCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                {},heap,data,{},true);
            if(captured)captured->scissorRegisters=driverScissors[device];
            const bool queriesIdle=inlineGraphics && inlineGraphics->logicalVisibilityIdle();
            const char* readOnlyStage="capture-unavailable";
            nativeDrawCompleted=captured && captured->vertexShader==*vs &&
                recomp::gpu::planNativeReadOnlyScreenRectangle(*captured,nativeScreenSpaceProgram.draw,*ps,
                    queriesIdle,&readOnlyStage);
            if(recomp::gpu::finishNativeReadOnlyDraw(nativeDrawCompleted,physicalMutation,live?&live->valid:nullptr))++liveDraws;
            rendererLog("RENDER_API_READ_ONLY_RECTANGLE vertices=%u stride=%u captured=%u query_owner_idle=%u "
                "completed=%u stage=%s executed=0 guest_published=0 physical_content_changed=0\n",
                pending.count,pending.stride,unsigned(captured.has_value()),unsigned(queriesIdle),
                unsigned(nativeDrawCompleted),readOnlyStage);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1864u,"Native read-only rectangle proof failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_depth_only_shader_pack.h")
        // A rectangle pass with NO pixel shader. The guest bound none and
        // disabled colour, depth and stencil writes, so this draw has no
        // output to reproduce: see native_inert_draw_plan.h. It is matched on
        // the vertex identity alone, because there is no pixel identity to
        // match, and the absent pixel object is required rather than assumed.
        if(!pending.indexCount && vertexCode && !pixelCode && *ps==0 &&
           nativeDepthOnlyProgram.vertexIdentity==*vertexCode) {
            additionalDraw=true;
            // This draw's vertex buffer is not readable on this device, and the
            // proof does not need it: a draw with no colour, depth, stencil or
            // memory write produces nothing whatever its geometry. So the state
            // this plan actually reads is taken directly, rather than through a
            // full inline capture that would fail on the unreadable vertices.
            recomp::gpu::Gta5InlineDraw inert;
            inert.device=device;inert.primitive=pending.primitive;
            inert.vertexCount=pending.count;inert.stride=pending.stride;
            const char* inertStage="no-device-state";
            if(auto state=heap(device,13860)) {
                inert.deviceState=std::move(*state);
                inert.scissorRegisters=driverScissors[device];
                inertStage="not-evaluated";
                // Completing without executing is an equivalence ONLY while the
                // plan holds; it is re-proved from this draw's own device state.
                nativeDrawCompleted=recomp::gpu::planNativeInertRectangleDraw(
                    inert,nativeDepthOnlyProgram,&inertStage);
            }
            if(recomp::gpu::finishNativeReadOnlyDraw(nativeDrawCompleted,physicalMutation,live?&live->valid:nullptr))++liveDraws;
            rendererLog("RENDER_API_INERT_RECTANGLE vertices=%u stride=%u colour_mask=%u depth_control=%08x "
                "pixel_object=%08x completed=%u stage=%s executed=0 guest_published=0\n",
                pending.count,pending.stride,
                recomp::gpu::gta5DrawRegister(inert,0x2104).value_or(0),
                recomp::gpu::gta5DrawRegister(inert,0x2200).value_or(0),
                *ps,unsigned(nativeDrawCompleted),inertStage);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1902u,"Native inert rectangle proof failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_composite_shader_pack.h") || __has_include("native_screen_shader_pack.h")
        // ABI20 restores raw depth through colour aliases. The current header
        // can live on the owned worker stack; the old tile cache is not authority.
        // ABI26 is separately typed and qualified: position-only VS and generated
        // pixel XY. Both must publish the actual fenced physical colour rows;
        // neither a completed display nor a matching object substitutes for it.
        const recomp::gpu::NativeCompositeRectangleProgram* compositeProgram=nullptr;
        if(!pending.indexCount && vertexCode && pixelCode) {
            #if __has_include("native_composite_shader_pack.h")
            if(nativeCompositeProgram.draw.vertexIdentity==*vertexCode &&
               nativeCompositeProgram.draw.pixelIdentity==*pixelCode)compositeProgram=&nativeCompositeProgram;
            #endif
            #if __has_include("native_screen_shader_pack.h")
            if(nativeScreenSpaceProgram.draw.vertexIdentity==*vertexCode &&
               nativeScreenSpaceProgram.draw.pixelIdentity==*pixelCode)compositeProgram=&nativeScreenSpaceProgram;
            #endif
        }
        if(compositeProgram) {
            const auto& composite=*compositeProgram;
            additionalDraw=true;
            const auto slot=composite.draw.textureSlot;const auto fetch=submittedFetch(slot);
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                recomp::gpu::nativeTranslatedTextureSlots(composite.draw),heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5ArgumentBoundSurfaces(captured->deviceState,heap,data,surfaceArgument):std::nullopt;
            const char* planWhy="not-evaluated";
            const auto plan=captured && bindings?
                recomp::gpu::planNativeCompositeRectangleDraw(*captured,composite.draw,*bindings,&planWhy):std::nullopt;
            // Final allocation704..735 overlaps presentation720+. Live geometry
            // covers all32 rows; publish actual proved coverage, not a guessed
            //720-row cutoff. Authored partial-tail tests still leave padding unknown.
            const auto liveEntry=liveTargets.find(device);
            const bool haveLiveEntry=liveEntry!=liveTargets.end();
            const auto liveObject=haveLiveEntry?liveEntry->second.object:0u;
            const char* compositeStage="not-attempted";
            unsigned publishedRows=0;
            if(!haveLiveEntry)compositeStage="no-live-entry";
            else if(!plan)compositeStage=planWhy;
            else if(plan->object==liveObject)compositeStage="claims-live-object";
            if(haveLiveEntry && inlineGraphics && plan && plan->object!=liveObject) {
                const auto key=std::make_pair(device,plan->object);
                if(!(offscreenTargets.count(key) || offscreenTargets.size()<16))compositeStage="offscreen-target-capacity";
                if(offscreenTargets.count(key) || offscreenTargets.size()<16) {
                    auto& target=offscreenTargets[key];target.valid=false;
                    if(!target.image || target.image->plan()!=*plan)target.image=inlineGraphics->createOffscreenDrawTarget(*plan);
                    invalidatePhysicalNativeViews(device,plan->baseTile,plan->tileCount);
                    if(!target.image)compositeStage="target-image-create";
                    else nativeDrawCompleted=recomp::gpu::executeNativeEdram32ColorRows(physicalMutation,*plan,
                        [&]{return inlineGraphics->executeCompositeRectangles(
                            *target.image,composite,*captured,*bindings,&compositeStage);},
                        [&]{compositeStage="physical-band-readback";return target.image->readCompletedColorRows();},
                        [&]{return target.image->completedColorRows();});
                    if(nativeDrawCompleted) {
                        compositeStage="physical-band-published";
                        publishedRows=target.image->completedColorRows();
                        target.header=*bindings->surfaces[0].header;target.valid=target.image->valid();
                        recordCompletedTextures(liveEntry->second,*captured);++liveDraws;
                    }
                }
            }
            rendererLog("%s object=%08x width=%u height=%u base=%u tiles=%u "
                "register_target=%08x tile_cache_target=%08x fetch=%u state=%u completed=%u stage=%s guest_published=0 colour_info=%08x published_rows=%u abi=%u\n",
                composite.draw.screenSpaceRectangle?"RENDER_API_SCREEN_RECTANGLE":"RENDER_API_COMPOSITE_RECTANGLES",
                plan?plan->object:0,plan?plan->width:0,plan?plan->height:0,plan?plan->baseTile:0,plan?plan->tileCount:0,
                bindings?bindings->surfaces[0].object:0,bindings?bindings->surfaces[0].tileCacheObject:0,
                unsigned(fetch.has_value()),unsigned(plan.has_value()),unsigned(nativeDrawCompleted),compositeStage,
                captured?recomp::gpu::gta5DrawRegister(*captured,0x2001).value_or(UINT32_MAX):UINT32_MAX,publishedRows,composite.draw.abiVersion);
            if(!nativeDrawCompleted){diagnosticSkipDraw(1976u,"Native composite rectangle state/coverage/ownership/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_cube_rectangle_shader_pack.h")
        if(!pending.indexCount && vertexCode && pixelCode &&
           nativeCubeRectangleProgram.draw.vertexIdentity==*vertexCode && nativeCubeRectangleProgram.draw.pixelIdentity==*pixelCode) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            const auto slot=nativeCubeRectangleProgram.draw.textureSlot;const auto fetch=submittedFetch(slot);
            const auto texture=fetch?recomp::gpu::captureNativeCubeBase(slot,*fetch,data):std::nullopt;
            if(captured && texture && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];captured->textures={*texture};
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            const auto plan=captured && bindings?recomp::gpu::planNativeCubeRectangleDraw(*captured,nativeCubeRectangleProgram.draw,*bindings):std::nullopt;
            const bool disjoint=plan && (plan->baseTile>=1440 || plan->baseTile+plan->tileCount<=720);
            if(live && inlineGraphics && plan && disjoint && plan->object!=live->object) {
                const auto key=std::make_pair(device,plan->object);
                if(offscreenTargets.count(key) || offscreenTargets.size()<16) {
                    auto& target=offscreenTargets[key];target.valid=false;
                    if(!target.image || target.image->plan()!=*plan)target.image=inlineGraphics->createOffscreenDrawTarget(*plan);
                    // Each original write invalidates every overlapping owner,
                    // including views of depth bytes aliased by this color range.
                    for(auto& [otherKey,other]:offscreenTargets)if(other.image && otherKey!=key) {
                        const auto& p=other.image->plan();
                        if(p.baseTile<plan->baseTile+plan->tileCount && plan->baseTile<p.baseTile+p.tileCount)other.valid=false;
                    }
                    for(auto& [otherKey,other]:clearedColor8Targets)if(other.image &&
                        recomp::gpu::color8ClearIntersects(other.image->plan(),plan->baseTile,plan->tileCount))other.valid=false;
                    invalidateFixed16Overlaps(*plan);
                    nativeDrawCompleted=target.image && inlineGraphics->executeCubeRectangles(*target.image,nativeCubeRectangleProgram,*captured,*bindings);
                    if(nativeDrawCompleted) {
                        target.header=*bindings->surfaces[0].header;target.valid=true;live->valid=true;
                        recordCompletedTextures(*live,*captured);++liveDraws;
                    }
                }
            }
            rendererLog("RENDER_API_CUBE_RECTANGLES object=%08x width=%u height=%u captured_cube=%u state=%u completed=%u guest_published=0\n",
                plan?plan->object:0,plan?plan->width:0,plan?plan->height:0,unsigned(texture.has_value()),unsigned(plan.has_value()),unsigned(nativeDrawCompleted));
            if(!nativeDrawCompleted){diagnosticSkipDraw(2017u,"Native cube rectangle state/coverage/ownership/draw failed; original inputs preserved");}
            return;
        }
        #endif
        #if __has_include("native_lod_rectangle_shader_pack.h") || __has_include("native_predicated_lod_rectangle_shader_pack.h") || __has_include("native_retail_lod5_rectangle_shader_pack.h")
        const recomp::gpu::NativeLodRectangleProgram* lodProgram=nullptr;
        if(!pending.indexCount && vertexCode && pixelCode) {
            #if __has_include("native_lod_rectangle_shader_pack.h")
            if(nativeLodRectangleProgram.draw.vertexIdentity==*vertexCode && nativeLodRectangleProgram.draw.pixelIdentity==*pixelCode)
                lodProgram=&nativeLodRectangleProgram;
            #endif
            #if __has_include("native_predicated_lod_rectangle_shader_pack.h")
            if(nativePredicatedLodRectangleProgram.draw.vertexIdentity==*vertexCode && nativePredicatedLodRectangleProgram.draw.pixelIdentity==*pixelCode)
                lodProgram=&nativePredicatedLodRectangleProgram;
            #endif
            #if __has_include("native_retail_lod5_rectangle_shader_pack.h")
            if(nativeRetailLod5RectangleProgram.draw.vertexIdentity==*vertexCode && nativeRetailLod5RectangleProgram.draw.pixelIdentity==*pixelCode)
                lodProgram=&nativeRetailLod5RectangleProgram;
            #endif
        }
        if(lodProgram) {
            additionalDraw=true;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,{},heap,data);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            const auto fetch=submittedFetch(lodProgram->draw.textureSlot);
            const auto texture=fetch?recomp::gpu::captureNativeMipTexture(*fetch,data):std::nullopt;
            const auto use=texture?mipTextureUse(texture->fetch):std::nullopt;
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap,data,40):std::nullopt;
            const auto plan=captured && bindings && texture?
                recomp::gpu::planNativeLodRectangleDraw(*captured,lodProgram->draw,*bindings,*texture):std::nullopt;
            const auto object=plan?plan->target.object:0;const auto key=std::make_pair(device,object);
            const auto found=offscreenTargets.find(key);
            const bool disjoint=plan && (plan->target.baseTile>=1440 || plan->target.baseTile+plan->target.tileCount<=720);
            bool owner=false;
            if(live && inlineGraphics && use && plan && disjoint && object!=live->object && found!=offscreenTargets.end()) {
                auto& target=found->second;
                owner=target.valid && target.image && target.image->plan()==plan->target &&
                    bindings->surfaces[0].header && *bindings->surfaces[0].header==target.header;
                if(owner) {
                    // A partial writer may only LOAD an existing complete
                    // owner. Never create a new target or silently clear it.
                    target.valid=false;
                    for(auto& [otherKey,other]:offscreenTargets)if(other.image && otherKey!=key) {
                        const auto& p=other.image->plan();
                        if(p.baseTile<plan->target.baseTile+plan->target.tileCount && plan->target.baseTile<p.baseTile+p.tileCount)other.valid=false;
                    }
                    for(auto& [otherKey,other]:clearedColor8Targets)if(other.image &&
                        recomp::gpu::color8ClearIntersects(other.image->plan(),plan->target.baseTile,plan->target.tileCount))other.valid=false;
                    invalidateFixed16Overlaps(plan->target);
                    nativeDrawCompleted=inlineGraphics->executeLodRectangle(*target.image,*lodProgram,*captured,*bindings,*texture);
                    if(nativeDrawCompleted) {
                        target.valid=true;live->valid=true;++liveDraws;
                        // Both independent uploads and this draw have fenced.
                        // This record means no pending GPU reads, NOT that all
                        // resident levels were initialized or sampled.
                        live->completedMipTextures[use->baseAddress]=*use;
                    }
                }
            }
            rendererLog("RENDER_API_LOD_RECTANGLE object=%08x mip=%u width=%u height=%u owned_planes=%u profile=%u preserved_owner=%u completed=%u shader_abi=%u guest_published=0\n",
                object,plan?plan->outputMip:0,plan?plan->width:0,plan?plan->height:0,unsigned(use.has_value()),
                unsigned(plan.has_value()),unsigned(owner),unsigned(nativeDrawCompleted),lodProgram->draw.abiVersion);
            if(!nativeDrawCompleted){diagnosticSkipDraw(2082u,"Native LOD rectangle input/state/ownership/draw failed; original inputs preserved");}
            return;
        }
        #endif
        if(additional) {
            additionalDraw=true;
            std::optional<std::vector<std::uint32_t>> indices=std::vector<std::uint32_t>{};
            if(pending.indexCount) {
                indices.reset();
                const auto address=word(device+13852);
                const auto bytes=std::uint64_t{pending.indexCount}*2;
                if(address && bytes<=NativeInlineBytes && word(device+13860)==(bytes+3)/4) {
                    const auto raw=data(*address,static_cast<std::uint32_t>(bytes));
                    if(raw)indices=recomp::gpu::decodeNativeInlineIndices(pending.indexCount,
                        pending.indexFormat,pending.minVertex,pending.count,*raw);
                }
            }
            const bool textured=additional->textureSlot!=UINT32_MAX;
            captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
                recomp::gpu::nativeTranslatedTextureSlots(*additional),heap,data,submittedFetch);
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(captured && driverConstantsValid && constants!=driverConstants.end()) {
                constants->second.apply(*captured);captured->scissorRegisters=driverScissors[device];
            } else captured.reset();
            if(additional->abiVersion==5) {
                const auto bindings=captured?recomp::gpu::captureGta5BoundSurfaces(captured->deviceState,heap):std::nullopt;
                const auto plan=captured && bindings?recomp::gpu::planNativeOffscreenDraw(*captured,*additional,*bindings):std::nullopt;
                // The existing presentation target is1280x720, pitch1280,
                // 32bpp7e3 at tile720 (validated by its clear/draw profiles).
                // This path must not overwrite or pretend to reconstruct it.
                const bool disjoint=plan && (plan->baseTile>=1440 || plan->baseTile+plan->tileCount<=720);
                bool completed=false;
                if(live && inlineGraphics && indices && indices->empty() && plan && disjoint && plan->object!=live->object) {
                    const auto key=std::make_pair(device,plan->object);
                    if(offscreenTargets.count(key) || offscreenTargets.size()<16) {
                        auto& offscreen=offscreenTargets[key];offscreen.valid=false;
                        if(!offscreen.image || offscreen.image->plan()!=*plan)
                            offscreen.image=inlineGraphics->createOffscreenDrawTarget(*plan);
                        // Writes invalidate overlapping previous views. Reading
                        // an invalidated view later fails; no alias is guessed.
                        for(auto& [otherKey,other]:offscreenTargets)if(other.image && otherKey!=key) {
                            const auto& p=other.image->plan();
                            if(p.baseTile<plan->baseTile+plan->tileCount && plan->baseTile<p.baseTile+p.tileCount)other.valid=false;
                        }
                        for(auto& [otherKey,other]:clearedColor8Targets)if(other.image) {
                            const auto& p=other.image->plan();
                            if(recomp::gpu::color8ClearIntersects(p,plan->baseTile,plan->tileCount))other.valid=false;
                        }
                        invalidateFixed16Overlaps(*plan);
                        completed=offscreen.image && offscreen.image->draw(*additional,*captured,*bindings);
                        if(completed) {
                            offscreen.header=*bindings->surfaces[0].header;offscreen.valid=true;
                            // Restore the previously valid, disjoint presentation
                            // image; this offscreen draw did not modify its pixels.
                            live->valid=true;recordCompletedTextures(*live,*captured);++liveDraws;
                        }
                    }
                }
                nativeDrawCompleted=completed;
                rendererLog("RENDER_API_OFFSCREEN_DRAW object=%08x width=%u height=%u pitch=%u base=%u state=%u completed=%u guest_published=0\n",
                    plan?plan->object:0,plan?plan->width:0,plan?plan->height:0,plan?plan->pitch:0,plan?plan->baseTile:0,
                    unsigned(plan.has_value()),unsigned(completed));
                if(!completed){diagnosticSkipDraw(2144u,"Native offscreen draw state/ownership/GPU execution failed");}
                return;
            }
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
            const char* showWhy=!captured?"capture":!indices?"indices":
                recomp::gpu::diagShowFramesTranslatedDraw(*captured,*additional,*indices);
            const bool state=!showWhy;
            if(showWhy)diagShowFramesReason("translated-draw",showWhy);
#else
            const bool state=captured && indices && recomp::gpu::validateNativeTranslatedDraw(*captured,*additional,*indices);
#endif
            const auto object=word(device+12816);
            const bool completed=live && state && object && *object==live->object && live->image->drawTranslated(*additional,*captured,*indices);
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
            if(state && !completed)diagShowFramesReason("translated-draw",!live?"no-presentation-image":!object?"no-bound-target":
                *object!=live->object?"other-bound-target":"draw-execution-or-texture");
#endif
            nativeDrawCompleted=completed;
            if(completed){live->valid=true;++liveDraws;++live->contentSerial;latestLiveDevice=device;recordCompletedTextures(*live,*captured);nativeUiDevice.store(device);
                ++completedTranslatedTextures.at(captured->textures.size());}
            if(completed)
                rendererLogQuiet("RENDER_API_TRANSLATED_DRAW vertices=%u stride=%u indices=%u captured=%u state=%u completed=%u\n",
                    pending.count,pending.stride,pending.indexCount,unsigned(captured.has_value()),unsigned(state),unsigned(completed));
            else
                rendererLog("RENDER_API_TRANSLATED_DRAW vertices=%u stride=%u indices=%u captured=%u state=%u completed=%u\n",
                    pending.count,pending.stride,pending.indexCount,unsigned(captured.has_value()),unsigned(state),unsigned(completed));
            if(!completed) {
                if(textured) {
                const auto fetch=submittedFetch(additional->textureSlot);
                rendererLog("RENDER_STOPPED_BINDING slot=%u submitted=%u word0=%08x shadow0=%08x\n",
                    additional->textureSlot,unsigned(fetch.has_value()),fetch?(*fetch)[0]:0,
                    word(device+1152+24*additional->textureSlot).value_or(0));
                }
                diagnosticSkipDraw(2162u,"Translated inline draw state/resource/GPU execution failed");
            }
            return;
        }
#endif
        if(!((vsRole==1 && psRole==2)||(vsRole==3 && psRole==4))) {
#if defined(RECOMP_ENUMERATE_UNSUPPORTED_INLINE)
            // DIAGNOSTIC BUILD ONLY. Instead of preserving the FIRST unsupported
            // draw and stopping, report each distinct unsupported pair once and
            // skip it, so a single run inventories every remaining class rather
            // than one run per class.
            //
            // A skipped draw is a draw that did NOT happen: anything this build
            // renders past this point is wrong, and nothing it produces is
            // evidence of correctness, completion or gameplay. It exists only to
            // enumerate the work, and must never be the installed path.
            {
                const auto identity=[](const std::optional<std::vector<std::uint8_t>>& code) {
                    std::uint64_t h=1469598103934665603ull;
                    if(code)for(auto b:*code){h^=b;h*=1099511628211ull;}
                    return h;
                };
                const auto vsId=identity(vertexCode),psId=identity(pixelCode);
                static std::set<std::pair<std::uint64_t,std::uint64_t>> reported;
                if(reported.emplace(vsId,psId).second) {
                    rendererLog("RENDER_INLINE_UNSUPPORTED_CLASS vs=%016llx ps=%016llx vs_bytes=%zu ps_bytes=%zu "
                        "primitive=%u vertices=%u stride=%u indices=%u pixel_object=%08x "
                        "colour_mask=%08x depth_control=%08x distinct=%zu skipped=1\n",
                        static_cast<unsigned long long>(vsId),static_cast<unsigned long long>(psId),
                        vertexCode?vertexCode->size():0,pixelCode?pixelCode->size():0,
                        pending.primitive,pending.count,pending.stride,pending.indexCount,*ps,
                        word(device+10460).value_or(0),word(device+10548).value_or(0),reported.size());
                }
                nativeDrawCompleted=true;
#if (defined(__ANDROID__) || defined(__APPLE__))
                if(live)live->valid=true;
#endif
                return;
            }
#else
            // Preserve the first unsupported draw's OWNED inputs, not the first
            // sixteen video frames. No replacement shader or completion is used.
            preserveStoppedDraw();
            return;
#endif
        }
        captured=recomp::gpu::captureGta5InlineDraw(device,pending.primitive,pending.count,pending.stride,
            psRole==2?std::vector<unsigned>{0,1,2}:std::vector<unsigned>{2},heap,data,submittedFetch);
        if(captured) {
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(!driverConstantsValid || constants==driverConstants.end())captured.reset();
            else {
                constants->second.apply(*captured);
                captured->scissorRegisters=driverScissors[device];
            }
        }
#if (defined(__ANDROID__) || defined(__APPLE__))
        if(live) {
            const unsigned pair=psRole==2?0:1;
            const auto packed=captured?recomp::gpu::packGta5StartupDraw(*captured,pair):std::nullopt;
            const bool geometry=captured && recomp::gpu::validateGta5StartupGeometry(*captured,
                inlinePrograms->identities()[vsRole],pair);
            const bool state=captured && recomp::gpu::matchesNativeColorRenderRegisters(
                [&](unsigned r){return recomp::gpu::gta5DrawRegister(*captured,r);},pair);
            const auto object=word(device+12816);
            live->valid=packed && geometry && state && object && *object==live->object &&
                live->image->drawValidatedAbi(pair,*packed);
            nativeDrawCompleted=live->valid;
            if(live->valid){
                ++liveDraws;++live->contentSerial;latestLiveDevice=device;
                recordCompletedTextures(*live,*captured);
            }
            rendererLog("RENDER_API_LIVE_DRAW pair=%u geometry=%u state=%u packed=%u completed=%u guest_published=0\n",
                pair,unsigned(geometry),unsigned(state),unsigned(packed.has_value()),unsigned(live->valid));
        }
#endif
    });
    const auto& draw=captured;
    std::size_t textureBytes=0;unsigned literals=0;
    if(draw)for(const auto& t:draw->textures){textureBytes+=t.source.size();literals+=t.literalZero;}
    // v31: sampled after the first 4096 calls (it was ~240 lines per world frame).
    if(index<4096 || !(index&1023))rendererLogQuiet("RENDER_INLINE_INPUT call=%u gate=%u vs_role=%d ps_role=%d status=%s "
        "vertices=%u stride=%u vertex_bytes=%zu textures=%zu texture_bytes=%zu literal_textures=%u\n",
        index+1,unsigned(gate),vsRole,psRole,draw?"owned-at-end":"unavailable",pending.count,pending.stride,
        draw?draw->vertices.size():0,draw?draw->textures.size():0,textureBytes,literals);
    if(firstUnsupportedInline)throw std::runtime_error("Unsupported inline shaders: original draw inputs captured");
    if(pending.nativeAllocation) {
        if(!gate || !nativeDrawCompleted || !gateRef([&] {nativeInlineBuffers.at(device).busy=false;}))
            throw std::runtime_error("Native EndVertices did not complete its real draw");
    }
    if(draw && !additionalDraw) {
        const bool compatible=recomp::gpu::matchesNativeColorRenderRegisters(
            [&](unsigned reg){return recomp::gpu::gta5DrawRegister(*draw,reg);},psRole==2?0:1);
        rendererLog("RENDER_DRAW_STATE call=%u compatible=%u scissor_known=%u\n",
            index+1,unsigned(compatible),unsigned(draw->scissorRegisters.has_value()));
        if(!compatible && index<2) {
            constexpr unsigned registers[]{0x2000,0x2001,0x200E,0x200F,0x2080,0x2081,0x2082,
                0x2100,0x2101,0x2102,0x2104,0x210F,0x2110,0x2111,0x2112,0x2113,0x2114,
                0x2180,0x2181,0x2182,0x2183,0x2184,0x2200,0x2201,0x2202,0x2203,0x2204,
                0x2205,0x2206,0x2208,0x2284,0x2292,0x2293,0x2301,0x2302,0x2303,0x2304,
                0x2305,0x2306,0x2307,0x2308,0x2312};
            for(auto reg:registers) {
                const auto value=recomp::gpu::gta5DrawRegister(*draw,reg);
                rendererLog("RENDER_STATE_WORD pair=%u register=%04x known=%u value=%08x\n",
                    psRole==2?0:1,reg,unsigned(value.has_value()),value.value_or(0));
            }
        }
    }
    if(index<CaptureLimit)inlineDraws[index]=std::move(captured);
    return pending.nativeAllocation;
}

bool clearLiveTarget(std::uint32_t device,std::uint32_t flags,std::uint32_t rect,std::uint32_t color,
                     const recomp::gpu::Gta5ClearCall* terminalCall=nullptr) {
    stopPendingTileRecording("executed-clear");
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineRegistry || !inlinePrograms || !inlineGraphics || !inlineGate)return false;
    bool completed=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("executed-clear");
        // Unknown clear profiles leave the owner empty. The validated fixed16
        // fallback commits only data outside BOTH of its written attachments.
        recomp::gpu::NativeEdram32Mutation physicalMutation(liveEdram32[device]);
        auto& live=liveTargets[device];const bool previousLiveValid=live.valid;live.valid=false;
        if(terminalCall && flags==0xA0) {
            const auto capture=[&]{return recomp::gpu::captureGta5ClearCall(*terminalCall,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});};
            const auto captured=capture();const char* stage=nullptr;
            const auto plan=recomp::gpu::planNativeStencilClear(captured,&stage);
            if(plan) {
                invalidatePhysicalNativeViews(device,plan->baseTile,plan->tileCount);
                completed=recomp::gpu::executeNativeStencilClear(captured,physicalMutation,
                    [&]{return recomp::gpu::sameNativeStencilClearInputs(captured,capture());},&stage);
                // The admitted depth band [0,720) is disjoint from display
                // [720,1440). Never revive an already-invalid presentation.
                if(completed){live.valid=previousLiveValid;++liveClears;}
            }
            rendererLog("RENDER_API_STENCIL_CLEAR flags=%08x object=%08x rect=%u/%u/%u/%u stencil=%02x physical_completed=%u display_valid=%u stage=%s query_result=0 ring_advance=0\n",
                flags,plan?plan->object:0,plan?plan->left:0,plan?plan->top:0,plan?plan->right:0,
                plan?plan->bottom:0,terminalCall->stencil,unsigned(completed),unsigned(live.valid),stage?stage:"unknown");
            if(completed)return;
            if(!firstUnsupportedClear)firstUnsupportedClear=captured;
            // This classified stencil request cannot fall into a color/depth
            // initialization fallback after stale-input or owner refusal.
            return;
        }
        // A clear this path cannot capture must not silently invalidate the
        // presentation image when it provably addresses a DISJOINT EDRAM range.
        // Measured: an uncapturable 256x256 clear (pitch 320, base 64, 64 tiles)
        // was invalidating the 1280x720 presentation band at 720+, after which
        // every offscreen consumer refused with no-live-target. Extents are read
        // from the bound surface header; anything unreadable stays invalidating.
        const auto liveRangeIntact=[&]()->bool {
            if(!previousLiveValid || !live.image)return false;
            const auto state=inlineRegistry->copySerializedGuestHeap(device,13128);
            if(!state || state->size()!=13128)return false;
            const auto word=[&](const auto& b,unsigned at) {
                return (std::uint32_t{(*b)[at]}<<24)|(std::uint32_t{(*b)[at+1]}<<16)|
                    (std::uint32_t{(*b)[at+2]}<<8)|(*b)[at+3];
            };
            const auto target=word(state,12816);
            if(!target || target%4)return false;
            const auto surface=inlineRegistry->copySerializedGuestHeap(target,32);
            if(!surface || surface->size()!=32)return false;
            const auto info=word(surface,24),colour=word(surface,28);
            const auto pitch=info&0x3FFFu;
            if(!pitch || pitch%80 || (info&0x3C000u))return false;
            // Height from the clear rectangle, clamped to the 2048-tile EDRAM.
            const auto clipRect=inlineRegistry->copySerializedGuestArgument(rect,16);
            if(!clipRect || clipRect->size()!=16)return false;
            const auto height=word(clipRect,12);
            if(!height || height>2048)return false;
            const auto base=colour&0x7FFu,tiles=(pitch/80)*((height+15)/16);
            if(!tiles || tiles>2048-base)return false;
            // The presentation band is tiles [720,1440), the same extent every
            // offscreen `disjoint` check in this file already relies on.
            // Disjoint EDRAM ranges cannot have touched the presentation image.
            return base+tiles<=720u || 1440u<=base;
        };
        const bool preserveLive=liveRangeIntact();
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
        auto clear=recomp::gpu::captureGta5ColorClear(device,flags,rect,color,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
        if(!clear) {
            clear=recomp::gpu::diagShowFramesColorClear(device,flags,rect,color,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
            if(clear)diagShowFramesReason("clear","relaxed-colour-clear");
        }
#else
        const auto clear=recomp::gpu::captureGta5ColorClear(device,flags,rect,color,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
#endif
        const auto primary=recomp::gpu::executeNativePrimaryClear(clear.has_value(),preserveLive,[&] {
            if(!live.image)live.image=inlineGraphics->createApiDrawTarget(*inlinePrograms);
            live.object=clear->target;
            return live.image && live.image->clearColor(clear->color);
        });
        live.valid=primary.presentationValid;completed=primary.completed;
        if(completed){++liveClears;++live.contentSerial;}
        if(!clear) {
            const auto state=inlineRegistry->copySerializedGuestHeap(device,13128);
            const auto rectangle=inlineRegistry->copySerializedGuestArgument(rect,16);
            const auto rgba=inlineRegistry->copySerializedGuestArgument(color,16);
            const auto word=[](const auto& data,unsigned at) {
                if(!data || data->size()<at+4)return UINT32_MAX;
                const auto* b=data->data()+at;
                return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];
            };
            const auto surface=inlineRegistry->copySerializedGuestHeap(word(state,12816),32);
            rendererLog("RENDER_CLEAR_RESOURCE object=%08x word24=%08x word28=%08x available=%u\n",
                word(state,12816),word(surface,24),word(surface,28),unsigned(surface.has_value()));
            rendererLog("RENDER_CLEAR_UNAVAILABLE surface=%08x color_info=%08x flags=%08x target0=%08x targets=%08x/%08x/%08x "
                "viewport=%08x/%08x/%08x/%08x rect=%08x/%08x/%08x/%08x rgba=%08x/%08x/%08x/%08x scissor_enable=%08x\n",
                word(state,10368),word(state,10372),word(state,11068),word(state,12816),
                word(state,12820),word(state,12824),word(state,12828),
                word(state,13024),word(state,13028),word(state,13032),word(state,13036),
                word(rectangle,0),word(rectangle,4),word(rectangle,8),word(rectangle,12),
                word(rgba,0),word(rgba,4),word(rgba,8),word(rgba,12),word(state,12288));
        }
        if(!clear && preserveLive) {
            // Disjoint uncapturable clear: the presentation image is untouched.
            rendererLog("RENDER_LIVE_PRESERVED_DISJOINT_CLEAR flags=%08x requested_clear_completed=0\n",flags);
        }
        rendererLog("RENDER_API_LIVE_CLEAR flags=%08x target=%08x packed=%08x captured=%u completed=%u\n",
            flags,clear?clear->target:0,clear?clear->packedColor:0,unsigned(clear.has_value()),unsigned(completed));
        if(!completed && terminalCall) {
            const auto captured=recomp::gpu::captureGta5ClearCall(*terminalCall,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
            const auto audit=recomp::gpu::auditGta5Color8Clear(captured);const auto& plan=audit.plan;
            const char* color8Stage="color8-clear-owner";
            if(recomp::gpu::nativeColor8ClearOwnerAllowed(audit,live.ownsPresentation,live.object)) {
                const auto key=std::make_pair(device,plan.object);
                if(clearedColor8Targets.count(key) || clearedColor8Targets.size()<16) {
                    auto& target=clearedColor8Targets[key];target.valid=false;
                    if(!target.image || target.image->plan()!=plan)target.image=inlineGraphics->createColor8ClearTarget(plan);
                    // A completed view never authorizes a different EDRAM alias.
                    // Invalidate all overlapping prior views BEFORE submission.
                    for(auto& [otherKey,other]:clearedColor8Targets)if(other.image && otherKey!=key) {
                        const auto& p=other.image->plan();
                        if(recomp::gpu::color8ClearPlansOverlap(p,plan))other.valid=false;
                    }
                    for(auto& [otherKey,other]:offscreenTargets)if(other.image) {
                        const auto& p=other.image->plan();
                        if(recomp::gpu::color8ClearIntersects(plan,p.baseTile,p.tileCount))other.valid=false;
                    }
                    invalidateFixed16Overlaps(plan);
                    // Invalidate every overlapping derived view, including
                    // newer target families. Disjoint display validity stays
                    // exactly what it was before this clear, never forced true.
                    invalidatePhysicalNativeViews(device,plan.baseTile,plan.tileCount);
                    if(plan.depth)invalidatePhysicalNativeViews(device,plan.depth->baseTile,plan.depth->tileCount);
                    const auto current=[&] {
                        const auto now=recomp::gpu::captureGta5ClearCall(*terminalCall,
                            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
                            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
                        if(now.state!=captured.state || now.rectangle!=captured.rectangle || now.color!=captured.color)return false;
                        for(unsigned i=0;i<5;++i)if(now.surfaces[i].object!=captured.surfaces[i].object ||
                            now.surfaces[i].tileCache!=captured.surfaces[i].tileCache || now.surfaces[i].header!=captured.surfaces[i].header)return false;
                        return true;
                    };
                    completed=recomp::gpu::executeNativeColor8ClearWrite(audit,live.ownsPresentation,live.object,
                        physicalMutation,current,[&]{return target.image && target.image->clear(plan.color);},
                        [&]{return target.image?target.image->readUniformResolved():std::optional<std::vector<std::uint8_t>>{};},&color8Stage);
                    if(completed) {
                        target.header=*captured.surfaces[0].header;target.valid=true;++target.contentSerial;
                        live.valid=previousLiveValid;++liveClears;
                    }
                } else color8Stage="color8-clear-target-capacity";
            }
            rendererLog("RENDER_API_COLOR8_CLEAR_OWNER presentation_owned=%u presentation_valid_before=%u presentation_valid_after=%u stage=%s physical_completed=%u\n",
                unsigned(live.ownsPresentation),unsigned(previousLiveValid),unsigned(live.valid),color8Stage,unsigned(completed));
            rendererLog("RENDER_API_COLOR8_CLEAR object=%08x width=%u height=%u pitch=%u samples=%u format=%u base=%u tiles=%u issues=%08x effective_flags=%08x completed=%u\n",
                plan.object,plan.width,plan.height,plan.pitch,plan.samples,plan.format,plan.baseTile,plan.tileCount,
                audit.issues,audit.effectiveFlags,unsigned(completed));
            if(plan.depth)rendererLog("RENDER_API_FLOAT24_CLEAR object=%08x base=%u tiles=%u samples=%u encoded=%06x stencil=%02x completed=%u\n",
                plan.depth->object,plan.depth->baseTile,plan.depth->tileCount,plan.depth->samples,
                plan.depth->value.encoded,plan.depth->value.stencil,unsigned(completed));
            if(!completed && plan.format==4) {
                completed=clearFixed16Target(device,captured,physicalMutation);
                if(completed){live.valid=previousLiveValid;++liveClears;}
            }
            if(!completed && !firstUnsupportedClear)firstUnsupportedClear=captured;
        }
        if(!completed && terminalCall && firstUnsupportedClear) {
            rendererLog("RENDER_STOPPED_CLEAR flags=%08x depth_object=%08x depth_bits=%016llx stencil=%08x frames=%zu\n",
                flags,firstUnsupportedClear->surfaces[4].object,
                static_cast<unsigned long long>(terminalCall->depthBits),terminalCall->stencil,firstUnsupportedClear->frames.size());
        }
    });
    return gated && completed;
#else
    (void)device;(void)flags;(void)rect;(void)color;(void)terminalCall;return false;
#endif
}

void capturePendingTileResolve(const PPCContext& ctx) {
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__))
    if(tileRecordingDevice.load() && inlineRegistry && inlineGate)gateRef([&] {
        if(stoppedTileResolve || !tileRecordingDiagnostic.pending())return;
        stoppedTileResolve=recomp::gpu::captureGta5TileResolveBoundary(ctx.r1.u32,
            {ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
             static_cast<std::uint32_t>(ctx.lr)},ctx.f1.u64,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);});
        rendererLog("RENDER_TILE_RESOLVE_CAPTURE device=%08x caller=%08x destination=%08x state=%u header=%u gpu_completed=0 guest_published=0\n",
            ctx.r3.u32,static_cast<unsigned>(ctx.lr),ctx.r6.u32,unsigned(stoppedTileResolve->state.has_value()),
            unsigned(stoppedTileResolve->destinationHeader.has_value()));
    });
#else
    (void)ctx;
#endif
}

void capturePendingTileEnd(const PPCContext& ctx) {
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__))
    if(tileRecordingDevice.load() && inlineRegistry && inlineGate)gateRef([&] {
        if(stoppedTileEnd || !tileRecordingDiagnostic.pending() ||
           ctx.r3.u32!=tileRecordingDevice.load())return;
        const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
        const auto links=[&](std::uint32_t a,std::uint32_t n)->std::optional<std::vector<std::uint8_t>> {
            if(a>=0xC0000000 && a<0xE0000000) {
                const auto physical=a-0xC0000000;
                const auto owner=inlineRegistry->stoppedGraphicsPhysicalOwnership(physical,n);
                if(!owner)return std::nullopt;
                const auto address=owner->virtualBase+(physical-owner->physicalBase);
                if(recomp::gpu::Gta5TileRecordingDiagnostic::originalLink(address)!=a)return std::nullopt;
                return graphics(address,n);
            }
            return graphics(a,n);
        };
        stoppedTileEnd=recomp::gpu::captureGta5EndTilingBoundary(ctx.r1.u32,
            {ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
             static_cast<unsigned>(ctx.lr)},ctx.f1.u64,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},graphics,links,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);});
        stoppedTileEnd->liveVideoRootMatches=inlineRegistry->snapshotNativeInterruptDeviceRoot(ctx.r3.u32);
        rendererLog("RENDER_TILE_END_CAPTURE device=%08x caller=%08x state=%u pending_bytes=%zu original_executed=0 gpu_completed=0 guest_published=0\n",
            ctx.r3.u32,static_cast<unsigned>(ctx.lr),unsigned(stoppedTileEnd->state.has_value()),
            stoppedTileEnd->pendingCommands?stoppedTileEnd->pendingCommands->size():0);
#if __has_include("native_logical_geometry_shader_pack.h")
        pendingNativeSceneInputs.reset();
        pendingNativeSceneResolves.reset();pendingNativeSceneStorage.reset();
        pendingNativeSceneCommands.reset();
        std::vector<unsigned> sequences;
        for(const auto& event:tileRecordingDiagnostic.events())
            if(event.kind==recomp::gpu::Gta5TileRecordingDiagnostic::Kind::Indexed)sequences.push_back(event.index);
        std::vector<recomp::gpu::Gta5NativeSceneDrawSource> sources;
        for(const auto& draw:boundIndexedSnapshots) {
            if(draw.device!=ctx.r3.u32 || !draw.geometryRecordingOnly)continue;
            sources.push_back({draw.sequence,draw.orderedConstants,draw.originalEmissionReturned,
                draw.geometry?&*draw.geometry:nullptr,draw.surfaces?&*draw.surfaces:nullptr,
                &draw.vertexCode,&draw.pixelCode,draw.recordedScissor?&*draw.recordedScissor:nullptr,&draw.mipTextures});
        }
        recomp::gpu::Pm4Snapshot commands;recomp::gpu::Pm4Error error;
        if(stoppedTileEnd->pendingCommands && !boundIndexedFailures.load() && !boundIndexedDropped.load() &&
           recomp::gpu::decodePm4(stoppedTileEnd->pendingCommands->data(),stoppedTileEnd->pendingCommands->size(),{},commands,error))
            pendingNativeSceneInputs=recomp::gpu::prepareGta5NativeSceneInputs(ctx.r3.u32,tileRecordingDiagnostic.seed(),
                sequences,sources,nativeLogicalGeometryPrograms,commands,nativeLogicalGeometryQueryPrograms);
        rendererLog("RENDER_NATIVE_SCENE_INPUTS device=%08x recorded=%zu captured=%zu prepared=%zu shader_pack=%zu gpu_completed=0 guest_published=0\n",
            ctx.r3.u32,sequences.size(),sources.size(),pendingNativeSceneInputs?pendingNativeSceneInputs->draws.size():0,
            nativeLogicalGeometryPrograms.size());
        if(pendingNativeSceneInputs) {
            std::vector<recomp::gpu::Gta5NativeSceneResolveSource> resolveSources;
            for(const auto& r:recordedTileResolves)resolveSources.push_back({&r.before,r.output?&*r.output:nullptr,
                r.plan.command+4,r.plan.tile,r.originalReturned});
            pendingNativeSceneResolves=recomp::gpu::prepareGta5NativeSceneResolves(tileRecordingDiagnostic.seed(),
                stoppedTileEnd->pendingAddress,*stoppedTileEnd->pendingCommands,resolveSources,sources);
            if(pendingNativeSceneResolves)
                pendingNativeSceneStorage=recomp::gpu::captureGta5NativeSceneResolveStorage(*pendingNativeSceneResolves,graphics);
        }
        rendererLog("RENDER_NATIVE_SCENE_RESOLVES device=%08x recorded=%zu planned=%zu owned_destinations=%zu gpu_completed=0 guest_published=0\n",
            ctx.r3.u32,recordedTileResolves.size(),pendingNativeSceneResolves?pendingNativeSceneResolves->resolves.size():0,
            pendingNativeSceneStorage?pendingNativeSceneStorage->current.size():0);
        unsigned rejected=0;
        if(pendingNativeSceneInputs && pendingNativeSceneResolves)
            pendingNativeSceneCommands=recomp::gpu::prepareGta5NativeSceneCommands(pendingNativeSceneInputs->queryCommands,
                *pendingNativeSceneResolves,*stoppedTileEnd->pendingCommands,&rejected);
        rendererLog("RENDER_NATIVE_SCENE_COMMANDS device=%08x prepared=%u packets=%u deferred_extent_packets=%u rejected_offset=%08x gpu_completed=0 guest_published=0\n",
            ctx.r3.u32,unsigned(pendingNativeSceneCommands.has_value()),
            pendingNativeSceneCommands?pendingNativeSceneCommands->accountedPackets:0,
            pendingNativeSceneCommands?pendingNativeSceneCommands->deferredExtentPackets:0,rejected);
#endif
    });
#else
    (void)ctx;
#endif
}

#include "renderer_native_scene.inc"

bool recordPendingTileResolve(PPCContext& ctx,const std::function<void()>& original) {
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__))
    if(!tileRecordingDevice.load() || !originalRecordingGate)return false;
    if(!inlineRegistry || !original || originalResolveRecordingDevice ||
       recordedTileResolves.size()>=recomp::gpu::Gta5TileRecordingDiagnostic::MaxResolves)
        stopPendingTileRecording("recorded-resolve-owner");
    bool recorded=false;
    const bool gated=originalRecordingGate([&] {
        const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
        const auto graphics=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
        auto before=recomp::gpu::captureGta5TileResolveBoundary(ctx.r1.u32,
            {ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
             static_cast<unsigned>(ctx.lr)},ctx.f1.u64,heap,graphics,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);});
        const auto arguments=[&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);};
        const auto plan=before.arguments[1]==0x14?
            recomp::gpu::describeRecordedDepthResolve(before,graphics,arguments):
            recomp::gpu::describeRecordedColorClearResolve(before,graphics,arguments);
        if(!plan) {if(!stoppedTileResolve)stoppedTileResolve=std::move(before);return;}
        recomp::gpu::Gta5ClearCapture current;current.call.device=ctx.r3.u32;current.state=before.state;
        for(unsigned i=0;i<5;++i) {
            const auto& b=before.surfaces->surfaces[i];current.surfaces[i]={b.object,b.tileCacheObject,b.header};
        }
        const auto index=static_cast<unsigned>(recordedTileResolves.size());
        if(!tileRecordingDiagnostic.recordResolve(current,index))return;
        recordedTileResolves.push_back({std::move(before),*plan});
        auto& capture=recordedTileResolves.back();
        const auto stack=ctx.r1.u64,pcr=ctx.r13.u64,link=ctx.lr;
        // This token permits ORIGINAL register packet recording only while
        // the dedicated gate parks every other guest writer. No nested gate.
        originalResolveRecordingDevice=ctx.r3.u32;
        struct ClearScope {~ClearScope(){originalResolveRecordingDevice=0;}} clearScope;
        original();
        capture.originalReturned=true;
        if(ctx.r1.u64!=stack || ctx.r13.u64!=pcr || ctx.lr!=link)
            stopPendingTileRecording("recorded-resolve-return-abi");
        capture.output=recomp::gpu::captureRecordedDepthResolveOutput(capture.before,capture.plan,heap,graphics);
        if(!capture.output)stopPendingTileRecording("recorded-resolve-output");
        recorded=true;
        rendererLog("RENDER_TILE_RESOLVE_RECORDED index=%u tile=%u command_bytes=%zu quad_bytes=%zu original_returned=1 gpu_completed=0 guest_published=0\n",
            index,plan->tile,capture.output->commands.size(),capture.output->quad.size());
    });
    if(!gated || !recorded)stopPendingTileRecording("unsupported-recorded-resolve");
    return true; // ORIGINAL CPU return, never native resolve completion.
#else
    (void)ctx;(void)original;return false;
#endif
}

// Caller already holds inlineGate. Keep this independent of live display
// validity and tiled-recording state so early rejection retains its actual ABI.
void captureNativeResolveFailure(const PPCContext& ctx,recomp::gpu::NativeResolveFailureStage stage) noexcept {
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(firstNativeResolveFailure || !inlineRegistry)return;
    // Before native presentation takeover a false resolve permits the original
    // driver fallback. That is not a terminal failure and must not consume the
    // single failure slot needed for the later native-owned rejection.
    const auto display=liveTargets.find(ctx.r3.u32);
    if(display==liveTargets.end() || !display->second.ownsPresentation)return;
    try {
        recomp::gpu::NativeResolveFailureCapture c;c.stage=stage;
        c.boundary=recomp::gpu::captureGta5TileResolveBoundary(ctx.r1.u32,
            {ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,
             static_cast<std::uint32_t>(ctx.lr)},ctx.f1.u64,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);});
        if(const auto it=liveTargets.find(ctx.r3.u32);it!=liveTargets.end()) {
            c.displayFound=true;c.displayValid=it->second.valid;c.ownsPresentation=it->second.ownsPresentation;
        }
        if(c.boundary.surfaces) {
            const auto object=c.boundary.surfaces->surfaces[0].object;
            if(const auto it=fixed16Targets.find({ctx.r3.u32,object});it!=fixed16Targets.end()) {
                c.fixed16ColorFound=true;c.fixed16ColorValid=it->second.valid;c.fixed16ColorSerial=it->second.serial;
            }
        }
        firstNativeResolveFailure=std::move(c);
        rendererLog("RENDER_NATIVE_RESOLVE_FAILURE stage=%s device=%08x caller=%08x flags=%08x destination=%08x state=%u header=%u display_valid=%u fixed16_valid=%u capture_only=1\n",
            recomp::gpu::nativeResolveFailureStageName(stage),ctx.r3.u32,static_cast<unsigned>(ctx.lr),ctx.r4.u32,ctx.r6.u32,
            unsigned(firstNativeResolveFailure->boundary.state.has_value()),unsigned(firstNativeResolveFailure->boundary.destinationHeader.has_value()),
            unsigned(firstNativeResolveFailure->displayValid),unsigned(firstNativeResolveFailure->fixed16ColorValid));
    }catch(...) {
        ++nativeResolveFailureCaptureFailures;
        rendererLog("%s","RENDER_NATIVE_RESOLVE_FAILURE capture-unavailable original-rejection-preserved\n");
    }
#else
    (void)ctx;(void)stage;
#endif
}

bool resolveLiveTarget(const PPCContext& ctx) {
    capturePendingTileResolve(ctx);
    stopPendingTileRecording("resolve");
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineRegistry || !inlineGate)return false;
    bool resolved=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("resolve");
        recomp::gpu::NativeEdram32Mutation physicalMutation(liveEdram32[ctx.r3.u32]);
        const auto found=liveTargets.find(ctx.r3.u32);
        if(const auto single=tryResolveSingleColor8Target(ctx,physicalMutation);single.has_value()) {
            resolved=*single;
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return; // Typed owner; never fall through into the MSAA/display routes.
        }
        if(const auto raw10=tryResolveRaw10Color(ctx,physicalMutation.candidate());raw10.has_value()) {
            resolved=*raw10;
            if(resolved)physicalMutation.commit(); // Completed copy; no source clear.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
        if(const auto rgb7e3=tryResolveRgb7e3Float(ctx,physicalMutation.candidate());rgb7e3.has_value()) {
            resolved=*rgb7e3;
            if(resolved)physicalMutation.commit(); // Clear footprint already UNKNOWN; no invented display ownership.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
        if(const auto r32=tryResolveR32Float(ctx,physicalMutation.candidate());r32.has_value()) {
            resolved=*r32;
            if(resolved)physicalMutation.commit(); // Pure copy; display remains invalid.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
        if(const auto exposure=tryResolveExposure(ctx,physicalMutation.candidate());exposure.has_value()) {
            resolved=*exposure;
            if(resolved)physicalMutation.commit(); // Two-word copy, no source clear/display revival.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
        // A completed offscreen source has its own owner. It must not inherit
        // presentation-image validity. Only the typed, validated fixed16 route
        // can take this branch; all other routes retain their existing policy.
        if(const auto fixed16=tryResolveFixed16Target(ctx);fixed16.has_value()) {
            resolved=*fixed16;
            if(resolved)physicalMutation.commit(); // This contract forbids source clears.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
        // Same rule for the ABI22 band: a completed rectangle MRT source has
        // its own owner and must not inherit presentation-image validity, which
        // the inline gate has already cleared for this draw.
#if defined(RECOMP_HAVE_RECTANGLE_MRT)
        if(const auto mrt=tryResolveRectangleMrtTarget(ctx);mrt.has_value()) {
            resolved=*mrt;
            if(resolved)physicalMutation.commit(); // Pure copy to guest texture memory.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
#endif
#if defined(RECOMP_HAVE_OFFSET_SAMPLE)
        // Same rule for the ABI23 band's own completed target.
        if(const auto offset=tryResolveOffsetSampleTarget(ctx);offset.has_value()) {
            resolved=*offset;
            if(resolved)physicalMutation.commit(); // Pure copy; no physical source mutation.
            if(!resolved)captureNativeResolveFailure(ctx,recomp::gpu::NativeResolveFailureStage::ReturnedFalse);
            return;
        }
#endif
        resolved=recomp::gpu::observeNativeResolveAttempt(found!=liveTargets.end(),
            found!=liveTargets.end(),[&] { /* DIAGNOSTIC: resolve partial display */
        const auto resolve=[&] {
        auto& live=found->second;
        const auto destination=ctx.r6.u32;
        // 82388A38 adapter: source0/default full rectangles, destination r6,
        // no array slice, mip, offset, clear, exponent change or special flags.
        bool shape=ctx.r4.u32==0 && ctx.r5.u32==0 && ctx.r7.u32==0 && ctx.r8.u32==0 &&
            ctx.r9.u32==0 && ctx.r10.u32==0 && ctx.f1.f64==1.0;
        for(unsigned offset:{92u,100u}) {
            const auto arg=std::uint64_t{ctx.r1.u32}+offset;
            const auto bytes=arg<=UINT32_MAX?inlineRegistry->copySerializedGuestArgument(static_cast<std::uint32_t>(arg),4):std::nullopt;
            shape=shape && bytes && bytes->size()==4 && std::all_of(bytes->begin(),bytes->end(),[](auto b){return !b;});
        }
        const auto header=recomp::gpu::captureNativeResolveTextureHeader(destination,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
        const auto description=header?recomp::gpu::describeGta5TextureHeader(*header):std::nullopt;
        const auto layout=description?recomp::gpu::planXenosTexture2D(*description):std::nullopt;
        const auto source=inlineRegistry->copySerializedGuestHeap(ctx.r3.u32+12816,4);
        std::uint32_t object=0;
        if(source)for(const auto b:*source)object=(object<<8)|b;
        const auto clearedFound=clearedColor8Targets.find({ctx.r3.u32,object});
        if(clearedFound!=clearedColor8Targets.end() && clearedFound->second.valid) {
            auto& target=clearedFound->second;
            if(!target.image)return;
            const auto& p=target.image->plan();const bool wasUniform=target.image->isUniformClear();
            const auto state=inlineRegistry->copySerializedGuestHeap(ctx.r3.u32,13860);
            const auto bindings=state?recomp::gpu::captureGta5BoundSurfaces(*state,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},40):std::nullopt;
            const recomp::gpu::NativeOffscreenResolveRequest request{ctx.r1.u32,ctx.r4.u32,ctx.r5.u32,
                ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
            const auto args=recomp::gpu::captureNativeOffscreenResolveArguments(request,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
            const auto audit=recomp::gpu::auditNativeColor8ResolveArguments(p,request,args,description?&*description:nullptr);
            const auto face=description?recomp::gpu::planNativeColor8ResolveFace(*description,request.slice):std::nullopt;
            const bool sourceReady=state && bindings && bindings->surfaces[0].header &&
                *bindings->surfaces[0].header==target.header && recomp::gpu::nativeColor8ResolveSourceMatches(p,*state,*bindings);
            const bool shape=audit.plan && face && sourceReady && face->original.width==p.width && face->original.height==p.height;
            const auto address=shape?textureStorageAddress(face->storage.description,face->storage.sourceBytes):std::nullopt;
            const auto before=address?inlineRegistry->copySerializedGraphicsData(*address,face->storage.sourceBytes):std::nullopt;
            // Capture actual completed PRE-clear per-sample bytes. No previous
            // presentation image, canned clear image, or supplied game pixels.
            StoppedColorSamples samples;
#if __has_include("native_gamma_snapshot_module.h")
            samples=captureCompletedColorSamples(ctx.r3.u32,object);
#endif
            const auto uniformRgba=target.image->readUniformResolved();
            const auto completed=shape && before?recomp::gpu::packNativeColor8ResolveFace(p,*description,samples.rgba,*before,request.slice):std::nullopt;
            const bool needsClear=audit.plan && audit.plan->clearAfterCopy.has_value();
            if(completed && needsClear)physicalMutation.candidate().reset();
            const bool clearCompleted=completed && needsClear && target.image->clearColorPreservingDepthStencil(*audit.plan->clearAfterCopy);
            const bool published=completed && (!needsClear || clearCompleted) &&
                inlineRegistry->publishSerializedGraphicsData(*address,*before,*completed);
            if(clearCompleted)++target.contentSerial;
            if(published) {
                // For a cube this records ONLY the completed face's byte range;
                // the normal 2D resource-wait/sampling planner still rejects it.
                live.completedTextures[*address]={*description,face->storage.sourceBytes};++liveResolves;
                rendererLog("RENDER_API_COLOR8_RESOLVE source=%08x destination=%08x storage=%08x samples=%u policy=pwl-linear-rne cube=%u face=%u bytes=%zu clear_completed=%u depth_uniform=%u serial=%llu published=1\n",
                    object,destination,*address,p.samples,unsigned(description->dimension==recomp::gpu::XenosTextureDescription::Dimension::Cube),request.slice,completed->size(),
                    unsigned(clearCompleted),unsigned(target.image->isDepthStencilUniform()),static_cast<unsigned long long>(target.contentSerial));
                resolved=true;return;
            }
            // A clear/publication failure after a prepared copy may have
            // changed source storage. Keep the PRE-clear evidence and stop.
            if(completed && needsClear)target.valid=false;
            if(!firstUnsupportedResolve) {
                firstUnsupportedResolve.emplace();auto& stopped=*firstUnsupportedResolve;
                stopped.arguments={ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,
                    ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,static_cast<std::uint32_t>(ctx.lr),object};
                stopped.stack=ctx.r1.u32;stopped.source=object;stopped.f1=ctx.f1.u64;
                stopped.sourcePlan={p.object,p.width,p.height,p.pitch,p.baseTile,p.tileCount};
                stopped.sourceSamples=p.samples;stopped.sourceColorFormat=p.format;stopped.sourceClearOnly=wasUniform;
                stopped.colorSamples=std::move(samples);stopped.pointed=args;stopped.clearCompleted=clearCompleted;
                // 8..12: source, destination face, storage ownership, samples/
                // packing, publication respectively. Low bits remain ABI audit.
                stopped.issues=audit.issues|(!sourceReady?1u<<8:0)|(!face?1u<<9:0)|(!before?1u<<10:0)|
                    (!completed?1u<<11:0)|(completed && !published?1u<<12:0);
                stopped.graphicsHeader=destination>=0x20000000;
                if(state)stopped.state=*state;stopped.surfaces=bindings;
                const auto ownedSurface=bindings?bindings->surfaces[0].header:std::nullopt;
                if(ownedSurface && ownedSurface->size()==40) {
                    stopped.sourceHeader.assign(ownedSurface->begin(),ownedSurface->begin()+32);
                    stopped.sourceResourceTail.assign(ownedSurface->begin()+32,ownedSurface->end());
                    if(*ownedSurface==target.header && uniformRgba)stopped.sourceRgba=*uniformRgba;
                }
                if(before)stopped.destinationStorage=*before;
                if(header)stopped.destinationHeader=*header;
                stopped.stack92=stopped.pointed.stack92.value_or(std::vector<std::uint8_t>{});
                stopped.stack100=stopped.pointed.stack100.value_or(std::vector<std::uint8_t>{});
            }
            rendererLog("RENDER_API_COLOR8_RESOLVE_UNAVAILABLE source=%08x samples=%u format=%u clear_only=%u serial=%llu source_ready=%u face=%u shape=%u storage=%u packed=%u clear_completed=%u\n",
                object,p.samples,p.format,unsigned(wasUniform),static_cast<unsigned long long>(target.contentSerial),unsigned(sourceReady),unsigned(face.has_value()),
                unsigned(shape),unsigned(before.has_value()),unsigned(completed.has_value()),unsigned(clearCompleted));
            return;
        }
        const auto offscreenFound=offscreenTargets.find({ctx.r3.u32,object});
        if(offscreenFound!=offscreenTargets.end()) {
            auto& offscreen=offscreenFound->second;
            const auto surface=inlineRegistry->copySerializedGuestHeap(object,static_cast<std::uint32_t>(offscreen.header.size()));
            const recomp::gpu::NativeOffscreenResolveRequest request{ctx.r1.u32,ctx.r4.u32,ctx.r5.u32,
                ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32};
            const auto args=recomp::gpu::captureNativeOffscreenResolveArguments(request,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
            const auto selected=description?recomp::gpu::planNativeOffscreenResolveDestination(*description,request.mip):std::nullopt;
            auto audit=recomp::gpu::auditNativeOffscreenResolve(offscreen.image->plan(),request,args,description?&*description:nullptr);
            bool mipRegion=false;
            if(!audit.plan && description && request.mip) {
                audit=recomp::gpu::auditNativeOffscreenMipRegionResolve(offscreen.image->plan(),request,args,*description);
                mipRegion=audit.plan.has_value();
            }
            const bool sourceReady=offscreen.valid && surface && *surface==offscreen.header;
            shape=audit.plan && sourceReady && selected;
            const auto rgba=sourceReady?offscreen.image->readRgba():std::nullopt;
            const auto address=shape?textureStorageAddress(selected->storage.description,selected->storage.sourceBytes):std::nullopt;
            const auto before=address?inlineRegistry->copySerializedGraphicsData(*address,selected->storage.sourceBytes):std::nullopt;
            const auto completed=rgba && before?(mipRegion?recomp::gpu::packNativeOffscreenResolveMipRegion(
                offscreen.image->plan(),*description,request.mip,audit.plan->width,audit.plan->height,*rgba,*before):
                recomp::gpu::packNativeOffscreenResolveLevel(offscreen.image->plan(),*description,request.mip,*rgba,*before)):std::nullopt;
            // The destination is the PRE-clear GPU readback. Validate and pack
            // everything before doing the requested source clear; publish only
            // after actual completion, still inside the exclusive guest gate.
            const bool needsClear=audit.plan && audit.plan->clearAfterCopy.has_value();
            if(completed && address && needsClear)physicalMutation.candidate().reset();
            const bool clearCompleted=completed && address && needsClear &&
                (mipRegion?offscreen.image->clearColorRegion(*audit.plan->clearAfterCopy,audit.plan->width,audit.plan->height):
                    offscreen.image->clearColor(*audit.plan->clearAfterCopy));
            const bool published=completed && address && (!needsClear || clearCompleted) &&
                inlineRegistry->publishSerializedGraphicsData(*address,*before,*completed);
            if(published) {
                // Only this copied level completed. Never claim other mips
                // initialized or the original whole chain consumed by a draw.
                live.completedTextures[*address]={selected->storage.description,selected->storage.sourceBytes};++liveResolves;
            } else {
                offscreen.valid=false;
                if(!firstUnsupportedResolve) {
                    firstUnsupportedResolve.emplace();auto& stopped=*firstUnsupportedResolve;
                    stopped.arguments={ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,
                        ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,static_cast<std::uint32_t>(ctx.lr),object};
                    stopped.stack=ctx.r1.u32;stopped.source=object;stopped.f1=ctx.f1.u64;
                    stopped.pointed=args;stopped.issues=audit.issues;
                    stopped.sourcePlan=offscreen.image->plan();
                    if(rgba)stopped.sourceRgba=*rgba;
                    if(before)stopped.destinationStorage=*before;
                    stopped.graphicsHeader=destination>=0x20000000;stopped.clearCompleted=clearCompleted;
                    if(auto b=inlineRegistry->copySerializedGuestHeap(ctx.r3.u32,13860))stopped.state=std::move(*b);
                    if(surface)stopped.sourceHeader=*surface;
                    if(header)stopped.destinationHeader=*header;
                    for(unsigned offset:{92u,100u}) {
                        const auto a=std::uint64_t{ctx.r1.u32}+offset;
                        if(a<=UINT32_MAX)if(auto b=inlineRegistry->copySerializedGuestArgument(static_cast<std::uint32_t>(a),4))
                            (offset==92?stopped.stack92:stopped.stack100)=std::move(*b);
                    }
                }
            }
            rendererLog("RENDER_API_OFFSCREEN_RESOLVE source=%08x destination=%08x width=%u height=%u pitch=%u format=%u shape=%u published=%u bytes=%zu issues=%08x clear_requested=%u clear_completed=%u mip=%u storage=%08x\n",
                object,destination,description?description->width:0,description?description->height:0,
                description?description->pitchTexels:0,description?description->format:0,unsigned(shape),unsigned(published),published?completed->size():0,
                audit.issues,unsigned(needsClear),unsigned(clearCompleted),request.mip,address.value_or(0));
            resolved=published;return;
        }
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
        if(!shape && object==live.object && live.image) {
            char why[160];std::snprintf(why,sizeof why,"r4=%08x r5=%08x r7=%08x r8=%08x r9=%08x r10=%08x f1=%g",
                ctx.r4.u32,ctx.r5.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,ctx.f1.f64);
            diagShowFramesReason("front-resolve-arguments",why);
            shape=true;
        }
        shape=shape && live.image;
#endif
        shape=shape && object==live.object && layout && layout->sourceBytes==3768320 &&
            description->width==1280 && description->height==720 && description->pitchTexels==1280 &&
            description->format==6 && description->tiled && description->endian==0 &&
            description->swizzle==std::array<unsigned,4>{2,1,0,5};
        const auto before=shape?inlineRegistry->copySerializedGraphicsData(description->baseAddress,layout->sourceBytes):std::nullopt;
        auto completed=before?live.image->resolveTiled(*before):std::nullopt;
        bool published=false;
        if(completed) {
            auto& saved=liveResolvedTextures[destination];
            saved.address=description->baseAddress;
            saved.bytes=std::move(*completed);
            published=inlineRegistry->publishSerializedGraphicsData(saved.address,*before,saved.bytes);
            if(published){
                ++liveResolves;live.resolvedSerial=live.contentSerial;
                live.completedTextures[description->baseAddress]={*description,layout->sourceBytes};
            }
            if(!published)liveResolvedTextures.erase(destination);
        }
        if(!published)live.valid=false;
        rendererLog("RENDER_API_LIVE_RESOLVE destination=%08x shape=%u gpu_completed=%u published=%u bytes=%zu\n",
            destination,unsigned(shape),unsigned(completed.has_value()),unsigned(published),published?before->size():0);
        resolved=published;
        };
        resolve();return resolved;
        },[&](auto stage){captureNativeResolveFailure(ctx,stage);});
        if(resolved)physicalMutation.commit();
    });
    return gated && resolved;
#else
    (void)ctx;return false;
#endif
}

// Attempt v181/v222 (goal step 1, PGO; restored in v222): in a build linked with -fprofile-generate, touching files/private-trace/diag-pgo-dump
// makes the next present write the instrumentation counters to files/private-trace/pgo-<pid>-<n>.profraw (the app is
// force-stopped, so the runtime's own at-exit write never happens) and removes the switch file. Weak references: every
// other build has no profile runtime and this does nothing. Checked every 60 presents.
extern "C" __attribute__((weak)) int __llvm_profile_write_file(void);
extern "C" __attribute__((weak)) void __llvm_profile_reset_counters(void); // v417 (PGO round 5 twin)
extern "C" __attribute__((weak)) void __llvm_profile_set_filename(const char*);
void pgoTick() {
    static unsigned calls=0,dumps=0;
    if(!__llvm_profile_write_file || (calls++%60))return;
    // v417 (PGO round 5 twin, as v372/v392): files/private-trace/diag-pgo-reset zeroes the counters (training covers gameplay, not loading) and is removed
    constexpr const char* Reset=RECOMP_APP_PATH("/private-trace/diag-pgo-reset");
    if(__llvm_profile_reset_counters && ::access(Reset,F_OK)==0){
        __llvm_profile_reset_counters();::unlink(Reset);rendererLog("RENDER_DIAG_PGO_RESET presents=%u\n",calls);
    }
    constexpr const char* Switch=RECOMP_APP_PATH("/private-trace/diag-pgo-dump");
    if(::access(Switch,F_OK)!=0)return;
    char path[160];std::snprintf(path,sizeof path,RECOMP_APP_PATH("/private-trace/pgo-%d-%u.profraw"),
        static_cast<int>(::getpid()),++dumps);
    if(__llvm_profile_set_filename)__llvm_profile_set_filename(path);
    const auto started=std::chrono::steady_clock::now();
    const int rc=__llvm_profile_write_file();
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    ::unlink(Switch);
    rendererLog("RENDER_DIAG_PGO_DUMP path=%s rc=%d ms=%.1f presents=%u\n",path,rc,ms,calls);
}
bool ownsNativeRendering(std::uint32_t device);
extern "C" std::atomic<unsigned> recomp_v338_spin_render_us,recomp_v338_presents; // v338 (generic_renderer.cpp)
extern "C" std::atomic<unsigned> recomp_v344_render_cpu_us,recomp_v344_render_wait_us,recomp_v344_main_cpu_us,recomp_v344_main_wait_us; // v344
static bool v344Schedstat(const char* path,std::uint64_t& run,std::uint64_t& wait) {
    FILE* f=std::fopen(path,"r");if(!f)return false;
    unsigned long long a=0,b=0;const bool ok=std::fscanf(f,"%llu %llu",&a,&b)==2;std::fclose(f);
    if(ok){run=a;wait=b;}return ok;
}
// pace-caps (lane A, pace_caps.h; switches diag-pace-40 / -30 / -24 / -auto, polled by generic_renderer.cpp paceWindow()): the render
// thread's holds. Phase 0, at the top of the present hook, before the gate: until the frame's slot minus the lead (main, when it
// reaches its kick, parks on the render thread's acknowledgement as it does when the game's own limiter sleeps here). Phase 1, at
// the top of the presentation callback (the frame's present is queued): diag-pace-lowlat's second hold. Both park through
// recomp_pace_park_until (trace_runner.cpp: the executor's delay, i.e. the task gives its permit back and sleeps on its condition
// variable until the deadline; a world stop does not wait for it; no spin), after recomp_pace_park_ready (a pending deferred
// interrupt is delivered first). The sequence itself is pace_caps.h hold(), which the host tests run too. While the thread is
// held, main may use the prime core until its kick, through the standing v245 protocol's own calls (pace_caps.h PrimeLend;
// nothing happens there unless diag-v245-swap is on; diag-pace-nolend leaves the protocol alone).
// With no cap file, or on any other thread than the render thread: one relaxed load (and a token compare) and a return.
// Out of line, and called where no cleanup is live (the park may throw the executor's cancellation like any guest wait): the two
// callers keep their control flow (PGO records). Under a cap main's schedstat is read every 8 presents (read-only /proc).
extern "C" __attribute__((weak)) bool recomp_pace_park_ready();                   // trace_runner.cpp (absent in host tests)
extern "C" __attribute__((weak)) bool recomp_pace_park_until(long long steadyNs); // trace_runner.cpp (absent in host tests)
recomp::pace::PrimeLend<recomp_v245::PrimeSwap> paceLend; // (render thread only)
[[gnu::noinline]] void paceHold(unsigned phase) {
    auto& p=recomp::pace::pacer();
    if(!p.on() || recomp::bootstrap::currentGuestThreadToken()!=recomp_v245::RenderToken)return;
    if(phase==0 && p.wantsMainBusy()) if(const int mt=recomp_v245::state().mainTid.load(std::memory_order_relaxed)) {
        char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/schedstat",mt);
        std::uint64_t run=0,wait=0;
        if(v344Schedstat(path,run,wait))p.mainBusy(recomp::pace::monotonicNs(),run+wait);
    }
    recomp::pace::hold(p,paceLend,recomp_v245::state(),phase,recomp::pace::monotonicNs,
        []{return recomp_pace_park_ready && recomp_pace_park_until && recomp_pace_park_ready();},
        [](std::int64_t until){return recomp_pace_park_until(until);});
}
bool genericPresentHook(const PPCContext& ctx) {
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    paceHold(0); // pace-caps (lane A): the frame's slot (no cap file: a load and a return)
    {static unsigned presents=0;if(!(++presents%60))switchEpoch.fetch_add(1,std::memory_order_relaxed);} // v330a
    ::recomp_v338_presents.fetch_add(1,std::memory_order_relaxed); // v338 (diagnostic causal spins)
    { // v344 (diagnostic accounting): the render thread's and main's CPU and run-queue wait per frame, every 60 presents
        static unsigned n=0;static std::uint64_t rRun=0,rWait=0,mRun=0,mWait=0;
        if(!(++n%60)) {
            std::uint64_t run=0,wait=0;
            if(v344Schedstat("/proc/thread-self/schedstat",run,wait)){if(rRun){::recomp_v344_render_cpu_us.store(unsigned((run-rRun)/60000),std::memory_order_relaxed);
                ::recomp_v344_render_wait_us.store(unsigned((wait-rWait)/60000),std::memory_order_relaxed);}rRun=run;rWait=wait;}
            if(const int mt=recomp_v245::state().mainTid.load(std::memory_order_relaxed)) {
                char path[64];std::snprintf(path,sizeof path,"/proc/self/task/%d/schedstat",mt);
                if(v344Schedstat(path,run,wait)){if(mRun){::recomp_v344_main_cpu_us.store(unsigned((run-mRun)/60000),std::memory_order_relaxed);
                    ::recomp_v344_main_wait_us.store(unsigned((wait-mWait)/60000),std::memory_order_relaxed);}mRun=run;mWait=wait;}
            }
        }
    }
    if(const auto us=::recomp_v338_spin_render_us.load(std::memory_order_relaxed)) {
        const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(us);while(std::chrono::steady_clock::now()<until){}
    }
    const HostActivityScope presentActivity("present"); // v226
    if(!genericReady() || !ownsNativeRendering(ctx.r3.u32) || !inlinePresent || !inlineGate)return false;
    finishDrawVerticesFrame(); // v56
    finishInlineConstantsFrame(); // v80
    bool presented=false,handledQueued=false;const char* why="gate";
    gateRef([&] {
        const auto header=inlineRegistry->copySerializedGuestHeap(ctx.r4.u32,52);
        const auto d=header?recomp::gpu::describeGta5TextureHeader(*header):std::nullopt;
        if(!d){why="front-buffer-header";return;}
        static unsigned exposureStateCalls=0; // v75
        const bool censusFrame=recomp::gpu::generic::censusActive();
        if(censusFrame || !(exposureStateCalls++%120))genericExposureState(censusFrame?"census-before-present":"periodic");
        recomp::diag::timeline::mark(recomp::diag::timeline::Present); // v159 (frame boundary)
        { // v286 (measurement): per-frame present intervals at the same boundary as the timeline's Present, as a histogram
          // every 60 intervals (RENDER_DIAG_FRAME_HIST n sum_us bins=index:count,... with 0.25 ms bins, index = us / 250,
          // capped at 399 = 99.75 ms and over) so tools/iter/frame_median.py can merge any stretch of the run into an exact
          // per-frame median (60-present window means are not the goal's median). Render thread only.
            static std::uint64_t lastNs=0;static std::array<std::uint16_t,400> hist{};static unsigned count=0;static std::uint64_t sumUs=0;
            const auto nowNs=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if(lastNs && nowNs>lastNs){const auto us=(nowNs-lastNs)/1000;++hist[std::min<std::uint64_t>(us/250,399)];++count;sumUs+=us;}
            lastNs=nowNs;
            if(count>=60) {
                char bins[1600];int w=0;
                for(unsigned i=0;i<hist.size() && w<int(sizeof bins)-16;++i)if(hist[i])w+=std::snprintf(bins+w,sizeof bins-w,"%s%u:%u",w?",":"",i,unsigned(hist[i]));
                rendererLog("RENDER_DIAG_FRAME_HIST n=%u sum_us=%llu bins=%s\n",count,(unsigned long long)sumUs,bins);
                hist.fill(0);count=0;sumUs=0;
            }
        }
        recomp_v245::state().arm(); // v245: the render thread's next not-ready wait may lend cpu7 to main
        recomp::diag::timeline::tick();
        pgoTick(); // v181/v222 (inert unless the profile runtime is linked)
        auto rgba=recomp::gpu::generic::present(d->baseAddress,1280,720,&why);
        if(censusFrame)genericExposureState("census-after-present");
        // v177: a queued present without an image yet (the first one after it engages) handled this frame: its image
        // is shown by the next present, so the native present path must not run for it.
        if(!rgba){
            if(why && !std::strcmp(why,"ios-shared-queued"))handledQueued=true; // handled, not Accepted or displayed
            else if(why && !std::strcmp(why,"present-queued"))presented=true;
            return;
        }
        auto& target=liveTargets[ctx.r3.u32];
        // v43: queued for the presenter thread when it takes the frame (the scan-out mapping and the app's
        // display then run there, outside this gate); otherwise displayed here as before. A queued frame is
        // counted as presented now: a display failure is logged when it happens and turns queuing off, so
        // later frames take this synchronous path and its fallbacks again.
        // v158: with an interpolated frame, it is shown now and the real frame half the present interval later.
        static std::chrono::steady_clock::time_point lastPresent{};static double intervalUs=50000;
        const auto nowPresent=std::chrono::steady_clock::now();
        if(lastPresent!=std::chrono::steady_clock::time_point{}) {
            const double us=std::chrono::duration<double,std::micro>(nowPresent-lastPresent).count();
            if(us>0 && us<500000)intervalUs=0.8*intervalUs+0.2*us;
        }
        lastPresent=nowPresent;
        auto mid=asyncPresentPair?recomp::gpu::generic::takeInterpolatedFrame():std::nullopt;
        const auto delay=std::chrono::microseconds(std::int64_t(std::clamp(intervalUs*0.5,4000.0,60000.0)));
        // v255: an empty image is a frame the renderer presented to the window itself (direct_present.h): shown; the sink
        // only reports it (performance hint).
        const bool direct=rgba->empty();
        const bool queued=direct ? (!asyncPresent || asyncPresent(*rgba,1280,720))
                        : mid && mid->size()==rgba->size() ? asyncPresentPair(*mid,*rgba,1280,720,delay)
                                                          : asyncPresent && asyncPresent(*rgba,1280,720);
        if(queued)++hot.asyncPresents;else ++hot.syncPresents;
        // v96: the renderer stops copying frames out while the display keeps failing (surface gone), see
        // generic::displayResult; the fallback below is unchanged.
        const bool shown=queued || inlinePresent(*rgba,1280,720);
        recomp::gpu::generic::displayResult(shown);
        if(shown) {
            presented=true;++livePresents;++target.presentCount;
            if(!direct){auto old=std::move(diagnosticLastFrame);diagnosticLastFrame=std::move(*rgba);recomp::gpu::generic::recycleFrame(std::move(old));} // v225
            // (v255: a direct frame keeps the last CPU frame for presentLiveTexture's re-present fallbacks)
            recordDiagnosticPresent();
        } else why="display";
        if(presented && !(livePresents&255)) {
            const auto& c=genericShaderState;
            rendererLog("RENDER_DIAG_HOT presents=%u owner_checks_ungated=%llu owner_checks_gated=%llu shader_code_hits=%llu "
                "shader_code_misses=%llu shader_code_clears=%llu alternates_reused=%llu alternates_rebuilt=%llu variant_walks=%llu "
                "constant_banks_in_place=%llu constant_banks_assembled=%llu constant_banks_shadow=%llu async_presents=%llu "
                "sync_presents=%llu scissor_lines=%llu scissor_lines_suppressed=%llu texture_spans=%llu texture_slot_reads=%llu "
                "constant_uploads=%llu literal_loads=%llu vertex_selections_kept=%llu vertex_selections_read=%llu scissor_direct=%llu\n",
                livePresents,(unsigned long long)hot.ownerUngated.load(std::memory_order_relaxed),
                (unsigned long long)hot.ownerGated.load(std::memory_order_relaxed),(unsigned long long)c.cache.hits,
                (unsigned long long)c.cache.misses,(unsigned long long)c.cache.clears,(unsigned long long)c.memoHits,
                (unsigned long long)c.memoMisses,(unsigned long long)c.unknownWalks,(unsigned long long)hot.bankInPlace,
                (unsigned long long)hot.bankAssembled,(unsigned long long)hot.bankShadowOnly,(unsigned long long)hot.asyncPresents,
                (unsigned long long)hot.syncPresents,(unsigned long long)hot.scissorLines,(unsigned long long)hot.scissorLinesSuppressed,
                (unsigned long long)hot.textureSpans,(unsigned long long)hot.textureSlotReads,(unsigned long long)hot.constantUploads,
                (unsigned long long)hot.literalLoads,(unsigned long long)hot.vertexSelectionsKept,(unsigned long long)hot.vertexSelectionsRead,
                (unsigned long long)hot.scissorDirect); // (v278)
        }
    });
    genericNote("present",why);
    return presented || handledQueued; // suppress native fallback without a display-success count
#else
    (void)ctx;return false;
#endif
}
bool genericResolveHook(const PPCContext& ctx) {
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    if(!genericReady() || !ownsNativeRendering(ctx.r3.u32) || !inlineGate)return false;
    const char* why="gate";
    gateRef([&] {
        const auto heap=[&](std::uint32_t a,std::uint32_t n){return inlineRegistry->copySerializedGuestHeap(a,n);};
        const auto data=[&](std::uint32_t a,std::uint32_t n){return inlineRegistry->copySerializedGraphicsData(a,n);};
        const auto argument=[&](std::uint32_t a,std::uint32_t n){return inlineRegistry->copySerializedGuestArgument(a,n);};
        const auto state=heap(ctx.r3.u32,13860);
        if(!state || state->size()!=13860){why="device-state";return;}
        const auto header=recomp::gpu::captureNativeResolveTextureHeader(ctx.r6.u32,heap,data);
        const auto d=header?recomp::gpu::describeGta5TextureHeader(*header):std::nullopt;
        if(!d){why="destination-header";return;}
        recomp::gpu::generic::ResolveInput in;in.state=&*state;in.flags=ctx.r4.u32;in.destinationFetch=d->fetch;
        if(ctx.r5.u32)if(const auto r=argument(ctx.r5.u32,16);r && r->size()==16)
            in.sourceRect=std::array<std::int32_t,4>{std::int32_t(genericBe32(*r,0)),std::int32_t(genericBe32(*r,4)),
                std::int32_t(genericBe32(*r,8)),std::int32_t(genericBe32(*r,12))};
        if(ctx.r7.u32)if(const auto p=argument(ctx.r7.u32,8);p && p->size()==8)
            in.destinationPoint=std::array<std::int32_t,2>{std::int32_t(genericBe32(*p,0)),std::int32_t(genericBe32(*p,4))};
        if(ctx.r10.u32)if(const auto c=argument(ctx.r10.u32,16);c && c->size()==16)
            for(unsigned i=0;i<4;++i){const auto bits=genericBe32(*c,4*i);std::memcpy(&in.clearColor[i],&bits,4);}
        // v40: the rest of 82388A38's arguments as 823C2738 consumes them: r8 destination level, r9
        // slice, f1 the depth clear value, caller stack words +92 (stencil clear value, 836ADD90 ORs it
        // into RB_DEPTH_CLEAR) and +100 (a parameter pointer, logged).
        in.mip=ctx.r8.u32;in.slice=ctx.r9.u32;in.clearDepth=float(ctx.f1.f64);
#ifdef RECOMP_THOR_EDITION
        in.caller=static_cast<std::uint32_t>(ctx.lr); // v580 (census only): who resolves (G7: the G-buffer tile resolves) (ios-duo: from v687)
        if(const auto bc=argument(ctx.r1.u32,4);bc && bc->size()==4)if(const auto lr=argument(genericBe32(*bc,0)-8,4);lr && lr->size()==4)in.caller2=genericBe32(*lr,0);
#endif /* RECOMP_THOR_EDITION */
        if(const auto s=argument(ctx.r1.u32+92,4);s && s->size()==4)in.clearStencil=genericBe32(*s,0);
        if(const auto s=argument(ctx.r1.u32+100,4);s && s->size()==4)in.parameters=genericBe32(*s,0);
        recomp::gpu::generic::resolve(in,&why);
    });
    genericNote("resolve",why);
    return true;
#else
    (void)ctx;return false;
#endif
}
void genericClear(std::uint32_t device,std::uint32_t flags,std::uint32_t rect,std::uint32_t color,double depth,std::uint32_t stencil) {
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    const char* why="gate";
    gateRef([&] {
        const auto state=inlineRegistry->copySerializedGuestHeap(device,13860);
        if(!state || state->size()!=13860){why="device-state";return;}
        recomp::gpu::generic::ClearInput in;in.state=&*state;in.scissor=driverScissors[device];in.flags=flags;
        if(rect)if(const auto r=inlineRegistry->copySerializedGuestArgument(rect,16);r && r->size()==16) {
            in.hasRect=true;for(unsigned i=0;i<4;++i)in.rect[i]=std::int32_t(genericBe32(*r,4*i));
        }
        if(color)if(const auto c=inlineRegistry->copySerializedGuestArgument(color,16);c && c->size()==16)
            for(unsigned i=0;i<4;++i){const auto bits=genericBe32(*c,4*i);std::memcpy(&in.color[i],&bits,4);}
        in.depth=float(depth);in.stencil=stencil;
        recomp::gpu::generic::clear(in,&why);
    });
    genericNote("clear",why);
#else
    (void)device;(void)flags;(void)rect;(void)color;(void)depth;(void)stencil;
#endif
}

bool presentLiveTexture(std::uint32_t device,std::uint32_t object) {
    stopPendingTileRecording("present");
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineRegistry || !inlineGate || !inlinePresent)return false;
    bool completed=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("present");
        const auto target=liveTargets.find(device);
        const auto texture=liveResolvedTextures.find(object);
        if(target==liveTargets.end())return;
        if(!target->second.valid || texture==liveResolvedTextures.end()) {
            if(!target->second.ownsPresentation)return;
            recordDiagnosticInvalidPresent(object,target->second.valid,texture!=liveResolvedTextures.end());
            if(texture==liveResolvedTextures.end()) {
                completed=!diagnosticLastFrame.empty() && inlinePresent(diagnosticLastFrame,1280,720);
                if(!completed)throw std::runtime_error("Native Present encountered an unsupported or unresolved frame");
                ++livePresents;++target->second.presentCount;recordDiagnosticPresent();return;
            }
            // DIAGNOSTIC: fall through and present the last published (partial or stale) resolve.
        }
        const auto& resolved=texture->second;
        const auto header=inlineRegistry->copySerializedGuestHeap(object,52);
        const auto d=header?recomp::gpu::describeGta5TextureHeader(*header):std::nullopt;
        const auto current=inlineRegistry->copySerializedGraphicsData(resolved.address,
            static_cast<std::uint32_t>(resolved.bytes.size()));
        const auto pixels=d && d->baseAddress==resolved.address && current && *current==resolved.bytes?
            recomp::gpu::linearizeXenosTexture2D(*d,resolved.bytes):std::nullopt;
        bool presented=false;
        if(pixels && pixels->size()==1280*720*4) {
            // Preserve the API's cumulative count across the initialization
            // handover. Subsequent counts represent completed native outputs,
            // not writes to Xbox GPU/ring counters.
            if(!target->second.ownsPresentation) {
                const auto count=inlineRegistry->copySerializedGuestHeap(device+16952,20);
                if(!count || count->size()!=20)return;
                target->second.presentCount=0;
                target->second.initialVBlank=0;
                for(unsigned i=0;i<4;++i) {
                    target->second.presentCount=(target->second.presentCount<<8)|(*count)[16+i];
                    target->second.initialVBlank=(target->second.initialVBlank<<8)|(*count)[i];
                }
                target->second.presentationEpoch=std::chrono::steady_clock::now();
            }
            std::vector<std::uint8_t> rgba(pixels->size());
            for(unsigned p=0;p<1280*720;++p)for(unsigned c=0;c<4;++c) {
                const auto swizzle=d->swizzle[c];
                rgba[p*4+c]=swizzle<4?(*pixels)[p*4+swizzle]:swizzle==4?0:255;
            }
            presented=inlinePresent(rgba,1280,720);if(presented){diagnosticLastFrame=rgba;recordDiagnosticPresent();}
            if(presented){++livePresents;++target->second.presentCount;target->second.ownsPresentation=true;completed=true;
                notePresentationOwner(device);} // v43: the owner mirror, written with the flag
        }
        // Native Present returns only after actual ordered Android output.
        // Initialization may still use the original driver until the scaler is
        // ready. After takeover, unsupported frames fail instead of silently
        // mixing an incomplete native frame back into the old submission path.
        rendererLog("RENDER_API_LIVE_PRESENT object=%08x completed=%u native_return=%u\n",object,unsigned(presented),unsigned(completed));
        if(!presented && target->second.ownsPresentation) {
            recordDiagnosticInvalidPresent(object,2,2);
            presented=!diagnosticLastFrame.empty() && inlinePresent(diagnosticLastFrame,1280,720);
            if(!presented)throw std::runtime_error("Native Present output failed after takeover");
            ++livePresents;++target->second.presentCount;completed=true;recordDiagnosticPresent();
        }
    });
    return gated && completed;
#else
    (void)device;(void)object;
    return false;
#endif
}

void deliverNativePresentationCallback(PPCContext& ctx,std::uint8_t* base) {
    paceHold(1); // pace-caps (lane A): diag-pace-lowlat's hold after the present (no cap file: a load and a return)
    stopPendingTileRecording("presentation-callback");
#if (defined(__ANDROID__) || defined(__APPLE__))
    std::uint32_t callback=0;
    std::array<std::uint32_t,3> notification{};
    const auto device=ctx.r3.u32;
    const auto stack=ctx.r1.u32;
    if(!inlineGate || !inlineRegistry || stack<128 || !gateRef([&] {
        stopPendingTileRecording("presentation-callback");
        const auto it=liveTargets.find(device);
        if(it==liveTargets.end() || !it->second.ownsPresentation)
            throw std::runtime_error("Native completion has no presentation owner");
        // 836B5470 registers this callback. Original 836B4A68 invokes it
        // with {vblank count, completed presentations, zero}. The native
        // 60-Hz display clock is monotonic; completed count comes only from
        // successful ordered output. No Xbox MMIO/counter writes are needed.
        const auto slot=inlineRegistry->copySerializedGuestHeap(device+16948,4);
        if(!slot || !inlineRegistry->copySerializedGuestArgument(stack-128,128))
            throw std::runtime_error("Native completion callback storage is unavailable");
        for(auto b:*slot)callback=(callback<<8)|b;
        const auto& live=it->second;
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-live.presentationEpoch).count();
        notification={live.initialVBlank+static_cast<std::uint32_t>(elapsed*60/1000000000),
            live.presentCount,0};
    }))throw std::runtime_error("Native completion callback gate rejected");
    if(!callback)return;
    // Borrow an ordinary frame on this actual guest worker's stack. Run the
    // registered game code through the validated indirect resolver, with its
    // normal scheduler checkpoints, then restore our caller's CPU registers.
    struct Restore {PPCContext& ctx;PPCContext saved;~Restore(){ctx=saved;}} restore{ctx,ctx};
    const auto frame=stack-128;
    PPC_STORE_U32(frame,stack);
    for(unsigned i=0;i<3;++i)PPC_STORE_U32(frame+80+i*4,notification[i]);
    ctx.r1.u64=frame;ctx.r3.u64=frame+80;
    PPC_CALL_INDIRECT_FUNC(callback);
    if(ctx.r1.u32!=frame || ctx.r13.u64!=restore.saved.r13.u64)
        throw std::runtime_error("Native completion callback did not preserve stack/PCR");
    rendererLogQuiet("RENDER_API_PRESENT_CALLBACK callback=%08x completed_count=%u returned=1\n",
        callback,notification[1]);
#else
    (void)ctx;(void)base;
#endif
}

bool completeNativePresentDrain(std::uint32_t device) {
    stopPendingTileRecording("present-drain");
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineGate || !ragePresentDepth)return false;
    bool completed=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("present-drain");
        const auto it=liveTargets.find(device);
        if(it==liveTargets.end() || !it->second.ownsPresentation)return;
        const auto& live=it->second;
        // This is the pre-Present drain only, not an arbitrary command-list
        // submission replacement. Every native clear/draw/resolve is already
        // submitted and fence-waited; require the current contents resolved.
        if(!live.valid || !live.contentSerial || live.contentSerial!=live.resolvedSerial ||
            pendingInline.device==device)
            recordDiagnosticForcedDrain(device);
        completed=true;
        rendererLogQuiet("RENDER_API_PRESENT_DRAIN device=%08x completed_serial=%llu\n",
            device,static_cast<unsigned long long>(live.resolvedSerial));
    });
    return gated && completed;
#else
    (void)device;return false;
#endif
}

bool nativePresentStatistics(PPCContext& ctx,std::uint8_t* base) {
    stopPendingTileRecording("present-statistics");
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineRegistry || !inlineGate)return false;
    bool completed=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("present-statistics");
        const auto it=liveTargets.find(ctx.r3.u32);
        if(it==liveTargets.end() || !it->second.ownsPresentation)return;
        if(!inlineRegistry->copySerializedGuestArgument(ctx.r4.u32,12))
            throw std::runtime_error("Native presentation statistics output is not owned");
        // The observed API returns {status, present count, queued presents}.
        // Our output callback is synchronous, so no presents remain queued.
        PPC_STORE_U32(ctx.r4.u32,0);
        PPC_STORE_U32(ctx.r4.u32+4,it->second.presentCount);
        PPC_STORE_U32(ctx.r4.u32+8,0);
        completed=true;
    });
    return gated && completed;
#else
    (void)ctx;(void)base;return false;
#endif
}

bool completeNativeResourceWait(std::uint32_t object) {
    stopPendingTileRecording("resource-wait");
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineRegistry || !inlineGate)return false;
    bool completed=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("resource-wait");
        bool owned=false;
        for(const auto& [device,live]:liveTargets)owned|=live.ownsPresentation;
        if(!owned)return;
        // Video texture objects may themselves live in graphics allocations,
        // unlike the presentation textures allocated from the ordinary heap.
        const auto header=recomp::gpu::captureNativeResolveTextureHeader(object,
            [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
        const auto d=header?recomp::gpu::describeGta5TextureHeader(*header):std::nullopt;
        const auto mip=d?mipTextureUse(d->fetch):std::nullopt;
        if(mip)for(const auto& [device,live]:liveTargets) {
            if(!live.ownsPresentation || !live.valid || pendingInline.device==device)continue;
            const auto prior=live.completedMipTextures.find(mip->baseAddress);
            if(prior==live.completedMipTextures.end() || !recomp::gpu::sameNativeMipTextureStorage(prior->second,*mip))continue;
            const auto base=inlineRegistry->copySerializedGraphicsData(mip->baseAddress,mip->baseBytes);
            const auto levels=inlineRegistry->copySerializedGraphicsData(mip->mipAddress,mip->mipBytes);
            if(!base || !levels || base->size()!=mip->baseBytes || levels->size()!=mip->mipBytes)continue;
            // Only records from a successful fenced draw are eligible. Both
            // storage planes are re-owned; no GPU completion word is forged.
            completed=true;
            rendererLog("RENDER_API_MIP_RESOURCE_WAIT object=%08x base=%08x mip=%08x base_bytes=%u mip_bytes=%u completed=1\n",
                object,mip->baseAddress,mip->mipAddress,mip->baseBytes,mip->mipBytes);
            return;
        }
        const auto layout=d?recomp::gpu::planXenosTexture2D(*d):std::nullopt;
        const auto address=layout?textureStorageAddress(*d,layout->sourceBytes):std::nullopt;
        for(const auto& [device,live]:liveTargets) {
            if(!live.ownsPresentation || !live.valid || pendingInline.device==device || !address)continue;
            const auto use=live.completedTextures.find(*address);
            if(use==live.completedTextures.end())continue;
            const auto& prior=use->second.description;
            // Sampling state may change independently of resource storage.
            // Require the same complete storage extent/layout, and revalidate
            // that the actual allocation still owns the requested bytes.
            if(use->second.bytes!=layout->sourceBytes || prior.width!=d->width ||
                prior.height!=d->height || prior.depthOrLayers!=d->depthOrLayers ||
                prior.pitchTexels!=d->pitchTexels || prior.format!=d->format ||
                prior.dimension!=d->dimension || prior.tiled!=d->tiled ||
                prior.endian!=d->endian || prior.mipMin!=d->mipMin || prior.mipMax!=d->mipMax)continue;
            if(!inlineRegistry->copySerializedGraphicsData(*address,layout->sourceBytes))continue;
            // drawValidatedAbi/resolveTiled wait their real Vulkan fences.
            // They own copies, not deferred reads from this guest resource.
            // A newer clear of the render target does not make a previously
            // sampled texture busy. No driver completion word is modified.
            completed=true;
            rendererLogQuiet("RENDER_API_RESOURCE_WAIT object=%08x storage=%08x bytes=%u completed=1\n",
                object,*address,layout->sourceBytes);
            return;
        }
        rendererLogQuiet("RENDER_API_RESOURCE_WAIT object=%08x storage=%08x bytes=%u header=%u decoded=%u completed=0\n",
            object,address.value_or(0),layout?layout->sourceBytes:0,unsigned(header.has_value()),unsigned(d.has_value()));
        if(d)rendererLogQuiet("RENDER_RESOURCE_LAYOUT size=%ux%ux%u pitch=%u format=%u tiled=%u endian=%u mips=%u..%u\n",
            d->width,d->height,d->depthOrLayers,d->pitchTexels,d->format,unsigned(d->tiled),d->endian,d->mipMin,d->mipMax);
        recordDiagnosticForcedWait(object);completed=true;
    });
    return gated && completed;
#else
    (void)object;return false;
#endif
}

extern "C" std::atomic<bool> recomp_v278_baseline; std::atomic<bool> recomp_v278_baseline{false}; // v278 (set with the renderer's A/B windows)
bool captureDriverScissor(std::uint32_t device,const std::array<std::uint32_t,4>& rectangle,std::uint32_t caller) {
    if(!inlineRegistry || !inlineGate)return false;
    bool captured=false;
    const bool gated=gateRef([&] {
        auto& scissor=driverScissors[device];scissor.reset();
        // v278: a scissor the planner would not bin is taken from the header in place (gta5DriverScissorDirect: the same
        // decision and value, no 13 KB copy); binned ones, and a header the view refuses, plan from the copies below.
        // diag-v278-baseline: always the copies.
        if(!recomp_v278_baseline.load(std::memory_order_relaxed))
            if(const auto direct=recomp::gpu::gta5DriverScissorDirect(deviceRange(device,device,13128),rectangle)) { // (v335)
                scissor=*direct;captured=true;++hot.scissorDirect;return;
            }
        // v43: the same two copies into reused buffers (v35: new 13,128- and 13,860-byte vectors per call).
        static thread_local std::vector<std::uint8_t> headerBytes,fullBytes;
        std::vector<std::uint8_t>* bytes=copyGuestHeapInto(device,13128,headerBytes)?&headerBytes:nullptr;
        auto plan=bytes?recomp::gpu::planGta5DriverScissor(*bytes,rectangle):std::nullopt;
        // The direct path keeps its original ownership extent. Read the full
        // fixed tables only for a binned/planning failure; no new GPU admission.
        if(bytes && (!plan || plan->binned)) {
            if(copyGuestHeapInto(device,13860,fullBytes))bytes=&fullBytes;
            plan=recomp::gpu::planGta5DriverScissor(*bytes,rectangle);
        }
#if (defined(__ANDROID__) || defined(__APPLE__)) && defined(RECOMP_CAPTURE_TILED_RECORDING)
        if(plan && plan->binned && bytes) {
            // v366e (exact; diag-v366-render-trims2): the bound-surface capture and the 13860-byte state copy below are used only by
            // begin() / recordScissor(). With no recording pending, begin() can run only for caller 823ABBC8 without
            // diag-generic-first-scene (the configuration the game is played with skips it: every binned scissor, ~45 per frame,
            // built them and dropped them; v349 drive profile ~0.5% of the render thread). Skipped then; the same calls otherwise.
            const bool firstPeek=!tileRecordingDiagnostic.pending();
            static const bool genericFirstSceneEarly=access(RECOMP_APP_PATH("/private-trace/diag-generic-first-scene"),F_OK)==0;
            const bool unusable=recomp::v366::scissorCaptureUnused(recomp_v366_render_trims2.load(std::memory_order_relaxed),firstPeek,genericFirstSceneEarly,caller);
            std::optional<recomp::gpu::Gta5BoundSurfaceCapture> bindings;
            recomp::gpu::Gta5ClearCapture current;
            if(!unusable) {
            bindings=recomp::gpu::captureGta5BoundSurfaces(*bytes,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},40);
            current.call.device=device;current.state=*bytes;
            if(bindings)for(unsigned i=0;i<5;++i) {
                const auto& b=bindings->surfaces[i];current.surfaces[i]={b.object,b.tileCacheObject,b.header};
            }
            }
            const bool first=!tileRecordingDiagnostic.pending();
            // v166 DIAGNOSTIC SWITCH (default off): files/private-trace/diag-generic-first-scene never begins the
            // capture-only first-scene recording, so the first tiled scene takes the same original-emission +
            // diagnostic-retire path as every later scene (8534 unrecorded binned scissors in run 0awbeg40) and the
            // generic renderer takes over by its existing triggers (native-present-invalid / shutdown_loading_screen).
            // Reason: a boot that loads a save starts in a different first scene (>128 indexed draws, shaders outside
            // the 14-program native pack: load-test run nihdin95 stopped at unsupported-recorded-indexed-input).
            const bool genericFirstScene=genericFirstSceneEarly; // (v366e: the one read of the switch file, above)
            if(first && genericFirstScene) {
                static bool logged=false;
                if(!logged){logged=true;rendererLog("RENDER_TILE_RECORDING_SKIPPED device=%08x caller=%08x reason=diag-generic-first-scene\n",device,caller);}
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
                // v167: the unrecorded first scene belongs to the generic renderer, as every later scene does: its
                // immediate-mode (EndVertices) draws otherwise reach the loading-screen native inline path, which
                // cannot draw into a binned scene (v166 run tr497hii: "Native EndVertices did not complete its real
                // draw" 4 s after the skip). Same rule as native-present-invalid: take over where native stops.
                genericTakeOver("first-tiled-scene-unrecorded");
#endif
            }
            const bool recorded=first?
                (!genericFirstScene && caller==0x823ABBC8 && tileRecordingDiagnostic.begin(current,rectangle,
                    [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);})):
                tileRecordingDiagnostic.recordScissor(current,rectangle);
            if(recorded) {
                if(first) {
                    firstUnsupportedScissor=UnsupportedScissor{device,caller,rectangle,*bytes,*plan,bindings};
                    // A record-only diagnostic cannot reuse a completed prior
                    // frame or discard the first scene draw behind boot history.
                    for(auto& [key,live]:liveTargets)live.valid=false;
                    // Recorded GPU work has not preserved these separate native
                    // views. Do not retain a fixed16 owner across an unexecuted
                    // EDRAM recording generation.
                    for(auto& [key,owner]:fixed16Targets)owner.valid=false;
                    boundIndexedSnapshots.clear();boundIndexedBudget={};
                    boundIndexedAttempts=0;boundIndexedFailures=0;boundIndexedDropped=0;
                    tileRecordingDevice.store(device);
                }
                rendererLog("RENDER_TILE_SCISSOR_RECORDED device=%08x first=%u scissors=%zu gpu_completed=0\n",
                    device,unsigned(first),tileRecordingDiagnostic.scissors().size());
                captured=true;return; // Keep direct scissor absent; NEVER admit a draw.
            }
        }
#endif
        if(!plan || plan->binned) {
            // v43: the first line of each distinct shape, then every power-of-two repeat, with its count
            // (v35 logged every call: ~4.6 lines per world frame on the render thread, all alike).
            const std::array<std::uint32_t,12> shape{device,caller,bytes?unsigned(bytes->size()):0,bytes?unsigned((*bytes)[11068]):0,
                bytes?unsigned((*bytes)[11071]):0,unsigned(plan.has_value()),plan?unsigned(plan->binned):0,
                plan?unsigned(plan->tiles.size()):0,rectangle[0],rectangle[1],rectangle[2],rectangle[3]};
            static std::map<std::array<std::uint32_t,12>,unsigned> seenShapes;
            // v213: the shape table and its log lines only with files/private-trace/diag-scissor-shapes (diagnostic; the
            // rectangle follows the camera, so new shapes kept logging on the render thread: 15459 lines in ~7 min, v212).
            static const bool shapeLog=access(RECOMP_APP_PATH("/private-trace/diag-scissor-shapes"),F_OK)==0;
            const bool logShapes=shapeLog || v213Baseline();
            const auto known=logShapes?seenShapes.find(shape):seenShapes.end();
            const unsigned seen=!logShapes?0:known!=seenShapes.end()?++known->second:seenShapes.size()<4096?(seenShapes[shape]=1):0;
            // v185: seen==0 is a shape past the 4096-entry table (the rectangle follows the camera, so the table fills
            // within minutes); v43-v184 logged every such call (~500 lines/s on the render thread). Counted instead.
            if(seen && !(seen&(seen-1))) {
                ++hot.scissorLines;
                rendererLog("RENDER_SCISSOR_UNSUPPORTED device=%08x caller=%08x owned_bytes=%u flags=%02x flags3=%02x planned=%u binned=%u tiles=%u rectangle=%08x/%08x/%08x/%08x seen=%u\n",
                    shape[0],shape[1],shape[2],shape[3],shape[4],shape[5],shape[6],shape[7],shape[8],shape[9],shape[10],shape[11],seen);
            } else ++hot.scissorLinesSuppressed;
            if(!firstUnsupportedScissor) {
                // Preserve actual resource dimensions/formats at THIS boundary.
                // Earlier loading-draw headers are not the current MRT headers.
                auto bindings=bytes?recomp::gpu::captureGta5BoundSurfaces(*bytes,
                    [&](auto at,auto size){return inlineRegistry->copySerializedGuestHeap(at,size);},
                    [&](auto at,auto size){return inlineRegistry->copySerializedGraphicsData(at,size);},40):std::nullopt;
                firstUnsupportedScissor=UnsupportedScissor{device,caller,rectangle,
                    bytes?std::move(*bytes):std::vector<std::uint8_t>{},std::move(plan),std::move(bindings)};
            }
            return;
        }
        scissor=plan->direct;
        captured=true;
    });
    if(!gated)rendererLog("RENDER_SCISSOR_GATE_UNAVAILABLE device=%08x caller=%08x\n",device,caller);
    return gated && captured;
}

// Capture the actual two constant-loading APIs. This preserves ordering between
// shader literals and application uploads, without interpreting PM4 packets.
bool captureDriverConstants(std::uint32_t device,std::uint64_t mask,
        std::uint32_t firstOrHeader,std::uint32_t source,bool literals) {
    if(!inlineRegistry || !inlineGate)return false;
    bool captured=false;
    const bool gated=gateRef([&] {
        const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
        // v43: the same reads through views (each the copy's validation, bytes in place); a transaction still
        // commits only after every read passed (see gta5_driver_constants.h).
        const auto heapView=[device](std::uint32_t a,std::uint32_t n){return deviceRange(device,a,n);}; // (v335: device-relative runs from the cached shadow)
        const auto dataView=[](std::uint32_t a,std::uint32_t n){return inlineRegistry->viewSerializedGraphicsData(a,n);};
        flushInlineConstants(device); // v80: a pending skinning packet precedes this upload / literal load
        auto& constants=driverConstants[device];
        ++(literals?hot.literalLoads:hot.constantUploads);
        captured=literals?constants.loadLiteralsViewed(firstOrHeader,source,heapView,dataView):
            constants.uploadViewed(mask,firstOrHeader,source,heapView);
        if(!captured && literals) {
            const auto read=[&](std::uint64_t a)->std::optional<std::uint32_t> {
                if(a>0xFFFFFFFCULL)return std::nullopt;
                const auto bytes=heap(static_cast<std::uint32_t>(a),4);
                if(!bytes || bytes->size()!=4)return std::nullopt;
                return (std::uint32_t{(*bytes)[0]}<<24)|(std::uint32_t{(*bytes)[1]}<<16)|
                    (std::uint32_t{(*bytes)[2]}<<8)|(*bytes)[3];
            };
            const auto relative=read(std::uint64_t{firstOrHeader}+20);
            const auto size=relative?read(std::uint64_t{firstOrHeader}+*relative+16):std::nullopt;
            rendererLog("RENDER_LITERAL_LAYOUT header=%08x data=%08x relative=%08x table_bytes=%u\n",
                firstOrHeader,source,relative.value_or(0xFFFFFFFF),size.value_or(0xFFFFFFFF));
        }
        if(!captured)driverConstantsValid=false;
    });
    if(!gated)driverConstantsValid=false;
    if(!captured)rendererLog("RENDER_CONSTANTS status=unavailable literals=%u device=%08x\n",unsigned(literals),device);
    return gated && captured;
}

bool ownsNativeRendering(std::uint32_t device) {
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineGate)return false;
    // v43: nearly every renderer hook asks this before its own gate (about 9 extra lock gates per draw
    // in v35). The answer is the owner mirror; admission is the gate's own refusal check (no live task,
    // stop observed, nested scope), so a refused gate still answers false. Unlike the gate this does not
    // first wait for an exclusive owner to finish: the caller's next gate or safepoint does. Unknown
    // admission (cooperative builds), device 0 or an overflowed mirror take the gated lookup below.
    if(lockGateAdmits && device && !presentationOwnersOverflow.load(std::memory_order_acquire)) {
        const int admitted=lockGateAdmits();
        if(admitted>=0) {
            { // v329c: published every 256 calls per thread (a shared RMW per call before; diag-v329c-baseline = per call)
                unsigned& pendingOwner=::recomp::v362::tls<::recomp::v362::Slot::OwnerTick,unsigned>([]() noexcept -> unsigned& {static thread_local unsigned pendingOwner=0;return pendingOwner;});
                if(v329cBaseline())hot.ownerUngated.fetch_add(1,std::memory_order_relaxed);
                else if(++pendingOwner==256){hot.ownerUngated.fetch_add(256,std::memory_order_relaxed);pendingOwner=0;}
            }
            if(!admitted)return false;
            for(const auto& slot:presentationOwners)if(slot.load(std::memory_order_acquire)==device)return true;
            return false;
        }
    }
    hot.ownerGated.fetch_add(1,std::memory_order_relaxed);
    bool owns=false;
    return gateRef([&] {
        const auto it=liveTargets.find(device);
        owns=it!=liveTargets.end() && it->second.ownsPresentation;
    }) && owns;
#else
    (void)device;return false;
#endif
}

void captureReturnedBoundIndexed(std::uint32_t device,std::uint32_t caller,
        const recomp::gpu::Gta5BoundIndexedRequest& request,BoundIndexedCapturePhase phase) noexcept {
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(!inlineRegistry || !inlineGate || !inlineGraphics)return;
    // v362 (exact): relaxed RMWs (v349 profiles: this seq_cst fetch_add, which waits for the draw's stores to drain, ~0.4-0.5%
    // of the render thread per bound draw past the 128-record budget). Same counts in the same modification order; every
    // reader (the tile-end check, the capture report, the recorded-indexed check on this thread) reads them after its own
    // synchronization (the gate or program order), which a relaxed increment respects as a seq_cst one does.
    const bool relaxed=recomp::v362::trimsOn();
    const auto sequence=(relaxed?boundIndexedAttempts.fetch_add(1,std::memory_order_relaxed):boundIndexedAttempts.fetch_add(1))+1;
    const bool exception=phase==BoundIndexedCapturePhase::Exception;
    if(!exception && sequence>BoundIndexedCaptureBudget::RoutineRecords){
        if(relaxed)boundIndexedDropped.fetch_add(1,std::memory_order_relaxed);else ++boundIndexedDropped;
        return;
    }
    try {
        if(!gateRef([&] {
            const auto available=boundIndexedBudget.available(sequence,exception);
            if(!available){++boundIndexedDropped;return;}
            BoundIndexedSnapshot s;s.sequence=sequence;s.caller=caller;s.device=device;s.request=request;
            s.capturePhase=phase;s.originalEmissionReturned=phase==BoundIndexedCapturePhase::Returned;
            const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
            const auto data=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
            const auto resource=[&](auto a,auto n){return a>=0xA0000000?data(a,n):heap(a,n);};
            const auto word=[&](std::uint32_t a)->std::optional<std::uint32_t> {
                const auto bytes=heap(a,4);if(!bytes || bytes->size()!=4)return std::nullopt;
                return (std::uint32_t{(*bytes)[0]}<<24)|(std::uint32_t{(*bytes)[1]}<<16)|
                    (std::uint32_t{(*bytes)[2]}<<8)|(*bytes)[3];
            };
            const auto& state=inlineGraphics->stateBatchSnapshot();
            const recomp::gpu::DrawBinState bins{{static_cast<std::uint32_t>(state.binMask),
                static_cast<std::uint32_t>(state.binMask>>32),static_cast<std::uint32_t>(state.binSelect),
                static_cast<std::uint32_t>(state.binSelect>>32)},state.knownBinWords};
            s.nativeBins=bins;
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
            if(tileRecordingDevice.load()==device && tileRecordingDiagnostic.pending()) {
                s.geometryRecordingOnly=true;
                s.geometry=recomp::gpu::captureGta5RecordedIndexedStorage(device,request,heap,data);
            } else
#endif
            s.geometry=recomp::gpu::captureGta5BoundIndexedDraw(device,request,bins,heap,data);
            if(auto b=heap(device,13860))s.deviceState=std::move(*b);
            s.surfaces=recomp::gpu::captureGta5BoundSurfaces(s.deviceState,heap,data,40);
            const auto vs=word(device+13072),ps=word(device+13068);
            if(const auto object=word(device+12812);object && *object)
                if(auto b=resource(*object,32))s.indexHeader=std::move(*b);
            if(const auto object=word(device+12836);object && *object)
                if(auto b=resource(*object,32))s.vertexHeader=std::move(*b);
            if(ps && *ps)if(const auto range=recomp::gpu::describeGta5ShaderCode(*ps,1,0,word))
                if(range->byteCount<=4096)if(auto b=data(range->virtualAddress,range->byteCount))s.pixelCode=std::move(*b);
            if(vs && *vs) {
                const auto binding=selectedVertexShaders.find(device);
                if(binding!=selectedVertexShaders.end())
                    if(const auto* selected=binding->second.forObject(*vs))s.vertexCode=selected->code;
            }
            s.textures=recomp::gpu::captureGta5SubmittedTextures([&](unsigned slot) {
                const auto it=driverTextures.find(device);
                return it==driverTextures.end()?std::optional<std::array<std::uint32_t,6>>{}:it->second.fetch(slot);
            });
            // Keep ordered constants even if geometry rejects. A missing
            // resource capture must not discard independent shader evidence.
            flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */
            if(s.deviceState.size()==13860 && driverConstantsValid && constants!=driverConstants.end()) {
                recomp::gpu::Gta5InlineDraw prepared;
                const auto capturedWord=[&](unsigned at) {
                    const auto* p=s.deviceState.data()+at;
                    return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
                };
                for(unsigned i=0;i<1024;++i) {
                    prepared.vertexConstants[i]=capturedWord(1920+i*4);
                    prepared.pixelConstants[i]=capturedWord(6016+i*4);
                }
                constants->second.apply(prepared);s.orderedConstants=true;
                s.vertexConstants=prepared.vertexConstants;s.pixelConstants=prepared.pixelConstants;
            }
            if(s.geometry) {
                if(s.orderedConstants) {
                    s.geometry->draw.vertexConstants=s.vertexConstants;
                    s.geometry->draw.pixelConstants=s.pixelConstants;
                }
                s.geometry->draw.scissorRegisters=driverScissors[device];
            }
            std::size_t surfaceBytes=0;
            if(s.surfaces)for(const auto& surface:s.surfaces->surfaces)if(surface.header)surfaceBytes+=surface.header->size();
            auto bytes=8192+surfaceBytes+s.deviceState.size()+s.vertexCode.size()+s.pixelCode.size()+s.indexHeader.size()+s.vertexHeader.size()+
                (s.geometry?s.geometry->draw.vertices.size()+s.geometry->draw.deviceState.size()+8192+
                    8*s.geometry->rawIndices.size():0);
            if(bytes>available){++boundIndexedDropped;return;}
            // Derive the current pixel shader's actual slots. The previous
            // union captured stale slot3/5 bindings on unrelated draws, spending
            // the budget before their own slot0 images were complete.
            // Fetch-all changes invocation semantics, not the resource slot.
            // This is diagnostic input retention ONLY, for both recording and
            // live draws; native shader/sampler/execution admission is separate.
            s.pixelTextureSlots=recomp::gpu::inspectCapturedPixelTextureSlots(s.pixelCode.data(),s.pixelCode.size());
            auto budget=std::min<std::size_t>(2*1024*1024,available-bytes);
            std::size_t textureBytes=0;
            const auto textureData=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
                if(n>budget)return std::nullopt;
                auto b=data(a,n);if(b && b->size()==n){budget-=n;textureBytes+=n;}return b;
            };
            recomp::gpu::Gta5SubmittedTextureSnapshot selected{};std::uint32_t completeSlots=0;
            for(unsigned slot=0;slot<32;++slot)if(s.pixelTextureSlots && (*s.pixelTextureSlots&(1u<<slot)) && s.textures[slot]) {
                selected[slot]=s.textures[slot];
                if(auto mip=recomp::gpu::captureNativeMipTextureStorage(*selected[slot],textureData,budget)) {
                    completeSlots|=1u<<slot;s.mipTextures.emplace_back(slot,std::move(*mip));
                }
            }
            s.texturePages=recomp::gpu::captureGta5SubmittedTexturePages(selected,completeSlots,textureData);
            bytes+=textureBytes;
            if(!boundIndexedBudget.retain(sequence,exception,bytes)){++boundIndexedDropped;return;}
            if(!s.geometry || s.vertexCode.empty() || s.pixelCode.empty() || !s.orderedConstants)++boundIndexedFailures;
            if(sequence<=4)rendererLog("RENDER_BOUND_INDEXED_INPUT sequence=%u count=%u geometry=%u constants=%u vertex_bytes=%zu vs_bytes=%zu ps_bytes=%zu recording_only=%u diagnostic_only=1\n",
                sequence,request.indexCount,unsigned(s.geometry.has_value()),unsigned(s.orderedConstants),
                s.geometry?s.geometry->draw.vertices.size():0,s.vertexCode.size(),s.pixelCode.size(),unsigned(s.geometryRecordingOnly));
            boundIndexedSnapshots.push_back(std::move(s));
        }))++boundIndexedFailures;
    } catch(...) {++boundIndexedFailures;}
#else
    (void)device;(void)caller;(void)request;(void)phase;
#endif
}

bool capturePreparedRecordedIndexed(PPCContext& ctx) {
#if (defined(__ANDROID__) || defined(__APPLE__)) && defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(!tileRecordingDevice.load())return false;
    const auto device=ctx.r31.u32;
    if(device!=tileRecordingDevice.load() || preparedTileIndexedSequence || !inlineGate || !inlineRegistry)
        stopPendingTileRecording("recorded-indexed-owner");
    const recomp::gpu::Gta5BoundIndexedRequest request{ctx.r18.u32,ctx.r17.u32,ctx.r21.u32,ctx.r19.u32};
    const auto before=boundIndexedAttempts.load();
    // Caller0 means unavailable, not the observation PC disguised as a caller.
    captureReturnedBoundIndexed(device,0,request,BoundIndexedCapturePhase::Prepared);
    bool recorded=false;
    const bool gated=gateRef([&] {
        if(boundIndexedAttempts.load()!=before+1 || boundIndexedSnapshots.empty())return;
        auto& s=boundIndexedSnapshots.back();
        if(s.sequence!=before+1 || s.device!=device)return;
        s.observationPc=0x823B9DBC;
        if(!s.geometryRecordingOnly || !s.geometry || !s.orderedConstants || !s.surfaces ||
           s.vertexCode.empty() || s.pixelCode.empty() || !tileRecordingDiagnostic.pending())return;
        const auto window=recomp::gpu::describeGta5RecordedIndexedEmission(*s.geometry,
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
        if(!window)return;
        recomp::gpu::Gta5ClearCapture current;current.call.device=device;current.state=s.deviceState;
        for(unsigned i=0;i<5;++i) {
            const auto& b=s.surfaces->surfaces[i];current.surfaces[i]={b.object,b.tileCacheObject,b.header};
        }
        if(!tileRecordingDiagnostic.recordIndexed(current,s.sequence))return;
        s.recordingWindow=*window;s.recordedScissor=tileRecordingDiagnostic.scissors().back();
        preparedTileIndexedSequence=s.sequence;recorded=true;
        rendererLog("RENDER_TILE_INDEXED_CAPTURED sequence=%u count=%u original_emission_pending=1 gpu_completed=0\n",s.sequence,request.indexCount);
    });
    if(!gated || !recorded)stopPendingTileRecording("unsupported-recorded-indexed-input");
    return true; // Allow ORIGINAL CPU recording, never return native completion.
#else
    (void)ctx;return false;
#endif
}
bool finishPreparedRecordedIndexed(bool returned) {
#if (defined(__ANDROID__) || defined(__APPLE__)) && defined(RECOMP_CAPTURE_TILED_RECORDING)
    auto& prepared=preparedTileIndexedSequenceRef(); // v362
    const auto sequence=prepared;if(!sequence)return false;
    prepared=0;
    if(inlineGate)gateRef([&] {
        for(auto& s:boundIndexedSnapshots)if(s.sequence==sequence) {s.originalEmissionReturned=returned;break;}
    });
    return true; // Snapshot already retained; suppress a duplicate full capture.
#else
    (void)returned;return false;
#endif
}
}

// Called from the private original BeginVertices immediately after ordered
// state/shader preparation, before any inline command or vertex allocation.
// Its PPC epilogue still restores all saved registers/stack/link state.
// Original 823BAAC8 still selects/patches shaders, records CPU shadow state,
// and routes literal uploads through the ordered native cache. Once native
// rendering owns this device, only its raw PM4 writes/cursor/flush are omitted
// in the private translation. Native draws bind the actual translated program.
bool usesNativeShaderBinding(std::uint32_t device) {return ownsNativeRendering(device);}

// Invoked by the private original-code overrides below, not by shader patch
// history or resource setters. Conditional multi-bind history is intentionally
// invalidated until an ordered/predicate-aware consumer is implemented.
// Tracking must begin BEFORE presentation takeover: the first native frame
// consumes shaders selected while the original command path still owns Present.
// This does not grant permission to suppress or acknowledge any guest commands.
bool tracksNativeShaderBinding(std::uint32_t device) {
    return device && inlineRegistry && inlineGate;
}
void invalidateNativeVertexShader(std::uint32_t device) {
    if(!inlineGate || !gateRef([&] {selectedVertexShaders.erase(device);++vertexSelectionSerial;})) // (v366d: serial)
        throw std::runtime_error("Native vertex shader invalidation lost executor ownership");
}
void observeNativeVertexShader(std::uint32_t device,std::uint32_t object,
        std::uint32_t variant,std::uint32_t address,std::uint32_t bytes,bool copied) {
    if(!inlineRegistry || !inlineGate)
        throw std::runtime_error("Native vertex shader selection lacks a memory owner");
    bool selected=false;
    const bool gated=gateRef([&] {
        if(!selectedVertexShaders.count(device) && selectedVertexShaders.size()>=16)
            throw std::runtime_error("Native vertex shader device limit exceeded");
        auto& binding=selectedVertexShaders[device];
        const auto source=copied?recomp::gpu::Gta5VertexShaderBinding::Source::CommandCopy:
                                 recomp::gpu::Gta5VertexShaderBinding::Source::CachedLoad;
        // v43: selecting the range that is already selected, with the same bytes, leaves the same selection
        // (select() would read and store identical bytes). Compare in place, with the copy's validation.
        if(const auto* current=binding.forObject(object);current && current->variant==variant && current->address==address &&
           current->source==source && current->code.size()==bytes) {
            const auto* view=inlineRegistry->viewSerializedGraphicsData(address,bytes);
            if(view && !std::memcmp(view,current->code.data(),bytes)){selected=true;++hot.vertexSelectionsKept;return;}
        }
        ++hot.vertexSelectionsRead;++vertexSelectionSerial; // (v366d: select() rewrites or drops the binding's bytes)
        selected=binding.select(object,variant,address,bytes,source,
            [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
    });
    if(!gated || !selected)
        throw std::runtime_error("Native selected vertex shader range is not owned or supported");
}

// Generated from verified resident originals, with observers at actual
// unconditional cached-load and successful command-copy boundaries. The
// strong entries replace weak calls while all frozen PPC objects stay intact.
// No extracted game code is checked into this repository.
#if __has_include("native_shader_selection_original.h")
#include "native_shader_selection_original.h"
#endif

// Called at original823B9DBC, after its ordered preparation but BEFORE raw
// draw emission. Only a fenced native draw may return true. No ring cursor is
// advanced here, and the original epilogue remains responsible for PPC state.
struct DiagnosticIndexedSkip {const char* stage;};
bool finishNativeBoundIndexedChecked(PPCContext& ctx,std::uint8_t* base);
bool finishNativeBoundIndexed(PPCContext& ctx,std::uint8_t* base) {
    // DIAGNOSTIC: a rejected bound-indexed draw is skipped (returned as done).
    try {return finishNativeBoundIndexedChecked(ctx,base);}
    catch(const DiagnosticIndexedSkip& skip) {recordDiagnosticSkippedIndexed(ctx.r31.u32,skip.stage);return true;}
}
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
// white-floor (2026-10-06; bound_null_pixel.h, agents/ship-week/white-floor/RESULT.md): a bound indexed draw with NO pixel
// shader bound (device +13068 == 0) is a depth-only draw of the game: every opaque sun-shadow caster is submitted that way
// (605 to 1028 per frame, all between the shadow atlas's clear and its resolve). v16 refused it below as "bound-objects"
// and the shadow atlas held the alpha-tested casters only, so the sun reached every place the game left to the shadow map
// (the light shafts of an interior's portals: the white floor of Franklin's hall). The renderer draws a draw without pixel
// code with no fragment stage since v41 (as the inline-draw and DrawVertices entries already rely on); it is now handed over.
// files/private-trace/diag-bound-skip-null-ps restores v470 (the refusal); read once per switch epoch (60 presents). One
// line at the first such draw and at every change of the switch, with the draws without a pixel shader counted so far:
// "RENDER_GENERIC_BOUND_NULL_PS mode=drawn|skipped-v470 passed=N refused=M" (passed: let through these tests; a later
// refusal of such a draw, by this hook or the renderer, shows in their own lines and counters as for any other draw).
// Three new functions; genericBoundIndexed reads two values in its two existing tests and keeps its control flow (checked:
// tools/iter/attempt_tu.py --pgo-warnings reports no hash mismatch for its body; white-floor/out/build/fix.pgo.txt).
[[gnu::noinline]] static bool boundNullPixelSkipped() noexcept { // (called for draws without a pixel shader only)
    static unsigned seen=0,checks=0;static bool skip=false; // (under the gate, as the other switch readers of the hooks)
    static unsigned long long passed=0,refused=0;
    if(switchEpochChanged(seen) && ++checks) {
        const bool now=access(RECOMP_APP_PATH("/private-trace/diag-bound-skip-null-ps"),F_OK)==0;
        if(now!=skip || checks==1)
            rendererLog("RENDER_GENERIC_BOUND_NULL_PS mode=%s passed=%llu refused=%llu\n",now?"skipped-v470":"drawn",passed,refused);
        skip=now;
    }
    ++(skip?refused:passed);
    return skip;
}
// The values the hook's two tests read (never inlined, non-throwing: one plain call each in the hook's body).
[[gnu::noinline]] static std::uint32_t boundPixelGate(std::uint32_t ps) noexcept { // the "bound objects" test's pixel term
    return recomp::gpu::boundnull::pixelGate(ps,!ps && boundNullPixelSkipped()); // (the switch is read for such draws only)
}
[[gnu::noinline]] static bool boundPixelCodeMissing(const void* pixelCode,std::uint32_t ps) noexcept { // the "shader code" test's
    return !recomp::gpu::boundnull::pixelCodeUsable(pixelCode!=nullptr,ps); // (true = refuse: the sense of the test it replaces)
}
// Attempt v16: request = 823B9B58 arguments (primitive, base vertex, start index, index count).
void genericBoundIndexed(std::uint32_t device,const recomp::gpu::Gta5BoundIndexedRequest& request) {
    const char* why="gate";
    std::uint32_t vsObject=0,psObject=0,indexObject=0; // v40 census
    gateRef([&] {
        // v43: the reads below keep v35's order and validation (each view is its copy's validation, bytes in
        // place) and take the same bytes at the same point of the draw; what changes is where they go. The
        // device shadow is copied once into a reused buffer (v35: a new 13,860-byte vector per draw), the
        // index header into a local array, the indices converted straight from the guest bytes into a reused
        // buffer (v35: a copy of the raw bytes, then a push_back loop), the constants merged in one pass
        // (v35: 2048 decodes, then apply()), and the shader code comes from genericShaderCode's byte-checked
        // cache. No Gta5InlineDraw is built: the renderer only reads the shadow, constants and code.
        auto& s=genericDrawScratch();
        // v78: the device shadow in place, with the copy's validation (v43-v71: copied into s.state per draw, then read
        // for the rest of this body). The render thread's own D3D calls write it, and that thread is inside this gate;
        // every other renderer caller (tile worker, resource-wait hook) holds its permit domain, so none runs guest
        // code while this body reads (a writer in another domain would have torn the v71 copy just the same).
        const bool inPlace=genericStateInPlace();
        const std::uint8_t* state=inPlace?deviceRange(device,device,13860):nullptr; // (v335)
        if(!inPlace){if(!copyGuestHeapInto(device,13860,s.state)){why="device-state";return;}state=s.state.data();}
        if(!state){why="device-state";return;}
        const auto field=[&](unsigned at){return recomp::gpu::hot::be32(state+at);};
        const auto indexResource=field(12812),vs=field(13072),ps=field(13068);
        vsObject=vs;psObject=ps;indexObject=indexResource;
        const auto psGate=boundPixelGate(ps); // white-floor: ps, or non-zero for a draw without a pixel shader (depth-only)
        if(!indexResource || !vs || !psGate){why="bound-objects";return;}
        const auto* headerView=indexResource>=0xA0000000?inlineRegistry->viewSerializedGraphicsData(indexResource,32):
                                                         inlineRegistry->viewSerializedGuestHeap(indexResource,32);
        if(!headerView){why="index-header";return;}
        std::array<std::uint8_t,32> header;std::memcpy(header.data(),headerView,32);
        const bool index32=(recomp::gpu::hot::be32(header.data())>>31)&1;const unsigned size=index32?4:2;
        const auto indexBase=recomp::gpu::hot::be32(header.data()+24);
        const auto start=std::uint64_t{indexBase}+std::uint64_t{request.startIndex}*size;
        if(!request.indexCount || request.indexCount>1'000'000 || start>0xFFFFFFFFull){why="index-range";return;}
        const auto physical=[](std::uint32_t address){return (address&0x1FFFFFFFu)+(((address>>20)+512u)&0x1000u);};
        const auto* raw=graphicsIndexView(physical(static_cast<std::uint32_t>(start)),request.indexCount*size); // (index view, odd 16-bit start)
        if(!raw){why="index-data";return;}
        recomp::gpu::generic::DrawInput in;
        in.indices.swap(s.indices);
        struct Return {recomp::gpu::generic::IndexVector& to;recomp::gpu::generic::IndexVector& from;~Return(){to.swap(from);}} give{s.indices,in.indices};
        if(recomp::gpu::generic::deferIndexConversion()){in.rawIndices=raw;in.rawIndexCount=request.indexCount;in.rawIndex32=index32;in.indices.clear();} // v236
        else {
            in.indices.resize(request.indexCount);
            recomp::gpu::hot::bigEndianIndices(raw,request.indexCount,index32,in.indices.data());
        }
        const auto [vertexConstants,pixelConstants]=genericDrawConstants(device,state,s);
        recomp::gpu::generic::SkinConstantsInfo skinInfo; // v80
        if(genericSkinConstants(skinInfo)){why="skin-constants";return;}
        in.skin=&skinInfo;
        const auto code=genericShaderCode(device,vs,ps);
        // (white-floor: a bound pixel shader needs its code, a draw without one needs none. The call stands in the test itself
        //  and in the test's own sense: a named local here opens a new clean-up scope and with it a new landing pad in this
        //  body, and a negated result swaps the branch's successors; either changes the body's PGO hash.)
        if(boundPixelCodeMissing(code.pixel,ps) || !(code.identity || code.effective)){why="shader-code";return;}
        if(!code.identity)genericNote("bound-draw-identity",code.effective?"original-code-unreadable":"no-vertex-code");
        if(inPlace)in.stateBytes=state;else in.state=&s.state;
        in.scissor=driverScissors[device];
        genericCodeVersions(in,code); // v78
        in.vertexConstants=vertexConstants;in.pixelConstants=pixelConstants;
        s.constantsInto(in); // v123 (v276: + sources and dirty masks)
        in.fetch=[&](unsigned slot) {
            const auto it=driverTextures.find(device);
            return it==driverTextures.end()?std::optional<std::array<std::uint32_t,6>>{}:it->second.fetch(slot);
        };
        in.vertexIdentity=code.identity?code.identity:code.effective;
        in.vertexEffective=code.effective?code.effective:code.identity;
        in.vertexAlternates=code.alternates;
        in.pixelCode=code.pixel;in.primitive=request.primitive;in.baseVertex=request.baseVertex;
        in.disposable=true; // v111: the indices are this call's (taken, not copied, when pipelined)
        recomp::gpu::generic::draw(in,genericDataReader(),&why);
    });
    genericNote("bound-indexed-draw",why);
    // v40 census: a draw skipped before it reached the renderer (the renderer logs its own refusals).
    // v253: the refusal-name comparison runs only in census frames (its only use)
    const bool hookSkip=recomp::gpu::generic::censusActive() && [&]{for(const char* w:{"gate","device-state","bound-objects","index-header","index-range","index-data","shader-code","skin-constants"})
        if(!std::strcmp(why,w))return true;return false;}();
    if(hookSkip) {
        char detail[192];std::snprintf(detail,sizeof detail,"why=%s prim=%u base_vertex=%u start=%u count=%u vs=%08x ps=%08x ib=%08x",why,
            request.primitive,request.baseVertex,request.startIndex,request.indexCount,vsObject,psObject,indexObject);
        recomp::gpu::generic::censusNote("bound-indexed-draw",detail);
    }
}
// v56: D3D DrawVertices (823CE5A0), called after the original has emitted its PM4 (the device shadow,
// the submitted constants and the texture fetches are the ones that draw used). Each chunk of the
// original's 65535-vertex split is a draw of sequential indices from its start vertex. The counts go
// to the per-frame RENDER_DIAG_DRAW_VERTICES line; files/private-trace/diag-generic-no-draw-vertices
// stops the routing (the counting stays).
bool drawVerticesRouting() {
    static unsigned checks=0,seen=0;
    if(switchEpochChanged(seen) && ++checks) { // (v330a)
        const bool off=access(RECOMP_APP_PATH("/private-trace/diag-generic-no-draw-vertices"),F_OK)==0;
        if(off==drawVerticesRoutingEnabled || checks==1)rendererLog("RENDER_DIAG_DRAW_VERTICES_MODE routing=%u\n",unsigned(!off));
        drawVerticesRoutingEnabled=!off;
    }
    return drawVerticesRoutingEnabled;
}
void genericDrawVertices(std::uint32_t device,std::uint32_t primitive,std::uint32_t start,std::uint32_t count) {
    // The routing below runs inside inlineGate, like the bound-indexed and inline hooks (shader cache,
    // driver texture/scissor tables, renderer); only the frame record is touched outside it.
    const auto caller=recomp::bootstrap::currentGuestThreadToken();
    unsigned sampledSoFar=0;
    {std::lock_guard lock(drawVerticesMutex);drawVerticesFrame.call(primitive,count,caller);sampledSoFar=drawVerticesFrame.sampled;}
    struct Tally {unsigned chunks=0,routed=0,refused=0,skipped=0;} tally;
    const char* why="gate";std::uint32_t vsObject=0,psObject=0;
    std::vector<std::uint32_t> textures;
    if(!genericReady() || !ownsNativeRendering(device) || !inlineGate){why="not-owned";++tally.skipped;}
    else {
        const bool routing=drawVerticesRouting();
        bool attempted=false;
        gateRef([&] {
            auto& s=genericDrawScratch();
            const bool inPlace=genericStateInPlace(); // v78: as genericBoundIndexed
            const std::uint8_t* state=inPlace?deviceRange(device,device,13860):nullptr; // (v335)
            if(!inPlace){if(!copyGuestHeapInto(device,13860,s.state)){why="device-state";return;}state=s.state.data();}
            if(!state){why="device-state";return;}
            const auto field=[&](unsigned at){return recomp::gpu::hot::be32(state+at);};
            const auto vs=field(13072),ps=field(13068);vsObject=vs;psObject=ps;
            if(!vs){why="bound-objects";return;}
            if(!routing){why="routing-disabled";return;}
            const auto code=genericShaderCode(device,vs,ps);
            if(!(code.identity || code.effective)){why="shader-code";return;}
            std::optional<std::array<std::uint32_t,2>> table;
            if(count>65535)if(const auto t=inlineRegistry->copySerializedTitleImage(0x82006610u+(primitive&0x3F)*8,8);t && t->size()==8)
                table=std::array<std::uint32_t,2>{genericBe32(*t,0),genericBe32(*t,4)};
            const auto chunks=recomp::gpu::planDrawVertices(start,count,table);
            if(count>65535 && !table)genericNote("draw-vertices-split","table-unreadable-one-draw");
            const auto [vertexConstants,pixelConstants]=genericDrawConstants(device,state,s);
            recomp::gpu::generic::SkinConstantsInfo skinInfo; // v80
            if(genericSkinConstants(skinInfo)){why="skin-constants";return;}
            attempted=true;
            for(std::size_t k=0;k<chunks.size();++k) {
                recomp::gpu::generic::DrawInput in;
                if(inPlace)in.stateBytes=state;else in.state=&s.state;
                in.scissor=driverScissors[device];
                in.vertexConstants=vertexConstants;in.pixelConstants=pixelConstants;
                s.constantsInto(in); // v123 (v276: + sources and dirty masks)
                in.fetch=[&](unsigned slot) {
                    const auto it=driverTextures.find(device);
                    return it==driverTextures.end()?std::optional<std::array<std::uint32_t,6>>{}:it->second.fetch(slot);
                };
                in.vertexIdentity=code.identity?code.identity:code.effective;
                in.vertexEffective=code.effective?code.effective:code.identity;
                in.vertexAlternates=code.alternates;
                in.pixelCode=code.pixel;in.primitive=primitive&0x3F;
                genericCodeVersions(in,code); // v78
                // v366f (exact; diag-v366-render-trims2): the indices are written into a reused buffer (resize, then every element
                // written: the same values); the capture exchanges it for the packet's previous one, which is kept for the next draw.
                // (v349 drive profile: the per-draw vector's allocation and free ~0.2% of the render thread.)
                const bool reuseIndices=recomp_v366_render_trims2.load(std::memory_order_relaxed);
                if(reuseIndices)in.indices.swap(s.v366Indices);
                recomp::gpu::drawVerticesIndices(chunks[k],in.indices,in.baseVertex);
                in.source="dv";in.skin=&skinInfo; // v80
                in.disposable=true; // v111
                in.pixelSamples=(k==0 && sampledSoFar<4)?&textures:nullptr;
                const char* w="?";
                ++tally.chunks;
                const bool drawn=recomp::gpu::generic::draw(in,genericDataReader(),&w);
                if(reuseIndices){in.indices.clear();s.v366Indices.swap(in.indices);} // (the packet's previous buffer, or this one if not taken)
                if(drawn)++tally.routed;else ++tally.refused;
                why=w;
            }
        });
        if(!attempted)++tally.skipped;
    }
    {
        std::lock_guard lock(drawVerticesMutex);
        auto& frame=drawVerticesFrame;
        frame.chunks+=tally.chunks;frame.routed+=tally.routed;frame.refused+=tally.refused;frame.skipped+=tally.skipped;
        if(std::strcmp(why,"generic-draw-recorded"))frame.lastWhy=why;
        frame.sample(primitive,count,vsObject,psObject,textures,why);
        genericNote("draw-vertices",why);
    }
}
#endif
bool finishNativeBoundIndexedChecked(PPCContext& ctx,std::uint8_t* base) {
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    // v25: draw generically, then continue into the original flow so tile recording and the raw PM4
    // emission stay exactly as in the pre-generic builds. v21/v23 skipped the emission and froze
    // ~160 s into gameplay on the known merged tile-pass/end-marker walker case (TAIL_EXPECTED_MISSING).
    if(genericReady() && ownsNativeRendering(ctx.r31.u32))
        genericBoundIndexed(ctx.r31.u32,recomp::gpu::Gta5BoundIndexedRequest{ctx.r18.u32,ctx.r17.u32,ctx.r21.u32,ctx.r19.u32});
#endif
    if(capturePreparedRecordedIndexed(ctx))return false;
    stopPendingTileRecording("prepared-indexed-draw");
#if defined(RECOMP_DIAGNOSTIC_FAST_FRAMES)
    // Attempt v7 frame-rate mode: original raw emission (the diagnostic ring retirement skips the draw),
    // as when native rendering is not owned. v5 profile: ~27% of the render thread.
    (void)base;return false;
#endif
    (void)base;
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_bound_geometry_shader_pack.h") && __has_include("native_prepared_indexed_original.h")
    const auto device=ctx.r31.u32;
    if(!ownsNativeRendering(device))return false;
    const auto reject=[&](const char* stage) {
        rendererLog("RENDER_API_BOUND_INDEXED_REJECT device=%08x stage=%s completed=0\n",device,stage);
        throw DiagnosticIndexedSkip{stage};
    };
    if(!inlineRegistry || !inlineGraphics || !inlineGate)
        reject("execution-owner");
    const recomp::gpu::Gta5BoundIndexedRequest request{ctx.r18.u32,ctx.r17.u32,ctx.r21.u32,ctx.r19.u32};
    bool completed=false;
    const bool gated=gateRef([&] {
        stopPendingTileRecording("prepared-indexed-draw");
        recomp::gpu::NativeEdram32Mutation physicalMutation(liveEdram32[device]);
        const auto heap=[&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);};
        const auto data=[&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);};
        const auto word=[&](std::uint32_t a)->std::optional<std::uint32_t> {
            const auto b=heap(a,4);if(!b || b->size()!=4)return std::nullopt;
            return (std::uint32_t{(*b)[0]}<<24)|(std::uint32_t{(*b)[1]}<<16)|(std::uint32_t{(*b)[2]}<<8)|(*b)[3];
        };
        const auto& state=inlineGraphics->stateBatchSnapshot();
        const recomp::gpu::DrawBinState bins{{static_cast<std::uint32_t>(state.binMask),static_cast<std::uint32_t>(state.binMask>>32),
            static_cast<std::uint32_t>(state.binSelect),static_cast<std::uint32_t>(state.binSelect>>32)},state.knownBinWords};
        auto geometry=recomp::gpu::captureGta5BoundIndexedDraw(device,request,bins,heap,data);
        if(!geometry)reject("geometry-visibility-owner");
        auto& draw=geometry->draw;
        flushInlineConstants(device);const auto constants=driverConstants.find(device); /* v80 flush */const auto scissor=driverScissors.find(device);
        if(!driverConstantsValid || constants==driverConstants.end() || scissor==driverScissors.end() || !scissor->second)
            reject("submitted-constants-scissor");
        constants->second.apply(draw);draw.scissorRegisters=scissor->second;
        const auto selection=selectedVertexShaders.find(device);
        const auto* selected=selection==selectedVertexShaders.end()?nullptr:selection->second.forObject(draw.vertexShader);
        const auto range=recomp::gpu::describeGta5ShaderCode(draw.pixelShader,1,0,word);
        const auto pixel=range && range->byteCount<=4096?data(range->virtualAddress,range->byteCount):std::nullopt;
#if __has_include("native_fixed16_draw_shader_pack.h")
        if(selected && pixel)for(const auto& p:nativeFixed16DrawPrograms)
            if(p.vertexIdentity==selected->code && recomp::gpu::sameXenosStraightLineShader(p.pixelIdentity,*pixel)) {
                const auto bindings=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
                if(!bindings)reject("fixed16-bound-surfaces");
                completed=drawFixed16Target(device,p,draw,geometry->effectiveIndices,*bindings,physicalMutation);
                if(!completed)reject("fixed16-execution");
                ++liveDraws;
                return; // Real fenced draw; common gate result is checked below.
            }
#endif
        #if __has_include("native_indexed_rgb7e3_shader_pack.h")
        if(selected && pixel && selected->code==nativeIndexedRgb7e3Program.draw.vertexIdentity &&
           *pixel==nativeIndexedRgb7e3Program.draw.pixelIdentity) {
            const auto bound=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
            if(!bound)reject("indexed-rgb7e3-bound-surfaces");
            const char* why="indexed-rgb7e3-not-executed";
            completed=inlineGraphics->executeIndexedRgb7e3(nativeIndexedRgb7e3Program,draw,
                geometry->effectiveIndices,*bound,selected->code,*pixel,physicalMutation,&why);
            if(!completed)reject(why);
            const bool clipped=std::strcmp(why,"current-indexed-view-exclusion")==0;
            // Still under the exclusive submission gate. Physical publication
            // is fenced; invalidate overlapping cached native views only for
            // real writes, without resurrecting a stale display/texture view.
            if(!clipped)invalidatePhysicalNativeViews(device,0,1440);
            ++liveDraws;
            rendererLog("RENDER_API_BOUND_INDEXED_RGB7E3 vertices=%u indices=%zu completed=1 "
                "clipped=%u physical_rgb7e3_depth_stencil=%u stage=%s query_result=0 ring_advance=0\n",
                draw.vertexCount,geometry->effectiveIndices.size(),unsigned(clipped),unsigned(!clipped),why);
            return;
        }
        #endif
        #if __has_include("native_indexed_zero_depth_shader_pack.h")
        if(selected && pixel && selected->code==nativeIndexedZeroDepthProgram.draw.vertexIdentity &&
           *pixel==nativeIndexedZeroDepthProgram.draw.pixelIdentity) {
            const auto bound=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
            if(!bound)reject("indexed-zero-depth-bound-surfaces");
            const auto submitted=driverTextures.find(device);
            if(submitted==driverTextures.end())reject("indexed-zero-depth-submitted-textures");
            const auto textures=recomp::gpu::captureNativeIndexedZeroDepthTextures(
                [&](unsigned slot){return submitted->second.fetch(slot);},data);
            if(!textures)reject("indexed-zero-depth-texture-planes-owner");
            std::vector<recomp::gpu::NativeMipTextureUse> mipUses;
            std::vector<std::pair<std::uint32_t,LiveTarget::TextureUse>> baseUses;
            for(const auto& t:*textures) {
                if(!t.mips.empty()) {
                    const auto use=mipTextureUse(t.fetch);
                    if(!use)reject("indexed-zero-depth-mip-allocation-owner");mipUses.push_back(*use);
                } else {
                    const auto desc=recomp::gpu::XenosTextureDescription::decode(t.fetch);
                    const auto address=desc?textureStorageAddress(*desc,t.base.size()):std::nullopt;
                    if(!address)reject("indexed-zero-depth-base-allocation-owner");
                    baseUses.emplace_back(*address,LiveTarget::TextureUse{*desc,static_cast<std::uint32_t>(t.base.size())});
                }
            }
            const char* why="indexed-zero-depth-not-executed";
            completed=inlineGraphics->executeIndexedZeroDepth(nativeIndexedZeroDepthProgram,draw,
                geometry->effectiveIndices,*bound,*textures,selected->code,*pixel,physicalMutation,&why);
            if(!completed)reject(why);
            // Fenced physical colour publication precedes cached-view invalidation.
            // Depth/stencil are unchanged. No guest ring/query completion is invented.
            invalidatePhysicalNativeViews(device,720,720);
            auto& target=liveTargets[device];
            for(const auto& use:mipUses)target.completedMipTextures[use.baseAddress]=use;
            for(const auto& use:baseUses)target.completedTextures[use.first]=use.second;
            ++liveDraws;
            rendererLog("RENDER_API_BOUND_INDEXED_ZERO_DEPTH vertices=%u indices=%zu textures=4 completed=1 "
                "physical_rgb7e3=1 depth_stencil_preserved=1 stage=%s query_result=0 ring_advance=0\n",
                draw.vertexCount,geometry->effectiveIndices.size(),why);
            return;
        }
        #endif
        #if __has_include("native_indexed_blend_depth_shader_pack.h")
        if(selected && pixel && selected->code==nativeIndexedBlendDepthProgram.draw.vertexIdentity &&
           *pixel==nativeIndexedBlendDepthProgram.draw.pixelIdentity) {
            const auto bound=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
            if(!bound)reject("indexed-blend-depth-bound-surfaces");
            const auto submitted=driverTextures.find(device);
            if(submitted==driverTextures.end())reject("indexed-blend-depth-submitted-textures");
            const auto textures=recomp::gpu::captureNativeIndexedBlendDepthTextures(
                [&](unsigned slot){return submitted->second.fetch(slot);},data);
            if(!textures)reject("indexed-blend-depth-texture-planes-owner");
            std::vector<std::pair<std::uint32_t,LiveTarget::TextureUse>> baseUses;
            for(const auto& t:*textures) {
                if(!t.mips.empty())reject("indexed-blend-depth-unqualified-mips");
                const auto desc=recomp::gpu::XenosTextureDescription::decode(t.fetch);
                const auto address=desc?textureStorageAddress(*desc,t.base.size()):std::nullopt;
                if(!address)reject("indexed-blend-depth-base-allocation-owner");
                baseUses.emplace_back(*address,LiveTarget::TextureUse{*desc,static_cast<std::uint32_t>(t.base.size())});
            }
            const char* why="indexed-blend-depth-not-executed";
            completed=inlineGraphics->executeIndexedBlendDepth(nativeIndexedBlendDepthProgram,draw,
                geometry->effectiveIndices,*bound,*textures,selected->code,*pixel,physicalMutation,&why);
            if(!completed)reject(why);
            invalidatePhysicalNativeViews(device,720,720);
            auto& target=liveTargets[device];
            for(const auto& use:baseUses)target.completedTextures[use.first]=use.second;
            ++liveDraws;
            rendererLog("RENDER_API_BOUND_INDEXED_BLEND_DEPTH vertices=%u indices=%zu textures=3 completed=1 "
                "physical_rgb7e3=1 single_coverage=1 depth_stencil_preserved=1 stage=%s query_result=0 ring_advance=0\n",
                draw.vertexCount,geometry->effectiveIndices.size(),why);
            return;
        }
        #endif
        #if __has_include("native_clipped_indexed_shader_pack.h")
        if(selected && pixel && selected->code==indexedClipProgram.vertexIdentity &&
           *pixel==indexedClipProgram.pixelIdentity) {
            const auto bound=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
            if(!bound)reject("clipped-indexed-bound-surfaces");
            const char* why="clipped-indexed-not-executed";
            const auto proof=inlineGraphics->executeClippedIndexed(indexedClipProgram,draw,
                geometry->effectiveIndices,*bound,selected->code,*pixel,physicalMutation,&why);
            if(!proof)reject(why);
            completed=true;++liveDraws;
            rendererLog("RENDER_API_BOUND_INDEXED_CLIPPED vertices=%u indices=%zu triangles=%u behind=%u outside_xy=%u "
                "completed=1 physical_preserved=1 target_writes=0 query_result=0 ring_advance=0\n",
                draw.vertexCount,geometry->effectiveIndices.size(),proof->triangles,proof->behind,proof->outsideXY);
            return;
        }
        #endif
        #if __has_include("native_indexed_lod_shader_pack.h")
        const recomp::gpu::NativeIndexedLodProgram* lodProgram=nullptr;
        if(selected && pixel && selected->code==nativeIndexedLodProgram.positions.vertexIdentity &&
           *pixel==nativeIndexedLodProgram.positions.pixelIdentity)lodProgram=&nativeIndexedLodProgram;
        #if defined(RECOMP_HAVE_INDEXED_LOD_601)
        // Reuse the identical qualified ABI33 modules. This immutable copy is
        // constructed once, not per draw; no generated shader/PPC changes.
        static const auto nativeIndexedLodWindowedProgram=[] {
            auto program=nativeIndexedLod601Program;
            program.raster=recomp::gpu::NativeIndexedLodRaster::ClippedWindows;
            return program;
        }();
        if(!lodProgram && selected && pixel && selected->code==nativeIndexedLod601Program.positions.vertexIdentity &&
           *pixel==nativeIndexedLod601Program.positions.pixelIdentity)lodProgram=&nativeIndexedLodWindowedProgram;
        #endif
        if(lodProgram) {
            const auto bound=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
            if(!bound)reject("indexed-lod-bound-surfaces");
            const auto submitted=driverTextures.find(device);
            if(submitted==driverTextures.end())reject("indexed-lod-submitted-textures");
            const auto textures=recomp::gpu::captureNativeIndexedLodTextures(
                [&](unsigned slot){return submitted->second.fetch(slot);},data,lodProgram->profile);
            if(!textures)reject("indexed-lod-texture-planes-owner");
            std::vector<recomp::gpu::NativeMipTextureUse> mipUses;
            std::vector<std::pair<std::uint32_t,LiveTarget::TextureUse>> baseUses;
            for(const auto& t:*textures) {
                if(!t.mips.empty()) {
                    const auto use=mipTextureUse(t.fetch);
                    if(!use)reject("indexed-lod-mip-allocation-owner");mipUses.push_back(*use);
                } else {
                    const auto desc=recomp::gpu::XenosTextureDescription::decode(t.fetch);
                    const auto address=desc?textureStorageAddress(*desc,t.base.size()):std::nullopt;
                    if(!address)reject("indexed-lod-base-allocation-owner");
                    baseUses.emplace_back(*address,LiveTarget::TextureUse{*desc,static_cast<std::uint32_t>(t.base.size())});
                }
            }
            const char* why="indexed-lod-not-executed";
            completed=inlineGraphics->executeIndexedLod(*lodProgram,draw,
                geometry->effectiveIndices,*bound,*textures,selected->code,*pixel,physicalMutation,&why);
            if(!completed)reject(why);
            // This path writes BOTH depth and color. Invalidate every cached
            // alias intersecting those physical ranges, only after commit.
            invalidatePhysicalNativeViews(device,0,1440);
            auto& target=liveTargets[device];
            for(const auto& use:mipUses)target.completedMipTextures[use.baseAddress]=use;
            for(const auto& use:baseUses)target.completedTextures[use.first]=use.second;
            ++liveDraws;
            rendererLog("RENDER_API_BOUND_INDEXED_LOD vertices=%u indices=%zu textures=5 completed=1 "
                "physical_rgb7e3=1 depth_written=1 stage=%s query_result=0 ring_advance=0 profile=%u windowed=%u\n",
                draw.vertexCount,geometry->effectiveIndices.size(),why,
                lodProgram->profile==recomp::gpu::NativeIndexedLodProfile::Indexed601?601u:600u,
                unsigned(lodProgram->raster==recomp::gpu::NativeIndexedLodRaster::ClippedWindows));
            return;
        }
        #endif
        const recomp::gpu::NativeTranslatedDrawProgram* program=nullptr;
        if(selected && pixel)for(const auto& p:nativeBoundGeometryPrograms)
            if(p.vertexIdentity==selected->code && recomp::gpu::sameXenosStraightLineShader(p.pixelIdentity,*pixel) && recomp::gpu::planNativeGeometryDrawState(draw,p)) {
                program=&p;break;
            }
        if(!program)reject("effective-shader-state-pack");
        const auto bindings=recomp::gpu::captureGta5BoundSurfaces(draw.deviceState,heap,data,40);
        const auto object=bindings?bindings->surfaces[0].object:0;const auto key=std::make_pair(device,object);
        const auto found=clearedColor8Targets.find(key);
        const auto liveFound=liveTargets.find(device);
        if(!bindings || found==clearedColor8Targets.end() || liveFound==liveTargets.end())
            reject("color8-target-owner");
        auto& target=found->second;auto& live=liveFound->second;
        if(!live.valid || !target.valid || !target.image || !bindings->surfaces[0].header || *bindings->surfaces[0].header!=target.header ||
           object==live.object || recomp::gpu::color8ClearIntersects(target.image->plan(),720,720) ||
           !recomp::gpu::nativeGammaZeroDepthTargetBindings(draw,target.image->plan(),*bindings))
            reject("color8-headers-display-alias");
        std::vector<std::pair<unsigned,recomp::gpu::NativeMipTextureCapture>> textures;
        std::vector<recomp::gpu::NativeMipTextureUse> mipUses;
        std::vector<std::pair<std::uint32_t,LiveTarget::TextureUse>> baseUses;
        const auto submitted=driverTextures.find(device);std::size_t budget=64u*1024*1024;
        if(submitted==driverTextures.end())reject("submitted-textures");
        for(auto slot:recomp::gpu::nativeTranslatedTextureSlots(*program)) {
            const auto fetch=submitted->second.fetch(slot);
            auto texture=fetch?recomp::gpu::captureNativeMipTextureStorage(*fetch,data,budget):std::nullopt;
            if(!texture)reject("texture-planes-owner");
            budget-=texture->base.size()+texture->mips.size();
            if(!texture->mips.empty()) {
                const auto use=mipTextureUse(texture->fetch);if(!use)reject("mip-allocation-owner");
                mipUses.push_back(*use);
            } else {
                const auto desc=recomp::gpu::XenosTextureDescription::decode(texture->fetch);
                const auto address=desc?textureStorageAddress(*desc,texture->base.size()):std::nullopt;
                if(!address)reject("base-allocation-owner");
                baseUses.emplace_back(*address,LiveTarget::TextureUse{*desc,static_cast<std::uint32_t>(texture->base.size())});
            }
            textures.emplace_back(slot,std::move(*texture));
        }
        // A failed write invalidates this frame; a later offscreen draw may
        // never resurrect an already-invalid presentation owner.
        const auto& plan=target.image->plan();target.valid=false;live.valid=false;
        for(auto& [otherKey,other]:clearedColor8Targets)if(other.image && otherKey!=key &&
            recomp::gpu::color8ClearPlansOverlap(other.image->plan(),plan))other.valid=false;
        for(auto& [otherKey,other]:offscreenTargets)if(other.image &&
            recomp::gpu::color8ClearIntersects(plan,other.image->plan().baseTile,other.image->plan().tileCount))other.valid=false;
        invalidateFixed16Overlaps(plan);
        completed=inlineGraphics->executeBoundGeometry(*target.image,*program,draw,geometry->effectiveIndices,*bindings,textures);
        if(!completed)reject("color8-gpu-execution");
        target.valid=true;++target.contentSerial;live.valid=true;++liveDraws;
        for(const auto& use:mipUses)live.completedMipTextures[use.baseAddress]=use;
        for(const auto& use:baseUses)live.completedTextures[use.first]=use.second;
        rendererLog("RENDER_API_BOUND_GEOMETRY object=%08x vertices=%u indices=%zu completed=1 serial=%llu ring_advance=0\n",
            object,draw.vertexCount,geometry->effectiveIndices.size(),static_cast<unsigned long long>(target.contentSerial));
    });
    if(!gated || !completed)reject("gate-completion");
    return true;
#else
    (void)ctx;return false;
#endif
}
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_bound_geometry_shader_pack.h") && __has_include("native_prepared_indexed_original.h")
#include "native_prepared_indexed_original.h"
#endif

bool finishNativeInlineBegin(PPCContext& ctx,std::uint8_t* base) {
#if (defined(__ANDROID__) || defined(__APPLE__))
    const auto device=ctx.r31.u32;
    if(!ownsNativeRendering(device))return false;
    bool completed=false,fallback=false; // v156: fallback = run the original BeginVertices
    const bool gated=gateRef([&] {
        auto& pendingSlot=pendingInlineRef();const auto& p=pendingSlot; // v362
        const auto bytes=std::uint64_t{p.count}*p.stride;
        const char* refused=p.device!=device?"device":p.nativeAllocation?"pending-native":p.primitive!=ctx.r23.u32?"primitive":
            p.count!=ctx.r24.u32?"count":(!bytes || bytes>NativeInlineBytes || (bytes&3))?"bytes":
            std::uint64_t{ctx.r22.u32}*4!=bytes?"words":!deviceRange(device,device,13860)?"device-state":nullptr; // (v335) // v253: the same validation, no 13.8 KB copy
        if(refused) { // v156: as the indexed Begin, the original BeginVertices runs unless a native draw is pending
            static std::atomic<unsigned> logged{0};
            const auto n=logged.fetch_add(1,std::memory_order_relaxed);
            if(n<32 || !(n&1023))
                rendererLog("RENDER_API_BEGIN_REFUSED reason=%s n=%u device=%08x/%08x pending=%u primitive=%u/%u count=%u/%u stride=%u words=%u lr=%08x\n",
                    refused,n+1,p.device,device,unsigned(p.nativeAllocation),p.primitive,ctx.r23.u32,p.count,ctx.r24.u32,p.stride,ctx.r22.u32,
                    std::uint32_t(ctx.lr));
            if(p.nativeAllocation && p.device==device)throw std::runtime_error("Native BeginVertices arguments/state are unsupported");
            fallback=true;return;
        }
        auto& buffer=nativeInlineBuffers[device];
        if(buffer.busy)throw std::runtime_error("Native BeginVertices overlaps an unfinished draw");
        if(!buffer.address) {
            recomp::bootstrap::GuestImportCall allocation;
            allocation.arguments={0,NativeInlineBytes,4,0,0,0,0,0};
            const auto result=inlineRegistry->dispatch("__imp__MmAllocatePhysicalMemory",allocation);
            if(!result.handled() || !allocation.returnValue || allocation.returnValue>UINT32_MAX)
                throw std::runtime_error("Native inline vertex allocation failed");
            buffer.address=static_cast<std::uint32_t>(allocation.returnValue);
            rendererLog("RENDER_API_INLINE_BUFFER device=%08x address=%08x bytes=%u allocated=1\n",device,buffer.address,NativeInlineBytes);
        }
        // v362 (exact, diag-v362-render-trims): the ownership test without the copy (the same validation; v349 copied up to the
        // whole inline buffer per inline draw and discarded it).
        if(!(recomp::v362::trimsOn()?inlineRegistry->viewSerializedGraphicsData(buffer.address,static_cast<std::uint32_t>(bytes))!=nullptr:
             bool(inlineRegistry->copySerializedGraphicsData(buffer.address,static_cast<std::uint32_t>(bytes)))))
            throw std::runtime_error("Native inline vertex allocation lost ownership");
        // Preserve the original CPU-visible BeginVertices metadata. No old
        // commands were emitted, so the saved command cursor is unchanged.
        PPC_STORE_U8(device+12904,ctx.r25.u8);
        PPC_STORE_U64(device+24,PPC_LOAD_U64(device+24)|1);
        PPC_STORE_U32(device+13844,PPC_LOAD_U32(device+48));
        PPC_STORE_U32(device+13848,buffer.address);
        PPC_STORE_U32(device+13856,ctx.r22.u32);
        buffer.busy=true;pendingSlot.nativeAllocation=true;
        ctx.r3.u64=buffer.address;completed=true;
    });
    if(gated && fallback)return false; // v156: RENDER_API_BEGIN_REFUSED (rate-limited) names the reason
    if(!gated || !completed)throw std::runtime_error("Native BeginVertices did not enter its exclusive gate");
    return true;
#else
    (void)ctx;(void)base;return false;
#endif
}

// Same ordered state preparation as the original indexed Begin. Retain both
// output pointers and CPU metadata; EndVertices consumes the actual filled
// buffers and requires a completed native submission before either is reused.
bool finishNativeInlineIndexedBegin(PPCContext& ctx,std::uint8_t* base) {
#if (defined(__ANDROID__) || defined(__APPLE__))
    const auto device=ctx.r31.u32;
    if(!ownsNativeRendering(device))return false;
    bool fallback=false; // v26: outputs outside the caller's argument area -> original Begin
    const bool gated=gateRef([&] {
        auto& pendingSlot=pendingInlineRef();const auto& p=pendingSlot; // v362
        const auto vertexBytes=std::uint64_t{p.count}*p.stride;
        const auto indexBytes=std::uint64_t{p.indexCount}*((p.indexFormat&4)?4:2);
        // v156: which check refuses (in order); every refusal except a native draw still pending on this device runs
        // the original Begin (the v26 fallback, ~300k calls per play run). v153 run y1vpd8vx stopped in the cutscene
        // after the prologue getaway on one of these checks (not logged then).
        const char* refused=p.device!=device?"device":p.nativeAllocation?"pending-native":p.primitive!=ctx.r21.u32?"primitive":
            p.minVertex!=ctx.r20.u32?"min-vertex":p.indexCount!=ctx.r23.u32?"index-count":p.indexFormat!=ctx.r24.u32?"index-format":
            (!vertexBytes || vertexBytes>NativeInlineBytes || (vertexBytes&3))?"vertex-bytes":
            (!indexBytes || indexBytes>NativeInlineBytes)?"index-bytes":ctx.r19.u32!=vertexBytes/4?"vertex-words":
            ctx.r18.u32!=(indexBytes+3)/4?"index-words":!inlineRegistry->copySerializedGuestHeap(device,13864)?"device-state":
            !inlineRegistry->copySerializedGuestArgument(ctx.r1.u32+308,4)?"output-argument":nullptr;
        if(refused) {
            static std::atomic<unsigned> logged{0};
            const auto n=logged.fetch_add(1,std::memory_order_relaxed);
            if(n<32 || !(n&1023))
                rendererLog("RENDER_API_INDEXED_BEGIN_REFUSED reason=%s n=%u device=%08x/%08x pending=%u primitive=%u/%u count=%u stride=%u min=%u/%u "
                    "indices=%u/%u format=%u/%u words=%u/%u lr=%08x\n",refused,n+1,p.device,device,unsigned(p.nativeAllocation),p.primitive,ctx.r21.u32,
                    p.count,p.stride,p.minVertex,ctx.r20.u32,p.indexCount,ctx.r23.u32,p.indexFormat,ctx.r24.u32,ctx.r19.u32,ctx.r18.u32,
                    std::uint32_t(ctx.lr));
            if(p.nativeAllocation && p.device==device)throw std::runtime_error("Native indexed Begin arguments/state are unsupported");
            fallback=true;return;
        }
        const auto vertexOutput=PPC_LOAD_U32(ctx.r1.u32+308),indexOutput=ctx.r17.u32;
        // v26 (diagnostic): v25 reached a gameplay draw whose output pointers are not in the caller's
        // argument area. Accept owned guest heap words; anything else keeps the original Begin.
        const auto owned=[&](std::uint32_t a){return inlineRegistry->copySerializedGuestArgument(a,4) ||
            inlineRegistry->copySerializedGuestHeap(a,4);};
        if(!owned(vertexOutput) || !owned(indexOutput)){fallback=true;return;}
        auto& buffer=nativeInlineBuffers[device];
        if(buffer.busy)throw std::runtime_error("Native indexed Begin overlaps an unfinished draw");
        const auto allocate=[&](std::uint32_t& address) {
            if(address)return;
            recomp::bootstrap::GuestImportCall call;call.arguments={0,NativeInlineBytes,4,0,0,0,0,0};
            const auto result=inlineRegistry->dispatch("__imp__MmAllocatePhysicalMemory",call);
            if(!result.handled() || !call.returnValue || call.returnValue>UINT32_MAX)
                throw std::runtime_error("Native indexed buffer allocation failed");
            address=static_cast<std::uint32_t>(call.returnValue);
            rendererLog("RENDER_API_INLINE_BUFFER device=%08x address=%08x bytes=%u allocated=1\n",device,address,NativeInlineBytes);
        };
        allocate(buffer.address);allocate(buffer.indexAddress);
        const auto ownedRange=[&](std::uint32_t a,std::uint32_t n){ // v362 (exact): the ownership test without the copy
            return recomp::v362::trimsOn()?inlineRegistry->viewSerializedGraphicsData(a,n)!=nullptr:bool(inlineRegistry->copySerializedGraphicsData(a,n));};
        if(!ownedRange(buffer.address,static_cast<std::uint32_t>(vertexBytes)) ||
           !ownedRange(buffer.indexAddress,static_cast<std::uint32_t>((indexBytes+3)&~3ULL)))
            throw std::runtime_error("Native indexed buffers lost ownership");
        PPC_STORE_U8(device+12904,ctx.r26.u8);
        PPC_STORE_U64(device+24,PPC_LOAD_U64(device+24)|1);
        PPC_STORE_U32(device+13844,PPC_LOAD_U32(device+48));
        PPC_STORE_U32(device+13848,buffer.address);
        PPC_STORE_U32(device+13852,buffer.indexAddress);
        PPC_STORE_U32(device+13856,ctx.r19.u32);
        PPC_STORE_U32(device+13860,ctx.r18.u32);
        PPC_STORE_U32(vertexOutput,buffer.address);
        PPC_STORE_U32(indexOutput,buffer.indexAddress);
        buffer.busy=true;pendingSlot.nativeAllocation=true;ctx.r3.u64=0;
    });
    if(!gated)throw std::runtime_error("Native indexed Begin did not enter its exclusive gate");
    if(fallback){ // v185: the first line, then every power-of-two count (v184 and earlier: every call, ~60 lines/s)
        static std::atomic<unsigned long long> fallbacks{0};const auto n=fallbacks.fetch_add(1,std::memory_order_relaxed)+1;
        if(!(n&(n-1)))rendererLog("RENDER_API_INDEXED_BEGIN_FALLBACK outputs-not-owned original-begin=1 count=%llu\n",n);
        return false;}
    return true;
#else
    (void)ctx;(void)base;return false;
#endif
}

void bindRendererInlineCapture(recomp::bootstrap::GuestImportRegistry* registry,
        const recomp::gpu::NativeColorChainProgram* programs,
        std::function<bool(const std::function<void()>&)> gate,recomp::gpu::VulkanEngine* graphics,
        std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)> present,
        std::function<bool(const std::function<void()>&)> recordingGate,
        std::function<bool(const std::function<void()>&)> endRecordingGate,
        std::function<bool(unsigned,const recomp::gpu::Gta5TileCommandInterrupt&)> sceneInterrupt,
        std::function<bool(unsigned,const recomp::gpu::Gta5TileSceneContinuation&)> sceneContinuation,
        RendererSceneScheduler sceneScheduler) {
    inlineRegistry=registry;inlinePrograms=programs;inlineGate=std::move(gate);
    originalRecordingGate=std::move(recordingGate);
    originalEndRecordingGate=std::move(endRecordingGate);
    nativeSceneInterrupt=std::move(sceneInterrupt);stoppedInitialTileQueue.reset();stoppedInitialRingRetired=false;
    nativeSceneContinuation=std::move(sceneContinuation);nativeSceneScheduler=std::move(sceneScheduler);activeSceneQueue=nullptr;
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
    liveNativeScene.reset();
#endif
    activeEndQueue=nullptr;stoppedEndQueue.reset();stoppedEndCpuState.reset();stoppedEndCpuReturned=false;endRestoredScissor.reset();
    selectedVertexShaders.clear();++vertexSelectionSerial;driverConstants.clear();driverTextures.clear();driverScissors.clear();driverConstantsValid=true; // (v366d)
    for(auto& capture:shaderPatchHistory)capture.reset();
    shaderPatchSequences={};shaderPatchAttempts=0;shaderPatchCaptureFailures=0;
    boundIndexedSnapshots.clear();boundIndexedBudget={};
    boundIndexedAttempts=0;boundIndexedFailures=0;boundIndexedDropped=0;
    nativeUiDevice.store(0);textureBindingDiagnostics.store(0);
    nativeInlineBuffers.clear();
    inlineDrawCount=0;for(auto& draw:inlineDraws)draw.reset();firstUnsupportedInline.reset();firstUnsupportedResolve.reset();firstUnsupportedClear.reset();
    latestQueryCommandTrace.reset();
    retainedQueryEndTrace.reset();
    firstNativeResolveFailure.reset();nativeResolveFailureCaptureFailures=0;
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
    tileRecordingDevice.store(0);tileRecordingStop.store(nullptr);tileRecordingDiagnostic={};stoppedTileResolve.reset();stoppedTileEnd.reset();
    recordedTileResolves.clear();originalResolveRecordingDevice=0;
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
    pendingNativeSceneInputs.reset();
    pendingNativeSceneResolves.reset();pendingNativeSceneStorage.reset();
    pendingNativeSceneCommands.reset();
#endif
#endif
    firstUnsupportedQueue.reset();firstUnsupportedScissor.reset();
#if (defined(__ANDROID__) || defined(__APPLE__))
    // ABI22/23/24 targets hold Vulkan images too. They must not survive a
    // renderer generation or retain a reference to the previous GPU owner.
    rectangleMrtTargets.clear();offsetSampleTargets.clear();liveEdram32.clear();
    completedNativeSceneTarget.reset();pendingNativeSceneTarget.reset();inlineGraphics=graphics;liveTargets.clear();offscreenTargets.clear();clearedColor8Targets.clear();fixed16Targets.clear();liveResolvedTextures.clear();latestLiveHalf.reset();
    stoppedFixed16Targets.clear();stoppedFixed16CaptureFailed=false;
#if __has_include("native_logical_geometry_shader_pack.h") && defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(inlineGraphics && !inlineGraphics->requestNativeLogicalGeometry())
        throw std::runtime_error("Native logical scene features require compatible device creation");
#elif __has_include("native_bound_geometry_shader_pack.h") && __has_include("native_prepared_indexed_original.h")
    if(inlineGraphics && !inlineGraphics->requestNativeGeometry())
        throw std::runtime_error("Native geometry features were requested after incompatible device creation");
#endif
    liveClears=0;liveDraws=0;liveResolves=0;livePresents=0;latestLiveDevice=0;
    completedTranslatedTextures={};
    inlinePresent=std::move(present);
#else
    (void)graphics;(void)present;
#endif
    clearPresentationOwners();asyncPresent={};asyncPresentPair={};lockGateAdmits={}; // v43: liveTargets was cleared above (v158: + pair)
}
// Attempt v43 (declared in trace_runner.cpp, which binds it right after bindRendererInlineCapture):
// the optional asynchronous display for generic presents and the lock gate's admission check. Both are
// cleared by bindRendererInlineCapture and unbindRendererInlineCapture.
void bindRendererDiagnosticV43(std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)> present,
        std::function<int()> admits) {
    asyncPresent=std::move(present);lockGateAdmits=std::move(admits);
}
void bindRendererDiagnosticV158Pair(std::function<bool(const std::vector<std::uint8_t>&,const std::vector<std::uint8_t>&,unsigned,unsigned,
        std::chrono::microseconds)> pair) {
    asyncPresentPair=std::move(pair);
}
void unbindRendererInlineCapture() {
    // Keep the stopped diagnostic's owned copies for saveStoppedRendererDraw,
    // which runs after this function and worker retirement. Only bind resets it.
#if (defined(__ANDROID__) || defined(__APPLE__))
    const auto found=liveTargets.find(latestLiveDevice);
    if(found!=liveTargets.end() && found->second.valid)latestLiveHalf=found->second.image->readHalf();
    preserveStoppedFixed16Targets();
    rectangleMrtTargets.clear();offsetSampleTargets.clear();liveEdram32.clear();
    completedNativeSceneTarget.reset();pendingNativeSceneTarget.reset();liveTargets.clear();offscreenTargets.clear();clearedColor8Targets.clear();fixed16Targets.clear();inlineGraphics=nullptr;inlinePresent={};
#endif
    clearPresentationOwners();asyncPresent={};asyncPresentPair={};lockGateAdmits={}; // v43: before the presenter is joined (v158: + pair)
    // All writers have joined and every native GPU call returned its fence.
    // Buffers are private renderer allocations, not game resources to retain.
    if(inlineRegistry)for(const auto& [device,buffer]:nativeInlineBuffers)
      for(const auto address:{buffer.address,buffer.indexAddress})if(address) {
        recomp::bootstrap::GuestImportCall release;release.arguments[1]=address;
        const auto result=inlineRegistry->dispatch("__imp__MmFreePhysicalMemory",release);
        rendererLog("RENDER_API_INLINE_BUFFER device=%08x address=%08x freed=%u\n",device,address,unsigned(result.handled()));
    }
    nativeInlineBuffers.clear();
    inlineGate={};originalRecordingGate={};originalEndRecordingGate={};nativeSceneInterrupt={};activeEndQueue=nullptr;inlinePrograms=nullptr;inlineRegistry=nullptr;
    nativeSceneContinuation={};nativeSceneScheduler={};activeSceneQueue=nullptr;
    latestQueryCommandTrace.reset();
}
std::optional<bool> captureRendererTileSubmission(std::uint32_t writeWord) {
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__))
    auto* queue=activeEndQueue?activeEndQueue:activeSceneQueue;
    if(!queue)return std::nullopt;
    if(!inlineRegistry)return false;
    // activeEndQueue is installed only INSIDE originalEndRecordingGate; no
    // second gate, yield, read-pointer write or submission fallback is allowed.
    const bool accepted=queue->capture(writeWord,
        [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
        [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
    rendererLog("RENDER_END_CPU_ENQUEUE accepted=%u write=%u read=%u submissions=%zu gpu_completed=0 guest_published=0\n",
        unsigned(accepted),writeWord,queue->entryReadWord(),queue->submissions().size());
    return accepted;
#else
    (void)writeWord;return std::nullopt;
#endif
}
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
#include "renderer_runtime_maintenance.inc"
#else
bool submitRendererRuntimeMaintenance(std::uint32_t writeWord) {(void)writeWord;return false;}
#endif
void compareRendererInlineInputs(const recomp::gpu::NativeColorChainCapture& commandCapture,
        recomp::gpu::VulkanEngine* graphics,const recomp::gpu::NativeColorChainProgram* program) {
    recomp::gpu::PackedNativeColorChainInputs commandInputs;
    if(inlineDrawCount<2 || inlineDrawCount>CaptureLimit || !commandCapture.pack(commandInputs))return;
    bool matched=true;
    for(unsigned pair=0;pair<2;++pair) {
        const auto index=inlineDrawCount-2+pair;
        const auto& draw=inlineDraws[index];
        const auto native=draw?recomp::gpu::packGta5StartupDraw(*draw,pair):std::nullopt;
        rendererLog("RENDER_INLINE_ABI call=%u pair=%u bytes=%zu status=%s comparison_only=1\n",
            index+1,pair,native?native->size():0,
            native && *native==commandInputs.pairs[pair]?"matches-command-inputs":"mismatch-or-unavailable");
        const auto& expected=commandInputs.pairs[pair];
        const bool compatible=draw && recomp::gpu::matchesNativeColorRenderRegisters(
            [&](unsigned reg){return recomp::gpu::gta5DrawRegister(*draw,reg);},pair);
        if(!native || *native!=expected || !compatible)matched=false;
        if(native && native->size()==expected.size() && *native!=expected) {
            const std::array<std::size_t,6> boundaries=pair?
                std::array<std::size_t,6>{0,4096,4112,4144,4288,4352}:
                std::array<std::size_t,6>{0,1382400,1382416,1382496,1382640,1382720};
            constexpr const char* names[]{"textures","unused","pixel-constants","vertices","vertex-constants"};
            for(unsigned region=0;region<5;++region) {
                std::size_t count=0,first=boundaries[region+1];
                for(auto n=boundaries[region];n<boundaries[region+1];++n)if((*native)[n]!=expected[n]) {
                    ++count;first=std::min(first,n);
                }
                if(!count)continue;
                const auto at=first&~std::size_t{3};
                const auto word=[&](const auto& bytes) {
                    return (std::uint32_t{bytes[at]}<<24)|(std::uint32_t{bytes[at+1]}<<16)|
                        (std::uint32_t{bytes[at+2]}<<8)|bytes[at+3];
                };
                rendererLog("RENDER_INLINE_DIFF pair=%u region=%s different_bytes=%zu first=%zu "
                    "word_offset=%zu api_word=%08x command_word=%08x\n",
                    pair,names[region],count,first,at,word(*native),word(expected));
            }
        }
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
    if(matched && graphics && program && program->matches(commandCapture.active())) {
        const auto& half=latestLiveHalf;
        bool same=half && half->size()==1280*720*8;
        for(unsigned y=0;same && y<720;++y)for(unsigned x=0;same && x<1280;++x) {
            const auto* pixel=half->data()+(y*1280+x)*8;
            std::array<std::uint32_t,4> rgba{};
            for(unsigned c=0;c<4;++c)same=recomp::gpu::nativeColorChainHalfBits(
                static_cast<std::uint16_t>(pixel[c*2]|(unsigned(pixel[c*2+1])<<8)),rgba[c]) && same;
            std::uint32_t packed=0,offset=0,expected=0;
            same=same && recomp::gpu::packColor7e3(rgba,packed) &&
                recomp::gpu::aliasSampleOffset({0x14000500,0xC02D0,1280,720,false},x,y,0,offset) &&
                graphics->retainedColorEdramWord(offset,expected) && packed==expected;
        }
        rendererLog("RENDER_API_TARGET live_clears=%u live_draws=%u "
            "status=%s guest_texture_publishes=%u native_presents=%u native_present_return_enabled=1\n",liveClears,liveDraws,
            same?"packed-edram-match":"failed-or-different",liveResolves,livePresents);
    }
#else
    (void)graphics;(void)program;(void)matched;
#endif
}

void dumpStoppedRendererTextureDescriptions(
        const recomp::bootstrap::GuestImportRegistry& registry,
        recomp::gpu::VulkanEngine* graphics,
        const recomp::gpu::NativeColorChainProgram* programs) {
    (void)graphics;
#if (defined(__ANDROID__) || defined(__APPLE__))
    rendererLog("RENDER_API_TRANSLATED_COUNTS textures0=%u textures1=%u textures2=%u\n",
        completedTranslatedTextures[0],completedTranslatedTextures[1],completedTranslatedTextures[2]);
    for(const auto& [object,resolved]:liveResolvedTextures) {
        const auto bytes=registry.snapshotStoppedGraphicsVirtual(resolved.address,static_cast<std::uint32_t>(resolved.bytes.size()));
        rendererLog("RENDER_API_RESOLVE_RETAINED object=%08x status=%s\n",object,
            bytes && *bytes==resolved.bytes?"matches-retained-guest-memory":"different-or-unavailable");
    }
    liveResolvedTextures.clear();
#endif
    // Slots have distinct writers and are read only after all workers joined.
    for (unsigned i=0; i<CaptureLimit; ++i) {
        const auto address=presentedTextures[i];
        if (!address) continue;
        const auto bytes=registry.snapshotStoppedGuestHeap(address, 52);
        const auto description=bytes ? recomp::gpu::describeGta5TextureHeader(*bytes) : std::nullopt;
        if (!description) {
            rendererLog("RENDER_TEXTURE_STOPPED call=%u object=%08x status=%s\n",
                i+1, address, bytes ? "not-texture-fetch" : "unowned-heap-range");
            continue;
        }
        const auto& d=*description;
        rendererLog("RENDER_TEXTURE_STOPPED call=%u object=%08x status=decoded "
            "size=%ux%ux%u dimension=%u pitch=%u format=%u endian=%u tiled=%u "
            "base_virtual=%08x mip_virtual=%08x mips=%u..%u swizzle=%u%u%u%u\n",
            i+1, address, d.width, d.height, d.depthOrLayers,
            static_cast<unsigned>(d.dimension), d.pitchTexels, d.format, d.endian,
            unsigned(d.tiled), d.baseAddress, d.mipAddress, d.mipMin, d.mipMax,
            d.swizzle[0], d.swizzle[1], d.swizzle[2], d.swizzle[3]);
    }
    const auto word=[&](std::uint32_t address)->std::optional<std::uint32_t> {
        const auto bytes=registry.snapshotStoppedGuestHeap(address,4);
        if(!bytes)return std::nullopt;
        return (std::uint32_t{(*bytes)[0]}<<24)|(std::uint32_t{(*bytes)[1]}<<16)|
            (std::uint32_t{(*bytes)[2]}<<8)|(*bytes)[3];
    };
    const auto match=[&](unsigned stage,const std::vector<std::uint8_t>& bytes) {
        if(programs)for(unsigned i=0;i<programs->identities().size();++i) {
            const auto& key=programs->identities()[i];
            if(key.stage==stage && key.bigEndianCode==bytes)return static_cast<int>(i);
        }
        return -1;
    };
    for(unsigned i=0;i<CaptureLimit;++i) {
        const auto& patch=vertexPatches[i];
        if(patch.object && patch.returned) {
            const auto range=recomp::gpu::describeGta5ShaderCode(patch.object,0,patch.variant,word);
            const auto code=range?registry.snapshotStoppedGraphicsVirtual(patch.code,range->byteCount):std::nullopt;
            rendererLog("RENDER_VERTEX_PATCH call=%u object=%08x variant=%u patched_virtual=%08x "
                "bytes=%u native_role=%d status=%s\n",i+1,patch.object,patch.variant,patch.code,
                range?range->byteCount:0,code?match(0,*code):-1,code?"captured-after-return":"unavailable");
        }
        const auto object=pixelShaders[i];
        if(object) {
            const auto range=recomp::gpu::describeGta5ShaderCode(object,1,0,word);
            const auto code=range?registry.snapshotStoppedGraphicsVirtual(range->virtualAddress,range->byteCount):std::nullopt;
            rendererLog("RENDER_PIXEL_RESOURCE call=%u object=%08x code_virtual=%08x bytes=%u "
                "native_role=%d status=%s\n",i+1,object,range?range->virtualAddress:0,
                range?range->byteCount:0,code?match(1,*code):-1,code?"captured":"unavailable");
        }
    }
}

bool saveStoppedRendererDraw(const std::string& directory) {
    if(!firstUnsupportedInline && !firstUnsupportedResolve && !firstNativeResolveFailure && !firstUnsupportedClear && !firstUnsupportedQueue && !firstUnsupportedScissor && !shaderPatchAttempts && !boundIndexedAttempts)return true;
    // The runner supplies a new request-local directory, never a game path.
    // Reject an existing directory so an old capture cannot be overwritten.
    std::error_code error;
    if(!std::filesystem::create_directory(directory,error) || error)return false;
    const auto save=[&](const std::string& name,const auto& bytes) {
        // The trace process has a per-file output cap. Keep large private
        // textures complete as bounded chunks instead of silently truncating.
        constexpr std::size_t Chunk=65536;
        if(bytes.size()<=Chunk) {
            std::ofstream file(std::filesystem::path(directory)/name,std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(file);
        }
        for(std::size_t at=0;at<bytes.size();at+=Chunk) {
            const auto part=name.substr(0,name.size()-4)+"-part-"+std::to_string(at/Chunk)+".bin";
            std::ofstream file(std::filesystem::path(directory)/part,std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()+at),std::min(Chunk,bytes.size()-at));if(!file)return false;
        }
        return true;
    };
    bool queueOk=true;
    if(retainedQueryEndTrace && !retainedQueryEndTrace->commandsThroughEnd.empty()) {
        std::ofstream meta(std::filesystem::path(directory)/"latest-query-command-trace.json");
        queueOk=recomp::gpu::writeGta5QueryCommandTrace(*retainedQueryEndTrace,meta,
            [&](const char* name,const auto& bytes){return save(std::string("latest-")+name,bytes);})&&queueOk;
        meta.close();queueOk=bool(meta)&&queueOk;
    }
    if(firstNativeResolveFailure) {
        std::ofstream meta(std::filesystem::path(directory)/"native-resolve-failure.json");
        queueOk=recomp::gpu::writeNativeResolveFailureCapture(*firstNativeResolveFailure,meta,save)&&queueOk;
        meta.close();queueOk=bool(meta)&&queueOk;
    }
#if (defined(__ANDROID__) || defined(__APPLE__))
    queueOk=saveStoppedFixed16Targets(directory,save) && queueOk;
#endif
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(tileRecordingDiagnostic.pending()) {
        const auto& d=tileRecordingDiagnostic;
        std::ofstream meta(std::filesystem::path(directory)/"tile-recording-diagnostic.json");
        meta<<"{\"version\":2,\"capture_only\":true,\"device\":"<<d.device()
            <<",\"gpu_executed\":false,\"guest_pixels_published\":false,\"completion_granted\":false,\"stop_boundary\":";
        if(const auto stop=tileRecordingStop.load())meta<<'"'<<stop<<'"';else meta<<"null";
        meta<<",\"seed_state_file\":\"scissor-device-state.bin\",\"seed_records\":[";
        for(unsigned i=0;i<d.initialRecords().size();++i) {
            if(i)meta<<',';const auto name="tile-seed-record-"+std::to_string(i)+".bin";
            queueOk=save(name,d.initialRecords()[i]) && queueOk;
            meta<<"{\"descriptor_offset\":"<<d.DescriptorOffsets[i]<<",\"bytes\":"<<d.initialRecords()[i].size()
                <<",\"file\":\""<<name<<"\"}";
        }
        meta<<"],\"scissors\":[";
        for(unsigned i=0;i<d.scissors().size();++i) {
            if(i)meta<<',';meta<<'[';
            for(unsigned c=0;c<4;++c){if(c)meta<<',';meta<<d.scissors()[i][c];}meta<<']';
        }
        meta<<"],\"clears\":[";
        for(unsigned i=0;i<d.clears().size();++i) {
            if(i)meta<<',';const auto& capture=d.clears()[i];const auto& call=capture.call;
            const auto prefix="tile-recorded-clear-"+std::to_string(i)+"-";
            meta<<"{\"stack\":"<<call.stack<<",\"device\":"<<call.device<<",\"flags\":"<<call.flags
                <<",\"rectangle_pointer\":"<<call.rectangle<<",\"color_pointer\":"<<call.color
                <<",\"stencil\":"<<call.stencil<<",\"caller\":"<<call.link<<",\"depth_bits_hex\":\""
                <<std::hex<<call.depthBits<<std::dec<<'"';
            const auto optionalFile=[&](const char* key,const auto& bytes) {
                meta<<",\""<<key<<"\":";
                if(bytes){const auto name=prefix+key+".bin";queueOk=save(name,*bytes) && queueOk;meta<<'"'<<name<<'"';}
                else meta<<"null";
            };
            optionalFile("state",capture.state);optionalFile("rectangle",capture.rectangle);optionalFile("color",capture.color);
            meta<<",\"surfaces\":[";
            for(unsigned slot=0;slot<5;++slot) {
                if(slot)meta<<',';const auto& surface=capture.surfaces[slot];
                meta<<"{\"slot\":"<<slot<<",\"object\":"<<surface.object<<",\"tile_cache\":"<<surface.tileCache;
                const auto key="surface-"+std::to_string(slot);optionalFile(key.c_str(),surface.header);meta<<'}';
            }
            meta<<"]}";
        }
        meta<<"],\"events\":[";
        for(unsigned i=0;i<d.events().size();++i) {
            if(i)meta<<',';const auto& e=d.events()[i];
            meta<<"{\"kind\":\""<<(e.kind==recomp::gpu::Gta5TileRecordingDiagnostic::Kind::Scissor?"scissor":
                e.kind==recomp::gpu::Gta5TileRecordingDiagnostic::Kind::Clear?"clear":
                e.kind==recomp::gpu::Gta5TileRecordingDiagnostic::Kind::Indexed?"indexed":"resolve")
                <<"\",\"index\":"<<e.index<<'}';
        }
        meta<<"],\"recorded_resolves\":[";
        for(unsigned i=0;i<recordedTileResolves.size();++i) {
            const auto& c=recordedTileResolves[i];const auto& r=c.before;
            const auto prefix="tile-recorded-resolve-"+std::to_string(i)+"-";
            const auto filename=prefix+"record.json";
            if(i)meta<<',';meta<<'"'<<filename<<'"';
            std::ofstream record(std::filesystem::path(directory)/filename);
            record<<"{\"version\":1,\"index\":"<<i<<",\"tile\":"<<c.plan.tile
                <<",\"original_returned\":"<<(c.originalReturned?"true":"false")
                <<",\"output_available\":"<<(c.output?"true":"false")
                <<",\"gpu_executed\":false,\"guest_pixels_published\":false,\"completion_granted\":false,\"stack\":"<<r.stack
                <<",\"f1_bits\":"<<r.f1<<",\"arguments\":[";
            for(unsigned j=0;j<r.arguments.size();++j){if(j)record<<',';record<<r.arguments[j];}
            record<<"],\"command_address\":"<<c.plan.command+4<<",\"command_budget\":"<<c.plan.CommandBytes
                <<",\"quad_address\":"<<c.plan.allocation.address<<",\"files\":{";
            bool first=true;
            const auto file=[&](const std::string& key,const auto& bytes) {
                if(!first)record<<',';first=false;record<<'"'<<key<<"\":";
                if(bytes) {const auto name=prefix+key+".bin";queueOk=save(name,*bytes) && queueOk;record<<'"'<<name<<'"';}
                else record<<"null";
            };
            file("state-before",r.state);file("allocator-tail",r.allocatorTail);file("allocator-fence",r.allocatorFence);
            file("resolve-globals",r.resolveGlobals);file("destination-before",r.destinationHeader);
            file("rectangle",r.pointed.rectangle);file("point",r.pointed.point);file("clear-color",r.pointed.color);
            file("stack92",r.pointed.stack92);file("stack100",r.pointed.stack100);
            if(r.surfaces)for(unsigned j=0;j<5;++j)file("surface-"+std::to_string(j),r.surfaces->surfaces[j].header);
            if(c.output) {
                const auto saved=[&](const char* key,const auto& bytes){file(key,std::optional<std::vector<std::uint8_t>>{bytes});};
                saved("commands",c.output->commands);saved("quad",c.output->quad);
                saved("state-after",c.output->state);saved("destination-after",c.output->destinationHeader);
            }
            record<<"}}\n";record.close();queueOk=bool(record) && queueOk;
        }
        meta<<"]}\n";meta.close();queueOk=bool(meta) && queueOk;
        if(stoppedTileResolve) {
            const auto& r=*stoppedTileResolve;
            std::ofstream resolve(std::filesystem::path(directory)/"tile-resolve.json");
            resolve<<"{\"version\":1,\"capture_only\":true,\"gpu_executed\":false,\"guest_pixels_published\":false,\"completion_granted\":false,\"abi\":\"82388A38-r3-r10-lr\",\"stack\":"
                <<r.stack<<",\"f1_bits\":"<<r.f1<<",\"arguments\":[";
            for(unsigned i=0;i<r.arguments.size();++i){if(i)resolve<<',';resolve<<r.arguments[i];}
            resolve<<"],\"files\":{";
            bool first=true;
            const auto file=[&](const std::string& key,const auto& bytes) {
                if(!first)resolve<<',';first=false;resolve<<'"'<<key<<"\":";
                if(bytes) {
                    const auto name="tile-resolve-"+key+".bin";queueOk=save(name,*bytes) && queueOk;
                    resolve<<'"'<<name<<'"';
                } else resolve<<"null";
            };
            file("state",r.state);file("destination-header",r.destinationHeader);
            file("allocator-tail",r.allocatorTail);
            file("allocator-header",r.allocatorHeader);
            file("allocator-fence",r.allocatorFence);file("resolve-globals",r.resolveGlobals);
            file("rectangle",r.pointed.rectangle);file("destination-point",r.pointed.point);
            file("clear-color",r.pointed.color);file("stack92",r.pointed.stack92);file("stack100",r.pointed.stack100);
            if(r.surfaces)for(unsigned i=0;i<r.surfaces->surfaces.size();++i)
                file("surface-"+std::to_string(i),r.surfaces->surfaces[i].header);
            resolve<<"}}\n";resolve.close();queueOk=bool(resolve) && queueOk;
        }
    }
#endif
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(stoppedTileEnd) {
        std::ofstream meta(std::filesystem::path(directory)/"tile-end.json");
        // This separately bounded capture keeps one named file per owned
        // range, unlike texture snapshots' existing chunked-file convention.
        const auto endSave=[&](const std::string& name,const auto& bytes) {
            std::ofstream file(std::filesystem::path(directory)/name,std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(file);
        };
        queueOk=recomp::gpu::writeGta5EndTilingCapture(*stoppedTileEnd,meta,endSave)&&queueOk;
        meta.close();queueOk=bool(meta)&&queueOk;
        if(stoppedEndQueue) {
            std::ofstream cpu(std::filesystem::path(directory)/"tile-end-cpu.json");
            cpu<<"{\"version\":3,\"original_cpu_called\":true,\"original_cpu_returned\":"<<(stoppedEndCpuReturned?"true":"false")
               <<",\"queue_sealed\":"<<(stoppedEndQueue->phase()==recomp::gpu::Gta5NativeTileSubmissionCapture::Phase::Sealed?"true":"false")
               <<",\"initial_packets_attempted\":"<<(stoppedInitialTileQueue && stoppedInitialTileQueue->attempted()?"true":"false")
               <<",\"native_cache_completed\":"<<(stoppedInitialTileQueue && stoppedInitialTileQueue->cacheCompleted()?"true":"false")
               <<",\"original_irq_returned\":"<<(stoppedInitialTileQueue && stoppedInitialTileQueue->interruptCompleted()?"true":"false")
               <<",\"initial_ring_retired\":"<<(stoppedInitialRingRetired?"true":"false")
               <<",\"scene_gpu_executed\":false,\"scene_ring_retired\":false,\"scene_completion_granted\":false,\"entry_read_word\":"<<stoppedEndQueue->entryReadWord()
               <<",\"last_write_word\":"<<stoppedEndQueue->lastWriteWord()<<",\"state_after\":";
            if(stoppedEndCpuState) {
                cpu<<"\"tile-end-cpu-state-after.bin\"";
                queueOk=endSave("tile-end-cpu-state-after.bin",*stoppedEndCpuState)&&queueOk;
            }else cpu<<"null";
            cpu<<",\"submissions\":[";
            for(unsigned i=0;i<stoppedEndQueue->submissions().size();++i) {
                const auto& s=stoppedEndQueue->submissions()[i];if(i)cpu<<',';
                cpu<<"{\"before\":"<<s.before<<",\"after\":"<<s.after<<",\"ring_words\":[";
                for(unsigned w=0;w<s.ringWords.size();++w){if(w)cpu<<',';cpu<<s.ringWords[w];}
                cpu<<"],\"indirects\":[";
                for(unsigned j=0;j<s.indirects.size();++j) {
                    const auto& ib=s.indirects[j];if(j)cpu<<',';
                    const auto name="tile-end-cpu-ib-"+std::to_string(i)+"-"+std::to_string(j)+".bin";
                    cpu<<"{\"physical\":"<<ib.physical<<",\"file\":\""<<name<<"\",\"bytes\":"<<ib.bytes.size()<<'}';
                    queueOk=endSave(name,ib.bytes)&&queueOk;
                }
                cpu<<"]}";
            }
            cpu<<"]}\n";cpu.close();queueOk=bool(cpu)&&queueOk;
        }
    }
#endif
    if(firstUnsupportedScissor) {
        const auto& s=*firstUnsupportedScissor;
        if(!s.state.empty())queueOk=save("scissor-device-state.bin",s.state) && queueOk;
        std::ofstream meta(std::filesystem::path(directory)/"scissor.json");
        meta<<"{\"version\":1,\"boundary\":\"823ABBE0-before-emission\",\"device\":"<<s.device
            <<",\"caller\":"<<s.caller<<",\"rectangle\":[";
        for(unsigned i=0;i<4;++i){if(i)meta<<',';meta<<s.rectangle[i];}
        meta<<"],\"state_bytes\":"<<s.state.size()<<",\"state_file\":"
            <<(s.state.empty()?"null":"\"scissor-device-state.bin\"")
            <<",\"plan_available\":"<<(s.plan?"true":"false")
            <<",\"binned\":"<<(s.plan?(s.plan->binned?"true":"false"):"null")
            <<",\"planned_words\":[";
        if(s.plan)for(unsigned i=0;i<s.plan->words.size();++i){if(i)meta<<',';meta<<s.plan->words[i];}
        meta<<"],\"surfaces\":[";
        if(s.bindings)for(unsigned i=0;i<s.bindings->surfaces.size();++i) {
            if(i)meta<<',';
            const auto& surface=s.bindings->surfaces[i];
            const auto name="scissor-surface-"+std::to_string(i)+".bin";
            meta<<"{\"slot\":"<<i<<",\"object\":"<<surface.object<<",\"tile_cache_object\":"<<surface.tileCacheObject
                <<",\"header_bytes\":"<<(surface.header?surface.header->size():0)<<",\"header_file\":";
            if(surface.header){queueOk=save(name,*surface.header) && queueOk;meta<<'"'<<name<<'"';}
            else meta<<"null";
            meta<<'}';
        }
        meta<<"],\"gpu_executed_by_capture\":false,\"native_binned_execution\":false}";
        meta.close();queueOk=bool(meta) && queueOk;
    }
    if(boundIndexedAttempts) {
        // Separate bounded record files keep every JSON below JNI's 64KiB
        // per-file limit. Vertex payloads use save()'s existing chunk contract.
        std::ofstream index(std::filesystem::path(directory)/"bound-indexed-draws.json");
        index<<"{\"version\":1,\"boundary\":\"823B9B58-observation\",\"attempts\":"<<boundIndexedAttempts.load()
            <<",\"failures\":"<<boundIndexedFailures.load()<<",\"dropped\":"<<boundIndexedDropped.load()
            <<",\"retained_bytes\":"<<boundIndexedBudget.retainedBytes()
            <<",\"failure_retained_bytes\":"<<boundIndexedBudget.failureBytes()
            <<",\"routine_limit\":"<<BoundIndexedCaptureBudget::RoutineRecords
            <<",\"failure_reserved_bytes\":"<<BoundIndexedCaptureBudget::FailureReserve
            <<",\"gpu_executed_by_capture\":false,\"records\":[";
        bool first=true;
        for(const auto& s:boundIndexedSnapshots) {
            const auto prefix="bound-indexed-"+std::to_string(s.sequence)+"-";
            const auto filename=prefix+"record.json";
            if(!first)index<<',';first=false;index<<'"'<<filename<<'"';
            std::ofstream meta(std::filesystem::path(directory)/filename);
            meta<<"{\"sequence\":"<<s.sequence<<",\"caller\":"<<s.caller<<",\"device\":"<<s.device
                <<",\"request\":["<<s.request.primitive<<','<<s.request.baseVertex<<','<<s.request.startIndex<<','<<s.request.indexCount
                <<"],\"geometry_available\":"<<(s.geometry?"true":"false")
                <<",\"geometry_recording_only\":"<<(s.geometryRecordingOnly?"true":"false")
                <<",\"boundary\":\""<<(s.observationPc?"823B9DBC-before-original-recording":
                    s.capturePhase==BoundIndexedCapturePhase::Exception?"823B9B58-exception":
                    s.capturePhase==BoundIndexedCapturePhase::Returned?"823B9B58-after-return":"prepared-indexed-draw-stop")<<'"'
                <<",\"capture_phase\":\""<<(s.capturePhase==BoundIndexedCapturePhase::Prepared?"prepared":
                    s.capturePhase==BoundIndexedCapturePhase::Returned?"returned":"exception")<<'"'
                <<",\"observation_pc\":"<<s.observationPc
                <<",\"original_recording_admitted\":"<<(s.recordingWindow?"true":"false")
                <<",\"original_emission_returned\":"<<(s.originalEmissionReturned?"true":"false")
                <<",\"recorded_scissor\":";
            if(s.recordedScissor) {
                meta<<'[';for(unsigned i=0;i<4;++i){if(i)meta<<',';meta<<(*s.recordedScissor)[i];}meta<<']';
            } else meta<<"null";
            meta
                <<",\"ordered_constants\":"<<(s.orderedConstants?"true":"false")
                <<",\"native_bins\":[";
            for(unsigned i=0;i<4;++i){if(i)meta<<',';meta<<s.nativeBins.words[i];}
            meta<<','<<s.nativeBins.knownWords<<"],\"submitted_textures\":[";
            bool firstTexture=true;
            for(unsigned slot=0;slot<32;++slot)if(s.textures[slot]) {
                if(!firstTexture)meta<<',';firstTexture=false;meta<<'['<<slot;
                for(auto word:*s.textures[slot])meta<<','<<word;
                meta<<']';
            }
            meta<<']';
            if(s.geometry) {
                const auto& g=*s.geometry;
                meta<<",\"index_resource\":"<<g.indexResource<<",\"index_physical\":"<<g.indexPhysicalAddress
                    <<",\"vertex_resource\":"<<g.vertexResource<<",\"vertex_physical\":"<<g.draw.vertexAddress
                    <<",\"vertex_extent\":"<<g.vertexExtent<<",\"stride\":"<<g.draw.stride
                    <<",\"vertex_count\":"<<g.draw.vertexCount<<",\"fetch_slot\":"<<g.fetchSlot
                    <<",\"vertex_shader\":"<<g.draw.vertexShader<<",\"pixel_shader\":"<<g.draw.pixelShader
                    <<",\"scissor\":";
                if(g.draw.scissorRegisters)meta<<'['<<(*g.draw.scissorRegisters)[0]<<','<<(*g.draw.scissorRegisters)[1]
                    <<','<<(*g.draw.scissorRegisters)[2]<<']';else meta<<"null";
            }
            meta<<",\"artifacts\":{";bool firstArtifact=true;
            const auto artifact=[&](const char* name,const auto& bytes) {
                if(bytes.empty())return;
                const auto file=prefix+name+".bin";queueOk=save(file,bytes) && queueOk;
                if(!firstArtifact)meta<<',';firstArtifact=false;
                meta<<'"'<<name<<"\":{\"path\":\""<<file<<"\",\"bytes\":"<<bytes.size()<<",\"chunk_bytes\":65536}";
            };
            const auto words=[&](const char* name,const auto& values) {
                std::vector<std::uint8_t> bytes;bytes.reserve(values.size()*4);
                for(auto word:values)for(unsigned b=0;b<4;++b)bytes.push_back(word>>(24-b*8));
                artifact(name,bytes);
            };
            artifact("device",s.deviceState);artifact("vs",s.vertexCode);artifact("ps",s.pixelCode);
            artifact("index-header",s.indexHeader);artifact("vertex-header",s.vertexHeader);
            if(s.surfaces)for(unsigned slot=0;slot<5;++slot) {
                const auto& surface=s.surfaces->surfaces[slot];
                if(surface.header)artifact(("surface-"+std::to_string(slot)+"-header").c_str(),*surface.header);
            }
            if(s.orderedConstants) {
                words("vertex-constants",s.vertexConstants);words("pixel-constants",s.pixelConstants);
            }
            for(const auto& [slot,mip]:s.mipTextures) {
                const auto name="texture-"+std::to_string(slot);
                artifact((name+"-base").c_str(),mip.base);artifact((name+"-mips").c_str(),mip.mips);
            }
            for(const auto& plane:s.texturePages.planes)for(unsigned page=0;page<plane.pages.size();++page)
                if(plane.pages[page]) {
                    const auto name="texture-"+std::to_string(plane.slot)+(plane.mip?"-mip-page-":"-base-page-")+std::to_string(page);
                    artifact(name.c_str(),*plane.pages[page]);
                }
            if(s.geometry) {
                artifact("vertices",s.geometry->draw.vertices);
                words("raw-indices-u32be",s.geometry->rawIndices);words("effective-indices-u32be",s.geometry->effectiveIndices);
            }
            meta<<"},\"texture_capture\":{\"shader_slots_known\":"<<(s.pixelTextureSlots?"true":"false")<<",\"selected_slots\":[";
            bool firstSelected=true;
            if(s.pixelTextureSlots)for(unsigned slot=0;slot<32;++slot)if(*s.pixelTextureSlots&(1u<<slot)) {
                if(!firstSelected)meta<<',';firstSelected=false;meta<<slot;
            }
            meta<<"],\"complete_mip_slots\":[";
            for(unsigned i=0;i<s.mipTextures.size();++i){if(i)meta<<',';meta<<s.mipTextures[i].first;}
            meta<<"],\"pages_are_complete_textures\":false,\"unavailable_pages\":"<<s.texturePages.unavailablePages
                <<",\"page_bytes\":4096,\"pages_per_plane_limit\":16}}";
            meta.close();queueOk=bool(meta) && queueOk;
        }
        index<<"]}";index.close();queueOk=bool(index) && queueOk;
    }
    if(shaderPatchAttempts) {
        std::ofstream meta(std::filesystem::path(directory)/"command-shader-patches.json");
        meta<<"{\"version\":1,\"caller\":"<<0x823BDD60u<<",\"attempts\":"<<shaderPatchAttempts
            <<",\"capacity\":"<<ShaderPatchHistoryLimit<<",\"failures\":"<<shaderPatchCaptureFailures.load()
            <<",\"gpu_executed_by_capture\":false,\"snapshots\":[";
        bool first=true;
        const auto begin=shaderPatchAttempts>ShaderPatchHistoryLimit?shaderPatchAttempts-ShaderPatchHistoryLimit+1:1;
        for(auto sequence=begin;sequence<=shaderPatchAttempts;++sequence) {
            const auto slot=(sequence-1)%ShaderPatchHistoryLimit;
            if(shaderPatchSequences[slot]!=sequence || !shaderPatchHistory[slot])continue;
            const auto& s=*shaderPatchHistory[slot];
            const auto prefix="command-shader-patch-"+std::to_string(sequence)+"-";
            if(!first)meta<<',';first=false;
            meta<<"{\"sequence\":"<<sequence<<",\"arguments\":[";
            for(unsigned i=0;i<s.arguments.size();++i){if(i)meta<<',';meta<<s.arguments[i];}
            meta<<"],\"source_address\":"<<s.sourceRange.virtualAddress<<",\"byte_count\":"<<s.sourceRange.byteCount
                <<",\"returned\":"<<(s.returned?"true":"false")<<",\"after_available\":"<<(s.afterAvailable?"true":"false")
                <<",\"source_equals_before\":"<<(s.source==s.before?"true":"false")<<",\"artifacts\":{";
            bool firstArtifact=true;
            const auto artifact=[&](const char* name,const auto& bytes) {
                if(bytes.empty())return;
                const auto file=prefix+name+".bin";
                queueOk=save(file,bytes) && queueOk;
                if(!firstArtifact)meta<<',';firstArtifact=false;
                meta<<'"'<<name<<"\":{\"path\":\""<<file<<"\",\"bytes\":"<<bytes.size()<<'}';
            };
            artifact("source",s.source);artifact("before",s.before);artifact("after",s.after);
            artifact("header",s.sourceHeader);artifact("patch-table",s.patchTable);
            artifact("fetch-templates",s.fetchTemplates);artifact("declaration",s.declaration);artifact("lookup",s.lookup);
            meta<<"}}";
        }
        meta<<"]}";meta.close();queueOk=bool(meta) && queueOk;
    }
    if(firstUnsupportedQueue) {
        std::ofstream meta(std::filesystem::path(directory)/"render-queue.json");
        queueOk=recomp::gpu::writeGta5RenderQueueInventory(*firstUnsupportedQueue,meta,save) && queueOk;
        meta.close();queueOk=bool(meta) && queueOk;
    }
    // A queue-only failure has no inline shader capture to dereference or
    // fabricate. Preserve the original protocol/exception alongside this file.
    if(!firstUnsupportedInline && !firstUnsupportedResolve && !firstUnsupportedClear)return queueOk;
    if(firstUnsupportedClear && !firstUnsupportedInline && !firstUnsupportedResolve) {
        const auto& s=*firstUnsupportedClear;const auto& call=s.call;
        const std::vector<std::uint8_t> unavailable;
        bool ok=save("device-state.bin",s.state.value_or(unavailable));
        ok=save("clear-rectangle.bin",s.rectangle.value_or(unavailable)) && ok;
        ok=save("clear-color.bin",s.color.value_or(unavailable)) && ok;
        std::ofstream meta(std::filesystem::path(directory)/"clear.json");
        const auto audit=recomp::gpu::auditGta5Color8Clear(s);
        meta<<"{\"version\":1,\"issues\":"<<audit.issues<<",\"effective_flags\":"<<audit.effectiveFlags
            <<",\"stack\":"<<call.stack<<",\"device\":"<<call.device
            <<",\"flags\":"<<call.flags<<",\"rectangle\":"<<call.rectangle<<",\"color\":"<<call.color
            <<",\"depth_bits\":"<<call.depthBits<<",\"stencil\":"<<call.stencil<<",\"link\":"<<call.link
            <<",\"state_available\":"<<(s.state?"true":"false")<<",\"surfaces\":[";
        for(unsigned i=0;i<s.surfaces.size();++i) {
            const auto& surface=s.surfaces[i];if(i)meta<<',';
            meta<<"{\"slot\":"<<i<<",\"object\":"<<surface.object<<",\"tile_cache_object\":"<<surface.tileCache
                <<",\"header_bytes\":"<<(surface.header?surface.header->size():0)<<",\"header_reader\":\""
                <<(surface.object<0x20000000?"heap":"graphics")<<"\"}";
            ok=save("surface-"+std::to_string(i)+"-resource.bin",surface.header.value_or(unavailable)) && ok;
        }
        meta<<"],\"frames\":[";
        for(unsigned i=0;i<s.frames.size();++i) {
            const auto& f=s.frames[i];if(i)meta<<',';
            meta<<"{\"stack\":"<<f.stack<<",\"parent\":"<<f.parent<<",\"saved_link\":"<<f.savedLink<<'}';
        }
        meta<<"]}";return queueOk && ok && bool(meta);
    }
    if(firstUnsupportedResolve && !firstUnsupportedInline) {
        const auto& s=*firstUnsupportedResolve;
        const std::vector<std::uint8_t> unavailable;
        const bool ok=save("device-state.bin",s.state) && save("source-resource.bin",s.sourceHeader) &&
            save("destination-resource.bin",s.destinationHeader) && save("stack-92.bin",s.stack92) && save("stack-100.bin",s.stack100);
        const bool pointedOk=save("source-rectangle.bin",s.pointed.rectangle.value_or(unavailable)) &&
            save("destination-point.bin",s.pointed.point.value_or(unavailable)) &&
            save("clear-color.bin",s.pointed.color.value_or(unavailable)) &&
            save("source-rgba.bin",s.sourceRgba) && save("destination-storage.bin",s.destinationStorage) &&
            save("source-resource-tail.bin",s.sourceResourceTail) && save("source-samples-rgba8.bin",s.colorSamples.rgba) &&
            save("source-depth-f32le.bin",s.colorSamples.depth);
        std::ofstream meta(std::filesystem::path(directory)/"resolve.json");
        meta<<"{\"version\":2,\"stack\":"<<s.stack<<",\"source\":"<<s.source<<",\"f1_bits\":"<<s.f1
            <<",\"issues\":"<<s.issues<<",\"header_reader\":\""<<(s.graphicsHeader?"graphics":"heap")
            <<"\",\"header_available\":"<<(s.destinationHeader.size()==52?"true":"false")
            <<",\"clear_completed\":"<<(s.clearCompleted?"true":"false")
            <<",\"source_samples\":"<<s.sourceSamples<<",\"source_color_format\":"<<s.sourceColorFormat
            <<",\"source_clear_only\":"<<(s.sourceClearOnly?"true":"false")
            <<",\"source_sample_bytes\":"<<s.colorSamples.rgba.size()<<",\"source_content_serial\":"<<s.colorSamples.serial
            <<",\"source_sample_order\":\"pixel-sample-rgba8\",\"source_sample_provenance\":\""<<s.colorSamples.provenance<<"\""
            <<",\"source_depth_object\":"<<s.colorSamples.depthObject<<",\"source_depth_bytes\":"<<s.colorSamples.depth.size()
            <<",\"source_depth_uniform\":"<<(s.colorSamples.depthUniform?"true":"false")<<",\"source_depth_order\":\"pixel-sample-f32le\""
            <<",\"source_plan\":{\"width\":"<<s.sourcePlan.width<<",\"height\":"<<s.sourcePlan.height
            <<",\"pitch\":"<<s.sourcePlan.pitch<<",\"base_tile\":"<<s.sourcePlan.baseTile
            <<",\"tile_count\":"<<s.sourcePlan.tileCount<<"},\"arguments\":[";
        for(unsigned i=0;i<s.arguments.size();++i){if(i)meta<<',';meta<<s.arguments[i];}
        meta<<"],\"bound_surfaces\":[";bool surfacesOk=true;
        if(s.surfaces)for(unsigned i=0;i<s.surfaces->surfaces.size();++i) {
            const auto& b=s.surfaces->surfaces[i];if(i)meta<<',';
            meta<<"{\"slot\":"<<i<<",\"object\":"<<b.object<<",\"tile_cache\":"<<b.tileCacheObject
                <<",\"header_bytes\":"<<(b.header?b.header->size():0)<<'}';
            surfacesOk=save("bound-surface-"+std::to_string(i)+"-resource.bin",b.header.value_or(unavailable)) && surfacesOk;
        }
        meta<<"]}";return queueOk && ok && pointedOk && surfacesOk && bool(meta);
    }
    const auto& s=*firstUnsupportedInline;
    if(s.queryEvidence) {
        std::ofstream queryMeta(std::filesystem::path(directory)/"stopped-query-evidence.json");
        queueOk=recomp::gpu::writeGta5StoppedQueryEvidence(*s.queryEvidence,queryMeta,save)&&queueOk;
        queryMeta.close();queueOk=bool(queryMeta)&&queueOk;
    }
    if(s.queryCommands) {
        std::ofstream queryMeta(std::filesystem::path(directory)/"query-command-trace.json");
        queueOk=recomp::gpu::writeGta5QueryCommandTrace(*s.queryCommands,queryMeta,save)&&queueOk;
        queryMeta.close();queueOk=bool(queryMeta)&&queueOk;
    }
    const auto& raw=s.submittedTexturePages;
    bool pagesOk=true;
    std::ofstream pagesMeta(std::filesystem::path(directory)/"submitted-texture-pages.json");
    pagesMeta<<"{\"version\":1,\"source\":\"submitted-fetch-owned-pages\",\"layout_verified\":false,\"complete_texture\":false"
        <<",\"page_bytes\":4096,\"pages_per_plane_limit\":16,\"total_byte_limit\":"<<raw.MaxBytes
        <<",\"captured_bytes\":"<<raw.capturedBytes<<",\"unavailable_pages\":"<<raw.unavailablePages<<",\"planes\":[";
    for(unsigned i=0;i<raw.planes.size();++i) {
        const auto& plane=raw.planes[i];if(i)pagesMeta<<',';
        const std::string prefix="raw-texture-"+std::to_string(plane.slot)+(plane.mip?"-mip":"-base");
        pagesMeta<<"{\"slot\":"<<plane.slot<<",\"mip\":"<<(plane.mip?"true":"false")<<",\"address\":"<<plane.address
            <<",\"address_space_truncated\":"<<(plane.addressSpaceTruncated?"true":"false")<<",\"pages\":[";
        for(unsigned j=0;j<plane.pages.size();++j) {
            if(j)pagesMeta<<',';
            const auto& page=plane.pages[j];pagesMeta<<(page?page->size():0);
            if(page)pagesOk=save(prefix+"-page-"+std::to_string(j)+".bin",*page) && pagesOk;
        }
        pagesMeta<<"]}";
    }
    pagesMeta<<"]}";pagesMeta.close();pagesOk=pagesOk && bool(pagesMeta);
    bool ok=save("vertex-shader.bin",s.vertexCode) && save("pixel-shader.bin",s.pixelCode) &&
        save("vertex-resource.bin",s.vertexResource) && save("pixel-resource.bin",s.pixelResource) &&
        save("device-state.bin",s.deviceState) && save("indices.bin",s.indices) && save("source-samples-rgba8.bin",s.colorSamples.rgba) &&
        save("source-depth-f32le.bin",s.colorSamples.depth);
    std::ofstream meta(std::filesystem::path(directory)/"draw.json");
    meta<<"{\"version\":2,\"device\":"<<s.pending.device<<",\"primitive\":"<<s.pending.primitive
        <<",\"vertex_count\":"<<s.pending.count<<",\"stride\":"<<s.pending.stride
        <<",\"index_count\":"<<s.pending.indexCount<<",\"index_format\":"<<s.pending.indexFormat
        <<",\"min_vertex\":"<<s.pending.minVertex<<",\"index_address\":"<<s.indexAddress
        <<",\"index_words\":"<<s.indexWords
        <<",\"vertex_object\":"<<s.vertexObject<<",\"pixel_object\":"<<s.pixelObject
        <<",\"texture_captured\":"<<s.textureCaptured<<",\"texture_unavailable\":"<<s.textureUnavailable;
    meta<<",\"caller_stack\":"<<s.surfaceEvidence.stack<<",\"caller_link\":"<<s.surfaceEvidence.link
        <<",\"argument_surface_evidence_execution_authority\":false,\"caller_frames\":[";
    for(unsigned i=0;i<s.surfaceEvidence.frames.size();++i) {
        const auto& frame=s.surfaceEvidence.frames[i];if(i)meta<<',';
        meta<<"{\"stack\":"<<frame.stack<<",\"parent\":"<<frame.parent<<",\"saved_link\":"<<frame.savedLink<<'}';
    }
    meta<<']';
    meta<<",\"source_sample_storage\":{\"object\":"<<s.colorSamples.object<<",\"width\":"<<s.colorSamples.width
        <<",\"height\":"<<s.colorSamples.height<<",\"samples\":"<<s.colorSamples.samples<<",\"format\":"<<s.colorSamples.format
        <<",\"content_serial\":"<<s.colorSamples.serial<<",\"bytes\":"<<s.colorSamples.rgba.size()<<",\"order\":\"pixel-sample-rgba8\""
        <<",\"provenance\":\""<<s.colorSamples.provenance<<"\"}"
        <<",\"source_depth_storage\":{\"object\":"<<s.colorSamples.depthObject<<",\"bytes\":"<<s.colorSamples.depth.size()
        <<",\"uniform\":"<<(s.colorSamples.depthUniform?"true":"false")<<",\"order\":\"pixel-sample-f32le\"}";
    meta<<",\"submitted_texture_source\":\"driver-upload\",\"submitted_textures\":[";
    for(unsigned slot=0;slot<s.submittedTextures.size();++slot) {
        if(slot)meta<<',';
        const auto& fetch=s.submittedTextures[slot];
        if(!fetch){meta<<"null";continue;}
        meta<<'[';
        for(unsigned j=0;j<fetch->size();++j){if(j)meta<<',';meta<<(*fetch)[j];}
        meta<<']';
    }
    meta<<']';
    meta<<",\"mip_textures\":[";
    for(unsigned i=0;i<s.mipTextures.size();++i) {
        const auto& [slot,t]=s.mipTextures[i];if(i)meta<<',';
        meta<<"{\"slot\":"<<slot<<",\"fetch\":[";
        for(unsigned j=0;j<6;++j){if(j)meta<<',';meta<<t.fetch[j];}
        meta<<"],\"base_bytes\":"<<t.base.size()<<",\"mip_bytes\":"<<t.mips.size()
            <<",\"source\":\"owned-submitted-unpacked-storage\",\"sampling_verified\":false}";
        ok=save("mip-texture-"+std::to_string(slot)+"-base.bin",t.base) && ok;
        ok=save("mip-texture-"+std::to_string(slot)+"-mips.bin",t.mips) && ok;
    }
    meta<<']';
    meta<<",\"surfaces\":";
    if(!s.surfaces)meta<<"null";
    else {
        meta<<'[';
        for(unsigned i=0;i<s.surfaces->surfaces.size();++i) {
            const auto& surface=s.surfaces->surfaces[i];if(i)meta<<',';
            meta<<"{\"slot\":"<<i<<",\"object\":"<<surface.object
                <<",\"tile_cache_object\":"<<surface.tileCacheObject
                <<",\"header_bytes\":"<<(surface.header?surface.header->size():0)
                <<",\"tile_cache_header_bytes\":"<<(surface.tileCacheHeader?surface.tileCacheHeader->size():0)
                <<",\"argument_header_bytes\":"<<(s.surfaceEvidence.surfaces[i].argumentHeader?
                    s.surfaceEvidence.surfaces[i].argumentHeader->size():0)<<'}';
            if(surface.header)ok=save("surface-"+std::to_string(i)+"-resource.bin",*surface.header) && ok;
            if(surface.tileCacheHeader)ok=save("surface-"+std::to_string(i)+"-tile-cache.bin",*surface.tileCacheHeader) && ok;
            if(s.surfaceEvidence.surfaces[i].argumentHeader)
                ok=save("surface-"+std::to_string(i)+"-argument.bin",*s.surfaceEvidence.surfaces[i].argumentHeader) && ok;
        }
        meta<<']';
    }
    if(s.draw) {
        const auto& d=*s.draw;ok=save("vertices.bin",d.vertices) && ok;
        const auto constants=[&](const auto& words,const char* name) {
            std::vector<std::uint8_t> bytes;bytes.reserve(words.size()*4);
            for(auto word:words)for(unsigned b=0;b<4;++b)bytes.push_back(word>>(24-8*b));
            return save(name,bytes);
        };
        ok=constants(d.vertexConstants,"vertex-constants.bin") && constants(d.pixelConstants,"pixel-constants.bin") && ok;
        meta<<",\"vertex_address\":"<<d.vertexAddress<<",\"scissor\":";
        if(d.scissorRegisters)meta<<'['<<(*d.scissorRegisters)[0]<<','<<(*d.scissorRegisters)[1]<<','<<(*d.scissorRegisters)[2]<<']';
        else meta<<"null";
        meta<<",\"textures\":[";
        for(unsigned i=0;i<d.textures.size();++i) {
            const auto& t=d.textures[i];if(i)meta<<',';
            meta<<"{\"slot\":"<<t.slot<<",\"literal_zero\":"<<(t.literalZero?"true":"false")<<",\"fetch\":[";
            for(unsigned j=0;j<6;++j){if(j)meta<<',';meta<<t.fetch[j];}
            meta<<"],\"bytes\":"<<t.source.size()<<'}';
            ok=save("texture-"+std::to_string(t.slot)+".bin",t.source) && ok;
        }
        meta<<']';
    }
    meta<<",\"physical_at_stop\":";
    if(s.physicalAtStop) {
        const auto& physical=*s.physicalAtStop;
        const auto words=[&](const auto& source,const char* name) {
            std::vector<std::uint8_t> bytes;bytes.reserve(source.size()*4);
            for(auto w:source)for(unsigned b=0;b<4;++b)bytes.push_back(w>>(24-8*b));
            return save(name,bytes);
        };
        ok=words(physical.color,"physical-at-stop-color-u32be.bin") && ok;
        ok=words(physical.depth,"physical-at-stop-depth-u32be.bin") && ok;
        meta<<"{\"serial\":"<<physical.serial<<",\"width\":1280,\"height\":720,\"pitch\":1280"
            <<",\"color_base_tile\":720,\"depth_base_tile\":0,\"color_format\":3,\"depth_format\":1"
            <<",\"order\":\"row-major-u32be\",\"provenance\":\"known-physical-candidate-at-stop\""
            <<",\"draw_completion_proven\":false,\"execution_authority\":false}";
    } else meta<<"null";
    meta<<",\"raw10_textures\":[";
    for(unsigned i=0;i<s.raw10Textures.size();++i) {
        const auto& t=s.raw10Textures[i];if(i)meta<<',';
        meta<<"{\"slot\":"<<t.slot<<",\"fetch\":[";
        for(unsigned j=0;j<6;++j){if(j)meta<<',';meta<<t.fetch[j];}
        meta<<"],\"bytes\":"<<t.source.size()
            <<",\"source\":\"owned-submitted-raw10-storage\",\"complete_base_storage\":true"
            <<",\"sampling_verified\":false,\"execution_authority\":false}";
        ok=save("raw10-texture-"+std::to_string(t.slot)+".bin",t.source) && ok;
    }
    meta<<"],\"expanded16_textures\":[";
    for(unsigned i=0;i<s.expanded16Textures.size();++i) {
        const auto& t=s.expanded16Textures[i];if(i)meta<<',';
        meta<<"{\"slot\":"<<t.slot<<",\"fetch\":[";
        for(unsigned j=0;j<6;++j){if(j)meta<<',';meta<<t.fetch[j];}
        meta<<"],\"bytes\":"<<t.source.size()
            <<",\"source\":\"owned-submitted-expanded16-storage\",\"complete_base_storage\":true"
            <<",\"sampling_verified\":false,\"execution_authority\":false}";
        ok=save("expanded16-texture-"+std::to_string(t.slot)+".bin",t.source) && ok;
    }
    meta<<"]}\n";meta.close();
    return queueOk && pagesOk && bool(meta) && ok;
}

// Strong definitions replace both direct weak calls and entries in the
// generated function mapping. __imp__ remains the unmodified original.
PPC_FUNC_IMPL(__imp__sub_823B5BC8);
PPC_FUNC(sub_823B5BC8) {
    PPC_FUNC_PROLOGUE();
    const auto root=ctx.r3.u32,head=ctx.r4.u32;
    try {
        // Every original callback, mutation and return register remains intact.
        __imp__sub_823B5BC8(ctx,base);
    } catch(...) {
        // Capture only on failure, not on every dispatch/frame. The original
        // exception is always rethrown, even if this optional diagnostic fails.
        try {
            if(inlineRegistry && inlineGate)gateRef([&] {
                // MMIO/submission exceptions need the same bounded queue
                // context; do not require a prior draw/resolve/clear failure.
                if(firstUnsupportedQueue)return;
                firstUnsupportedQueue=recomp::gpu::captureGta5RenderQueue(root,head,
                    [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                    [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
                const auto& q=*firstUnsupportedQueue;
                rendererLog("RENDER_QUEUE_INVENTORY root=%08x entry=%08x records=%zu bytes=%u unavailable_payloads=%u unavailable_handlers=%u stop=%s execution_proven=0\n",
                    root,head,q.records.size(),q.payloadBytes,q.unavailablePayloads,q.unavailableHandlers,q.stop);
            });
        } catch(...) {rendererLog("%s", "RENDER_QUEUE_INVENTORY diagnostic-unavailable original-exception-preserved\n");}
        throw;
    }
}

// loading-race: begin (loading_handover.h; agents/loading-race/ROOT-CAUSE.md). CRenderPhaseScript2d's draw-list callback
// sub_82921898(r3 = count, r4 = entries): for each live entry it locks a script render target, clears it and draws the script's
// 2D items into it. Before the takeover that work reaches the loading-screen inline path, which owns the presentation image
// only (v398 replay lqntz0k2 and v459 replay 1rzo_s_w: "Native EndVertices did not complete its real draw" in the first game
// frame, ahead of the first tiled scene). Same rule as native-present-invalid and first-tiled-scene-unrecorded: take over where
// native stops. New functions only; no existing function changes (every one keeps its control flow and its PGO record).
// The first NotedCalls calls are logged (RENDER_SCRIPT_TARGETS), before and after the takeover: the callback did not run in
// any ordinary session on disk, and the generic renderer has not been seen drawing a script render target.
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
static std::atomic<std::uint32_t> scriptRenderTargetCalls{0};
[[gnu::noinline]] void scriptRenderTargetsHandOver(PPCContext& ctx,std::uint8_t* base) {
    namespace lh=recomp::loading_handover;
    const bool takenOver=genericTakeover.load(std::memory_order_acquire);
    const auto load=[&](std::uint32_t address)->std::uint32_t {return PPC_LOAD_U32(address);};
    if(const std::uint32_t call=lh::notedCall(scriptRenderTargetCalls)) {
        const auto live=lh::firstLiveEntry(ctx.r3.u32,ctx.r4.u32,load);
        rendererLog("RENDER_SCRIPT_TARGETS call=%u count=%u live=%u entry=%u target=%08x owner=%08x taken_over=%u\n",call,ctx.r3.u32,
            unsigned(live.has_value()),live?live->index:0u,live?live->target:0u,live?live->owner:0u,unsigned(takenOver));
    }
    lh::handOver(takenOver,ctx.r3.u32,ctx.r4.u32,load,
        [] {return access(RECOMP_APP_PATH("/private-trace/diag-generic-first-scene"),F_OK)==0;},
        [](const char* why) {genericTakeOver(why);},
        [&](const lh::LiveEntry& live) {
            rendererLog("RENDER_LOADING_HANDOVER site=script-render-targets count=%u entry=%u target=%08x owner=%08x lr=%08x\n",
                ctx.r3.u32,live.index,live.target,live.owner,static_cast<std::uint32_t>(ctx.lr));
        });
}
#endif
PPC_FUNC_IMPL(__imp__sub_82921898);
PPC_FUNC(sub_82921898) {
    PPC_FUNC_PROLOGUE();
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    scriptRenderTargetsHandOver(ctx,base);
#endif
    __imp__sub_82921898(ctx,base);
}
// loading-race: end

// v319 (generic_renderer.cpp): ORs the device's five dirty words into this thread's pending snapshot while diag-v319-shadow is on.
extern "C" void recomp_v319_note_commit(const std::uint8_t* device) noexcept;
#define OBSERVE_RENDER_BOUNDARY(address) \
    PPC_FUNC_IMPL(__imp__sub_##address); \
    PPC_FUNC(sub_##address) { \
        PPC_FUNC_PROLOGUE(); \
        static std::atomic_uint calls{0}; \
        const auto seen = calls.load(std::memory_order_relaxed); /* v254: no RMW once past the capture limit */ \
        const auto call = seen < CaptureLimit ? calls.fetch_add(1, std::memory_order_relaxed) : seen; \
        RagePresentScope presentScope(0x##address == 0x8251DB68); \
        [[maybe_unused]] const auto patchTicket=0x##address==0x823C3A88?beginCommandShaderPatch(ctx):std::nullopt; \
        if constexpr (0x##address == 0x822D41E8) { \
            if (call < CaptureLimit) presentedTextures[call]=ctx.r4.u32; \
            if(genericPresentHook(ctx) || presentLiveTexture(ctx.r3.u32,ctx.r4.u32)){deliverNativePresentationCallback(ctx,base);return;} \
        } \
        if constexpr (0x##address == 0x823BB848) { \
            if (call < CaptureLimit) pixelShaders[call]=ctx.r4.u32; \
        } \
        if constexpr (0x##address == 0x823C3A88) { \
            if (call < CaptureLimit) vertexPatches[call]={ctx.r3.u32,ctx.r4.u32,ctx.r7.u32,false}; \
        } \
        if constexpr (0x##address == 0x823BA608) { \
            recomp_v319_note_commit(base+ctx.r3.u32); /* v319: the dirty words before BeginVertices' commit */ \
            pendingInlineRef()={ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32}; /* v362 */ \
        } \
        if constexpr (0x##address == 0x823C1088) { \
            recomp_v319_note_commit(base+ctx.r3.u32); /* v319 */ \
            pendingInlineRef()={ctx.r3.u32,ctx.r4.u32,ctx.r6.u32,ctx.r9.u32,false,ctx.r7.u32,ctx.r8.u32,ctx.r5.u32}; /* v362 */ \
        } \
        if constexpr (0x##address == 0x823BD360) {if(captureInline(ctx.r3.u32,ctx.r1.u32,ctx.lr,ctx.r20.u32,ctx.r21.u32))return;stopPendingTileRecording("inline-draw-unavailable");} \
        if constexpr (0x##address == 0x82388A38) { \
            if(recordPendingTileResolve(ctx,[&]{__imp__sub_##address(ctx,base);}))return; \
            if(genericResolveHook(ctx))return; \
            const bool native=ownsNativeRendering(ctx.r3.u32); \
            const bool resolved=resolveLiveTarget(ctx); \
            if(native) { \
                if(!resolved)recordDiagnosticSkippedResolve(ctx); \
                return; \
            } \
        } \
        record(#address, call, "enter", ctx); \
        __imp__sub_##address(ctx, base); \
        if constexpr (0x##address == 0x823C3A88) { \
            endCommandShaderPatch(patchTicket); \
            if (call < CaptureLimit) vertexPatches[call].returned=true; \
        } \
        record(#address, call, "return", ctx); \
    }

OBSERVE_RENDER_BOUNDARY(824325A0) // RAGE frame end; resolve then rotating present.
OBSERVE_RENDER_BOUNDARY(8251DB68) // RAGE present; advances backbuffer index mod 3.
OBSERVE_RENDER_BOUNDARY(822D41E8) // Driver present; sole direct VdSwap caller.
OBSERVE_RENDER_BOUNDARY(82388A38) // Resolve argument adapter used by frame end.
OBSERVE_RENDER_BOUNDARY(823C2738) // Resolve implementation; semantics under study.
OBSERVE_RENDER_BOUNDARY(822FC4D0) // Indirect RAGE frame-end caller: recover its LR.
OBSERVE_RENDER_BOUNDARY(823B9768) // Vertex-stream resource binding candidate.
OBSERVE_RENDER_BOUNDARY(823B9A00) // Index-buffer resource binding candidate.
OBSERVE_RENDER_BOUNDARY(823BA608) // Inline vertex draw setup candidate.
OBSERVE_RENDER_BOUNDARY(823C1088) // Inline indexed draw setup candidate.
OBSERVE_RENDER_BOUNDARY(823BD360) // EndVertices command publication.
OBSERVE_RENDER_BOUNDARY(823BB848) // Pixel shader resource binding.
OBSERVE_RENDER_BOUNDARY(823BBA08) // Vertex shader resource binding.
OBSERVE_RENDER_BOUNDARY(823C3A88) // Effective vertex microcode patched in copied command storage.

#undef OBSERVE_RENDER_BOUNDARY

// Attempt v10 probe: the title's native registration (r3 = Jenkins hash of the lowercase name,
// r4 = handler), recorded before the original runs so every later script call can be named.
extern "C" void recomp_native_probe_register(std::uint32_t hash,std::uint32_t handler);
PPC_FUNC_IMPL(__imp__sub_83438FE8);
PPC_FUNC(sub_83438FE8) {
    PPC_FUNC_PROLOGUE();
    recomp_native_probe_register(ctx.r3.u32,ctx.r4.u32);
    __imp__sub_83438FE8(ctx,base);
}

// world-load (world_load.h; the user's optional sliders, default = the original game): strong hooks of the game's own two
// per-frame multiplier resets (update list #33 sub_8223F728 and #34 sub_82533758). With every slider at the original the
// helpers return after one load: no guest access, the originals run unchanged. NEEDS the link's redirect script without
// these two wrappers (world-load/build/redirect-v458-wl.ld), or the hooks are silently bypassed (RENDER_DIAG_WL hook=0).
#include "world_load.h"
#include "world_load_hooks.inc"

// occlusion package (occlusion_queries.h, diag-oc-queries): the ZPD event an occlusion query's Issue emits. Marked before
// the original writes its PM4 (so no ring walk can meet the event before its marker), in D3D call order; voided after it
// when the query was issued while tiling (word q+148 > 1: one sample block per tile, v436's fabricated path). Type 8
// (event) and 10 (screen extent) queries write no ZPD event. Out of line, noexcept: this hook's CFG gains no unwind edge.
[[gnu::noinline]] static std::uint64_t ocqBeforeIssue(std::uint8_t* base,std::uint32_t query,std::uint32_t flags) noexcept {
    if(!recomp::gpu::ocq::enabled.load(std::memory_order_relaxed) || !(flags&3u) || !query)return 0;
    const auto be=[&](std::uint32_t a){std::uint32_t v;std::memcpy(&v,base+a,4);return __builtin_bswap32(v);};
    if(be(query+4)!=9)return 0;
    try {return recomp::gpu::ocq::engine().mark(recomp::gpu::ocq::physicalOf(be(query+28)),(flags&2u)!=0,recomp::gpu::generic::ocqStamp());}
    catch(...) {return 0;}
}
[[gnu::noinline]] static void ocqAfterIssue(std::uint8_t* base,std::uint32_t query,std::uint64_t id) noexcept {
    if(!id)return;
    const auto be=[&](std::uint32_t a){std::uint32_t v;std::memcpy(&v,base+a,4);return __builtin_bswap32(v);};
    const std::uint32_t tiles=be(query+148);
    if(tiles<=1u)return;
    // the last tile's block (D3D's word q + 24 + 4 * tiles, the one GetData checks)
    try{recomp::gpu::ocq::engine().voidMarker(id,tiles<=16u?recomp::gpu::ocq::physicalOf(be(query+24+4*tiles)):0u);}catch(...){}
}
PPC_FUNC_IMPL(__imp__sub_82210888);
PPC_FUNC(sub_82210888) {
    PPC_FUNC_PROLOGUE();
    const std::uint32_t ocqQuery=ctx.r3.u32; // occlusion package
    const auto ocqMarker=ocqBeforeIssue(base,ocqQuery,ctx.r4.u32);
    // Observe the exact known BEGIN caller, without replacing original query
    // execution or emitting/consuming any commands. Separate read-only gates
    // are intentional: the snapshots are diagnostics, NOT an atomic lease.
    std::optional<recomp::gpu::Gta5QueryCommandTrace> trace;
    const auto endedQuery=ctx.r4.u32==1?ctx.r3.u32:0u;
    if(ctx.r4.u32==2 && ctx.r21.u32==0x83817028 &&
       (ctx.lr==0x82364A34 || ctx.lr==0x82364A74) && inlineRegistry && inlineGate)try {
        const bool gated=gateRef([&] {
            latestQueryCommandTrace.reset();
            trace=recomp::gpu::Gta5QueryCommandTrace::begin(ctx.r21.u32,ctx.r3.u32,ctx.lr,
                [&](auto a,auto n){return inlineRegistry->copySerializedTitleImage(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);});
        });
        if(!gated)trace.reset();
    } catch(...) {trace.reset();}
    __imp__sub_82210888(ctx,base); // Original exceptions remain original exceptions.
    ocqAfterIssue(base,ocqQuery,ocqMarker); // occlusion package
    if(endedQuery && inlineRegistry && inlineGate)try {
        (void)gateRef([&] {
            if(latestQueryCommandTrace && latestQueryCommandTrace->query==endedQuery) {
                latestQueryCommandTrace->ended(
                    [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                    [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
                const bool retained=recomp::gpu::retainGta5QueryEndDiagnostic(*latestQueryCommandTrace,retainedQueryEndTrace);
                static std::atomic<unsigned long long> queryEndLines{0}; // v218: sampled (diag_log_sample.h)
                if(!retained || recomp::diag::sampled(queryEndLines)) {
                    rendererLogSampled(!retained,"RENDER_QUERY_END_DIAGNOSTIC_RETAINED retained=%u teardown_safe=1 execution_authority=0\n",unsigned(retained));
                    rendererLogSampled(!retained,"RENDER_QUERY_ORIGINAL_END query=%08x status=%s bytes=%zu query_completion=0 retirement=0\n",
                        endedQuery,latestQueryCommandTrace->endStatus,latestQueryCommandTrace->commandsThroughEnd.size());
                }
            }
        });
    } catch(...) {} // Read-only observation never acknowledges the original work.
    if(trace && inlineRegistry && inlineGate)try {
        (void)gateRef([&] {
            trace->returned([&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
            latestQueryCommandTrace=std::move(trace);
        });
    } catch(...) {} // A diagnostic allocation failure cannot acknowledge work.
}

PPC_FUNC_IMPL(__imp__sub_823B9B58);
PPC_FUNC(sub_823B9B58) {
    PPC_FUNC_PROLOGUE();
    static std::atomic_uint calls{0};
    const auto call=calls.fetch_add(1,std::memory_order_relaxed);
    const auto device=ctx.r3.u32,caller=static_cast<std::uint32_t>(ctx.lr);
    const recomp::gpu::Gta5BoundIndexedRequest request{ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32};
    const bool native=ownsNativeRendering(device);
    if(native)recomp_v319_note_commit(base+device); // v319: the dirty words before DrawIndexedVertices' commit
    record("823B9B58",call,"enter",ctx);
    // Without the qualified geometry pack, original emission is unchanged.
    // With it, preparation and the epilogue remain original; a fenced native
    // draw replaces only raw emission. Preserve failed-boundary inputs too.
    try {
#if (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_bound_geometry_shader_pack.h") && __has_include("native_prepared_indexed_original.h")
        nativePreparedIndexedOriginal(ctx,base);
#else
        __imp__sub_823B9B58(ctx,base);
#endif
    } catch(...) {
        rendererLog("RENDER_BOUND_INDEXED_EXCEPTION device=%08x caller=%08x count=%u returned=0\n",device,caller,request.indexCount);
        if(native && !finishPreparedRecordedIndexed(false))captureReturnedBoundIndexed(device,caller,request,BoundIndexedCapturePhase::Exception);
        throw;
    }
    if(native && !finishPreparedRecordedIndexed(true))captureReturnedBoundIndexed(device,caller,request,BoundIndexedCapturePhase::Returned);
    record("823B9B58",call,"return",ctx);
}

// v56: D3D DrawVertices: the original first (its PM4, tile recording and fences unchanged), then the
// generic renderer draws it from the device state the original used.
PPC_FUNC_IMPL(__imp__sub_823CE5A0);
PPC_FUNC(sub_823CE5A0) {
    PPC_FUNC_PROLOGUE();
    static std::atomic_uint calls{0};
    const auto call=calls.fetch_add(1,std::memory_order_relaxed);
    const auto device=ctx.r3.u32,primitive=ctx.r4.u32,start=ctx.r5.u32,count=ctx.r6.u32;
    record("823CE5A0",call,"enter",ctx);
    recomp_v319_note_commit(base+device); // v319: the dirty words before DrawVertices' commit
    __imp__sub_823CE5A0(ctx,base);
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    genericDrawVertices(device,primitive,start,count);
#else
    (void)device;(void)primitive;(void)start;(void)count;
#endif
    record("823CE5A0",call,"return",ctx);
}

// v79: D3DDevice_SetGammaRamp (836AFAF8: device r3, flags r4, D3DGAMMARAMP r5). The original converts the
// ramp for the display's gamma and keeps it at device+15408, which the next swap (822D41E8 -> 836B5480 ->
// 836B8668) writes to the scan-out LUT. The generic present replaces that swap (the 822D41E8 hook returns
// before the original), so the renderer takes the converted ramp here and maps presented pixels through it.
// Callers: D3D's device setup (836BA400, the identity from 836B8888) and GTA's 82989588 (the static table
// at 837CB088).
PPC_FUNC_IMPL(__imp__sub_836AFAF8);
PPC_FUNC(sub_836AFAF8) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32;const auto caller=std::uint32_t(ctx.lr);
    __imp__sub_836AFAF8(ctx,base);
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    if(device && std::uint64_t{device}+15408+1536<=0x100000000ull)
        recomp::gpu::generic::setGammaRamp(recomp::gpu::generic::post::packD3DRamp(base+device+15408),caller);
#else
    (void)device;(void)caller;
#endif
}

// Bounded transition evidence after the first real UI draw. No guest writes.
void traceUiTextureBinding(const char* phase,std::uint32_t caller,unsigned slot,std::uint32_t resource) {
    // v312: the bound is read before the increment (the same 64 lines; no atomic RMW per call once it is reached)
    if(textureBindingDiagnostics.load(std::memory_order_relaxed)>=64)return;
    const auto device=nativeUiDevice.load();
    if(!device || slot>=32 || textureBindingDiagnostics.fetch_add(1)>=64 || !inlineGate || !inlineRegistry)return;
    gateRef([&] {
        const auto word=[&](std::uint32_t at) {
            const auto bytes=inlineRegistry->copySerializedGuestHeap(at,4);
            if(!bytes)return std::uint32_t{0xFFFFFFFF};
            return (std::uint32_t{(*bytes)[0]}<<24)|(std::uint32_t{(*bytes)[1]}<<16)|
                (std::uint32_t{(*bytes)[2]}<<8)|(*bytes)[3];
        };
        rendererLog("RENDER_UI_BINDING phase=%s caller=%08x slot=%u resource=%08x header0=%08x resource_fetch0=%08x bound=%08x shadow0=%08x\n",
            phase,caller,slot,resource,resource?word(resource):0,resource?word(resource+28):0,
            word(device+12920+slot*4),word(device+1152+slot*24));
    });
}
PPC_FUNC_IMPL(__imp__sub_823B8B70);
PPC_FUNC(sub_823B8B70) {
    PPC_FUNC_PROLOGUE();
    const auto caller=static_cast<std::uint32_t>(ctx.lr),slot=ctx.r4.u32,resource=ctx.r5.u32;
    traceUiTextureBinding("set-enter",caller,slot,resource);
    __imp__sub_823B8B70(ctx,base);
    traceUiTextureBinding("set-return",caller,slot,resource);
}
PPC_FUNC_IMPL(__imp__sub_823B8818);
PPC_FUNC(sub_823B8818) {
    PPC_FUNC_PROLOGUE();
    const auto slot=ctx.r3.u32,resource=ctx.r4.u32,device=nativeUiDevice.load();
    // v254: the body below changes something only when the fast-path cache slot names `resource`; a cheap guest read
    // first skips the gate and two views otherwise (the same read the body makes, one step earlier).
    constexpr std::uint32_t V254Cache=0x83BFB400;
    if(device && slot<32 && resource && PPC_LOAD_U32(V254Cache+slot*4)==resource && ownsNativeRendering(device)) {
        gateRef([&] {
            // 830CE180 explicitly unbinds the atlas through the lower driver
            // API. RAGE's separate fast-path cache may still name that atlas.
            // Invalidate ONLY a contradicted cache hit, so original 823B8818
            // rebinds its own requested object through original 823B8B70.
            // No valid-type forcing, old texture substitution, or GPU receipt.
            constexpr std::uint32_t Cache=0x83BFB400,DeviceGlobal=0x83BFB3EC;
            // v43: the copies' validation in place; the header is still snapshotted (52 bytes, on the stack).
            const auto* bytes=inlineRegistry->viewSerializedGuestHeap(device+12920+slot*4,4);
            const auto* headerView=inlineRegistry->viewSerializedGuestHeap(resource,52);
            std::array<std::uint8_t,52> header{};
            if(headerView)std::memcpy(header.data(),headerView,52);
            if(PPC_LOAD_U32(DeviceGlobal)!=device || !bytes || !headerView ||
               !recomp::gpu::describeGta5TextureHeader(header.data(),header.size()))return;
            const auto bound=recomp::gpu::hot::be32(bytes);
            const auto cached=PPC_LOAD_U32(Cache+slot*4);
            if(cached==resource && bound!=resource) {
                PPC_STORE_U32(Cache+slot*4,0);
                rendererLog("RENDER_UI_CACHE_INVALIDATED slot=%u requested=%08x cached=%08x actual=%08x\n",
                    slot,resource,cached,bound);
            }
        });
    }
    __imp__sub_823B8818(ctx,base);
}
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
// v86 (HUD/UI, ui_texture_tracking.h): after the original unlock (sub_836B0840 -> 8224AC08) the CPU's writes into
// the texture are complete. GTA V's Scaleform glyph cache (a 1024x1024 k_8 linear texture, object 10271080 in the
// v70 logs) is rewritten this way whenever glyphs are rasterised; the generic renderer re-reads any cached texture
// over the unlocked texture's base and mip bytes at its next use (files/private-trace/diag-ui-no-unlock-invalidate:
// v71, no re-read). Reads only the 52-byte object header, under the gate as traceUiTextureBinding does.
void genericTextureUnlocked(std::uint32_t resource) {
    // (No generic::available(): it takes the renderer mutex, which a present holds through fence waits; the
    // queue is bounded and harmless without a renderer.)
    if(!resource || !inlineRegistry || !inlineGate || !genericTakeover.load(std::memory_order_acquire))return;
    std::optional<std::array<std::uint32_t,6>> fetch;
    gateRef([&] {
        if(const auto* header=inlineRegistry->viewSerializedGuestHeap(resource,52))
            if(const auto d=recomp::gpu::describeGta5TextureHeader(header,52))fetch=d->fetch;
    });
    if(fetch)recomp::gpu::generic::invalidateTextureFetch(*fetch);
}
#endif
PPC_FUNC_IMPL(__imp__sub_836B0840);
PPC_FUNC(sub_836B0840) {
    PPC_FUNC_PROLOGUE();
    const auto caller=static_cast<std::uint32_t>(ctx.lr),resource=ctx.r3.u32;
    traceUiTextureBinding("unlock-enter",caller,2,resource);
    __imp__sub_836B0840(ctx,base);
    traceUiTextureBinding("unlock-return",caller,2,resource);
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
    genericTextureUnlocked(resource);
#endif
}

PPC_FUNC_IMPL(__imp__sub_8232E278);
PPC_FUNC(sub_8232E278) {
    PPC_FUNC_PROLOGUE();
    const auto device=tileRecordingDevice.load();
    if(device && ctx.r3.u32==device) {
        if(!inlineRegistry || !originalRecordingGate)
            throw std::runtime_error("Ordinary tile producer lacks its serialized owner");
        // activeEndQueue can exist only inside the already-admitted whole
        // EndTiling executor scope. Borrow THAT scope for this original lock
        // pair; do not attempt or permit a nested executor non-yielding scope.
        const auto result=recomp::bootstrap::invokeNativeTileLockCall(*inlineRegistry,
            activeEndQueue?recomp::bootstrap::GuestImportRegistry::NativeSerializedGate{[](const auto& f){f();return true;}}:originalRecordingGate,
            device,recomp::bootstrap::NativeTileLockCall::PendingProducer,
            [&]{__imp__sub_8232E278(ctx,base);});
        if(result==recomp::bootstrap::NativeTileLockCallResult::Returned)return;
        if(result==recomp::bootstrap::NativeTileLockCallResult::Refused)
            throw std::runtime_error("Ordinary tile producer lock/profile refused; no completion");
    }
    __imp__sub_8232E278(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_836BE058);
PPC_FUNC(sub_836BE058) {
    PPC_FUNC_PROLOGUE();
    const auto device=tileRecordingDevice.load();
    if(device && std::uint64_t{device}+11080==ctx.r3.u32) {
        if(!inlineRegistry || !originalRecordingGate ||
           recomp::bootstrap::invokeNativeTileLockCall(*inlineRegistry,originalRecordingGate,device,
               recomp::bootstrap::NativeTileLockCall::OrdinaryWorker,
               [&]{__imp__sub_836BE058(ctx,base);})!=recomp::bootstrap::NativeTileLockCallResult::Returned)
            throw std::runtime_error("Ordinary tile worker lock/profile refused; no completion");
        return;
    }
    __imp__sub_836BE058(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_8232DFA8);
PPC_FUNC(sub_8232DFA8) {
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
    if(submitLiveNativeScene(ctx.r3.u32,[&]{__imp__sub_8232DFA8(ctx,base);}))return;
#endif
    // During original EndTiling, the outer exclusive scope routes real MMIO
    // into the bounded non-completing queue. All other raw submissions stop.
    if(!activeEndQueue)stopPendingTileRecording("raw-submission");
    const auto caller=static_cast<std::uint32_t>(ctx.lr),device=ctx.r3.u32;
    try {__imp__sub_8232DFA8(ctx,base);}
    catch(...) {
        rendererLog("RENDER_SUBMISSION_STOP caller=%08x device=%08x\n",caller,device);
#if (defined(__ANDROID__) || defined(__APPLE__))
        // One bounded caller backtrace on failure, not per-frame tracing.
        // Library-relative PCs identify the API that still submitted work.
        unsigned frames=0;
        _Unwind_Backtrace([](_Unwind_Context* unwind,void* user) {
            auto& n=*static_cast<unsigned*>(user);
            const auto pc=static_cast<std::uintptr_t>(_Unwind_GetIP(unwind));
            Dl_info info{};
            if(pc && dladdr(reinterpret_cast<void*>(pc),&info) && info.dli_fbase)
                rendererLog("RENDER_SUBMISSION_CALLER frame=%u offset=%llx\n",n,
                    static_cast<unsigned long long>(pc-reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
            return ++n<18?_URC_NO_REASON:_URC_END_OF_STACK;
        },&frames);
#endif
        throw;
    }
}

PPC_FUNC_IMPL(__imp__sub_836B1570);
PPC_FUNC(sub_836B1570) {
    PPC_FUNC_PROLOGUE();
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__)) && __has_include("native_logical_geometry_shader_pack.h")
    if(beginLiveSceneSelect(ctx.r3.u32,[&]{__imp__sub_836B1570(ctx,base);}))return;
#endif
    __imp__sub_836B1570(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_836B1B90);
PPC_FUNC(sub_836B1B90) {
    PPC_FUNC_PROLOGUE();
    stopPendingTileRecording("driver-drain");
    // 824F3030 drains before changing the Present mode flags. Keep that CPU
    // mode-setting code, replacing only its drain with completed native work.
    if(static_cast<std::uint32_t>(ctx.lr)==0x824F3050 && completeNativePresentDrain(ctx.r3.u32))return;
    __imp__sub_836B1B90(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_82476080);
PPC_FUNC(sub_82476080) {
    PPC_FUNC_PROLOGUE();
    capturePendingTileEnd(ctx);
#if defined(RECOMP_CAPTURE_TILED_RECORDING) && (defined(__ANDROID__) || defined(__APPLE__))
    if(tileRecordingDevice.load() && ctx.r3.u32==tileRecordingDevice.load() && inlineRegistry && originalEndRecordingGate &&
       stoppedTileEnd && stoppedTileEnd->liveVideoRootMatches==std::optional{true} &&
       pendingNativeSceneInputs && pendingNativeSceneResolves && pendingNativeSceneStorage && pendingNativeSceneCommands &&
       !stoppedEndQueue && !activeEndQueue && !ctx.r5.u32 && !ctx.r6.u32) {
        const auto device=ctx.r3.u32;
        const auto stack=ctx.r1.u64;
        const auto pcr=ctx.r13.u64;
        const auto lr=ctx.lr;
        bool entered=false;
        const bool gated=originalEndRecordingGate([&] {
            if(inlineRegistry->snapshotNativeInterruptDeviceRoot(device)!=std::optional{true})return;
            stoppedEndQueue=recomp::gpu::Gta5NativeTileSubmissionCapture::begin(device,
                [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);});
            if(!stoppedEndQueue)return;
            activeEndQueue=&*stoppedEndQueue;entered=true;
            struct Clear {~Clear(){activeEndQueue=nullptr;}} clear;
            __imp__sub_82476080(ctx,base);stoppedEndCpuReturned=true;
            stoppedEndCpuState=inlineRegistry->copySerializedGuestHeap(device,21944);
            if(ctx.r1.u64!=stack || ctx.r13.u64!=pcr || ctx.lr!=lr || !stoppedEndCpuState ||
               !activeEndQueue->seal([&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                    [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);}))
                throw std::runtime_error("Original EndTiling CPU return/queued-byte validation failed; no GPU completion");
        });
        if(entered) {
            if(!gated || !stoppedEndCpuReturned || !endRestoredScissor ||
               !captureDriverScissor(device,*endRestoredScissor,0x823ABBC8))
                throw std::runtime_error("Original EndTiling scissor/recording scope did not complete");
            rendererLog("RENDER_END_ORIGINAL_CPU_RETURN device=%08x submissions=%zu read=%u write=%u original_executed=1 gpu_completed=0 guest_published=0\n",
                device,stoppedEndQueue->submissions().size(),stoppedEndQueue->entryReadWord(),stoppedEndQueue->lastWriteWord());
            if(inlineGraphics && nativeSceneInterrupt) {
                stoppedInitialTileQueue=recomp::gpu::Gta5NativeTileInitialQueue::prepare(*stoppedEndQueue);
                if(stoppedInitialTileQueue) {
#if __has_include("native_logical_geometry_shader_pack.h")
                    if(!liveNativeScene && nativeSceneContinuation && nativeSceneScheduler.currentToken &&
                       nativeSceneScheduler.parked && nativeSceneScheduler.wait && liveSceneClearPrefix()) {
                        const auto issuer=nativeSceneScheduler.currentToken();
                        if(issuer){liveNativeScene.emplace();liveNativeScene->device=device;liveNativeScene->issuer=issuer;}
                    }
#endif
                    completedNativeSceneTarget.reset(); // A new recording owns a new target generation.
                    liveEdram32[device].reset(); // Tiled replay can replace physical colour AND depth.
                    pendingNativeSceneTarget=inlineGraphics->createLogicalTileTarget(tileRecordingDiagnostic.seed());
                    if(!pendingNativeSceneTarget || !stoppedInitialTileQueue->execute(*inlineGraphics,*pendingNativeSceneTarget,
                        [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                        [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
                        [&](unsigned d,const auto& irq) {
#if __has_include("native_logical_geometry_shader_pack.h")
                            if(liveNativeScene) {
                                std::optional<unsigned> waiter;
                                if(!nativeSceneScheduler.wait([&]{
                                    waiter=inlineRegistry->nativeTileSceneWaitingWorker(d);
                                    return waiter && nativeSceneScheduler.parked(*waiter);
                                }) || !waiter)return false;
                                liveNativeScene->worker=*waiter;
                            }
#endif
                            return nativeSceneInterrupt(d,irq);
                        }))
                        throw std::runtime_error("Initial tiled cache/IRQ execution failed; scene/ring not completed");
                    if(!inlineRegistry->retireNativeTileInitialQueue(*stoppedInitialTileQueue,originalRecordingGate))
                        throw std::runtime_error("Initial tiled queue retirement rejected; scene not completed");
                    stoppedInitialRingRetired=true;
                    rendererLog("RENDER_INITIAL_TILE_PACKETS device=%08x cache_completed=1 original_irq_returned=1 initial_ring_retired=1 scene_completed=0\n",device);
#if __has_include("native_logical_geometry_shader_pack.h")
                    if(liveNativeScene && completeLiveNativeScene()) {
                        // Both original scene/deferred submissions really completed,
                        // and their original producer returned to its wait loop.
                        // Preserve the completed target before releasing CPU recording
                        // state: later maintenance uses this same native image owner.
                        if(!handoffRendererCompletedSceneTarget())
                            throw std::runtime_error("Completed scene target handoff rejected");
                        rendererLog("RENDER_LIVE_END_TILING_RETURN device=%08x scene_passes=2 original_worker_parked=1 resources_published=1 final_ring_retired=1 gameplay_unverified=1\n",device);
                        pendingNativeSceneInputs.reset();pendingNativeSceneResolves.reset();pendingNativeSceneStorage.reset();pendingNativeSceneCommands.reset();
                        tileRecordingDevice.store(0);tileRecordingDiagnostic={};
                        stoppedTileEnd.reset();stoppedTileResolve.reset();recordedTileResolves.clear();
                        stoppedInitialTileQueue.reset();stoppedEndQueue.reset();stoppedEndCpuState.reset();
                        stoppedEndCpuReturned=false;stoppedInitialRingRetired=false;endRestoredScissor.reset();
                        return;
                    }
#endif
                }
            }
            // Initial cache/IRQ packets can now execute and notify the real
            // waiter, retiring only these two entries. Its later scene draws,
            // resource publication and scene-ring retirement need a consumer.
            stopPendingTileRecording("end-tiling-native-completion-required");
        }
    }
#endif
    stopPendingTileRecording("end-tiling");
    __imp__sub_82476080(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_836B1800);
PPC_FUNC(sub_836B1800) {
    PPC_FUNC_PROLOGUE();
    // Original: select the current device and wait on object+8's fence ID.
    // Native resources instead wait for the native uses of their own storage.
    if(completeNativeResourceWait(ctx.r3.u32))return;
    __imp__sub_836B1800(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_824FD8C8);
PPC_FUNC(sub_824FD8C8) {
    PPC_FUNC_PROLOGUE();
    // Texture Lock's shared resource mapper has a second wait path, separate
    // from BlockUntilNotBusy. Retain its mapping, lock count and output logic.
    // Scope the resource through the original call even when its internal
    // wait receives a fence token instead of a resource pointer.
    struct Scope {std::uint32_t prior;~Scope(){lockingTexture=prior;}} scope{lockingTexture};
    if(static_cast<std::uint32_t>(ctx.lr)==0x836B0BF8)lockingTexture=ctx.r3.u32;
    else lockingTexture=0;
    traceUiTextureBinding("lock-enter",static_cast<std::uint32_t>(ctx.lr),2,lockingTexture);
    __imp__sub_824FD8C8(ctx,base);
    traceUiTextureBinding("lock-return",0,2,lockingTexture);
}
PPC_FUNC_IMPL(__imp__sub_8250D988);
PPC_FUNC(sub_8250D988) {
    PPC_FUNC_PROLOGUE();
    stopPendingTileRecording("fence-wait");
    if(static_cast<std::uint32_t>(ctx.lr)==0x824FDA18 && lockingTexture &&
       ownsNativeRendering(ctx.r3.u32) && completeNativeResourceWait(lockingTexture)) {
        // The actual native sampler/resolve operation has already completed
        // its Vulkan fence. No Xbox completion counter or return value changes.
        rendererLog("RENDER_API_TEXTURE_LOCK_WAIT object=%08x device=%08x fence=%08x completed=1\n",
            lockingTexture,ctx.r3.u32,ctx.r4.u32);
        return;
    }
    __imp__sub_8250D988(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_8251DC10);
PPC_FUNC(sub_8251DC10) {
    PPC_FUNC_PROLOGUE();
    if(nativePresentStatistics(ctx,base))return;
    __imp__sub_8251DC10(ctx,base);
}

PPC_FUNC_IMPL(__imp__sub_823D89A8);
PPC_FUNC(sub_823D89A8) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32,flags=ctx.r4.u32,rect=ctx.r5.u32,color=ctx.r6.u32;
    const recomp::gpu::Gta5ClearCall call{ctx.r1.u32,device,flags,rect,color,ctx.r8.u32,
        static_cast<std::uint32_t>(ctx.lr),ctx.f1.u64};
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(tileRecordingDevice.load()) {
            bool recorded=false;
            if(!inlineRegistry || !inlineGate || !gateRef([&] {
                const auto capture=recomp::gpu::captureGta5ClearCall(call,
                    [&](auto a,auto n){return inlineRegistry->copySerializedGuestHeap(a,n);},
                    [&](auto a,auto n){return inlineRegistry->copySerializedGraphicsData(a,n);},
                    [&](auto a,auto n){return inlineRegistry->copySerializedGuestArgument(a,n);});
                recorded=tileRecordingDiagnostic.recordClear(capture);
                if(!recorded && !firstUnsupportedClear)firstUnsupportedClear=capture;
            }) || !recorded)stopPendingTileRecording("unsupported-recorded-clear");
            rendererLog("RENDER_TILE_CLEAR_RECORDED device=%08x flags=%08x gpu_completed=0 guest_published=0\n",device,flags);
            return; // Record-only API return, NOT executed clear or completion.
    }
#endif
    if(ownsNativeRendering(device)) {
#if defined(RECOMP_DIAGNOSTIC_GENERIC_RENDER)
        if(genericReady()){genericClear(device,flags,rect,color,ctx.f1.f64,ctx.r8.u32);return;}
#endif
        // Native Clear preserves bound draw state. The original temporarily
        // clobbers GPU shaders/registers, then marks CPU shadows dirty for their
        // restoration; those hardware-only transitions are unnecessary here.
        // Pending application state remains in the authoritative CPU shadows.
        bool diagnosticLiveValid=false;
        if(inlineGate)gateRef([&]{const auto it=liveTargets.find(device);diagnosticLiveValid=it!=liveTargets.end() && it->second.valid;});
        if(!clearLiveTarget(device,flags,rect,color,&call)) {
            recordDiagnosticSkippedClear(device,flags,rect,color,static_cast<std::uint32_t>(ctx.lr));
            if(diagnosticLiveValid && inlineGate)gateRef([&]{const auto it=liveTargets.find(device);if(it!=liveTargets.end())it->second.valid=true;});
        }
        return;
    }
    __imp__sub_823D89A8(ctx,base);
    // The original flushes dirty surface state before emitting the clear. Its
    // caller-owned rectangle/color arguments survive this call. Read the
    // effective shadow after that flush, before any following draw can run.
    clearLiveTarget(device,flags,rect,color);
}

PPC_FUNC_IMPL(__imp__sub_823B9FD0);
PPC_FUNC(sub_823B9FD0) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32,first=ctx.r5.u32,source=ctx.r6.u32;
    const auto mask=ctx.r4.u64;
    if(ownsNativeRendering(device)) {
        // Ordinary uploads and shader literals share one ordered native cache.
        // Original 823B9FD0 only copies these blocks into command storage.
        if(!captureDriverConstants(device,mask,first,source,false))
            throw std::runtime_error("Native shader constant upload is unsupported or unowned");
        return;
    }
    __imp__sub_823B9FD0(ctx,base);
    captureDriverConstants(device,mask,first,source,false);
}
PPC_FUNC_IMPL(__imp__sub_823BA1A8);
extern "C" std::atomic<bool> recomp_v274_baseline; std::atomic<bool> recomp_v274_baseline{false}; // v274 (set with the renderer's A/B windows)
PPC_FUNC(sub_823BA1A8) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32;
    bool captured=false;
    const bool gated=inlineRegistry && inlineGate && gateRef([&] {
        // v43: every set slot's fetch words (device+1152+slot*24) lie in one span from the first to the last
        // set slot; validate it once and serve the slots from it. A span that does not validate as a whole
        // leaves every slot to its own read, as before. (Heap validity is monotone: a range inside a valid
        // one is valid.)
        const std::uint64_t mask=ctx.r4.u64;
        const std::uint8_t* span=nullptr;std::uint32_t spanBase=0,spanBytes=0;
        if(device && mask && !(mask&0xFFFFFFFFULL)) {
            const unsigned first=static_cast<unsigned>(std::countl_zero(mask)),last=63u-static_cast<unsigned>(std::countr_zero(mask));
            const auto base=std::uint64_t{device}+1152+first*24,end=std::uint64_t{device}+1152+(last+1)*24;
            // v362 (exact, diag-v362-render-trims): a single set slot is served from its own validated 24-byte span too (v349:
            // a copySerializedGuestHeap vector per single-slot upload, ~0.6-0.8% of the render thread with its malloc/free).
            // The same validation (the device-shadow view of v335, else viewSerializedGuestHeap) over the same bytes; a
            // refused span leaves the slot to its own read, as before. Only RENDER_DIAG_HOT's span/slot-read counts move.
            if((last>first || (last==first && recomp::v362::trimsOn())) && end<=0x100000000ULL) {
                spanBase=static_cast<std::uint32_t>(base);spanBytes=static_cast<std::uint32_t>(end-base);
                span=deviceRange(device,spanBase,spanBytes); // (v335)
            }
        }
        if(!recomp_v274_baseline.load(std::memory_order_relaxed)) { // v274 (A/B: diag-v274-baseline = the per-slot copies)
            if(span)++hot.textureSpans;
            captured=driverTextures[device].uploadFromSpan(device,mask,span,spanBase,spanBytes,
                [&](std::uint32_t a,std::uint32_t n)->std::optional<std::vector<std::uint8_t>> {
                    ++hot.textureSlotReads;return inlineRegistry->copySerializedGuestHeap(a,n);},recomp::v362::trimsOn()); // (v362: staged)
        } else
        captured=driverTextures[device].upload(device,mask,
            [&](std::uint32_t a,std::uint32_t n)->std::optional<std::vector<std::uint8_t>> {
                if(span && a>=spanBase && std::uint64_t{a}+n<=std::uint64_t{spanBase}+spanBytes) {
                    ++hot.textureSpans;return std::vector<std::uint8_t>(span+(a-spanBase),span+(a-spanBase)+n);
                }
                ++hot.textureSlotReads;return inlineRegistry->copySerializedGuestHeap(a,n);
            });
    });
    if(ownsNativeRendering(device)) {
        // Preserve the actual submitted bindings, not the mutable CPU shadow.
        // EndVertices still owns fresh texture bytes for each native draw.
        if(!gated || !captured)
            throw std::runtime_error("Native texture binding upload is unmapped or unowned");
        return;
    }
    __imp__sub_823BA1A8(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_823BA300);
PPC_FUNC(sub_823BA300) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32,first=ctx.r5.u32,source=ctx.r6.u32;
#if defined(RECOMP_CAPTURE_TILED_RECORDING)
    if(const auto recording=originalResolveRecordingDeviceRef()) { // v362
        if(device!=recording)stopPendingTileRecording("recorded-resolve-upload-owner");
        __imp__sub_823BA300(ctx,base);return;
    }
#endif
    if(ownsNativeRendering(device)) {
        // Original only emits register packets and advances device+48. The
        // caller clears dirty flags; native EndVertices reads the same shadow.
        const auto bytes=recomp::gpu::gta5DrawStateUploadBytes(device,ctx.r4.u64,first,source);
        bool owned=false;
        // v329b (exact): the view is thread-safe by itself (a validated snapshot, else under the registry reader lock) and
        // the body touches nothing else, so the global lock gate adds no exclusion here (diag-v329b-baseline = gated).
        if(bytes && inlineRegistry && !v329bBaseline())owned=inlineRegistry->viewSerializedGuestHeap(source,*bytes)!=nullptr;
        else if(!bytes || !gateRef([&] {
            // v43: the copy's validation without the copy (v35 discarded the bytes).
            owned=inlineRegistry->viewSerializedGuestHeap(source,*bytes)!=nullptr;
        }) || !owned) owned=false;
        if(!owned) {
            rendererLog("RENDER_STATE_UPLOAD unsupported device=%08x first=%08x source=%08x mask=%016llx\n",
                device,first,source,static_cast<unsigned long long>(ctx.r4.u64));
            throw std::runtime_error("Native state upload is unmapped or unowned");
        }
        return;
    }
    __imp__sub_823BA300(ctx,base);
}
PPC_FUNC_IMPL(__imp__sub_823BB448);
PPC_FUNC(sub_823BB448) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32,header=ctx.r4.u32,source=ctx.r5.u32;
    if(ownsNativeRendering(device)) {
        // Only emits LOAD_ALU_CONSTANT packets and advances the old command
        // cursor. Native constants are applied to each actual draw; there is
        // no guest CPU shadow or semantic return value to replace here.
        if(!captureDriverConstants(device,0,header,source,true))
            throw std::runtime_error("Native shader literal upload is unsupported or unowned");
        return;
    }
    __imp__sub_823BB448(ctx,base);
    captureDriverConstants(device,0,header,source,true);
}
// v80: 823BDB28 (the skinned draw 823BBBD8's SET_SHADER_CONSTANTS reservation, gta5_inline_constants.h).
// The original reserves the packet and returns the data pointer; the packet is remembered and its
// data (written by the caller next) committed before the device's constants are next read or changed.
// An earlier packet still pending is committed first.
PPC_FUNC_IMPL(__imp__sub_823BDB28);
PPC_FUNC(sub_823BDB28) {
    PPC_FUNC_PROLOGUE();
    recomp::gpu::InlineConstantPacket packet{ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,0};
    __imp__sub_823BDB28(ctx,base);
    packet.data=ctx.r3.u32;
    inlineConstantsSwitches();
    const bool earlier=[&]{std::lock_guard lock(inlineConstantsMutex);return inlineConstants.pending();}();
    if(earlier && inlineGate)gateRef([&]{
        std::uint32_t device=0;{std::lock_guard lock(inlineConstantsMutex);device=inlineConstants.pendingDevice();}
        flushInlineConstants(device);});
    std::lock_guard lock(inlineConstantsMutex);
    inlineConstants.reserve(packet);
    noteInlineConstantsPendingLocked(); // v362
}
// v80: 836B4920 (the instanced draw 83210990's LOAD_ALU_CONSTANT): the original, then its source's
// float4 are committed as the GPU loads them (after a pending skinning packet).
PPC_FUNC_IMPL(__imp__sub_836B4920);
PPC_FUNC(sub_836B4920) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32,bank=ctx.r4.u32,start=ctx.r5.u32,address=ctx.r6.u32,count=ctx.r7.u32;
    __imp__sub_836B4920(ctx,base);
    inlineConstantsSwitches();
    if(inlineGate)gateRef([&]{
        std::lock_guard lock(inlineConstantsMutex);
        inlineConstants.gpuLoad(device,bank,start,address,count,inlineConstantsView,driverConstants[device]);
        noteInlineConstantsPendingLocked(); // v362
    });
}
PPC_FUNC_IMPL(__imp__sub_823ABBE0);
PPC_FUNC(sub_823ABBE0) {
    PPC_FUNC_PROLOGUE();
    const auto device=ctx.r3.u32;
    const std::array<std::uint32_t,4> rectangle{ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32};
    if(activeEndQueue) {
        if(device!=tileRecordingDevice.load() || static_cast<std::uint32_t>(ctx.lr)!=0x823ABBC8 || endRestoredScissor)
            throw std::runtime_error("Unexpected scissor producer inside original EndTiling");
        __imp__sub_823ABBE0(ctx,base);endRestoredScissor=rectangle;return;
    }
    if(ownsNativeRendering(device)) {
        // Non-binned original is pure window/scissor packet emission. Native
        // drawing applies the captured tuple; unsupported tiled binning stops.
        if(!captureDriverScissor(device,rectangle,ctx.lr)) {
            // DIAGNOSTIC: binned/unowned scissor -> original packet emission; its kicks are retired.
            static unsigned long long diagnosticBinned=0;
            if(const auto n=++diagnosticBinned;(n&(n-1))==0)
                rendererLog("RENDER_DIAG_BINNED_SCISSOR_ORIGINAL caller=%08x rectangle=%08x/%08x/%08x/%08x total=%llu\n",
                    static_cast<unsigned>(ctx.lr),rectangle[0],rectangle[1],rectangle[2],rectangle[3],n);
            __imp__sub_823ABBE0(ctx,base);
        }
        return;
    }
    __imp__sub_823ABBE0(ctx,base);
    captureDriverScissor(device,rectangle,ctx.lr);
}
