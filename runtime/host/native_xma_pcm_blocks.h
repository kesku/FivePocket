#pragma once
#include "native_xma_frame_decoder.h"
#include <algorithm>

namespace recomp::audio {
// Copies a window of already decoded, finite samples. A block is 128 scalar
// samples (256 bytes), not 128 samples per channel. Callers own ring free-space
// accounting. Reject bounds before writing; the codec checks sample finiteness.
inline bool writeXmaPcmBlocks(const NativeXmaFrameDecoder::Frame& frame,
                             unsigned firstBlock, unsigned blocks,
                             std::uint8_t* output, unsigned capacity,
                             unsigned& write) {
    if (!output || !capacity || capacity%256 || write>=capacity || write%256 ||
        frame.channels<1 || frame.channels>2 || frame.samples!=512 ||
        firstBlock>frame.channels*4 || blocks>frame.channels*4-firstBlock ||
        blocks>capacity/256) return false;
    for (unsigned scalar=firstBlock*128;scalar<(firstBlock+blocks)*128;++scalar) {
        const auto sample=scalar/frame.channels, channel=scalar%frame.channels;
        const float f=std::clamp(frame.planar[channel][sample],-1.0f,1.0f);
        const auto v=static_cast<std::uint16_t>(static_cast<std::int16_t>(f*32767.0f));
        output[write]=v>>8;output[(write+1)%capacity]=v;write=(write+2)%capacity;
    }
    return true;
}
}
