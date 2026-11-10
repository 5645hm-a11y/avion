#include "media/audio/SpectrumAnalyser.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::audio {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kLog2Window = 11;
static_assert((1u << kLog2Window) == SpectrumAnalyser::kWindow && SpectrumAnalyser::kWindow == 2 * SpectrumAnalyser::kBlock);
}
bool SpectrumAnalyser::Configure(core::LinearMemoryArena& arena, unsigned rate, unsigned channels) noexcept {
    if (history_ || rate < 8000 || rate > 192000 || !channels || channels > kMaxChannels || arena.IsSealed()) return false;
    auto* history = static_cast<float*>(arena.Allocate(sizeof(float) * kWindow));
    window_ = static_cast<float*>(arena.Allocate(sizeof(float) * kWindow));
    cosine_ = static_cast<float*>(arena.Allocate(sizeof(float) * kWindow / 2));
    sine_ = static_cast<float*>(arena.Allocate(sizeof(float) * kWindow / 2));
    real_ = static_cast<float*>(arena.Allocate(sizeof(float) * kWindow));
    imaginary_ = static_cast<float*>(arena.Allocate(sizeof(float) * kWindow));
    reversed_ = static_cast<std::uint16_t*>(arena.Allocate(sizeof(std::uint16_t) * kWindow, alignof(std::uint16_t)));
    if (!history || !window_ || !cosine_ || !sine_ || !real_ || !imaginary_ || !reversed_) return false;
    for (unsigned n = 0; n < kWindow; ++n) {
        window_[n] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * n / kWindow)); // Periodic Hann.
        unsigned r = 0; for (unsigned bit = 0; bit < kLog2Window; ++bit) r |= ((n >> bit) & 1u) << (kLog2Window - 1 - bit);
        reversed_[n] = static_cast<std::uint16_t>(r);
    }
    for (unsigned k = 0; k < kWindow / 2; ++k) {
        cosine_[k] = static_cast<float>(std::cos(2 * kPi * k / kWindow)); sine_[k] = static_cast<float>(-std::sin(2 * kPi * k / kWindow));
    }
    rate_ = rate; channels_ = channels;
    const double top = (std::min)(16000.0, 0.95 * rate / 2), bottom = 40.0;
    for (unsigned b = 0; b <= AudioPicture::kBands; ++b) edges_hz_[b] = bottom * std::pow(top / bottom, static_cast<double>(b) / AudioPicture::kBands);
    const double bin = static_cast<double>(rate) / kWindow;
    for (unsigned b = 0; b < AudioPicture::kBands; ++b) {
        // Bin k is at k x bin: those in [f_b, f_(b+1)) are ceil(f_b / bin) .. ceil(f_(b+1) / bin) - 1.
        auto from = static_cast<unsigned>(std::ceil(edges_hz_[b] / bin)), to = static_cast<unsigned>(std::ceil(edges_hz_[b + 1] / bin));
        widened_[b] = to < from + 3;
        if (widened_[b]) { const auto centre = static_cast<unsigned>(std::lround(BandCentre(b) / bin)); from = centre - 1; to = centre + 2; }
        from_[b] = static_cast<std::uint16_t>(from); to_[b] = static_cast<std::uint16_t>((std::min)(to, kWindow / 2));
    }
    history_ = history;
    Reset();
    return true;
}
void SpectrumAnalyser::Reset() noexcept {
    filled_ = 0; ready_ = false;
    if (history_) std::memset(history_, 0, sizeof(float) * kWindow);
}
double SpectrumAnalyser::BandCentre(unsigned b) const noexcept {
    return b < AudioPicture::kBands ? std::sqrt(edges_hz_[b] * edges_hz_[b + 1]) : 0.0;
}
unsigned SpectrumAnalyser::BandOf(double hz) const noexcept {
    for (unsigned b = 0; b < AudioPicture::kBands; ++b) if (hz >= edges_hz_[b] && hz < edges_hz_[b + 1]) return b;
    return AudioPicture::kBands;
}
unsigned SpectrumAnalyser::Feed(const float* pcm, unsigned frames, std::int64_t media, std::uint32_t epoch) noexcept {
    if (!history_ || ready_ || !pcm) return 0; // The completed picture is taken first.
    if (!filled_) { picture_.media_100ns = media; picture_.epoch = epoch; }
    const unsigned n = (std::min)(frames, kBlock - filled_);
    const float scale = 1.0f / static_cast<float>(channels_);
    float* block = history_ + kBlock;
    for (unsigned f = 0; f < n; ++f) {
        float sum = 0; for (unsigned c = 0; c < channels_; ++c) sum += pcm[static_cast<std::size_t>(f) * channels_ + c];
        block[filled_++] = sum * scale;
    }
    if (filled_ == kBlock) {
        Analyse(); filled_ = 0; ready_ = true;
        std::memcpy(history_, block, sizeof(float) * kBlock); // This block is the next one's predecessor.
    }
    return n;
}
void SpectrumAnalyser::Analyse() noexcept {
    auto& p = picture_;
    const float* block = history_ + kBlock;
    // Level, peak and the waveform envelope from the block as heard.
    double square = 0; float peak = 0;
    for (unsigned c = 0; c < AudioPicture::kColumns; ++c) {
        const unsigned from = c * kBlock / AudioPicture::kColumns, to = (c + 1) * kBlock / AudioPicture::kColumns;
        float low = block[from], high = block[from];
        for (unsigned f = from; f < to; ++f) {
            const float v = block[f]; low = (std::min)(low, v); high = (std::max)(high, v);
            square += static_cast<double>(v) * v; peak = (std::max)(peak, std::fabs(v));
        }
        p.low[c] = static_cast<std::int8_t>(std::lround((std::clamp)(low, -1.0f, 1.0f) * 127.0f));
        p.high[c] = static_cast<std::int8_t>(std::lround((std::clamp)(high, -1.0f, 1.0f) * 127.0f));
    }
    const double rms = std::sqrt(square / kBlock);
    const double rms_db = rms > 1e-9 ? 20.0 * std::log10(rms) : -200.0;
    p.level = static_cast<float>((std::clamp)((rms_db + 60.0) / 60.0, 0.0, 1.0)); p.peak = (std::min)(peak, 1.0f);
    // The windowed history, in bit-reversed order, then the iterative radix-2 butterflies.
    for (unsigned n = 0; n < kWindow; ++n) { real_[reversed_[n]] = history_[n] * window_[n]; imaginary_[reversed_[n]] = 0; }
    for (unsigned size = 2; size <= kWindow; size <<= 1) {
        const unsigned half = size / 2, step = kWindow / size;
        for (unsigned start = 0; start < kWindow; start += size)
            for (unsigned k = 0; k < half; ++k) {
                const float wr = cosine_[k * step], wi = sine_[k * step];
                const unsigned a = start + k, b = a + half;
                const float tr = real_[b] * wr - imaginary_[b] * wi, ti = real_[b] * wi + imaginary_[b] * wr;
                real_[b] = real_[a] - tr; imaginary_[b] = imaginary_[a] - ti;
                real_[a] += tr; imaginary_[a] += ti;
            }
    }
    constexpr double quarter = kWindow / 4.0, reference = 1.5 * quarter * quarter; // A full-scale sine's band energy.
    for (unsigned b = 0; b < AudioPicture::kBands; ++b) {
        double energy = 0;
        for (unsigned k = from_[b]; k < to_[b]; ++k) energy += static_cast<double>(real_[k]) * real_[k] + static_cast<double>(imaginary_[k]) * imaginary_[k];
        const double db = energy > 0 ? 10.0 * std::log10(energy / reference) : -200.0;
        p.bands[b] = static_cast<std::uint8_t>(std::lround((std::clamp)((db + 72.0) / 78.0, 0.0, 1.0) * 255.0));
    }
}
}
