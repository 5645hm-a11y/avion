#include "media/audio/SoundLocator.hpp"
#include <algorithm>
#include <cmath>

namespace media::audio {
namespace {
constexpr double kSpeedOfSound = 343.0; // m/s, air at 20 C.
constexpr double kPi = 3.14159265358979323846;
double Db(double mean_square) noexcept { return mean_square > 1e-20 ? 10.0 * std::log10(mean_square / 0.5) : -144.0; } // Re a full-scale sine.
}
bool SoundLocator::Configure(unsigned rate, unsigned channels, float spacing) noexcept {
    rate_ = 0;
    if (channels < 2 || rate < 8000 || rate > kMaxRate) return false;
    rate_ = rate; channels_ = channels; block_ = rate * kBlockMs / 1000;
    spacing_ = spacing > 0.005f && spacing < 10.0f ? spacing : 0.2f;
    max_lag_ = (std::min)(64u, static_cast<unsigned>(std::ceil(spacing_ / kSpeedOfSound * rate)) + 2u);
    sequence_ = 0; published_.store(0, std::memory_order_relaxed);
    Reset();
    return true;
}
void SoundLocator::Reset() noexcept {
    energy_ = 0; filled_ = 0; block_pts_ = -1; floor_set_ = false; floor_db_ = 0; recent_ = 0;
    captured_ = 0; capturing_ = false; onset_pts_ = -1; last_onset_ = INT64_MIN; frames_seen_ = 0;
}
void SoundLocator::Process(const float* frames, std::uint32_t count, std::int64_t pts) noexcept {
    if (!rate_ || !frames) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        float l = frames[static_cast<std::size_t>(i) * channels_], r = frames[static_cast<std::size_t>(i) * channels_ + 1];
        if (!std::isfinite(l)) l = 0;
        if (!std::isfinite(r)) r = 0;
        if (filled_ == 0) block_pts_ = pts >= 0 ? pts + static_cast<std::int64_t>(static_cast<double>(i) * 1e7 / rate_) : -1;
        // The block goes where the window needs it: the start while listening, after the onset's block while capturing.
        const unsigned at = (capturing_ ? captured_ : 0) + filled_;
        if (at < kWindow) { left_[at] = l; right_[at] = r; }
        energy_ += static_cast<double>(l) * l + static_cast<double>(r) * r;
        if (++filled_ < block_) continue;
        const double db = Db(energy_ / (2.0 * block_));
        const std::int64_t when = block_pts_ >= 0 ? block_pts_ : static_cast<std::int64_t>(static_cast<double>(frames_seen_) * 1e7 / rate_);
        frames_seen_ += block_; energy_ = 0; filled_ = 0;
        if (capturing_) {
            captured_ += block_;
            if (captured_ >= 2 * block_) { Analyse(); capturing_ = false; captured_ = 0; }
            continue;
        }
        float before = -144; // The loudest of the blocks before (silence before the first ones).
        for (unsigned k = 0; k < kAttackBlocks && k < recent_; ++k) before = (std::max)(before, recent_db_[k]);
        recent_db_[recent_ % kAttackBlocks] = static_cast<float>(db); ++recent_;
        if (!floor_set_) { floor_db_ = static_cast<float>(db); floor_set_ = true; continue; }
        if (db >= floor_db_ + kOnsetDb && db >= before + kAttackDb && db >= kMinLevelDb && (last_onset_ == INT64_MIN || when - last_onset_ >= kRefractory)) {
            capturing_ = true; captured_ = block_; onset_pts_ = block_pts_; onset_db_ = db; last_onset_ = when;
            continue; // The floor stays the floor before the sound.
        }
        floor_db_ = db < floor_db_ ? static_cast<float>(db) : floor_db_ + kFloorRiseDbPerBlock;
    }
}
void SoundLocator::Analyse() noexcept {
    const unsigned n = (std::min)(captured_, kWindow);
    double el = 0, er = 0;
    for (unsigned k = 0; k < n; ++k) { el += static_cast<double>(left_[k]) * left_[k]; er += static_cast<double>(right_[k]) * right_[k]; }
    const double ild = std::clamp(10.0 * std::log10((er + 1e-12) / (el + 1e-12)), -60.0, 60.0);
    const double from_ild = 4.0 / kPi * std::atan(std::pow(10.0, ild / 20.0)) - 1.0;
    // The cross-correlation's peak within the pair's spacing: R[k + lag] ~ L[k] means the right heard it `lag` later.
    const int lags = static_cast<int>(max_lag_);
    double scores[129]; int best = 0; double top = -2;
    const double norm = std::sqrt(el * er);
    for (int lag = -lags; lag <= lags; ++lag) {
        double sum = 0;
        for (int k = (std::max)(0, -lag); k < static_cast<int>(n) && k + lag < static_cast<int>(n); ++k) sum += static_cast<double>(left_[k]) * right_[k + lag];
        const double s = norm > 1e-20 ? sum / norm : 0.0;
        scores[lag + lags] = s;
        if (s > top) { top = s; best = lag; }
    }
    double lag = best;
    if (best > -lags && best < lags) {
        const double m = scores[best + lags - 1], c = scores[best + lags], p = scores[best + lags + 1], d = m - 2 * c + p;
        if (d < -1e-12) lag += std::clamp(0.5 * (m - p) / d, -0.5, 0.5);
    }
    const double itd = lag / rate_; // Seconds the left channel leads.
    const double from_itd = std::clamp(-itd * kSpeedOfSound / spacing_, -1.0, 1.0);
    // A spaced pair's direction is in the time difference: used while the levels nearly match, and beyond that when the lag is
    // real (a panned programme has none) and agrees with the levels' side (a room's reverberation tilts the levels by 3 dB and more).
    const bool timed = top >= kMinCorrelation && (std::fabs(ild) < kItdMaxIld || (best != 0 && (from_itd > 0) == (from_ild > 0) &&
                                                                                    std::fabs(from_itd) > std::fabs(from_ild)));
    const double azimuth = std::clamp(timed ? from_itd : from_ild, -1.0, 1.0);
    // Published under a seqlock: odd while the fields change.
    const std::uint32_t s = ++sequence_;
    published_.store(2 * s - 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    azimuth_.store(static_cast<float>(azimuth), std::memory_order_relaxed); level_.store(static_cast<float>(onset_db_), std::memory_order_relaxed);
    ild_.store(static_cast<float>(ild), std::memory_order_relaxed); itd_.store(static_cast<float>(itd * 1e6), std::memory_order_relaxed);
    correlation_.store(static_cast<float>(top), std::memory_order_relaxed); pts_.store(onset_pts_, std::memory_order_relaxed);
    published_.store(2 * s, std::memory_order_release);
}
bool SoundLocator::Latest(SoundEvent& out) const noexcept {
    const auto before = published_.load(std::memory_order_acquire);
    if (!before || (before & 1u)) return false;
    SoundEvent e;
    e.sequence = before / 2; e.azimuth = azimuth_.load(std::memory_order_relaxed); e.level_db = level_.load(std::memory_order_relaxed);
    e.ild_db = ild_.load(std::memory_order_relaxed); e.itd_us = itd_.load(std::memory_order_relaxed); e.correlation = correlation_.load(std::memory_order_relaxed);
    e.pts_100ns = pts_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (published_.load(std::memory_order_relaxed) != before) return false;
    out = e;
    return true;
}
}
