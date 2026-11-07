#if defined(__APPLE__)
#include "apple/vulkan_extensions.h"
#endif
#include "vk11_gate.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include "device.h"
#include "apple/ios_gpu_lifecycle.h"
#include <atomic>
#include "recomp_app_dir.h" // data-paths: the app's files directory. v293: VK_EXT_load_store_op_none enabled on the device this file creates (read by the generic renderer, weak there).
extern "C" { std::atomic<bool> recomp_v293_store_none{false}; }
std::atomic<bool> recomp_v318_display_timing{false}; // v318
// v294: scalarBlockLayout (Vulkan 1.2 core) enabled for the constant banks as uniform blocks (the generic renderer reads it
// once, weakly). v297: on unless files/private-trace/diag-v294-ssbo-banks exists at device creation (then the device is v293's).
extern "C" { std::atomic<bool> recomp_v294_ubo_banks{false}; }
// occlusion package: occlusionQueryPrecise enabled when offered (exact sample counts for diag-oc-queries; enabling it
// changes nothing else). The generic renderer reads it weakly.
extern "C" { std::atomic<bool> recomp_ocq_precise{false}; }
// depth-float (package depth-float): VK_EXT_depth_bias_control enabled with floatRepresentation (depth bias in absolute depth
// units) because files/private-trace/diag-df-float AND diag-df-bias-abs existed at device creation and the device offers it:
// 0 no, 1 yes, 2 yes with depthBiasExact (read once by the generic renderer, weak there).
extern "C" { std::atomic<unsigned> recomp_df_bias_float{0}; }
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <unistd.h>
#include <vector>
// boot-stall: the pipeline cache kept between runs (pipeline_cache_store.h: the file, its checks, the saver thread).
#include "pipeline_cache_store.h"
#include <chrono>
#include <memory>
#ifndef RECOMP_PCACHE_DIR // where the cache file and its off switch live (host tests name their own directory)
#define RECOMP_PCACHE_DIR RECOMP_APP_PATH("/private-trace")
#endif

namespace recomp::gpu {
// boot-stall. pcacheCreate() makes the device's VkPipelineCache: from files/private-trace/pipeline-cache.bin when that file
// passes every check of pipeline_cache_store.h (our header: magic, version, length, checksum, this library's build-id; then
// Vulkan's header: size, version, vendorID, deviceID, pipelineCacheUUID of THIS physical device), empty otherwise - also when
// the driver refuses the file's data. The load never changes or removes the file. Unless files/private-trace/
// diag-pipeline-cache-off existed at this moment (then: no load, no thread, no save; the cache is v467's), one saver thread per
// device writes the cache back (after the warm-up, on growth, at the orderly end). One line RENDER_DIAG_PCACHE_LOAD per device.
// The guard (repair 1; pipeline_cache_store.h, guardSet): files/private-trace/pipeline-cache.loading is made before the file's
// data goes to the driver and removed by the saver thread once the start has lived with it (100 pipelines created through the
// cache, or a save attempt). Found at a start, it means the start before did not get that far with this file: the file is
// then NOT handed to the driver (reason=last-start-unfinished), so a file the driver crashes or hangs on costs one start, not
// every start. If the guard cannot be made the file is not handed either (reason=guard-failed).
// pcacheStop() ends that thread (its last save included) before the cache is destroyed.
// Out of line and noexcept: ProbeDevice::create() and the destructor each gain one plain call.
namespace {
// The savers by device. Made at first use and never destroyed: nothing of it runs at the process's exit (a static object here
// would be destroyed in an order nobody controls, perhaps before a ProbeDevice that still wants to stop its saver).
struct PcacheSavers { std::mutex mutex; std::map<VkDevice,std::unique_ptr<recomp::pcache::Saver>> byDevice; };
PcacheSavers& pcacheSavers() noexcept { static PcacheSavers* const all = new PcacheSavers; return *all; }
[[gnu::noinline]] VkPipelineCache pcacheCreate(ProbeDevice& d,bool& seeded) noexcept {
    namespace pc = recomp::pcache;
    const auto t0 = std::chrono::steady_clock::now();
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VkPipelineCache cache = VK_NULL_HANDLE;
    pc::Loaded seed;
    pc::Identity identity{};
    pc::DeviceId id{};
    const char* identitySource = "none";
    std::size_t cap = 0;
    bool on = false, handed = false, confirm = false;
    seeded = false;
    try {
        const std::string dir = RECOMP_PCACHE_DIR;
        on = !pc::switchOff(dir); // read once: here
        id.vendorID = d.properties.vendorID; id.deviceID = d.properties.deviceID;
        static_assert(sizeof id.uuid == sizeof d.properties.pipelineCacheUUID, "VK_UUID_SIZE");
        std::memcpy(id.uuid, d.properties.pipelineCacheUUID, sizeof id.uuid);
        cap = pc::effectiveCap(pc::DefaultCapBytes);
        identity = pc::libraryIdentity(&identitySource);
        if (on) seed = pc::load(dir, identity, id, cap);
        else seed.reason = pc::Reason::SwitchOff;
#if defined(RECOMP_PCACHE_MUTANT_NO_SEED) // host-test mutant: a good file is read and then not used
        if (seed.reason == pc::Reason::Ok) seed.reason = pc::Reason::Missing;
#endif
        if (seed.reason == pc::Reason::Ok) { // the guard: a file whose last use ended before the guard was removed is not used again
            if (pc::guardPresent(dir)) seed.reason = pc::Reason::Unfinished;
#if defined(RECOMP_PCACHE_MUTANT_UNGUARDED) // host-test mutant: the file is handed to the driver although the guard could not be made
            else (void)pc::guardSet(dir);
#else
            else if (!pc::guardSet(dir)) { seed.error = errno; seed.reason = pc::Reason::GuardFailed; }
#endif
        }
        if (seed.reason == pc::Reason::Ok) {
            info.initialDataSize = seed.file.size() - pc::HeaderBytes; // (load(): exactly the bytes that were read and checked)
            info.pInitialData = seed.payload();
            if (vkCreatePipelineCache(d.device, &info, nullptr, &cache) == VK_SUCCESS) handed = seeded = confirm = true;
            else { // the driver refused the data: the empty cache of every build before this one. (The guard stays until a
                   // save of this start has replaced the file: what the driver refused is not offered to it again.)
                cache = VK_NULL_HANDLE; seed.reason = pc::Reason::CreateFailed;
                info.initialDataSize = 0; info.pInitialData = nullptr;
            }
        }
#if defined(RECOMP_PCACHE_MUTANT_NO_FALLBACK) // host-test mutant: no second try after the driver refused the file's data
        if (!handed && seed.reason != pc::Reason::CreateFailed)
#else
        if (!handed)
#endif
            if (vkCreatePipelineCache(d.device, &info, nullptr, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE;
        pc::logLoad(on, seed, handed, cap, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), identity, identitySource, id);
        if (on && cache) {
            pc::Saver::Config config;
            config.dir = dir; config.identity = identity; config.device = id; config.cap = cap; config.confirm = confirm;
#if defined(RECOMP_PCACHE_TEST_TIMING) // host tests: milliseconds instead of minutes (the rules are the same)
            config.timing = RECOMP_PCACHE_TEST_TIMING;
#endif
            const VkDevice device = d.device;
            auto saver = std::make_unique<pc::Saver>(std::move(config), d.pipelineCacheMutex, [device, cache](std::size_t* size, void* data) {
                return int(vkGetPipelineCacheData(device, cache, size, data));
            });
            auto& all = pcacheSavers();
            std::lock_guard lock(all.mutex);
            all.byDevice[device] = std::move(saver);
        }
    } catch (...) { // (out of memory while reading the file or starting the thread: an empty cache, no saver; a guard that was
                    // made stays, so the next start does not hand the file on)
        if (!cache) { info.initialDataSize = 0; info.pInitialData = nullptr; if (vkCreatePipelineCache(d.device, &info, nullptr, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE; }
    }
    return cache;
}
[[gnu::noinline]] void pcacheStop(VkDevice device) noexcept {
    std::unique_ptr<recomp::pcache::Saver> saver;
    {
        auto& all = pcacheSavers();
        std::lock_guard lock(all.mutex);
        if (const auto it = all.byDevice.find(device); it != all.byDevice.end()) { saver = std::move(it->second); all.byDevice.erase(it); }
    }
    saver.reset(); // joins the thread: no vkGetPipelineCacheData after this line
}
}
// Attempt v78 (opt-in experiment): with files/private-trace/diag-generic-own-queue present when a device is created and a
// queue family offering two queues, the device gets a second queue (priority 0 against the first queue's 1) that only
// the generic renderer submits to (probeDedicatedQueue); the native engine keeps queue 0. Without the file: v71 (one queue).
namespace {
std::mutex dedicatedQueuesMutex;
std::map<VkDevice,VkQueue> dedicatedQueues;
std::map<VkDevice,VkQueue> presentQueues; // v259
std::map<VkDevice,std::pair<VkQueue,uint32_t>> sideQueues; // ios-side-queue
std::atomic<unsigned> familyQueueCount{0}; // v259 (logged by the direct present)
}
VkQueue probePresentQueue(VkDevice device) { // v259
    std::lock_guard lock(dedicatedQueuesMutex);
    const auto it=presentQueues.find(device);
    return it==presentQueues.end()?VK_NULL_HANDLE:it->second;
}
unsigned probeFamilyQueueCount() { return familyQueueCount.load(std::memory_order_relaxed); } // v259
// ios-side-queue (Claude 2026-10-21; MEASUREMENT ONLY, RECOMP_IOS_SIDE_QUEUE set when the device is made): MoltenVK offers four
// families of one queue each (VULKAN_QUEUE_FAMILY, c438), each its own Metal command queue; the device then also gets the
// queue of another family, for the presenter's experiments (does GPU work on a second queue overlap the game's frame?).
std::pair<VkQueue,uint32_t> probeSideQueue(VkDevice device) {
    std::lock_guard lock(dedicatedQueuesMutex);
    const auto it=sideQueues.find(device);
    return it==sideQueues.end()?std::pair<VkQueue,uint32_t>{VK_NULL_HANDLE,0u}:it->second;
}
// Attempt v255 (direct present): with files/private-trace/diag-direct-present present when the device is made, the instance
// also enables VK_KHR_surface + VK_KHR_android_surface and the device VK_KHR_swapchain (each only when offered), so the
// library can present to the app's window itself (direct_present.h). Without the file: v254 (no extensions).
namespace {
std::atomic<bool> surfaceInstanceEnabled{false};
std::mutex swapchainDevicesMutex;
std::set<VkDevice> swapchainDevices;
bool offered(const std::vector<VkExtensionProperties>& all,const char* name) {
    for (const auto& e : all) if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}
}
bool directPresentWanted() {
#if defined(__APPLE__)
    // ios-direct-present (Claude, 2026-10-16): the iOS shell selects V6 with RECOMP_IOS_DIRECT_PRESENT=1 (its private-trace folder
    // is not writable by the shell), so the same choice enables the surface extensions here.
    static const bool on = access(RECOMP_APP_PATH("/private-trace/diag-direct-present"), F_OK) == 0 ||
        [] { const char* e = std::getenv("RECOMP_IOS_DIRECT_PRESENT"); return e && e[0] == '1' && !e[1]; }();
#else
    static const bool on = access(RECOMP_APP_PATH("/private-trace/diag-direct-present"), F_OK) == 0;
#endif
    return on;
}
bool displayTimingEnabled() {return recomp_v318_display_timing.load(std::memory_order_acquire);} // v318
bool probeSwapchainEnabled(VkDevice device) {
    std::lock_guard lock(swapchainDevicesMutex);
    return swapchainDevices.count(device) != 0;
}
VkQueue probeDedicatedQueue(VkDevice device) {
    std::lock_guard lock(dedicatedQueuesMutex);
    const auto it=dedicatedQueues.find(device);
    return it==dedicatedQueues.end()?VK_NULL_HANDLE:it->second;
}
bool checkVulkan(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::printf("VULKAN_PROBE_ERROR %s %d\n", operation, static_cast<int>(result));
    return false;
}
ProbeDevice::~ProbeDevice() {
    if (device && ::recomp::apple::iosGpuLifecycle().snapshot().phase !=
            ::recomp::apple::IosGpuLifecycle::Phase::Disabled) {
        if (!::recomp::apple::iosGpuLifecycle().retireDevice(device) ||
                vkDeviceWaitIdle(device) != VK_SUCCESS) {
            std::fprintf(stderr,"APPLE_IOS_LIFECYCLE_TEARDOWN_FAILED owner=ProbeDevice\n");
            std::_Exit(4);
        }
    }
#if !defined(RECOMP_PCACHE_MUTANT_STOP_LATE) // host-test mutant: the saver is stopped only after the cache is destroyed
    if (device) pcacheStop(device); // boot-stall: the saver thread's last save, then no more use of the cache
#endif
    if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
#if defined(RECOMP_PCACHE_MUTANT_STOP_LATE)
    if (device) pcacheStop(device);
#endif
    if (device) {{std::lock_guard lock(swapchainDevicesMutex); swapchainDevices.erase(device);} vkDestroyDevice(device, nullptr);}
    if (instance) vkDestroyInstance(instance, nullptr);
}
// vk11 (v506): set by inspect() when the driver is Vulkan 1.1 and accepted; create() then names what 1.2 has in core.
static bool vk11Legacy = false;
static recomp::vk11::Offered vk11Offered{};
#if defined(__ANDROID__)
#define VK11_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", __VA_ARGS__)
#else
#define VK11_LOG(...) (std::printf(__VA_ARGS__), std::putchar('\n'))
#endif
bool ProbeDevice::inspect() {
    if (instance) return false;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Recomp headless probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    std::vector<const char*> instanceExtensions; // v255
#if defined(__APPLE__)
    const bool appleSurface = directPresentWanted();
    if (!recomp::apple::instanceExtensions(appleSurface, instanceExtensions, info.flags)) return false;
#else
    if (directPresentWanted()) {
        uint32_t n = 0;
        std::vector<VkExtensionProperties> all;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) == VK_SUCCESS && n && n < 1024) {
            all.resize(n);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data()) != VK_SUCCESS) all.clear();
            all.resize(std::min<std::size_t>(all.size(), n));
        }
        if (offered(all, "VK_KHR_surface") && offered(all, "VK_KHR_android_surface"))
            instanceExtensions = {"VK_KHR_surface", "VK_KHR_android_surface"};
        std::printf("VULKAN_V255_DIRECT_INSTANCE surface_extensions=%u\n", unsigned(!instanceExtensions.empty()));
    }
#endif
    info.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    info.ppEnabledExtensionNames = instanceExtensions.empty() ? nullptr : instanceExtensions.data();
    if (!checkVulkan(vkCreateInstance(&info, nullptr, &instance), "create-instance")) return false;
#if defined(__APPLE__)
    surfaceInstanceEnabled.store(appleSurface, std::memory_order_release);
#else
    surfaceInstanceEnabled.store(!instanceExtensions.empty(), std::memory_order_release);
#endif
    const auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!features2) return false;
    uint32_t count = 0;
    if (!checkVulkan(vkEnumeratePhysicalDevices(instance, &count, nullptr), "device-count") ||
            count == 0 || count > 16) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (!checkVulkan(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "devices")) return false;
    // Explicit diagnostic limitation: use device zero, no heuristic GPU choice.
    physical = devices[0];
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2) {
        // vk11 (v506): a 1.1 driver is taken when it offers descriptor indexing as an extension (vk11_gate.h). Drivers that
        // report 1.2 or newer never enter this block.
        std::vector<VkExtensionProperties> all;
        uint32_t n = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr) == VK_SUCCESS && n && n < 4096) {
            all.resize(n);
            if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, all.data()) != VK_SUCCESS) all.clear();
            all.resize(std::min<std::size_t>(all.size(), n));
        }
        vk11Offered = {offered(all, "VK_EXT_descriptor_indexing"), offered(all, "VK_EXT_scalar_block_layout"),
            offered(all, "VK_KHR_sampler_mirror_clamp_to_edge"), offered(all, "VK_KHR_buffer_device_address")};
        const auto first = recomp::vk11::plan(properties.apiVersion, vk11Offered, false, false);
        VK11_LOG("VULKAN_VK11 api=%u.%u.%u descriptor_indexing=%u scalar_block_layout=%u mirror_clamp=%u buffer_address=%u accepted=%u",
            VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion),
            unsigned(vk11Offered.descriptorIndexing), unsigned(vk11Offered.scalarBlockLayout), unsigned(vk11Offered.mirrorClampToEdge),
            unsigned(vk11Offered.bufferDeviceAddress), unsigned(first.accepted));
        if (!first.accepted) return false;
        vk11Legacy = true;
        recomp::vk11::legacyDriver.store(true, std::memory_order_release);
    }
    // Keep driver identity with each hardware result. Vendor-specific driver
    // versions are raw integers, not assumed Vulkan semantic versions.
    const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (!properties2) return false;
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 allProperties{};
    allProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    allProperties.pNext = &driver;
    properties2(physical, &allProperties);
    std::printf("VULKAN_DRIVER_V1 device_name=%.*s driver_version_raw=%u driver_id=%u "
        "driver_name=%.*s driver_info=%.*s conformance=%u.%u.%u.%u\n",
        VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, properties.deviceName, properties.driverVersion,
        static_cast<unsigned>(driver.driverID), VK_MAX_DRIVER_NAME_SIZE, driver.driverName,
        VK_MAX_DRIVER_INFO_SIZE, driver.driverInfo, driver.conformanceVersion.major,
        driver.conformanceVersion.minor, driver.conformanceVersion.subminor,
        driver.conformanceVersion.patch);
    indexing_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    addresses_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addresses_.pNext = &indexing_;
    VkPhysicalDeviceFeatures2 all{};
    all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    all.pNext = &addresses_;
    features2(physical, &all);
    features_ = all.features;
    std::printf("VULKAN_PROBE_V1 device=0 api=%u vendor=%u device_id=%u "
        "shaderInt64=%u shaderClipDistance=%u bufferDeviceAddress=%u runtimeDescriptorArray=%u "
        "sampledImageArrayDynamicIndexing=%u "
        "sampledImageNonUniformIndexing=%u descriptorBindingPartiallyBound=%u "
        "maxPushConstantsSize=%u maxBoundDescriptorSets=%u\n",
        properties.apiVersion, properties.vendorID, properties.deviceID,
        features_.shaderInt64, features_.shaderClipDistance, addresses_.bufferDeviceAddress,
        indexing_.runtimeDescriptorArray, features_.shaderSampledImageArrayDynamicIndexing,
        indexing_.shaderSampledImageArrayNonUniformIndexing,
        indexing_.descriptorBindingPartiallyBound, properties.limits.maxPushConstantsSize,
        properties.limits.maxBoundDescriptorSets);
    // Report support separately from device creation. These bits are NOT
    // enabled by this query and do not certify a third-party runtime or shader.
    std::printf("VULKAN_CORE_CAPS_V1 independentBlend=%u fragmentStoresAndAtomics=%u "
        "vertexPipelineStoresAndAtomics=%u geometryShader=%u fillModeNonSolid=%u "
        "maxStorageBufferRange=%u maxPerStageDescriptorStorageBuffers=%u\n",
        features_.independentBlend, features_.fragmentStoresAndAtomics,
        features_.vertexPipelineStoresAndAtomics, features_.geometryShader,
        features_.fillModeNonSolid, properties.limits.maxStorageBufferRange,
        properties.limits.maxPerStageDescriptorStorageBuffers);
    return true;
}
bool ProbeDevice::create(const ShaderRequirements& requirements) {
    return create(requirements,false);
}
bool ProbeDevice::create(const ShaderRequirements& requirements,bool samplerAnisotropy) {
    return create(requirements,samplerAnisotropy,false);
}
bool ProbeDevice::create(const ShaderRequirements& requirements,bool samplerAnisotropy,bool sampleRateShading) {
    return create(requirements,samplerAnisotropy,sampleRateShading,false);
}
bool ProbeDevice::create(const ShaderRequirements& requirements,bool samplerAnisotropy,bool sampleRateShading,bool independentBlend) {
    if (!physical || device) return false;
    // Conservative descriptor-heap profile: a runtime array capability does not
    // imply that its dynamically uniform accesses declare capability 29. Those
    // accesses still require the core feature to be explicitly enabled. This
    // does not authorize non-uniform indexing (unsupported by our module gate).
    const bool dynamicImages = requirements.sampledImageArrayDynamicIndexing ||
        requirements.runtimeDescriptorArray;
    if ((requirements.int64 && !features_.shaderInt64) ||
        (requirements.clipDistance && !features_.shaderClipDistance) ||
        (dynamicImages && !features_.shaderSampledImageArrayDynamicIndexing) ||
        (requirements.runtimeDescriptorArray && !indexing_.runtimeDescriptorArray) ||
        (requirements.bufferDeviceAddress && !addresses_.bufferDeviceAddress) ||
        (requirements.fragmentStoresAndAtomics && !features_.fragmentStoresAndAtomics) ||
        (requirements.vertexStoresAndAtomics && !features_.vertexPipelineStoresAndAtomics) ||
        (samplerAnisotropy && !features_.samplerAnisotropy) ||
        (sampleRateShading && !features_.sampleRateShading) ||
        (independentBlend && !features_.independentBlend)) {
        std::puts("VULKAN_PROBE_UNSUPPORTED required-shader-feature");
        if (vk11Legacy) VK11_LOG("VULKAN_VK11_DEVICE refused: required-shader-feature clip=%u dynamic_images=%u runtime_array=%u fragment_stores=%u",
            unsigned(features_.shaderClipDistance), unsigned(features_.shaderSampledImageArrayDynamicIndexing),
            unsigned(indexing_.runtimeDescriptorArray), unsigned(features_.fragmentStoresAndAtomics));
        return false;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    if (count == 0 || count > 256) return false;
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    for (uint32_t i = 0; i < count; ++i) // ios-queue-families (Claude 2026-10-21; diagnostic): what the driver offers for a second queue
        std::printf("VULKAN_QUEUE_FAMILY index=%u count=%u flags=%#x families=%u\n", i, queues[i].queueCount, unsigned(queues[i].queueFlags), count);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (queues[i].queueCount &&
            (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queueFamily = i; found = true; break;
        }
    }
    if (!found) return false;
    const bool secondQueue = queues[queueFamily].queueCount >= 2 &&
        access(RECOMP_APP_PATH("/private-trace/diag-generic-own-queue"), F_OK) == 0; // v78
    // v259: with the direct present, one more queue of the family (when offered) only for vkQueuePresentKHR, which on this
    // driver waits for the presented frame's GPU work (~4 ms): it must hold neither the shared queue nor the renderer.
    const unsigned renderQueues = secondQueue ? 2u : 1u;
    const bool presentQueue = surfaceInstanceEnabled.load(std::memory_order_acquire) && queues[queueFamily].queueCount > renderQueues;
    const float priorities[3] = {1.0f, secondQueue ? 0.0f : 1.0f, 1.0f};
    const bool sideQueue = std::getenv("RECOMP_IOS_SIDE_QUEUE") != nullptr && count >= 2; // ios-side-queue
    const uint32_t sideFamily = queueFamily == 0 ? 1u : 0u;
    const float sidePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = renderQueues + (presentQueue ? 1u : 0u);
    queueInfo.pQueuePriorities = priorities;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = requirements.int64;
    enabled.shaderClipDistance = requirements.clipDistance;
    enabled.shaderSampledImageArrayDynamicIndexing = dynamicImages;
    enabled.fragmentStoresAndAtomics = requirements.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics = requirements.vertexStoresAndAtomics;
    enabled.samplerAnisotropy = samplerAnisotropy;
    enabled.sampleRateShading = sampleRateShading;
    enabled.independentBlend = independentBlend;
    enabled.occlusionQueryPrecise = features_.occlusionQueryPrecise; // occlusion package
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    indexing.runtimeDescriptorArray = requirements.runtimeDescriptorArray;
    VkPhysicalDeviceBufferDeviceAddressFeatures addresses{};
    addresses.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addresses.bufferDeviceAddress = requirements.bufferDeviceAddress;
    addresses.pNext = &indexing;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &addresses;
    VkDeviceQueueCreateInfo queueInfos[2] = {queueInfo, {}}; // ios-side-queue: the other family's queue as a second entry
    queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;queueInfos[1].queueFamilyIndex = sideFamily;
    queueInfos[1].queueCount = 1;queueInfos[1].pQueuePriorities = &sidePriority;
    info.queueCreateInfoCount = sideQueue ? 2u : 1u;
    info.pQueueCreateInfos = queueInfos;
    info.pEnabledFeatures = &enabled;
    const char* swapchainExtension = "VK_KHR_swapchain"; // v255
    const char* storeNoneExtension = "VK_EXT_load_store_op_none"; // v293
    // v318: frame display times for the latency probe (used only while the timeline is armed; enabling it changes nothing else)
    const char* displayTimingExtension = "VK_GOOGLE_display_timing";
    bool swapchain = false, displayTiming = false;
    std::vector<VkExtensionProperties> all;
    {
        uint32_t n = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr) == VK_SUCCESS && n && n < 4096) {
            all.resize(n);
            if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, all.data()) != VK_SUCCESS) all.clear();
            all.resize(std::min<std::size_t>(all.size(), n));
        }
    }
    std::vector<const char*> extensions;
#if defined(__APPLE__)
    recomp::apple::appendDevicePortability(all, extensions);
#endif
    if (surfaceInstanceEnabled.load(std::memory_order_acquire)) {
        swapchain = offered(all, swapchainExtension);
        if (swapchain) extensions.push_back(swapchainExtension);
        displayTiming = swapchain && offered(all, displayTimingExtension);
        if (displayTiming) extensions.push_back(displayTimingExtension);
        std::printf("VULKAN_V255_DIRECT_DEVICE swapchain=%u display_timing=%u\n", unsigned(swapchain), unsigned(displayTiming));
    }
    // v293: STORE_OP_NONE for depth attachments the renderer proves unwritten in a pass (off: diag-v293-no-extension).
    const bool storeNone = offered(all, storeNoneExtension) &&
        access(RECOMP_APP_PATH("/private-trace/diag-v293-no-extension"), F_OK) != 0;
    if (storeNone) extensions.push_back(storeNoneExtension);
    std::printf("VULKAN_V293_STORE_NONE offered=%u enabled=%u\n", unsigned(offered(all, storeNoneExtension)), unsigned(storeNone));
    info.enabledExtensionCount = std::uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar{}; // v294
    scalar.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
    bool uboBanks = false;
    // v297: the default (two replicated run pairs: GPU -7.5..-9% house, -12.5..-16% outdoor); diag-v294-ssbo-banks restores the
    // storage-buffer banks at device creation.
    if (access(RECOMP_APP_PATH("/private-trace/diag-v294-ssbo-banks"), F_OK) != 0) {
        if (const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"))) {
            VkPhysicalDeviceFeatures2 q{};q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;q.pNext = &scalar;
            query(physical, &q);
            if (scalar.scalarBlockLayout) {scalar.pNext = const_cast<void*>(info.pNext);info.pNext = &scalar;uboBanks = true;}
        }
    }
    std::printf("VULKAN_V294_UBO_BANKS wanted=%u scalar_block_layout=%u\n",
        unsigned(access(RECOMP_APP_PATH("/private-trace/diag-v294-ssbo-banks"), F_OK) != 0), unsigned(uboBanks));
    // depth-float: diag-df-float + diag-df-bias-abs -> VK_EXT_depth_bias_control (floatRepresentation) when offered; without both files
    // nothing here runs (the device is v436's).
    VkPhysicalDeviceDepthBiasControlFeaturesEXT biasControl{};
    biasControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT;
    unsigned dfBias = 0;
    const char* biasExtension = "VK_EXT_depth_bias_control";
    const bool dfBiasWanted = access(RECOMP_APP_PATH("/private-trace/diag-df-float"), F_OK) == 0 &&
        access(RECOMP_APP_PATH("/private-trace/diag-df-bias-abs"), F_OK) == 0;
    if (dfBiasWanted && offered(all, biasExtension)) {
        if (const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"))) {
            VkPhysicalDeviceFeatures2 q{};q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;q.pNext = &biasControl;
            query(physical, &q);
            if (biasControl.depthBiasControl && biasControl.floatRepresentation) {
                biasControl.pNext = const_cast<void*>(info.pNext);info.pNext = &biasControl; // (every feature it reports, enabled)
                extensions.push_back(biasExtension);
                info.enabledExtensionCount = std::uint32_t(extensions.size());info.ppEnabledExtensionNames = extensions.data();
                dfBias = biasControl.depthBiasExact ? 2u : 1u;
            }
        }
    }
    if (dfBiasWanted)
        std::printf("VULKAN_DF_BIAS offered=%u depth_bias_control=%u float=%u exact=%u enabled=%u\n", unsigned(offered(all, biasExtension)),
            unsigned(biasControl.depthBiasControl), unsigned(biasControl.floatRepresentation), unsigned(biasControl.depthBiasExact), dfBias);
    if (vk11Legacy) { // vk11 (v506): name what Vulkan 1.2 has in core; 1.2 and newer drivers skip this block
        const auto p = recomp::vk11::plan(properties.apiVersion, vk11Offered, uboBanks, requirements.bufferDeviceAddress);
        if (!p.accepted) {VK11_LOG("VULKAN_VK11_DEVICE refused: a required extension is not offered"); return false;}
        if (p.enableDescriptorIndexing) extensions.push_back("VK_EXT_descriptor_indexing");
        if (p.enableScalarBlockLayout) extensions.push_back("VK_EXT_scalar_block_layout");
        if (p.enableMirrorClampToEdge) extensions.push_back("VK_KHR_sampler_mirror_clamp_to_edge");
        if (p.enableBufferDeviceAddress) extensions.push_back("VK_KHR_buffer_device_address");
        else { // its extension is not enabled: the all-false feature structure leaves the chain
            if (info.pNext == &addresses) info.pNext = &indexing;
            if (scalar.pNext == &addresses) scalar.pNext = &indexing;
            if (biasControl.pNext == &addresses) biasControl.pNext = &indexing;
        }
        info.enabledExtensionCount = std::uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        VK11_LOG("VULKAN_VK11_DEVICE extensions=%u descriptor_indexing=%u scalar_block_layout=%u mirror_clamp=%u buffer_address=%u ubo_banks=%u store_none=%u",
            unsigned(extensions.size()), unsigned(p.enableDescriptorIndexing), unsigned(p.enableScalarBlockLayout),
            unsigned(p.enableMirrorClampToEdge), unsigned(p.enableBufferDeviceAddress), unsigned(uboBanks), unsigned(storeNone));
    }
    const VkResult vk11Created = vkCreateDevice(physical, &info, nullptr, &device);
    if (vk11Legacy) VK11_LOG("VULKAN_VK11_DEVICE create_result=%d", int(vk11Created));
    if (!checkVulkan(vk11Created, "create-device")) return false;
    recomp_df_bias_float.store(dfBias, std::memory_order_release);
    recomp_v293_store_none.store(storeNone, std::memory_order_release);
    recomp_v318_display_timing.store(displayTiming, std::memory_order_release);
    recomp_v294_ubo_banks.store(uboBanks, std::memory_order_release);
    recomp_ocq_precise.store(enabled.occlusionQueryPrecise == VK_TRUE, std::memory_order_release); // occlusion package
    std::printf("VULKAN_OCQ_PRECISE offered=%u enabled=%u\n", unsigned(features_.occlusionQueryPrecise), unsigned(enabled.occlusionQueryPrecise));
    if (swapchain) {std::lock_guard lock(swapchainDevicesMutex); swapchainDevices.insert(device);}
    fragmentStoresAndAtomicsEnabled = enabled.fragmentStoresAndAtomics;
    vertexStoresAndAtomicsEnabled = enabled.vertexPipelineStoresAndAtomics;
    samplerAnisotropyEnabled = enabled.samplerAnisotropy;
    sampleRateShadingEnabled = enabled.sampleRateShading;
    independentBlendEnabled = enabled.independentBlend;
    std::printf("VULKAN_MRT_POLICY independent_blend_enabled=%u\n",enabled.independentBlend);
    std::printf("VULKAN_SAMPLE_RATE_POLICY enabled=%u\n",enabled.sampleRateShading);
    std::printf("VULKAN_NATIVE_SAMPLER_POLICY anisotropy_enabled=%u max_anisotropy=%.9g\n",
        enabled.samplerAnisotropy,properties.limits.maxSamplerAnisotropy);
    std::printf("VULKAN_ENABLED_V1 shaderInt64=%u sampledImageArrayDynamicIndexing=%u "
        "runtimeDescriptorArray=%u\n", enabled.shaderInt64,
        enabled.shaderSampledImageArrayDynamicIndexing, indexing.runtimeDescriptorArray);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if(!::recomp::apple::iosGpuLifecycle().registerQueue(device,queue))return false;
    std::printf("VULKAN_V78_QUEUES family=%u family_queues=%u created=%u\n", queueFamily, queues[queueFamily].queueCount, secondQueue ? 2u : 1u);
    if (secondQueue) {
        VkQueue second = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queueFamily, 1, &second);
        if(!::recomp::apple::iosGpuLifecycle().registerQueue(device,second))return false;
        if (second) {std::lock_guard lock(dedicatedQueuesMutex); dedicatedQueues[device] = second;}
    }
    if (presentQueue) { // v259
        VkQueue present = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queueFamily, renderQueues, &present);
        if(!::recomp::apple::iosGpuLifecycle().registerQueue(device,present))return false;
        if (present) {std::lock_guard lock(dedicatedQueuesMutex); presentQueues[device] = present;}
    }
    if (sideQueue) { // ios-side-queue
        VkQueue side = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, sideFamily, 0, &side);
        if(!::recomp::apple::iosGpuLifecycle().registerQueue(device,side))return false;
        if (side) {std::lock_guard lock(dedicatedQueuesMutex); sideQueues[device] = {side, sideFamily};}
        std::printf("VULKAN_SIDE_QUEUE family=%u queue=%p\n", sideFamily, (void*)side);
    }
    familyQueueCount.store(queues[queueFamily].queueCount, std::memory_order_relaxed);
    // boot-stall (2026-10-06): the cache is seeded from this app's own files/private-trace/pipeline-cache.bin when that file
    // passes every check (pcacheCreate above); otherwise it starts empty, as it always did before. What a pipeline compiles
    // to is still decided by the driver from the shader code this process hands it: a cache entry only spares the compile.
    // A failure here is not fatal, because VK_NULL_HANDLE is exactly the previous behaviour.
    bool seededFromDisk = false;
    pipelineCache = pcacheCreate(*this, seededFromDisk);
    std::printf("VULKAN_PIPELINE_CACHE_POLICY created=%u seeded_from_disk=%u\n",
        pipelineCache != VK_NULL_HANDLE, unsigned(seededFromDisk));
    return queue != VK_NULL_HANDLE;
}
}
