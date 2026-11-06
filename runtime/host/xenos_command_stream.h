#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace recomp::gpu {
enum class Pm4Kind : uint8_t { RegisterSequence, RegisterPair, Filler, Command, ZeroPadding };
struct Pm4Packet {
    Pm4Kind kind = Pm4Kind::Filler;
    uint32_t offsetWords = 0;
    uint32_t payloadWords = 0;
    uint32_t firstRegister = 0;
    uint32_t secondRegister = 0;
    uint32_t opcode = 0;
    bool repeatRegister = false;
    bool predicated = false;
};
enum class Pm4Failure {
    None, InvalidInput, WordLimit, PacketLimit, TruncatedPacket, AllocationFailure,
    UnsupportedOpcode, PredicatedCommand, RegisterRange, UnsupportedConstantSelector,
    ShaderLayout, ShaderLoadLimit, DrawLayout, DrawLimit,
};
struct Pm4Error {
    Pm4Failure failure = Pm4Failure::None;
    uint32_t offsetWords = 0;
    uint32_t opcode = 0;
};
struct Pm4Limits {
    uint32_t maxWords = 1024 * 1024;
    uint32_t maxPackets = 65536;
    // Explicit Xenia-compatible diagnostic policy; ambiguous zero headers are
    // ordinary type-0 packets by default, not silently discarded.
    bool zeroWordIsPadding = false;
};
struct RegisterWrite {
    uint32_t packetOffsetWords = 0;
    uint32_t index = 0;
    uint32_t value = 0;
};
struct Pm4ShaderLoad {
    uint32_t packetOffsetWords = 0;
    uint32_t stage = 0; // Packet encoding: 0 vertex, 1 pixel (not SPIR-V models).
    uint32_t instructionWords = 0;
    bool immediate = false;
    uint32_t physicalAddress = 0; // Metadata only; zero for an immediate load.
    std::vector<uint8_t> inlineBigEndianBytes;
};
struct Pm4ShaderInspection {
    std::vector<Pm4ShaderLoad> loads;
    uint32_t otherPackets = 0; // Every non-shader packet remains unexecuted.
};
struct Pm4DrawInitiator {
    std::uint32_t packetOffsetWords = 0, opcode = 0, indexCount = 0, primitive = 0, source = 0, majorMode = 0;
    bool index32 = false, notEndOfPacket = false, conditionalVizQuery = false, predicated = false;
};
class Pm4Snapshot {
public:
    const std::vector<uint32_t>& words() const { return words_; }
    const std::vector<Pm4Packet>& packets() const { return packets_; }
private:
    std::vector<uint32_t> words_;
    std::vector<Pm4Packet> packets_;
    friend bool decodePm4(const uint8_t*, size_t, const Pm4Limits&, Pm4Snapshot&, Pm4Error&);
    friend bool decodePm4Ring(const uint8_t*, size_t, uint32_t, uint32_t,
                              const Pm4Limits&, Pm4Snapshot&, Pm4Error&);
};
// Owns a big-endian input snapshot. Never follows indirect pointers or executes.
bool decodePm4(const uint8_t* bytes, size_t size, const Pm4Limits& limits,
               Pm4Snapshot& output, Pm4Error& error);
// Caller must provide a stable owned ring snapshot. Equal indices mean empty,
// not full. Offsets in the result are relative to the supplied read pointer.
bool decodePm4Ring(const uint8_t* ring, size_t ringBytes, uint32_t readWord,
                   uint32_t writeWord, const Pm4Limits& limits,
                   Pm4Snapshot& output, Pm4Error& error);
// The control field is not log2(bytes): bytes = 1 << (control + 3), capped here.
bool pm4RingBytes(uint32_t controlSizeLog2, uint32_t& outputBytes);
// A pure, all-or-nothing plan. It does NOT apply writes or their hardware side
// effects, signal completion, consume a ring pointer, or issue a Vulkan command.
bool planPm4RegisterWrites(const Pm4Snapshot& snapshot, uint32_t registerCount,
                           std::vector<RegisterWrite>& output, Pm4Error& error);
// Inspect only IM_LOAD / IM_LOAD_IMMEDIATE metadata. Other packets are counted,
// NOT accepted as implemented or executed. No physical address is followed,
// shader is validated/compiled, or active-stage state changed. Unsupported
// shader layouts reject the entire inspection and preserve the previous output.
bool inspectPm4ShaderLoads(const Pm4Snapshot& snapshot, Pm4ShaderInspection& output,
                           Pm4Error& error, uint32_t maximumLoads = 1024);
// Decode only documented VGT_DRAW_INITIATOR fields from DRAW_INDX / _2. Does
// not validate/follow the index payload, resolve primitive topology, select
// shaders, evaluate predicates/queries or execute a draw. Zero indices remain
// distinguishable from nonempty requests. Failures preserve prior output.
bool inspectPm4DrawInitiators(const Pm4Snapshot& snapshot, std::vector<Pm4DrawInitiator>& output,
                            Pm4Error& error, uint32_t maximumDraws = 64);
}
