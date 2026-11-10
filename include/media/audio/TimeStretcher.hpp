#pragma once
#include "media/core/linear_memory_arena.hpp"
#include <cstdint>

namespace media::audio {
// Pitch-preserving playback speed (docs/audio-dsp.md, "Playback speed"): WSOLA, waveform-similarity overlap-add.
//
// Output is made in hops of Hs = 12 ms. Each hop overlap-adds one Hann-windowed frame of W = 2 Hs input frames (periodic
// Hann: two half-windows sum to exactly 1). Frame k is read near its analysis position a_k = a_(k-1) + Ha, where
// Ha = speed x Hs, within +-8 ms. The offset chosen is the one whose first half best continues the previous frame's
// second half: normalised cross-correlation of the channels' mean, coarse at every 4th sample, then refined to the
// sample. The analysis positions never take the offsets in, so speed is exact over any span longer than a hop
// (input = speed x output, within +-8 ms + 1 frame at any instant) and pitch is untouched: frames are copied, never
// resampled.
//
// At 1x the stretcher copies its input sample for sample (Passing()). A change of speed away from 1x starts the first
// frame from the input it was passing on (its overlap holds that input's next half hop, as a natural continuation would),
// so nothing clicks. A change back to 1x ends at the next frame boundary: its second half and its natural continuation
// sum to the input itself, so passing resumes exactly there, with nothing to fade.
//
// Positions: input frames since Reset (Pushed), output frames since Reset (Pulled). Change() reports where the speed in
// force last changed, as an output frame and the input position it stands for: the clock maps one onto the other with
// the speed (piecewise linear).
// Storage comes from the arena at Configure; after that nothing allocates, locks or reads a clock. One owner thread.
class TimeStretcher final {
public:
    static constexpr unsigned kMaxChannels = 8;
    static constexpr float kMinSpeed = 0.25f, kMaxSpeed = 4.0f; // 4x since API 16 (was 2).
    // Setup only. 8-192 kHz; `capacity` input frames can be held (Room()), at least 4 frames' worth. At 4x a hop reads
    // Ha + W + 2 x 8 ms = 88 ms of input: 16,896 frames at 192 kHz, hence 32,768 (16,384 sufficed for 2x).
    [[nodiscard]] bool Configure(core::LinearMemoryArena&, unsigned rate, unsigned channels, unsigned capacity = 32768) noexcept;
    [[nodiscard]] bool Configured() const noexcept { return input_ != nullptr; }
    // A new stream (a seek): everything held is dropped; the speed asked for stays and applies from the first frame.
    void Reset() noexcept;
    // kMinSpeed to kMaxSpeed (false otherwise). Applies from the next hop made (1x: at the next frame boundary).
    bool SetSpeed(float speed) noexcept;
    [[nodiscard]] float Speed() const noexcept { return speed_; }        // Asked for.
    [[nodiscard]] float SpeedInForce() const noexcept { return passing_ ? 1.0f : static_cast<float>(hop_in_ / hop_); }
    [[nodiscard]] bool Passing() const noexcept { return passing_; }    // Copying at 1x.
    [[nodiscard]] unsigned Room() const noexcept;                        // Input frames Push can take now.
    unsigned Push(const float* interleaved, unsigned frames) noexcept;  // At most Room(); returns the frames taken.
    [[nodiscard]] unsigned Available() const noexcept;                   // Output frames Pull can make now.
    unsigned Pull(float* interleaved, unsigned frames) noexcept;        // At most Available(); returns the frames made.
    [[nodiscard]] unsigned Held() const noexcept { return held_; }       // Input frames not yet consumed.
    [[nodiscard]] std::uint64_t Pushed() const noexcept { return pushed_; }
    [[nodiscard]] std::uint64_t Pulled() const noexcept { return pulled_; }
    struct Change { std::uint64_t output = 0; double input = 0; float speed = 1; };
    [[nodiscard]] const Change& LastChange() const noexcept { return change_; }
    [[nodiscard]] unsigned Hop() const noexcept { return hop_; }
    [[nodiscard]] unsigned Search() const noexcept { return search_; }
private:
    void MakeHop() noexcept;                 // One output hop into out_ (stretching).
    long long Best(long long target) noexcept; // The analysis frame's start near `target`.
    [[nodiscard]] bool CanMakeHop() const noexcept;
    float Mono(long long index) const noexcept; // The channels' mean at an absolute input index.
    const float* Frame(long long index) const noexcept { return input_ + (index - base_) * channels_; }
    void Compact() noexcept;
    unsigned rate_ = 0, channels_ = 0, capacity_ = 0, hop_ = 0, window_ = 0, search_ = 0;
    float* input_ = nullptr;   // capacity_ frames; frame i is absolute index base_ + i.
    float* overlap_ = nullptr; // hop_ frames: the previous frame's windowed second half.
    float* out_ = nullptr;     // hop_ frames made and not yet pulled.
    float* region_ = nullptr;  // Best's scratch: the channels' mean over the candidates (2 search_ + hop_ + 1 frames)...
    float* natural_ = nullptr; // ...and over the natural continuation (hop_ frames), each computed once per hop.
    float* window_table_ = nullptr; // window_ coefficients (periodic Hann).
    long long base_ = 0;       // Absolute index of input_[0].
    unsigned held_ = 0;        // Frames in input_ from `read_` (relative) on.
    long long read_ = 0;       // Passing: the next input index to copy. Stretching: the oldest index still needed.
    long long previous_ = 0;   // Stretching: the previous frame's start.
    double analysis_ = 0;      // Stretching: the previous frame's analysis position.
    double hop_in_ = 0;        // Stretching: Ha.
    unsigned out_count_ = 0, out_read_ = 0;
    std::uint64_t pushed_ = 0, pulled_ = 0;
    float speed_ = 1;
    bool passing_ = true, ending_ = false; // ending_: 1x asked for while stretching; passes on after the next hop.
    Change change_{};
};
}
