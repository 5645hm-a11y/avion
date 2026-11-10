#pragma once
#include <cstdint>
namespace media::audio {
// ITU-R BS.1770-4 loudness (docs/audio-dsp.md, "Loudness and night mode"). Portable; fixed storage (nothing allocated,
// no locks, no clock); one owner thread.
//
// K-weighting: the spec's two biquads (a high shelf of +4 dB above ~1.7 kHz, then the RLB high-pass at ~38 Hz), derived
// for any sample rate from their analogue prototypes (at 48 kHz they are the coefficients the spec tabulates). Channel
// weights: 1 for left, right and centre, 1.41 (+1.5 dB) for the surrounds, 0 for LFE. Loudness = -0.691 + 10 log10 of
// the weighted mean square. Measured over 100 ms sub-blocks: momentary over the last 4 (400 ms), short-term over the last
// 30 (3 s), and integrated over every 400 ms block with the spec's gating (absolute -70 LUFS, then relative -10 LU), held
// in a 0.1 LU histogram of block energies (exact energy sums, the gate's threshold to 0.1 LU).
//
// True peak: each channel 4x oversampled by a 48-tap polyphase interpolator (12 taps a phase, windowed sinc), the largest
// magnitude of the samples and the three points between each pair (Annex 2's method). It reports peaks a sample-peak
// meter misses: a full-scale sine at a quarter of the sample rate, sampled 45 degrees off its crests, has sample peaks of
// 0.707 and a true peak of 1.0.
class LoudnessMeter final {
public:
    static constexpr unsigned kMaxChannels = 8;
    static constexpr float kSilence = -144.0f; // Reported before any measurement, or for digital silence.
    // Setup only. 8-192 kHz, 1-8 channels. `layout`: channel `i` is LFE when bit i of `lfe` is set; surround when bit i of
    // `surround` is set.
    [[nodiscard]] bool Configure(unsigned rate, unsigned channels, std::uint32_t surround = 0, std::uint32_t lfe = 0) noexcept;
    void Reset() noexcept; // Forgets every measurement and filter state (a seek).
    // Interleaved input, `stride` floats from one frame to the next (the first `channels` of them are measured).
    void Feed(const float* interleaved, unsigned frames, unsigned stride) noexcept;
    float Momentary() const noexcept { return momentary_; }   // LUFS, the last 400 ms (updated every 100 ms).
    float ShortTerm() const noexcept { return short_term_; }  // LUFS, the last 3 s.
    float Integrated() const noexcept;                         // LUFS, gated, since the last Reset.
    float TruePeak() const noexcept;                           // dBTP: the largest true peak since the last Reset.
    float RecentTruePeak() const noexcept;                     // dBTP: the largest of the last 3 s.
    // The two K-weighting stages (b0, b1, b2, a1, a2; a0 = 1) for a sample rate: tests compare them with the spec's table.
    static void KWeighting(double rate, double shelf[5], double highpass[5]) noexcept;
    // One channel's true-peak interpolator, for the limiter: feed a sample, get the largest magnitude of it and of the three
    // points interpolated before it (they lie kTruePeakDelay samples back).
    struct Oversampler {
        float history[12]{};
        unsigned write = 0;
        float Push(float sample) noexcept;
        void Reset() noexcept { for (auto& h : history) h = 0; write = 0; }
    };
    static constexpr unsigned kTruePeakDelay = 6; // Samples between an input and the points interpolated around it.
private:
    struct Stage { double b0, b1, b2, a1, a2; };
    struct State { double z1 = 0, z2 = 0; };
    static constexpr unsigned kSubBlocks = 30, kBins = 750; // 3 s of 100 ms; -70..+5 LUFS in 0.1 LU.
    void CloseSubBlock() noexcept;
    unsigned rate_ = 48000, channels_ = 0, sub_frames_ = 4800, in_sub_ = 0;
    float weights_[kMaxChannels]{};
    Stage shelf_{}, highpass_{};
    State shelf_state_[kMaxChannels]{}, highpass_state_[kMaxChannels]{};
    double sub_energy_ = 0;            // The open sub-block's weighted sum of squares.
    double subs_[kSubBlocks]{};        // Mean squares of the last 30 sub-blocks.
    unsigned sub_count_ = 0, sub_next_ = 0;
    float momentary_ = kSilence, short_term_ = kSilence;
    double bin_energy_[kBins]{};       // Integrated: sum of block mean squares per 0.1 LU bin, and their count.
    std::uint32_t bin_count_[kBins]{};
    Oversampler peaks_[kMaxChannels]{};
    float peak_ = 0, sub_peak_ = 0;    // Linear: all time; the open sub-block.
    float sub_peaks_[kSubBlocks]{};
};

// Night / smart loudness mode: lifts quiet passages toward a target loudness and pulls sudden loud ones down, then limits
// the true peak. For speech under music and effects, at night, through laptop speakers.
// - Level: the short-term loudness of the input (3 s, K-weighted) against the target (-20 LUFS): up to +12 dB of lift, down
//   to -12 dB. The gain moves at most 3 dB/s up and 10 dB/s down: slow enough not to pump with syllables or beats.
// - Bursts: the momentary loudness (400 ms) above target + 4 LU pulls the gain down at up to 30 dB/s.
// - Silence: while the momentary loudness is below -50 LUFS, or more than 15 LU below the short-term, the gain is frozen:
//   pauses between sentences and the noise floor of a quiet room are never lifted.
// - Limiter: a look-ahead true-peak limiter (Latency(): kLookahead - 1 frames plus the oversampler's 6) at -1 dBTP. Its gain is the
//   minimum required over the look-ahead window, averaged over the window (so it is in place before the peak arrives,
//   without a step), released at 8 dB/s.
// - Switching on and off crossfades over 50 ms. Off and settled, the samples pass bit-identical and undelayed; on, they are
//   delayed by Latency() frames.
class NightMode final {
public:
    static constexpr unsigned kMaxChannels = LoudnessMeter::kMaxChannels;
    static constexpr unsigned kLookahead = 64;
    static constexpr float kTarget = -20.0f, kMaxLift = 12.0f, kMaxCut = -12.0f, kCeiling = -1.0f;
    [[nodiscard]] bool Configure(unsigned rate, unsigned channels, std::uint32_t surround = 0, std::uint32_t lfe = 0) noexcept;
    void Reset() noexcept;
    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
    bool Enabled() const noexcept { return enabled_; }
    // In place, interleaved: the first `channels` floats of each frame, `stride` floats apart (0: `channels`). Meters the
    // input (Meter()) and the output (Output()) whether on or off.
    void Process(float* interleaved, unsigned frames, unsigned stride = 0) noexcept;
    bool Active() const noexcept { return enabled_ || mix_ > 0; }
    static constexpr unsigned Latency() noexcept { return kLookahead - 1 + LoudnessMeter::kTruePeakDelay; }
    const LoudnessMeter& Meter() const noexcept { return input_; }    // The programme as decoded.
    const LoudnessMeter& Output() const noexcept { return output_; } // After night mode and its limiter.
    float GainDb() const noexcept { return gain_db_; }     // The level gain now (the limiter's on top).
    float LimiterDb() const noexcept { return limit_db_; } // The limiter's deepest reduction in the last block (<= 0).
private:
    static constexpr unsigned kDelay = 128; // Ring length, a power of two above Latency().
    static constexpr unsigned kControl = 32;
    void Control() noexcept;
    unsigned rate_ = 48000, channels_ = 0, phase_ = 0;
    bool enabled_ = false;
    LoudnessMeter input_, output_;
    float gain_db_ = 0, gain_ = 1, gain_step_ = 0, mix_ = 0, mix_rate_ = 0;
    float rise_ = 0, fall_ = 0, burst_ = 0; // dB per control block.
    // Limiter: per channel delay line and oversampler; the required gain per frame in the look-ahead window, its running
    // minimum (monotonic queue) and the window's average of those minima.
    float delay_[kMaxChannels][kDelay]{};
    unsigned write_ = 0;
    LoudnessMeter::Oversampler peaks_[kMaxChannels]{};
    float required_[kDelay]{};
    unsigned queue_[kDelay]{}; unsigned queue_head_ = 0, queue_size_ = 0;
    std::uint64_t frame_ = 0;
    float held_[kDelay]{};
    float limiter_ = 1, release_ = 0, limit_db_ = 0;
};
}
