#pragma once

namespace recomp::gpu {
struct NativeStencilTileBatchResult {
    unsigned accepted=0;
    bool completed=false;
    const char* stage="empty-tile-list";
};

// The callback's boolean is execution authority; its text is diagnostic only.
// Stop at the first rejection, including after an already completed tile.
template<class DrawTile>
NativeStencilTileBatchResult executeNativeStencilTileBatch(unsigned tiles,DrawTile&& draw) {
    NativeStencilTileBatchResult result;
    if(!tiles)return result;
    for(unsigned tile=0;tile<tiles;++tile) {
        const char* reason=nullptr;
        if(!draw(tile,&reason)) {
            result.stage=reason?reason:"tile-draw";
            return result;
        }
        ++result.accepted;
    }
    result.completed=true;
    result.stage="drawn";
    return result;
}
}
