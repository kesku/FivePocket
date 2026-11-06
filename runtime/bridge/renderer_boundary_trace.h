#pragma once
#include <functional>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace recomp::bootstrap { class GuestImportRegistry; }
namespace recomp::gpu { class VulkanEngine; }
namespace recomp::gpu { class NativeColorChainProgram; }
namespace recomp::gpu { class NativeColorChainCapture; }
namespace recomp::gpu { class Gta5TileCommandInterrupt; }
namespace recomp::gpu { class Gta5TileSceneContinuation; }

struct RendererSceneScheduler {
    std::function<std::uint32_t()> currentToken;
    std::function<bool(std::uint32_t)> parked;
    std::function<bool(const std::function<bool()>&)> wait;
};

// Opt-in API clear/draw/resolve and native Present after display initialization.
// Original submission remains for APIs that have not transferred ownership.
// Resolve can publish completed native pixels into owned guest textures.
// Bind only for a single serialized executor run; unbind after its workers join.
// The gate must actually enforce non-yielding ownership, not simply invoke f.
void bindRendererInlineCapture(recomp::bootstrap::GuestImportRegistry*,
    const recomp::gpu::NativeColorChainProgram*,
    std::function<bool(const std::function<void()>&)> gate,
    recomp::gpu::VulkanEngine* graphics=nullptr,
    std::function<bool(const std::vector<std::uint8_t>&,unsigned,unsigned)> present={},
    // Separate non-yielding scope for bounded ORIGINAL CPU recording. Unlike
    // the read-only one-entry gate this must allow up to256 original entries;
    // it must still reject nesting, waits and owner changes. Empty disables it.
    std::function<bool(const std::function<void()>&)> originalRecordingGate={},
    // Whole original EndTiling CPU recording, with its own bounded exclusive
    // scope. MMIO acceptance captures queued work; it does not complete it.
    std::function<bool(const std::function<void()>&)> originalEndRecordingGate={},
    // Real native packet consumer + exclusive registered CPU2 IRQ handoff.
    // Completes only this interrupt request, never its queued scene or RPTR.
    std::function<bool(std::uint32_t,const recomp::gpu::Gta5TileCommandInterrupt&)> sceneInterrupt={},
    // A selected scene continuation includes its ordered pre/post IRQ effects;
    // it cannot be replaced by invoking just the unpredicated initial request.
    std::function<bool(std::uint32_t,const recomp::gpu::Gta5TileSceneContinuation&)> sceneContinuation={},
    RendererSceneScheduler sceneScheduler={});
// Only active inside that original-CPU scope. nullopt means not this path;
// true means queued bytes retained, NEVER GPU completion/RPTR advancement.
// false is a terminal rejection, not permission to try another backend.
std::optional<bool> captureRendererTileSubmission(std::uint32_t writeWord);
bool submitRendererRuntimeMaintenance(std::uint32_t writeWord);
void unbindRendererInlineCapture();
// One bounded owned snapshot, emitted only for the first unsupported shader
// draw after all workers join. Directory must not already exist.
bool saveStoppedRendererDraw(const std::string& directory);
// Independent comparison of the latest two API-level captures against the
// existing command-derived shader inputs and the retained live native output.
// This after-stop comparison does not execute a second copy of the draws,
// publish guest memory, acknowledge a ring or present.
void compareRendererInlineInputs(const recomp::gpu::NativeColorChainCapture&,
    recomp::gpu::VulkanEngine*,const recomp::gpu::NativeColorChainProgram*);

// Call only after every guest writer has joined, while the registry still owns
// the guest mappings. Reports metadata, not a resource residency guarantee.
void dumpStoppedRendererTextureDescriptions(
    const recomp::bootstrap::GuestImportRegistry& registry,
    recomp::gpu::VulkanEngine* graphics=nullptr,
    const recomp::gpu::NativeColorChainProgram* programs=nullptr);
