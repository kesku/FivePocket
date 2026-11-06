#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace recomp::audio {
constexpr std::size_t AudioFramesPerPacket=256, AudioGuestChannels=6;
constexpr std::size_t AudioGuestPacketBytes=AudioFramesPerPacket*AudioGuestChannels*4;
using PlanarAudioPacket=std::array<float,AudioFramesPerPacket*AudioGuestChannels>;
using StereoAudioPacket=std::array<float,AudioFramesPerPacket*2>;

// The caller owns a complete independent 6144-byte snapshot, never a pointer
// retained into mutable guest memory. Original title callback transposes 256
// frames into six BE float planes. Reject non-finite data instead of concealing
// uninitialized memory. Failure preserves the complete previous output.
inline bool decodePlanarAudioPacket(const std::uint8_t* source,std::size_t bytes,PlanarAudioPacket& output) noexcept {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    if (!source || bytes!=AudioGuestPacketBytes) return false;
    PlanarAudioPacket decoded;
    for(std::size_t i=0;i<decoded.size();++i) {
        const auto* p=source+i*4;
        const std::uint32_t bits=(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
        std::memcpy(&decoded[i],&bits,4);
        if (!std::isfinite(decoded[i])) return false;
    }
    output=decoded;return true;
}

// Explicit native port policy, not a reconstruction of the console mixer:
// planes FL,FR,FC,LFE,BL,BR; output L=.4FL+.2FC+.4BL, R=.4FR+.2FC+.4BR.
// LFE is deliberately omitted. This normalized matrix follows the pinned
// Xenia x86 mapping, not its ARM fallback (which swaps the rear channels).
// Double intermediates avoid overflow for finite float inputs. Clip only at
// output, counting every clipped sample. NaN/Inf reject transactionally.
inline bool mixNormalizedStereoNoLfe(const PlanarAudioPacket& source,StereoAudioPacket& output,
                                     std::uint32_t& clippedSamples) noexcept {
    for(float value:source) if (!std::isfinite(value)) return false;
    StereoAudioPacket mixed;std::uint32_t clipped=0;
    for(std::size_t frame=0;frame<AudioFramesPerPacket;++frame) for(std::size_t side=0;side<2;++side) {
        const double value=(double(source[side*AudioFramesPerPacket+frame])+
            double(source[(4+side)*AudioFramesPerPacket+frame])+
            0.5*double(source[2*AudioFramesPerPacket+frame]))/2.5;
        if (value<-1 || value>1) ++clipped;
        mixed[frame*2+side]=static_cast<float>(std::clamp(value,-1.0,1.0));
    }
    output=mixed;clippedSamples=clipped;return true;
}
}
