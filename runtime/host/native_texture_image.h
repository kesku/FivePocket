#pragma once
#include "xenos_texture_mip_chain.h"
#include "device.h"
#include <memory>

namespace recomp::gpu {
// Persistent native sampled image. The device/queue must outlive it. Caller
// serializes queue use and completes all draws using view() before destruction
// or upload(). Samplers remain independent of resource storage, as in D3D.
class NativeTextureImage {
public:
    static std::unique_ptr<NativeTextureImage> create(ProbeDevice& device,
        const XenosTextureDescription&, const std::vector<std::uint8_t>& guestBytes);
    // Explicit opt-in storage import; legacy create() still rejects mips.
    // No sampler, draw admission, address alias or guest ownership inference.
    static std::unique_ptr<NativeTextureImage> createUnpackedMips(ProbeDevice& device,
        const XenosTextureDescription&,const std::vector<std::uint8_t>& base,
        const std::vector<std::uint8_t>& mips);
    // Storage-only six-layer mip0 import. Each face must be independently
    // present; no seamless cube behavior or PWL sampling is implied by view().
    static std::unique_ptr<NativeTextureImage> createCubeBase(ProbeDevice& device,
        const XenosTextureDescription&,const std::vector<std::uint8_t>& allFaces);
    // Explicit format23 mip0 import into R32_SFLOAT, replicated to RGBA by the
    // image view. Caller owns every input byte; not a sampler/guest completion.
    static std::unique_ptr<NativeTextureImage> createDepth24Float(ProbeDevice& device,
        const XenosTextureDescription&,const std::vector<std::uint8_t>& guestBytes);
    // Explicit unsigned format54 mip0 packed UNORM image. No sampling exponent
    // is baked into pixels: the consumer must apply the original descriptor's
    // exponent after sampling. General create() remains strict.
    static std::unique_ptr<NativeTextureImage> createRaw10Unsigned(ProbeDevice& device,
        const XenosTextureDescription&,const std::vector<std::uint8_t>& guestBytes);
    ~NativeTextureImage();
    NativeTextureImage(const NativeTextureImage&)=delete;
    NativeTextureImage& operator=(const NativeTextureImage&)=delete;
    bool upload(const std::vector<std::uint8_t>& guestBytes);
    bool uploadMipChain(const std::vector<std::uint8_t>& base,const std::vector<std::uint8_t>& mips);
    bool uploadCubeBase(const std::vector<std::uint8_t>& allFaces);
    bool uploadDepth24Float(const std::vector<std::uint8_t>& guestBytes);
    bool uploadRaw10Unsigned(const std::vector<std::uint8_t>& guestBytes);
    std::optional<std::vector<std::uint8_t>> readback(unsigned level=0,unsigned layer=0); // Raw channels, before view swizzle.
    unsigned levelCount() const noexcept {return static_cast<unsigned>(levels_.size());}
    unsigned layerCount() const noexcept {return layers_;}
    VkImageView view() const noexcept { return view_; }
    VkImage image() const noexcept { return image_; }
    bool supportsLinearFiltering() const noexcept {return linearFilterSupported_;}
    const XenosTextureDescription& description() const noexcept { return layout_.description; }
private:
    NativeTextureImage(ProbeDevice& device, XenosTexture2DLayout layout):
        device_(device),layout_(std::move(layout)),levels_{layout_}{}
    bool initialize();
    bool uploadLevels(const std::vector<std::vector<std::uint8_t>>& levels);
    bool transfer(std::vector<std::uint8_t>& bytes, bool upload);
    ProbeDevice& device_;
    XenosTexture2DLayout layout_;
    std::vector<XenosTexture2DLayout> levels_;
    std::vector<std::uint32_t> linearOffsets_;
    std::uint32_t stagingBytes_=0;
    unsigned layers_=1;
    bool unpackedMips_=false;
    bool depth24Float_=false;
    bool raw10Unsigned_=false;
    // k_32_FLOAT addresses through a format-6 raw view, so the plan's own
    // descriptor no longer names it. This records the ORIGINAL format, which is
    // what the sampled image format and the channel replication depend on.
    bool float32_=false;
    // Likewise for k_16_16, whose raw view also reports format 6.
    bool fixed16_=false;
    bool linearFilterSupported_=false;
    VkImage image_=VK_NULL_HANDLE;
    VkDeviceMemory memory_=VK_NULL_HANDLE;
    VkImageView view_=VK_NULL_HANDLE;
    bool uploaded_=false;
};
}
