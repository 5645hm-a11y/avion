#pragma once
#include <cstdint>

namespace media::engine {
// The cross-modal frame signature (docs/sdk.md, "Frame signatures"): the C SDK's media_frame_signature_t, field for field.
// What the conversion's statistics pass measured of a picture on screen and what the DSP's analysis heard under it.
// Plain (an SpscRingBuffer element): no default member initialisers.
struct FrameSignature {
    std::int64_t pts_100ns, presented_100ns;
    std::uint32_t sequence, epoch;
    float luminance_mean, luminance_peak, tile_motion, speech_probability, dominant_band_hz, dominant_band_db;
    std::uint32_t flags, dominant_band;
    std::int64_t scene_cut_pts_100ns; // kFrameSceneCut: the cut's picture (confirmed kDepth pictures later); else -1.
};
static_assert(sizeof(FrameSignature) == 64);
enum : std::uint32_t { kFrameSceneCut = 1, kFrameMeasured = 2 };
// Called on the UI thread (in host mode, the thread calling HostEngine::Step) for every picture put on screen, right after
// the presentation pass that showed it (so the host may give commands from it); it should return quickly all the same: the
// next picture waits for it.
using FrameCallback = void (*)(void* context, const FrameSignature&) noexcept;
}
