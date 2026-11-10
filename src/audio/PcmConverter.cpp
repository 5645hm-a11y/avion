#include "media/audio/PcmConverter.hpp"
#include "media/audio/DspProcessor.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace media::audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kBeta = 8.0;     // Kaiser window: about 80 dB of stop-band.
// Of the lower rate's Nyquist frequency: with 48 zero crossings the window's transition band (about 0.106 of Nyquist
// wide) then ends at Nyquist itself, so nothing above it folds back into the band.
constexpr double kCutoff = 0.945;
// Modified Bessel function of the first kind, order 0 (power series; converges fast for the window's arguments).
double BesselI0(double x) noexcept {
    double sum = 1, term = 1;
    for (unsigned k = 1; k < 64; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; if (term < sum * 1e-17) break; }
    return sum;
}
bool RateOk(unsigned rate) noexcept { return rate >= 8000 && rate <= 192000; }
}

bool PcmConverter::Mix(unsigned in, unsigned out, float (&mix)[kMaxChannels][kMaxChannels]) noexcept {
    for (auto& row : mix) for (auto& w : row) w = 0;
    if (!in || !out || in > kMaxChannels || out > kMaxChannels) return false;
    if (in == out) { for (unsigned c = 0; c < in; ++c) mix[c][c] = 1; return true; }
    SpeakerLayout layout{};
    if (in == 1 && out == 2) { mix[0][0] = mix[1][0] = 1; return true; }
    if (in == 1 && out == 6) { mix[2][0] = 1; return true; }   // The centre.
    if (in == 2 && out == 1) { mix[0][0] = mix[0][1] = 0.5f; return true; }
    if (in == 2 && out == 6) { mix[0][0] = mix[1][1] = 1; return true; } // The front pair.
    if ((in == 6 || in == 8) && LayoutFor(in, layout) && (out == 2 || out == 1)) {
        for (unsigned c = 0; c < in; ++c) {
            float left = 0, right = 0; DspProcessor::DownmixWeights(in, c, left, right);
            if (out == 2) { mix[0][c] = left; mix[1][c] = right; } else mix[0][c] = 0.5f * (left + right);
        }
        return true;
    }
    return false;
}
bool PcmConverter::Supported(unsigned in_rate, unsigned in_channels, unsigned out_rate, unsigned out_channels) noexcept {
    float mix[kMaxChannels][kMaxChannels];
    if (!RateOk(in_rate) || !RateOk(out_rate) || in_rate > 4u * out_rate || out_rate > 4u * in_rate || !Mix(in_channels, out_channels, mix)) return false;
    return out_rate / std::gcd(in_rate, out_rate) <= kMaxPhases;
}
bool PcmConverter::Configure(core::LinearMemoryArena& arena, unsigned in_rate, unsigned in_channels, unsigned out_rate, unsigned out_channels, unsigned capacity) noexcept {
    if (buffer_ || !capacity || capacity > (1u << 20) || !Supported(in_rate, in_channels, out_rate, out_channels)) return false;
    (void)Mix(in_channels, out_channels, mix_);
    const unsigned divisor = std::gcd(in_rate, out_rate);
    const unsigned phases = out_rate / divisor, step = in_rate / divisor;
    unsigned before = 0, after = 0;
    double width = 0, cutoff = 0; // Kernel half-width and cut-off, both in input frames / cycles per input frame.
    if (in_rate != out_rate) {
        const double ratio = (std::min)(1.0, static_cast<double>(out_rate) / in_rate);
        width = kZeroCrossings / ratio;
        cutoff = 0.5 * kCutoff * ratio;
        after = static_cast<unsigned>(std::ceil(width)); before = after - 1;
    }
    const unsigned taps = before + 1 + after;
    auto* table = static_cast<float*>(arena.Allocate(sizeof(float) * static_cast<std::size_t>(phases) * taps));
    auto* buffer = static_cast<float*>(arena.Allocate(sizeof(float) * static_cast<std::size_t>(before + capacity + after + 1) * out_channels));
    if (!table || !buffer) return false;
    if (in_rate == out_rate) table[0] = 1;
    else {
        const double norm = BesselI0(kBeta);
        for (unsigned r = 0; r < phases; ++r) {
            // Row r: an output frame r / phases of an input frame after an integer position; tap t reads the input
            // frame t - before from that position.
            const double fraction = static_cast<double>(r) / phases;
            double sum = 0; double row[2 * (kZeroCrossings * 4 + 1)];
            for (unsigned t = 0; t < taps; ++t) {
                const double x = static_cast<double>(t) - before - fraction;
                const double u = x / width;
                if (u <= -1 || u >= 1) { row[t] = 0; continue; }
                const double arg = 2 * cutoff * x;
                const double sinc = arg == 0 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
                row[t] = 2 * cutoff * sinc * BesselI0(kBeta * std::sqrt(1 - u * u)) / norm;
                sum += row[t];
            }
            for (unsigned t = 0; t < taps; ++t) table[static_cast<std::size_t>(r) * taps + t] = static_cast<float>(row[t] / sum);
        }
    }
    in_rate_ = in_rate; out_rate_ = out_rate; in_channels_ = in_channels; out_channels_ = out_channels;
    phases_ = phases; step_ = step; before_ = before; after_ = after; capacity_ = capacity;
    table_ = table; buffer_ = buffer;
    Reset();
    return true;
}
void PcmConverter::Reset(double phase) noexcept {
    if (!buffer_) return;
    // The silence before the stream: the frames the first outputs read before their position.
    std::memset(buffer_, 0, sizeof(float) * static_cast<std::size_t>(before_) * out_channels_);
    first_ = -static_cast<std::int64_t>(before_); held_ = before_;
    position_ = 0; remainder_ = 0; pushed_ = pulled_ = 0; finished_ = false;
    if (phase > 0 && phase < 1) {
        remainder_ = static_cast<unsigned>(std::lround(phase * phases_));
        if (remainder_ >= phases_) { remainder_ -= phases_; ++position_; }
    }
}
void PcmConverter::Compact() noexcept {
    // Frames before the next output's first tap are read by nothing any more.
    const std::int64_t needed = position_ - static_cast<std::int64_t>(before_);
    if (needed <= first_) return;
    const auto drop = static_cast<unsigned>((std::min)(needed - first_, static_cast<std::int64_t>(held_)));
    if (!drop) return;
    std::memmove(buffer_, buffer_ + static_cast<std::size_t>(drop) * out_channels_, sizeof(float) * static_cast<std::size_t>(held_ - drop) * out_channels_);
    held_ -= drop; first_ += drop;
}
unsigned PcmConverter::Room() const noexcept {
    if (!buffer_ || finished_) return 0;
    const std::int64_t needed = position_ - static_cast<std::int64_t>(before_);
    const auto droppable = needed > first_ ? static_cast<unsigned>((std::min)(needed - first_, static_cast<std::int64_t>(held_))) : 0u;
    const unsigned total = before_ + capacity_; // The last after_ + 1 frames are kept for Finish's padding.
    const unsigned kept = held_ - droppable;
    return kept < total ? total - kept : 0;
}
unsigned PcmConverter::Push(const float* in, unsigned frames) noexcept {
    if (!buffer_ || !in || finished_) return 0;
    Compact();
    frames = (std::min)(frames, Room());
    float* out = buffer_ + static_cast<std::size_t>(held_) * out_channels_;
    for (unsigned f = 0; f < frames; ++f, in += in_channels_, out += out_channels_)
        for (unsigned o = 0; o < out_channels_; ++o) {
            float sum = 0;
            for (unsigned i = 0; i < in_channels_; ++i) sum += mix_[o][i] * in[i];
            out[o] = sum;
        }
    held_ += frames; pushed_ += frames;
    return frames;
}
void PcmConverter::Finish() noexcept {
    if (!buffer_ || finished_) return;
    Compact();
    const unsigned total = before_ + capacity_ + after_ + 1;
    const unsigned pad = (std::min)(after_, total - held_);
    std::memset(buffer_ + static_cast<std::size_t>(held_) * out_channels_, 0, sizeof(float) * static_cast<std::size_t>(pad) * out_channels_);
    held_ += pad; finished_ = true;
    // Past the last real input frame there is nothing to produce: the padding only completes the frames before it.
    end_ = first_ + held_ - pad; // Exclusive end of the real input.
}
unsigned PcmConverter::Available() const noexcept {
    if (!buffer_) return 0;
    // The last input frame held; an output frame needs every frame up to its position plus after_.
    std::int64_t limit = first_ + static_cast<std::int64_t>(held_) - 1 - static_cast<std::int64_t>(after_);
    if (finished_) limit = (std::min)(limit, end_ - 1); // Output only up to the last real input frame.
    const std::int64_t span = limit - position_ + 1;
    if (span <= 0) return 0;
    const std::int64_t count = (span * phases_ - remainder_ + step_ - 1) / step_;
    return count > 0 ? static_cast<unsigned>((std::min)(count, std::int64_t{1} << 30)) : 0u;
}
unsigned PcmConverter::Pull(float* out, unsigned frames) noexcept {
    if (!buffer_ || !out) return 0;
    frames = (std::min)(frames, Available());
    const unsigned taps = Taps();
    for (unsigned n = 0; n < frames; ++n, out += out_channels_) {
        const auto start = static_cast<std::size_t>(position_ - static_cast<std::int64_t>(before_) - first_);
        const float* row = table_ + static_cast<std::size_t>(remainder_) * taps;
        const float* x = buffer_ + start * out_channels_;
        for (unsigned c = 0; c < out_channels_; ++c) {
            float sum = 0;
            for (unsigned t = 0; t < taps; ++t) sum += row[t] * x[static_cast<std::size_t>(t) * out_channels_ + c];
            out[c] = sum;
        }
        remainder_ += step_;
        position_ += remainder_ / phases_; remainder_ %= phases_;
    }
    pulled_ += frames;
    return frames;
}
}
