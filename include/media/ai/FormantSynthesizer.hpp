#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace media::ai {
// A speaker, as the synthesiser varies one: what changes between people (pitch, vocal-tract length, pace, voice
// quality) and between rooms (reverberation, noise). Every field has a neutral default: an adult male voice, dry.
struct SynthVoice {
    float f0 = 115;              // Mean fundamental frequency, Hz (adult men ~85-155, women ~165-255, children higher).
    float formant_scale = 1;     // Vocal-tract length: every formant and noise band times this (women ~1.12, children ~1.25).
    float rate = 1;              // Speaking rate: 1.25 is a quarter faster (durations divided by it).
    float breathiness = 0.06f;   // Aspiration noise mixed into voiced sound, relative to the voicing (0-0.5).
    float open_quotient = 0.6f;  // Share of each glottal period the glottis is open (0.4 pressed - 0.75 lax).
    float jitter = 0.01f;        // Period-to-period pitch variation (fraction of the period).
    float shimmer = 0.04f;       // Period-to-period amplitude variation (fraction).
    float tilt = 0.4f;           // Spectral tilt of the source, 0 (bright) - 0.9 (dark).
    float intonation = 1;        // Depth of the pitch contour (declination, stress, final fall) times this.
    float reverb_seconds = 0;    // RT60 of a synthetic room (0: dry). A Schroeder reverberator, 0.1-1.5 s.
    float reverb_mix = 0.25f;    // Wet share when reverb_seconds > 0.
    std::uint32_t seed = 1;      // Noise sources and jitter: the same seed, the same samples.
};
// Where each word of the phoneme string lies in the output, in samples (begin inclusive, end exclusive).
struct SynthWord { std::uint32_t begin = 0, end = 0; };

// A deterministic formant synthesiser for training and testing the voice path without recorded or OS voices
// (docs/voice.md, "Synthesised speech"). Speech in every language is made of the same physics: a glottal source
// shaped by the resonances of the vocal tract (formants F1-F5), turbulent noise at a constriction (fricatives),
// bursts after a closure (plosives), and a nasal branch. Phonemes are rendered from articulatory targets, so a
// Hebrew, French, English or Arabic word is just a phoneme string; speakers are varied by the SynthVoice fields.
//
// Source: a Rosenberg glottal pulse (differentiated: lip radiation) on a pitch contour (declination, a rise on each
// stressed vowel, a final fall) with jitter and shimmer, a one-pole spectral tilt, and aspiration noise.
// Tract: five cascaded second-order resonators (Klatt 1980) with a nasal pole for nasals and nasal vowels; formant
// targets glide between phonemes (coarticulation), plosives start their vowel's formants at the place's locus.
// Noise: a parallel resonator per fricative place (s 5.5 kHz, sh 3 kHz, f broad, the uvular/velar fricative 1.4 kHz).
// Output: 16 kHz mono, 100 ms of silence before and 150 ms after the speech, peak-normalised to 0.5.
//
// Phoneme strings: tokens separated by spaces; `|` ends a word; a `'` before a vowel stresses it.
//   Vowels: a e i o u, ae (cat), E (bed), I (bit), O (thought / Hebrew-French o open), V (cut), @ (schwa), U (book),
//           o~ (French nasal on, "avion"), a~ (French an).
//   Plosives: p b t d k g, ? (glottal stop). Fricatives: f v s z S (sh) Z (zh) X (Hebrew ח / כ, uvular-velar) h.
//   Affricates: ts (Hebrew צ), tS (ch). Nasals: m n N (ng). Approximants: l j w, R (Hebrew/French uvular r), r (English r).
// Example: "a n 'i | R o ts 'e | S e t a ? a ts 'o R" (אני רוצה שתעצור).
//
// Portable; no allocation, no lock, no clock: all state is inside the object (about 40 KB). One thread at a time.
class FormantSynthesizer final {
public:
    static constexpr unsigned kSampleRate = 16000;
    static constexpr unsigned kMaxSegments = 256;  // Phonemes after expansion (a plosive is closure, burst, aspiration).
    static constexpr unsigned kMaxWords = 24;
    static constexpr unsigned kLeadSamples = 1600, kTailSamples = 2400;
    // Renders `phonemes` spoken by `voice` into `out`. Returns the samples written; 0 when the string has an unknown
    // token or more than kMaxSegments, or when `out` is too small (Samples() tells how many it needs). Word spans go to
    // `words` (up to `word_capacity`), their count to `word_count`.
    std::size_t Speak(std::string_view phonemes, const SynthVoice& voice, std::span<float> out, SynthWord* words = nullptr,
                      unsigned word_capacity = 0, unsigned* word_count = nullptr) noexcept;
    // Samples Speak would write (0 for an invalid string).
    std::size_t Samples(std::string_view phonemes, const SynthVoice& voice) noexcept;
    // Whether `token` is a phoneme of the notation.
    [[nodiscard]] static bool Known(std::string_view token) noexcept;
    struct Segment {
        float f[5], b[3];             // Formant targets (Hz, before formant_scale) and bandwidths.
        float voicing, aspiration, frication, fric_hz, fric_bw, nasal, stress;
        std::uint32_t samples;        // Duration.
        std::uint8_t word;            // Word index.
        bool vowel, transition_fast;  // A plosive's burst moves its formants at once.
    };
private:
    bool Parse(std::string_view phonemes, const SynthVoice& voice) noexcept;
    struct Resonator {
        float a = 1, b = 0, c = 0, y1 = 0, y2 = 0;
        void Set(float hz, float bandwidth) noexcept;
        float Run(float x) noexcept { const float y = a * x + b * y1 + c * y2; y2 = y1; y1 = y; return y; }
    };
    Segment segments_[kMaxSegments]{};
    unsigned count_ = 0, words_ = 0;
    std::size_t total_ = 0;
    // Schroeder reverberator: four combs and two all-passes (delays for 16 kHz).
    static constexpr unsigned kCombs = 4, kCombLength = 512, kAllpasses = 2, kAllpassLength = 256;
    float comb_[kCombs][kCombLength]{};
    float allpass_[kAllpasses][kAllpassLength]{};
};
}
