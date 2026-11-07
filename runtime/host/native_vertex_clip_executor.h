#pragma once
#include "device.h"
#include "native_vertex_clip.h"
#include "native_vertex_clip_program.h"
#include <cstdlib>
#include <memory>

namespace recomp::gpu {
// Execute the actual translated VS position expression over every owned input,
// fence, validate canaries, then classify. A non-clipped draw returns failure
// without touching any target. This never replaces arbitrary rendering with an
// empty submit. Code identity / compiler side-effect checks are the caller's
// responsibility. No PS sampling is needed for a proven fully clipped draw.
class NativeVertexClipExecutor {
    static constexpr unsigned InputBytes=8208+256*1024,OutputBytes=32+65536*16;
    ProbeDevice& device_;
    NativeClippedFanProgram program_;
    struct Resources {
        VkDevice d;std::array<VkBuffer,2> buffers{};std::array<VkDeviceMemory,2> memory{};std::array<void*,2> mapped{};
        VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkShaderModule shader{};VkPipeline pipeline{};
        VkDescriptorPool descriptors{};VkDescriptorSet set{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
            if(shader)vkDestroyShaderModule(d,shader,nullptr);
            if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
            if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
            if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
            for(unsigned i=0;i<2;++i){if(mapped[i])vkUnmapMemory(d,memory[i]);if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);if(memory[i])vkFreeMemory(d,memory[i],nullptr);}
        }
    } r_;
    NativeVertexClipExecutor(ProbeDevice& d,const NativeClippedFanProgram& p):device_(d),program_(p),r_{d.device}{}
    bool setup() {
        auto& r=r_;VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device_.physical,&props);
        for(unsigned i=0;i<2;++i) {
            VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=i?OutputBytes:InputBytes;b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if(vkCreateBuffer(r.d,&b,nullptr,&r.buffers[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.buffers[i],&req);
            unsigned type=0;const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            while(type<props.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (props.memoryTypes[type].propertyFlags&flags)!=flags))++type;
            if(type==props.memoryTypeCount)return false;
            VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=req.size;a.memoryTypeIndex=type;
            if(vkAllocateMemory(r.d,&a,nullptr,&r.memory[i])!=VK_SUCCESS || vkBindBufferMemory(r.d,r.buffers[i],r.memory[i],0)!=VK_SUCCESS ||
               vkMapMemory(r.d,r.memory[i],0,VK_WHOLE_SIZE,0,&r.mapped[i])!=VK_SUCCESS)return false;
        }
        std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;si.bindingCount=2;si.pBindings=bindings.data();
        if(vkCreateDescriptorSetLayout(r.d,&si,nullptr,&r.setLayout)!=VK_SUCCESS)return false;
        VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=1;li.pSetLayouts=&r.setLayout;
        if(vkCreatePipelineLayout(r.d,&li,nullptr,&r.layout)!=VK_SUCCESS)return false;
        VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=program_.positionCode.size()*4;mi.pCode=program_.positionCode.data();
        if(vkCreateShaderModule(r.d,&mi,nullptr,&r.shader)!=VK_SUCCESS)return false;
        VkComputePipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pi.layout=r.layout;
        pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=r.shader;pi.stage.pName="main";
        {
            // Most clipped draws build this executor afresh, so without the device's
            // compiled-shader cache the driver recompiled the same position shader for
            // every draw (attempt v4 profile: libllvm-qgl.so, the Adreno compiler, took
            // 25% of process CPU). The cache cannot change the pipeline; see ProbeDevice.
            std::lock_guard<std::mutex> cacheLock(device_.pipelineCacheMutex);
            if(vkCreateComputePipelines(r.d,device_.pipelineCache,1,&pi,nullptr,&r.pipeline)!=VK_SUCCESS)return false;
        }
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};VkDescriptorPoolCreateInfo pool{};
        pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        if(vkCreateDescriptorPool(r.d,&pool,nullptr,&r.descriptors)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ds.descriptorPool=r.descriptors;ds.descriptorSetCount=1;ds.pSetLayouts=&r.setLayout;
        if(vkAllocateDescriptorSets(r.d,&ds,&r.set)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cp.queueFamilyIndex=device_.queueFamily;
        if(vkCreateCommandPool(r.d,&cp,nullptr,&r.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=r.pool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        if(vkAllocateCommandBuffers(r.d,&ca,&r.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkCreateFence(r.d,&fi,nullptr,&r.fence)==VK_SUCCESS;
    }
public:
    static std::unique_ptr<NativeVertexClipExecutor> create(ProbeDevice& d,const NativeClippedFanProgram& p) {
        if(!d.device || !d.queue || !p.stride || p.stride%4 || p.stride>256 || p.positionCode.size()<5 ||
           p.positionCode.size()>65536 || p.positionCode[0]!=0x07230203)return nullptr;
        auto result=std::unique_ptr<NativeVertexClipExecutor>(new NativeVertexClipExecutor(d,p));
        return result->setup()?std::move(result):nullptr;
    }
    const NativeClippedFanProgram& program()const{return program_;}
    std::optional<std::vector<std::array<float,4>>> positions(const Gta5InlineDraw& draw) {
        if(!nativeClippedFanState(draw,program_.stride))return std::nullopt;
        return runPositions(draw);
    }
    std::optional<std::vector<std::array<float,4>>> fanPositions(const Gta5InlineDraw& draw) {
        // Raw readback only, not the older fan's state/exclusion admission.
        // A separate consumer must prove program identity, state and queries.
        if(draw.primitive!=5 || draw.vertexCount<3 || draw.vertexCount>65536 ||
           draw.stride!=program_.stride || draw.vertices.size()>256*1024 ||
           draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride)return std::nullopt;
        return runPositions(draw);
    }
    std::optional<std::vector<std::array<float,4>>> quadPositions(const Gta5InlineDraw& draw) {
        if(!nativeQuadPositionState(draw,program_.stride))return std::nullopt;
        return runPositions(draw);
    }
    std::optional<std::vector<std::array<float,4>>> rectanglePositions(const Gta5InlineDraw& draw) {
        // Raw compute only; the cube rectangle consumer validates raster state,
        // all original outputs, texture and ownership before any target write.
        if(draw.primitive!=8 || draw.vertexCount<3 || draw.vertexCount>384 || draw.vertexCount%3 ||
           draw.stride!=program_.stride || draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride)return std::nullopt;
        return runPositions(draw);
    }
    std::optional<std::vector<std::array<float,4>>> stripPositions(const Gta5InlineDraw& draw) {
        // Position readback ONLY. This neither expands/rasterizes a strip nor
        // admits its interpolation, depth/stencil, blending or shader identity.
        if(draw.primitive!=6 || draw.vertexCount<3 || draw.vertexCount>65536 ||
           draw.stride!=program_.stride || draw.vertices.size()>256*1024 ||
           draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride)return std::nullopt;
        return runPositions(draw);
    }
    std::optional<std::vector<std::array<float,4>>> indexedPositions(const Gta5InlineDraw& draw) {
        // Position readback only. The scene consumer separately proves shader
        // identity, original raster controls, index extent and query semantics.
        if(draw.primitive!=4 || draw.vertexCount<3 || draw.vertexCount>65536 ||
           // The captured zero-depth mesh uses float3 + packed16x4 (20B).
           // This extends position READBACK only; the independent draw consumer
           // still owns shader/state/texture admission and all target effects.
           draw.stride!=program_.stride || (draw.stride!=20 && draw.stride!=24 && draw.stride!=28 && draw.stride!=36 && draw.stride!=40 && draw.stride!=44) ||
           draw.vertices.size()>256*1024 ||
           draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride)return std::nullopt;
        return runPositions(draw);
    }
private:
    std::optional<std::vector<std::array<float,4>>> runPositions(const Gta5InlineDraw& draw) {
        auto& r=r_;std::vector<std::uint8_t> input(8208+draw.vertices.size());
        const auto put=[&](unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)input[at+i]=v>>(24-8*i);};
        put(0,draw.vertexCount);
        for(unsigned i=0;i<1024;++i){put(16+i*4,draw.vertexConstants[i]);put(4112+i*4,draw.pixelConstants[i]);}
        std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
        std::memcpy(r.mapped[0],input.data(),input.size());std::memset(r.mapped[1],0x5A,OutputBytes);
        const auto outputBytes=32+draw.vertexCount*16;
        std::array<VkDescriptorBufferInfo,2> buffers{{{r.buffers[0],0,input.size()},{r.buffers[1],0,outputBytes}}};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(unsigned i=0;i<2;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=r.set;w.dstBinding=i;
            w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.descriptorCount=1;w.pBufferInfo=&buffers[i];}
        vkUpdateDescriptorSets(r.d,2,writes.data(),0,nullptr);
        if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return std::nullopt;
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return std::nullopt;
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
        vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);
        vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&r.set,0,nullptr);
        vkCmdDispatch(r.command,(draw.vertexCount+63)/64,1,1);
        host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return std::nullopt;
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
        if(vkQueueSubmit(device_.queue,1,&submit,r.fence)!=VK_SUCCESS || vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        const auto* out=static_cast<const std::uint8_t*>(r.mapped[1]);
        if(std::memcmp(r.mapped[0],input.data(),input.size()) || !std::all_of(out,out+16,[](auto b){return b==0x5A;}) ||
           !std::all_of(out+outputBytes-16,out+OutputBytes,[](auto b){return b==0x5A;}))return std::nullopt;
        std::vector<std::array<float,4>> result(draw.vertexCount);std::memcpy(result.data(),out+16,result.size()*16);
        // A sentinel left by a missing invocation must never look like a valid
        // far-out position and accidentally pass the clipping classification.
        for(const auto& p:result)for(float f:p){std::uint32_t bits=0;std::memcpy(&bits,&f,4);if(bits==0x5A5A5A5A || !std::isfinite(f))return std::nullopt;}
        return result;
    }
public:
    std::optional<unsigned> execute(const Gta5InlineDraw& draw) {
        const auto result=positions(draw);
        return result?nativeClippedFanOutside(draw,*result):std::nullopt;
    }
    std::optional<NativeClippedQuadList> executeQuads(const Gta5InlineDraw& draw) {
        const auto result=quadPositions(draw);
        return result?nativeClippedQuadListOutside(draw,*result):std::nullopt;
    }
};
}
