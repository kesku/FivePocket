#pragma once
#include "xenos_maintenance_point_plan.h"
#include "vulkan_constant_points.h"
#include "native_constant_shaders.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recomp::gpu {
struct NativeMaintenancePointResult {
    MaintenancePointPlan plan;
    NativeInitializationReadback readback;
};
// Executes ONLY the proved point/barrier/event portion of an independent
// snapshot. Caller still owns retained register/cache state, resource leases,
// exclusive scheduling and guest publication. Never consumes the guest ring.
// Revalidates raw snapshots, not a caller-mutable plan. Every call records fresh
// GPU work and audits a private buffer; no guest address reaches Vulkan.
inline bool executeNativeMaintenancePoints(ProbeDevice& device,const Pm4Snapshot& root,
        const Pm4IndirectGraph& graph,NativeMaintenancePointResult& output,
        MaintenancePointProfile profile=MaintenancePointProfile::LeadingIdle) {
    using namespace constant_points;
    NativeMaintenancePointResult candidate;MaintenancePointError error;
    if(!device.device || !device.queue ||
       !planMaintenancePointsProfile(root,graph,candidate.plan,error,true,profile))return false;
    const auto& plan=candidate.plan;
    std::array<NativeConstantPointInputs,128> inputs;
    for(std::size_t i=0;i<plan.points.size();++i)
        if(!packNativeConstantPointInputs(plan.points[i].pair,inputs[i]))return false;
    std::array<std::uint8_t,256> expected;expected.fill(0x5A);
    for(std::size_t i=0;i<plan.updates.count;++i) {
        const auto& word=plan.updates.words[i];
        std::copy(word.bigEndianBytes.begin(),word.bigEndianBytes.end(),expected.begin()+word.byteOffset);
    }
    Resources r(device.device,256);
    if(!setup(device,r,genericConstantShaderCode(),true) || !pipeline(r,0))return false;
    std::memset(r.mapped,0x5A,Bytes+256);
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    const auto dependency=[&](VkPipelineStageFlags from,VkPipelineStageFlags to,VkAccessFlags src,VkAccessFlags dst) {
        VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask=src;barrier.dstAccessMask=dst;
        vkCmdPipelineBarrier(r.command,from,to,0,1,&barrier,0,nullptr,0,nullptr);
    };
    dependency(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    unsigned drawn=0;
    const auto pointsThrough=[&](unsigned end) {
        if(end==drawn)return;
        // The game points provably cannot modify any color/depth/query or shader
        // memory. Use a private throwaway attachment, NEVER a guest target.
        // Splitting at a barrier clears only this private attachment again.
        VkClearValue clear{};for(auto& c:clear.color.float32)c=1;
        VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass=r.pass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{Side,Side}};
        pass.clearValueCount=1;pass.pClearValues=&clear;
        vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipeline);
        for(;drawn<end;++drawn) {
            vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(inputs[drawn]),&inputs[drawn]);
            vkCmdDraw(r.command,1,1,0,0);
        }
        vkCmdEndRenderPass(r.command);
    };
    for(const auto& barrier:plan.barriers) {
        pointsThrough(barrier.precedingPoints);
        // Conservative full dependency for source idle/INVALIDATE_STATE and
        // TILE_FLUSH. Loaded state remains CPU-owned, each segment binds fresh
        // native constants; no claim of Xenos cache-status register fidelity.
        dependency(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    }
    pointsThrough(static_cast<unsigned>(plan.points.size()));
    dependency(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    for(std::size_t i=0;i<plan.updates.count;++i) {
        const auto& w=plan.updates.words[i];alignas(4) const auto data=w.bigEndianBytes;
        vkCmdUpdateBuffer(r.command,r.buffer,Bytes+w.byteOffset,4,data.data());
    }
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={Side,Side,1};
    vkCmdCopyImageToBuffer(r.command,r.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffer,1,&copy);
    dependency(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS || vkGetFenceStatus(r.device,r.fence)!=VK_NOT_READY)return false;
    for(unsigned i=0;i<Bytes+256;++i)if(static_cast<const std::uint8_t*>(r.mapped)[i]!=0x5A)return false;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
       vkWaitForFences(r.device,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS) {
        std::fflush(stdout);std::_Exit(4); // Never destroy potentially in-flight resources.
    }
    const auto* bytes=static_cast<const std::uint8_t*>(r.mapped);
    for(unsigned i=0;i<Bytes;++i)if(bytes[i]!=255)return false;
    AuditedNativeWords audited;NativeWordReadbackFailure auditError;
    if(std::memcmp(bytes+Bytes,expected.data(),expected.size()) ||
       !auditNativeWordReadback(plan.updates,bytes+Bytes,plan.updates.bufferBytes,audited,auditError))return false;
    candidate.readback.byteCount=plan.updates.bufferBytes;
    candidate.readback.draws=static_cast<unsigned>(plan.points.size());
    std::copy_n(bytes+Bytes,plan.updates.bufferBytes,candidate.readback.bytes.begin());
    output=std::move(candidate);return true;
}
}
