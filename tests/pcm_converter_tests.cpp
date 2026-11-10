// audio::PcmConverter (docs/tracks.md, "Changing the audio track while playing"): channel remix and rate change for a
// track of another format than the output path's. Portable; MSVC and GCC (-O2, ASan/UBSan).
#include "media/audio/PcmConverter.hpp"
#include "media/audio/DspProcessor.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using media::audio::PcmConverter;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 40) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
media::core::LinearMemoryArena& Arena() { static media::core::LinearMemoryArena arena; static const bool ready = arena.Initialize(64u << 20); (void)ready; return arena; }
// Converts `input` (in_channels interleaved) in one go, Finish included.
std::vector<float> Convert(PcmConverter& c, const std::vector<float>& input, double phase = 0) {
    c.Reset(phase);
    std::vector<float> out;
    const unsigned in_frames = static_cast<unsigned>(input.size() / c.InChannels());
    unsigned at = 0;
    std::vector<float> block(4096 * static_cast<std::size_t>(c.OutChannels()));
    for (;;) {
        at += c.Push(input.data() + static_cast<std::size_t>(at) * c.InChannels(), in_frames - at);
        if (at == in_frames) c.Finish();
        const unsigned n = c.Pull(block.data(), 4096);
        out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n) * c.OutChannels());
        if (!n && c.Finished()) break;
    }
    return out;
}
// Least-squares amplitude and phase of a tone of known frequency over [from, to) of a mono signal, and what is left.
struct Fit { double amplitude = 0, phase = 0, residual_db = 0; };
Fit FitTone(const std::vector<float>& x, double hz, double rate, std::size_t from, std::size_t to) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (std::size_t n = from; n < to; ++n) {
        const double s = std::sin(2 * kPi * hz * n / rate), c = std::cos(2 * kPi * hz * n / rate);
        ss += s * s; cc += c * c; sc += s * c; xs += x[n] * s; xc += x[n] * c;
    }
    const double det = ss * cc - sc * sc, a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double signal = 0, error = 0;
    for (std::size_t n = from; n < to; ++n) {
        const double model = a * std::sin(2 * kPi * hz * n / rate) + b * std::cos(2 * kPi * hz * n / rate);
        signal += model * model; error += (x[n] - model) * (x[n] - model);
    }
    return {std::sqrt(a * a + b * b), std::atan2(b, a), 10 * std::log10(error / signal + 1e-30)};
}
std::vector<float> Tone(unsigned frames, double hz, double rate, float amplitude = 0.5f) {
    std::vector<float> x(frames);
    for (unsigned n = 0; n < frames; ++n) x[n] = amplitude * static_cast<float>(std::sin(2 * kPi * hz * n / rate));
    return x;
}
void Formats() {
    constexpr unsigned rates[]{8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
    unsigned supported = 0;
    for (unsigned a : rates) for (unsigned b : {44100u, 48000u}) supported += PcmConverter::Supported(a, 2, b, 2) ? 1 : 0;
    // 8 kHz to 44.1 or 48 kHz and 11.025 kHz to 48 kHz are more than 4:1 apart; every other AAC rate pair is supported.
    CHECK(supported == 2 * std::size(rates) - 3);
    CHECK(!PcmConverter::Supported(7999, 2, 48000, 2)); CHECK(!PcmConverter::Supported(48000, 2, 192001, 2));
    CHECK(!PcmConverter::Supported(48000, 3, 48000, 2)); CHECK(!PcmConverter::Supported(48000, 6, 48000, 8));
    CHECK(!PcmConverter::Supported(48000, 0, 48000, 2)); CHECK(!PcmConverter::Supported(48000, 2, 48000, 9));
    CHECK(PcmConverter::Supported(48000, 6, 48000, 2)); CHECK(PcmConverter::Supported(44100, 2, 48000, 6)); CHECK(PcmConverter::Supported(48000, 8, 48000, 2));
    // 44.1 kHz -> 48 kHz advances 147/160 of an input frame per output frame: 160 phases.
    PcmConverter c; CHECK(c.Configure(Arena(), 44100, 2, 48000, 2)); CHECK(!c.Identity());
    CHECK(c.Taps() == 96 && c.Lookahead() == 48);
    PcmConverter down; CHECK(down.Configure(Arena(), 48000, 1, 44100, 1)); CHECK(down.Taps() == 2 * 53); // 48 crossings at 44.1/48.
    CHECK(!c.Configure(Arena(), 44100, 2, 48000, 2)); // Once.
}
void Identity() {
    // Same rate and channels: every frame passes at once, bit for bit (signed zero, denormal, extremes).
    PcmConverter c; CHECK(c.Configure(Arena(), 48000, 2, 48000, 2)); CHECK(c.Identity()); CHECK(c.Lookahead() == 0);
    std::vector<float> in{0.25f, -0.0f, 1e-40f, -1.0f, 0.999f, 3.0f, -7.5f, 0.125f};
    c.Reset(); CHECK(c.Push(in.data(), 4) == 4); CHECK(c.Available() == 4);
    float out[8]{}; CHECK(c.Pull(out, 8) == 4);
    CHECK(std::memcmp(out, in.data(), sizeof(out)) == 0 || (out[1] == 0 && out[0] == 0.25f && out[2] == 1e-40f && out[5] == 3.0f));
    for (unsigned i = 0; i < 8; ++i) if (i != 1) CHECK(out[i] == in[i]);
    // Same rate, other layout: the remix alone, still no delay.
    PcmConverter up; CHECK(up.Configure(Arena(), 48000, 2, 48000, 6)); CHECK(!up.Identity());
    up.Reset(); const float stereo[]{0.5f, -0.25f}; CHECK(up.Push(stereo, 1) == 1); float six[6]{};
    CHECK(up.Pull(six, 1) == 1); CHECK(six[0] == 0.5f && six[1] == -0.25f && six[2] == 0 && six[3] == 0 && six[4] == 0 && six[5] == 0);
}
void Remix() {
    // 5.1 -> stereo is the engine's fold-down: DspProcessor::DownmixWeights, LFE left out.
    PcmConverter c; CHECK(c.Configure(Arena(), 48000, 6, 48000, 2));
    for (unsigned ch = 0; ch < 6; ++ch) {
        float l = 0, r = 0; media::audio::DspProcessor::DownmixWeights(6, ch, l, r);
        CHECK(c.Weight(0, ch) == l && c.Weight(1, ch) == r);
    }
    CHECK(c.Weight(0, 3) == 0 && c.Weight(1, 3) == 0);         // LFE.
    CHECK(c.Weight(0, 0) == 0.5f && c.Weight(1, 0) == 0);      // FL to the left only.
    CHECK(std::fabs(c.Weight(0, 2) - 0.5f / std::sqrt(2.0f)) < 1e-6f && c.Weight(0, 2) == c.Weight(1, 2)); // Centre.
    c.Reset(); const float frame[6]{0.2f, 0.4f, 0.6f, 0.9f, 0.1f, -0.3f}; CHECK(c.Push(frame, 1) == 1);
    float lr[2]{}; CHECK(c.Pull(lr, 1) == 1);
    CHECK(std::fabs(lr[0] - (0.5f * 0.2f + 0.3535534f * 0.6f + 0.3535534f * 0.1f)) < 1e-6f);
    CHECK(std::fabs(lr[1] - (0.5f * 0.4f + 0.3535534f * 0.6f + 0.3535534f * -0.3f)) < 1e-6f);
    PcmConverter mono_up, mono_six, to_mono, six_mono;
    CHECK(mono_up.Configure(Arena(), 48000, 1, 48000, 2) && mono_six.Configure(Arena(), 48000, 1, 48000, 6));
    CHECK(to_mono.Configure(Arena(), 48000, 2, 48000, 1) && six_mono.Configure(Arena(), 48000, 6, 48000, 1));
    CHECK(mono_up.Weight(0, 0) == 1 && mono_up.Weight(1, 0) == 1);
    CHECK(mono_six.Weight(2, 0) == 1 && mono_six.Weight(0, 0) == 0);
    CHECK(to_mono.Weight(0, 0) == 0.5f && to_mono.Weight(0, 1) == 0.5f);
    CHECK(six_mono.Weight(0, 0) == 0.25f && six_mono.Weight(0, 3) == 0);
}
void Quality() {
    // A tone keeps its frequency, level and phase on the output's own time base: residual after fitting the tone
    // that was put in, at the time each output frame stands for (no delay), over the middle of 2 s.
    struct Pair { unsigned from, to; double hz; double residual_db; };
    // Measured (MSVC Release): -93.5 to -133.7 dB; the 7 kHz tone at 16 kHz is near the band's edge.
    const Pair pairs[]{{44100, 48000, 1000, -100}, {44100, 48000, 15000, -92}, {48000, 44100, 1000, -100}, {48000, 44100, 19000, -92},
                       {22050, 48000, 3000, -100}, {96000, 48000, 5000, -120}, {32000, 44100, 440, -95}, {16000, 48000, 7000, -88}};
    for (const auto& p : pairs) {
        PcmConverter c; CHECK(c.Configure(Arena(), p.from, 1, p.to, 1));
        const auto in = Tone(2 * p.from, p.hz, p.from);
        const auto out = Convert(c, in);
        const auto fit = FitTone(out, p.hz, p.to, p.to / 4, out.size() - p.to / 4);
        std::printf("  %u -> %u Hz, %.0f Hz tone: level %.6f (0.5 in), phase %.2e rad, residual %.1f dB\n", p.from, p.to, p.hz, fit.amplitude, fit.phase, fit.residual_db);
        CHECK(std::fabs(fit.amplitude - 0.5) < 0.5 * 0.001); // Within 0.01 dB.
        CHECK(std::fabs(fit.phase) < 1e-4);                  // 1e-4 rad at 1 kHz is 16 ns: no delay.
        CHECK(fit.residual_db < p.residual_db);
        // Frames: one per output instant before the input ends (N input frames last N / from seconds), exactly.
        const auto expected = static_cast<std::size_t>((static_cast<std::uint64_t>(in.size()) * p.to + p.from - 1) / p.from);
        CHECK(out.size() == expected);
    }
    // A start between input frames: the first output frame half an input frame after the first input frame.
    PcmConverter c; CHECK(c.Configure(Arena(), 44100, 1, 48000, 1));
    const auto out = Convert(c, Tone(44100, 1000, 44100), 0.5);
    const auto fit = FitTone(out, 1000, 48000, 12000, 36000);
    CHECK(std::fabs(fit.phase - 2 * kPi * 1000 * 0.5 / 44100) < 1e-4);
}
void Aliasing() {
    // Down: a 23 kHz tone at 48 kHz is above the 44.1 kHz output's Nyquist frequency and must not fold back (to 21.1 kHz).
    {
        PcmConverter c; CHECK(c.Configure(Arena(), 48000, 1, 44100, 1));
        const auto out = Convert(c, Tone(96000, 23000, 48000));
        double energy = 0; for (std::size_t n = 11025; n + 11025 < out.size(); ++n) energy += static_cast<double>(out[n]) * out[n];
        const double db = 10 * std::log10(energy / static_cast<double>(out.size() - 22050) / 0.125 + 1e-30);
        std::printf("  48 -> 44.1 kHz, 23 kHz tone: %.1f dB left\n", db);
        CHECK(db < -80); // Measured -85.5 dB.
    }
    // Up: an 18 kHz tone at 44.1 kHz has its image at 26.1 kHz, which a 48 kHz output would fold to 21.9 kHz.
    {
        PcmConverter c; CHECK(c.Configure(Arena(), 44100, 1, 48000, 1));
        const auto out = Convert(c, Tone(88200, 18000, 44100));
        const auto image = FitTone(out, 48000 - 26100, 48000, 12000, out.size() - 12000);
        std::printf("  44.1 -> 48 kHz, 18 kHz tone: image at 21.9 kHz %.1f dB\n", 20 * std::log10(image.amplitude / 0.5 + 1e-30));
        CHECK(image.amplitude < 0.5 * 1e-4); // -80 dB; measured -94 dB.
    }
}
void Determinism() {
    // Any split of pushes and pulls gives the same output bit for bit, and Room bounds what is held.
    PcmConverter whole, split; CHECK(whole.Configure(Arena(), 44100, 2, 48000, 6)); CHECK(split.Configure(Arena(), 44100, 2, 48000, 6, 1024));
    std::vector<float> in(2 * 30000);
    std::uint32_t state = 7;
    for (auto& x : in) { state = state * 1664525u + 1013904223u; x = static_cast<float>(state >> 8) / 8388608.0f - 1.0f; }
    const auto reference = Convert(whole, in);
    split.Reset(); std::vector<float> got, block(6 * 997);
    unsigned at = 0, round = 0; const unsigned pushes[]{1, 7, 1023, 160, 2, 999, 333}, pulls[]{3, 997, 1, 64, 500, 17};
    for (;;) {
        const unsigned want = (std::min)(pushes[round % 7], 30000 - at);
        CHECK(split.Room() <= 1024 + 47);
        at += split.Push(in.data() + static_cast<std::size_t>(at) * 2, want);
        if (at == 30000 && !split.Finished()) split.Finish();
        const unsigned n = split.Pull(block.data(), pulls[round % 6]);
        got.insert(got.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n) * 6);
        ++round;
        if (split.Finished() && !split.Available()) break;
        if (round > 1000000) { CHECK(false); break; }
    }
    CHECK(got.size() == reference.size());
    CHECK(got.size() == reference.size() && std::memcmp(got.data(), reference.data(), got.size() * sizeof(float)) == 0);
    CHECK(split.Push(in.data(), 1) == 0); // Finished: nothing more until Reset.
}
void Drift() {
    // An hour's worth of positions is exact: after N input frames the output count is the exact rational count (the
    // position is an integer and a remainder, never a rounded float). One minute is converted, then counted.
    PcmConverter c; CHECK(c.Configure(Arena(), 44100, 1, 48000, 1, 8192));
    c.Reset(); std::vector<float> in(4410, 0.25f), out(8192);
    std::uint64_t pulled = 0; float middle = 0;
    for (unsigned block = 0; block < 600; ++block) {
        CHECK(c.Push(in.data(), 4410) == 4410);
        const unsigned n = c.Pull(out.data(), 8192); pulled += n;
        if (block == 300 && n) middle = out[n / 2];
    }
    c.Finish(); pulled += c.Pull(out.data(), 8192);
    const std::uint64_t frames = 600ull * 4410;
    CHECK(pulled == (frames * 48000 + 44099) / 44100);
    CHECK(c.Pushed() == frames && c.Pulled() == pulled);
    CHECK(std::fabs(middle - 0.25f) < 1e-6f); // A constant stays the constant (unity gain at DC on every phase).
}
void Allocation() {
    PcmConverter c; CHECK(c.Configure(Arena(), 44100, 2, 48000, 6));
    std::vector<float> in(2 * 1024, 0.1f), out(6 * 2048);
    c.Reset();
    allocation_probe::Begin();
    for (unsigned i = 0; i < 200; ++i) { (void)c.Push(in.data(), 1024); (void)c.Pull(out.data(), 2048); }
    c.Finish(); (void)c.Pull(out.data(), 2048); c.Reset(0.25);
    CHECK(allocation_probe::End() == 0);
    media::core::LinearMemoryArena sealed; CHECK(sealed.Initialize(1u << 20)); CHECK(sealed.Seal());
    PcmConverter refused; CHECK(!refused.Configure(sealed, 44100, 2, 48000, 2)); CHECK(!refused.Configured());
    CHECK(refused.Push(in.data(), 4) == 0 && refused.Available() == 0 && refused.Pull(out.data(), 4) == 0);
}
}
int main() {
    std::puts("media_pcm_converter_tests");
    Formats(); Identity(); Remix(); Quality(); Aliasing(); Determinism(); Drift(); Allocation();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
