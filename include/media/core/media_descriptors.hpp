#pragma once

#include "media/core/platform.hpp"

#include <limits>
#include <type_traits>

namespace media::core {
inline constexpr std::int64_t kUnknownTimestamp = (std::numeric_limits<std::int64_t>::min)();
inline constexpr std::uint32_t kInvalidResourceId = (std::numeric_limits<std::uint32_t>::max)();

enum class PacketFlags : std::uint32_t {
    None = 0, KeyFrame = 1u << 0, Discontinuity = 1u << 1,
    EndOfStream = 1u << 2, Corrupt = 1u << 3
};

// Timestamps/durations are signed 100 ns media-time units, never QPC ticks.
// payload_offset is relative to the stable CPU block identified by payload_block_id.
// The block stays leased until the final CPU/GPU consumer explicitly returns it.
struct alignas(kCacheLineBytes) MediaPacket {
    std::int64_t pts_100ns;
    std::int64_t dts_100ns;
    std::int64_t duration_100ns;
    std::uint64_t payload_offset;
    std::uint32_t payload_bytes;
    std::uint32_t payload_block_id;
    std::uint32_t payload_generation;
    std::uint32_t stream_id;
    std::uint32_t playback_epoch;
    std::uint32_t codec_config_id;
    PacketFlags flags;
    std::uint32_t reserved; // Must be zero.
};

enum class VideoSurfaceFormat : std::uint16_t {
    Unknown = 0, Nv12 = 1, P010 = 2, Rgba16Float = 3, Rgb10A2 = 4, Bgra8 = 5
};
enum class VideoFrameFlags : std::uint16_t {
    None = 0, Interlaced = 1u << 0, TopFieldFirst = 1u << 1,
    Discontinuity = 1u << 2, Synthetic = 1u << 3,
    NoDisplay = 1u << 4 // Decoded for reference only (pic_output_flag 0, before the edit start): retire, never present.
};

// surface_id indexes an owner-held resource table; no COM AddRef/Release per frame.
// ready_fence_id + ready_fence_value identify the GPU timeline dependency.
// subresource is the base (luma for planar video) D3D12 subresource; plane SRVs and
// other subresource indices come from the immutable resource table.
struct alignas(kCacheLineBytes) VideoFrameDescriptor {
    std::int64_t pts_100ns;
    std::int64_t duration_100ns;
    std::uint64_t ready_fence_value;
    std::uint32_t surface_id;
    std::uint32_t surface_generation;
    std::uint32_t playback_epoch;
    std::uint32_t ready_fence_id;
    std::uint32_t subresource;
    std::uint32_t color_metadata_id;
    std::uint16_t coded_width;
    std::uint16_t coded_height;
    std::uint16_t crop_left;
    std::uint16_t crop_top;
    std::uint16_t crop_right;  // Exclusive.
    std::uint16_t crop_bottom; // Exclusive.
    VideoSurfaceFormat format;
    VideoFrameFlags flags;
};

static_assert(sizeof(MediaPacket) == 64 && alignof(MediaPacket) == 64);
static_assert(sizeof(VideoFrameDescriptor) == 64 && alignof(VideoFrameDescriptor) == 64);
static_assert(std::is_trivial_v<MediaPacket> && std::is_standard_layout_v<MediaPacket>);
static_assert(std::is_trivial_v<VideoFrameDescriptor> &&
              std::is_standard_layout_v<VideoFrameDescriptor>);
static_assert(offsetof(MediaPacket, payload_offset) == 24);
static_assert(offsetof(VideoFrameDescriptor, surface_id) == 24);
static_assert(offsetof(VideoFrameDescriptor, coded_width) == 48);
} // namespace media::core
