#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <cstdint>

namespace media::audio {
// Converts one audio track's decoded PCM to the format the output path was opened for (docs/tracks.md): the
// channels are remixed as they arrive, then the sample rate is changed. Used when a track of another rate or layout
// replaces the playing one: the endpoint, the DSP and the clock keep the format they were opened with.
//
// Channels (interleaved, WAVEFORMATEXTENSIBLE order: 5.1 = FL FR FC LFE BL BR):
//   same count: copied; mono -> stereo: both sides; mono -> 5.1: the centre; stereo -> mono: the mean;
//   stereo -> 5.1: the front pair; 5.1 or 7.1 -> stereo: DspProcessor::DownmixWeights, the engine's own fold-down
//   (ITU-R BS.775 shape at scale 0.5, LFE left out), so a 5.1 track sounds the same whether it started the
//   playback on a stereo path or was switched to; 5.1 or 7.1 -> mono: the mean of that downmix's two sides.
//   Anything else is refused.
// Rate: band-limited interpolation with an exact rational step. Output frame n lies at input position
//   p0 + n * in_rate / out_rate (input frames since Reset), kept as an integer and a remainder in units of
//   1 / (out_rate / gcd): there is no drift, ever, and no interpolation of the kernel either. Each of those positions
//   has its own row of a Kaiser-windowed sinc (beta 8, 48 zero crossings either side at the lower of the two rates'
//   Nyquist, cut at 0.945 of it so that the stop band begins at that Nyquist frequency), normalised to unity gain at DC, built at Configure.
//   The filter is centred on the output frame's position: no delay. An output frame needs `Lookahead()` input
//   frames past its position, so a converter holds that much input back until more arrives (or Finish pads it).
//   Positions before the first input frame read silence.
// Equal rates: no filter at all, every frame is passed on as soon as it arrives (channels still remixed).
// Storage comes from the arena at Configure; after that nothing allocates, locks or reads a clock. One owner thread.
class PcmConverter final {
public:
    static constexpr unsigned kMaxChannels = 8;
    static constexpr unsigned kMaxPhases = 1024;    // out_rate / gcd(in_rate, out_rate): every AAC rate pair fits.
    static constexpr unsigned kZeroCrossings = 48;  // Kernel half-width at the cut-off rate.
    // Setup only. Rates 8-192 kHz at most 4:1 apart; `capacity` input frames can be held (Room()).
    [[nodiscard]] bool Configure(core::LinearMemoryArena& arena, unsigned in_rate, unsigned in_channels, unsigned out_rate,
                                 unsigned out_channels, unsigned capacity = 4096) noexcept;
    [[nodiscard]] static bool Supported(unsigned in_rate, unsigned in_channels, unsigned out_rate, unsigned out_channels) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return buffer_ != nullptr; }
    // Same rate and same channels: a copy (the caller can skip the converter).
    [[nodiscard]] bool Identity() const noexcept { return in_rate_ == out_rate_ && in_channels_ == out_channels_; }
    // A new stream: everything held is dropped. The first output frame lies `phase` input frames (0 <= phase < 1)
    // after the first input frame pushed next, rounded to the nearest representable position.
    void Reset(double phase = 0) noexcept;
    [[nodiscard]] unsigned Room() const noexcept;      // Input frames Push can take now.
    // Takes `frames` interleaved input frames (in_channels each); at most Room(). Returns the frames taken.
    unsigned Push(const float* interleaved, unsigned frames) noexcept;
    // End of the stream: pads with silence so that every output frame up to the last input frame can be pulled.
    // Nothing more can be pushed until Reset.
    void Finish() noexcept;
    [[nodiscard]] bool Finished() const noexcept { return finished_; }
    [[nodiscard]] unsigned Available() const noexcept; // Output frames Pull can produce now.
    // Writes `frames` (at most Available()) interleaved output frames (out_channels each). Returns the frames written.
    unsigned Pull(float* interleaved, unsigned frames) noexcept;
    [[nodiscard]] unsigned Lookahead() const noexcept { return after_; }
    [[nodiscard]] unsigned Taps() const noexcept { return before_ + 1 + after_; }
    [[nodiscard]] unsigned InRate() const noexcept { return in_rate_; }
    [[nodiscard]] unsigned OutRate() const noexcept { return out_rate_; }
    [[nodiscard]] unsigned InChannels() const noexcept { return in_channels_; }
    [[nodiscard]] unsigned OutChannels() const noexcept { return out_channels_; }
    // The remix weight of input channel `from` in output channel `to`.
    [[nodiscard]] float Weight(unsigned to, unsigned from) const noexcept { return to < kMaxChannels && from < kMaxChannels ? mix_[to][from] : 0.0f; }
    // Input frames pushed and output frames pulled since Reset.
    [[nodiscard]] std::uint64_t Pushed() const noexcept { return pushed_; }
    [[nodiscard]] std::uint64_t Pulled() const noexcept { return pulled_; }
private:
    static bool Mix(unsigned in_channels, unsigned out_channels, float (&mix)[kMaxChannels][kMaxChannels]) noexcept;
    void Compact() noexcept;
    unsigned in_rate_ = 0, out_rate_ = 0, in_channels_ = 0, out_channels_ = 0;
    unsigned step_ = 1, phases_ = 1;   // Position step in input frames: step_ / phases_ (both divided by the gcd).
    unsigned before_ = 0, after_ = 0;  // Input frames read before and after an output frame's position.
    unsigned capacity_ = 0;            // Input frames held at most, beyond the history the filter needs.
    float mix_[kMaxChannels][kMaxChannels]{};
    const float* table_ = nullptr;     // phases_ rows of Taps() coefficients.
    float* buffer_ = nullptr;          // (before_ + capacity_ + after_ + 1) frames of out_channels, remixed input.
    // buffer_ frame 0 is input frame first_ (negative: the silence before the stream); `held_` frames are in it.
    std::int64_t first_ = 0;
    unsigned held_ = 0;
    std::int64_t position_ = 0;        // Integer input frame of the next output frame,
    unsigned remainder_ = 0;           // and its fraction, in units of 1 / phases_.
    std::uint64_t pushed_ = 0, pulled_ = 0;
    std::int64_t end_ = 0;             // After Finish: the input frame past the last real one.
    bool finished_ = false;
};
}
