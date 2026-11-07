#pragma once
#include "native_exposure_scalar_plan.h"
#include "native_translated_resources.h"
#include "native_texture_image.h"
#include "native_edram32_bands.h"
#include "texture_fragment_target.h"
#include <cstdlib>

namespace recomp::gpu {
// Single full-replacement RGBA16F texel. No clear, CPU half conversion, blending,
// presentation or guest completion. The caller must hold exclusive queue/state
// access; physical publication happens only after the fenced native readback.
inline std::optional<std::vector<std::uint8_t>> renderNativeExposureScalar(
        ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const NativeExposureScalarPlan& plan,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<std::vector<std::uint8_t>>{};};
    if(!validateNativeExposureScalarProgram(program) || draw.vertices.size()!=72 || draw.textures.size()!=1 ||
       plan.target.width!=1 || plan.target.height!=1 || plan.target.format!=7)return no("exposure-render-input");
    auto image=NativeTextureImage::create(device,plan.submitted,draw.textures[0].source);
    if(!image)return no("exposure-r32-import");
    struct Resources {
        VkDevice d;VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};
        VkRenderPass renderPass{};VkFramebuffer framebuffer{};VkSampler sampler{};
        VkBuffer output{};VkDeviceMemory outputMemory{};void* mapped=nullptr;
        VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        ~Resources(){
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(sampler)vkDestroySampler(d,sampler,nullptr);
            if(mapped)vkUnmapMemory(d,outputMemory);
            if(output)vkDestroyBuffer(d,output,nullptr);
            if(outputMemory)vkFreeMemory(d,outputMemory,nullptr);
            if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
            if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
            if(colorView)vkDestroyImageView(d,colorView,nullptr);
            if(colorImage)vkDestroyImage(d,colorImage,nullptr);
            if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        }
    } r{device.device};
    FragmentTargetConfig config{1,1,VK_FORMAT_R16G16B16A16_SFLOAT};config.fullOverwrite=true;
    if(!setupTextureFragmentTarget(device,r,config))return no("exposure-half-target");
    VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(r.d,&sampler,nullptr,&r.sampler)!=VK_SUCCESS)return no("exposure-point-sampler");
    NativeTranslatedPipelineOptions options;
    options.depthStencil=decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0);
    options.externalTextures={{r.sampler,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    NativeTranslatedResources pipeline(r.d);
    if(!pipeline.setup(device,r.renderPass,program,{plan.submitted},NativeTranslatedBlend::Replace,options))return no("exposure-pipeline");
    auto* input=static_cast<std::uint8_t*>(pipeline.inputMapped);
    const auto put=[&](unsigned at,unsigned v){for(unsigned i=0;i<4;++i)input[at+i]=v>>(24-i*8);};
    put(0,3);put(4,0);put(8,0);put(12,0);
    for(unsigned i=0;i<1024;++i){put(16+4*i,draw.vertexConstants[i]);put(4112+4*i,draw.pixelConstants[i]);}
    std::memcpy(input+8208,draw.vertices.data(),72);
    VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;buffer.size=40;buffer.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(vkCreateBuffer(r.d,&buffer,nullptr,&r.output)!=VK_SUCCESS)return no("exposure-readback-buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.output,&req);
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
    unsigned type=props.memoryTypeCount;constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(unsigned i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(type==props.memoryTypeCount)return no("exposure-readback-memory-type");
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
    if(vkAllocateMemory(r.d,&allocation,nullptr,&r.outputMemory)!=VK_SUCCESS || vkBindBufferMemory(r.d,r.output,r.outputMemory,0)!=VK_SUCCESS ||
       vkMapMemory(r.d,r.outputMemory,0,VK_WHOLE_SIZE,0,&r.mapped)!=VK_SUCCESS)return no("exposure-readback-memory");
    std::memset(r.mapped,0x5A,40);
    VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device.queueFamily;
    if(vkCreateCommandPool(r.d,&pool,nullptr,&r.pool)!=VK_SUCCESS)return no("exposure-command-pool");
    VkCommandBufferAllocateInfo commands{};commands.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool=r.pool;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commands.commandBufferCount=1;
    if(vkAllocateCommandBuffers(r.d,&commands,&r.command)!=VK_SUCCESS)return no("exposure-command-buffer");
    VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if(vkCreateFence(r.d,&fence,nullptr,&r.fence)!=VK_SUCCESS)return no("exposure-fence");
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return no("exposure-command-begin");
    VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;pass.renderPass=r.renderPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{1,1}};
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.pipeline);
    const VkRect2D scissor{{0,0},{1,1}};vkCmdSetScissor(r.command,0,1,&scissor);
    vkCmdPushConstants(r.command,pipeline.layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,plan.viewport.data());
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.layout,0,1,&pipeline.set,0,nullptr);
    vkCmdDraw(r.command,6,1,0,0);vkCmdEndRenderPass(r.command);
    VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={1,1,1};
    vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.output,1,&copy);
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return no("exposure-command-end");
    const auto* bytes=static_cast<const std::uint8_t*>(r.mapped);
    if(vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY || !std::all_of(bytes,bytes+40,[](auto b){return b==0x5A;}))return no("exposure-before-submit");
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS || vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
    if(!std::all_of(bytes,bytes+16,[](auto b){return b==0x5A;}) || !std::all_of(bytes+24,bytes+40,[](auto b){return b==0x5A;}))return no("exposure-readback-canary");
    for(unsigned i=0;i<4;++i)if(((unsigned{bytes[16+i*2]}|(unsigned{bytes[17+i*2]}<<8))&0x7C00)==0x7C00)return no("exposure-nonfinite-half");
    if(why)*why="fenced-exposure-rgba16f";
    return std::vector<std::uint8_t>(bytes+16,bytes+24);
}
inline bool executeNativeExposureScalarDraw(ProbeDevice& device,const NativeTranslatedDrawProgram& program,
        const Gta5InlineDraw& draw,const Gta5BoundSurfaceCapture& bound,
        const std::vector<std::uint8_t>& currentVertex,const std::vector<std::uint8_t>& currentPixel,
        bool queriesIdle,NativeEdram32Mutation& mutation,const char** why=nullptr) {
    const auto plan=planNativeExposureScalarDraw(draw,bound,program,currentVertex,currentPixel,false,queriesIdle,why);
    if(!plan)return false;
    const auto bytes=renderNativeExposureScalar(device,program,draw,*plan,why);if(!bytes)return false;
    if(!recordCompletedNativeColorDraw(mutation,plan->target,*bytes,NativeEdram64Model::Xenia40x16)) {
        if(why)*why="exposure-physical64-replacement";return false;
    }
    mutation.commit();if(why)*why="fenced-exposure-physical64";return true;
}
}
