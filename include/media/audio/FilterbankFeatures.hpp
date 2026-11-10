#pragma once
#include <cstdint>
#include <span>

namespace media::audio {
// Log mel-filterbank energies of 16 kHz mono speech: the acoustic features the voice path matches on
// (docs/voice.md). Streaming and deterministic: the same samples give the same vectors however they are split
// between calls. Fixed storage inside the object (about 9 KiB); no allocation, no locks; one owner thread.
//
//   samples -> pre-emphasis (0.97) -> 25 ms Hann window every 10 ms -> 512-point FFT (radix 2, in place)
//           -> power spectrum -> 24 triangular filters equally spaced in mel from 80 Hz to 7.6 kHz -> natural log
//
// A vector is produced for every 10 ms of input once the first 25 ms have arrived.
class FilterbankFeatures final {
public:
    static constexpr std::uint32_t kSampleRate = 16000, kFrame = 400, kHop = 160, kFft = 512, kChannels = 24;
    FilterbankFeatures() noexcept;
    // Forgets the samples held (a new utterance).
    void Reset() noexcept;
    // Feeds samples and writes one vector of kChannels values per completed frame to `out`, which holds `capacity`
    // vectors. Returns the vectors written; frames that did not fit are counted in Dropped(). Non-finite samples
    // count as zero.
    std::uint32_t Push(std::span<const float> samples, float* out, std::uint32_t capacity) noexcept;
    [[nodiscard]] std::uint64_t Frames() const noexcept { return frames_; }   // Produced since Reset, dropped ones included.
    [[nodiscard]] std::uint32_t Dropped() const noexcept { return dropped_; }
    // The centre frequency of a channel, in Hz.
    [[nodiscard]] static float CentreHz(std::uint32_t channel) noexcept;
private:
    void Analyse(float* out) noexcept;
    float window_[kFrame];
    float cosine_[kFft / 2], sine_[kFft / 2];
    std::uint16_t reversed_[kFft];
    // Each FFT bin feeds at most two neighbouring filters: `lower_[k]` (kChannels: none) with weight `weight_[k]`, and
    // the next one with the rest.
    std::uint8_t lower_[kFft / 2 + 1];
    float weight_[kFft / 2 + 1];
    float history_[kFrame];   // The newest kFrame samples, a ring whose oldest is at head_.
    float real_[kFft], imaginary_[kFft];
    float previous_ = 0;      // The sample before the frame, for the pre-emphasis of its first sample.
    std::uint32_t head_ = 0, held_ = 0, since_ = 0;
    std::uint64_t frames_ = 0;
    std::uint32_t dropped_ = 0;
};
}
