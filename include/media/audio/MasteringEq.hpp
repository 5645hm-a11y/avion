#pragma once
#include <cstdint>
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // Intentional padding for aligned arrays.
#endif

namespace media::audio {
// Psychoacoustic mastering EQ and audio-visual focus (docs/audio-dsp.md, "Mastering EQ and audio zoom"). Portable, fixed
// storage, no allocation, no locks; one owner thread (the media worker, through DspProcessor).

// What the analysis of the programme sees, one hop (1024 frames) at a time: the octave band levels of the ten EQ bands, the
// dominant band, a speech probability and the stationary tones (drones, mains hum) it has followed for long enough.
// - The input is the mean of the channels, Hann-windowed into a 4096-point radix-2 FFT every 1024 frames (75 % overlap).
// - Band b's level sums the power of the bins in [centre / sqrt2, centre * sqrt2), normalised so that a full-scale sine reads
//   0 dB in its band (the window spreads an on-bin sine over three bins whose powers sum to 1.5 (N/4)^2).
// - Speech: the speech band's (300-3400 Hz) share of everything from 300 Hz to 8 kHz (bass and drones excluded, so music under
//   a voice does not hide it), times its syllabic modulation (the standard deviation of its level over the last 0.5 s: a voice
//   comes and goes 4-10 dB, steady noise and drones do not), gated below -65 dBFS. Smoothed over 150 ms.
// - Tones: local maxima standing at least 15 dB above the median of the 48 bins around them (excluding the peak's own lobe) and
//   above -70 dBFS. Each frequency is measured from the phase advance of its bin between hops (a phase vocoder: a stable tone is
//   located to a small fraction of a hertz, where the bins are 11.7 Hz apart at 48 kHz). A tone found again within 3 Hz is the
//   same tone; it becomes stationary once followed for 2 s with its frequency within 1 Hz (mean absolute deviation) and its
//   level within 3 dB (2 Hz and 6 dB keep it so: a voice's fundamental passing inside the tone's main lobe makes both jitter).
//   It is dropped after 1 s unseen. Speech harmonics glide and come and go: they never qualify. A synthetic pad held without
//   vibrato for more than 2 s would (a known limit).
// The output does not depend on how the stream is split into blocks.
class SpectralTracker final {
public:
    static constexpr unsigned kSize = 4096, kHop = 1024, kBins = kSize / 2 + 1, kBands = 10, kMaxTracks = 12, kCandidates = 8;
    static constexpr float kBandCentres[kBands]{31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
    struct Tone {
        float hz = 0, level_db = -144.0f, prominence_db = 0;
        float deviation_hz = 0, level_deviation_db = 0; // Mean absolute deviations (exponential, 0.5 s).
        unsigned age = 0, missed = 0;                   // Hops followed; hops since last seen.
        bool live = false, stationary = false;
    };
    [[nodiscard]] bool Configure(unsigned rate) noexcept; // 8-192 kHz. Setup only.
    void Reset() noexcept;                               // A seek: history, tracks and levels start over.
    void Push(const float* interleaved, unsigned frames, unsigned channels) noexcept; // Feed, analysing at every hop.
    // Stores up to the end of the hop in progress (returns the frames taken); `hop` true when that completed one: the caller
    // then calls Analyse, after it has used the analysis so far for these frames (DspProcessor: the EQ sees only hops
    // completed before a frame, so the output does not depend on the block sizes).
    unsigned Feed(const float* interleaved, unsigned frames, unsigned channels, bool& hop) noexcept;
    void Analyse() noexcept;
    [[nodiscard]] unsigned UntilHop() const noexcept { return kHop - filled_; }
    [[nodiscard]] std::uint64_t Analyses() const noexcept { return analyses_; }
    [[nodiscard]] float Speech() const noexcept { return speech_; }        // 0..1.
    [[nodiscard]] unsigned Dominant() const noexcept { return dominant_; }  // The loudest enabled band (kBands: silence).
    [[nodiscard]] float BandDb(unsigned b) const noexcept { return b < kBands ? band_db_[b] : -144.0f; }
    [[nodiscard]] bool BandEnabled(unsigned b) const noexcept { return b < kBands && kBandCentres[b] * 1.2f < 0.5f * static_cast<float>(rate_); }
    [[nodiscard]] const Tone& Track(unsigned i) const noexcept { return tracks_[i < kMaxTracks ? i : 0]; }
    [[nodiscard]] unsigned Rate() const noexcept { return rate_; }
private:
    unsigned rate_ = 0, filled_ = 0, write_ = 0, stationary_hops_ = 0, drop_hops_ = 0, level_count_ = 0;
    std::uint64_t analyses_ = 0, since_reset_ = 0;
    float speech_ = 0, speech_rate_ = 0, deviation_rate_ = 0;
    unsigned dominant_ = kBands;
    float band_db_[kBands]{};
    float speech_levels_[24]{};
    Tone tracks_[kMaxTracks]{};
    float history_[kSize]{};   // Ring of the channel mean.
    float window_[kSize]{};    // Periodic Hann.
    float real_[kSize]{}, imaginary_[kSize]{};
    float cosine_[kSize / 2]{}, sine_[kSize / 2]{};
    float power_[kBins]{}, phase_[kBins]{}, previous_phase_[kBins]{};
    std::uint16_t reversed_[kSize]{};
};

// The ten-band psychoacoustic EQ (ISO octave centres 31.5 Hz to 16 kHz), in place on interleaved float PCM.
// - Bands: RBJ peaking biquads, Q 1.414 (one octave), +-12 dB, one per channel; a band whose centre is not below the sample
//   rate / 2.4 stays flat (16 kHz needs 44.1 kHz). Each band's gain is the sum of the user's (SetBand), iso-loudness and ducking, ramped in dB at a
//   32-frame control rate (50 ms; ducking 50 ms down and 300 ms up), so moving a slider never clicks.
// - Iso-loudness (SetIsoLoudness): quiet playback loses bass and treble first (ISO 226:2003 equal-loudness contours). With full
//   volume taken as an 80-phon reference, playing `level_db` lower (OutputGain's taper) is listening at 80 + level_db phon;
//   each band gets the contour difference (L(f, phon) - phon) - (L(f, 80) - 80), from 0 to +12 dB.
// - Speech-protected dynamic EQ (SetSpeechDucking): while the analysis hears speech, the bands from 31.5 to 250 Hz that stand
//   more than 6 dB above the speech bands (1-4 kHz) are lowered by the excess, at most 6 dB, times the speech probability, so
//   music and effects stop masking the voice; and the user's cuts on 500 Hz-4 kHz are held at -3 dB at most (protected).
// - Notch killer (SetNotchKiller): every stationary tone the analysis has followed (SpectralTracker) gets an RBJ notch at Q 12
//   centred on its measured frequency, faded in over 100 ms; when the tone has gone for 1 s, the notch fades out over 300 ms.
//   At most kMaxNotches at once (the most prominent first).
// - Above -1 dBFS a soft clipper keeps boosts below full scale (as DspProcessor's).
// With every band at 0 dB, no notch and the ramps finished, Process leaves the samples bit-identical. The output does not
// depend on how the stream is split into blocks.
class MasteringEq final {
public:
    static constexpr unsigned kBands = SpectralTracker::kBands, kMaxChannels = 8, kMaxNotches = 6;
    static constexpr float kMaxDb = 12.0f, kNotchQ = 12.0f, kBandQ = 1.41421356f;
    [[nodiscard]] bool Configure(unsigned rate, unsigned channels) noexcept;
    void Reset() noexcept; // Filter states and ramps (seek); keeps the settings.
    void SetBand(unsigned band, float db) noexcept;
    [[nodiscard]] float Band(unsigned band) const noexcept { return band < kBands ? user_db_[band] : 0.0f; }
    void SetIsoLoudness(bool on, float level_db) noexcept { iso_ = on; iso_level_db_ = level_db; }
    void SetSpeechDucking(bool on) noexcept { ducking_ = on; }
    void SetNotchKiller(bool on) noexcept { notch_killer_ = on; }
    // `analysis`: the hops analysed before these frames (DspProcessor feeds it); null: no ducking and no notches.
    void Process(float* interleaved, unsigned frames, const SpectralTracker* analysis) noexcept;
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] bool Analysing() const noexcept { return ducking_ || notch_killer_; } // Needs the analysis.
    [[nodiscard]] float AppliedDb(unsigned band) const noexcept { return band < kBands ? current_db_[band] : 0.0f; } // Total now.
    [[nodiscard]] float DuckDb(unsigned band) const noexcept { return band < kBands ? duck_db_[band] : 0.0f; }
    [[nodiscard]] float IsoDb(unsigned band) const noexcept { return band < kBands ? iso_db_[band] : 0.0f; }
    [[nodiscard]] unsigned Notches() const noexcept;                   // Notches engaged (depth > 0).
    [[nodiscard]] float NotchHz(unsigned i) const noexcept { return i < kMaxNotches ? notches_[i].hz : 0.0f; }
    [[nodiscard]] float NotchDepth(unsigned i) const noexcept { return i < kMaxNotches ? notches_[i].depth : 0.0f; }
    // ISO 226:2003: the sound pressure level (dB) of a tone at `hz` (20 Hz to 12.5 kHz, clamped) heard as loud as `phon`.
    [[nodiscard]] static double EqualLoudness(double hz, double phon) noexcept;
    // The iso-loudness gain of a band when playing `level_db` below the 80-phon reference.
    [[nodiscard]] static float IsoGain(unsigned band, float level_db) noexcept;
    // Analytic response of an RBJ peaking biquad (tests, docs).
    [[nodiscard]] static double PeakingResponseDb(double hz, double centre, double gain_db, double q, double rate) noexcept;
private:
    static constexpr unsigned kControl = 32;
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct Notch { float hz = 0, depth = 0, from = 0, target = 0; Biquad c{}; bool live = false; };
    void ControlUpdate(const SpectralTracker*) noexcept;
    void ResetStates() noexcept;
    [[nodiscard]] bool Wanted() const noexcept; // The loop must run: something on, or still ramping out.
    unsigned rate_ = 48000, channels_ = 0, phase_ = 0;
    bool iso_ = false, ducking_ = false, notch_killer_ = false, idle_ = true;
    float iso_level_db_ = 0, iso_cached_db_ = 0, period_ = 0; // iso_cached_db_: the level iso_db_ holds (0: flat).
    float user_db_[kBands]{}, iso_db_[kBands]{}, duck_db_[kBands]{}, smooth_db_[kBands]{}, current_db_[kBands]{}, coefficient_db_[kBands]{};
    Biquad bands_[kBands]{};
    bool band_live_[kBands]{};
    unsigned tail_[kBands]{}, tail_frames_ = 12000; // Frames a band at 0 dB still runs (its state decaying).
    float z_[kBands][kMaxChannels][2]{};
    Notch notches_[kMaxNotches]{};
    float notch_z_[kMaxNotches][kMaxChannels][2]{};
    float notch_rate_in_ = 0, notch_rate_out_ = 0;
};

// Audio-visual focus ("audio zoom"): the picture's zoom steers a directional lobe of the sound toward what is zoomed into.
// The zoom's centre across the picture, x in [0, 1], is a direction in the stereo image (constant-power panning angle
// phi0 = x pi / 2: 0 hard left, pi/4 the centre, pi/2 hard right). With u = (cos phi0, sin phi0) and v = (-sin phi0, cos phi0)
// the sound splits into the beam b = u.(L, R), which holds every source panned at phi0 whole, and the residual o = v.(L, R),
// which holds what comes from elsewhere. Output: u (b + boost * presence(b)) + g v o: a pure real 2x2 matrix (phase coherent,
// no latency), plus a presence band-pass (2.5 kHz, Q 0.7) on the beam only, so detail and dialogue from the target come forward.
// Focus f = smoothstep(1.2, 2.4, zoom): g = -12 f dB (a source on the opposite side loses 12 dB at full focus, one at phi0
// nothing), boost +5 f dB. Surround layouts: the front pair is the stereo pair; the centre keeps its on-axis share
// sqrt(cos^2 + g^2 sin^2) of the angle between it and phi0; surrounds and LFE (ambience) take g. Mono is left alone.
// Matrix and boost are interpolated per frame over 50 ms toward each new target, so panning glides. Off (or zoom <= 1.2)
// with the ramps finished, the samples are left bit-identical.
class AudioFocus final {
public:
    static constexpr float kStart = 1.2f, kFull = 2.4f, kResidualDb = -12.0f, kBoostDb = 5.0f;
    [[nodiscard]] bool Configure(unsigned rate, unsigned channels) noexcept;
    void Reset() noexcept;
    void Set(bool enabled, float zoom, float x) noexcept { enabled_ = enabled; zoom_ = zoom; x_ = x; }
    void Process(float* interleaved, unsigned frames) noexcept;
    [[nodiscard]] bool Active() const noexcept { return channels_ >= 2 && (focus_ > 0 || Target() > 0); }
    [[nodiscard]] float Focus() const noexcept { return focus_; }
    [[nodiscard]] float Angle() const noexcept { return angle_; }
    [[nodiscard]] float Target() const noexcept;
    // The steady-state gain of a source panned at `x` (0..1, constant power) with focus `focus` toward `target_x` (tests).
    [[nodiscard]] static float SourceGain(float x, float target_x, float focus) noexcept;
private:
    unsigned rate_ = 48000, channels_ = 0;
    bool enabled_ = false;
    float zoom_ = 1, x_ = 0.5f;
    float focus_ = 0, angle_ = 0.785398163f, step_ = 0; // Ramped focus and angle; per-frame ramp rate.
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0; // Presence band-pass on the beam.
};
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
