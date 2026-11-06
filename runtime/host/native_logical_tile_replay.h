#pragma once
#include "gta5_tile_replay_schedule.h"
#include "native_logical_tile_target.h"
#include <functional>
#include <map>

namespace recomp::gpu {
// Each operation is already bound to owned native inputs/outputs. The callback
// evaluates this actual worker mask against the operation's recorded predicate
// and executes/fences its selected effects. It may not publish guest pixels or
// retire guest resources. Unknown descriptors must fail preparation, not become
// no-op callbacks. Queue access and guest allocation leases remain external.
using NativeLogicalTileOperation=std::function<bool(NativeLogicalTileTarget&,unsigned,std::uint32_t)>;
using NativeLogicalTileOperationPreparer=std::function<std::optional<NativeLogicalTileOperation>(std::uint32_t,std::uint32_t)>;
struct NativeLogicalTileReplayPlan {
    Gta5TileReplaySchedule schedule;
    std::map<std::pair<std::uint32_t,std::uint32_t>,NativeLogicalTileOperation> operations;
};
inline std::optional<NativeLogicalTileReplayPlan> prepareNativeLogicalTileReplay(
        const Gta5ClearCapture& surfaces,std::uint32_t start,const Gta5ResourceReader& controls,
        const NativeLogicalTileOperationPreparer& prepare) {
    const auto layout=describeGta5LogicalTileSurfaces(surfaces);
    auto schedule=planGta5TileReplaySchedule(start,controls);
    if(!prepare || !layout || !schedule || schedule->beginFlags!=layout->layout.beginFlags ||
       schedule->binCount!=layout->layout.tiles.size())return std::nullopt;
    for(unsigned bin=0;bin<schedule->binCount;++bin)for(unsigned c=0;c<4;++c)
        if(schedule->rectangles[bin][c]!=layout->layout.tiles[bin].logical[c]-layout->layout.tiles[0].origin[c%2])return std::nullopt;
    NativeLogicalTileReplayPlan plan;plan.schedule=std::move(*schedule);
    // Bind every distinct descriptor BEFORE allocating the target or issuing
    // any GPU work. Repeated bins/phases keep the same immutable operation.
    for(const auto& event:plan.schedule.events)if(event.kind==Gta5TileReplaySchedule::Kind::Indirect) {
        const auto key=std::make_pair(event.descriptorWord,event.descriptorAddress);
        if(plan.operations.contains(key))continue;
        auto operation=prepare(key.first,key.second);
        if(!operation || !*operation)return std::nullopt;
        plan.operations.emplace(key,std::move(*operation));
    }
    return plan;
}
struct NativeLogicalTileReplayResult {
    std::unique_ptr<NativeLogicalTileTarget> target;
    unsigned operationCallbacks=0,finishedNativeTiles=0;
};
// Original-order native consumer. A result proves these native operations
// returned under their fence contract, NOT original worker/callback completion.
// Resolve callbacks retain output privately until a separate guest-publication
// transaction succeeds. Failed replay returns no target or completion token.
inline std::optional<NativeLogicalTileReplayResult> executeNativeLogicalTileReplay(
        ProbeDevice& device,const Gta5ClearCapture& surfaces,std::uint32_t start,
        const Gta5ResourceReader& controls,const NativeLogicalTileOperationPreparer& prepare) {
    const auto plan=prepareNativeLogicalTileReplay(surfaces,start,controls,prepare);
    if(!plan)return std::nullopt;
    NativeLogicalTileReplayResult result;
    result.target=NativeLogicalTileTarget::create(device,*surfaces.state);
    if(!result.target)return std::nullopt;
    unsigned current=0;
    for(const auto& event:plan->schedule.events) {
        if(event.kind==Gta5TileReplaySchedule::Kind::Mask) {
            if(event.bin==plan->schedule.binCount) {
                if(current+1!=event.bin || event.mask!=UINT32_MAX)return std::nullopt;
                result.finishedNativeTiles=event.bin;
            } else if(event.bin!=current) {
                if(event.bin!=current+1 || !result.target->carryCapturedTile(surfaces,current,event.bin))return std::nullopt;
                current=event.bin;
            }
            continue;
        }
        if(result.finishedNativeTiles || event.bin!=current)return std::nullopt;
        const auto& operation=plan->operations.at({event.descriptorWord,event.descriptorAddress});
        if(!operation(*result.target,event.bin,event.mask))return std::nullopt;
        ++result.operationCallbacks;
    }
    if(result.finishedNativeTiles!=plan->schedule.binCount)return std::nullopt;
    return result;
}
}
