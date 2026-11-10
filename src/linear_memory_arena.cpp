#include "media/core/linear_memory_arena.hpp"

#include "media/platform/SystemMemory.hpp"
#include <limits>

namespace media::core {

LinearMemoryArena::~LinearMemoryArena() noexcept {
    (void)Shutdown();
}

bool LinearMemoryArena::Initialize(const std::size_t capacity_bytes) noexcept {
    os_error_ = 0;
    if (base_ != nullptr) {
        error_ = ArenaError::AlreadyInitialized;
        return false;
    }
    constexpr auto max = (std::numeric_limits<std::size_t>::max)();
    if (capacity_bytes == 0 || capacity_bytes > max / 2) {
        error_ = ArenaError::InvalidSize;
        return false;
    }
    platform::MemoryRegion region{};
    if (!platform::ReserveCommitted(capacity_bytes, region, os_error_)) {
        error_ = ArenaError::OsFailure;
        return false;
    }
    // Commit alone doesn't materialize physical pages. Volatile prevents elision.
    auto* pages = static_cast<volatile unsigned char*>(region.address);
    for (std::size_t offset = 0; offset < region.bytes; offset += region.page_bytes) pages[offset] = 0;
    base_ = static_cast<std::byte*>(region.address);
    stats_ = {};
    stats_.capacity_bytes = capacity_bytes;
    stats_.committed_bytes = region.bytes;
    sealed_ = false;
    error_ = ArenaError::None;
    return true;
}

bool LinearMemoryArena::InitializeView(void* storage, const std::size_t capacity_bytes) noexcept {
    os_error_ = 0;
    if (base_ != nullptr) {
        error_ = ArenaError::AlreadyInitialized;
        return false;
    }
    if (storage == nullptr || capacity_bytes == 0) {
        error_ = ArenaError::InvalidSize;
        return false;
    }
    if ((reinterpret_cast<std::uintptr_t>(storage) & (kCacheLineBytes - 1)) != 0) {
        error_ = ArenaError::InvalidAlignment;
        return false;
    }
    base_ = static_cast<std::byte*>(storage);
    stats_ = {};
    stats_.capacity_bytes = capacity_bytes;
    stats_.committed_bytes = capacity_bytes; // Committed (and touched) by the arena that owns it.
    sealed_ = false;
    view_ = true;
    error_ = ArenaError::None;
    return true;
}

void* LinearMemoryArena::AllocationFailure(const ArenaError error) noexcept {
    error_ = error;
    if (stats_.failed_allocation_count != (std::numeric_limits<std::uint64_t>::max)())
        ++stats_.failed_allocation_count;
    return nullptr;
}

void* LinearMemoryArena::Allocate(const std::size_t bytes, std::size_t alignment) noexcept {
    os_error_ = 0;
    if (base_ == nullptr) return AllocationFailure(ArenaError::NotInitialized);
    if (sealed_) return AllocationFailure(ArenaError::Sealed);
    if (bytes == 0) return AllocationFailure(ArenaError::InvalidSize);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
        return AllocationFailure(ArenaError::InvalidAlignment);
    if (alignment < kCacheLineBytes) alignment = kCacheLineBytes;

    // Align the absolute address: a native mapping needn't satisfy huge alignments.
    const auto address = reinterpret_cast<std::uintptr_t>(base_) + stats_.used_bytes;
    const auto padding = (std::uintptr_t{0} - address) & (alignment - 1);
    const auto remaining = stats_.capacity_bytes - stats_.used_bytes;
    // Subtraction-first checks avoid overflow even for SIZE_MAX requests.
    if (padding > remaining || bytes > remaining - padding)
        return AllocationFailure(ArenaError::OutOfMemory);

    auto* result = base_ + stats_.used_bytes + padding;
    stats_.used_bytes += padding + bytes;
    stats_.requested_bytes += bytes;
    stats_.padding_bytes += padding;
    stats_.high_water_bytes = stats_.used_bytes;
    ++stats_.allocation_count;
    error_ = ArenaError::None;
    return result;
}

bool LinearMemoryArena::Seal() noexcept {
    os_error_ = 0;
    if (base_ == nullptr) {
        error_ = ArenaError::NotInitialized;
        return false;
    }
    sealed_ = true;
    error_ = ArenaError::None;
    return true;
}

bool LinearMemoryArena::Reset() noexcept {
    os_error_ = 0;
    if (base_ == nullptr) {
        error_ = ArenaError::NotInitialized;
        return false;
    }
    const auto capacity = stats_.capacity_bytes, committed = stats_.committed_bytes;
    stats_ = {};
    stats_.capacity_bytes = capacity;
    stats_.committed_bytes = committed;
    sealed_ = false;
    error_ = ArenaError::None;
    return true;
}

bool LinearMemoryArena::Shutdown() noexcept {
    os_error_ = 0;
    if (base_ != nullptr && !view_ && !platform::ReleaseCommitted({base_, stats_.committed_bytes, 0}, os_error_)) {
        error_ = ArenaError::OsFailure;
        return false; // Keep ownership so an explicit caller can retry.
    }
    base_ = nullptr;
    view_ = false;
    stats_ = {};
    sealed_ = false;
    error_ = ArenaError::None;
    return true;
}
} // namespace media::core
