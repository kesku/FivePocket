#pragma once
#include "vk11_gate.h"
#include "native_color_chain_profile.h"
#include "depth_chain_initialization.h"
#include "r8_plane_images.h"
#include "texture32_image.h"
#include "texture_fragment_target.h"
#include <array>
#include <vector>

namespace recomp::gpu {
// Exclusive native resources for one bounded chain. No input files, expected
// pixels, guest addresses or publication live here. The context must outlive
// this owner; all submitted work must finish before destruction. Setup is
// single-attempt even on failure, so partial allocations are only cleaned up,
// never overwritten by a retry. Instances cannot be copied or moved.
template<bool StoreCopy,bool Initialize,bool NativeDepth>
class NativeColorChainResources {
public:
    using Profile=NativeColorChainProfile<StoreCopy,Initialize,NativeDepth>;
    using Code=typename Profile::Code;
    using DepthCode=typename Profile::DepthCode;
    using BufferSizes=typename Profile::BufferSizes;
    static constexpr unsigned BufferCount=Profile::BufferCount,CodeCount=Profile::CodeCount;
    static constexpr unsigned ComputeCount=Profile::ComputeCount,OutputBytes=Profile::OutputBytes;
    static constexpr auto Stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    static constexpr FragmentTargetConfig Target{1280,720,VK_FORMAT_R16G16B16A16_SFLOAT,true,Initialize};
    struct Pair {
        VkDevice d{};VkShaderModule shader{},vertexShader{};VkPipeline pipeline{};
        VkPipelineLayout layout{};VkRenderPass renderPass{};VkDescriptorSet set{};
    };
    VkDevice d;
    std::array<VkBuffer,BufferCount> buffers{};std::array<VkDeviceMemory,BufferCount> memory{};
    std::array<void*,BufferCount> mapped{};
    std::array<VkShaderModule,ComputeCount> computeModules{};std::array<VkPipeline,ComputeCount> computePipelines{};
    VkDescriptorSetLayout computeSetLayout{};VkPipelineLayout computeLayout{};VkDescriptorSet computeSet{},initialSet{};
    // Explicit diagnostic perturbations. Live admission must require defaults.
    bool skipStore=false,skipCopy=false,zeroInitial=false;
    DepthChainInitialization depth;
    R8PlaneImages planes;
    VkImage image{};VkDeviceMemory imageMemory{};VkImageView view{};VkSampler sampler{};
    bool imageReady=false,replaceImageControl=false,forceClear=false,disableBlend=false;
    bool reversePairs=false,reuseFirst=false,skipSecond=false;
    FragmentViewportMode colorViewportMode=FragmentViewportMode::DiagnosticPositive;
    VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};
    VkRenderPass renderPass{},loadPass{};VkFramebuffer framebuffer{};
    std::array<Pair,2> pairs{};VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};
    VkDescriptorPool descriptors{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    explicit NativeColorChainResources(VkDevice device):d(device),depth(device),planes(device){}
    NativeColorChainResources(const NativeColorChainResources&)=delete;
    NativeColorChainResources& operator=(const NativeColorChainResources&)=delete;
    NativeColorChainResources(NativeColorChainResources&&)=delete;
    NativeColorChainResources& operator=(NativeColorChainResources&&)=delete;
    ~NativeColorChainResources(){
        if(fence)vkDestroyFence(d,fence,nullptr);
        if(pool)vkDestroyCommandPool(d,pool,nullptr);
        for(auto& p:pairs){
            if(p.pipeline)vkDestroyPipeline(d,p.pipeline,nullptr);
            if(p.shader)vkDestroyShaderModule(d,p.shader,nullptr);
            if(p.vertexShader)vkDestroyShaderModule(d,p.vertexShader,nullptr);
        }
        for(auto p:computePipelines)if(p)vkDestroyPipeline(d,p,nullptr);
        for(auto m:computeModules)if(m)vkDestroyShaderModule(d,m,nullptr);
        if(computeLayout)vkDestroyPipelineLayout(d,computeLayout,nullptr);
        if(computeSetLayout)vkDestroyDescriptorSetLayout(d,computeSetLayout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
        if(loadPass)vkDestroyRenderPass(d,loadPass,nullptr);
        if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
        if(colorView)vkDestroyImageView(d,colorView,nullptr);
        if(colorImage)vkDestroyImage(d,colorImage,nullptr);
        if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        if(sampler)vkDestroySampler(d,sampler,nullptr);
        if(view)vkDestroyImageView(d,view,nullptr);
        if(image)vkDestroyImage(d,image,nullptr);
        if(imageMemory)vkFreeMemory(d,imageMemory,nullptr);
        for(unsigned i=0;i<BufferCount;++i){
            if(mapped[i])vkUnmapMemory(d,memory[i]);
            if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);
            if(memory[i])vkFreeMemory(d,memory[i],nullptr);
        }
    }
    bool configured()const noexcept{return configured_;}
    bool setup(ProbeDevice& device,const Code& code,const BufferSizes& sizes,const DepthCode* depthCode=nullptr,
            bool externalInitialContents=false){
        if(attempted_)return false;
        attempted_=true;
        if(!d || device.device!=d || !Profile::validBufferSizes(sizes) || !Profile::validModuleFraming(code))return false;
        auto target=Target;target.viewportMode=colorViewportMode;
        target.transferDestination=target.transferDestination || externalInitialContents;
        if(!target.valid())return false;
        if constexpr(NativeDepth){
            if(!depthCode || !Profile::validModuleFraming(*depthCode) || !Profile::validDepthSourceBytes(depth.source.size()))return false;
        } else if(depthCode)return false;
        auto& r=*this;
        const auto& limits=device.properties.limits;
        for(auto size:sizes)if(limits.maxStorageBufferRange<size)return false;
        if constexpr(StoreCopy) {
            if(limits.maxComputeWorkGroupInvocations<64 || limits.maxComputeWorkGroupSize[0]<8 ||
                limits.maxComputeWorkGroupSize[1]<8 || limits.maxComputeWorkGroupCount[0]<160 ||
                limits.maxComputeWorkGroupCount[1]<90)return false;
        }
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
        for(unsigned i=0;i<BufferCount;++i){
            VkBufferCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;info.size=sizes[i];
            info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|(i>=2?VK_BUFFER_USAGE_TRANSFER_DST_BIT:VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if(i==2 && externalInitialContents)info.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if constexpr(Initialize) {
                if(i==2)info.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            }
            if(vkCreateBuffer(r.d,&info,nullptr,&r.buffers[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.buffers[i],&req);
            unsigned type=props.memoryTypeCount;
            constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for(unsigned n=0;n<props.memoryTypeCount;++n)if((req.memoryTypeBits&(1u<<n)) &&
                (props.memoryTypes[n].propertyFlags&flags)==flags){type=n;break;}
            if(type==props.memoryTypeCount)return false;
            VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
            if(vkAllocateMemory(r.d,&alloc,nullptr,&r.memory[i])!=VK_SUCCESS ||
                vkBindBufferMemory(r.d,r.buffers[i],r.memory[i],0)!=VK_SUCCESS ||
                vkMapMemory(r.d,r.memory[i],0,VK_WHOLE_SIZE,0,&r.mapped[i])!=VK_SUCCESS)return false;
        }
        if(!r.planes.setup(device) || !recomp::gpu::setupTexture32Image(device,r) ||
            !recomp::gpu::setupTextureFragmentTarget(device,r,target,&r.loadPass))return false;
        std::array<VkDescriptorSetLayoutBinding,5> bindings{};
        for(unsigned i=0;i<5;++i)bindings[i]={i,i<2?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,Stages,nullptr};
        VkDescriptorSetLayoutCreateInfo set{};set.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set.bindingCount=bindings.size();set.pBindings=bindings.data();
        if(vkCreateDescriptorSetLayout(r.d,&set,nullptr,&r.setLayout)!=VK_SUCCESS)return false;
        VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount=1;layout.pSetLayouts=&r.setLayout;
        if(vkCreatePipelineLayout(r.d,&layout,nullptr,&r.layout)!=VK_SUCCESS)return false;
        const std::array<VkDescriptorPoolSize,2> counts{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,CodeCount==7?10u:(CodeCount>=6?7u:4u)},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6}}};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets=CodeCount==7?4:(CodeCount>=6?3:2);pool.poolSizeCount=counts.size();pool.pPoolSizes=counts.data();
        if(vkCreateDescriptorPool(r.d,&pool,nullptr,&r.descriptors)!=VK_SUCCESS)return false;
        for(unsigned i=0;i<2;++i){
            auto& p=r.pairs[i];p.d=r.d;p.layout=r.layout;p.renderPass=r.renderPass;
            for(unsigned stage=0;stage<2;++stage){
                const auto& words=code[i*2+stage];VkShaderModuleCreateInfo module{};
                module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;module.codeSize=words.size()*4;module.pCode=words.data();
                if(vkCreateShaderModule(r.d,&module,nullptr,stage?&p.vertexShader:&p.shader)!=VK_SUCCESS)return false;
            }
            auto config=target;if(r.disableBlend)config.alphaBlend=false;
            if(!recomp::gpu::createTextureFragmentPipeline(p,true,config))return false;
            VkDescriptorSetAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool=r.descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&r.setLayout;
            if(vkAllocateDescriptorSets(r.d,&alloc,&p.set)!=VK_SUCCESS)return false;
            std::array<VkDescriptorBufferInfo,2> bufferInfo{{{r.buffers[i],0,sizes[i]},{r.buffers[2],0,OutputBytes}}};
            std::array<VkDescriptorImageInfo,3> images{};
            for(unsigned n=0;n<3;++n)images[n]={r.planes.sampler,r.planes.views[n],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            if(i)images[0]={r.sampler,r.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            std::array<VkWriteDescriptorSet,5> writes{};
            for(unsigned n=0;n<5;++n){auto& w=writes[n];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet=p.set;w.dstBinding=n;w.descriptorCount=1;w.descriptorType=bindings[n].descriptorType;
                if(n<2)w.pBufferInfo=&bufferInfo[n];else w.pImageInfo=&images[n-2];}
            vkUpdateDescriptorSets(r.d,writes.size(),writes.data(),0,nullptr);
        }
        if constexpr(StoreCopy) {
            std::array<VkDescriptorSetLayoutBinding,3> computeBindings{};
            for(unsigned i=0;i<3;++i)computeBindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            set.bindingCount=computeBindings.size();set.pBindings=computeBindings.data();
            if(vkCreateDescriptorSetLayout(r.d,&set,nullptr,&r.computeSetLayout)!=VK_SUCCESS)return false;
            layout.pSetLayouts=&r.computeSetLayout;
            if(vkCreatePipelineLayout(r.d,&layout,nullptr,&r.computeLayout)!=VK_SUCCESS)return false;
            VkDescriptorSetAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool=r.descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&r.computeSetLayout;
            if(vkAllocateDescriptorSets(r.d,&alloc,&r.computeSet)!=VK_SUCCESS)return false;
            std::array<VkDescriptorBufferInfo,3> data{};std::array<VkWriteDescriptorSet,3> writes{};
            for(unsigned i=0;i<3;++i){
                data[i]={r.buffers[i+2],0,sizes[i+2]};auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet=r.computeSet;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&data[i];
            }
            vkUpdateDescriptorSets(r.d,writes.size(),writes.data(),0,nullptr);
            if constexpr(Initialize) {
                if(vkAllocateDescriptorSets(r.d,&alloc,&r.initialSet)!=VK_SUCCESS)return false;
                const std::array<unsigned,3> initialBuffers{3,2,4};
                for(unsigned i=0;i<3;++i){data[i]={r.buffers[initialBuffers[i]],0,sizes[initialBuffers[i]]};writes[i].dstSet=r.initialSet;}
                vkUpdateDescriptorSets(r.d,writes.size(),writes.data(),0,nullptr);
            }
            for(unsigned i=0;i<ComputeCount;++i){
                const auto& words=code[4+i];VkShaderModuleCreateInfo module{};module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                module.codeSize=words.size()*4;module.pCode=words.data();
                // vk11: on an accepted Vulkan 1.1 driver the module's buffer-length guards are satisfied before it is handed
                // over (vk11_gate.h says why); on 1.2 and newer the module goes to the driver as it is.
                std::vector<std::uint32_t> vk11Words;
                if(recomp::vk11::legacyDriver.load(std::memory_order_acquire)){
                    vk11Words=recomp::vk11::withArrayLengthChecksSatisfied(words.data(),words.size());
                    module.codeSize=vk11Words.size()*4;module.pCode=vk11Words.data();
                }
                if(vkCreateShaderModule(r.d,&module,nullptr,&r.computeModules[i])!=VK_SUCCESS)return false;
                VkComputePipelineCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                info.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
                info.stage.module=r.computeModules[i];info.stage.pName="main";info.layout=r.computeLayout;
                if(vkCreateComputePipelines(r.d,VK_NULL_HANDLE,1,&info,nullptr,&r.computePipelines[i])!=VK_SUCCESS)return false;
            }
        }
        VkCommandPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex=device.queueFamily;
        if(vkCreateCommandPool(r.d,&poolInfo,nullptr,&r.pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo cmd{};cmd.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd.commandPool=r.pool;cmd.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cmd.commandBufferCount=1;
        if(vkAllocateCommandBuffers(r.d,&cmd,&r.command)!=VK_SUCCESS)return false;
        VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if(vkCreateFence(r.d,&fence,nullptr,&r.fence)!=VK_SUCCESS)return false;
        if constexpr(NativeDepth)
            if(!depth.setup(device,*depthCode,buffers[3],Profile::BackingBytes))return false;
        configured_=true;
        return true;
    }
private:
    bool attempted_=false,configured_=false;
};
}
