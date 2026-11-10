#pragma once

#include "media/core/platform.hpp"

#include <atomic>
#include <limits>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Cache-line tail padding is intentional and asserted.
#endif

namespace media::core {

// Exactly one producer and one consumer. No concurrent destruction or role changes.
// Full Capacity is usable. Each operation is bounded; there are no internal retries.
template <typename T, std::size_t Capacity>
class alignas(alignof(T) > kCacheLineBytes ? alignof(T) : kCacheLineBytes) SpscRingBuffer final {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a nonzero power of two.");
    static_assert(Capacity < (std::uint64_t{1} << 63));
    static_assert(std::is_trivial_v<T> && std::is_standard_layout_v<T>,
                  "Slots must be trivial standard-layout transport values.");
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    struct alignas(kCacheLineBytes) PublishedCursor {
        std::atomic<std::uint64_t> value;
    };
    struct alignas(kCacheLineBytes) LocalCursor {
        std::uint64_t next;
        std::uint64_t cached_peer;
        bool checked_out;
    };
    // Padding even small T prevents adjacent producer/consumer slots sharing a line.
    // Over-aligned T also retains its natural alignment.
    struct alignas(alignof(T) > kCacheLineBytes ? alignof(T) : kCacheLineBytes) Slot {
        T value;
    };
    static_assert(sizeof(PublishedCursor) == kCacheLineBytes);
    static_assert(sizeof(LocalCursor) == kCacheLineBytes);
    static_assert(sizeof(Slot) % kCacheLineBytes == 0);
    static_assert(Capacity <= ((std::numeric_limits<std::size_t>::max)() -
                              4 * kCacheLineBytes) / sizeof(Slot));

public:
    // Nonzero sequence supports resumed logical numbering and wraparound testing.
    explicit SpscRingBuffer(const std::uint64_t initial_sequence = 0) noexcept
        : published_write_{initial_sequence}, published_read_{initial_sequence},
          producer_{initial_sequence, initial_sequence, false},
          consumer_{initial_sequence, initial_sequence, false}, slots_{} {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] T* AcquireWriteSlot() noexcept {
        if (producer_.checked_out) return WritePointer();
        if (producer_.next - producer_.cached_peer == Capacity) {
            producer_.cached_peer = published_read_.value.load(std::memory_order_acquire);
            if (producer_.next - producer_.cached_peer == Capacity) return nullptr;
        }
        producer_.checked_out = true;
        return WritePointer();
    }

    // Invalidates all producer pointers to this slot, including aliases.
    [[nodiscard]] bool CommitWriteSlot() noexcept {
        if (!producer_.checked_out) return false;
        producer_.checked_out = false;
        ++producer_.next; // Unsigned modulo arithmetic is intentional.
        published_write_.value.store(producer_.next, std::memory_order_release);
        return true;
    }

    // Leaves the slot unpublished; the next checkout must fully initialize its value.
    [[nodiscard]] bool CancelWriteSlot() noexcept {
        if (!producer_.checked_out) return false;
        producer_.checked_out = false;
        return true;
    }

    [[nodiscard]] const T* PeekReadSlot() noexcept {
        if (consumer_.checked_out) return ReadPointer();
        if (consumer_.next == consumer_.cached_peer) {
            consumer_.cached_peer = published_write_.value.load(std::memory_order_acquire);
            if (consumer_.next == consumer_.cached_peer) return nullptr;
        }
        consumer_.checked_out = true;
        return ReadPointer();
    }

    // Invalidates all consumer pointers to this slot. Does not retire payload/GPU use.
    [[nodiscard]] bool ReleaseReadSlot() noexcept {
        if (!consumer_.checked_out) return false;
        consumer_.checked_out = false;
        ++consumer_.next;
        published_read_.value.store(consumer_.next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] static constexpr std::size_t GetCapacity() noexcept { return Capacity; }

private:
    [[nodiscard]] T* WritePointer() noexcept {
        return &slots_[static_cast<std::size_t>(producer_.next & (Capacity - 1))].value;
    }
    [[nodiscard]] const T* ReadPointer() const noexcept {
        return &slots_[static_cast<std::size_t>(consumer_.next & (Capacity - 1))].value;
    }

    PublishedCursor published_write_;
    PublishedCursor published_read_;
    LocalCursor producer_;
    LocalCursor consumer_;
    Slot slots_[Capacity];
};
} // namespace media::core

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
