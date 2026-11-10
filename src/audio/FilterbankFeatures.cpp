#include "media/audio/FilterbankFeatures.hpp"
#include <cmath>
#include <cstring>

namespace media::audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kLowHz = 80, kHighHz = 7600;
double Mel(double hz) noexcept { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double Hz(double mel) noexcept { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }
// Edge `i` of the filters: filter c rises from edge c to edge c + 1 and falls to edge c + 2.
double Edge(unsigned i) noexcept {
    const double low = Mel(kLowHz), high = Mel(kHighHz);
    return Hz(low + (high - low) * i / (FilterbankFeatures::kChannels + 1));
}
}
float FilterbankFeatures::CentreHz(std::uint32_t channel) noexcept { return static_cast<float>(Edge(channel + 1)); }
FilterbankFeatures::FilterbankFeatures() noexcept {
    for (unsigned i = 0; i < kFrame; ++i) window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / kFrame));
    for (unsigned i = 0; i < kFft / 2; ++i) {
        cosine_[i] = static_cast<float>(std::cos(2.0 * kPi * i / kFft)); sine_[i] = static_cast<float>(std::sin(2.0 * kPi * i / kFft));
    }
    unsigned bits = 0; while ((1u << bits) < kFft) ++bits;
    for (unsigned i = 0; i < kFft; ++i) {
        unsigned r = 0; for (unsigned b = 0; b < bits; ++b) if (i & (1u << b)) r |= 1u << (bits - 1 - b);
        reversed_[i] = static_cast<std::uint16_t>(r);
    }
    for (unsigned k = 0; k <= kFft / 2; ++k) {
        const double hz = static_cast<double>(k) * kSampleRate / kFft;
        lower_[k] = static_cast<std::uint8_t>(kChannels); weight_[k] = 0;
        // Between edges i and i + 1 the bin belongs to filter i - 1 (falling) and filter i (rising).
        for (unsigned i = 0; i <= kChannels; ++i) {
            const double a = Edge(i), b = Edge(i + 1);
            if (hz < a || hz >= b) continue;
            const double rising = (hz - a) / (b - a);
            if (i == 0) { lower_[k] = 255; weight_[k] = static_cast<float>(rising); }          // Only filter 0, rising.
            else { lower_[k] = static_cast<std::uint8_t>(i - 1); weight_[k] = static_cast<float>(1.0 - rising); }
            break;
        }
    }
    Reset();
}
void FilterbankFeatures::Reset() noexcept {
    std::memset(history_, 0, sizeof(history_)); previous_ = 0; head_ = held_ = since_ = 0; frames_ = 0; dropped_ = 0;
}
std::uint32_t FilterbankFeatures::Push(std::span<const float> samples, float* out, std::uint32_t capacity) noexcept {
    std::uint32_t written = 0;
    for (const float sample : samples) {
        // The history is a ring of the newest kFrame samples: the one replaced is the one before the frame.
        previous_ = history_[head_];
        history_[head_] = std::isfinite(sample) ? sample : 0.0f;
        head_ = head_ + 1 == kFrame ? 0 : head_ + 1;
        if (held_ < kFrame) { if (++held_ < kFrame) continue; since_ = 0; }
        else if (++since_ < kHop) continue;
        else since_ = 0;
        ++frames_;
        if (out && written < capacity) { Analyse(out + static_cast<std::size_t>(written) * kChannels); ++written; }
        else ++dropped_;
    }
    return written;
}
void FilterbankFeatures::Analyse(float* out) noexcept {
    // Pre-emphasis, window, zero padding, in bit-reversed order for the in-place transform.
    float before = previous_;
    for (unsigned i = 0; i < kFft; ++i) {
        float value = 0;
        if (i < kFrame) { const float x = history_[head_ + i < kFrame ? head_ + i : head_ + i - kFrame]; value = (x - 0.97f * before) * window_[i]; before = x; }
        real_[reversed_[i]] = value; imaginary_[reversed_[i]] = 0;
    }
    for (unsigned size = 2; size <= kFft; size *= 2) {
        const unsigned half = size / 2, step = kFft / size;
        for (unsigned start = 0; start < kFft; start += size)
            for (unsigned k = 0; k < half; ++k) {
                const float c = cosine_[k * step], s = sine_[k * step];
                const unsigned a = start + k, b = a + half;
                const float re = real_[b] * c + imaginary_[b] * s, im = imaginary_[b] * c - real_[b] * s;
                real_[b] = real_[a] - re; imaginary_[b] = imaginary_[a] - im;
                real_[a] += re; imaginary_[a] += im;
            }
    }
    float energy[kChannels + 1]{};
    for (unsigned k = 0; k <= kFft / 2; ++k) {
        const float power = real_[k] * real_[k] + imaginary_[k] * imaginary_[k];
        if (lower_[k] == 255) energy[0] += weight_[k] * power;
        else if (lower_[k] < kChannels) { energy[lower_[k]] += weight_[k] * power; energy[lower_[k] + 1] += (1.0f - weight_[k]) * power; }
    }
    // The floor is far below any real signal (a -96 dBFS tone gives about 1e-5 here) and keeps silence finite.
    for (unsigned c = 0; c < kChannels; ++c) out[c] = std::log(energy[c] + 1e-7f);
}
}
