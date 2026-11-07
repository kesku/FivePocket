#pragma once
#include "native_ordered_fragment7e3_stream.h"
#include "texture_fragment_target.h"
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <memory>

namespace recomp::gpu {
// ABI33 modules only. This type does not establish original shader identity,
// current resource ownership, finite original positions or guest raster state.
struct NativeOrderedFragmentRasterProgram {
    std::vector<std::uint32_t> vertex,pixel;
};
namespace ordered_fragment_detail {
struct NativeOrderedRasterResources {
    VkDevice d;std::array<VkBuffer,3> buffers{};std::array<VkDeviceMemory,3> memory{};std::array<void*,3> mapped{};
    VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkShaderModule vertexShader{},shader{};VkPipeline pipeline{};
    VkDescriptorPool descriptors{};VkDescriptorSet set{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};VkRenderPass renderPass{};VkFramebuffer framebuffer{};
    ~NativeOrderedRasterResources() {
        if(fence)vkDestroyFence(d,fence,nullptr);if(pool)vkDestroyCommandPool(d,pool,nullptr);
        if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);if(vertexShader)vkDestroyShaderModule(d,vertexShader,nullptr);if(shader)vkDestroyShaderModule(d,shader,nullptr);
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
        if(colorView)vkDestroyImageView(d,colorView,nullptr);if(colorImage)vkDestroyImage(d,colorImage,nullptr);if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        for(unsigned i=0;i<3;++i){if(mapped[i])vkUnmapMemory(d,memory[i]);if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);if(memory[i])vkFreeMemory(d,memory[i],nullptr);}
    }
};
template<class R> bool createOrderedFragmentPipeline(ProbeDevice& gpu,R& r,bool strip=false,FragmentTargetConfig config={}) {
    if(!config.valid())return false;
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module=i?r.shader:r.vertexShader;stages[i].pName="main";}
    VkPipelineVertexInputStateCreateInfo input{};input.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology=strip?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,float(config.width),float(config.height),0,1};
    if (config.viewportMode==FragmentViewportMode::Color1280ShaderHalfPixel) {
        // Matching translated VS applies Y and the half-pixel offset itself.
        viewport={0,0,2048,1024,1,0};
    } else if (config.viewportMode!=FragmentViewportMode::DiagnosticPositive) {
        const float origin=config.viewportMode==FragmentViewportMode::Color1280NegativeYHalfPixel?.5f:0.f;
        viewport={origin,float(config.height)+origin,float(config.width),-float(config.height),1,0};
    }
    VkRect2D scissor{{0,0},{config.width,config.height}};
    VkPipelineViewportStateCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount=1;view.pViewports=&viewport;view.scissorCount=1;view.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_BACK_BIT;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{};samples.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=15;
    if(config.alphaBlend){
        color.blendEnable=VK_TRUE;
        color.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.colorBlendOp=VK_BLEND_OP_ADD;
        color.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.alphaBlendOp=VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};blend.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount=1;blend.pAttachments=&color;
    VkGraphicsPipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    const VkDynamicState dynamicState=VK_DYNAMIC_STATE_SCISSOR;
    VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&dynamicState;info.pDynamicState=&dynamic;
    info.stageCount=stages.size();info.pStages=stages.data();info.pVertexInputState=&input;
    info.pInputAssemblyState=&assembly;info.pViewportState=&view;info.pRasterizationState=&raster;
    info.pMultisampleState=&samples;info.pColorBlendState=&blend;info.layout=r.layout;info.renderPass=r.renderPass;
    std::lock_guard<std::mutex> lock(gpu.pipelineCacheMutex);
    return vkCreateGraphicsPipelines(r.d,gpu.pipelineCache,1,&info,nullptr,&r.pipeline)==VK_SUCCESS;
}
} // namespace ordered_fragment_detail
// Single-sample1280x720, reverse depth, original shader XY viewport transform,
// CCW/back-cull, <=4 triangle primitives, stride28. Caller owns the queue gate
// and every borrowed image/sampler until this function fences. No live target
// is touched. Failed setup/overflow/input mutation returns no batch. An unknown
// submitted fence is fatal (as in the existing native executors): never destroy
// resources still used by the GPU or acknowledge unfinished guest work.
// One immutable input/texture binding and one pipeline per executor. Repeated
// spatial windows may reuse them ONLY after each previous submission is fenced.
// Borrowed texture handles must outlive this local executor. No cross-draw cache
// or ownership inference; the original caller still admits the guest state.
class NativeOrderedFragmentRasterExecutor final {
public:
    NativeOrderedFragmentRasterExecutor(const NativeOrderedFragmentRasterExecutor&)=delete;
    NativeOrderedFragmentRasterExecutor& operator=(const NativeOrderedFragmentRasterExecutor&)=delete;
    static std::unique_ptr<NativeOrderedFragmentRasterExecutor> create(
        ProbeDevice& gpu,const NativeOrderedFragmentRasterProgram& program,
        std::span<const std::uint8_t> input,std::span<const std::uint32_t> indices,
        const std::array<float,4>& viewport,const std::array<VkDescriptorImageInfo,5>& textures,
        unsigned capacity=OrderedFragment7e3MaxCapacity,const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::unique_ptr<NativeOrderedFragmentRasterExecutor>{};};
    if(!gpu.device || !gpu.queue || !gpu.fragmentStoresAndAtomicsEnabled ||
       !capacity || capacity>OrderedFragment7e3MaxCapacity || input.size()<8208+3*28 ||
       input.size()>8208+256*1024 || (input.size()-8208)%28 ||
       indices.empty() || indices.size()>12 || indices.size()%3)
        return no("ordered-raster-input-contract");
    const unsigned vertices=(unsigned(input[0])<<24)|(unsigned(input[1])<<16)|(unsigned(input[2])<<8)|input[3];
    if(vertices!=(input.size()-8208)/28)return no("ordered-raster-vertex-extent");
    for(auto index:indices)if(index>=vertices)return no("ordered-raster-index-range");
    for(auto v:viewport)if(!std::isfinite(v))return no("ordered-raster-viewport");
    for(const auto* code:{&program.vertex,&program.pixel})if(code->size()<5 || code->size()>262144 || code->front()!=0x07230203)
        return no("ordered-raster-program");
    for(const auto& t:textures)if(!t.sampler || !t.imageView || t.imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return no("ordered-raster-borrowed-texture");
    if(gpu.properties.limits.maxPerStageDescriptorSamplers<5 || gpu.properties.limits.maxPerStageDescriptorStorageBuffers<2 ||
       gpu.properties.limits.maxPushConstantsSize<24)return no("ordered-raster-device-limits");
    auto executor=std::unique_ptr<NativeOrderedFragmentRasterExecutor>(
        new NativeOrderedFragmentRasterExecutor(gpu,input,viewport,capacity,unsigned(indices.size())));
    auto& r=executor->resources_;
    const unsigned records=capacity,wordsPerRecord=10,traceBytes=(8+records*wordsPerRecord)*4;
    if(!(gpu.properties.limits.maxStorageBufferRange>=traceBytes))return no("ordered-raster-check-1");
    const std::array<unsigned,3> sizes{unsigned(input.size()),traceBytes,unsigned(indices.size_bytes())};
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(gpu.physical,&props);
    for(unsigned i=0;i<3;++i) {
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=sizes[i];b.usage=i==2?VK_BUFFER_USAGE_INDEX_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(!(vkCreateBuffer(r.d,&b,nullptr,&r.buffers[i])==VK_SUCCESS))return no("ordered-raster-check-2");
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.buffers[i],&req);
        const auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;unsigned type=0;
        while(type<props.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || (props.memoryTypes[type].propertyFlags&flags)!=flags))++type;if(!(type<props.memoryTypeCount))return no("ordered-raster-check-3");
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=req.size;a.memoryTypeIndex=type;
        if(!(vkAllocateMemory(r.d,&a,nullptr,&r.memory[i])==VK_SUCCESS && vkBindBufferMemory(r.d,r.buffers[i],r.memory[i],0)==VK_SUCCESS && vkMapMemory(r.d,r.memory[i],0,VK_WHOLE_SIZE,0,&r.mapped[i])==VK_SUCCESS))return no("ordered-raster-check-4");
    }
    std::memcpy(r.mapped[0],input.data(),input.size());std::memcpy(r.mapped[2],indices.data(),indices.size_bytes());
    auto* trace=static_cast<unsigned*>(r.mapped[1]);std::fill(trace,trace+traceBytes/4,0x5A5A5A5A);trace[4]=trace[5]=0;
    std::array<VkDescriptorSetLayoutBinding,7> bindings{};
    bindings[0]={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    for(unsigned i=0;i<5;++i)bindings[i+1]={i+1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    bindings[6]={6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;sl.bindingCount=7;sl.pBindings=bindings.data();if(!(vkCreateDescriptorSetLayout(r.d,&sl,nullptr,&r.setLayout)==VK_SUCCESS))return no("ordered-raster-check-5");
    std::array<VkPushConstantRange,2> push{{{VK_SHADER_STAGE_VERTEX_BIT,0,16},{VK_SHADER_STAGE_FRAGMENT_BIT,16,8}}};VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=1;li.pSetLayouts=&r.setLayout;li.pushConstantRangeCount=2;li.pPushConstantRanges=push.data();if(!(vkCreatePipelineLayout(r.d,&li,nullptr,&r.layout)==VK_SUCCESS))return no("ordered-raster-check-6");
    for(unsigned i=0;i<2;++i) {const auto& code=i?program.pixel:program.vertex;
        VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=code.size()*4;mi.pCode=code.data();
        if(vkCreateShaderModule(r.d,&mi,nullptr,i?&r.shader:&r.vertexShader)!=VK_SUCCESS)return no("ordered-raster-shader-module");}
    const FragmentTargetConfig config{1280,720,VK_FORMAT_R32G32B32A32_SFLOAT,false,false,FragmentViewportMode::Color1280ShaderHalfPixel,false};
    if(!(setupTextureFragmentTarget(gpu,r,config) && ordered_fragment_detail::createOrderedFragmentPipeline(gpu,r,false,config)))return no("ordered-raster-check-7");
    const std::array<VkDescriptorPoolSize,2> poolSizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,5}}};
    VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=poolSizes.data();if(!(vkCreateDescriptorPool(r.d,&dp,nullptr,&r.descriptors)==VK_SUCCESS))return no("ordered-raster-check-8");
    VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ds.descriptorPool=r.descriptors;ds.descriptorSetCount=1;ds.pSetLayouts=&r.setLayout;if(!(vkAllocateDescriptorSets(r.d,&ds,&r.set)==VK_SUCCESS))return no("ordered-raster-check-9");
    std::array<VkDescriptorBufferInfo,2> buffers{{{r.buffers[0],0,sizes[0]},{r.buffers[1],0,sizes[1]}}};
    std::array<VkDescriptorImageInfo,5> textureInfo{};std::array<VkWriteDescriptorSet,7> writes{};
    for(unsigned i=0;i<7;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=r.set;w.dstBinding=i;w.descriptorCount=1;
        if(i==0 || i==6){w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&buffers[i==6];}
        else {textureInfo[i-1]=textures[i-1];w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&textureInfo[i-1];}}
    vkUpdateDescriptorSets(r.d,7,writes.data(),0,nullptr);
    VkCommandPoolCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cp.queueFamilyIndex=gpu.queueFamily;if(!(vkCreateCommandPool(r.d,&cp,nullptr,&r.pool)==VK_SUCCESS))return no("ordered-raster-check-10");
    VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ca.commandPool=r.pool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;if(!(vkAllocateCommandBuffers(r.d,&ca,&r.command)==VK_SUCCESS))return no("ordered-raster-check-11");
    VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;if(!(vkCreateFence(r.d,&fi,nullptr,&r.fence)==VK_SUCCESS))return no("ordered-raster-check-12");
    if(why)*why="ordered-raster-executor-created";
    return executor;
    }
    std::optional<NativeOrderedFragment7e3Batch> execute(std::span<const std::uint32_t> indices,
            VkRect2D window={{0,0},{1280,720}},const char** why=nullptr) {
    const auto no=[&](const char* s){if(why)*why=s;return std::optional<NativeOrderedFragment7e3Batch>{};};
    if(indices.empty() || indices.size()>maximumIndices_ || indices.size()%3)
        return no("ordered-raster-reuse-index-extent");
    const unsigned vertices=(unsigned(input_[0])<<24)|(unsigned(input_[1])<<16)|(unsigned(input_[2])<<8)|input_[3];
    for(auto index:indices)if(index>=vertices)return no("ordered-raster-index-range");
    if(window.offset.x<0 || window.offset.y<0 || window.offset.x>=1280 || window.offset.y>=720 ||
       !window.extent.width || !window.extent.height || window.extent.width>1280u-unsigned(window.offset.x) ||
       window.extent.height>720u-unsigned(window.offset.y))return no("ordered-raster-window");
    auto& r=resources_;auto& gpu=gpu_;const auto& input=input_;const auto& viewport=viewport_;
    const unsigned capacity=capacity_,records=capacity,traceBytes=(8+capacity*10)*4;
    // Every earlier execute returned only after its fence; never reset/rewrite
    // in-flight commands or buffer bytes. Fatal fence failures do not return.
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)
        return no("ordered-raster-reuse-reset");
    std::vector<unsigned> indexWords(maximumIndices_,OrderedFragment7e3Guard);
    std::copy(indices.begin(),indices.end(),indexWords.begin());
    std::memcpy(r.mapped[2],indexWords.data(),indexWords.size()*4);
    auto* trace=static_cast<unsigned*>(r.mapped[1]);
    std::fill(trace,trace+traceBytes/4,OrderedFragment7e3Guard);trace[4]=trace[5]=0;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;if(!(vkBeginCommandBuffer(r.command,&begin)==VK_SUCCESS))return no("ordered-raster-check-13");
    VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    VkClearValue clear{};VkRenderPassBeginInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;rp.renderPass=r.renderPass;rp.framebuffer=r.framebuffer;rp.renderArea={{0,0},{1280,720}};rp.clearValueCount=1;rp.pClearValues=&clear;
    vkCmdBeginRenderPass(r.command,&rp,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipeline);vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.layout,0,1,&r.set,0,nullptr);
    vkCmdSetScissor(r.command,0,1,&window);
    vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,viewport.data());vkCmdBindIndexBuffer(r.command,r.buffers[2],0,VK_INDEX_TYPE_UINT32);for(unsigned primitive=0;primitive<indices.size()/3;++primitive){const std::array<unsigned,2> fragmentPush{primitive,records};vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_FRAGMENT_BIT,16,8,fragmentPush.data());vkCmdDrawIndexed(r.command,3,1,primitive*3,0,0);}vkCmdEndRenderPass(r.command);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    if(!(vkEndCommandBuffer(r.command)==VK_SUCCESS))return no("ordered-raster-check-14");VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(gpu.queue,1,&submit,r.fence)!=VK_SUCCESS || vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
    if(!(std::memcmp(r.mapped[0],input.data(),input.size())==0 && std::memcmp(r.mapped[2],indexWords.data(),indexWords.size()*4)==0))return no("ordered-raster-check-15");
    if(!(trace[0]==0x5A5A5A5A && trace[1]==0x5A5A5A5A && trace[2]==0x5A5A5A5A && trace[3]==0x5A5A5A5A && trace[6]==0x5A5A5A5A && trace[7]==0x5A5A5A5A))return no("ordered-raster-check-16");
    auto result=decodeNativeOrderedFragment7e3Stream(
        std::span<const unsigned>(trace,traceBytes/4),capacity,unsigned(indices.size()/3));
    if(!result)return no("ordered-raster-stream-refused");
    if(why)*why="fenced-ordered-fragment-stream";
    return result;
    }
private:
    NativeOrderedFragmentRasterExecutor(ProbeDevice& gpu,std::span<const std::uint8_t> input,
            const std::array<float,4>& viewport,unsigned capacity,unsigned maximumIndices)
        :gpu_(gpu),resources_{gpu.device},input_(input.begin(),input.end()),viewport_(viewport),
         capacity_(capacity),maximumIndices_(maximumIndices) {}
    ProbeDevice& gpu_;
    ordered_fragment_detail::NativeOrderedRasterResources resources_;
    std::vector<std::uint8_t> input_;
    std::array<float,4> viewport_;
    unsigned capacity_,maximumIndices_;
};
inline std::optional<NativeOrderedFragment7e3Batch> executeNativeOrderedFragmentRaster(
        ProbeDevice& gpu,const NativeOrderedFragmentRasterProgram& program,
        std::span<const std::uint8_t> input,std::span<const std::uint32_t> indices,
        const std::array<float,4>& viewport,const std::array<VkDescriptorImageInfo,5>& textures,
        unsigned capacity=OrderedFragment7e3MaxCapacity,const char** why=nullptr) {
    auto executor=NativeOrderedFragmentRasterExecutor::create(gpu,program,input,indices,viewport,textures,capacity,why);
    return executor?executor->execute(indices,{{0,0},{1280,720}},why):std::nullopt;
}
}
