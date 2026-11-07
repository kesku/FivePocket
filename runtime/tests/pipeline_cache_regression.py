#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the renderer's actual cache lookup and dynamic-stencil code with a fake creator.

The public export cannot link the whole renderer. Compile the production key,
lookup, hash and dynamic-reference code; only Vulkan creation is replaced.
There is no second implementation of the cache key or stencil mask rule.
"""
import argparse
from pathlib import Path
import subprocess


def between(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def source(renderer, cpu):
    hashes = between(cpu, 'inline std::uint64_t fnv(', '// ---- v23')
    hashes += between(cpu, 'inline std::uint64_t wordsKey(', 'inline std::uint64_t word64(')
    state = between(renderer, '    struct PipelineState {', '    PipelineState pipelineState;')
    cache = between(renderer, '    struct Pipeline {',
                    '    std::unordered_map<std::uint64_t,std::array<VkDescriptorSet,5>> frameSets;')
    index = next(line for line in renderer.splitlines() if 'static std::size_t v363Index(' in line)
    lookup = between(renderer, '    VkPipeline pipeline(const PipelineState& s,', '        const bool roundNull=')
    references = between(renderer, '        const std::uint32_t stencilFront=', '        biasSlope=rsBiasSlope(')
    commands = between(renderer, '        if(!recorded.haveStencil ||', '        std::array<float,4> blendConstants;')
    return r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
using VkPipeline=std::uint64_t;
using VkFormat=std::uint32_t;
using VkPrimitiveTopology=std::uint32_t;
constexpr auto VK_NULL_HANDLE=0;
constexpr auto VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST=3;
constexpr unsigned VK_STENCIL_FACE_FRONT_BIT=1,VK_STENCIL_FACE_BACK_BIT=2;
std::array<unsigned,2> deviceReferences{};
void vkCmdSetStencilReference(unsigned,unsigned face,unsigned reference) {
    deviceReferences[face==VK_STENCIL_FACE_FRONT_BIT?0:1]=reference;
}
''' + hashes + r'''
struct Harness {
    struct Attribute {unsigned location=0,binding=0,offset=0;VkFormat format=0;};
    struct PackEntry {};
    struct Bound {std::uint64_t key=1;};
    bool wordKeys=true,v363FlatMaps=true,blendGuard=true;
    std::uint64_t frame=0,creates=0;
    std::vector<std::uint64_t> pipelineWords;
    static constexpr unsigned V363PipeBits=2;
    struct {unsigned pipeHits=0,pipeMisses=0;} v363Stats;
    struct {std::atomic<bool> on{false},noClip{false};} vsTrim;
    static void v363Verify(bool same,const char*) {assert(same);}
    static std::uint64_t gcKeySalt() {return 0;}
    static std::uint64_t passKeyOf(const Bound& bound) {return bound.key;}
''' + index + '\n' + cache + state + lookup + r'''
        return p=++creates;
    }
    struct {bool haveStencil=false;unsigned stencilFront=0,stencilBack=0;} recorded;
    struct {unsigned stateSkips=0;} perf;
    VkPipeline draw(const PipelineState& s,const PackEntry& shader,const Bound& bound) {
        const auto handle=pipeline(s,shader,shader,bound);
        const auto depthControl=s.depthControl;
        const unsigned cmd=0;
''' + references + commands + r'''
        return handle;
    }
};

int main() {
    for(bool words:{false,true})for(bool front:{false,true}) {
        Harness h;h.wordKeys=words;h.v363FlatMaps=front;
        Harness::PackEntry shader;
        Harness::Bound bound;
        Harness::PipelineState state;
        state.vs=1;state.ps=2;state.depthControl=0x80;
        state.stencilMasks=0x00FFFF03;state.stencilMasksBack=0x00FFFF07;
        const auto original=h.draw(state,shader,bound);
        for(unsigned reference=0;reference<256;++reference) {
            state.stencilMasks=0xA5FFFF00|reference;
            state.stencilMasksBack=0x5AFFFF00|(255-reference);
            assert(h.draw(state,shader,bound)==original);
            assert(deviceReferences[0]==reference && deviceReferences[1]==255-reference);
        }
        assert(h.creates==1 && h.pipelines.size()==1);

        // Compare and write mask changes must still make different pipelines.
        for(unsigned field:{0x100u,0x10000u}) {
            state.stencilMasks^=field;
            const auto before=h.creates;
            h.draw(state,shader,bound);
            assert(h.creates==before+1);
            state.stencilMasksBack^=field;
            h.draw(state,shader,bound);
            assert(h.creates==before+2);
        }
        const auto distinct=h.creates;
        state.depthControl^=1;
        h.draw(state,shader,bound);
        ++state.ps;
        h.draw(state,shader,bound);
        ++bound.key;
        h.draw(state,shader,bound);
        assert(h.creates==distinct+3);

        // Single-sided stencil still uses the front reference for both faces.
        state.depthControl=0;state.stencilMasks=0xFFFF17;state.stencilMasksBack=0xFFFF99;
        h.draw(state,shader,bound);
        assert(deviceReferences[0]==0x17 && deviceReferences[1]==0x17);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'generic_renderer.cpp')
    parser.add_argument('--cpu', type=Path, default=Path(__file__).resolve().parents[1] / 'generic_renderer_cpu.h')
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cpp = args.out / 'pipeline-cache.cpp'
    binary = args.out / 'pipeline-cache'
    cpp.write_text(source(args.source.read_text(), args.cpu.read_text()))
    subprocess.run([args.compiler, '-std=c++20', '-O2', '-UNDEBUG', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
