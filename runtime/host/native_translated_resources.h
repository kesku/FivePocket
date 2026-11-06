#pragma once
#include "device.h"
#include "native_translated_draw.h"
#include "xenos_depth_stencil_state.h"
#include "xenos_mrt_blend.h"
#include "xenos_blend_control.h"
#include "native_sampled_stencil_plan.h"

namespace recomp::gpu {
// Owned Vulkan inputs/pipeline shared by presentation and offscreen consumers.
// State/resource admission is the consumer's separate obligation.
// SourceAlpha is the historical RGB blend / alpha replacement contract.
// GuestRegister selects separately validated original RGB AND alpha controls.
enum class NativeTranslatedBlend { SourceAlpha, Replace, DestinationAlphaAdd, ScreenStripSourceOne, GuestRegister };
inline bool validNativeTranslatedBlend(NativeTranslatedBlend b) {
    return b==NativeTranslatedBlend::SourceAlpha || b==NativeTranslatedBlend::Replace || b==NativeTranslatedBlend::DestinationAlphaAdd ||
        b==NativeTranslatedBlend::ScreenStripSourceOne || b==NativeTranslatedBlend::GuestRegister;
}
struct NativeTranslatedPipelineOptions {
    unsigned samples=1;
    float minDepth=1,maxDepth=0;
    std::optional<XenosDepthStencilState> depthStencil;
    // Borrowed images/samplers must remain valid until all pipeline draws have
    // fenced. No ownership transfer. Used for separately validated mip images.
    std::vector<VkDescriptorImageInfo> externalTextures;
    // Explicit raster setup. Callers must derive winding after their viewport
    // mapping and validate the original polygon controls separately.
    VkCullModeFlags cullMode=VK_CULL_MODE_NONE;
    VkFrontFace frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool sampleShading=false;
    // A four-slot render pass is never inferred from a new shader ABI. The
    // logical-tile consumer must explicitly opt in and match formats/exports.
    unsigned colorAttachmentCount=1;
    // ABI14 output-merger channel masks, independently applied to each slot.
    // They cannot enable an unwritten shader export. Legacy paths remain full-write.
    std::array<unsigned,4> colorWriteMasks{15,15,15,15};
    // Internal ABI16 snapshot capacity; the logical target owns/proves/fills it.
    unsigned depthWitnessPixels=0;
    // Explicit ABI12 consumer opt-in, not an override of an existing exact
    // renderer's state. Target precision, colour ownership and blend constants
    // are NOT inferred from this register.
    std::optional<std::uint32_t> guestBlendControl;
};
struct NativeTranslatedResources {
    static constexpr unsigned InputBytes=8208+256*1024;
    static constexpr unsigned MaxTextures=4;
    VkDevice d;
    VkBuffer input{},indices{};VkDeviceMemory inputMemory{},indexMemory{};
    void* inputMapped=nullptr;void* indexMapped=nullptr;
    VkBuffer completion{};VkDeviceMemory completionMemory{};void* completionMapped=nullptr;
    struct Texture {
        VkBuffer upload{};VkDeviceMemory uploadMemory{};void* uploadMapped=nullptr;
        VkImage image{};VkDeviceMemory imageMemory{};VkImageView view{};VkSampler sampler{};
        unsigned width=0,height=0,format=0;bool ready=false;
        std::array<unsigned,4> swizzle{};std::array<unsigned,3> clamp{};
    };
    std::array<Texture,MaxTextures> textures{};
    unsigned textureCount=0;
    VkShaderModule vertexShader{},shader{};VkPipeline pipeline{};VkPipelineLayout layout{};
    VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptors{};VkDescriptorSet set{};
    VkRenderPass renderPass{};
    std::vector<std::uint32_t> vertexCode,pixelCode;
    explicit NativeTranslatedResources(VkDevice device):d(device){}
    ~NativeTranslatedResources(){
        if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
        if(vertexShader)vkDestroyShaderModule(d,vertexShader,nullptr);
        if(shader)vkDestroyShaderModule(d,shader,nullptr);
        if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        for(auto& t:textures) {
            if(t.sampler)vkDestroySampler(d,t.sampler,nullptr);
            if(t.view)vkDestroyImageView(d,t.view,nullptr);
            if(t.image)vkDestroyImage(d,t.image,nullptr);
            if(t.imageMemory)vkFreeMemory(d,t.imageMemory,nullptr);
            if(t.uploadMapped)vkUnmapMemory(d,t.uploadMemory);
            if(t.upload)vkDestroyBuffer(d,t.upload,nullptr);
            if(t.uploadMemory)vkFreeMemory(d,t.uploadMemory,nullptr);
        }
        if(inputMapped)vkUnmapMemory(d,inputMemory);
        if(indexMapped)vkUnmapMemory(d,indexMemory);
        if(completionMapped)vkUnmapMemory(d,completionMemory);
        if(completion)vkDestroyBuffer(d,completion,nullptr);
        if(completionMemory)vkFreeMemory(d,completionMemory,nullptr);
        if(input)vkDestroyBuffer(d,input,nullptr);
        if(indices)vkDestroyBuffer(d,indices,nullptr);
        if(inputMemory)vkFreeMemory(d,inputMemory,nullptr);
        if(indexMemory)vkFreeMemory(d,indexMemory,nullptr);
    }
    bool setup(ProbeDevice& device,VkRenderPass pass,const NativeTranslatedDrawProgram& program,
               const std::vector<XenosTextureDescription>& descriptions,
               NativeTranslatedBlend blend=NativeTranslatedBlend::SourceAlpha,
               const NativeTranslatedPipelineOptions& options={}) {
        const bool color8Geometry=program.abiVersion==13;
        const bool mrtGeometry=program.abiVersion>=14 && program.abiVersion<=16;
        const bool fixed16Geometry=program.abiVersion==17 || program.abiVersion==18;
        const bool fixed16Rectangle=program.abiVersion==19;
        const bool guardedGeometry=color8Geometry || mrtGeometry || fixed16Geometry || fixed16Rectangle;
        const bool screenStrip=program.abiVersion==28;
        const bool worldStrip=program.abiVersion==29;
        const bool indexedRgb7e3=program.abiVersion==30;
        const bool filterWeights=program.abiVersion==35;
        const bool expanded16Max=program.abiVersion==36;
        const bool expanded16Luma=program.abiVersion==37;
        const bool r32Scalar=program.abiVersion==38;
        const bool exposureScalar=program.abiVersion==39;
        // ABI40: same rectangle state; three EXPAND storage views (slots1,10,17).
        const bool brightPass=program.abiVersion==40;
        // ABI41: same rectangle state except its captured disabled depth compare
        // (RB_DEPTHCONTROL 0x00700770) and stencil reference 0xFE.
        const bool expanded16Copy=program.abiVersion==41;
        // ABI42: ABI41's rectangle state except its captured stencil reference
        // 0x00 (RB_STENCILREFMASK 0x00FFFF00); same disabled depth compare.
        // ABI43 (pass82 vertical blur) shares ABI42's state exactly.
        const bool postfxBlur=program.abiVersion==42 || program.abiVersion==43;
        // ABI44 (pass83 additive upsample): ABI42's depth/stencil state. The
        // pipeline renders only the Replace source term into a scratch float
        // target; the captured ONE/ONE add is applied to physical words later.
        const bool postfxUpsample=program.abiVersion==44;
        if((filterWeights || expanded16Max || expanded16Luma || r32Scalar || exposureScalar || brightPass || expanded16Copy || postfxBlur || postfxUpsample) &&
            (!(filterWeights?validateNativeFilterWeightsProgram(program):expanded16Max?validateNativeExpanded16MaxProgram(program):expanded16Luma?validateNativeExpanded16LumaProgram(program):exposureScalar?validateNativeExposureScalarProgram(program):brightPass?validateNativeBrightPassProgram(program):expanded16Copy?validateNativeExpanded16CopyProgram(program):postfxBlur?validateNativePostfxBlurFamilyProgram(program):postfxUpsample?validateNativePostfxUpsampleProgram(program):validateNativeR32ScalarProgram(program)) || !options.depthStencil ||
             *options.depthStencil!=*(expanded16Copy?decodeXenosDepthStencilState(0x00700770,0xFFFFFE,0):(postfxBlur || postfxUpsample)?decodeXenosDepthStencilState(0x00700770,0xFFFF00,0):decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0)) ||
             options.samples!=1 || options.sampleShading || options.depthWitnessPixels ||
             options.colorAttachmentCount!=1 || options.colorWriteMasks!=std::array<unsigned,4>{15,15,15,15} ||
             descriptions.size()!=(brightPass?3u:1u) ||
             std::any_of(descriptions.begin(),descriptions.end(),[&](const auto& d){return d.format!=(filterWeights?54u:(r32Scalar || exposureScalar)?36u:32u);}) ||
             options.externalTextures.size()!=descriptions.size() ||
             options.minDepth!=1 || options.maxDepth!=0 || options.cullMode!=VK_CULL_MODE_NONE ||
             blend!=NativeTranslatedBlend::Replace))return false;
        const bool guestBlend=blend==NativeTranslatedBlend::GuestRegister;
        if(guestBlend!=options.guestBlendControl.has_value())return false;
        const auto decodedBlend=options.guestBlendControl?planNativeRegisterBlend(*options.guestBlendControl):std::nullopt;
        if(guestBlend && (!decodedBlend || program.abiVersion!=12 || options.colorAttachmentCount!=1))return false;
        if(indexedRgb7e3 && (!validateNativeIndexedRgb7e3Program(program) || !options.depthStencil ||
             options.samples!=1 || options.sampleShading || options.depthWitnessPixels ||
             options.colorAttachmentCount!=1 || options.colorWriteMasks!=std::array<unsigned,4>{15,0,0,0} ||
             !descriptions.empty() || !options.externalTextures.empty() ||
             options.minDepth!=0x1p-22f || options.maxDepth!=0x1p-24f ||
             options.cullMode!=VK_CULL_MODE_BACK_BIT || options.frontFace!=VK_FRONT_FACE_COUNTER_CLOCKWISE ||
             *options.depthStencil!=*decodeXenosDepthStencilState(0x00708767,0xFFFF00,0) ||
             blend!=NativeTranslatedBlend::Replace))return false;
        if(worldStrip!=program.worldStrip || (worldStrip &&
            (!validateNativeWorldStripProgram(program) || !options.depthStencil ||
             options.samples!=1 || options.sampleShading || options.depthWitnessPixels ||
             options.colorAttachmentCount!=1 || options.colorWriteMasks!=std::array<unsigned,4>{15,0,0,0} ||
             descriptions.size()!=4 || options.externalTextures.size()!=4 ||
             descriptions[0].format!=20 || descriptions[1].format!=25 || descriptions[2].format!=6 || descriptions[3].format!=18 ||
             options.minDepth!=0x1p-24f || options.maxDepth!=0x1p-24f || options.cullMode!=VK_CULL_MODE_NONE ||
             *options.depthStencil!=*decodeXenosDepthStencilState(0x00708767,0xFFFF00,0) ||
             blend!=NativeTranslatedBlend::Replace)))return false;
        if(screenStrip!=program.screenStrip || (screenStrip &&
            (!validateNativeScreenStripProgram(program) || !options.depthStencil ||
             options.samples!=1 || options.sampleShading || options.depthWitnessPixels ||
             options.colorAttachmentCount!=1 || options.colorWriteMasks!=std::array<unsigned,4>{7,0,0,0} ||
             descriptions.size()!=1 || options.externalTextures.size()!=1 || descriptions[0].format!=23 ||
             options.minDepth!=1 || options.maxDepth!=0 || options.cullMode!=VK_CULL_MODE_NONE ||
             *options.depthStencil!=*decodeXenosDepthStencilState(0x00700543,0x707,0) ||
             blend!=NativeTranslatedBlend::ScreenStripSourceOne)))return false;
        if(!screenStrip && blend==NativeTranslatedBlend::ScreenStripSourceOne)return false;
        // ABI20 composite band: a nonindexed rectangle_list program with one
        // sampled 2D texture and one interpolator. It carries none of the
        // geometry/MRT/fixed16 flags, so every guarded-profile predicate below
        // already excludes it; only this version bound had to be widened.
        const bool screenSpaceRectangle=program.abiVersion==26;
        if(screenSpaceRectangle!=program.screenSpaceRectangle || (screenSpaceRectangle &&
            (!validateNativeScreenSpaceRectangleProgram(program) || options.depthStencil ||
             options.samples!=1 || options.sampleShading || options.depthWitnessPixels)))return false;
        const bool compositeRectangle=program.abiVersion==20 || screenSpaceRectangle;
        // ABI21 is the inert depth-only band: it is admitted without execution
        // and never reaches a pipeline, so no resources may be created for it.
        if(program.abiVersion==21 || program.depthOnlyRectangle)return false;
        // ABI22 rectangle MRT: two plain colour attachments, no depth, no
        // geometry color8/MRT machinery. Only this band takes two attachments
        // outside the indexed geometry profile's four.
        // ABI25 stencil only: no fragment stage at all, so no colour
        // attachment may be written and a depth/stencil one must be present.
        // Its pipeline is built with a single stage below.
        const bool stencilOnly=program.abiVersion==25;
        const bool sampledStencil=program.abiVersion==34;
        if(sampledStencil && (!validateNativeSampledStencilProgram(program) ||
                !options.depthStencil || options.depthStencil->depthEnable || options.depthStencil->depthWrite ||
                *options.depthStencil!=*decodeXenosDepthStencilState(0x00708771,0x00101010,0x00101010) ||
                options.colorAttachmentCount!=0 || options.samples!=1 || options.sampleShading ||
                options.depthWitnessPixels || descriptions.size()!=1 || options.externalTextures.size()!=1 ||
                options.colorWriteMasks!=std::array<unsigned,4>{0,0,0,0} ||
                blend!=NativeTranslatedBlend::Replace))return false;
        if(stencilOnly && (!validateNativeStencilOnlyProgram(program) || !options.depthStencil ||
                // Explicitly separate physical depth-only storage (zero
                // colour slots) from the existing four-slot tiled subpass.
                (options.colorAttachmentCount!=0 && options.colorAttachmentCount!=4) ||
                options.samples!=1 || options.sampleShading || options.depthWitnessPixels ||
                !descriptions.empty() || !options.externalTextures.empty() ||
                options.colorWriteMasks!=std::array<unsigned,4>{0,0,0,0} ||
                blend!=NativeTranslatedBlend::Replace))return false;
        if(!stencilOnly && program.pixelCode.empty())return false;
        const bool rectangleMrt=program.abiVersion==22;
        if(rectangleMrt && (!validateNativeRectangleMrtProgram(program) ||
                options.colorAttachmentCount!=2u || options.depthStencil || options.samples!=1 ||
                options.sampleShading || options.depthWitnessPixels ||
                descriptions.size()!=nativeTranslatedTextureSlots(program).size() ||
                options.colorWriteMasks!=std::array<unsigned,4>{15,15,15,15} ||
                blend!=NativeTranslatedBlend::Replace))return false;
        if(!rectangleMrt && program.rectangleMrt)return false;
        // ABI23 offset sample: ONE plain colour attachment and ONE sampled slot,
        // whose k_16_16_16_16_FLOAT source is supplied externally because the
        // internal upload storage is R8/RGBA8 only. No depth, no MSAA and none
        // of the geometry machinery.
        // ABI24, the tonemap pass, is this band's draw with richer shader
        // arithmetic; it is admitted through the same predicates, not its own.
        const bool offsetSample=program.abiVersion==23 || program.abiVersion==24;
        if(offsetSample && (!validateNativeOffsetSampleProgram(program) ||
                options.colorAttachmentCount!=1u || options.depthStencil || options.samples!=1 ||
                options.sampleShading || options.depthWitnessPixels ||
                descriptions.size()!=1 || options.externalTextures.size()!=1 ||
                options.colorWriteMasks!=std::array<unsigned,4>{15,15,15,15} ||
                blend!=NativeTranslatedBlend::Replace))return false;
        if(!offsetSample && program.offsetSample)return false;
        if(program.abiVersion<4 || (program.abiVersion>20 && !rectangleMrt && !offsetSample && !stencilOnly && !sampledStencil && !screenSpaceRectangle && !screenStrip && !worldStrip && !indexedRgb7e3 && !filterWeights && !expanded16Max && !expanded16Luma && !r32Scalar && !exposureScalar && !brightPass && !expanded16Copy && !postfxBlur && !postfxUpsample) ||
            (compositeRectangle && (!program.compositeRectangle || program.indexed ||
                program.directTriangles || program.cubeRectanglePwl || program.explicitLodRectangle ||
                program.fixed16RectangleDepth || program.geometryFixed16GradientDepth ||
                program.interpolators!=(screenSpaceRectangle?0u:1u) || descriptions.size()!=1 ||
                !options.externalTextures.empty() || blend!=NativeTranslatedBlend::Replace)) ||
            (!compositeRectangle && program.compositeRectangle) ||
            (program.geometryFixed16GradientDepth!=fixed16Geometry) ||
            (program.fixed16RectangleDepth!=fixed16Rectangle) ||
            (program.geometryPixelEffects!=((program.abiVersion>=15 && program.abiVersion<=16) || program.abiVersion==18)) ||
            (program.geometryDepthColorWitness!=(program.abiVersion==16)) ||
            (options.depthWitnessPixels && !program.geometryDepthColorWitness) ||
            options.depthWitnessPixels>4u*1024*1024 ||
            (program.geometryDepthColorWitness && device.properties.limits.maxPushConstantsSize<44) ||
            (program.geometryParameterRegister!=UINT32_MAX &&
                (!program.geometryPixelEffects || program.geometryParameterRegister>=16)) ||
            (!program.geometryPixelEffects && program.geometryMrtBlendControls!=std::array<unsigned,4>{0x10001,0x10001,0x10001,0x10001}) ||
            (!rectangleMrt && !stencilOnly && !sampledStencil && options.colorAttachmentCount!=(mrtGeometry?4u:1u)) ||
            std::any_of(options.colorWriteMasks.begin(),options.colorWriteMasks.end(),[](auto mask){return mask>15;}) ||
            (!mrtGeometry && !stencilOnly && !sampledStencil && !screenStrip && !worldStrip && !indexedRgb7e3 && options.colorWriteMasks!=std::array<unsigned,4>{15,15,15,15}) ||
            descriptions.size()>textures.size() || !validNativeTranslatedBlend(blend) ||
            (descriptions.size()>2 && ((program.abiVersion!=12 && !guardedGeometry && !worldStrip && !brightPass) || options.externalTextures.empty())) ||
            (options.sampleShading && !device.sampleRateShadingEnabled) ||
            (color8Geometry && (!program.indexed || program.geometryColor8Format>1 || !program.geometryDepth20e4 ||
                !program.geometrySampleDepth || !device.vertexStoresAndAtomicsEnabled ||
                !device.fragmentStoresAndAtomicsEnabled || !options.sampleShading || blend!=NativeTranslatedBlend::Replace)) ||
            ((fixed16Geometry || fixed16Rectangle) &&
                (!(fixed16Rectangle?validateNativeFixed16RectangleProgram(program):validateNativeFixed16Program(program)) ||
                descriptions.size()!=nativeTranslatedTextureSlots(program).size() ||
                descriptions.size()!=options.externalTextures.size() ||
                options.samples!=1 || options.sampleShading || !options.depthStencil ||
                !device.vertexStoresAndAtomicsEnabled || !device.fragmentStoresAndAtomicsEnabled ||
                blend!=NativeTranslatedBlend::Replace)) ||
            (mrtGeometry && (!program.indexed || !program.geometryDepth20e4 || program.geometrySampleDepth ||
                program.geometryColor8Format!=UINT32_MAX || !program.geometryMrtOutputs || (program.geometryMrtOutputs&~15u) ||
                options.samples!=1 || options.sampleShading || !options.depthStencil ||
                !device.independentBlendEnabled || device.properties.limits.maxColorAttachments<4 ||
                !device.vertexStoresAndAtomicsEnabled || !device.fragmentStoresAndAtomicsEnabled ||
                blend!=NativeTranslatedBlend::Replace)) ||
            descriptions.size()>device.properties.limits.maxPerStageDescriptorSamplers ||
            descriptions.size()>device.properties.limits.maxPerStageDescriptorSampledImages ||
            (options.samples!=1 && options.samples!=2 && options.samples!=4) ||
            !std::isfinite(options.minDepth) || !std::isfinite(options.maxDepth) ||
            options.minDepth<0 || options.minDepth>1 || options.maxDepth<0 || options.maxDepth>1 ||
            (options.cullMode&~VkCullModeFlags(VK_CULL_MODE_FRONT_AND_BACK)) ||
            (options.frontFace!=VK_FRONT_FACE_CLOCKWISE && options.frontFace!=VK_FRONT_FACE_COUNTER_CLOCKWISE) ||
            (!options.externalTextures.empty() && options.externalTextures.size()!=descriptions.size()))return false;
        if(mrtGeometry)for(unsigned i=0;i<4;++i) {
            const auto f=program.geometryMrtFormats[i];
            if((f!=UINT32_MAX && f>1) || ((program.geometryMrtOutputs&(1u<<i)) && f==UINT32_MAX))return false;
            const auto b=decodeXenosMrtBlend(program.geometryMrtBlendControls[i]);
            // PWL gamma destination blending needs a separate linearization
            // contract. This profile only blends linear UNORM attachments.
            if(!b || (b->enabled && f!=0 && (program.geometryMrtOutputs&(1u<<i))))return false;
        }
        for(const auto& texture:options.externalTextures)
            if(!texture.imageView || !texture.sampler || texture.imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)return false;
        // Internal upload storage is R8/RGBA8. Float textures require the
        // format-aware NativeTextureImage owner supplied through externalTextures.
        // Never reinterpret the new R16_FLOAT layout as an RGBA8 upload.
        if(options.externalTextures.empty())for(const auto& desc:descriptions)
            if(desc.format==30)return false;
        textureCount=static_cast<unsigned>(descriptions.size());renderPass=pass;
        const bool textured=textureCount!=0;
        vertexCode=program.vertexCode;pixelCode=program.pixelCode;
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
        const auto memoryType=[&](const VkMemoryRequirements& req,VkMemoryPropertyFlags flags){
            for(unsigned i=0;i<props.memoryTypeCount;++i)
                if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags)return i;
            return props.memoryTypeCount;
        };
        const auto buffer=[&](unsigned bytes,VkBufferUsageFlags usage,VkBuffer& b,VkDeviceMemory& memory,void*& mapped){
            VkBufferCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;info.size=bytes;info.usage=usage;
            if(vkCreateBuffer(d,&info,nullptr,&b)!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,b,&req);
            const auto type=memoryType(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if(type==props.memoryTypeCount)return false;
            VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
            return vkAllocateMemory(d,&alloc,nullptr,&memory)==VK_SUCCESS && vkBindBufferMemory(d,b,memory,0)==VK_SUCCESS &&
                vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS;
        };
        if(!buffer(InputBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,input,inputMemory,inputMapped) ||
           (program.indexed && !buffer(65536*4,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices,indexMemory,indexMapped)))return false;
        // ABI25's tile consumer also checks these canaries after the fence.
        // It has no shader completion writer: this allocation is guard-only,
        // not proof of fragment coverage. Do not leave completionMapped null.
        if(guardedGeometry || stencilOnly || sampledStencil) {
            const auto bytes=64u+options.depthWitnessPixels*4u;
            if(bytes>device.properties.limits.maxStorageBufferRange ||
               !buffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,completion,completionMemory,completionMapped))return false;
            auto* words=static_cast<std::uint32_t*>(completionMapped);
            std::fill(words,words+16,0x5A5A5A5Au);words[4]=words[5]=0;
        }
        for(unsigned slot=0;slot<textureCount;++slot) {
        const auto& desc=descriptions[slot];auto& t=textures[slot];
        t.width=desc.width;t.height=desc.height;t.format=desc.format;t.swizzle=desc.swizzle;t.clamp=desc.clamp;
        if(!options.externalTextures.empty())continue; // Do not allocate/delete borrowed images.
        if(!buffer(t.width*t.height*(t.format==2?1:4),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,t.upload,t.uploadMemory,t.uploadMapped))return false;
        VkImageCreateInfo im{};im.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;im.imageType=VK_IMAGE_TYPE_2D;
        im.format=t.format==2?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8B8A8_UNORM;im.extent={t.width,t.height,1};im.mipLevels=1;im.arrayLayers=1;
        im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;
        im.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        if(vkCreateImage(d,&im,nullptr,&t.image)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(d,t.image,&req);
        const auto type=memoryType(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(type==props.memoryTypeCount)return false;
        VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
        if(vkAllocateMemory(d,&alloc,nullptr,&t.imageMemory)!=VK_SUCCESS || vkBindImageMemory(d,t.image,t.imageMemory,0)!=VK_SUCCESS)return false;
        VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t.image;
        vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=im.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        constexpr VkComponentSwizzle components[]{VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE};
        vi.components={components[t.swizzle[0]],components[t.swizzle[1]],components[t.swizzle[2]],components[t.swizzle[3]]};
        if(vkCreateImageView(d,&vi,nullptr,&t.view)!=VK_SUCCESS)return false;
        VkSamplerCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // ABI26's translator admits only explicit POINT opcodes. Those override
        // the original fetch constant's mag/min LINEAR fields; using them here
        // interpolates neighbouring texels at integer generated coordinates.
        // Older profiles retain their independently qualified sampler policy.
        sm.magFilter=sm.minFilter=screenSpaceRectangle?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
        sm.mipmapMode=screenSpaceRectangle?VK_SAMPLER_MIPMAP_MODE_NEAREST:VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sm.addressModeU=t.clamp[0]==0?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sm.addressModeV=t.clamp[1]==0?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;sm.maxLod=0;
        if(vkCreateSampler(d,&sm,nullptr,&t.sampler)!=VK_SUCCESS)return false;
        }
        std::array<VkDescriptorSetLayoutBinding,2+MaxTextures> bindings{};
        // A stage flag for a stage the pipeline does not have would name a
        // consumer that cannot exist; with no fragment stage the input buffer
        // is read by the vertex stage alone.
        bindings[0]={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,static_cast<VkShaderStageFlags>(stencilOnly?
            VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT),nullptr};
        for(unsigned i=0;i<textureCount;++i)
            bindings[i+1]={i+1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        if(guardedGeometry)bindings[1+textureCount]={5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;si.bindingCount=1+textureCount+unsigned(guardedGeometry);si.pBindings=bindings.data();
        if(vkCreateDescriptorSetLayout(d,&si,nullptr,&setLayout)!=VK_SUCCESS)return false;
        VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.setLayoutCount=1;li.pSetLayouts=&setLayout;
        const VkPushConstantRange viewportConstants{static_cast<VkShaderStageFlags>(guardedGeometry?
            VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT),0,program.geometryDepthColorWitness?44u:guardedGeometry?24u:16u};
        li.pushConstantRangeCount=1;li.pPushConstantRanges=&viewportConstants;
        if(vkCreatePipelineLayout(d,&li,nullptr,&layout)!=VK_SUCCESS)return false;
        // One module when there is no fragment stage; the loop is bounded by
        // that rather than by a constant, so the absent stage is never created.
        const unsigned stageCount=stencilOnly?1u:2u;
        for(unsigned i=0;i<stageCount;++i){
            const auto& code=i?pixelCode:vertexCode;VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            mi.codeSize=code.size()*4;mi.pCode=code.data();
            if(!checkVulkan(vkCreateShaderModule(d,&mi,nullptr,i?&shader:&vertexShader),
                i?"translated-fragment-module":"translated-vertex-module"))return false;
        }
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for(unsigned i=0;i<stageCount;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?shader:vertexShader;stages[i].pName="main";}
        VkPipelineVertexInputStateCreateInfo vs{};vs.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,2048,1024,options.minDepth,options.maxDepth};VkRect2D scissor{{0,0},{1280,720}};
        VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo rs{};rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=options.cullMode;rs.frontFace=options.frontFace;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples=static_cast<VkSampleCountFlagBits>(options.samples);
        ms.sampleShadingEnable=options.sampleShading;ms.minSampleShading=options.sampleShading?1.f:0.f;
        VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=15;
        // ONE/ZERO ADD for both color and alpha is replacement. Disable the
        // blend unit so a full overwrite never reads undefined destination data.
        color.blendEnable=blend==NativeTranslatedBlend::Replace?VK_FALSE:VK_TRUE;
        color.srcColorBlendFactor=(blend==NativeTranslatedBlend::Replace || screenStrip)?VK_BLEND_FACTOR_ONE:
            blend==NativeTranslatedBlend::DestinationAlphaAdd?VK_BLEND_FACTOR_DST_ALPHA:VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstColorBlendFactor=blend==NativeTranslatedBlend::Replace?VK_BLEND_FACTOR_ZERO:
            blend==NativeTranslatedBlend::DestinationAlphaAdd?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.colorBlendOp=VK_BLEND_OP_ADD;
        color.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;color.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;color.alphaBlendOp=VK_BLEND_OP_ADD;
        if(guestBlend) {
            // Xenia's register-driven output merger keeps these six fields
            // independent. Never reuse RGB factors or assume alpha replacement.
            const auto factor=[](unsigned f)->std::optional<VkBlendFactor> {
                switch(f) {
                case 0:return VK_BLEND_FACTOR_ZERO;case 1:return VK_BLEND_FACTOR_ONE;
                case 4:return VK_BLEND_FACTOR_SRC_COLOR;case 5:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case 6:return VK_BLEND_FACTOR_SRC_ALPHA;case 7:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case 8:return VK_BLEND_FACTOR_DST_COLOR;case 9:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case 10:return VK_BLEND_FACTOR_DST_ALPHA;case 11:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case 16:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;default:return std::nullopt;
                }
            };
            const auto& b=*decodedBlend;
            const auto cs=factor(b.colorSource),cd=factor(b.colorDestination),as=factor(b.alphaSource),ad=factor(b.alphaDestination);
            if(!cs || !cd || !as || !ad)return false;
            constexpr VkBlendOp operations[]{VK_BLEND_OP_ADD,VK_BLEND_OP_SUBTRACT,VK_BLEND_OP_MIN,VK_BLEND_OP_MAX,VK_BLEND_OP_REVERSE_SUBTRACT};
            color.blendEnable=b.enabled;
            color.srcColorBlendFactor=*cs;color.dstColorBlendFactor=*cd;color.colorBlendOp=operations[b.colorOperation];
            color.srcAlphaBlendFactor=*as;color.dstAlphaBlendFactor=*ad;color.alphaBlendOp=operations[b.alphaOperation];
        }
        std::array<VkPipelineColorBlendAttachmentState,4> colors{};colors.fill(color);
        if(screenStrip)colors[0].colorWriteMask=7; // Preserve ORIGINAL alpha.
        // No fragment stage does not disable colour writes in Vulkan. These
        // are the actual attachment states, not merely the admission options.
        if(stencilOnly)for(auto& attachment:colors)attachment.colorWriteMask=0;
        if(mrtGeometry)for(unsigned i=0;i<4;++i) {
            colors[i].colorWriteMask=program.geometryMrtOutputs&(1u<<i)?options.colorWriteMasks[i]:0;
            const auto b=*decodeXenosMrtBlend(program.geometryMrtBlendControls[i]);
            const auto factor=[](unsigned f) {
                switch(f){case 1:return VK_BLEND_FACTOR_ONE;case 6:return VK_BLEND_FACTOR_SRC_ALPHA;
                    case 7:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;default:return VK_BLEND_FACTOR_ZERO;}
            };
            colors[i].blendEnable=b.enabled;
            colors[i].srcColorBlendFactor=factor(b.colorSource);colors[i].dstColorBlendFactor=factor(b.colorDestination);
            colors[i].srcAlphaBlendFactor=factor(b.alphaSource);colors[i].dstAlphaBlendFactor=factor(b.alphaDestination);
        }
        VkPipelineColorBlendStateCreateInfo bs{};bs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        bs.attachmentCount=options.colorAttachmentCount;bs.pAttachments=colors.data();
        const VkDynamicState dynamicScissor=VK_DYNAMIC_STATE_SCISSOR;
        VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&dynamicScissor;
        VkGraphicsPipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;pi.stageCount=stageCount;pi.pStages=stages.data();
        pi.pVertexInputState=&vs;pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;
        pi.pMultisampleState=&ms;pi.pColorBlendState=&bs;pi.pDynamicState=&dynamic;pi.layout=layout;pi.renderPass=renderPass;
        VkPipelineDepthStencilStateCreateInfo depth{};depth.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        if(options.depthStencil) {
            const auto& state=*options.depthStencil;
            if(state.depthCompare>7)return false;
            const auto face=[](const XenosStencilFace& f)->std::optional<VkStencilOpState> {
                if(f.compare>7 || f.fail>7 || f.pass>7 || f.depthFail>7 || f.reference>255 || f.readMask>255 || f.writeMask>255)return std::nullopt;
                return VkStencilOpState{static_cast<VkStencilOp>(f.fail),static_cast<VkStencilOp>(f.pass),
                    static_cast<VkStencilOp>(f.depthFail),static_cast<VkCompareOp>(f.compare),f.readMask,f.writeMask,f.reference};
            };
            const auto front=face(state.front),back=face(state.backfaceEnable?state.back:state.front);if(!front || !back)return false;
            depth.depthTestEnable=state.depthEnable;depth.depthWriteEnable=state.depthEnable && state.depthWrite;
            depth.depthCompareOp=static_cast<VkCompareOp>(state.depthCompare);depth.stencilTestEnable=state.stencilEnable;
            depth.front=*front;depth.back=*back;pi.pDepthStencilState=&depth;
        }
        // Reuse the device's compiled-shader cache. This is the same pipeline
        // it always was; the cache only spares the driver recompiling identical
        // SPIR-V on every draw. The lock satisfies the external-synchronisation
        // requirement on the cache handle.
        {
            std::lock_guard<std::mutex> cacheLock(device.pipelineCacheMutex);
            if(!checkVulkan(vkCreateGraphicsPipelines(d,device.pipelineCache,1,&pi,nullptr,&pipeline),"translated-graphics-pipeline"))return false;
        }
        std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1+unsigned(guardedGeometry)},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,textureCount}}};
        VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pool.maxSets=1;pool.poolSizeCount=textured?2:1;pool.pPoolSizes=sizes.data();
        if(vkCreateDescriptorPool(d,&pool,nullptr,&descriptors)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ds.descriptorPool=descriptors;ds.descriptorSetCount=1;ds.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(d,&ds,&set)!=VK_SUCCESS)return false;
        VkDescriptorBufferInfo bi{input,0,InputBytes};std::array<VkDescriptorImageInfo,MaxTextures> images{};
        std::array<VkWriteDescriptorSet,2+MaxTextures> writes{};
        for(unsigned i=0;i<=textureCount;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
            writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if(i){const auto& t=textures[i-1];images[i-1]=options.externalTextures.empty()?
                VkDescriptorImageInfo{t.sampler,t.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}:options.externalTextures[i-1];
                writes[i].pImageInfo=&images[i-1];}}
        VkDescriptorBufferInfo ci{completion,0,64u+options.depthWitnessPixels*4u};
        if(guardedGeometry) {
            auto& w=writes[1+textureCount];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=5;
            w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&ci;
        }
        writes[0].pBufferInfo=&bi;vkUpdateDescriptorSets(d,1+textureCount+unsigned(guardedGeometry),writes.data(),0,nullptr);return true;
    }
};

}
