#include "media/render/QualityGovernor.hpp"

namespace media::render {
const char* Name(QualityTier tier) noexcept {
    switch (tier) {
    case QualityTier::Neural: return "Neural super-resolution";
    case QualityTier::EdgeDirected: return "Edge-directed";
    case QualityTier::Fast: return "Fast bilinear";
    }
    return "?";
}
const char* Name(QualityMode mode) noexcept {
    switch (mode) {
    case QualityMode::Auto: return "Auto";
    case QualityMode::Neural: return "Neural super-resolution";
    case QualityMode::EdgeDirected: return "Edge-directed";
    case QualityMode::Fast: return "Fast bilinear";
    }
    return "?";
}
void RollingPercentile::Add(std::int64_t value) noexcept {
    values_[next_] = value; next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
}
std::int64_t RollingPercentile::P95() const noexcept {
    if (!count_) return 0;
    std::int64_t sorted[kWindow];
    for (unsigned i = 0; i < count_; ++i) { // Insertion sort: at most 32 values.
        const auto v = values_[i]; unsigned j = i;
        for (; j > 0 && sorted[j - 1] > v; --j) sorted[j] = sorted[j - 1];
        sorted[j] = v;
    }
    const unsigned rank = (count_ * 95 + 99) / 100; // ceil(0.95 n), at least 1.
    return sorted[rank - 1];
}
void QualityGovernor::Configure(GovernorOptions options) noexcept {
    options_ = options; mode_ = QualityMode::Auto; tier_ = QualityTier::EdgeDirected; neural_available_ = false;
    window_.Clear(); over_run_ = 0; workload_ = 0; budget_ = 0; since_ = 0;
    Forget(); change_count_ = 0; last_ = {}; stats_ = {};
}
void QualityGovernor::Forget() noexcept {
    for (unsigned t = 0; t < kQualityTiers; ++t) known_[t] = known_at_[t] = retry_at_[t] = backoff_[t] = 0;
}
void QualityGovernor::Move(QualityTier to, Reason reason, std::int64_t now) noexcept {
    last_ = {now, tier_, to, reason, window_.P95(), budget_};
    if (change_count_ < kHistory) history_[change_count_] = last_;
    ++change_count_;
    if (reason == Reason::Overload) ++stats_.steps_down;
    else if (reason == Reason::Headroom) ++stats_.steps_up;
    else if (reason == Reason::Manual) ++stats_.manual;
    tier_ = to; window_.Clear(); over_run_ = 0; since_ = now;
}
void QualityGovernor::SetMode(QualityMode mode, std::int64_t now) noexcept {
    mode_ = mode;
    const auto fixed = mode == QualityMode::Neural ? QualityTier::Neural : mode == QualityMode::EdgeDirected ? QualityTier::EdgeDirected :
                       mode == QualityMode::Fast ? QualityTier::Fast : tier_;
    if (fixed != tier_) Move(fixed, Reason::Manual, now);
    else { window_.Clear(); over_run_ = 0; since_ = now; } // Auto from here: a fresh window at the current tier.
    if (mode == QualityMode::Auto && tier_ == QualityTier::Neural && !neural_available_) Move(QualityTier::EdgeDirected, Reason::Unavailable, now);
}
void QualityGovernor::SetNeuralAvailable(bool available, std::int64_t now) noexcept {
    neural_available_ = available;
    if (!available && mode_ == QualityMode::Auto && tier_ == QualityTier::Neural) Move(QualityTier::EdgeDirected, Reason::Unavailable, now);
}
bool QualityGovernor::Observe(QualityTier at, std::int64_t gpu, std::uint64_t workload, std::int64_t budget, std::int64_t now, std::int64_t queued) noexcept {
    if (budget <= 0 || gpu < 0) return false;
    const auto drift = budget > budget_ ? budget - budget_ : budget_ - budget;
    if (workload != workload_ || drift * 100 > budget_) {
        // Another picture size or frame rate: what each tier cost no longer holds.
        workload_ = workload; budget_ = budget; Forget(); window_.Clear(); over_run_ = 0; since_ = now;
    }
    const auto index = static_cast<unsigned>(at);
    ++stats_.frames[index]; if (gpu > stats_.worst) stats_.worst = gpu;
    if (at != tier_) { ++stats_.ignored; return false; } // Converted before the last change took effect.
    ++stats_.samples;
    window_.Add(gpu);
    over_run_ = gpu > budget ? over_run_ + 1 : 0;
    const auto p95 = window_.P95();
    if (window_.Count() >= options_.min_samples) { known_[index] = p95; known_at_[index] = now; }
    if (now - since_ >= options_.proven) backoff_[index] = 0; // Held long enough: a later failure starts over.
    if (mode_ != QualityMode::Auto) return false;
    const auto limit = static_cast<double>(budget);
    const bool thin = queued >= 0 && queued < options_.thin_queue && static_cast<double>(gpu) > options_.thin_step_down * limit;
    const bool overloaded = thin || over_run_ >= options_.overload_run ||
                            (window_.Count() >= options_.min_samples && static_cast<double>(p95) > options_.step_down * limit);
    if (overloaded) {
        if (tier_ == QualityTier::Fast) return false; // Nothing cheaper to go to.
        if (thin) ++stats_.thin;
        // This tier failed here: remember its cost, and do not try it again before the back-off has passed.
        known_[index] = p95 > gpu ? p95 : gpu; known_at_[index] = now;
        backoff_[index] = backoff_[index] ? (backoff_[index] * 2 < options_.max_backoff ? backoff_[index] * 2 : options_.max_backoff) : options_.backoff;
        retry_at_[index] = now + backoff_[index];
        Move(static_cast<QualityTier>(index + 1), Reason::Overload, now);
        return true;
    }
    if (tier_ == QualityTier::Neural || window_.Count() < RollingPercentile::kWindow || now - since_ < options_.settle ||
        static_cast<double>(p95) >= options_.step_up * limit) return false;
    const auto target = index - 1;
    if (target == static_cast<unsigned>(QualityTier::Neural) && !neural_available_) return false;
    if (now < retry_at_[target]) return false;
    // Measured too expensive here not long ago: not retried until that knowledge is old (or the workload changes).
    if (known_[target] && now - known_at_[target] < options_.memory && static_cast<double>(known_[target]) > options_.step_down * limit) return false;
    Move(static_cast<QualityTier>(target), Reason::Headroom, now);
    return true;
}
}
