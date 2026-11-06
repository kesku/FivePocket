#pragma once
#include "xenos_command_stream.h"
#include <functional>
#include <optional>
#include <algorithm>
#include <new>

namespace recomp::gpu {
enum class Pm4GraphFailure : std::uint32_t {
    None, InvalidLimits, Limit, IndirectLayout, Predicated, NonPhysicalAddress,
    ReadRejected, DecodeRejected, Cycle, AllocationFailure, ReaderException
};
struct Pm4GraphError {
    Pm4GraphFailure failure = Pm4GraphFailure::None;
    std::uint32_t parent = 0, offsetWords = 0;
    Pm4Error decode;
};
struct Pm4IndirectNode {
    std::uint32_t parent = 0, parentOffsetWords = 0, physicalAddress = 0, depth = 0;
    Pm4Snapshot snapshot;
};
struct Pm4IndirectGraph {
    // Node 0 is the separately supplied root; vector nodes are numbered 1..N.
    // Repeated sibling references stay separate, preserving traversal order.
    std::vector<Pm4IndirectNode> nodes;
    std::uint32_t totalWords = 0, totalPackets = 0;
};
struct Pm4GraphLimits {
    std::uint32_t maxNodes = 64, maxDepth = 8, maxWords = 1024 * 1024, maxPackets = 65536;
};
using Pm4PhysicalReader = std::function<std::optional<std::vector<std::uint8_t>>(
    std::uint32_t physicalAddress, std::uint32_t byteCount)>;

// Read-only stopped-stream inspection, not command execution. The caller must
// stop/join every memory writer and enforce physical allocation ownership in
// reader. Only unpredicated 0x3F / 0x37 exact two-word IBs are followed. Native
// profile requires canonical aligned 29-bit physical addresses (no alias guess).
// Non-IB packets stay opaque; no branch, wait, memory write or completion is
// simulated. A successful graph only establishes bounded structural readability.
// Any failure preserves output; no partial graph escapes.
inline bool inspectPm4IndirectGraph(const Pm4Snapshot& root, const Pm4PhysicalReader& reader,
        Pm4IndirectGraph& output, Pm4GraphError& error, const Pm4GraphLimits& limits = {}) {
    const auto fail = [&](Pm4GraphFailure why, std::uint32_t parent = 0, std::uint32_t at = 0) {
        error = {why, parent, at, {}}; return false;
    };
    if (!reader || !limits.maxNodes || limits.maxNodes > 64 || !limits.maxDepth || limits.maxDepth > 8 ||
        !limits.maxWords || limits.maxWords > 1024 * 1024 || !limits.maxPackets || limits.maxPackets > 65536)
        return fail(Pm4GraphFailure::InvalidLimits);
    if (root.words().size() > limits.maxWords || root.packets().size() > limits.maxPackets)
        return fail(Pm4GraphFailure::Limit);
    try {
        Pm4IndirectGraph candidate;
        candidate.nodes.reserve(limits.maxNodes); // References stable during bounded DFS.
        candidate.totalWords = static_cast<std::uint32_t>(root.words().size());
        candidate.totalPackets = static_cast<std::uint32_t>(root.packets().size());
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ancestors;
        ancestors.reserve(limits.maxDepth);
        const auto visit = [&](auto&& self, const Pm4Snapshot& snapshot, std::uint32_t parent, std::uint32_t depth) -> bool {
            for (const auto& packet : snapshot.packets()) {
                if (packet.kind != Pm4Kind::Command || (packet.opcode != 0x3F && packet.opcode != 0x37)) continue;
                const auto at = packet.offsetWords;
                if (packet.predicated) return fail(Pm4GraphFailure::Predicated, parent, at);
                if (packet.payloadWords != 2) return fail(Pm4GraphFailure::IndirectLayout, parent, at);
                const auto address = snapshot.words()[at + 1], words = snapshot.words()[at + 2];
                if (!words || words > 0xFFFFF) return fail(Pm4GraphFailure::IndirectLayout, parent, at);
                if ((address & 3) || std::uint64_t{address} + std::uint64_t{words} * 4 > 0x20000000)
                    return fail(Pm4GraphFailure::NonPhysicalAddress, parent, at);
                const auto key = std::make_pair(address, words);
                if (std::find(ancestors.begin(), ancestors.end(), key) != ancestors.end())
                    return fail(Pm4GraphFailure::Cycle, parent, at);
                if (depth == limits.maxDepth || candidate.nodes.size() == limits.maxNodes ||
                    words > limits.maxWords - candidate.totalWords)
                    return fail(Pm4GraphFailure::Limit, parent, at);
                std::optional<std::vector<std::uint8_t>> bytes;
                try { bytes = reader(address, words * 4); }
                catch (const std::bad_alloc&) { return fail(Pm4GraphFailure::AllocationFailure, parent, at); }
                catch (...) { return fail(Pm4GraphFailure::ReaderException, parent, at); }
                if (!bytes || bytes->size() != std::size_t{words} * 4)
                    return fail(Pm4GraphFailure::ReadRejected, parent, at);
                Pm4IndirectNode node{parent, at, address, depth + 1, {}};
                Pm4Error decode;
                if (!decodePm4(bytes->data(), bytes->size(), {}, node.snapshot, decode)) {
                    error = {Pm4GraphFailure::DecodeRejected, parent, at, decode}; return false;
                }
                if (node.snapshot.packets().size() > limits.maxPackets - candidate.totalPackets)
                    return fail(Pm4GraphFailure::Limit, parent, at);
                candidate.totalWords += words;
                candidate.totalPackets += static_cast<std::uint32_t>(node.snapshot.packets().size());
                candidate.nodes.push_back(std::move(node));
                const auto id = static_cast<std::uint32_t>(candidate.nodes.size());
                ancestors.push_back(key);
                if (!self(self, candidate.nodes[id - 1].snapshot, id, depth + 1)) return false;
                ancestors.pop_back();
            }
            return true;
        };
        if (!visit(visit, root, 0, 0)) return false;
        output = std::move(candidate); error = {}; return true;
    } catch (const std::bad_alloc&) { return fail(Pm4GraphFailure::AllocationFailure); }
}
}
