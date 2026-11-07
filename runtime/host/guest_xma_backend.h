#pragma once
#include <cstdint>

namespace recomp::bootstrap {
// Context storage is a real pinned physical allocation. The serialized guest
// executor owns all access; implementations must not re-enter guest execution.
class GuestXmaBackend {
public:
    virtual ~GuestXmaBackend() = default;
    virtual std::uint32_t virtualBase() const = 0;
    virtual std::uint32_t physicalBase() const = 0;
    virtual std::uint32_t allocate() = 0;
    virtual bool release(std::uint32_t address) = 0;
    static constexpr std::uint32_t ContextCount = 320, ContextBytes = 64;
    static constexpr std::uint32_t PoolBytes = ContextCount * ContextBytes;
};
}
