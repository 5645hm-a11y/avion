// BS.1770-4 loudness and night mode (docs/audio-dsp.md, "Loudness and night mode"):
// - K-weighting: the derived coefficients at 48 kHz are the spec's table; a 0 dBFS 997 Hz sine in one channel reads
//   -3.01 LUFS (two channels 0.00), at 48 and 44.1 kHz; the measured response at other frequencies is the filters' own;
// - gating: silence (absolute gate) and a quieter stretch (relative gate) do not pull the integrated loudness down;
// - true peak: a quarter-rate sine sampled 45 degrees off its crests reads 0 dBTP where its samples peak at -3 dB;
// - night mode: off and settled it is bit-identical; the noise floor of a quiet room is never lifted; quiet speech is
//   lifted toward -20 LUFS; after speech, a pause is not lifted further; a sudden blast is pulled down and its true peak
//   held at -1 dBTP; inter-sample peaks are limited; the output does not depend on the block sizes; nothing allocates.
#include "media/audio/Loudness.hpp"
#include "allocation_probe.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>
using media::audio::LoudnessMeter; using media::audio::NightMode;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kRate = 48000, kMaxSeconds = 40;
float buffer[kRate * kMaxSeconds * 2], copy[kRate * kMaxSeconds * 2], other[kRate * kMaxSeconds * 2]; // Stereo, static: no heap.
std::uint32_t noise_state = 12345;
float Noise() noexcept { noise_state = noise_state * 1664525u + 1013904223u; return static_cast<float>(static_cast<std::int32_t>(noise_state)) / 2147483648.0f; }
// `seconds` of a sine on the channels in `mask` of stereo frames from `at` (seconds).
void Sine(float* x, double at, double seconds, double hz, double amplitude, unsigned mask, double phase = 0, unsigned rate = kRate) {
    const auto first = static_cast<std::size_t>(at * rate), n = static_cast<std::size_t>(seconds * rate);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = static_cast<float>(amplitude * std::sin(2 * kPi * hz * static_cast<double>(i) / rate + phase));
        for (unsigned c = 0; c < 2; ++c) x[(first + i) * 2 + c] = (mask >> c) & 1u ? v : 0.0f;
    }
}
// Speech-like: two partials in 300 ms syllables with 100 ms gaps, at `amplitude`.
void Speech(float* x, double at, double seconds, double amplitude) {
    const auto first = static_cast<std::size_t>(at * kRate), n = static_cast<std::size_t>(seconds * kRate);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate, cycle = std::fmod(t, 0.4);
        const double envelope = cycle < 0.3 ? std::sin(kPi * cycle / 0.3) : 0.0;
        const float v = static_cast<float>(amplitude * envelope * (0.6 * std::sin(2 * kPi * 220 * t) + 0.4 * std::sin(2 * kPi * 1100 * t)));
        x[(first + i) * 2] = v; x[(first + i) * 2 + 1] = v;
    }
}
void Floor(float* x, double at, double seconds, double amplitude) {
    const auto first = static_cast<std::size_t>(at * kRate), n = static_cast<std::size_t>(seconds * kRate);
    for (std::size_t i = 0; i < n; ++i) { x[(first + i) * 2] = static_cast<float>(amplitude * Noise()); x[(first + i) * 2 + 1] = static_cast<float>(amplitude * Noise()); }
}
float MeasureShortTerm(const float* x, double seconds, unsigned mask_channels = 2, unsigned rate = kRate) {
    static LoudnessMeter meter; (void)meter.Configure(rate, mask_channels);
    meter.Feed(x, static_cast<unsigned>(seconds * rate), 2);
    return meter.ShortTerm();
}
// |H| in dB of the two K-weighting stages at `hz`.
double Response(double hz, double rate) {
    double s[5], h[5]; LoudnessMeter::KWeighting(rate, s, h);
    const auto stage = [&](const double* k) {
        const double w = 2 * kPi * hz / rate, c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = k[0] + k[1] * c1 + k[2] * c2, ni = -(k[1] * s1 + k[2] * s2);
        const double dr = 1 + k[3] * c1 + k[4] * c2, di = -(k[3] * s1 + k[4] * s2);
        return 10 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    };
    return stage(s) + stage(h);
}
void Meter() {
    // The spec's table at 48 kHz.
    double s[5], h[5]; LoudnessMeter::KWeighting(48000, s, h);
    const double shelf[5]{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
    const double highpass[5]{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    double worst = 0;
    for (unsigned i = 0; i < 5; ++i) worst = std::fmax(worst, std::fmax(std::fabs(s[i] - shelf[i]), std::fabs(h[i] - highpass[i])));
    std::printf("K-weighting at 48 kHz: largest difference from the spec's coefficients %.2e\n", worst);
    CHECK(worst < 1e-9);
    // Reference levels: one channel -3.01 LUFS, two 0.00, at 48 and 44.1 kHz.
    static constexpr unsigned kRates[]{48000u, 44100u};
    for (const unsigned rate : kRates) {
        Sine(buffer, 0, 5, 997, 1.0, 1, 0, rate);
        const float one = MeasureShortTerm(buffer, 5, 2, rate);
        Sine(buffer, 0, 5, 997, 1.0, 3, 0, rate);
        const float two = MeasureShortTerm(buffer, 5, 2, rate);
        std::printf("997 Hz at 0 dBFS, %u Hz: one channel %.3f LUFS, two %.3f LUFS\n", rate, one, two);
        CHECK(std::fabs(one + 3.01f) < 0.05f && std::fabs(two) < 0.05f);
    }
    // The response: each frequency's loudness against 997 Hz is the filters' response there.
    Sine(buffer, 0, 5, 997, 1.0, 1);
    const float reference = MeasureShortTerm(buffer, 5);
    double response_worst = 0; float at25 = 0, at10k = 0;
    for (const double hz : {25.0, 50.0, 100.0, 500.0, 2000.0, 5000.0, 10000.0}) {
        Sine(buffer, 0, 5, hz, 1.0, 1);
        const float delta = MeasureShortTerm(buffer, 5) - reference;
        const double expected = Response(hz, kRate) - Response(997, kRate);
        response_worst = std::fmax(response_worst, std::fabs(delta - expected));
        if (hz == 25.0) at25 = delta;
        if (hz == 10000.0) at10k = delta;
    }
    std::printf("Response against 997 Hz: 25 Hz %.2f dB, 10 kHz %+.2f dB; measured within %.3f dB of the filters'\n", at25, at10k, response_worst);
    CHECK(response_worst < 0.05 && at25 < -4.0f && at10k > 3.0f && at10k < 5.0f);
    // Gating: 10 s of digital silence, 15 s at about -23 LUFS, 15 s 20 LU quieter: the integrated loudness is the -23 stretch.
    std::memset(buffer, 0, sizeof(float) * kRate * 40 * 2);
    Sine(buffer, 10, 15, 997, 0.0708, 3); Sine(buffer, 25, 15, 997, 0.00708, 3);
    static LoudnessMeter meter; (void)meter.Configure(kRate, 2);
    meter.Feed(buffer, kRate * 10, 2); const float silent = meter.ShortTerm();
    meter.Feed(buffer + kRate * 10 * 2, kRate * 15, 2); const float loud = meter.Integrated();
    meter.Feed(buffer + kRate * 25 * 2, kRate * 15, 2); const float gated = meter.Integrated();
    std::printf("Gating: silence %.1f LUFS; integrated %.2f LUFS after the loud stretch, %.2f after the quiet one\n", silent, loud, gated);
    CHECK(silent <= LoudnessMeter::kSilence && std::fabs(loud + 23.0f) < 0.1f && std::fabs(gated - loud) < 0.1f);
    // True peak: a quarter-rate sine, 45 degrees off its crests.
    Sine(buffer, 0, 2, kRate / 4.0, 1.0, 3, kPi / 4);
    float sample_peak = 0; for (unsigned i = 0; i < kRate * 2 * 2; ++i) sample_peak = std::fmax(sample_peak, std::fabs(buffer[i]));
    (void)meter.Configure(kRate, 2); meter.Feed(buffer, kRate * 2, 2);
    std::printf("True peak: samples %.2f dBFS, true peak %.2f dBTP (recent %.2f)\n", 20 * std::log10(sample_peak), meter.TruePeak(), meter.RecentTruePeak());
    CHECK(std::fabs(20 * std::log10(sample_peak) + 3.01) < 0.05 && std::fabs(meter.TruePeak()) < 0.3f && meter.RecentTruePeak() == meter.TruePeak());
    Sine(buffer, 0, 2, 997, 0.5, 3); (void)meter.Configure(kRate, 2); meter.Feed(buffer, kRate * 2, 2);
    CHECK(std::fabs(meter.TruePeak() + 6.02f) < 0.1f);
}
void Night() {
    static NightMode night;
    // Off and settled: bit-identical, undelayed.
    Speech(buffer, 0, 4, 0.05); std::memcpy(copy, buffer, sizeof(float) * kRate * 4 * 2);
    CHECK(night.Configure(kRate, 2)); night.Process(buffer, kRate * 4);
    CHECK(std::memcmp(buffer, copy, sizeof(float) * kRate * 4 * 2) == 0 && !night.Active());
    // The noise floor of a quiet room: never lifted.
    Floor(buffer, 0, 10, 0.0003);
    (void)night.Configure(kRate, 2); night.SetEnabled(true);
    float most = -100;
    for (unsigned b = 0; b < 10 * 10; ++b) { night.Process(buffer + static_cast<std::size_t>(b) * 4800 * 2, 4800); most = std::fmax(most, night.GainDb()); }
    std::printf("Noise floor at %.1f LUFS: night gain at most %+.2f dB\n", night.Meter().ShortTerm(), most);
    CHECK(night.Meter().ShortTerm() < -60.0f && most <= 0.01f);
    // Quiet speech, then a pause (the room's floor), then a blast.
    Speech(buffer, 0, 15, 0.0125); Floor(buffer, 15, 6, 0.0003); Floor(buffer, 21, 2, 0.6); Speech(buffer, 23, 2, 0.0125);
    std::memcpy(copy, buffer, sizeof(float) * kRate * 25 * 2);
    (void)night.Configure(kRate, 2); night.SetEnabled(true);
    static LoudnessMeter out; (void)out.Configure(kRate, 2);
    float lifted = 0, input_speech = 0, output_speech = 0, after_speech = 0, in_pause = -100, blast_low = 100, blast_peak = -100;
    for (unsigned b = 0; b < 25 * 10; ++b) {
        float* block = buffer + static_cast<std::size_t>(b) * 4800 * 2;
        night.Process(block, 4800); out.Feed(block, 4800, 2);
        const double t = (b + 1) / 10.0;
        if (t == 15.0) { lifted = night.GainDb(); input_speech = night.Meter().ShortTerm(); output_speech = out.ShortTerm(); after_speech = lifted; }
        if (t > 15.0 && t <= 21.0) in_pause = std::fmax(in_pause, night.GainDb());
        if (t > 21.0 && t <= 23.0) { blast_low = std::fmin(blast_low, night.GainDb()); blast_peak = std::fmax(blast_peak, out.RecentTruePeak()); }
    }
    std::printf("Quiet speech at %.1f LUFS: lifted %+.1f dB to %.1f LUFS; in the pause after it, gain at most %+.2f dB (it was %+.2f); "
                "the blast: gain down to %+.1f dB, true peak out %.2f dBTP\n", input_speech, lifted, output_speech, in_pause, after_speech, blast_low, blast_peak);
    CHECK(input_speech < -37.0f && lifted >= 10.0f && output_speech >= input_speech + 9.0f && output_speech <= NightMode::kTarget + 1.5f);
    CHECK(in_pause <= after_speech + 0.01f);
    CHECK(blast_low < 0.0f && blast_peak <= NightMode::kCeiling + 0.15f);
    // Inter-sample peaks: a loud quarter-rate sine 45 degrees off its crests, held at the ceiling by the limiter alone.
    Sine(buffer, 0, 3, kRate / 4.0, 0.9, 3, kPi / 4);
    (void)night.Configure(kRate, 2); night.SetEnabled(true); (void)out.Configure(kRate, 2);
    for (unsigned b = 0; b < 30; ++b) { night.Process(buffer + static_cast<std::size_t>(b) * 4800 * 2, 4800); out.Feed(buffer + static_cast<std::size_t>(b) * 4800 * 2, 4800, 2); }
    std::printf("Inter-sample peaks: %.2f dBTP in, %.2f dBTP out (limiter %.2f dB)\n", 20 * std::log10(0.9), out.TruePeak(), night.LimiterDb());
    CHECK(out.TruePeak() <= NightMode::kCeiling + 0.15f);
    // Block sizes do not matter.
    Speech(buffer, 0, 8, 0.02); Floor(buffer, 4, 1, 0.5); std::memcpy(other, buffer, sizeof(float) * kRate * 8 * 2);
    (void)night.Configure(kRate, 2); night.SetEnabled(true); night.Process(buffer, kRate * 8);
    (void)night.Configure(kRate, 2); night.SetEnabled(true);
    for (unsigned done = 0, size = 1; done < kRate * 8; size = size * 7 % 2039 + 1) { const unsigned n = size < kRate * 8 - done ? size : kRate * 8 - done; night.Process(other + static_cast<std::size_t>(done) * 2, n); done += n; }
    CHECK(std::memcmp(buffer, other, sizeof(float) * kRate * 8 * 2) == 0);
    // Off again: a crossfade, then bit-identical.
    Speech(buffer, 0, 2, 0.02); std::memcpy(copy, buffer, sizeof(float) * kRate * 2 * 2);
    night.SetEnabled(false); night.Process(buffer, kRate * 2);
    const auto tail = static_cast<std::size_t>(kRate) * 2; // The second second.
    CHECK(!night.Active() && std::memcmp(buffer + tail, copy + tail, sizeof(float) * kRate * 2) == 0);
    std::printf("Night mode latency %u frames (%.2f ms at 48 kHz)\n", NightMode::Latency(), NightMode::Latency() * 1000.0 / kRate);
}
}
int main() {
    std::puts("Loudness tests"); // The first output allocates stdout's buffer: before any probe.
    allocation_probe::Begin();
    Meter();
    Night();
    const auto allocations = allocation_probe::End();
    std::printf("Loudness: %d failures, %llu allocations\n", failures, static_cast<unsigned long long>(allocations));
    CHECK(allocations == 0);
    return failures ? 1 : 0;
}
