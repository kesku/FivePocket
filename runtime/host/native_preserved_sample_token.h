#pragma once
#include <cstdint>
#include <limits>

namespace recomp::gpu {
// Failure-diagnostic authorization only, NOT guest ownership or completion.
// Taken under the queue/guest gate while the external owner is valid. A failed
// draw may invalidate that owner without submitting a persistent image write.
// The same image, content generation and mutation epoch must still match.
struct NativePreservedSampleToken {
    const void* image=nullptr;
    std::uint32_t object=0;
    std::uint64_t contentSerial=0,mutationSerial=0;
    bool matches(const void* currentImage,std::uint32_t currentObject,
                 std::uint64_t currentContent,std::uint64_t currentMutation)const {
        return image && object && contentSerial && mutationSerial &&
            mutationSerial!=std::numeric_limits<std::uint64_t>::max() &&
            image==currentImage && object==currentObject &&
            contentSerial==currentContent && mutationSerial==currentMutation;
    }
};
}
