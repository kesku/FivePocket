#pragma once
#include "native_geometry_query.h"
#include "present_fast.h" // v227
#include "scanout_fusion.h"
#include "guest_graphics_backend.h"
#include "device.h"
#include "native_graphics_state.h"
#include "native_color_state_commit.h"
#include "native_deferred_color_state_commit.h"
#include "native_texture_image.h"
#include "native_api_draw_target.h"
#include "native_offscreen_draw_target.h"
#include "native_direct_depth_target.h"
#include "native_sampled_stencil_draw.h"
#include "native_rectangle_mrt_target.h"
#include "native_offset_sample_program.h"
#include "native_rectangle_mrt_program.h"
#include "native_color8_clear_target.h"
#include "native_fixed16_clear_target.h"
#include "native_fixed16_geometry_draw.h"
#include "native_geometry_draw.h"
#include "native_logical_geometry_draw.h"
#include "native_logical_tile_replay.h"
#include "native_tile_command_interrupt.h"
#include "native_tile_deferred_tail.h"
#include "gta5_tile_extent_publication.h"
#include "native_tile_bin_command.h"
#include "native_tile_state_invalidation.h"
#include "native_tile_cache_barrier.h"
#include "native_tile_register_state.h"
#include "native_inline_shader_load.h"
#include "native_visibility_queries.h"
#include "native_vertex_clip_executor.h"
#include "native_query_neutral_fan.h"
#include "native_gamma_fan_program.h"
#include "native_gamma_quad_program.h"
#include "native_cube_rectangle_program.h"
#include "native_composite_rectangle_program.h"
#include "native_lod_rectangle_program.h"
#include "native_screen_strip_draw.h"
#include "native_filter_weights_draw.h"
#include "native_expanded16_max_draw.h"
#include "native_expanded16_luma_draw.h"
#include "native_r32_scalar_draw.h"
#include "native_exposure_scalar_draw.h"
#include "native_bright_pass_draw.h"
#include "native_expanded16_copy_draw.h"
#include "native_postfx_blur_draw.h"
#include "native_postfx_upsample_draw.h"
#include "native_world_strip_draw.h"
#include "native_clipped_indexed_draw.h"
#include "native_indexed_rgb7e3_draw.h"
#include "native_indexed_zero_depth_draw.h"
#include "native_indexed_blend_depth_draw.h"
#include "native_indexed_lod_draw.h"
#include "native_clipped_scene_fan_draw.h"
#include <optional>
#include <map>

namespace recomp::gpu {
class NativeColorChainProgram;
class NativeColorBatchCapture;
class NativeDeferredColorCapture;
struct NativeDeferredColorDiagnosticResult;
struct NativeColorChainExecutionResult;
class VulkanEngine final : public recomp::bootstrap::GuestGraphicsBackend {
public:
    ~VulkanEngine() override;
    bool initialize() override;
    bool ready() const noexcept override { return queueVerified_; }
    // Diagnostic generic renderer (attempt v16+): the verified device, or null before it is ready.
    ProbeDevice* diagnosticDevice() noexcept { return queueVerified_ ? &context_ : nullptr; }
    // Must be requested before device creation; never silently enable features
    // for old shader packs or accept ABI13 on a device created without them.
    bool requestNativeGeometry() {
        if(attempted_)return nativeGeometryEnabled_;
        nativeGeometryEnabled_=true;return true;
    }
    // Independent MRT blending is an additional explicit feature. Existing
    // loading-only users keep their old device-creation requirements.
    bool requestNativeLogicalGeometry() {
        if(attempted_)return nativeLogicalGeometryEnabled_;
        nativeGeometryEnabled_=true;nativeLogicalGeometryEnabled_=true;return true;
    }
    std::unique_ptr<NativeLogicalTileTarget> createLogicalTileTarget(const Gta5ClearCapture& surfaces) {
        if(!ready() || !nativeLogicalGeometryEnabled_ || !describeGta5LogicalTileSurfaces(surfaces))return nullptr;
        return NativeLogicalTileTarget::create(context_,*surfaces.state);
    }
    bool executeLogicalGeometry(NativeLogicalTileTarget& target,const NativeTranslatedDrawProgram& program,
            const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,const Gta5ClearCapture& surfaces,
            unsigned tile,const std::array<unsigned,4>& recordedScissor,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures,bool* visibleSamples=nullptr,
            const NativeClippedFanProgram* queryPositions=nullptr) {
        if(!ready() || !nativeLogicalGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_)return false;
        const auto control=gta5DrawRegister(draw,0x2293);if(!control)return false;
        bool measured=false;
        // The sufficient geometric-exclusion provider is explicit. Missing
        // code and non-excluded geometry remain unknown, never final-sample
        // substitutes. No target writes precede the position proof.
        if((*control&1) && (!queryPositions || !nativeGeometryQueryProgramMatches(program,*queryPositions)))return false;
        if(program.vertexIdentity.empty() || program.pixelIdentity.empty())return false;
        // Native upload hooks supply shader state outside recorded PM4. Stage
        // allocation before GPU work, then retain the exact successfully bound
        // raw identities. A copy-program load must not leak into the next draw.
        NativeInlineShaderLoad::Programs rawPrograms{program.vertexIdentity,program.pixelIdentity};
        const bool ok=visibilityQueries_.draw(*control,[&]()->std::optional<NativeVisibilityQueries::Measurement> {
            if(*control&1) {
                if(!logicalQueryPositions_ || logicalQueryPositions_->program().positionCode!=queryPositions->positionCode ||
                   !nativeGeometryQueryProgramMatches(program,logicalQueryPositions_->program()))
                    logicalQueryPositions_=NativeVertexClipExecutor::create(context_,*queryPositions);
                if(!logicalQueryPositions_ || !measureNativeGeometryViewExclusion(*logicalQueryPositions_,program,
                        draw,indices,surfaces,recordedScissor,textures))return std::nullopt;
            }
            if(!executeNativeLogicalGeometryDraw(context_,target,program,draw,indices,surfaces,tile,recordedScissor,textures,
                    ((*control&1) || visibleSamples)?&measured:nullptr))return std::nullopt;
            // An observed fragment contradicts the geometric proof: fail the
            // lifecycle rather than publishing that supposedly empty query.
            if(*control&1) {
                if(measured)return std::nullopt;
                return NativeVisibilityQueries::Measurement{false,NativeVisibilityQueries::Stage::PotentialVisibility};
            }
            return NativeVisibilityQueries::Measurement{measured,NativeVisibilityQueries::Stage::FinalSamples};
        });
        if(ok)retainedMaintenancePrograms_.swap(rawPrograms);
        if(ok && visibleSamples)*visibleSamples=measured;
        return ok;
    }
    bool executeLogicalVisibilityCommand(std::uint32_t payload) {
        return ready() && nativeLogicalGeometryEnabled_ && !tileInterruptActive_ && !tileInterruptFailed_ &&
            visibilityQueries_.command(payload,initializationRegisterJournal_);
    }
    std::optional<bool> logicalVisibilityCondition(std::uint32_t token) const noexcept {
        if(!ready() || !nativeLogicalGeometryEnabled_ || tileInterruptFailed_)return std::nullopt;
        return visibilityQueries_.condition(token);
    }
    bool logicalVisibilityIdle()const noexcept {
        return ready() && nativeLogicalGeometryEnabled_ && !tileInterruptActive_ && !tileInterruptFailed_ &&
            !ringRestartPending_ && visibilityQueries_.idle();
    }
    // Only for the planner's exact C0012201/0/84 zero-count packet. There is
    // no raster work; an active/unknown visibility lifecycle is not bypassed.
    // EXT and other adjacent effects remain the ordered consumer's obligations.
    bool executeLogicalZeroRaster()const noexcept {return logicalVisibilityIdle();}
    // Called in submission order under the owner's execution gate. Unlike the
    // cold-init state batch, this changes only a bin word; no resource/cache or
    // scratch side effect and no ring retirement. Invalid input leaves state intact.
    bool executeLogicalBinCommand(const NativeTileBinCommand& command) noexcept {
        if(!ready() || !nativeLogicalGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           visibilityQueries_.failed() || ringRestartPending_)return false;
        return command.apply(stateBatchSnapshot_);
    }
    // Original copy-program invalidation: fence this owner and discard derived
    // caches, preserving raw game state and images. No load/scene/RPTR claim.
    bool executeLogicalStateInvalidation(const NativeTileStateInvalidation&,NativeLogicalTileTarget&,unsigned* evictedExecutors=nullptr);
    bool executeLogicalCacheBarrier(const NativeTileCacheBarrier&,NativeLogicalTileTarget&,unsigned* evictedExecutors=nullptr);
    // Raw state only; actual native draw/copy admission remains independent.
    // In particular this cannot acknowledge scratch/cache/unknown MMIO writes.
    bool executeLogicalRegisterState(const NativeTileRegisterState& command) {
        if(!ready() || !nativeLogicalGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           visibilityQueries_.failed() || ringRestartPending_)return false;
        command.apply(initializationRegisterJournal_);return true;
    }
    bool executeLogicalInlineShaderLoad(const NativeInlineShaderLoad& command) {
        if(!ready() || !nativeLogicalGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           !visibilityQueries_.idle() || ringRestartPending_)return false;
        command.apply(retainedMaintenancePrograms_);return true;
    }
    std::optional<NativeLogicalTileReplayResult> executeLogicalReplay(const Gta5ClearCapture& surfaces,
            std::uint32_t start,const Gta5ResourceReader& controls,const NativeLogicalTileOperationPreparer& prepare) {
        if(!ready() || !nativeLogicalGeometryEnabled_)return std::nullopt;
        return executeNativeLogicalTileReplay(context_,surfaces,start,controls,prepare);
    }
    // Caller keeps current command/scratch allocation leases and exclusive
    // guest execution through the original ISR return. Neither API retires RPTR.
    bool executeTileCommandInterrupt(const Gta5TileCommandInterrupt&,
        const NativeTileCommandInterrupt::Memory&,const NativeTileCommandInterrupt::Guest&);
    bool executeTileSceneContinuation(const Gta5TileSceneContinuation&,
        const NativeTileCommandInterrupt::Memory&,const NativeTileCommandInterrupt::Guest&);
    bool executeTileDeferredTail(const Gta5TileDeferredTail&,NativeLogicalTileTarget&,
        const NativeTileDeferredTail::Publish&);
    // Publish only the original worker's conservative XY culling records after
    // the caller has executed the bound first-pass scene on this engine. The
    // executor gate and CURRENT command/table owners stay held by the caller;
    // no scene/RPTR completion or general EVENT_WRITE_EXT is implied.
    bool publishTileExtentBounds(Gta5TileExtentPublication&,
        const Gta5TileExtentPublication::Read&,const Gta5TileExtentPublication::Publish&);
    bool executeBoundGeometry(NativeColor8ClearTarget& target,const NativeTranslatedDrawProgram& program,
            const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bindings,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
        return ready() && nativeGeometryEnabled_ && executeNativeGeometryDraw(context_,target,program,draw,indices,bindings,textures);
    }
    bool executeFixed16BoundGeometry(NativeFixed16ClearTarget& target,const NativeTranslatedDrawProgram& program,
            const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bindings,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures={}) {
        return ready() && nativeGeometryEnabled_ && !tileInterruptActive_ && !tileInterruptFailed_ &&
            !ringRestartPending_ && visibilityQueries_.idle() &&
            executeNativeFixed16GeometryDraw(target,program,draw,indices,bindings,textures);
    }
    bool executeFixed16Rectangle(NativeFixed16ClearTarget& target,const NativeTranslatedDrawProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,
            const std::vector<std::pair<unsigned,NativeMipTextureCapture>>& textures) {
        return ready() && nativeGeometryEnabled_ && !tileInterruptActive_ && !tileInterruptFailed_ &&
            !ringRestartPending_ && visibilityQueries_.idle() &&
            executeNativeFixed16RectangleDraw(target,program,draw,bindings,textures);
    }
    // Native resource API: no command-ring epoch or numbered-frame gate.
    std::unique_ptr<NativeTextureImage> createTextureImage(const XenosTextureDescription& description,
            const std::vector<std::uint8_t>& bytes) {
        return ready()?NativeTextureImage::create(context_,description,bytes):nullptr;
    }
    std::unique_ptr<NativeApiDrawTarget> createApiDrawTarget(const NativeColorChainProgram& program) {
        return ready()?NativeApiDrawTarget::create(context_,program):nullptr;
    }
    std::unique_ptr<NativeOffscreenDrawTarget> createOffscreenDrawTarget(const NativeOffscreenDrawPlan& plan) {
        return ready()?NativeOffscreenDrawTarget::create(context_,plan):nullptr;
    }
    std::unique_ptr<NativeDirectDepthTarget> createDirectDepthTarget(const NativeStencilOnlyPlan& plan,
            const std::vector<std::uint32_t>& words) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_)return nullptr;
        return NativeDirectDepthTarget::create(context_,plan,words);
    }
    bool executeDirectStencil(NativeDirectDepthTarget& target,const NativeTranslatedDrawProgram& program,
            const std::vector<std::uint8_t>& input,unsigned vertices,
            const std::array<float,4>& viewport,const std::array<unsigned,4>& scissor) {
        return ready() && nativeGeometryEnabled_ && !tileInterruptActive_ && !tileInterruptFailed_ &&
            target.draw(program,input,vertices,viewport,scissor);
    }
    bool executeScreenStrip(const NativeScreenStripProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeScreenStripDraw(context_,program,draw,bound,true,physical,why);
    }
    bool executeExpanded16Max(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeExpanded16MaxDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executeExpanded16Luma(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeExpanded16LumaDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executeR32Scalar(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeR32ScalarDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executeExposureScalar(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeExposureScalarDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executeBrightPass(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeBrightPassDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executeExpanded16Copy(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeExpanded16CopyDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executePostfxBlur(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativePostfxBlurDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    // ABI44 reads the destination's physical words (ONE/ONE add): same guards;
    // an unknown destination refuses inside the executor before GPU work.
    bool executePostfxUpsample(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativePostfxUpsampleDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    bool executeFilterWeights(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& currentVertex,
            const std::vector<std::uint8_t>& currentPixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeFilterWeightsDraw(context_,program,draw,bound,currentVertex,currentPixel,true,physical,why);
    }
    template<class Current>
    bool executeSampledStencil(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& vertexIdentity,
            const std::vector<std::uint8_t>& pixelIdentity,NativeEdram32Mutation& physical,
            Current&& current,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeSampledStencil(context_,program,draw,bound,vertexIdentity,pixelIdentity,
            physical,std::forward<Current>(current),why);
    }
    bool executeWorldStrip(const NativeWorldStripProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const NativeWorldStripTextures& textures,
            NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="engine-query-or-interrupt-owner";return false;
        }
        return executeNativeWorldStripDraw(context_,program,draw,bound,textures,true,physical,why);
    }
    std::optional<NativeIndexedViewExclusion> executeClippedIndexed(const NativeClippedFanProgram& program,
            const Gta5InlineDraw& draw,const std::vector<std::uint32_t>& indices,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& vertex,
            const std::vector<std::uint8_t>& pixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="clipped-indexed-engine-query-owner";return std::nullopt;
        }
        return executeNativeClippedIndexed(context_,program,draw,indices,bound,vertex,pixel,true,physical,why);
    }
    bool executeIndexedRgb7e3(const NativeIndexedRgb7e3Program& program,const Gta5InlineDraw& draw,
            const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bound,
            const std::vector<std::uint8_t>& vertex,const std::vector<std::uint8_t>& pixel,
            NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="indexed-rgb7e3-engine-query-owner";return false;
        }
        return executeNativeIndexedRgb7e3Draw(context_,program,draw,indices,bound,vertex,pixel,true,physical,why);
    }
    std::unique_ptr<NativeRectangleMrtTarget> createRectangleMrtTarget(const NativeRectangleMrtPlan& plan) {
        return ready()?NativeRectangleMrtTarget::create(context_,plan):nullptr;
    }
    bool executeIndexedZeroDepth(const NativeIndexedZeroDepthProgram& program,const Gta5InlineDraw& draw,
            const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bound,
            const NativeIndexedZeroDepthTextures& textures,const std::vector<std::uint8_t>& vertex,
            const std::vector<std::uint8_t>& pixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="indexed-zero-depth-engine-query-owner";return false;
        }
        return executeNativeIndexedZeroDepthDraw(context_,program,draw,indices,bound,textures,vertex,pixel,true,physical,why);
    }
    bool executeIndexedBlendDepth(const NativeIndexedBlendDepthProgram& program,const Gta5InlineDraw& draw,
            const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bound,
            const NativeIndexedBlendDepthTextures& textures,const std::vector<std::uint8_t>& vertex,
            const std::vector<std::uint8_t>& pixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="indexed-blend-depth-engine-query-owner";return false;
        }
        return executeNativeIndexedBlendDepthDraw(context_,program,draw,indices,bound,textures,vertex,pixel,true,physical,why);
    }
    bool executeIndexedLod(const NativeIndexedLodProgram& program,const Gta5InlineDraw& draw,
            const std::vector<std::uint32_t>& indices,const Gta5BoundSurfaceCapture& bound,
            const NativeIndexedLodTextures& textures,const std::vector<std::uint8_t>& vertex,
            const std::vector<std::uint8_t>& pixel,NativeEdram32Mutation& physical,const char** why=nullptr) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="indexed-lod-engine-query-owner";return false;
        }
        return executeNativeIndexedLodDraw(context_,program,draw,indices,bound,textures,vertex,pixel,true,physical,why);
    }
    std::unique_ptr<NativeColor8ClearTarget> createColor8ClearTarget(const Gta5Color8ClearPlan& plan) {
        return ready()?NativeColor8ClearTarget::create(context_,plan):nullptr;
    }
    std::unique_ptr<NativeFixed16ClearTarget> createFixed16ClearTarget(const Gta5Fixed16ClearPlan& plan) {
        return ready()?NativeFixed16ClearTarget::create(context_,plan):nullptr;
    }
    std::optional<unsigned> executeClippedSceneFan(const NativeClippedFanProgram& program,const Gta5InlineDraw& draw,
            const Gta5BoundSurfaceCapture& bound,const std::vector<std::uint8_t>& vertex,
            const std::vector<std::uint8_t>& pixel,NativeEdram32Mutation& physical,const char** why=nullptr,
            NativeSceneFanProfile profile=NativeSceneFanProfile::Textured684) {
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="scene-fan-engine-query-owner";return std::nullopt;
        }
        return executeNativeClippedSceneFan(context_,program,draw,bound,vertex,pixel,true,physical,why,profile);
    }
    std::optional<NativeQueryNeutralFanProof> executeQueryNeutralFan(const NativeClippedFanProgram& program,
            const Gta5InlineDraw& draw,const std::vector<std::uint8_t>& vertex,
            const std::vector<std::uint8_t>& pixel,const char** why=nullptr) {
        // Pending ORIGINAL CPU-emitted query packets are left untouched. This
        // must not run inside a different native logical-query transaction.
        if(!ready() || !nativeGeometryEnabled_ || tileInterruptActive_ || tileInterruptFailed_ ||
           ringRestartPending_ || !visibilityQueries_.idle()) {
            if(why)*why="neutral-fan-engine-owner";return std::nullopt;
        }
        return executeNativeQueryNeutralFan(context_,program,draw,vertex,pixel,why);
    }
    std::optional<unsigned> executeClippedFan(const NativeClippedFanProgram& program,const Gta5InlineDraw& draw) {
        if(!ready() || !nativeClippedFanState(draw,program.stride))return std::nullopt;
        if(!clippedFan_ || clippedFan_->program().positionCode!=program.positionCode ||
           clippedFan_->program().stride!=program.stride)
            clippedFan_=NativeVertexClipExecutor::create(context_,program);
        return clippedFan_?clippedFan_->execute(draw):std::nullopt;
    }
    bool executeGammaFan(NativeColor8ClearTarget& target,const NativeGammaFanProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,const NativeMipTextureCapture* mipTexture=nullptr) {
        if(!ready() || program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity || program.positions.pixelIdentity!=program.draw.pixelIdentity ||
           !nativeGammaFanState(draw,program.draw,target.plan(),bindings,mipTexture))return false;
        if(!gammaFanPositions_ || gammaFanPositions_->program().positionCode!=program.positions.positionCode ||
           gammaFanPositions_->program().stride!=program.positions.stride)
            gammaFanPositions_=NativeVertexClipExecutor::create(context_,program.positions);
        const auto positions=gammaFanPositions_?gammaFanPositions_->positions(draw):std::nullopt;
        return positions && target.drawGammaConvexFan(program.draw,draw,bindings,*positions,
            program.transferVertex,program.encodePixel,program.decodePixel,mipTexture);
    }
    std::optional<NativeClippedQuadList> executeClippedGammaQuads(const NativeGammaQuadProgram& program,
            const Gta5InlineDraw& draw) {
        if(!ready() || program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity || program.positions.pixelIdentity!=program.draw.pixelIdentity ||
           !nativeQuadPositionState(draw,program.positions.stride))return std::nullopt;
        if(!gammaQuadPositions_ || gammaQuadPositions_->program().positionCode!=program.positions.positionCode ||
           gammaQuadPositions_->program().stride!=program.positions.stride)
            gammaQuadPositions_=NativeVertexClipExecutor::create(context_,program.positions);
        return gammaQuadPositions_?gammaQuadPositions_->executeQuads(draw):std::nullopt;
    }
    bool executeGammaQuads(NativeColor8ClearTarget& target,const NativeGammaQuadProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,
            const NativeTranslatedDrawProgram* persistentIndexed=nullptr) {
        if(!ready() || program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity || program.positions.pixelIdentity!=program.draw.pixelIdentity ||
           (!target.isDepthStencilUniform() && !persistentIndexed) || !nativeGammaQuadState(draw,program.draw,target.plan(),bindings))return false;
        if(!gammaQuadPositions_ || gammaQuadPositions_->program().positionCode!=program.positions.positionCode ||
           gammaQuadPositions_->program().stride!=program.positions.stride)
            gammaQuadPositions_=NativeVertexClipExecutor::create(context_,program.positions);
        const auto positions=gammaQuadPositions_?gammaQuadPositions_->quadPositions(draw):std::nullopt;
        return positions && target.drawGammaAxisQuads(program.draw,draw,bindings,*positions,
            program.transferVertex,program.encodePixel,program.decodePixel,program.depthPixel,persistentIndexed);
    }
    bool executeCubeRectangles(NativeOffscreenDrawTarget& target,const NativeCubeRectangleProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings) {
        if(!ready() || program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity || program.positions.pixelIdentity!=program.draw.pixelIdentity ||
           planNativeCubeRectangleDraw(draw,program.draw,bindings)!=std::optional{target.plan()})return false;
        if(!cubeRectanglePositions_ || cubeRectanglePositions_->program().positionCode!=program.positions.positionCode ||
           cubeRectanglePositions_->program().stride!=program.positions.stride)
            cubeRectanglePositions_=NativeVertexClipExecutor::create(context_,program.positions);
        const auto positions=cubeRectanglePositions_?cubeRectanglePositions_->rectanglePositions(draw):std::nullopt;
        return positions && target.drawCubeRectangles(program.draw,draw,bindings,*positions);
    }
    bool executeCompositeRectangles(NativeOffscreenDrawTarget& target,const NativeCompositeRectangleProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,const char** stage=nullptr) {
        const auto refuse=[&](const char* why){if(stage)*stage=why;return false;};
        if(stage)*stage="entered";
        if(!ready())return refuse("engine-not-ready");
        if(program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity ||
           program.positions.pixelIdentity!=program.draw.pixelIdentity)return refuse("positions-program-mismatch");
        if(planNativeCompositeRectangleDraw(draw,program.draw,bindings)!=std::optional{target.plan()})
            return refuse("plan-vs-target-mismatch");
        if(!compositeRectanglePositions_ || compositeRectanglePositions_->program().positionCode!=program.positions.positionCode ||
           compositeRectanglePositions_->program().stride!=program.positions.stride)
            compositeRectanglePositions_=NativeVertexClipExecutor::create(context_,program.positions);
        if(!compositeRectanglePositions_)return refuse("positions-executor-create");
        const auto positions=compositeRectanglePositions_->rectanglePositions(draw);
        if(!positions)return refuse("positions-compute");
        return target.drawCompositeRectangles(program.draw,draw,bindings,*positions,stage);
    }
    // ABI23 offset sample: same coverage proof as the composite band, computed
    // from the SAME pair's position-only translation before the colour draw is
    // admitted. `stage` names the refusing step.
    bool executeOffsetSample(NativeOffscreenDrawTarget& target,const NativeOffsetSampleProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,const char** stage=nullptr) {
        const auto refuse=[&](const char* why){if(stage)*stage=why;return false;};
        if(stage)*stage="entered";
        if(!ready())return refuse("engine-not-ready");
        if(program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity ||
           program.positions.pixelIdentity!=program.draw.pixelIdentity)return refuse("positions-program-mismatch");
        if(planNativeOffsetSampleDraw(draw,program.draw,bindings)!=std::optional{target.plan()})
            return refuse("plan-vs-target-mismatch");
        if(!offsetSamplePositions_ || offsetSamplePositions_->program().positionCode!=program.positions.positionCode ||
           offsetSamplePositions_->program().stride!=program.positions.stride)
            offsetSamplePositions_=NativeVertexClipExecutor::create(context_,program.positions);
        if(!offsetSamplePositions_)return refuse("positions-executor-create");
        const auto positions=offsetSamplePositions_->rectanglePositions(draw);
        if(!positions)return refuse("positions-compute");
        return target.drawOffsetSample(program.draw,draw,bindings,*positions,stage);
    }
    // ABI22 rectangle MRT: the coverage proof is computed from the SAME pair's
    // position-only translation, exactly as the composite band does, before the
    // two-attachment colour draw is admitted. `stage` names the refusing step.
    bool executeRectangleMrt(NativeRectangleMrtTarget& target,const NativeRectangleMrtProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,const char** stage=nullptr) {
        const auto refuse=[&](const char* why){if(stage)*stage=why;return false;};
        if(stage)*stage="entered";
        if(!ready())return refuse("engine-not-ready");
        if(program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity ||
           program.positions.pixelIdentity!=program.draw.pixelIdentity)return refuse("positions-program-mismatch");
        const char* planWhy="not-evaluated";
        const auto plan=planNativeRectangleMrtDraw(draw,program.draw,bindings,&planWhy);
        if(!plan)return refuse(planWhy);
        if(*plan!=target.plan())return refuse("plan-vs-target-mismatch");
        if(!rectangleMrtPositions_ || rectangleMrtPositions_->program().positionCode!=program.positions.positionCode ||
           rectangleMrtPositions_->program().stride!=program.positions.stride)
            rectangleMrtPositions_=NativeVertexClipExecutor::create(context_,program.positions);
        if(!rectangleMrtPositions_)return refuse("positions-executor-create");
        const auto positions=rectangleMrtPositions_->rectanglePositions(draw);
        if(!positions)return refuse("positions-compute");
        return target.draw(program.draw,draw,bindings,*positions,stage);
    }
    bool executeLodRectangle(NativeOffscreenDrawTarget& target,const NativeLodRectangleProgram& program,
            const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bindings,const NativeMipTextureCapture& texture) {
        if(!ready() || program.positions.stride!=program.draw.stride ||
           program.positions.vertexIdentity!=program.draw.vertexIdentity || program.positions.pixelIdentity!=program.draw.pixelIdentity)return false;
        const auto plan=planNativeLodRectangleDraw(draw,program.draw,bindings,texture);
        if(!plan || plan->target!=target.plan())return false;
        if(!lodRectanglePositions_ || lodRectanglePositions_->program().positionCode!=program.positions.positionCode ||
           lodRectanglePositions_->program().stride!=program.positions.stride)
            lodRectanglePositions_=NativeVertexClipExecutor::create(context_,program.positions);
        const auto positions=lodRectanglePositions_?lodRectanglePositions_->rectanglePositions(draw):std::nullopt;
        return positions && target.drawLodRectangle(program.draw,draw,bindings,*positions,texture);
    }
    // Owned pixels captured after the actual initial depth/alias GPU stage.
    // Intended for API-path comparison after writers join, not a live lease.
    const std::vector<std::uint8_t>& capturedColorInitialHalf()const noexcept{return apiDrawInitialHalf_;}
    bool configureDisplay(const recomp::bootstrap::NativeDisplayConfiguration&) override;
    bool queryDisplayGamma(std::uint32_t& type, std::uint32_t& powerBits) const noexcept override;
    bool selectNativeDisplayMode(std::uint32_t flags) noexcept override;
    bool querySelectedDisplayConfiguration(recomp::bootstrap::NativeDisplayConfiguration&) const noexcept override;
    bool completeNativeCommandStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeNativePostOutputSwapStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeNativeDeferredSwapStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeNativeRuntimeCommandStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeNativeScalerStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool beginNativeScalerInterrupt(const NativeScalerWrapper&,const NativeScalerInterruptSequence::Action&) override;
    bool queryNativeScratchWriteback(std::uint32_t&,std::uint32_t&) const noexcept override;
    bool acknowledgeNativeScalerInterrupt(const NativeScalerInterruptSequence::Action&) override;
    bool completeNativeScalerTail(NativeScalerTailResult&) override;
    bool completeNativePostScalerStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeNativePostDisplayStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeNativePostOutputStorageCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool executeNativeDisplayGamma(const NativeDisplayGammaWrapper&,const std::function<bool()>&,NativeDisplayGammaResult&) override;
    bool executeExperimentalNativeDisplayOutput(const NativeDisplayOutputWrapper&,const std::function<bool()>&,NativeDisplayOutputResult&) override;
    bool nativeScalerConfigured() const noexcept {
        return nativeScalerInterrupt_ && nativeScalerInterrupt_->phase()==NativeScalerInterruptSequence::Phase::Complete &&
            nativeScalerDisplay_.configured();
    }
    bool mapNativeScanout(const std::uint8_t* pixels,std::size_t bytes,unsigned width,unsigned height,
            std::vector<std::uint8_t>& output) const {
        if (!nativeScalerConfigured()) return false;
        // v227 (present_fast.h): the identity scaler copy straight into `output` (reused by the caller) and the ramp in
        // place through per-frame tables; same bytes as mapRgba + NativeDisplayGammaRamp::mapRgba. On failure `output`
        // may hold partial data (both callers discard it on false).
        if (present::baseline()) { // v227 A/B: the v226 per-pixel mapping (diag-v227-baseline)
            std::vector<std::uint8_t> mapped;
            if (!nativeScalerDisplay_.mapRgba(pixels,bytes,width,height,mapped) ||
                (nativeGammaRamp_ && !nativeGammaRamp_->mapRgba(mapped.data(),mapped.size(),mapped))) return false;
            output.swap(mapped);return true;
        }
        if (scanout_fusion::enabled()) {
            const auto result=scanout_fusion::map(nativeScalerDisplay_,nativeGammaRamp_?&*nativeGammaRamp_:nullptr,pixels,bytes,width,height,output);
            if(result!=scanout_fusion::Outcome::NotApplicable)return result==scanout_fusion::Outcome::Success;
        }
        if (!present::identityScalerInto(nativeScalerDisplay_,pixels,bytes,width,height,output)) return false;
        return !nativeGammaRamp_ || present::gammaInPlace(*nativeGammaRamp_,output.data(),output.size());
    }
    bool bindNativeSwapCompletionClock(NativeCompletionClock);
    bool executeNativeSwapWrapper(const NativeSwapWrapper&,const std::function<bool()>&,NativeSwapGpuResult&) override;
    bool executeNativePostOutputSwapWrapper(const NativePostOutputSwapWrapper&,const std::function<bool()>&,
        const NativePostOutputSwapInterrupt&,NativePostOutputSwapGpuResult&) override;
    bool executeNativeDeferredSwapWrapper(const NativeDeferredSwapSubmission&,const std::function<bool()>&,
        const NativeDeferredSwapInterrupt&,NativeDeferredSwapGpuResult&) override;
    bool completeInitialEmptyCacheCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeEmptyGuestCacheCoherency(const recomp::bootstrap::NativeInitialCoherency&) override;
    bool completeExperimentalConstantInitialization(const StoppedConstantInitialization&, NativeInitializationReadback&) override;
    bool completeNativeStateBatch(const std::vector<std::uint32_t>&, NativeInitializationReadback&) override;
    bool completeExperimentalIdleCachePair(const std::vector<std::uint32_t>&, NativeInitializationReadback&) override;
    bool executeExperimentalMaintenancePoints(const Pm4Snapshot&,const Pm4IndirectGraph&,
        const std::function<bool(const NativeInitializationReadback&)>&) override;
    // Runtime point/upload profile, separate from startup cursor history. The
    // caller pins original command/fence owners and excludes guest writers.
    // Target audit, fresh native fence and exact readback precede publication;
    // this method does not itself publish guest memory or retire the ring.
    bool executeRuntimeMaintenancePoints(const Pm4Snapshot&,const Pm4IndirectGraph&,
        const NativeLogicalTileTarget&,const std::function<bool(const NativeInitializationReadback&)>&);
    const std::array<std::vector<std::uint8_t>,2>& retainedMaintenancePrograms() const noexcept {
        // Raw active programs from maintenance, native geometry hooks, or
        // inline loads. These bytes are not evidence of native shader execution.
        return retainedMaintenancePrograms_;
    }
    bool drainForRingReconfiguration() override;
    bool completeWarmRingInitialization(const std::vector<std::uint32_t>&, const std::vector<std::uint32_t>&) override;
    bool warmRingInitialized() const noexcept { return warmRingInitialized_; }
    // One post-stop diagnostic on this SAME native device/queue. Caller has
    // joined every guest writer. Consumes independent captured bytes, returns
    // owned diagnostic results, and does not change guest register/residency
    // journals or authorize copy/events/RPTR publication. Never a submit API.
    bool executeStoppedColorBatchDiagnostic(const NativeColorChainProgram&,const NativeColorBatchCapture&,
        NativeColorChainExecutionResult&);
    // One separately requested post-output GPU diagnostic: fresh leading
    // wait/setup fence plus four audited stages, no live state/publication.
    bool executeStoppedPostOutputColorDiagnostic(const NativeColorChainProgram&,const NativeColorBatchCapture&,
        NativeColorChainExecutionResult&);
    // Separately requested off-screen diagnostic after the second swap and
    // completed fresh deferred storage fence. No final events/state commit.
    // Caller joins every writer; this is not a live prefix submit API.
    bool executeStoppedDeferredColorDiagnostic(const NativeColorChainProgram&,const NativeDeferredColorCapture&,
        NativeDeferredColorDiagnosticResult&);
    bool executeExperimentalDeferredColorPrefix(const NativeColorChainProgram&,const NativeDeferredColorCapture&,
        const NativeDeferredColorPublisher&) override;
    // Explicit one-batch native model: fresh derived resources, conservative
    // full fences regardless of zero guest COHER_SIZE, no inferred low status
    // readback. Publishes only through the synchronous failure-atomic callback,
    // then retains raw state/defined EDRAM without fallible work. Caller owns
    // all publication memory and parks writers. Registry must explicitly bind
    // this experiment; normal submission does not opt in automatically.
    bool executeExperimentalOwnedColorBatch(const NativeColorChainProgram&,const NativeColorBatchCapture&,const NativeColorPublisher&) override;
    bool executeExperimentalPostOutputColorBatch(const NativeColorChainProgram&,const NativeColorBatchCapture&,const NativeColorPublisher&) override;
    bool retainedColorEdramWord(std::uint32_t offset,std::uint32_t& value) const noexcept {
        return colorEdram_ && colorEdram_->readWord(offset,value);
    }
    using StateBatchSnapshot=NativeGraphicsState;
    const StateBatchSnapshot& stateBatchSnapshot() const noexcept { return stateBatchSnapshot_; }
    std::optional<std::uint32_t> retainedRegister(std::uint32_t index) const {
        const auto found=initializationRegisterJournal_.find(index);
        if (found==initializationRegisterJournal_.end() || found->second.second!=0xFFFFFFFF) return std::nullopt;
        return found->second.first;
    }
    EdramTrainingPolicy queryEdramTrainingPolicy() override;
    bool initialEmptyCacheCoherencyCompleted() const noexcept { return initialCoherencyCompleted_; }
    const std::optional<recomp::bootstrap::NativeDisplayConfiguration>& displayConfiguration() const { return displayConfiguration_; }
private:
    bool executeFreshDeferredColor(const NativeColorChainProgram&,const NativeDeferredColorCapture&,NativeDeferredColorExecutionResult&);
    bool executeAuditedPostOutputColor(const NativeColorChainProgram&,const NativeColorBatchCapture&,NativeColorChainExecutionResult&);
    bool completeStateBatch(const std::vector<std::uint32_t>&, NativeInitializationReadback&, bool experimentalIdleCachePair);
    bool fenceEmptyGuestCache(const recomp::bootstrap::NativeInitialCoherency&);
    bool fenceNativeOrdering(const recomp::bootstrap::NativeInitialCoherency&);
    ProbeDevice context_;
    std::unique_ptr<NativeVertexClipExecutor> clippedFan_;
    std::unique_ptr<NativeVertexClipExecutor> gammaFanPositions_;
    std::unique_ptr<NativeVertexClipExecutor> logicalQueryPositions_;
    std::unique_ptr<NativeVertexClipExecutor> gammaQuadPositions_;
    std::unique_ptr<NativeVertexClipExecutor> cubeRectanglePositions_;
    std::unique_ptr<NativeVertexClipExecutor> compositeRectanglePositions_;
    std::unique_ptr<NativeVertexClipExecutor> offsetSamplePositions_;
    std::unique_ptr<NativeVertexClipExecutor> rectangleMrtPositions_;
    std::unique_ptr<NativeVertexClipExecutor> lodRectanglePositions_;
    std::vector<std::uint8_t> apiDrawInitialHalf_;
    VkCommandPool commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkBuffer bootstrapBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bootstrapMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    bool attempted_ = false;
    bool nativeGeometryEnabled_ = false;
    bool nativeLogicalGeometryEnabled_ = false;
    bool tileInterruptActive_=false,tileInterruptFailed_=false;
    bool executeTileInterrupt(NativeTileCommandInterrupt,
        const NativeTileCommandInterrupt::Memory&,const NativeTileCommandInterrupt::Guest&);
    bool queueVerified_ = false;
    bool initialCoherencyAttempted_ = false;
    bool initialCoherencyCompleted_ = false;
    bool constantInitializationAttempted_ = false;
    bool constantInitializationCompleted_ = false;
    bool stoppedColorDiagnosticAttempted_=false;
    bool experimentalColorAttempted_=false;
    std::optional<NativeColorEdramSnapshot> colorEdram_;
    std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>> initializationRegisterJournal_;
    StateBatchSnapshot stateBatchSnapshot_;
    NativeVisibilityQueries visibilityQueries_;
    std::array<std::vector<std::uint8_t>,2> retainedMaintenancePrograms_;
    bool ringRestartPending_=false, warmRingInitialized_=false;
    std::array<std::uint32_t,19> warmMeInitProfile_{};
    std::optional<recomp::bootstrap::NativeDisplayConfiguration> displayConfiguration_;
    bool nativeDisplayModeSelected_=false;
    bool commandStorageCoherencyCompleted_=false;
    NativeCompletionClock nativeSwapClock_;
    bool nativeSwapWrapperCompleted_=false;
    bool nativeScalerStorageCompleted_=false;
    std::optional<NativeScalerInterruptSequence> nativeScalerInterrupt_;
    NativeScalerDisplayState nativeScalerDisplay_;
    bool writeNativeDisplayEvents(const std::array<std::uint32_t,2>&,std::array<std::uint8_t,8>&);
    bool nativeGammaAttempted_=false;
    bool nativeOutputAttempted_=false;
    bool nativeOutputCompleted_=false;
    // Diagnostic and live execution are mutually exclusive on one engine.
    bool nativePostOutputStorageCompleted_=false,postOutputColorExecutionAttempted_=false;
    bool postOutputSwapStorageAttempted_=false,postOutputSwapStorageCompleted_=false;
    bool postOutputSwapAttempted_=false,postOutputSwapCompleted_=false;
    bool deferredSwapStorageAttempted_=false,deferredSwapStorageCompleted_=false;
    bool deferredColorExecutionAttempted_=false; // Shared by diagnostic/live: no promotion or retry.
    std::optional<NativeDeferredSwapSubmission> committedDeferredColorSubmission_;
    bool deferredSwapAttempted_=false,deferredSwapCompleted_=false;
    std::optional<NativeDisplayGammaRamp> nativeGammaRamp_;
    bool fenceNativeQueue();
};
}
