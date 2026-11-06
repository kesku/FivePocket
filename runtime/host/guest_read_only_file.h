#pragma once

#include <cstddef>
#include <cstdint>

namespace recomp::bootstrap {
// Already-open, explicitly selected content. The registry never constructs a
// host pathname from guest data. Implementations must read exactly the requested
// bounded range or fail; failures are staged away from guest output memory.
class GuestReadOnlyFile {
public:
    virtual ~GuestReadOnlyFile() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual bool read(std::uint64_t offset, std::uint8_t* destination,
            std::size_t bytes) const noexcept = 0;
};
} // namespace recomp::bootstrap
