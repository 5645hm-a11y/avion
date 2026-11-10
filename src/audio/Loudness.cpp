#include "media/audio/Loudness.hpp"
#include <algorithm>
#include <cmath>
namespace media::audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
// The true-peak interpolator: 48 taps of a Hann-windowed sinc at 4x, as 4 phases of 12, each phase summing to 1.
// Phase p of input n is the point n - 5.875 + p / 4 samples.
struct Taps {
    float h[4][12]{};
    Taps() noexcept {
        for (unsigned p = 0; p < 4; ++p) {
            double sum = 0, v[12]{};
            for (unsigned j = 0; j < 12; ++j) {
                const double m = 4.0 * j + p, t = (m - 23.5) / 4.0;
                const double sinc = t == 0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
                const double window = 0.5 - 0.5 * std::cos(2 * kPi * (m + 0.5) / 48.0);
                v[j] = sinc * window; sum += v[j];
            }
            for (unsigned j = 0; j < 12; ++j) h[p][j] = static_cast<float>(v[j] / sum);
        }
    }
};
const Taps kTaps;
float Lufs(double mean_square) noexcept {
    return mean_square > 1e-15 ? static_cast<float>(-0.691 + 10.0 * std::log10(mean_square)) : LoudnessMeter::kSilence;
}
float Db(float linear) noexcept { return linear > 1e-8f ? 20.0f * std::log10(linear) : LoudnessMeter::kSilence; }
}
float LoudnessMeter::Oversampler::Push(float sample) noexcept {
    history[write] = sample;
    float peak = 0;
    for (unsigned p = 0; p < 4; ++p) {
        float y = 0;
        for (unsigned j = 0; j < 12; ++j) y += kTaps.h[p][j] * history[(write + 12 - j) % 12]; // x[n - j]
        peak = (std::max)(peak, std::fabs(y));
    }
    peak = (std::max)(peak, std::fabs(history[(write + 12 - kTruePeakDelay) % 12])); // The sample itself, among its neighbours.
    write = (write + 1) % 12;
    return peak;
}
void LoudnessMeter::KWeighting(double rate, double shelf[5], double highpass[5]) noexcept {
    // BS.1770-4's pre-filter (a high shelf) and RLB weighting (a high-pass), by the bilinear transform of their prototypes.
    {
        const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / rate), vh = std::pow(10.0, gain / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf[0] = (vh + vb * k / q + k * k) / a0; shelf[1] = 2.0 * (k * k - vh) / a0; shelf[2] = (vh - vb * k / q + k * k) / a0;
        shelf[3] = 2.0 * (k * k - 1.0) / a0; shelf[4] = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / rate), a0 = 1.0 + k / q + k * k;
        highpass[0] = 1.0; highpass[1] = -2.0; highpass[2] = 1.0;
        highpass[3] = 2.0 * (k * k - 1.0) / a0; highpass[4] = (1.0 - k / q + k * k) / a0;
    }
}
bool LoudnessMeter::Configure(unsigned rate, unsigned channels, std::uint32_t surround, std::uint32_t lfe) noexcept {
    if (rate < 8000 || rate > 192000 || !channels || channels > kMaxChannels) return false;
    rate_ = rate; channels_ = channels; sub_frames_ = rate / 10;
    double s[5], h[5]; KWeighting(rate, s, h);
    shelf_ = {s[0], s[1], s[2], s[3], s[4]}; highpass_ = {h[0], h[1], h[2], h[3], h[4]};
    for (unsigned c = 0; c < kMaxChannels; ++c)
        weights_[c] = c >= channels ? 0.0f : (lfe >> c) & 1u ? 0.0f : (surround >> c) & 1u ? 1.41f : 1.0f;
    Reset();
    return true;
}
void LoudnessMeter::Reset() noexcept {
    for (auto& s : shelf_state_) s = {};
    for (auto& s : highpass_state_) s = {};
    for (auto& p : peaks_) p.Reset();
    sub_energy_ = 0; in_sub_ = 0; sub_count_ = sub_next_ = 0;
    for (auto& s : subs_) s = 0;
    for (auto& s : sub_peaks_) s = 0;
    for (auto& b : bin_energy_) b = 0;
    for (auto& b : bin_count_) b = 0;
    momentary_ = short_term_ = kSilence; peak_ = sub_peak_ = 0;
}
void LoudnessMeter::Feed(const float* x, unsigned frames, unsigned stride) noexcept {
    if (!channels_ || !x) return;
    for (unsigned f = 0; f < frames; ++f, x += stride) {
        double energy = 0;
        for (unsigned c = 0; c < channels_; ++c) {
            const float in = x[c];
            sub_peak_ = (std::max)(sub_peak_, peaks_[c].Push(in));
            if (weights_[c] == 0.0f) continue;
            // Transposed direct form II, in double: the 38 Hz high-pass's poles sit close to the unit circle.
            auto& a = shelf_state_[c]; const double u = in;
            const double v = shelf_.b0 * u + a.z1; a.z1 = shelf_.b1 * u - shelf_.a1 * v + a.z2; a.z2 = shelf_.b2 * u - shelf_.a2 * v;
            auto& b = highpass_state_[c];
            const double y = highpass_.b0 * v + b.z1; b.z1 = highpass_.b1 * v - highpass_.a1 * y + b.z2; b.z2 = highpass_.b2 * v - highpass_.a2 * y;
            energy += weights_[c] * y * y;
        }
        sub_energy_ += energy;
        if (++in_sub_ == sub_frames_) CloseSubBlock();
    }
}
void LoudnessMeter::CloseSubBlock() noexcept {
    subs_[sub_next_] = sub_energy_ / sub_frames_; sub_peaks_[sub_next_] = sub_peak_;
    peak_ = (std::max)(peak_, sub_peak_);
    sub_next_ = (sub_next_ + 1) % kSubBlocks; if (sub_count_ < kSubBlocks) ++sub_count_;
    sub_energy_ = 0; in_sub_ = 0; sub_peak_ = 0;
    const auto mean = [&](unsigned n) {
        n = (std::min)(n, sub_count_); double sum = 0;
        for (unsigned i = 1; i <= n; ++i) sum += subs_[(sub_next_ + kSubBlocks - i) % kSubBlocks];
        return n ? sum / n : 0.0;
    };
    const double block = mean(4);
    momentary_ = Lufs(block); short_term_ = Lufs(mean(kSubBlocks));
    // Integrated: every 400 ms block (75 % overlap) above the absolute gate, in its 0.1 LU bin.
    if (sub_count_ >= 4 && momentary_ >= -70.0f) {
        const auto bin = (std::min)(static_cast<unsigned>((momentary_ + 70.0f) * 10.0f), kBins - 1);
        bin_energy_[bin] += block; ++bin_count_[bin];
    }
}
float LoudnessMeter::Integrated() const noexcept {
    double sum = 0; std::uint64_t count = 0;
    for (unsigned b = 0; b < kBins; ++b) { sum += bin_energy_[b]; count += bin_count_[b]; }
    if (!count) return kSilence;
    const float gate = Lufs(sum / static_cast<double>(count)) - 10.0f; // Relative gate.
    const auto first = gate <= -70.0f ? 0u : (std::min)(static_cast<unsigned>((gate + 70.0f) * 10.0f), kBins - 1);
    sum = 0; count = 0;
    for (unsigned b = first; b < kBins; ++b) { sum += bin_energy_[b]; count += bin_count_[b]; }
    return count ? Lufs(sum / static_cast<double>(count)) : kSilence;
}
float LoudnessMeter::TruePeak() const noexcept { return Db((std::max)(peak_, sub_peak_)); }
float LoudnessMeter::RecentTruePeak() const noexcept {
    float peak = sub_peak_;
    for (unsigned i = 0; i < sub_count_; ++i) peak = (std::max)(peak, sub_peaks_[i]);
    return Db(peak);
}

bool NightMode::Configure(unsigned rate, unsigned channels, std::uint32_t surround, std::uint32_t lfe) noexcept {
    if (!input_.Configure(rate, channels, surround, lfe) || !output_.Configure(rate, channels, surround, lfe)) return false;
    rate_ = rate; channels_ = channels;
    const float blocks = static_cast<float>(rate) / kControl; // Control blocks a second.
    rise_ = 3.0f / blocks; fall_ = 10.0f / blocks; burst_ = 30.0f / blocks;
    mix_rate_ = 1.0f / (0.05f * blocks); // 50 ms crossfade.
    release_ = std::pow(10.0f, 8.0f / 20.0f / static_cast<float>(rate)); // 8 dB/s, per frame.
    Reset(); gain_db_ = 0; gain_ = 1; mix_ = 0;
    return true;
}
void NightMode::Reset() noexcept {
    input_.Reset(); output_.Reset();
    for (auto& d : delay_) for (auto& s : d) s = 0;
    for (auto& p : peaks_) p.Reset();
    for (auto& r : required_) r = 1;
    for (auto& h : held_) h = 1;
    queue_head_ = queue_size_ = 0; frame_ = 0; write_ = 0; phase_ = 0;
    limiter_ = 1; limit_db_ = 0; gain_step_ = 0;
}
void NightMode::Control() noexcept {
    const float m = input_.Momentary(), s = input_.ShortTerm();
    if (enabled_) {
        const bool silent = m < -50.0f || m < s - 15.0f || s <= LoudnessMeter::kSilence;
        if (!silent) {
            const float wanted = std::clamp(kTarget - s, kMaxCut, kMaxLift);
            gain_db_ = wanted > gain_db_ ? (std::min)(wanted, gain_db_ + rise_) : (std::max)(wanted, gain_db_ - fall_);
        }
        // A burst: the last 400 ms well above the target, brought down quickly (the limiter catches what this cannot).
        if (m > LoudnessMeter::kSilence && m + gain_db_ > kTarget + 4.0f) gain_db_ = (std::max)(kTarget + 4.0f - m, gain_db_ - burst_);
        gain_db_ = std::clamp(gain_db_, kMaxCut - 6.0f, kMaxLift);
    } else if (mix_ <= 0) gain_db_ = 0; // Off and faded out: the next start begins at unity.
    mix_ = std::clamp(mix_ + (enabled_ ? mix_rate_ : -mix_rate_), 0.0f, 1.0f);
    gain_step_ = (std::pow(10.0f, gain_db_ / 20.0f) - gain_) / kControl;
}
void NightMode::Process(float* x, unsigned frames, unsigned stride) noexcept {
    if (!channels_ || !x) return;
    if (!stride) stride = channels_;
    float* const first = x;
    const float ceiling = std::pow(10.0f, kCeiling / 20.0f);
    constexpr unsigned D = Latency();
    float deepest = 1;
    for (unsigned f = 0; f < frames; ++f, x += stride) {
        input_.Feed(x, 1, stride); // Frame by frame: what the control law sees does not depend on the block sizes.
        if (phase_++ % kControl == 0) Control();
        const bool live = mix_ > 0 || enabled_;
        gain_ += gain_step_;
        // The gained frame into the delay line and the true-peak interpolators: what it requires of the limiter.
        float peak = 0;
        for (unsigned c = 0; c < channels_; ++c) {
            const float gained = x[c] * gain_;
            delay_[c][write_] = gained;
            if (live) peak = (std::max)(peak, peaks_[c].Push(gained));
        }
        const unsigned q = write_; // The required gain of the frame kTruePeakDelay back is stored here: the window is aligned.
        required_[q] = peak > ceiling ? ceiling / peak : 1.0f;
        // Running minimum of the last kLookahead required gains (indices in a monotonic queue).
        while (queue_size_ && required_[queue_[(queue_head_ + queue_size_ - 1) % kDelay]] >= required_[q]) --queue_size_;
        queue_[(queue_head_ + queue_size_++) % kDelay] = q;
        if (((q + kDelay - queue_[queue_head_]) % kDelay) >= kLookahead) { queue_head_ = (queue_head_ + 1) % kDelay; --queue_size_; }
        held_[q] = required_[queue_[queue_head_]]; // The least gain any frame of the window needs.
        const unsigned out = (write_ + kDelay - D) % kDelay;
        write_ = (write_ + 1) % kDelay; ++frame_;
        if (!live) continue; // Off and settled: the samples pass untouched.
        // The average of the window's minima: every one of them is at most what the output frame needs (each window
        // holds it), so the gain is in place when the frame leaves, and it got there without a step.
        float average = 0;
        for (unsigned k = 0; k < kLookahead; ++k) average += held_[(q + kDelay - k) % kDelay];
        average /= kLookahead;
        limiter_ = (std::min)(average, limiter_ * release_);
        deepest = (std::min)(deepest, limiter_);
        for (unsigned c = 0; c < channels_; ++c) {
            const float wet = delay_[c][out] * limiter_;
            x[c] = mix_ >= 1.0f ? wet : x[c] + (wet - x[c]) * mix_;
        }
    }
    limit_db_ = 20.0f * std::log10(deepest);
    output_.Feed(first, frames, stride);
}
}
