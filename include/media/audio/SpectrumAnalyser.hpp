#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <cstdint>

namespace media::audio {
// What the music mode draws while one block of PCM is heard (docs/audio-dsp.md, "Music mode"): 32 band energies, a
// waveform envelope across the block, its level and peak. Trivial (an SpscRingBuffer element, copied whole: its slots
// are cache-line aligned) and four cache lines long.
struct AudioPicture {
    static constexpr unsigned kBands = 32, kColumns = 80;
    std::int64_t media_100ns;   // Media time of the block's first frame (the spectrum window's centre).
    std::uint32_t epoch;        // The playback epoch it belongs to.
    float level;                // RMS of the block's channel mean: -60 dBFS..0 dBFS as 0..1.
    float peak;                 // Largest |sample| of the mean, 0..1.
    std::uint8_t bands[kBands]; // Band energy: -72 dB..+6 dB as 0..255 (a full-scale sine, 0 dB: 235).
    std::int8_t low[kColumns], high[kColumns]; // The mean's least and greatest sample per column of the block, x127.
    std::uint8_t reserved[44];
};
static_assert(sizeof(AudioPicture) == 256);
// The analysis behind AudioPicture, per block of 1024 frames of the channels' mean. The spectrum: the previous block and
// this one (2048 frames, centred on this block's first frame), Hann-windowed into a 2048-point radix-2 FFT. Band b
// covers [f_b, f_(b+1)), f log-spaced from 40 Hz to 16 kHz (or 0.95 x Nyquist below 33.7 kHz); its energy is the sum
// of the power of the bins whose frequency falls in it, or of the three bins around its centre when it holds fewer,
// normalised so that a full-scale sine reads 0 dB in its band (the Hann window spreads an on-bin sine over three bins,
// whose powers sum to 1.5 (N/4)^2), with 6 dB of headroom. The level, peak and waveform envelope are this block's own.
// Storage (window, twiddles, bit reversal, history and transform) comes from the arena at Configure; after that nothing
// allocates, locks or reads a clock. One owner thread.
class SpectrumAnalyser final {
public:
    static constexpr unsigned kBlock = 1024, kWindow = 2048, kMaxChannels = 8;
    [[nodiscard]] bool Configure(core::LinearMemoryArena&, unsigned rate, unsigned channels) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return history_ != nullptr; }
    void Reset() noexcept; // A new stream: the partial block and the previous one are dropped (silence before).
    // Takes interleaved frames until the block in progress is complete (then Ready()); returns the frames taken (none
    // while a completed picture waits to be taken). `media`: the media time of pcm's first frame; a block is stamped
    // with that of its own first frame.
    unsigned Feed(const float* interleaved, unsigned frames, std::int64_t media, std::uint32_t epoch) noexcept;
    [[nodiscard]] bool Ready() const noexcept { return ready_; }
    const AudioPicture& Take() noexcept { ready_ = false; return picture_; } // The completed block's picture.
    [[nodiscard]] unsigned BandOf(double hz) const noexcept;               // The band a frequency falls in (kBands: none).
    [[nodiscard]] double BandEdge(unsigned b) const noexcept { return b <= AudioPicture::kBands ? edges_hz_[b] : 0.0; }
    [[nodiscard]] double BandCentre(unsigned b) const noexcept; // Geometric.
    [[nodiscard]] bool Resolved(unsigned b) const noexcept { return b < AudioPicture::kBands && !widened_[b]; } // Three bins of its own or more.
private:
    void Analyse() noexcept;
    unsigned rate_ = 0, channels_ = 0, filled_ = 0;
    bool ready_ = false;
    float* history_ = nullptr;        // kWindow frames of the mean: the previous block, then this one.
    float* window_ = nullptr;         // Periodic Hann.
    float* cosine_ = nullptr; float* sine_ = nullptr; // kWindow / 2 twiddles.
    float* real_ = nullptr; float* imaginary_ = nullptr;
    std::uint16_t* reversed_ = nullptr;
    std::uint16_t from_[AudioPicture::kBands]{}, to_[AudioPicture::kBands]{}; // Bins summed, [from, to).
    bool widened_[AudioPicture::kBands]{};
    double edges_hz_[AudioPicture::kBands + 1]{};
    AudioPicture picture_{};
};
}
