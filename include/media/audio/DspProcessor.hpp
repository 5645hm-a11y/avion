#pragma once
#include "media/audio/Loudness.hpp"
#include "media/audio/MasteringEq.hpp"
#include <atomic>
#include <cstdint>
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Intentional padding for aligned four-lane SIMD arrays.
#endif

namespace media::audio {
// Interleaved channel orders follow WAVEFORMATEXTENSIBLE: 5.1 = FL FR FC LFE BL BR, 7.1 = FL FR FC LFE BL BR SL SR.
enum class SpeakerLayout : std::uint8_t { Mono, Stereo, Surround51, Surround71 };
[[nodiscard]] constexpr bool LayoutFor(unsigned channels, SpeakerLayout& layout) noexcept {
    switch (channels) {
    case 1: layout = SpeakerLayout::Mono; return true;
    case 2: layout = SpeakerLayout::Stereo; return true;
    case 6: layout = SpeakerLayout::Surround51; return true;
    case 8: layout = SpeakerLayout::Surround71; return true;
    default: return false;
    }
}
// UI thread -> audio thread, lock-free. The audio thread samples them once per processed block.
struct DspControls {
    std::atomic<std::uint32_t> clarity{0}; // Dialogue clarity: 0 off, 1 light, 2 medium, 3 strong.
    std::atomic<bool> spatial{false};      // Binaural headphone rendering.
    std::atomic<float> volume{1.0f};       // Output level, 0 to 2: 0..1 on OutputGain's 60 dB taper, 1..2 a boost to x2 (API 16).
    std::atomic<bool> muted{false};
    std::atomic<bool> night{false};        // Night / smart loudness mode (NightMode, after every other stage).
    // Mastering EQ and audio zoom (MasteringEq, AudioFocus; docs/audio-dsp.md, "Mastering EQ and audio zoom").
    std::atomic<float> eq_db[MasteringEq::kBands]{}; // The user's band gains, -12 to +12 dB.
    std::atomic<bool> iso_loudness{false};  // Equal-loudness compensation for the volume (ISO 226).
    std::atomic<bool> speech_ducking{false}; // Speech-protected dynamic EQ.
    std::atomic<bool> notch_killer{false};  // Automatic notches on stationary drones and hum.
    std::atomic<bool> audio_zoom{false};    // The picture's zoom steers the sound (AudioFocus).
    std::atomic<float> zoom{1.0f};          // The picture's zoom (1: the whole picture) ...
    std::atomic<float> zoom_x{0.5f};        // ... and its centre across the picture (0 left, 1 right).
    std::atomic<bool> analysis{false};      // Run the spectral analysis even with nothing above using it (telemetry).
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free &&
              std::atomic<float>::is_always_lock_free);
// Output level stage (volume and mute), applied to interleaved float PCM as it leaves the PCM ring for the endpoint, so a
// key press is heard within the endpoint's buffer rather than after the whole ring.
// - Volume v in [0, 1] follows a perceptual taper: gain = 10^(60 (v - 1) / 20) dB for v > 0, silence at 0. A 5 % step
//   is 3 dB anywhere on the scale.
// - Every change of the target (volume or mute) ramps the gain linearly in amplitude over 20 ms, from wherever it is
//   (a ramp in progress is simply re-aimed): no click, no zipper noise. The ramp is per frame, identical on every
//   channel, and does not depend on how the stream is split into blocks.
// - At unity with no ramp in progress the samples are left bit-identical.
// - Boost (API 16, 2026-11-04): v in (1, 2] is gain v, up to x2 (+6 dB), for quiet recordings. A gain above 1 can push samples
//   over full scale, so while it is above 1 every sample goes through a soft knee from kKnee (-1 dBFS): below it untouched,
//   above it k + (1 - k) tanh((|x| - k) / (1 - k)), continuous in value and slope, always under 1. Nothing hard-clips.
class OutputGain final {
public:
    static constexpr float kRangeDb = 60, kRampSeconds = 0.02f;
    static constexpr float kMaxVolume = 2.0f, kKnee = 0.8912509f, kCeiling = 0.99998f; // x2; -1 dBFS; just under full scale.
    struct Statistics { std::uint64_t frames = 0; unsigned ramps = 0; float peak = 0, max_step = 0; }; // Scaled frames; ramps started.
    [[nodiscard]] bool Configure(unsigned sample_rate, unsigned channels) noexcept; // Setup only.
    [[nodiscard]] static float Gain(float volume, bool muted) noexcept;             // Target linear gain.
    [[nodiscard]] static float Decibels(float volume) noexcept;                     // -inf at 0.
    void Apply(const DspControls& controls) noexcept {
        SetTarget(Gain(controls.volume.load(std::memory_order_relaxed), controls.muted.load(std::memory_order_relaxed)));
    }
    void SetTarget(float gain) noexcept;
    void Process(float* interleaved, unsigned frames) noexcept;
    [[nodiscard]] float Current() const noexcept { return current_; }
    [[nodiscard]] bool Ramping() const noexcept { return remaining_ != 0; }
    [[nodiscard]] const Statistics& Stats() const noexcept { return stats_; }
private:
    unsigned channels_ = 0, ramp_frames_ = 960, remaining_ = 0;
    float current_ = 1, target_ = 1, step_ = 0;
    Statistics stats_{};
};
struct DspStatistics {
    std::uint64_t frames = 0;        // Frames processed (bypassed blocks excluded).
    float peak = 0;                  // Largest output magnitude produced (always < 1).
    float max_boost_db = 0;          // Largest dialogue-band boost applied.
    float min_gain_db = 0;           // Deepest dynamic-range gain reduction applied (<= 0).
};

// Real-time audio DSP applied in place to interleaved float PCM, between the decoder and the endpoint (the
// engine runs it on the PCM ring span it has just filled). No allocation, no locks, no lookahead latency.
//
// Dialogue clarity (level 1-3):
//   The dialogue carrier (centre channel, else the stereo mid, else mono) is band-passed to the speech band
//   (300 Hz high-pass and 3.4 kHz low-pass biquads, minimum phase). Speech presence is the speech band's share of the
//   carrier's short-term energy, times centredness (mid versus side energy): music and noise spread energy across
//   the spectrum or the stereo field, dialogue does not. The band is boosted toward a -20 dBFS level, by at most 4, 7
//   or 10 dB, with slow gain ballistics (250 ms rise, 100 ms fall) that do not follow syllables: no pumping. A soft-knee
//   feed-forward compressor (3 ms attack, 180 ms release) then tames loud transients, and a soft clipper above
//   -1 dBFS guarantees |output| < 1 (it only touches samples the compressor's attack has not yet caught).
// Binaural rendering (spatial, two or more channels):
//   Every speaker of the layout is a virtual source at its standard azimuth (stereo +-30, centre 0, 5.1 surrounds
//   +-110, 7.1 sides +-90 and backs +-150). A spherical-head model (Brown and Duda) gives each ear a propagation delay
//   from its incidence angle (their difference is Woodworth's ITD), applied with a cubic Lagrange fractional delay,
//   and a first-order head-shadow filter whose high-frequency gain alpha(incidence) ranges from +6 dB (facing the ear)
//   to -20 dB (shadowed): a frequency-dependent ILD. Rear sources get a gentle pinna low-pass. LFE feeds both ears.
//   The ears replace channels 0 and 1; other channels are silenced. Mono input has no binaural output channel.
// Stereo fold-down (surround input, stereo endpoint): channels 0 and 1 carry the whole programme and the others are
//   always zero, so the caller can keep just the first two. With spatial off they carry an ITU-R BS.775 downmix
//   (L = FL + FC/sqrt2 + left surrounds/sqrt2, R likewise; LFE omitted), scaled by 0.5 so that a centred voice keeps the
//   level the binaural renderer gives it; with spatial on, the binaural ears. Toggling crossfades the two over the
//   same 30 ms ramp. A fold-down processor is never an exact passthrough.
// Audio zoom (AudioFocus) and the mastering EQ (MasteringEq, with its SpectralTracker analysis of the stage's input) run
// first, on the source channels, in that order; docs/audio-dsp.md "Mastering EQ and audio zoom".
// Night / smart loudness mode (audio::NightMode, docs/audio-dsp.md "Loudness and night mode") runs last, on the stage's
// output (a fold-down's stereo pair): BS.1770 meters always, level lift and burst control and a true-peak limiter when on.
// Every control change ramps (clarity over 50 ms, spatial over 30 ms), so toggling never clicks. With both off and
// the ramps finished (and no fold-down), Process leaves the samples bit-identical.
// Gains and presence are updated at a fixed 32-frame control rate, independent of block size: the output does not
// depend on how the stream is split into blocks. The head-shadow filter bank runs four lanes wide (SSE on x64,
// NEON on ARM, scalar elsewhere).
class DspProcessor final {
public:
    static constexpr unsigned kMaxChannels = 8;
    static constexpr float kClipThreshold = 0.8912509f; // -1 dBFS: above it the soft clipper engages.
    // Setup only. Sample rates 8-192 kHz; channel counts 1, 2, 6 or 8. `fold_down` (6 or 8 channels only) renders the
    // programme into channels 0 and 1 for a stereo endpoint.
    [[nodiscard]] bool Configure(unsigned sample_rate, unsigned channels, bool fold_down = false) noexcept;
    void Reset() noexcept; // Clears filter, delay, ramp and envelope state (seek); keeps settings and cumulative statistics.
    void SetClarity(unsigned level) noexcept { clarity_ = level > 3 ? 3 : level; }
    void SetSpatial(bool enabled) noexcept { spatial_ = enabled && channels_ >= 2; }
    void Apply(const DspControls& controls) noexcept {
        SetClarity(controls.clarity.load(std::memory_order_relaxed)); SetSpatial(controls.spatial.load(std::memory_order_relaxed));
        night_.SetEnabled(controls.night.load(std::memory_order_relaxed));
        for (unsigned b = 0; b < MasteringEq::kBands; ++b) eq_.SetBand(b, controls.eq_db[b].load(std::memory_order_relaxed));
        const float volume = controls.volume.load(std::memory_order_relaxed);
        eq_.SetIsoLoudness(controls.iso_loudness.load(std::memory_order_relaxed), OutputGain::Decibels(volume));
        eq_.SetSpeechDucking(controls.speech_ducking.load(std::memory_order_relaxed));
        eq_.SetNotchKiller(controls.notch_killer.load(std::memory_order_relaxed));
        focus_.Set(controls.audio_zoom.load(std::memory_order_relaxed), controls.zoom.load(std::memory_order_relaxed),
                   controls.zoom_x.load(std::memory_order_relaxed));
        analysis_ = controls.analysis.load(std::memory_order_relaxed);
    }
    MasteringEq& Eq() noexcept { return eq_; }
    const MasteringEq& Eq() const noexcept { return eq_; }
    AudioFocus& Focus() noexcept { return focus_; }
    const AudioFocus& Focus() const noexcept { return focus_; }
    // The spectral analysis the notch killer, the speech ducking and the frame signatures read: the caller's (112 KB; kept out
    // of the processor so that one on a thread's stack stays small), attached before Configure, which configures it too.
    // Without one, the notch killer and the ducking do nothing. Setup only.
    void AttachAnalysis(SpectralTracker* tracker) noexcept { tracker_ = tracker; }
    const SpectralTracker* Analysis() const noexcept { return tracker_; }
    void SetAnalysis(bool on) noexcept { analysis_ = on; } // Run the analysis with neither ducking nor the notch killer on.
    void SetNight(bool enabled) noexcept { night_.SetEnabled(enabled); }
    // Night mode and the BS.1770 meters (of the stage's output before night mode, and after it).
    const NightMode& Night() const noexcept { return night_; }
    void Process(float* interleaved, unsigned frames) noexcept;
    [[nodiscard]] bool Active() const noexcept; // False while the stage is an exact passthrough.
    [[nodiscard]] const DspStatistics& Statistics() const noexcept { return stats_; }
    [[nodiscard]] unsigned Channels() const noexcept { return channels_; }
    [[nodiscard]] bool FoldDown() const noexcept { return fold_; }
    // Downmix weights of `channel` into the left and right outputs for a layout (fold-down with spatial off).
    static void DownmixWeights(unsigned channels, unsigned channel, float& left, float& right) noexcept;
    // Model introspection for tests and documentation: per-ear delay (seconds, before removing the common part)
    // and head-shadow high-frequency gain for a source at `azimuth` degrees (negative = left).
    static void EarModel(float azimuth, float& delay_left, float& delay_right, float& alpha_left, float& alpha_right) noexcept;

private:
    static constexpr unsigned kControl = 32;      // Frames per control-rate update.
    static constexpr unsigned kDelay = 256;       // Fractional-delay history per channel (ITD up to 1.3 ms at 192 kHz).
    static constexpr unsigned kLanes = 16;        // Head-shadow filters: two ears for up to eight speakers.
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; float Run(float x) noexcept; };
    struct Ear { std::uint32_t offset = 0; float taps[4]{}; };
    void ControlUpdate() noexcept;
    void ProcessCore(float* interleaved, unsigned frames) noexcept;
    void ResetCore() noexcept;
    bool CoreActive() const noexcept;
    NightMode night_;
    SpectralTracker* tracker_ = nullptr;
    MasteringEq eq_;
    AudioFocus focus_;
    bool analysis_ = false;
    void ConfigureSpatial() noexcept;
    void ResetSpatial() noexcept;
    // Settings.
    unsigned rate_ = 48000, channels_ = 0, clarity_ = 0, level_ = 0, lanes_ = 0, carrier_ = 0; // carrier_: 0 mid, 1 centre, 2 mono.
    bool idle_ = true; // State cleared for passthrough.
    bool spatial_ = false, fold_ = false;
    float fold_left_[kMaxChannels]{}, fold_right_[kMaxChannels]{};
    SpeakerLayout layout_ = SpeakerLayout::Stereo;
    // Dialogue clarity state.
    Biquad high_, low_, presence_filter_; // Detection band (300 Hz HP, 3.4 kHz LP); in-phase presence band-pass.
    float band_energy_ = 0, carrier_energy_ = 0, mid_energy_ = 0, side_energy_ = 0, detector_ = 0;
    float presence_ = 0, boost_db_ = 0, reduction_db_ = 0, clarity_mix_ = 0;
    float boost_ = 0, boost_step_ = 0, gain_ = 1, gain_step_ = 0; // Per-sample interpolated linear values.
    float energy_rate_ = 0, detector_rate_ = 0, mix_rate_ = 0, spatial_rate_ = 0;
    unsigned phase_ = 0; // Frames since the last control update.
    // Binaural state.
    float spatial_mix_ = 0;
    float history_[kMaxChannels][kDelay]{};
    std::uint32_t write_ = 0;
    Ear ears_[kMaxChannels][2]{};
    float speaker_gain_[kMaxChannels]{};
    unsigned channel_lane_[kMaxChannels]{};
    bool lfe_[kMaxChannels]{};
    alignas(16) float b0_[kLanes]{}, b1_[kLanes]{}, a1_[kLanes]{}, state_[kLanes]{};
    alignas(16) float beta_[kLanes]{}, pinna_[kLanes]{}, lane_gain_[kLanes]{};
    float pinna_rate_ = 0;
    DspStatistics stats_{};
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
