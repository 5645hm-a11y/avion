#pragma once
#include "media/core/linear_memory_arena.hpp"
#include "media/platform/IAudioSink.hpp"
#include <atomic>
#include <span>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324)
#endif
namespace media::audio {
// Producer checks out PCM; audio owner consumes. Fixed capacity in frames, not
// bytes. Initialize/quiescent destruction are outside streaming.
class PcmRingBuffer final {
public:
    [[nodiscard]] bool Initialize(core::LinearMemoryArena&, std::uint32_t capacity_frames,
                                  std::uint32_t bytes_per_frame) noexcept;
    [[nodiscard]] platform::EndpointStatus TryAcquire(std::uint32_t frames, platform::AudioWriteSpan&) noexcept;
    [[nodiscard]] platform::EndpointStatus Commit(std::uint64_t lease, std::uint32_t frames) noexcept;
    // Consumer only; destination must hold whole frames. Returns consumed frames.
    [[nodiscard]] std::uint32_t ReadInto(std::span<std::byte> destination) noexcept;
    // Consumer only (seek flush): consumes every published frame without reading it. Returns the count.
    std::uint32_t Discard() noexcept;
    // Consumer only: consumes up to `frames` published frames without reading them (a null endpoint playing them).
    std::uint32_t Skip(std::uint32_t frames) noexcept;
    [[nodiscard]] std::uint32_t Capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t QueuedFrames() const noexcept; // Consumer only.
    [[nodiscard]] std::uint32_t FreeFrames() const noexcept;   // Producer only: frames Commit could still publish now.
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::byte* data_ = nullptr;
    std::uint32_t capacity_ = 0, frame_bytes_ = 0;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::uint64_t producer_ = 0;
    std::uint64_t next_lease_ = 0;
    std::uint32_t checked_frames_ = 0;
    alignas(64) std::uint64_t consumer_ = 0;
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
