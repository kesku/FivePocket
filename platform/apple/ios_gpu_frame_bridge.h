// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

// Optional, separate ABI. RecompPrivateTraceCallbacksV5 is unchanged.
// All pointers and Vulkan objects are borrowed only for the synchronous call.
// Accepted means destination fence + exact mapped-reference comparison completed;
// it is queue acceptance, not physical display completion.
#ifdef __cplusplus
extern "C" {
#endif
typedef enum RecompIosGpuResultV1 {
    RECOMP_IOS_GPU_INELIGIBLE = 0, // no transaction committed; CPU fallback allowed
    RECOMP_IOS_GPU_ACCEPTED = 1,
    RECOMP_IOS_GPU_TERMINAL = -1 // never CPU fallback/retry or reuse source
} RecompIosGpuResultV1;
typedef struct RecompIosGpuFrameV1 {
    uint32_t structureBytes, version;
    uint64_t reserved0, reserved1;
    uint32_t sequence;          // same owner as CPU V4 display sequence
    uint32_t reservedSequence;
    uint64_t deviceEpoch;       // nonzero runtime device lifetime
    uint64_t sourceGeneration;  // nonzero, advances on every source presentation
    uint64_t sourceFrame;       // renderer frame identity (not display sequence)
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    uint32_t width, height;
    VkImage image;
    VkImageView view;
    VkFormat format;
    VkImageLayout layout;
    VkImageUsageFlags usage;
    uint32_t sourceFenceComplete; // 1 only after actual current readback fence
    const uint8_t* mappedRgba;  // actual mapNativeScanout result, width*height*4
    uint64_t mappedBytes;
    uint64_t lutGeneration;     // native scaler/gamma finalized; immutable this run
    uint8_t nativeLut[3][256];  // exact RGB byte mapping, alpha forced opaque
} RecompIosGpuFrameV1;
typedef struct RecompIosGpuCallbacksV1 {
    uint32_t structureBytes, version;
    uint64_t reserved0, reserved1;
    void* user;
    // On entry only ACCEPTED or TERMINAL are legal. No source image or
    // CPU pointer may survive this synchronous call.
    int (*frame)(void*, const RecompIosGpuFrameV1*);
    // Synchronous drain/release borrowed-device resources, before device death.
    // terminal=1 means completion/source reuse is unproved: quarantine, no reuse.
    void (*close)(void*, uint32_t terminal);
} RecompIosGpuCallbacksV1;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
namespace recomp::ios_shared {
inline bool requested() {
    const char* v=std::getenv("RECOMP_IOS_SHARED_DEVICE_PRESENT");
    if(!v || std::strcmp(v,"0")==0)return false;
    if(std::strcmp(v,"1")==0)return true;
    throw std::invalid_argument("RECOMP_IOS_SHARED_DEVICE_PRESENT must be 0 or 1");
}
// Enabled once before worker creation. Callback publication is atomic and its
// removal is serialized by producerGate after guest work and display drain.
// An idle renderer worker may remain alive; enabled/gate pairing never changes.
using ProducerCallback=int(*)(void*,RecompIosGpuFrameV1&,const uint8_t*,uint64_t);
inline bool enabled=false; // set once before privateTraceEntry; never cleared
inline std::atomic<ProducerCallback> producer{nullptr};
inline std::atomic<void*> producerUser{nullptr};
inline std::recursive_mutex producerGate;
// Every rendererMutex acquisition participates; the present caller retains
// this gate after releasing rendererMutex and through the synchronous callback.
class RendererMutex final {
    std::mutex mutex_;
public:
    void lock(){if(enabled)producerGate.lock();try{mutex_.lock();}catch(...){if(enabled)producerGate.unlock();throw;}}
    bool try_lock(){if(enabled && !producerGate.try_lock())return false;if(mutex_.try_lock())return true;if(enabled)producerGate.unlock();return false;}
    void unlock(){mutex_.unlock();if(enabled)producerGate.unlock();}
};
}
#endif
