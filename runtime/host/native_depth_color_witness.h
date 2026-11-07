#pragma once
#include "xenos_depth_stencil_state.h"
#include "xenos_mrt_blend.h"
#include <array>

namespace recomp::gpu {
// Per-tile implication, NOT a coverage bitmap or whole-tile initialization:
// stored depth != 0 implies all channels of this color were initialized.
// A completed full depth0 clear establishes the implication vacuously. It
// survives a depth writer only if that writer also replaces the complete color.
// Call only after fenced, error-free operations. Failed sessions are unusable.
class NativeDepthColorWitness {
    std::array<unsigned,4> tiles_{};
public:
    void cleared(unsigned touched,unsigned complete,unsigned aspects,float depth) {
        if(!(aspects&0x10))return;
        complete&=touched;
        if(depth==0)for(auto& t:tiles_)t|=complete;
        else for(auto& t:tiles_)t&=~touched;
    }
    void drew(unsigned tile,const XenosDepthStencilState& depth,unsigned outputs,
              const std::array<unsigned,4>& masks,const std::array<unsigned,4>& controls) {
        if(tile>=32){tiles_.fill(0);return;}
        if(!depth.depthEnable || !depth.depthWrite)return;
        for(unsigned s=0;s<4;++s) {
            const auto blend=decodeXenosMrtBlend(controls[s]);
            if(!(outputs&(1u<<s)) || masks[s]!=15 || !blend || blend->readsDestination(15))
                tiles_[s]&=~(1u<<tile);
        }
    }
    void carried(unsigned from,unsigned to) {
        if(from>=32 || to>=32){tiles_.fill(0);return;}
        for(auto& t:tiles_) {
            const bool source=t&(1u<<from);t&=~(1u<<to);
            if(source)t|=1u<<to;
        }
    }
    bool permits(unsigned tile,unsigned colors,const XenosDepthStencilState& depth)const {
        // The shader checks before late depth/stencil tests. With GE and
        // nonnegative incoming depth, every zero-depth fragment would pass.
        // Reject other tests to avoid reporting an occluded unknown as an error.
        if(!depth.depthEnable || depth.depthWrite ||
           depth.depthCompare!=6 || depth.stencilEnable)return false;
        return witnesses(tile,colors);
    }
    bool witnesses(unsigned tile,unsigned colors)const {
        if(tile>=32 || !colors || colors&~15u)return false;
        for(unsigned s=0;s<4;++s)if((colors&(1u<<s)) && !(tiles_[s]&(1u<<tile)))return false;
        return true;
    }
};
}
