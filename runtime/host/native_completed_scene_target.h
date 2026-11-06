#pragma once
#include <cstdint>
#include <memory>
#include <utility>

namespace recomp::gpu {
// The recording is temporary, but a completed target remains an owned image.
// Keep at most one generation, independently of the recording's CPU snapshots.
// The caller must establish real scene retirement before transferring it here.
template<class Target,class Owner> class NativeCompletedSceneTarget {
public:
    bool retain(std::uint32_t device,Owner* owner,std::unique_ptr<Target>& pending) noexcept {
        if(!device || !owner || !pending || target_)return false;
        device_=device;owner_=owner;target_=std::move(pending);return true;
    }
    void reset() noexcept {target_.reset();device_=0;owner_=nullptr;}
    Target* target(Owner* owner) const noexcept {
        return owner && owner==owner_ ? target_.get() : nullptr;
    }
    std::uint32_t device() const noexcept {return target_?device_:0;}
private:
    std::unique_ptr<Target> target_;
    Owner* owner_=nullptr;
    std::uint32_t device_=0;
};
}
