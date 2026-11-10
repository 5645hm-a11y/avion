#pragma once
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Cache-line isolation pads the published event on purpose.
#endif
namespace media::audio {
// A localised sound (SoundLocator::Latest).
struct SoundEvent {
    std::uint32_t sequence = 0;  // Events since Configure (0: none yet).
    float azimuth = 0;           // -1 (the left channel's side) .. +1 (the right's): the sine of the direction for a spaced pair.
    float level_db = -144;       // The onset's level, dB relative to a full-scale sine.
    float ild_db = 0;            // Right over left, dB.
    float itd_us = 0;            // The left channel's lead over the right, microseconds (positive: the sound reached the left first).
    float correlation = 0;       // The channels' normalised cross-correlation at that lag.
    std::int64_t pts_100ns = -1; // The onset's time (the caller's timestamps), -1 when the caller gave none.
};
// Where a sudden sound came from (docs/audio-dsp.md, "Sound localisation and the audio saccade"), from a pair of channels
// (0 left, 1 right): 10 ms blocks; an onset is a block kOnsetDb above the noise floor (the quietest recent level, rising
// 5 dB/s), kAttackDb above the loudest of the kAttackBlocks before it (a sudden sound, not a swell of surf, wind or traffic:
// 2026-10-22, real ambience fired the detector every refractory period without it) and above kMinLevelDb, at least
// kRefractory after the previous one. Its 20 ms from the onset are analysed:
// - the level difference (ILD) read through the constant-power pan law, azimuth = 4/pi atan(10^(ILD/20)) - 1, exact for a
//   source panned in a stereo programme and for a one-sided sound;
// - the time difference (ITD): the lag of the cross-correlation's peak within the pair's spacing (refined by a parabola),
//   azimuth = -ITD c / spacing, for a spaced microphone pair hearing a source at about the same level.
// The ITD is used when the channels correlate (kMinCorrelation: 0.7 until 2026-10-22, when a room's reverberation brought a
// spaced pair's onsets down to 0.56-0.63 with the time difference still right) and differ by less than kItdMaxIld, or by more
// when the lag is not zero and says the same side, further out (reverberation tilts a spaced pair's levels by 3 dB and more);
// otherwise the ILD.
// Fixed storage (about 33 KB), no allocation. Process runs on one thread; Latest may be read from one other (a seqlock).
class SoundLocator final {
public:
    static constexpr unsigned kMaxRate = 192000, kBlockMs = 10;
    static constexpr float kOnsetDb = 12, kAttackDb = 6, kMinLevelDb = -60, kMinCorrelation = 0.5f, kItdMaxIld = 3, kFloorRiseDbPerBlock = 0.05f;
    static constexpr unsigned kAttackBlocks = 4;
    static constexpr std::int64_t kRefractory = 2500000; // 250 ms.
    // `channels` interleaved at `rate`; `spacing_m` between the pair (0: 0.2 m). False for fewer than 2 channels.
    bool Configure(unsigned rate, unsigned channels, float spacing_m) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return rate_ != 0; }
    void Reset() noexcept;
    // `frames` interleaved frames; `pts_100ns` the first one's time (-1: unknown; events then carry -1).
    void Process(const float* frames, std::uint32_t count, std::int64_t pts_100ns) noexcept;
    // The newest event; false when there is none yet or it is being written (try again).
    bool Latest(SoundEvent& out) const noexcept;
    [[nodiscard]] std::uint32_t Events() const noexcept { return published_.load(std::memory_order_acquire) / 2; }
private:
    static constexpr unsigned kWindow = kMaxRate * 2 * kBlockMs / 1000; // Two blocks at the highest rate.
    void Analyse() noexcept;
    unsigned rate_ = 0, channels_ = 0, block_ = 0, max_lag_ = 0;
    float spacing_ = 0.2f;
    // The current block.
    double energy_ = 0; unsigned filled_ = 0; std::int64_t block_pts_ = -1;
    float floor_db_ = 0; bool floor_set_ = false;
    float recent_db_[kAttackBlocks]{}; unsigned recent_ = 0; // The blocks before this one (a ring), for the attack.
    // An onset's window.
    float left_[kWindow]{}, right_[kWindow]{};
    unsigned captured_ = 0; bool capturing_ = false; std::int64_t onset_pts_ = -1, last_onset_ = INT64_MIN; double onset_db_ = -144;
    std::uint64_t frames_seen_ = 0;
    std::uint32_t sequence_ = 0;
    // The published event: sequence*2 (odd while it is written), then its fields.
    alignas(64) std::atomic<std::uint32_t> published_{0};
    std::atomic<float> azimuth_{0}, level_{-144}, ild_{0}, itd_{0}, correlation_{0};
    std::atomic<std::int64_t> pts_{-1};
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
