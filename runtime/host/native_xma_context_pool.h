#pragma once
#include "guest_xma_backend.h"
#include "native_xma_frame_decoder.h"
#include <functional>
#include <memory>
#include <string>

namespace recomp::audio {
// Xenia's XMA context/packet layout, decoded synchronously by the accessing guest
// thread. No pretend hardware worker or progress counter. The qualified .cpp relies
// on the cooperative executor gate; the multicore attempt .cpp (v12) is safe for
// concurrent callers: a pool lock plus per-context locks, map_ never called with an
// XMA lock held (it may take the registry mutex, R > L_xma), guest context words
// accessed as aligned 32-bit atomics. failure() describes the latest access in
// cooperative builds; multicore builds keep the first failure once recorded.
// Layout is identical in both; counters are read only after guest threads join.
class NativeXmaContextPool final : public bootstrap::GuestXmaBackend {
public:
    using MapPhysical = std::function<std::uint8_t*(std::uint32_t, std::uint32_t, bool)>;
    NativeXmaContextPool(std::uint32_t virtualBase, std::uint32_t physicalBase,
                         std::uint8_t* storage, MapPhysical map);
    ~NativeXmaContextPool() override;
    bool ready() const;
    std::uint32_t virtualBase() const override { return virtualBase_; }
    std::uint32_t physicalBase() const override { return physicalBase_; }
    std::uint32_t allocate() override;
    bool release(std::uint32_t address) override;
    // Values use PPC scalar (big-endian) convention. Hardware registers are LE.
    bool access(std::uint32_t address, bool write, std::uint32_t& value);
    const std::string& failure() const { return failure_; }
    const std::array<std::uint32_t,16>& failureContext() const { return failureContext_; }
    std::uint64_t decodedFrames() const { return decodedFrames_; }
    std::uint64_t writtenPcmBytes() const { return writtenPcmBytes_; }
private:
    struct Context;
    bool decode(unsigned index);
    bool fail(const char* reason);
    std::uint32_t virtualBase_, physicalBase_;
    std::uint8_t* storage_;
    MapPhysical map_;
    std::array<std::unique_ptr<Context>, ContextCount> contexts_{};
    std::uint32_t lastContext_ = 0;
    std::uint32_t control_ = 3;
    std::array<bool,ContextCount> pending_{};
    std::uint64_t decodedFrames_ = 0, writtenPcmBytes_ = 0;
    std::string failure_;
    std::array<std::uint32_t,16> failureContext_{};
};
}
