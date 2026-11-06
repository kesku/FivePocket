#pragma once
#include "native_color_chain_profile.h"
#include "texture32_image.h"

namespace recomp::gpu {
// Command recording for the bounded 1280x720 native color/depth/alias profile.
// This is not a PM4 decoder, residency manager, queue submission, fence waiter,
// reference oracle or guest publisher. Callers own matching resources/modules,
// make source bytes stable, and wait for prior work before resetting the pool.
// Stages: 0 initialize/empty pass, 1 first color pair, 2 second pair, 3 empty load.
// The fixture's optional perturbation fields remain explicit controls; the live
// caller must use the validated profile. No reference pixels enter this helper.
// Events occupy two guarded staging slots; an empty event list is also allowed.
template<bool StoreCopy,bool Initialize,bool NativeDepth,class R>
bool recordNativeColorChainStage(R& r,unsigned step,const NativeWordUpdatePlan& eventPlan) {
    static_assert(!Initialize || StoreCopy);
    static_assert(!NativeDepth || Initialize);
    if (step>3 || !r.configured()) return false;
    if constexpr (StoreCopy) if (!validNativeColorChainEvents(eventPlan)) return false;
    constexpr unsigned OutputBytes=1280*720*8+32;
    constexpr auto PipelineStage=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkMemoryBarrier before{};before.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    before.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|PipelineStage|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,1,&before,0,nullptr,0,nullptr);
    r.planes.upload(r.command,r.buffers[0],PipelineStage);
    recomp::gpu::uploadTexture32Image(r,r.command,r.buffers[1],PipelineStage);
    if constexpr (Initialize) {
        if(step==0){
            if constexpr (NativeDepth) {
                r.depth.record(r.command);
            }
            if(r.zeroInitial)vkCmdFillBuffer(r.command,r.buffers[2],16,OutputBytes-32,0);
            else {
                vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computePipelines[2]);
                vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computeLayout,0,1,&r.initialSet,0,nullptr);
                vkCmdDispatch(r.command,160,90,1);
            }
            VkMemoryBarrier toUpload{};toUpload.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toUpload.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;toUpload.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&toUpload,0,nullptr,0,nullptr);
            VkImageMemoryBarrier upload{};upload.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            upload.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;upload.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            upload.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;upload.image=r.colorImage;
            upload.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;upload.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            upload.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&upload);
            VkBufferImageCopy initial{};initial.bufferOffset=16;initial.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            initial.imageExtent={1280,720,1};vkCmdCopyBufferToImage(r.command,r.buffers[2],r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&initial);
            upload.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            upload.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            upload.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;upload.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0,0,nullptr,0,nullptr,1,&upload);
        }
    }
    VkClearValue clear{};for(auto& c:clear.color.float32)c=203.25f;
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass=step && !r.forceClear?r.loadPass:r.renderPass;pass.framebuffer=r.framebuffer;
    if constexpr (Initialize) {
        if(!r.forceClear)pass.renderPass=r.loadPass;
    }
    pass.renderArea={{0,0},{1280,720}};pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    if(step==1 || (step==2 && !r.skipSecond)){
        unsigned index=step-1;if(r.reversePairs)index=1-index;if(r.reuseFirst)index=0;
        const auto& p=r.pairs[index];vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,p.pipeline);
        vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.layout,0,1,&p.set,0,nullptr);
        vkCmdDraw(r.command,4,1,0,0);
    }
    vkCmdEndRenderPass(r.command);
    if constexpr (Initialize) {
        // The half buffer was read by the upload; its subsequent image readback
        // must not overwrite it while the upload is still consuming those bytes.
        VkMemoryBarrier toReadback{};toReadback.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toReadback.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;toReadback.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&toReadback,0,nullptr,0,nullptr);
    }
    VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={1280,720,1};
    vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffers[2],1,&copy);
    if constexpr (StoreCopy) {
        VkMemoryBarrier toCompute{};toCompute.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toCompute.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
        toCompute.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&toCompute,0,nullptr,0,nullptr);
        vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computeLayout,0,1,&r.computeSet,0,nullptr);
        if(!r.skipStore){vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computePipelines[0]);vkCmdDispatch(r.command,160,90,1);}
        toCompute.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&toCompute,0,nullptr,0,nullptr);
        if(!r.skipCopy){vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computePipelines[1]);vkCmdDispatch(r.command,160,90,1);}
        VkMemoryBarrier toEvents{};toEvents.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toEvents.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;toEvents.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&toEvents,0,nullptr,0,nullptr);
        for(std::size_t i=0;i<eventPlan.count;++i){const auto& w=eventPlan.words[i];
            vkCmdUpdateBuffer(r.command,r.buffers[5],w.byteOffset,4,w.bigEndianBytes.data());}
    }
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
    return vkEndCommandBuffer(r.command)==VK_SUCCESS;
}
}
