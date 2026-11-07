// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <MoltenVK/mvk_private_api.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace iosmvkpolicy {
inline int requested() {
    const char* value=std::getenv("RECOMP_IOS_MVK_EXPECT_SYNC");
    if(!value)return -1;
    if(!std::strcmp(value,"0"))return 0;
    if(!std::strcmp(value,"1"))return 1;
    return -2;
}
// Read the library's effective global settings; never mutate live configuration.
inline bool verify(const char* stage,PFN_vkGetMoltenVKConfigurationMVK get=vkGetMoltenVKConfigurationMVK) {
    const int expected=requested();
    if(expected==-1)return true;
    if(expected<0) {
        std::fprintf(stderr,"APPLE_IOS_MVK_POLICY_FAIL stage=%s reason=request\n",stage);
        return false;
    }
    MVKConfiguration config{};
    size_t copied=sizeof(config);
    const VkResult result=get(VK_NULL_HANDLE,&config,&copied);
    constexpr size_t required=offsetof(MVKConfiguration,prefillMetalCommandBuffers)+sizeof(config.prefillMetalCommandBuffers);
    if((result!=VK_SUCCESS && result!=VK_INCOMPLETE) || copied<required || copied>sizeof(config)) {
        std::fprintf(stderr,"APPLE_IOS_MVK_POLICY_FAIL stage=%s reason=layout result=%d bytes=%zu\n",stage,int(result),copied);
        return false;
    }
    const bool matched=config.synchronousQueueSubmits==unsigned(expected) && config.prefillMetalCommandBuffers==0;
    std::fprintf(stderr,"APPLE_IOS_MVK_POLICY stage=%s requested=%d effective=%u prefill=%u global=1 matched=%u\n",
        stage,expected,unsigned(config.synchronousQueueSubmits),unsigned(config.prefillMetalCommandBuffers),unsigned(matched));
    return matched;
}
}
