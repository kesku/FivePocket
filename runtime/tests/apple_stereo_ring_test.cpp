// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_stereo_ring.h"
#include <array>
#include <cassert>
#include <thread>

namespace {
void fifoAndDiscard() {
    recomp::apple::StereoRing<4> ring;
    const float first[]{1,-1,2,-2,3,-3,4,-4};
    float output[12]{};
    assert(ring.write(first,4)==4);
    assert(ring.write(first,1)==0);
    assert(ring.render(output,2)==2);
    assert(output[0]==1 && output[1]==-1 && output[2]==2 && output[3]==-2);
    assert(ring.discard(1)==1);
    assert(ring.write(first,4)==3);
    assert(ring.render(output,6)==4);
    const float expected[]{4,-4,1,-1,2,-2,3,-3,0,0,0,0};
    for (unsigned i=0;i<12;++i) assert(output[i]==expected[i]);
    assert(ring.read()==ring.written());
    const auto consumed=ring.read();
    assert(ring.render(output,6)==0);
    assert(ring.read()==consumed);
    for (float value:output) assert(value==0);
}

void concurrentRenderAndDiscard() {
    constexpr unsigned capacity=256, iterations=100000;
    recomp::apple::StereoRing<capacity> ring;
    std::array<float,capacity*2> input{};
    std::atomic<bool> start{false},finished{false};
    std::thread consumer([&] {
        std::array<float,capacity*2> output{};
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        do {
            const auto consumed=ring.render(output.data(),capacity);
            assert(consumed==0 || consumed==capacity);
            for (unsigned i=0;i<capacity;++i) {
                assert(output[i*2]==-output[i*2+1]);
                if (consumed) assert(output[i*2]==output[0]);
            }
        } while(!finished.load(std::memory_order_acquire));
    });
    start.store(true,std::memory_order_release);
    for (unsigned generation=1;generation<=iterations;++generation) {
        for (unsigned i=0;i<capacity;++i) {
            input[i*2]=static_cast<float>(generation);
            input[i*2+1]=-input[i*2];
        }
        ring.discard(capacity);
        ring.write(input.data(),capacity);
    }
    finished.store(true,std::memory_order_release);
    consumer.join();
    assert(ring.read()<=ring.written());
    assert(ring.written()-ring.read()<=capacity);
}
}

int main() {
    fifoAndDiscard();
    concurrentRenderAndDiscard();
}
