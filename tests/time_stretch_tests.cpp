// Pitch-preserving playback speed (audio::TimeStretcher; docs/audio-dsp.md, "Playback speed"): 1x is an exact copy; at
// 0.25x to 2x a tone keeps its frequency and level, a voiced harmonic sound keeps its fundamental, the input consumed
// follows the speed, changes of speed neither click nor drift, a return to 1x copies the input exactly again, and nothing
// allocates after Configure.
#include "media/audio/TimeStretcher.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using media::audio::TimeStretcher;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kRate = 48000;
std::vector<float> Tone(double hz, double seconds, unsigned channels, double amplitude = 0.5) {
    std::vector<float> v(static_cast<std::size_t>(seconds * kRate) * channels);
    for (std::size_t i = 0; i < v.size() / channels; ++i)
        for (unsigned c = 0; c < channels; ++c) v[i * channels + c] = static_cast<float>(amplitude * std::sin(2 * kPi * hz * static_cast<double>(i) / kRate));
    return v;
}
// A voiced sound: a 180 Hz fundamental and seven harmonics at 1/k, as a vowel's source.
std::vector<float> Voiced(double seconds) {
    std::vector<float> v(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < v.size(); ++i) {
        double s = 0; for (unsigned k = 1; k <= 8; ++k) s += std::sin(2 * kPi * 180.0 * k * static_cast<double>(i) / kRate + 0.3 * k) / k;
        v[i] = static_cast<float>(0.25 * s);
    }
    return v;
}
// Runs the whole signal through: pushes 1024 frames when there is room, pulls 480 when they are ready. `allocations`:
// those made while it ran (the output's storage is reserved before).
std::vector<float> Run(TimeStretcher& t, const std::vector<float>& in, unsigned channels, std::uint64_t* allocations = nullptr) {
    std::vector<float> out; out.reserve(in.size() * 5);
    std::vector<float> chunk(480 * channels);
    if (allocations) allocation_probe::Begin();
    std::size_t pushed = 0; const std::size_t frames = in.size() / channels;
    for (;;) {
        const auto room = t.Room();
        if (pushed < frames && room) pushed += t.Push(in.data() + pushed * channels, static_cast<unsigned>(std::min<std::size_t>({1024, room, frames - pushed})));
        const auto ready = t.Available();
        if (ready) {
            const auto n = std::min(ready, 480u);
            const auto got = t.Pull(chunk.data(), n);
            if (got != n) { ++failures; std::printf("FAIL Pull %u of %u\n", got, n); break; }
            out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got) * channels);
        } else if (pushed == frames || !room) break;
    }
    if (allocations) *allocations = allocation_probe::End();
    return out;
}
// Power of one frequency (Goertzel) through a Hann window: a neighbouring frequency's leakage falls below -60 dB within
// a few bins (a rectangular window leaves -30 dB at 5 bins, as much as a real defect).
double Goertzel(const float* x, std::size_t n, double hz) {
    const double k = 2 * std::cos(2 * kPi * hz / kRate); double a = 0, b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const double c = w * x[i] + k * a - b; b = a; a = c;
    }
    return 4 * (a * a + b * b - k * a * b) / (static_cast<double>(n) * static_cast<double>(n)); // x4: the window's mean is 1/2.
}
// The fundamental: the shortest lag between 2.5 and 20 ms whose normalised autocorrelation peaks within 5 % of the
// highest (its multiples score as high), refined by a parabola through the peak.
double Fundamental(const float* x, std::size_t n) {
    std::vector<double> r(kRate / 50 + 2, 0.0);
    double best = -1;
    for (unsigned lag = kRate / 400 - 1; lag <= kRate / 50 + 1; ++lag) {
        double dot = 0, e0 = 1e-12, e1 = 1e-12;
        for (std::size_t i = 0; i + lag < n; ++i) { dot += x[i] * x[i + lag]; e0 += x[i] * x[i]; e1 += x[i + lag] * x[i + lag]; }
        r[lag] = dot / std::sqrt(e0 * e1); best = std::max(best, r[lag]);
    }
    for (unsigned lag = kRate / 400; lag <= kRate / 50; ++lag)
        if (r[lag] >= 0.95 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
            const double d = r[lag - 1] - 2 * r[lag] + r[lag + 1];
            const double offset = d != 0 ? 0.5 * (r[lag - 1] - r[lag + 1]) / d : 0;
            return static_cast<double>(kRate) / (lag + offset);
        }
    return 0;
}
// Mean Goertzel power over 0.1 s windows (a stretch is a tone in pieces; one long window would blur its phase steps).
double Power(const std::vector<float>& x, std::size_t from, std::size_t n, double hz) {
    double sum = 0; unsigned windows = 0;
    for (std::size_t w = from; w + 4800 <= from + n; w += 4800, ++windows) sum += Goertzel(x.data() + w, 4800, hz);
    return windows ? sum / windows : 0;
}

void Copy() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    TimeStretcher t; CHECK(t.Configure(arena, kRate, 2) && t.Passing() && t.Speed() == 1.0f);
    CHECK(!t.Configure(arena, kRate, 2)); // Once.
    std::vector<float> noise(kRate * 2 * 2); std::uint32_t s = 1;
    for (auto& v : noise) { s = s * 1664525u + 1013904223u; v = static_cast<float>(static_cast<int>(s >> 8) - (1 << 23)) / static_cast<float>(1 << 23); }
    std::uint64_t allocations = 0;
    const auto out = Run(t, noise, 2, &allocations);
    CHECK(out == noise && t.Pulled() == t.Pushed() && t.Passing() && allocations == 0); // Every sample, as it came.
    CHECK(!t.SetSpeed(0.2f) && !t.SetSpeed(4.5f) && !t.SetSpeed(std::nanf("")) && t.Speed() == 1.0f); // 0.25x to 4x (API 16).
    TimeStretcher odd; CHECK(!odd.Configure(arena, 7000, 2) && !odd.Configure(arena, kRate, 9) && !odd.Configure(arena, kRate, 2, 100));
}

void Pitch() {
    for (const float speed : {0.25f, 0.5f, 0.75f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f}) { // 3x and 4x since API 16.
        media::core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
        TimeStretcher t; CHECK(t.Configure(arena, kRate, 1));
        CHECK(t.SetSpeed(speed));
        const auto tone = Tone(220, 6.0, 1);
        std::uint64_t allocations = 0;
        const auto out = Run(t, tone, 1, &allocations);
        // Length: the input consumed is `speed` times the output, within the frames the stretcher still holds.
        const double consumed = static_cast<double>(t.Pulled()) * speed;
        const double held = static_cast<double>(tone.size()) - consumed;
        // Pitch: the tone stays at 220 Hz; a resampler would have moved it to 220 x speed.
        const std::size_t from = out.size() / 4, n = out.size() / 2;
        const double kept = Power(out, from, n, 220), moved = Power(out, from, n, 220 * speed);
        // Level: 10 ms windows of the steady part stay within 1 dB of each other (no comb, no gaps).
        double low = 1e9, high = 0;
        for (std::size_t w = from; w + 480 <= from + n; w += 480) {
            double e = 0; for (std::size_t i = w; i < w + 480; ++i) e += static_cast<double>(out[i]) * out[i];
            low = std::min(low, e); high = std::max(high, e);
        }
        // Continuity: no step larger than the tone's own steepest one, 10 % over (a click is far larger).
        double step = 0; for (std::size_t i = from + 1; i < from + n; ++i) step = std::max(step, std::fabs(static_cast<double>(out[i]) - out[i - 1]));
        const double steepest = 0.5 * 2 * kPi * 220 / kRate;
        std::printf("%.2fx: %zu of %zu frames out (input held %.0f); 220 Hz %.2e, %.0f Hz %.2e; level range %.2f dB; largest step %.4f (tone %.4f)\n",
                    static_cast<double>(speed), out.size(), tone.size(), held, kept, 220.0 * speed, moved, 10 * std::log10(high / low), step, steepest);
        CHECK(held >= 0 && held < 3.0 * t.Hop() + 2 * t.Search() + 2);
        CHECK(kept > 1e-2 && kept > 1000 * moved);
        CHECK(10 * std::log10(high / low) < 1.0);
        CHECK(step < 1.1 * steepest);
        CHECK(allocations == 0 && !t.Passing() && t.SpeedInForce() == speed);
    }
    // A voiced sound keeps its fundamental at the extremes.
    for (const float speed : {0.5f, 2.0f}) {
        media::core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
        TimeStretcher t; CHECK(t.Configure(arena, kRate, 1) && t.SetSpeed(speed));
        const auto voice = Voiced(4.0);
        const auto out = Run(t, voice, 1);
        const double f = Fundamental(out.data() + out.size() / 3, 4800), f_in = Fundamental(voice.data() + voice.size() / 3, 4800);
        std::printf("Voiced 180 Hz at %.1fx: fundamental %.1f Hz out, %.1f Hz in\n", static_cast<double>(speed), f, f_in);
        CHECK(std::fabs(f - 180) < 1.8 && std::fabs(f_in - 180) < 1.8);
    }
}

// 1x -> 2x -> 0.5x -> 1x on a stereo tone: no click at any change, the clock's map (LastChange) follows the input, and
// after the return to 1x the output is the input again, sample for sample.
void Changes() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    TimeStretcher t; CHECK(t.Configure(arena, kRate, 2));
    const auto tone = Tone(330, 12.0, 2);
    const std::size_t frames = tone.size() / 2;
    std::vector<float> out; out.reserve(tone.size() * 3); std::vector<float> chunk(480 * 2);
    std::size_t pushed = 0; unsigned stage = 0;
    TimeStretcher::Change changes[4]{}; unsigned seen = 0;
    allocation_probe::Begin();
    for (;;) {
        if (pushed < frames && t.Room()) pushed += t.Push(tone.data() + pushed * 2, static_cast<unsigned>(std::min<std::size_t>({1024, t.Room(), frames - pushed})));
        const auto ready = t.Available();
        if (!ready) { if (pushed == frames) break; continue; }
        const auto n = std::min(ready, 480u);
        if (t.Pull(chunk.data(), n) != n) { ++failures; break; }
        out.insert(out.end(), chunk.begin(), chunk.begin() + n * 2);
        const auto seconds = static_cast<double>(t.Pulled()) / kRate;
        if (stage == 0 && seconds >= 1.0) { CHECK(t.SetSpeed(2.0f)); ++stage; }
        else if (stage == 1 && seconds >= 2.0) { CHECK(t.SetSpeed(0.5f)); ++stage; }
        else if (stage == 2 && seconds >= 4.0) { CHECK(t.SetSpeed(1.0f)); ++stage; }
        if (seen < 4 && (seen == 0 || t.LastChange().output != changes[seen - 1].output)) changes[seen++] = t.LastChange();
    }
    const auto allocations = allocation_probe::End();
    CHECK(allocations == 0 && t.Passing() && seen == 4);
    // The changes: 2x from about 1 s of output, 0.5x from about 2 s, 1x again after 4 s; each one's input position is
    // where the previous speed would have brought it, within the search window and a hop.
    CHECK(changes[1].speed == 2.0f && changes[2].speed == 0.5f && changes[3].speed == 1.0f);
    for (unsigned i = 1; i < 4; ++i) {
        const double expected = changes[i - 1].input + static_cast<double>(changes[i].output - changes[i - 1].output) * changes[i - 1].speed;
        std::printf("Change %u: output %llu, input %.0f (the previous speed says %.0f), speed %.2f\n", i, static_cast<unsigned long long>(changes[i].output),
                    changes[i].input, expected, static_cast<double>(changes[i].speed));
        CHECK(std::fabs(changes[i].input - expected) <= t.Search() + t.Hop());
    }
    // After the return: the input itself, from the input position reported.
    bool exact = true;
    const auto output = static_cast<std::size_t>(changes[3].output), input = static_cast<std::size_t>(changes[3].input);
    for (std::size_t i = 0; i < kRate && exact; ++i) exact = out[(output + i) * 2] == tone[(input + i) * 2] && out[(output + i) * 2 + 1] == tone[(input + i) * 2 + 1];
    CHECK(exact);
    // No click anywhere, the left and right channels the same throughout.
    double step = 0; bool same = true;
    for (std::size_t i = 1; i < out.size() / 2; ++i) {
        step = std::max(step, std::fabs(static_cast<double>(out[i * 2]) - out[(i - 1) * 2]));
        same = same && out[i * 2] == out[i * 2 + 1];
    }
    const double steepest = 0.5 * 2 * kPi * 330 / kRate;
    std::printf("Changes: %zu frames out, largest step %.4f (the tone's %.4f)\n", out.size() / 2, step, steepest);
    CHECK(step < 1.15 * steepest && same);
    // Reset: a new stream starts at the speed asked for, from its first frame.
    t.Reset(); CHECK(t.SetSpeed(1.5f) && t.Pushed() == 0 && t.Pulled() == 0 && t.Passing());
    const auto again = Run(t, Tone(330, 2.0, 2), 2);
    CHECK(!t.Passing() && t.LastChange().output == 0 && t.LastChange().input == 0 && t.LastChange().speed == 1.5f && !again.empty());
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // Progress shows as it happens (a slow build is visibly slow, not hung).
    std::puts("Time stretch tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    Copy(); Pitch(); Changes();
    std::printf("Time stretch: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
