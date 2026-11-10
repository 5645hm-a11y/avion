#include "media/audio/DspProcessor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

#if !defined(MEDIA_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#include <emmintrin.h>
#define MEDIA_DSP_SSE 1
#elif !defined(MEDIA_DSP_FORCE_SCALAR) && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define MEDIA_DSP_NEON 1
#endif

namespace media::audio {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kHead = 0.0875f, kSound = 343.0f;   // Head radius (m), speed of sound (m/s).
constexpr float kAntiDenormal = 1e-20f;             // Keeps recursive filter states out of the denormal range.
// Four-lane vector: SSE on x64, NEON on ARM, scalar otherwise. Multiply and add stay separate (no fused
// multiply-add) so every build computes the same rounding.
#if defined(MEDIA_DSP_SSE)
using Vec = __m128;
inline Vec Load(const float* p) { return _mm_load_ps(p); }
inline void Store(float* p, Vec v) { _mm_store_ps(p, v); }
inline Vec Add(Vec a, Vec b) { return _mm_add_ps(a, b); }
inline Vec Sub(Vec a, Vec b) { return _mm_sub_ps(a, b); }
inline Vec Mul(Vec a, Vec b) { return _mm_mul_ps(a, b); }
inline Vec Splat(float v) { return _mm_set1_ps(v); }
#elif defined(MEDIA_DSP_NEON)
using Vec = float32x4_t;
inline Vec Load(const float* p) { return vld1q_f32(p); }
inline void Store(float* p, Vec v) { vst1q_f32(p, v); }
inline Vec Add(Vec a, Vec b) { return vaddq_f32(a, b); }
inline Vec Sub(Vec a, Vec b) { return vsubq_f32(a, b); }
inline Vec Mul(Vec a, Vec b) { return vmulq_f32(a, b); }
inline Vec Splat(float v) { return vdupq_n_f32(v); }
#else
struct Vec { float v[4]; };
inline Vec Load(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline void Store(float* p, Vec a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline Vec Add(Vec a, Vec b) { for (int i = 0; i < 4; ++i) a.v[i] += b.v[i]; return a; }
inline Vec Sub(Vec a, Vec b) { for (int i = 0; i < 4; ++i) a.v[i] -= b.v[i]; return a; }
inline Vec Mul(Vec a, Vec b) { for (int i = 0; i < 4; ++i) a.v[i] *= b.v[i]; return a; }
inline Vec Splat(float v) { return {{v, v, v, v}}; }
#endif
float Smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float Decibels(float energy) { return 10.0f * std::log10(energy + 1e-20f); }
float Rate(float seconds, float rate) { return 1.0f - std::exp(-1.0f / (seconds * rate)); } // One-pole coefficient.
// Audio EQ cookbook biquads (R. Bristow-Johnson), normalized by a0.
struct Coefficients { float b0, b1, b2, a1, a2; };
Coefficients Butterworth(float frequency, float rate, bool high) {
    const float w = 2 * kPi * frequency / rate, c = std::cos(w), alpha = std::sin(w) / (2 * 0.70710678f), a0 = 1 + alpha;
    const float b = high ? (1 + c) / 2 : (1 - c) / 2;
    return {b / a0, (high ? -(1 + c) : (1 - c)) / a0, b / a0, -2 * c / a0, (1 - alpha) / a0};
}
Coefficients BandPass(float frequency, float q, float rate) { // Constant 0 dB peak: phase stays within +-90 degrees.
    const float w = 2 * kPi * frequency / rate, alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    return {alpha / a0, 0.0f, -alpha / a0, -2 * std::cos(w) / a0, (1 - alpha) / a0};
}
// Speaker positions (azimuth in degrees, negative = left), gains and LFE flags per layout, in channel order.
struct Speaker { float azimuth, gain; bool lfe; };
constexpr Speaker kMono[]{{0, 1, false}};
constexpr Speaker kStereo[]{{-30, 1, false}, {30, 1, false}};
constexpr Speaker k51[]{{-30, 1, false}, {30, 1, false}, {0, 0.70710678f, false}, {0, 0.5f, true}, {-110, 0.70710678f, false}, {110, 0.70710678f, false}};
constexpr Speaker k71[]{{-30, 1, false}, {30, 1, false}, {0, 0.70710678f, false}, {0, 0.5f, true}, {-150, 0.70710678f, false},
                       {150, 0.70710678f, false}, {-90, 0.70710678f, false}, {90, 0.70710678f, false}};
const Speaker* Layout(SpeakerLayout layout) {
    switch (layout) {
    case SpeakerLayout::Mono: return kMono;
    case SpeakerLayout::Stereo: return kStereo;
    case SpeakerLayout::Surround51: return k51;
    default: return k71;
    }
}
// Dialogue clarity per level: largest band boost (dB) and compression ratio.
constexpr float kMaxBoost[4]{0, 4, 7, 10}, kRatio[4]{1, 1.5f, 2.5f, 4};
constexpr float kSpeechTarget = -20, kThreshold = -16, kKnee = 8; // dBFS, sine-peak equivalent.
}

float DspProcessor::Biquad::Run(float x) noexcept {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

void DspProcessor::EarModel(float azimuth, float& delay_left, float& delay_right, float& alpha_left, float& alpha_right) noexcept {
    // Brown and Duda's spherical head: ears at -90 and +90 degrees; incidence is the angle between the source and
    // the ear axis. Delay: -(a/c) cos(incidence) while the ear sees the source, (a/c)(incidence - 90 deg) around the
    // head; the interaural difference is Woodworth's ITD. Head shadow: a one-pole/one-zero filter with unit gain at
    // DC and high-frequency gain alpha(incidence) = 1.05 + 0.95 cos(pi * incidence / 150 deg).
    auto incidence = [](float azimuth_deg, float ear) {
        float d = std::fabs(azimuth_deg - ear);
        while (d > 360) d -= 360;
        return (d > 180 ? 360 - d : d) * kPi / 180;
    };
    auto delay = [](float theta) { return theta < kPi / 2 ? -kHead / kSound * std::cos(theta) : kHead / kSound * (theta - kPi / 2); };
    auto alpha = [](float theta) { return 1.05f + 0.95f * std::cos(theta / (150 * kPi / 180) * kPi); };
    const float left = incidence(azimuth, -90), right = incidence(azimuth, 90);
    delay_left = delay(left); delay_right = delay(right); alpha_left = alpha(left); alpha_right = alpha(right);
}

void DspProcessor::DownmixWeights(unsigned channels, unsigned channel, float& left, float& right) noexcept {
    left = right = 0;
    SpeakerLayout layout{};
    if (!LayoutFor(channels, layout) || channel >= channels) return;
    // ITU-R BS.775 shape (fronts 1, centre and surrounds 1/sqrt2, LFE 0) at the binaural renderer's per-ear scale 0.5.
    const auto& s = Layout(layout)[channel];
    if (s.lfe) return;
    const float w = 0.5f * s.gain;
    if (s.azimuth <= 0) left = w;
    if (s.azimuth >= 0) right = w;
}

bool DspProcessor::Configure(unsigned sample_rate, unsigned channels, bool fold_down) noexcept {
    SpeakerLayout layout{};
    if (sample_rate < 8000 || sample_rate > 192000 || !LayoutFor(channels, layout) || (fold_down && channels < 6)) return false;
    rate_ = sample_rate; channels_ = channels; layout_ = layout; fold_ = fold_down;
    for (unsigned c = 0; c < kMaxChannels; ++c) DownmixWeights(channels, c, fold_left_[c], fold_right_[c]);
    spatial_ = spatial_ && channels >= 2;
    stats_ = {};
    carrier_ = layout == SpeakerLayout::Mono ? 2u : layout == SpeakerLayout::Stereo ? 0u : 1u;
    const float rate = static_cast<float>(sample_rate);
    const auto high = Butterworth(300, rate, true), low = Butterworth(3400, rate, false);
    high_ = {high.b0, high.b1, high.b2, high.a1, high.a2};
    low_ = {low.b0, low.b1, low.b2, low.a1, low.a2};
    const auto band = BandPass(1500, 0.6f, rate);
    presence_filter_ = {band.b0, band.b1, band.b2, band.a1, band.a2};
    energy_rate_ = Rate(0.020f, rate);     // Speech-band and carrier energies: 20 ms.
    detector_rate_ = Rate(0.005f, rate);   // Compressor level detector: 5 ms.
    mix_rate_ = 1.0f / (0.050f * rate);    // Clarity on/off ramp: 50 ms.
    spatial_rate_ = 1.0f / (0.030f * rate); // Spatial on/off ramp: 30 ms.
    ConfigureSpatial();
    // Night mode works on what leaves the stage: the stereo pair of a fold-down, else every channel (LFE unweighted in its
    // meters; the surrounds +1.5 dB, BS.1770).
    const unsigned out = fold_down ? 2u : channels;
    const std::uint32_t lfe = !fold_down && channels >= 6 ? 1u << 3 : 0u;
    const std::uint32_t surround = fold_down ? 0u : channels == 6 ? 0x30u : channels == 8 ? 0xf0u : 0u;
    if (!night_.Configure(sample_rate, out, surround, lfe)) return false;
    if ((tracker_ && !tracker_->Configure(sample_rate)) || !eq_.Configure(sample_rate, channels) || !focus_.Configure(sample_rate, channels)) return false;
    Reset();
    return true;
}

void DspProcessor::ConfigureSpatial() noexcept {
    const Speaker* speakers = Layout(layout_);
    const float rate = static_cast<float>(rate_);
    const float k = rate / (kSound / kHead); // Bilinear transform: s/(2 w0) -> k (1 - z^-1)/(1 + z^-1), w0 = c/a.
    pinna_rate_ = 1.0f - std::exp(-2 * kPi * 4500.0f / rate);
    std::memset(b0_, 0, sizeof(b0_)); std::memset(b1_, 0, sizeof(b1_)); std::memset(a1_, 0, sizeof(a1_));
    std::memset(beta_, 0, sizeof(beta_)); std::memset(lane_gain_, 0, sizeof(lane_gain_));
    lanes_ = 0;
    for (unsigned c = 0; c < channels_; ++c) {
        const auto& s = speakers[c];
        speaker_gain_[c] = s.gain; lfe_[c] = s.lfe;
        if (s.lfe) continue;
        float delay[2]{}, alpha[2]{};
        EarModel(s.azimuth, delay[0], delay[1], alpha[0], alpha[1]);
        const float common = std::min(delay[0], delay[1]); // The nearer ear is undelayed.
        for (unsigned ear = 0; ear < 2; ++ear) {
            const float d = (delay[ear] - common) * rate;
            auto& e = ears_[c][ear];
            std::memset(e.taps, 0, sizeof(e.taps));
            if (d < 1) { e.offset = 0; e.taps[0] = 1 - d; e.taps[1] = d; } // Linear between the two newest samples.
            else {
                // Cubic Lagrange over delays n-1 .. n+2 at fraction u.
                const float n = std::floor(d), u = d - n;
                e.offset = static_cast<std::uint32_t>(n) - 1;
                e.taps[0] = -u * (u - 1) * (u - 2) / 6; e.taps[1] = (u + 1) * (u - 1) * (u - 2) / 2;
                e.taps[2] = -(u + 1) * u * (u - 2) / 2; e.taps[3] = (u + 1) * u * (u - 1) / 6;
            }
            const unsigned lane = lanes_ + ear;
            b0_[lane] = (1 + alpha[ear] * k) / (1 + k); b1_[lane] = (1 - alpha[ear] * k) / (1 + k); a1_[lane] = (1 - k) / (1 + k);
            beta_[lane] = std::fabs(s.azimuth) > 90 ? 0.5f : 0.0f; // Rear: half-weight pinna low-pass.
            lane_gain_[lane] = 0.5f * s.gain; // A centred phantom (both front speakers) keeps its level.
        }
        channel_lane_[c] = lanes_;
        lanes_ += 2;
    }
}

void DspProcessor::ResetSpatial() noexcept {
    std::memset(history_, 0, sizeof(history_)); std::memset(state_, 0, sizeof(state_)); std::memset(pinna_, 0, sizeof(pinna_));
    write_ = 0;
}

void DspProcessor::Reset() noexcept { ResetCore(); night_.Reset(); if (tracker_) tracker_->Reset(); eq_.Reset(); focus_.Reset(); }
void DspProcessor::ResetCore() noexcept {
    high_.z1 = high_.z2 = low_.z1 = low_.z2 = presence_filter_.z1 = presence_filter_.z2 = 0;
    band_energy_ = carrier_energy_ = mid_energy_ = side_energy_ = detector_ = 0;
    presence_ = boost_db_ = reduction_db_ = 0; boost_ = boost_step_ = 0; gain_ = 1; gain_step_ = 0; phase_ = 0;
    clarity_mix_ = spatial_mix_ = 0; level_ = clarity_; idle_ = true;
    ResetSpatial();
}

bool DspProcessor::Active() const noexcept { return CoreActive() || night_.Active() || eq_.Active() || focus_.Active(); }
bool DspProcessor::CoreActive() const noexcept {
    return fold_ || clarity_ || spatial_ || clarity_mix_ > 0 || spatial_mix_ > 0 || boost_ != 0 || gain_ != 1;
}

void DspProcessor::ControlUpdate() noexcept {
    // Presence: the speech band's share of the carrier's energy, times centredness.
    const float share = band_energy_ / (carrier_energy_ + 1e-20f);
    const float centred = carrier_ == 0 ? mid_energy_ / (mid_energy_ + side_energy_ + 1e-20f) : 1.0f;
    const float target = Smoothstep(0.2f, 0.45f, share) * Smoothstep(0.55f, 0.85f, centred);
    const float period = static_cast<float>(kControl) / static_cast<float>(rate_);
    presence_ += (target - presence_) * (1.0f - std::exp(-period / 0.150f));
    // Boost toward the speech target level, gated off below -70 dBFS (no boosting of near-silence).
    const float band_db = Decibels(2 * band_energy_);
    const float wanted = std::clamp(kSpeechTarget - band_db, 0.0f, kMaxBoost[level_]) * presence_ * Smoothstep(-70, -60, band_db) * clarity_mix_;
    boost_db_ += (wanted - boost_db_) * (1.0f - std::exp(-period / (wanted > boost_db_ ? 0.250f : 0.100f)));
    // Soft-knee feed-forward compression on the sine-peak-equivalent level.
    const float level = Decibels(2 * detector_), over = level - kThreshold, slope = 1 / kRatio[level_] - 1;
    float reduction = 0;
    if (over >= kKnee / 2) reduction = slope * over;
    else if (over > -kKnee / 2) reduction = slope * (over + kKnee / 2) * (over + kKnee / 2) / (2 * kKnee);
    reduction *= clarity_mix_;
    reduction_db_ += (reduction - reduction_db_) * (1.0f - std::exp(-period / (reduction < reduction_db_ ? 0.003f : 0.180f)));
    // Linear targets reached by per-sample interpolation over the next control period.
    const float boost = std::pow(10.0f, boost_db_ / 20) - 1, gain = std::pow(10.0f, reduction_db_ / 20);
    boost_step_ = (boost - boost_) / kControl; gain_step_ = (gain - gain_) / kControl;
    stats_.max_boost_db = std::max(stats_.max_boost_db, boost_db_);
    stats_.min_gain_db = std::min(stats_.min_gain_db, reduction_db_);
}

void DspProcessor::Process(float* x, unsigned frames) noexcept {
    if (channels_ && x) {
        // Audio zoom, then the EQ, a hop of the analysis at a time: the EQ of a frame sees the hops completed before it.
        const bool analyse = tracker_ && (analysis_ || eq_.Analysing());
        float* p = x; unsigned left = frames;
        while (left) {
            bool hop = false;
            const unsigned n = analyse ? tracker_->Feed(p, left, channels_, hop) : left; // The stage's input.
            focus_.Process(p, n);
            eq_.Process(p, n, tracker_);
            if (hop) tracker_->Analyse();
            p += static_cast<std::size_t>(n) * channels_; left -= n;
        }
    }
    ProcessCore(x, frames);
    if (channels_ && x) night_.Process(x, frames, channels_); // Meters always; changes the samples only when on.
}
void DspProcessor::ProcessCore(float* x, unsigned frames) noexcept {
    if (!channels_ || !x) return;
    if (clarity_) level_ = clarity_;
    if (!CoreActive()) { if (!idle_) { ResetCore(); idle_ = true; } return; } // Exact passthrough; later activation starts clean.
    idle_ = false;
    if (spatial_ && spatial_mix_ == 0) ResetSpatial(); // Enabling starts from silence under a zero mix.
    const unsigned ch = channels_;
    const float clarity_target = clarity_ ? 1.0f : 0.0f, spatial_target = spatial_ ? 1.0f : 0.0f;
    alignas(16) float input[kLanes]{}, output[kLanes]{}; // Lanes beyond lanes_ stay zero (zero coefficients too).
    for (unsigned f = 0; f < frames; ++f, x += ch) {
        // Finish bypass at a sample boundary, independent of how callers split blocks. The last
        // ramp sample removes the remaining wet gain; asymptotic envelopes must not keep us active.
        if (!clarity_ && clarity_mix_ == 0) {
            boost_ = boost_step_ = boost_db_ = 0;
            gain_ = 1; gain_step_ = reduction_db_ = 0;
        }
        if (!CoreActive()) { ResetCore(); return; }
        const bool clarity = clarity_ || clarity_mix_ > 0;
        // Reject non-finite decoder data before it can poison recursive filters and envelopes.
        for (unsigned c = 0; c < ch; ++c) if (!std::isfinite(x[c])) x[c] = 0;
        const float old_mix = clarity_mix_;
        if (phase_ == 0) ControlUpdate();
        phase_ = (phase_ + 1) % kControl;
        clarity_mix_ = clarity_target > clarity_mix_ ? std::min(clarity_mix_ + mix_rate_, clarity_target) : std::max(clarity_mix_ - mix_rate_, clarity_target);
        spatial_mix_ = spatial_target > spatial_mix_ ? std::min(spatial_mix_ + spatial_rate_, spatial_target) : std::max(spatial_mix_ - spatial_rate_, spatial_target);
        if (!clarity_ && old_mix > 0) {
            const float ratio = clarity_mix_ / old_mix;
            boost_ *= ratio; boost_step_ *= ratio;
            gain_ = 1 + (gain_ - 1) * ratio; gain_step_ *= ratio;
        }
        // ---- Dialogue clarity: detection, then the in-phase presence boost on the dialogue channels ----
        if (clarity) {
            float carrier = carrier_ == 2 ? x[0] : carrier_ == 1 ? x[2] : 0.5f * (x[0] + x[1]);
            const float band = low_.Run(high_.Run(carrier + kAntiDenormal));
            const float presence = presence_filter_.Run(carrier + kAntiDenormal);
            band_energy_ += (band * band - band_energy_) * energy_rate_;
            carrier_energy_ += (carrier * carrier - carrier_energy_) * energy_rate_;
            if (carrier_ == 0) {
                const float side = 0.5f * (x[0] - x[1]);
                mid_energy_ += (carrier * carrier - mid_energy_) * energy_rate_;
                side_energy_ += (side * side - side_energy_) * energy_rate_;
            }
            boost_ += boost_step_;
            const float lift = boost_ * presence;
            if (carrier_ == 1) x[2] += lift;
            else { x[0] += lift; if (carrier_ == 0) x[1] += lift; }
        }
        // ---- Binaural: fractional delays, then the head-shadow filter bank four lanes at a time ----
        if (spatial_mix_ > 0) {
            for (unsigned c = 0; c < ch; ++c) history_[c][write_] = x[c];
            float ears[2]{0, 0};
            for (unsigned c = 0; c < ch; ++c) {
                if (lfe_[c]) { ears[0] += 0.5f * speaker_gain_[c] * x[c]; ears[1] += 0.5f * speaker_gain_[c] * x[c]; continue; }
                for (unsigned ear = 0; ear < 2; ++ear) {
                    const auto& e = ears_[c][ear];
                    float v = 0;
                    for (unsigned t = 0; t < 4; ++t) v += e.taps[t] * history_[c][(write_ - e.offset - t) & (kDelay - 1)];
                    input[channel_lane_[c] + ear] = v + kAntiDenormal;
                }
            }
            Vec sum = Splat(0);
            for (unsigned lane = 0; lane < lanes_; lane += 4) {
                const Vec in = Load(input + lane);
                const Vec y = Add(Mul(Load(b0_ + lane), in), Load(state_ + lane));
                Store(state_ + lane, Sub(Mul(Load(b1_ + lane), in), Mul(Load(a1_ + lane), y)));
                Vec pinna = Load(pinna_ + lane);
                pinna = Add(pinna, Mul(Splat(pinna_rate_), Sub(y, pinna)));
                Store(pinna_ + lane, pinna);
                const Vec shaped = Add(y, Mul(Load(beta_ + lane), Sub(pinna, y)));
                sum = Add(sum, Mul(shaped, Load(lane_gain_ + lane)));
            }
            Store(output, sum); // Lanes alternate left/right ears.
            ears[0] += output[0] + output[2]; ears[1] += output[1] + output[3];
            write_ = (write_ + 1) & (kDelay - 1);
            const float m = spatial_mix_;
            if (fold_) {
                // Crossfade from the downmix (computed from the same input) to the ears.
                float left = 0, right = 0;
                for (unsigned c = 0; c < ch; ++c) { left += fold_left_[c] * x[c]; right += fold_right_[c] * x[c]; }
                x[0] = left + m * (ears[0] - left); x[1] = right + m * (ears[1] - right);
                for (unsigned c = 2; c < ch; ++c) x[c] = 0;
            } else {
                x[0] += m * (ears[0] - x[0]); x[1] += m * (ears[1] - x[1]);
                for (unsigned c = 2; c < ch; ++c) x[c] -= m * x[c];
            }
        } else if (fold_) {
            float left = 0, right = 0;
            for (unsigned c = 0; c < ch; ++c) { left += fold_left_[c] * x[c]; right += fold_right_[c] * x[c]; }
            x[0] = left; x[1] = right;
            for (unsigned c = 2; c < ch; ++c) x[c] = 0;
        }
        // ---- Dynamic range: feed-forward gain, then the soft clipper that bounds every sample below 1 ----
        if (clarity) {
            float loudest = 0;
            for (unsigned c = 0; c < ch; ++c) loudest = std::max(loudest, x[c] * x[c]);
            detector_ += (loudest - detector_) * detector_rate_;
            gain_ += gain_step_;
            for (unsigned c = 0; c < ch; ++c) x[c] *= gain_;
        }
        for (unsigned c = 0; c < ch; ++c) {
            float v = std::isnan(x[c]) ? 0.0f : x[c]; // A NaN from the decoder cannot propagate.
            const float magnitude = std::fabs(v);
            if (magnitude > kClipThreshold) {
                // Smooth saturation into (threshold, 1): tanh reaches exactly 1.0f in float, hence the final bound.
                const float room = 1 - kClipThreshold;
                const float bounded = kClipThreshold + room * std::tanh(std::min(magnitude - kClipThreshold, 1e3f) / room);
                v = std::copysign(std::min(bounded, 0.99999994f), v);
            }
            x[c] = v;
            stats_.peak = std::max(stats_.peak, std::fabs(v));
        }
        ++stats_.frames;
    }
}
}
namespace media::audio {
bool OutputGain::Configure(unsigned sample_rate, unsigned channels) noexcept {
    if (sample_rate < 8000 || sample_rate > 192000 || !channels || channels > DspProcessor::kMaxChannels) return false;
    channels_ = channels; ramp_frames_ = (std::max)(1u, static_cast<unsigned>(std::lround(sample_rate * kRampSeconds)));
    current_ = target_; remaining_ = 0; step_ = 0;
    return true;
}
float OutputGain::Gain(float volume, bool muted) noexcept {
    if (muted || !(volume > 0)) return 0; // NaN and non-positive volumes are silence.
    if (volume > 1) return (std::min)(volume, kMaxVolume); // The boost: linear in amplitude.
    return static_cast<float>(std::pow(10.0, kRangeDb * (static_cast<double>(volume) - 1) / 20));
}
float OutputGain::Decibels(float volume) noexcept {
    if (volume > 1) return 20.0f * std::log10((std::min)(volume, kMaxVolume));
    return volume > 0 ? kRangeDb * (volume - 1) : -HUGE_VALF;
}
void OutputGain::SetTarget(float gain) noexcept {
    gain = gain >= 0 ? (std::min)(gain, kMaxVolume) : 0; // Also maps NaN to 0.
    if (gain == target_) return;
    // Re-aim from the current gain: a new target mid-ramp continues smoothly, never jumps.
    target_ = gain; remaining_ = ramp_frames_; step_ = (target_ - current_) / static_cast<float>(ramp_frames_);
    ++stats_.ramps; stats_.max_step = (std::max)(stats_.max_step, std::fabs(step_));
}
void OutputGain::Process(float* interleaved, unsigned frames) noexcept {
    if (!remaining_ && current_ == 1) return; // Unity: bit-identical passthrough.
    float peak = stats_.peak;
    for (unsigned f = 0; f < frames; ++f) {
        if (remaining_) { if (--remaining_) current_ += step_; else current_ = target_; }
        float* frame = interleaved + static_cast<std::size_t>(f) * channels_;
        if (current_ > 1) {
            // Boosted: the soft knee above -1 dBFS (the class comment).
            constexpr float span = 1.0f - kKnee;
            for (unsigned c = 0; c < channels_; ++c) {
                const float y = frame[c] * current_, a = std::fabs(y);
                // tanh rounds to 1 in float for a large excess: the ceiling keeps "always under 1" true.
                frame[c] = a <= kKnee ? y : std::copysign((std::min)(kKnee + span * std::tanh((a - kKnee) / span), kCeiling), y);
                peak = (std::max)(peak, std::fabs(frame[c]));
            }
            continue;
        }
        for (unsigned c = 0; c < channels_; ++c) { frame[c] *= current_; peak = (std::max)(peak, std::fabs(frame[c])); }
    }
    stats_.peak = peak; stats_.frames += frames;
}
}
