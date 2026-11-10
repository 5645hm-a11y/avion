#pragma once
#include "media/platform/IAudioSink.hpp"
#include <atomic>
#include <cstdint>

namespace media::clock {
struct ClockOptions {
    double time_constant_seconds = 0.5;
    double max_skew_ppm = 1000.0;
    std::int64_t min_interval_100ns = 20000;
    std::int64_t max_interval_100ns = 500000;
    std::int64_t max_innovation_100ns = 200000;
    std::int64_t max_extrapolation_100ns = 2500000;
    std::int64_t max_prediction_100ns = 20000000;
    std::uint32_t rejection_limit = 3;
};
enum class ClockUpdate : std::uint8_t { Accepted, TooSoon, Rejected, NeedsAnchor, InvalidArgument };
enum class ClockQuery : std::uint8_t { Ready, NotAnchored, Busy, Stale, OutOfRange };
struct ClockEstimate {
    std::int64_t media_time_100ns = 0;
    double rate = 1.0;  // The device's rate against the monotonic clock (the PLL's).
    std::uint32_t epoch = 0;
    double speed = 1.0; // Playback speed: media time per device time.
};

// One writer (audio owner), any number of readers. Every query has four bounded
// snapshot attempts, never a mutex or unbounded seqlock spin. All payload words
// are atomic too: this is not a racy seqlock over ordinary C++ objects.
class PredictiveMasterClock final {
public:
    explicit PredictiveMasterClock(ClockOptions options = {}) noexcept;
    PredictiveMasterClock(const PredictiveMasterClock&) = delete;
    PredictiveMasterClock& operator=(const PredictiveMasterClock&) = delete;
    // `speed` (docs/audio-dsp.md, "Playback speed"): media time per device time from this anchor on, 0.25 to 4. The
    // device's position is tracked as always; the media time it stands for advances `speed` times as fast.
    [[nodiscard]] bool Reanchor(platform::AudioClockSample sample, std::int64_t media_time_100ns,
                                 std::uint32_t epoch, double speed = 1.0) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] ClockUpdate Observe(platform::AudioClockSample sample) noexcept;
    [[nodiscard]] ClockQuery Estimate(std::int64_t monotonic_now_100ns, ClockEstimate&) const noexcept;
    [[nodiscard]] ClockQuery Deadline(std::int64_t media_pts_100ns, std::int64_t monotonic_now_100ns,
                                      std::int64_t& monotonic_deadline_100ns,
                                      std::uint32_t& epoch) const noexcept;
private:
    struct Snapshot {
        std::int64_t anchor_media, monotonic;
        double phase, rate;
        std::uint32_t epoch;
        bool valid;
        double speed;
    };
    void Publish() noexcept;
    [[nodiscard]] ClockQuery Read(Snapshot&) const noexcept;
    [[nodiscard]] ClockUpdate Reject(bool invalidate_now = false) noexcept;
    ClockOptions options_;
    bool options_valid_ = false, anchored_ = false;
    std::uint64_t origin_position_ = 0, last_position_ = 0, frequency_ = 0;
    std::int64_t anchor_media_ = 0, last_monotonic_ = 0;
    double phase_ = 0.0, rate_ = 1.0, speed_ = 1.0;
    std::uint32_t epoch_ = 0, generation_ = 0, rejects_ = 0;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    // Atomic bit representations avoid assumptions about atomic<double> lock freedom.
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> published_[7]{};
};
}
