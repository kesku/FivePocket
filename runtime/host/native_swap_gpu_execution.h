#pragma once
#include "native_swap_completion.h"
#include "device.h"
#include <algorithm>
#include <functional>
#include <cstdlib>
#include <cstdio>

namespace recomp::gpu {
// Native compatibility policy: sample a shared monotonic HOST session clock
// after actual native queue fences, then GPU-write/audit those samples at
// 500MHz modulo32. Not Xbox timing, GPU timestamp queries, vblank or swap count.
// The caller must preflight all guest owners and model the idle/cache pair,
// post-swap invalidation and state commit. This helper owns no guest resource
// cache. Present callback must never return false/throw after queue acceptance.
inline bool executeNativeSwapGpuWrapper(ProbeDevice& context,const NativeSwapWrapper& plan,
        const NativeCompletionClock& clock,const std::function<bool()>& present,NativeSwapGpuResult& output){
    if(!context.device || !context.physical || !context.queue || !clock || !present)return false;
    struct Resources {
        VkDevice device;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
        VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void* mapped=nullptr;
        ~Resources(){
            if(mapped)vkUnmapMemory(device,memory);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(pool)vkDestroyCommandPool(device,pool,nullptr);
            if(buffer)vkDestroyBuffer(device,buffer,nullptr);
            if(memory)vkFreeMemory(device,memory,nullptr);
        }
    } r{context.device};
    bool queued=false;
    const auto fail=[&]{if(queued){std::fflush(stdout);std::_Exit(4);}return false;};
    try {
        VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=context.queueFamily;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size=4096;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateCommandPool(context.device,&pool,nullptr,&r.pool)!=VK_SUCCESS ||
            vkCreateFence(context.device,&fence,nullptr,&r.fence)!=VK_SUCCESS ||
            vkCreateBuffer(context.device,&buffer,nullptr,&r.buffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(context.device,r.buffer,&requirements);
        VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(context.physical,&properties);
        unsigned memoryType=UINT32_MAX;
        for(unsigned i=0;i<properties.memoryTypeCount;++i){constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if((requirements.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&flags)==flags){memoryType=i;break;}}
        if(memoryType==UINT32_MAX)return false;
        VkMemoryAllocateInfo memory{};memory.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memory.allocationSize=requirements.size;memory.memoryTypeIndex=memoryType;
        if(vkAllocateMemory(context.device,&memory,nullptr,&r.memory)!=VK_SUCCESS ||
            vkBindBufferMemory(context.device,r.buffer,r.memory,0)!=VK_SUCCESS ||
            vkMapMemory(context.device,r.memory,0,requirements.size,0,&r.mapped)!=VK_SUCCESS)return false;
        // All resources and four command buffers exist before the first queue
        // operation. No allocator work may turn accepted presentation into retry.
        std::array<VkCommandBuffer,4> commands{};
        VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool=r.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=commands.size();
        if(vkAllocateCommandBuffers(context.device,&allocate,commands.data())!=VK_SUCCESS)return false;
        NativeSwapGpuResult candidate;
        const auto serialize=[&](unsigned index,std::uint32_t value){const auto endian=plan.events()[index].endian;
            for(unsigned lane=0;lane<4;++lane)candidate.eventBytes[index*4+lane]=value>>(endian==0?lane*8:24-lane*8);};
        for(unsigned i=2;i<4;++i){if(!plan.events()[i].literal)return false;serialize(i,*plan.events()[i].literal);}
        const auto batch=[&](unsigned index,unsigned first,unsigned count)->bool {
            auto command=commands[index];std::array<std::uint8_t,4096> expected{};expected.fill(0x5A);
            std::memset(r.mapped,0x5A,4096);
            VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS)return false;
            VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            for(unsigned i=first;i<first+count;++i){
                alignas(4) std::array<std::uint8_t,4> bytes{};
                std::copy_n(candidate.eventBytes.data()+i*4,4,bytes.data());
                std::copy(bytes.begin(),bytes.end(),expected.begin()+16+i*4);
                vkCmdUpdateBuffer(command,r.buffer,16+i*4,4,bytes.data());
            }
            barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            if(vkEndCommandBuffer(command)!=VK_SUCCESS || vkResetFences(context.device,1,&r.fence)!=VK_SUCCESS)return false;
            VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
            if(vkQueueSubmit(context.queue,1,&submit,r.fence)!=VK_SUCCESS ||
                vkWaitForFences(context.device,1,&r.fence,VK_TRUE,2'000'000'000)!=VK_SUCCESS){
                std::fflush(stdout);std::_Exit(4); // Unknown in-flight work cannot be retried/destroyed.
            }
            ++candidate.nativeFences;
            return std::memcmp(r.mapped,expected.data(),expected.size())==0;
        };
        if(!batch(0,0,0))return false; // Real prefix idle/order completion before sampling.
        const auto before=clock();if(!before)return false;
        candidate.beforeCompletionNs=*before;serialize(0,static_cast<std::uint32_t>(*before/2));
        if(!batch(1,0,1))return false; // GPU counter-word write plus full staging audit.
        queued=present();if(!queued)return false;
        if(!batch(2,0,0))return fail();
        const auto after=clock();
        // The guest observed consumer unwraps a single32bit interval. Reject a
        // backwards source or more than one full wrap, never invent a sample.
        if(!after || *after<*before || *after-*before>=8'589'934'592ULL)return fail();
        candidate.afterCompletionNs=*after;serialize(1,static_cast<std::uint32_t>(*after/2));
        if(!batch(3,1,3))return fail();
        output=candidate;return true; // Guest/state publication belongs to caller.
    }catch(...){return fail();}
}
}
