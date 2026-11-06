#pragma once
#include "native_geometry_sampler_plan.h"
#include "native_texture_image.h"

namespace recomp::gpu {
// Owns only the sampler. Image and device must outlive every queued use;
// caller must fence draws before destroying this object. No guest retirement.
class NativeGeometrySampler {
    VkDevice device_;VkSampler sampler_{};
    NativeGeometrySampler(VkDevice d,VkSampler s):device_(d),sampler_(s){}
public:
    ~NativeGeometrySampler(){if(sampler_)vkDestroySampler(device_,sampler_,nullptr);}
    NativeGeometrySampler(const NativeGeometrySampler&)=delete;
    NativeGeometrySampler& operator=(const NativeGeometrySampler&)=delete;
    VkSampler sampler()const{return sampler_;}
    static std::unique_ptr<NativeGeometrySampler> create(ProbeDevice& device,const NativeTextureImage& image) {
        const auto p=planNativeGeometrySampler2D(image.description().fetch);
        if(!p || !device.device || !image.view() || image.layerCount()!=1 ||
           image.levelCount()!=p->sampling.maxLevel+1 ||
           ((p->sampling.magFilter || p->sampling.minFilter || p->anisotropy) && !image.supportsLinearFiltering()) ||
           (p->anisotropy && (!device.samplerAnisotropyEnabled ||
               p->maxAnisotropy>device.properties.limits.maxSamplerAnisotropy)))return nullptr;
        const auto& s=p->sampling;
        VkSamplerCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter=s.magFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        ci.minFilter=s.minFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        ci.mipmapMode=s.mipFilter?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
        constexpr VkSamplerAddressMode clamp[]{VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        ci.addressModeU=clamp[s.clamp[0]];ci.addressModeV=clamp[s.clamp[1]];ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod=float(s.maxLevel);ci.anisotropyEnable=p->anisotropy!=0;ci.maxAnisotropy=float(p->maxAnisotropy);
        VkSampler sampler{};
        if(vkCreateSampler(device.device,&ci,nullptr,&sampler)!=VK_SUCCESS)return nullptr;
        return std::unique_ptr<NativeGeometrySampler>(new NativeGeometrySampler(device.device,sampler));
    }
};
}
