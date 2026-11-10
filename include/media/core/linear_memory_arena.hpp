#pragma once

#include "media/core/platform.hpp"

#include <memory>
#include <type_traits>
#include <utility>

namespace media::core {

enum class ArenaError : std::uint8_t {
    None, AlreadyInitialized, NotInitialized, InvalidSize, InvalidAlignment,
    OutOfMemory, Sealed, OsFailure
};

struct ArenaStatistics {
    std::size_t capacity_bytes;       // Exact requested usable capacity.
    std::size_t committed_bytes;      // Page-rounded OS commitment.
    std::size_t used_bytes;           // Includes inter-allocation alignment gaps.
    std::size_t requested_bytes;      // Sum of successful request sizes.
    std::size_t padding_bytes;
    std::size_t high_water_bytes;
    std::uint64_t allocation_count;
    std::uint64_t failed_allocation_count;
};

// Single-owner setup allocator. Publish its objects only after initialization.
// Seal before playback; statistics/lifecycle calls require external quiescence.
class LinearMemoryArena final {
public:
    LinearMemoryArena() noexcept = default;
    ~LinearMemoryArena() noexcept;
    LinearMemoryArena(const LinearMemoryArena&) = delete;
    LinearMemoryArena& operator=(const LinearMemoryArena&) = delete;
    LinearMemoryArena(LinearMemoryArena&&) = delete;
    LinearMemoryArena& operator=(LinearMemoryArena&&) = delete;

    // Obtains native virtual memory and write-touches every page before returning.
    [[nodiscard]] bool Initialize(std::size_t capacity_bytes) noexcept;
    // A nested arena over storage another arena owns (an engine module's region, docs/sdk.md "Modules"): no OS call, and
    // Shutdown only forgets the storage. It allocates, seals and resets like any arena. `storage` must be 64-byte aligned and
    // outlive the view. A view may be initialised again (a new region) after Shutdown.
    [[nodiscard]] bool InitializeView(void* storage, std::size_t capacity_bytes) noexcept;
    [[nodiscard]] bool IsView() const noexcept { return view_; }
    // At least 64-byte alignment; any larger power-of-two alignment is accepted.
    [[nodiscard]] void* Allocate(std::size_t bytes,
                                 std::size_t alignment = kCacheLineBytes) noexcept;

    // construct_at starts object lifetime in existing storage; it allocates no heap.
    // Restrict destruction to trivial objects so Shutdown needs no destructor list.
    template <typename T, typename... Args>
    [[nodiscard]] T* Create(Args&&... args) noexcept {
        static_assert(!std::is_array_v<T> && !std::is_const_v<T>);
        static_assert(std::is_nothrow_constructible_v<T, Args...>);
        static_assert(std::is_trivially_destructible_v<T>);
        void* storage = Allocate(sizeof(T), alignof(T));
        if (storage == nullptr) return nullptr;
        return std::construct_at(static_cast<T*>(storage), std::forward<Args>(args)...);
    }

    [[nodiscard]] bool Seal() noexcept;
    // All users, checked-out slots and GPU uses of CPU storage must be retired first.
    // Rewinds to empty and unseals, keeping the committed (and touched) pages: the next file of a warm session
    // (docs/session.md) prepares in memory that costs nothing to commit again. Every object created from the arena
    // must be dead by then: its owner closed it. Preparing only.
    [[nodiscard]] bool Reset() noexcept;
    [[nodiscard]] bool Shutdown() noexcept;
    [[nodiscard]] bool IsSealed() const noexcept { return sealed_; }
    [[nodiscard]] ArenaStatistics Statistics() const noexcept { return stats_; }
    [[nodiscard]] ArenaError LastError() const noexcept { return error_; }
    [[nodiscard]] std::uint32_t LastOsError() const noexcept { return os_error_; }

private:
    [[nodiscard]] void* AllocationFailure(ArenaError error) noexcept;
    std::byte* base_ = nullptr;
    ArenaStatistics stats_{};
    ArenaError error_ = ArenaError::None;
    std::uint32_t os_error_ = 0;
    bool sealed_ = false;
    bool view_ = false;
};
} // namespace media::core
