#pragma once
#include <cstdint>

namespace media::render {
// Picture reconstruction tiers, most expensive first (docs/picture.md, "Quality governor"):
// - Neural: neural 2x luma super-resolution while upscaling, guided chroma, the user's sharpening;
// - EdgeDirected: edge-directed luma while upscaling, guided chroma, the user's sharpening (the long-standing default);
// - Fast: bilinear luma and chroma, no sharpening.
// Guided chroma and sharpening follow the user's settings (C key, PlaybackOptions::sharpness) in the first two tiers.
enum class QualityTier : std::uint8_t { Neural, EdgeDirected, Fast };
inline constexpr unsigned kQualityTiers = 3;
// Auto: the governor chooses the tier from measured GPU time. The others fix it (the N key cycles them).
enum class QualityMode : std::uint8_t { Auto, Neural, EdgeDirected, Fast };
const char* Name(QualityTier) noexcept;
const char* Name(QualityMode) noexcept;

// The 95th percentile (nearest rank) of the last kWindow values. Fixed storage; Add and P95 do not allocate.
class RollingPercentile {
public:
    static constexpr unsigned kWindow = 32;
    void Clear() noexcept { count_ = next_ = 0; }
    void Add(std::int64_t value) noexcept;
    unsigned Count() const noexcept { return count_; }
    // Of the values held: the smallest v such that at least 95 % of them are <= v (0 when empty).
    std::int64_t P95() const noexcept;
    std::int64_t Last() const noexcept { return count_ ? values_[(next_ + kWindow - 1) % kWindow] : 0; }
private:
    std::int64_t values_[kWindow]{};
    unsigned count_ = 0, next_ = 0;
};

struct GovernorOptions {
    double step_down = 0.85;          // p95 above this fraction of the frame budget: one tier down.
    double step_up = 0.55;            // p95 below this fraction (over a full window): one tier up may be tried.
    // Pictures measured at a tier before its p95 can step it down: from 20 on, the nearest-rank p95 leaves out the
    // slowest picture (below 20 it is the maximum), so one slow picture after a change (shader warm-up) moves nothing.
    unsigned min_samples = 20;
    unsigned overload_run = 4;        // Consecutive pictures over the whole budget: one tier down at once.
    std::int64_t settle = 10000000;   // 1 s at a tier, and a full window, before trying the next tier up.
    std::int64_t backoff = 20000000;  // After a tier was left for overload, it is not tried again for 2 s,
    std::int64_t max_backoff = 640000000; // doubling at each further failure up to 64 s,
    std::int64_t proven = 100000000;  // and back to 2 s once a tier has held for 10 s.
    std::int64_t memory = 300000000;  // A tier measured too expensive is not retried for 30 s (same workload).
    // A thin queue (2026-11-03; docs/fixtures.md, "Cold starts"): while the pictures converted ahead of presentation hold less
    // than thin_queue of media (a start, a seek's landing), one picture over thin_step_down of the frame budget steps down at
    // once (as an overload: its cost remembered, the back-off). The rules above assume a full queue that absorbs a slow
    // picture; at a start there is none, and a heavy tier also slows the decoder that fills it (the GPU's memory bandwidth is
    // shared): a 4K60 HDR start on the UHD 630 built its queue in 0.73 s at Edge-directed and 0.33 s at Fast.
    std::int64_t thin_queue = 1000000;   // 100 ms.
    double thin_step_down = 0.5;
};

// Chooses the reconstruction tier so that the presentation queue's GPU work per picture (conversion and neural pass,
// measured with timestamp queries) stays inside the frame interval, with hysteresis: down quickly when over budget, up
// only with clear headroom, never straight back into a tier that just failed (exponential back-off), and never into a
// tier already measured too expensive for this workload. Single owner (the presentation thread); no allocation,
// no locks, no waits. Times are 100 ns on one monotonic clock.
class QualityGovernor {
public:
    // Why the tier changed.
    enum class Reason : std::uint8_t { Overload, Headroom, Manual, Unavailable };
    struct Change { std::int64_t at = 0; QualityTier from = QualityTier::EdgeDirected, to = QualityTier::EdgeDirected; Reason reason = Reason::Manual; std::int64_t p95 = 0, budget = 0; };
    static constexpr unsigned kHistory = 16; // Changes kept (the first ones).
    struct Statistics {
        unsigned samples = 0, ignored = 0;   // Measured pictures used / measured at another tier than the current one.
        unsigned steps_down = 0, steps_up = 0, manual = 0;
        unsigned thin = 0;                   // Of the steps down, those a thin queue made (2026-11-03).
        unsigned frames[kQualityTiers]{};    // Measured pictures per tier.
        std::int64_t worst = 0;              // Longest picture measured (100 ns).
    };
    void Configure(GovernorOptions options = {}) noexcept;
    // Manual modes fix the tier (Neural falls back to EdgeDirected while neural cannot run); Auto starts from the
    // current tier, or EdgeDirected the first time. `now` stamps the change.
    void SetMode(QualityMode, std::int64_t now) noexcept;
    QualityMode Mode() const noexcept { return mode_; }
    QualityTier Tier() const noexcept { return tier_; } // For the next conversion.
    // Whether the Neural tier can run now (the picture is upscaled and the neural pass exists). In Auto, a Neural tier
    // that cannot run becomes EdgeDirected (Reason::Unavailable, no cue); it is offered again once it can.
    void SetNeuralAvailable(bool available, std::int64_t now) noexcept;
    // One picture's measured GPU time `gpu` at tier `at` (the tier it was converted with), for a workload `workload`
    // (any value that changes when the cost model does: sizes) and a frame interval `budget`. A different workload or
    // budget forgets what was learnt. True when the tier changed (show a cue: LastChange()). `queued`: the media converted
    // ahead of presentation when it was measured (100 ns; -1: unknown, the thin-queue rule off).
    bool Observe(QualityTier at, std::int64_t gpu, std::uint64_t workload, std::int64_t budget, std::int64_t now, std::int64_t queued = -1) noexcept;
    std::int64_t P95() const noexcept { return window_.P95(); } // Current tier's window.
    std::int64_t Budget() const noexcept { return budget_; }
    const RollingPercentile& Window() const noexcept { return window_; }
    const Statistics& Stats() const noexcept { return stats_; }
    unsigned Changes() const noexcept { return change_count_; } // All changes, also beyond kHistory.
    const Change& ChangeAt(unsigned i) const noexcept { return history_[i < kHistory ? i : kHistory - 1]; }
    const Change& LastChange() const noexcept { return last_; }
    // What a tier cost when last measured here (p95, 100 ns; 0: unknown).
    std::int64_t Known(QualityTier tier) const noexcept { return known_[static_cast<unsigned>(tier)]; }
private:
    void Move(QualityTier to, Reason, std::int64_t now) noexcept;
    void Forget() noexcept;
    GovernorOptions options_{};
    QualityMode mode_ = QualityMode::Auto;
    QualityTier tier_ = QualityTier::EdgeDirected;
    bool neural_available_ = false;
    RollingPercentile window_;
    unsigned over_run_ = 0;
    std::uint64_t workload_ = 0;
    std::int64_t budget_ = 0, since_ = 0;                  // since_: when the current tier was entered.
    std::int64_t known_[kQualityTiers]{}, known_at_[kQualityTiers]{}; // Last p95 at each tier, and when.
    std::int64_t retry_at_[kQualityTiers]{}, backoff_[kQualityTiers]{};
    Change history_[kHistory]{}, last_{};
    unsigned change_count_ = 0;
    Statistics stats_{};
};
}
