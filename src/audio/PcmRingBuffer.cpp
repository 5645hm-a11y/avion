#include "media/audio/PcmRingBuffer.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace media::audio {
using platform::EndpointStatus;
bool PcmRingBuffer::Initialize(core::LinearMemoryArena& arena, std::uint32_t frames, std::uint32_t stride) noexcept {
    if (data_ || !frames || (frames & (frames - 1)) || frames > (1u << 30) || !stride ||
        frames > (std::numeric_limits<std::size_t>::max)() / stride) return false;
    data_ = static_cast<std::byte*>(arena.Allocate(static_cast<std::size_t>(frames) * stride));
    if (!data_) return false;
    capacity_ = frames; frame_bytes_ = stride; return true;
}
EndpointStatus PcmRingBuffer::TryAcquire(std::uint32_t frames, platform::AudioWriteSpan& span) noexcept {
    span = {};
    if (!data_ || !frames || frames > capacity_) return EndpointStatus::InvalidArgument;
    if (!checked_frames_) {
        const auto occupied = producer_ - read_.load(std::memory_order_acquire);
        const auto free = capacity_ - static_cast<std::uint32_t>(occupied);
        if (!free) return EndpointStatus::WouldBlock;
        if (next_lease_ == UINT64_MAX) return EndpointStatus::Failed;
        ++next_lease_;
        const auto contiguous = capacity_ - static_cast<std::uint32_t>(producer_ & (capacity_ - 1));
        checked_frames_ = (std::min)({frames, free, contiguous});
    }
    span = {data_ + (producer_ & (capacity_ - 1)) * frame_bytes_, checked_frames_, next_lease_};
    return EndpointStatus::Ready;
}
EndpointStatus PcmRingBuffer::Commit(std::uint64_t lease, std::uint32_t frames) noexcept {
    if (!checked_frames_ || lease != next_lease_ || frames > checked_frames_) return EndpointStatus::InvalidArgument;
    producer_ += frames; checked_frames_ = 0;
    write_.store(producer_, std::memory_order_release); return EndpointStatus::Ready;
}
std::uint32_t PcmRingBuffer::QueuedFrames() const noexcept {
    return static_cast<std::uint32_t>(write_.load(std::memory_order_acquire) - consumer_);
}
std::uint32_t PcmRingBuffer::FreeFrames() const noexcept {
    return data_ ? capacity_ - static_cast<std::uint32_t>(producer_ - read_.load(std::memory_order_acquire)) : 0;
}
std::uint32_t PcmRingBuffer::ReadInto(std::span<std::byte> output) noexcept {
    if (!data_ || output.size() % frame_bytes_) return 0;
    const auto frames = static_cast<std::uint32_t>((std::min)(output.size() / frame_bytes_,
                                                    static_cast<std::size_t>(QueuedFrames())));
    if (!frames) return 0;
    const auto index = static_cast<std::uint32_t>(consumer_ & (capacity_ - 1));
    const auto first = (std::min)(frames, capacity_ - index);
    std::memcpy(output.data(), data_ + static_cast<std::size_t>(index) * frame_bytes_,
                static_cast<std::size_t>(first) * frame_bytes_);
    if (frames > first) std::memcpy(output.data() + static_cast<std::size_t>(first) * frame_bytes_, data_,
                                   static_cast<std::size_t>(frames - first) * frame_bytes_);
    consumer_ += frames; read_.store(consumer_, std::memory_order_release); return frames;
}
std::uint32_t PcmRingBuffer::Discard() noexcept { return Skip(QueuedFrames()); }
std::uint32_t PcmRingBuffer::Skip(std::uint32_t frames) noexcept {
    frames = (std::min)(frames, QueuedFrames());
    consumer_ += frames; read_.store(consumer_, std::memory_order_release); return frames;
}
}
