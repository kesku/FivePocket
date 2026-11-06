// vk11 (attempt v506, 2026-10-07): which Vulkan 1.1 drivers this engine accepts, and what must then be enabled by name.
// Seen on a Galaxy Z Fold4 (Adreno 730, driver 0615.98, Vulkan 1.1): Play stopped at VdInitializeEngines because the device
// set-up refused every driver below 1.2. The engine takes three things from 1.2, all of them extensions on 1.1 drivers:
// descriptor indexing (required: every translated shader declares RuntimeDescriptorArray), scalar block layout (optional:
// the uniform constant banks of v294), mirror-clamp-to-edge sampling (one sampler address mode). The shaders are SPIR-V 1.3,
// which Vulkan 1.1 accepts. A driver that reports 1.2 or newer never reaches the legacy branch: its plan is "nothing to add".
// Plain numbers in and out, so the rule runs in a host test (vk11_gate_test.cpp beside this file).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace recomp::vk11 {
constexpr std::uint32_t Api11 = (1u << 22) | (1u << 12), Api12 = (1u << 22) | (2u << 12);
struct Offered { bool descriptorIndexing, scalarBlockLayout, mirrorClampToEdge, bufferDeviceAddress; };
struct Plan {
    bool accepted, legacy, enableDescriptorIndexing, enableScalarBlockLayout, enableMirrorClampToEdge, enableBufferDeviceAddress;
};
// scalarWanted: the device reports scalarBlockLayout and the uniform banks are on. addressRequired: a shader needs buffer addresses.
constexpr Plan plan(std::uint32_t api, Offered o, bool scalarWanted, bool addressRequired) {
    if (api >= Api12) return {true, false, false, false, false, false};
    if (api < Api11 || !o.descriptorIndexing || (addressRequired && !o.bufferDeviceAddress)) return {false, true, false, false, false, false};
    return {true, true, true, scalarWanted && o.scalarBlockLayout, o.mirrorClampToEdge, addressRequired};
}
// Set by the device set-up when it accepted a Vulkan 1.1 driver. Read where a module needs the workaround below.
inline std::atomic<bool> legacyDriver{false};
// Driver workaround (Adreno 730, driver 0615.98, Vulkan 1.1; seen on a Galaxy Z Fold4, 2026-10-07). The startup's copy shader
// guards its one write with `edram.length() != 2621448 || outputWords.length() != 942088` and wrote NOTHING (the whole
// output buffer kept its 0x5A fill), although both bound ranges are exactly that long; the store shader's same kind of guard
// passes. The copy shader uses bindings 1 and 2 of its set, the store shader bindings 0 and 1. Removing the write-only
// qualifier did not help (tried: test build 6). HYPOTHESIS, not shown: the driver looks the length up by the shader's n-th
// used buffer, not by binding number. Whatever the cause, the length this driver reports cannot be relied on.
// This returns the module with every comparison of an array length against another value satisfied: in
// `OpIEqual/OpINotEqual a b` where exactly one operand is an OpArrayLength result (directly or through OpBitcast), that
// operand is replaced by the other one, so "length != expected" is false and "length == expected" is true. The buffers'
// sizes are fixed by the caller (the profile's constants) and the caller's byte-exact audit of the results remains the
// check that the stage did its work. A malformed module is returned as it is. Used only on an accepted 1.1 driver.
inline std::vector<std::uint32_t> withArrayLengthChecksSatisfied(const std::uint32_t* words, std::size_t count) {
    std::vector<std::uint32_t> out(words, words + (words ? count : 0));
    if (!words || count < 5 || words[0] != 0x07230203u) return out;
    std::vector<std::uint32_t> lengths;   // result ids that carry an array length
    const auto isLength = [&](std::uint32_t id) { for (auto x : lengths) if (x == id) return true; return false; };
    for (std::size_t i = 5; i < count;) {
        const std::uint32_t op = out[i] & 0xFFFFu, n = out[i] >> 16;
        if (n == 0 || i + n > count) return std::vector<std::uint32_t>(words, words + count);
        if (op == 68 && n == 5) lengths.push_back(out[i + 2]);                              // OpArrayLength type result struct member
        else if (op == 124 && n == 4 && isLength(out[i + 3])) lengths.push_back(out[i + 2]); // OpBitcast type result operand
        else if ((op == 170 || op == 171) && n == 5) {                                       // OpIEqual / OpINotEqual type result a b
            const bool a = isLength(out[i + 3]), b = isLength(out[i + 4]);
            if (a && !b) out[i + 3] = out[i + 4];
            else if (b && !a) out[i + 4] = out[i + 3];
        }
        i += n;
    }
    return out;
}
}
