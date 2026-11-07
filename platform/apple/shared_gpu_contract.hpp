// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ios_gpu_frame_bridge.h"
#include <cstring>
#include <limits>
namespace recomp::apple::sharedpresent {
// Main-thread serialized metadata guard. The producer owns and freezes all
// borrowed bytes/objects until callback return; this class stores metadata only.
class Contract final {
 bool committed_=false,closed_=false;
 VkInstance instance_{};VkPhysicalDevice physical_{};VkDevice device_{};VkQueue queue_{};
 uint32_t family_=0;uint64_t epoch_=0,lutGeneration_=0,generation_=0,frame_=0;
 uint8_t lut_[3][256]{};
public:
 bool valid(const RecompIosGpuFrameV1&f)const noexcept {
  constexpr uint64_t bytes=1280ULL*720*4;
  if(closed_ || f.structureBytes!=sizeof(f)||f.version!=1||f.reserved0||f.reserved1||f.reservedSequence) return false;
  if(!f.instance||!f.physicalDevice||!f.device||!f.queue||!f.image||!f.view||!f.deviceEpoch||!f.sourceGeneration||!f.lutGeneration) return false;
  if(f.width!=1280||f.height!=720||f.format!=VK_FORMAT_R8G8B8A8_UNORM||f.layout!=VK_IMAGE_LAYOUT_GENERAL||f.sourceFenceComplete!=1) return false;
  if((f.usage&(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))!=(VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return false;
  if(!f.mappedRgba||f.mappedBytes!=bytes||reinterpret_cast<uintptr_t>(f.mappedRgba)>std::numeric_limits<uintptr_t>::max()-bytes) return false;
  if(committed_ && (f.instance!=instance_||f.physicalDevice!=physical_||f.device!=device_||f.queue!=queue_||f.queueFamily!=family_||f.deviceEpoch!=epoch_||f.lutGeneration!=lutGeneration_||std::memcmp(lut_,f.nativeLut,sizeof(lut_))||f.sourceGeneration<=generation_||f.sourceFrame<=frame_))return false;
  return true;
 }
 void adopt(const RecompIosGpuFrameV1&f)noexcept {committed_=true;instance_=f.instance;physical_=f.physicalDevice;device_=f.device;queue_=f.queue;family_=f.queueFamily;epoch_=f.deviceEpoch;lutGeneration_=f.lutGeneration;std::memcpy(lut_,f.nativeLut,sizeof(lut_));}
 void accepted(const RecompIosGpuFrameV1&f)noexcept{generation_=f.sourceGeneration;frame_=f.sourceFrame;}
 bool committed()const noexcept{return committed_;}
 void close()noexcept{closed_=true;}
};
}
