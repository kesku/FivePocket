#include "guest_thread_storage_pool.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace recomp::bootstrap {
namespace {

std::uint64_t end(GuestThreadMemoryRange range) noexcept {
    return static_cast<std::uint64_t>(range.base) + range.size;
}

bool validRange(GuestThreadMemoryRange range) noexcept {
    return range.base != 0 && range.size != 0 && end(range) <= GuestAddressSpace::Size;
}

bool overlap(GuestThreadMemoryRange a, GuestThreadMemoryRange b) noexcept {
    return a.size != 0 && b.size != 0 && a.base < end(b) && b.base < end(a);
}

bool contains(GuestThreadMemoryRange outer, GuestThreadMemoryRange inner) noexcept {
    return validRange(inner) && inner.base >= outer.base && end(inner) <= end(outer);
}

std::uint32_t roundPage(std::uint32_t size) noexcept {
    // Callers first bound size to <= 16 MiB (stack) or <= 1 MiB + 8192 (TLS).
    return (size + GuestThreadStoragePool::PageSize - 1U)
            & ~(GuestThreadStoragePool::PageSize - 1U);
}

std::optional<std::uint32_t> stackSize(std::uint32_t requested) noexcept {
    if (requested == 0 || requested > GuestThreadStoragePool::MaximumStackSize) {
        return std::nullopt;
    }
    // Reference: Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
    // src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc: ExCreateThread_entry.
    return std::max(GuestThreadStoragePool::MinimumStackSize, roundPage(requested));
}

bool codeAddress(GuestThreadMemoryRange code, std::uint32_t address) noexcept {
    return (address & 3U) == 0 && contains(code, {address, 4});
}

} // namespace

GuestThreadStoragePool::~GuestThreadStoragePool() {
    reset();
}

bool GuestThreadStoragePool::initialize(
        GuestAddressSpace& addressSpace,
        const GuestThreadStorageConfig& config,
        const std::vector<GuestThreadMemoryRange>& excludedRanges) {
    if (addressSpace_ != nullptr || !addressSpace.isReserved()
            || !validRange(config.arena) || !validRange(config.image)
            || !contains(config.image, config.code)
            || ((config.code.base | config.code.size) & 3U) != 0
            || ((config.arena.base | config.arena.size) & (PageSize - 1U)) != 0
            || config.arena.size > MaximumArenaSize
            // Stack's exclusive high address must itself fit in a guest GPR.
            || end(config.arena) > std::numeric_limits<std::uint32_t>::max()
            || overlap(config.arena, config.image)
            || !stackSize(config.defaultStackSize).has_value()
            || config.firstThreadId < 2
            || config.tlsDataSize > GuestThreadEnvironment::MaxStaticTlsSize
            || (config.tlsDataSize & 3U) != 0
            || config.tlsSlotCount > GuestThreadEnvironment::MaxTlsSlots
            || config.tlsRawDataSize > config.tlsDataSize
            || (config.tlsRawDataSize != 0
                && !contains(config.image, {config.tlsRawDataAddress, config.tlsRawDataSize}))
            || excludedRanges.size() > MaximumExcludedRanges) {
        return false;
    }
    const auto tlsBytes = config.tlsDataSize + config.tlsSlotCount * 4U;
    if (tlsBytes == 0) {
        return false;
    }
    for (const auto& range : excludedRanges) {
        if (!validRange(range) || overlap(config.arena, range)) {
            return false;
        }
    }
    const auto tlsAllocation = roundPage(tlsBytes);
    if (static_cast<std::uint64_t>(PageSize) * 2U + tlsAllocation + MinimumStackSize
            > config.arena.size) {
        return false;
    }
    config_ = config;
    addressSpace_ = &addressSpace;
    tlsAllocationSize_ = tlsAllocation;
    nextThreadId_ = std::max(nextThreadId_, static_cast<std::uint64_t>(config.firstThreadId));
    return true;
}

GuestThreadStorageResult GuestThreadStoragePool::prepare(
        const GuestThreadStorageRequest& request) {
    if (!ready()) {
        return {};
    }
    const auto actualStack = stackSize(request.stackSize == 0
            ? config_.defaultStackSize : request.stackSize);
    if (!validCreationFlags(request.creationFlags) || !actualStack
            || (request.wrapStartContext && request.xapiStartup != 0)
            || !codeAddress(config_.code, request.startRoutine)
            || (request.xapiStartup != 0 && !codeAddress(config_.code, request.xapiStartup))) {
        return {GuestThreadStorageState::InvalidRequest, std::nullopt};
    }
    const auto slot = std::find_if(threads_.begin(), threads_.end(),
            [](const auto& thread) { return !thread.has_value(); });
    if (slot == threads_.end() || nextThreadId_ > std::numeric_limits<std::uint32_t>::max()) {
        return {GuestThreadStorageState::CapacityExceeded, std::nullopt};
    }
    const auto allocationSize = PageSize * (request.wrapStartContext ? 3U : 2U) + tlsAllocationSize_ + *actualStack;
    const auto base = findBlock(allocationSize);
    if (!base) {
        return {GuestThreadStorageState::ArenaExhausted, std::nullopt};
    }
    GuestPreparedThread prepared{};
    prepared.allocation = {*base, allocationSize};
    prepared.xapiStartup = request.xapiStartup;
    prepared.startRoutine = request.startRoutine;
    prepared.startContext = request.startContext;
    prepared.creationFlags = request.creationFlags;
    auto& layout = prepared.layout;
    layout.pcrAddress = *base;
    layout.threadAddress = *base + PageSize;
    layout.tlsAddress = *base + PageSize * 2U;
    layout.imageBase = config_.image.base;
    layout.imageSize = config_.image.size;
    layout.stackBase = layout.tlsAddress + tlsAllocationSize_;
    if (request.wrapStartContext) {
        prepared.wrappedContextAddress=layout.stackBase;
        prepared.startContext=prepared.wrappedContextAddress;
        layout.stackBase+=PageSize;
    }
    layout.stackSize = *actualStack;
    layout.tlsRawDataAddress = config_.tlsRawDataAddress;
    layout.tlsRawDataSize = config_.tlsRawDataSize;
    layout.tlsDataSize = config_.tlsDataSize;
    layout.tlsSlotCount = config_.tlsSlotCount;
    layout.threadId = static_cast<std::uint32_t>(nextThreadId_);
    prepared.dynamicTlsAddress = config_.tlsSlotCount == 0
            ? 0 : layout.tlsAddress + config_.tlsDataSize;

    // No map/host-thread allocation occurs during publication. The environment
    // helper may allocate diagnostic strings; a failure scrubs the unowned block
    // and leaves both the live table and next ID unchanged.
    try {
        GuestThreadEnvironment environment;
        std::memset(addressSpace_->translate(*base), 0, allocationSize);
        if (!environment.initialize(*addressSpace_, layout)) {
            scrub(prepared);
            return {GuestThreadStorageState::InitializationFailed, std::nullopt};
        }
        const auto mask = request.creationFlags >> 24U;
        std::uint8_t cpu = 0;
        if (mask != 0) { while ((1U << cpu) != mask) { ++cpu; } }
        *addressSpace_->translate(layout.pcrAddress + 0x10CU) = cpu;
        *addressSpace_->translate(layout.threadAddress + 0xBFU) = cpu;
        // Preserve the original flags at the Xbox KTHREAD offset, including
        // CREATE_SUSPENDED. Resuming changes live state, not creation history.
        auto* flags = addressSpace_->translate(layout.threadAddress + 0x16CU);
        for (unsigned i = 0; i < 4; ++i) {
            flags[i] = static_cast<std::uint8_t>(request.creationFlags >> (24 - 8 * i));
        }
        if (prepared.wrappedContextAddress) {
            auto* word=addressSpace_->translate(prepared.wrappedContextAddress);
            for(unsigned i=0;i<4;++i) word[i]=static_cast<std::uint8_t>(request.startContext>>(24-8*i));
        }
        // GuestThreadEnvironment is not an RAII scrubber; ownership of its
        // prepared bytes passes to this pool, which scrubs the entire block.
    } catch (const std::bad_alloc&) {
        scrub(prepared);
        return {GuestThreadStorageState::InitializationFailed, std::nullopt};
    }
    *slot = prepared;
    ++nextThreadId_;
    return {GuestThreadStorageState::Prepared, prepared};
}

std::optional<std::uint32_t> GuestThreadStoragePool::findBlock(std::uint32_t size) const {
    std::array<GuestThreadMemoryRange, MaximumThreads> allocations{};
    std::size_t count = 0;
    for (const auto& thread : threads_) {
        if (thread) {
            allocations[count++] = thread->allocation;
        }
    }
    std::sort(allocations.begin(), allocations.begin() + count,
            [](auto a, auto b) { return a.base < b.base; });
    std::uint64_t candidate = config_.arena.base;
    for (std::size_t i = 0; i < count; ++i) {
        if (candidate + size <= allocations[i].base) {
            return static_cast<std::uint32_t>(candidate);
        }
        candidate = end(allocations[i]);
    }
    return candidate + size <= end(config_.arena)
            ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(candidate))
            : std::nullopt;
}

std::optional<GuestPreparedThread> GuestThreadStoragePool::lookup(std::uint32_t threadId) const {
    if (ready()) {
        for (const auto& thread : threads_) {
            if (thread && thread->layout.threadId == threadId) {
                return thread;
            }
        }
    }
    return std::nullopt;
}

void GuestThreadStoragePool::scrub(const GuestPreparedThread& thread) noexcept {
    if (ready()) {
        std::memset(addressSpace_->translate(thread.allocation.base), 0, thread.allocation.size);
    }
}

bool GuestThreadStoragePool::release(std::uint32_t threadId) {
    if (!ready()) {
        return false;
    }
    for (auto& thread : threads_) {
        if (thread && thread->layout.threadId == threadId) {
            scrub(*thread);
            thread.reset();
            return true;
        }
    }
    return false;
}

void GuestThreadStoragePool::reset() noexcept {
    for (auto& thread : threads_) {
        if (thread) {
            scrub(*thread);
            thread.reset();
        }
    }
    addressSpace_ = nullptr;
    config_ = {};
    tlsAllocationSize_ = 0;
}

bool GuestThreadStoragePool::ready() const noexcept {
    return addressSpace_ != nullptr && addressSpace_->isReserved();
}

std::size_t GuestThreadStoragePool::size() const noexcept {
    return static_cast<std::size_t>(std::count_if(threads_.begin(), threads_.end(),
            [](const auto& thread) { return thread.has_value(); }));
}

} // namespace recomp::bootstrap
