#include "media/clock/PredictiveMasterClock.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace media::clock {
namespace {
double Difference(std::int64_t a, std::int64_t b) noexcept {
    return a >= b ? static_cast<double>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)) :
                  -static_cast<double>(static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a));
}
bool AddRounded(std::int64_t base, double delta, std::int64_t& result) noexcept {
    if (!std::isfinite(delta)) return false;
    delta = std::round(delta);
    if (delta >= static_cast<double>(INT64_MAX) || delta <= static_cast<double>(INT64_MIN)) return false;
    const auto ticks = static_cast<std::int64_t>(delta);
    if ((ticks > 0 && base > INT64_MAX - ticks) || (ticks < 0 && base < INT64_MIN - ticks)) return false;
    result = base + ticks;
    return result != INT64_MIN;
}
}
PredictiveMasterClock::PredictiveMasterClock(ClockOptions options) noexcept : options_(options) {
    options_valid_ = std::isfinite(options.time_constant_seconds) && options.time_constant_seconds > 0 &&
        std::isfinite(options.max_skew_ppm) && options.max_skew_ppm > 0 && options.max_skew_ppm < 100000 &&
        options.min_interval_100ns > 0 && options.max_interval_100ns >= options.min_interval_100ns &&
        options.max_innovation_100ns > 0 && options.max_extrapolation_100ns > 0 &&
        options.max_prediction_100ns > 0 && options.rejection_limit > 0;
}
bool PredictiveMasterClock::Reanchor(platform::AudioClockSample sample, std::int64_t media,
                                    std::uint32_t epoch, double speed) noexcept {
    if (!options_valid_ || !sample.precise || !sample.frequency || sample.monotonic_time_100ns < 0 || media == INT64_MIN ||
        !(speed >= 0.25 && speed <= 4.0))
        return false;
    speed_ = speed;
    origin_position_ = last_position_ = sample.position;
    frequency_ = sample.frequency; generation_ = sample.generation;
    anchor_media_ = media; last_monotonic_ = sample.monotonic_time_100ns;
    phase_ = 0; rate_ = 1; epoch_ = epoch; rejects_ = 0; anchored_ = true;
    Publish(); return true;
}
void PredictiveMasterClock::Invalidate() noexcept { anchored_ = false; Publish(); }
void PredictiveMasterClock::Publish() noexcept {
    // All accesses use the single sequentially consistent order. A reader seeing
    // the same even sequence twice cannot accept a mixture of publication epochs.
    const auto version = sequence_.load();
    sequence_.store(version + 1);
    published_[0].store(std::bit_cast<std::uint64_t>(anchor_media_));
    published_[1].store(std::bit_cast<std::uint64_t>(last_monotonic_));
    published_[2].store(std::bit_cast<std::uint64_t>(phase_));
    published_[3].store(std::bit_cast<std::uint64_t>(rate_));
    published_[4].store(epoch_);
    published_[5].store(anchored_ ? 1u : 0u);
    published_[6].store(std::bit_cast<std::uint64_t>(speed_));
    sequence_.store(version + 2);
}
ClockQuery PredictiveMasterClock::Read(Snapshot& out) const noexcept {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const auto start = sequence_.load();
        if (start & 1) continue;
        const auto media = published_[0].load(), monotonic = published_[1].load();
        const auto phase = published_[2].load(), rate = published_[3].load();
        const auto epoch = published_[4].load(), valid = published_[5].load(), speed = published_[6].load();
        if (start != sequence_.load()) continue;
        out = {std::bit_cast<std::int64_t>(media), std::bit_cast<std::int64_t>(monotonic),
            std::bit_cast<double>(phase), std::bit_cast<double>(rate), static_cast<std::uint32_t>(epoch), valid != 0, std::bit_cast<double>(speed)};
        return out.valid ? ClockQuery::Ready : ClockQuery::NotAnchored;
    }
    return ClockQuery::Busy;
}
ClockUpdate PredictiveMasterClock::Reject(bool immediate) noexcept {
    if (rejects_ < UINT32_MAX) ++rejects_;
    if (immediate || rejects_ >= options_.rejection_limit) {
        Invalidate(); return ClockUpdate::NeedsAnchor;
    }
    return ClockUpdate::Rejected;
}
ClockUpdate PredictiveMasterClock::Observe(platform::AudioClockSample sample) noexcept {
    if (!options_valid_) return ClockUpdate::InvalidArgument;
    if (!anchored_) return ClockUpdate::NeedsAnchor;
    if (sample.frequency != frequency_ || sample.generation != generation_) return Reject(true);
    if (!sample.precise) return Reject();
    if (sample.monotonic_time_100ns < last_monotonic_ || sample.position < last_position_) return Reject(true);
    const auto elapsed = sample.monotonic_time_100ns - last_monotonic_;
    if (elapsed < options_.min_interval_100ns) return ClockUpdate::TooSoon;
    if (elapsed > options_.max_interval_100ns) return Reject(true);
    if (sample.position == last_position_) return Reject();
    const double dt = static_cast<double>(elapsed);
    const double observation = static_cast<double>(sample.position - origin_position_) /
                               static_cast<double>(frequency_) * 10000000.0;
    const double prediction = phase_ + rate_ * dt;
    const double innovation = observation - prediction;
    if (!std::isfinite(innovation) || std::abs(innovation) > static_cast<double>(options_.max_innovation_100ns)) return Reject();
    const double pole = std::exp(-dt / (10000000.0 * options_.time_constant_seconds));
    const double alpha = 1.0 - pole * pole, beta = (1.0 - pole) * (1.0 - pole);
    phase_ = prediction + alpha * innovation;
    const double skew = options_.max_skew_ppm * 1e-6;
    rate_ = std::clamp(rate_ + beta * innovation / dt, 1.0 - skew, 1.0 + skew);
    last_monotonic_ = sample.monotonic_time_100ns; last_position_ = sample.position;
    rejects_ = 0; Publish(); return ClockUpdate::Accepted;
}
ClockQuery PredictiveMasterClock::Estimate(std::int64_t now, ClockEstimate& out) const noexcept {
    Snapshot sample{};
    const auto state = Read(sample);
    if (state != ClockQuery::Ready) return state;
    if (now < sample.monotonic) return ClockQuery::OutOfRange;
    const double dt = Difference(now, sample.monotonic);
    if (dt > static_cast<double>(options_.max_extrapolation_100ns)) return ClockQuery::Stale;
    std::int64_t estimate = 0;
    if (!AddRounded(sample.anchor_media, sample.speed * (sample.phase + sample.rate * dt), estimate)) return ClockQuery::OutOfRange;
    out = {estimate, sample.rate, sample.epoch, sample.speed}; return ClockQuery::Ready;
}
ClockQuery PredictiveMasterClock::Deadline(std::int64_t pts, std::int64_t now, std::int64_t& deadline,
                                         std::uint32_t& epoch) const noexcept {
    if (pts == INT64_MIN) return ClockQuery::OutOfRange;
    Snapshot sample{};
    const auto state = Read(sample);
    if (state != ClockQuery::Ready) return state;
    if (now < sample.monotonic) return ClockQuery::OutOfRange;
    if (Difference(now, sample.monotonic) > static_cast<double>(options_.max_extrapolation_100ns)) return ClockQuery::Stale;
    const double dt = (Difference(pts, sample.anchor_media) / sample.speed - sample.phase) / sample.rate;
    if (std::abs(dt - Difference(now, sample.monotonic)) > static_cast<double>(options_.max_prediction_100ns))
        return ClockQuery::OutOfRange;
    std::int64_t result = 0;
    if (!AddRounded(sample.monotonic, dt, result)) return ClockQuery::OutOfRange;
    deadline = result; epoch = sample.epoch; return ClockQuery::Ready;
}
}
