// MicroVad (energy + zero-crossing voice activity detection) and VoiceGate (its junction with UtteranceBuffer and the
// command grammar), on synthetic signals whose speech boundaries are known to the sample:
// - "speech": a harmonic vowel (120 Hz pitch, formants near 600, 1200 and 2500 Hz) with raised-cosine edges, led in
//   by a weak fricative (high-passed noise), in words separated by pauses;
// - backgrounds: white noise at several levels, digital silence, mains hum, clicks, tone bursts, a steady tone.
// Checked: segment boundaries against the truth, the hangover (configurable), what is delivered (bit-exact, contiguous,
// with pre-roll) and what is discarded, false positives, independence from how the stream is split, other sample rates
// and window lengths, hostile samples, and no allocation anywhere after Configure.
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ai/VoiceGate.hpp"
#include "media/audio/MicroVad.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

using namespace media;
using namespace media::audio;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 40) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
std::uint64_t allocations = 0;

struct Rng {
    std::uint32_t state = 0x1234567u;
    std::uint32_t Bits() noexcept { state = state * 1664525u + 1013904223u; return state >> 8; }
    float Next() noexcept { return static_cast<float>(Bits()) / 8388608.0f - 1.0f; } // Uniform in [-1, 1).
};
// One synthetic recording. Everything is added, so a background can lie under the speech.
constexpr unsigned kMaxFrames = 48000 * 12;
float stream[kMaxFrames];
struct Signal {
    unsigned rate = 16000, frames = 0;
    Rng rng;
    void Clear(unsigned sample_rate, double seconds) {
        rate = sample_rate; frames = static_cast<unsigned>(seconds * rate); rng = {};
        std::memset(stream, 0, sizeof(float) * frames);
    }
    unsigned At(double seconds) const { return (std::min)(frames, static_cast<unsigned>(seconds * rate + 0.5)); }
    static double Amplitude(double db) { return std::pow(10.0, db / 20.0); }
    // White noise with the given RMS level (dBFS).
    void Noise(double from, double to, double db) {
        const auto scale = static_cast<float>(Amplitude(db) * std::sqrt(3.0));
        for (unsigned i = At(from); i < At(to); ++i) stream[i] += scale * rng.Next();
    }
    // Raised-cosine edges: `attack` and `release` seconds.
    static double Edge(double t, double length, double attack, double release) {
        if (t < attack) return 0.5 - 0.5 * std::cos(kPi * t / attack);
        if (t > length - release) return 0.5 - 0.5 * std::cos(kPi * (length - t) / release);
        return 1.0;
    }
    // A vowel: harmonics of the pitch under three formants, at the given RMS level in its steady part.
    void Vowel(double from, double to, double db, double pitch = 120) {
        double weights[32]{}, power = 0; unsigned harmonics = 0;
        for (unsigned k = 1; k <= 32 && k * pitch < 3500 && k * pitch < rate * 0.45; ++k) {
            const double f = k * pitch;
            const auto formant = [&](double centre, double width, double gain) { return gain * std::exp(-0.5 * (f - centre) * (f - centre) / (width * width)); };
            weights[k - 1] = (formant(600, 150, 1.0) + formant(1200, 200, 0.5) + formant(2500, 300, 0.25) + 0.05) / k;
            power += 0.5 * weights[k - 1] * weights[k - 1]; harmonics = k;
        }
        const double scale = Amplitude(db) / std::sqrt(power), length = to - from;
        for (unsigned i = At(from); i < At(to); ++i) {
            const double t = static_cast<double>(i) / rate - from; double v = 0;
            for (unsigned k = 1; k <= harmonics; ++k) v += weights[k - 1] * std::sin(2 * kPi * k * pitch * t + 0.7 * k);
            stream[i] += static_cast<float>(scale * Edge(t, length, 0.015, 0.040) * v);
        }
    }
    // An unvoiced consonant: differenced (high-passed) noise.
    void Fricative(double from, double to, double db) {
        const auto scale = static_cast<float>(Amplitude(db) * std::sqrt(1.5)); float previous = rng.Next();
        const double length = to - from;
        for (unsigned i = At(from); i < At(to); ++i) {
            const float n = rng.Next(); const double t = static_cast<double>(i) / rate - from;
            stream[i] += static_cast<float>(scale * Edge(t, length, 0.005, 0.005)) * (n - previous); previous = n;
        }
    }
    // A voiced sound that is mostly its fundamental (a nasal, a closed vowel, a hummed note): harmonics falling as 1/k.
    void Hummed(double from, double to, double db, double pitch) {
        double power = 0; for (unsigned k = 1; k <= 12; ++k) power += 0.5 / (k * k);
        const double scale = Amplitude(db) / std::sqrt(power), length = to - from;
        for (unsigned i = At(from); i < At(to); ++i) {
            const double t = static_cast<double>(i) / rate - from; double v = 0;
            for (unsigned k = 1; k <= 12; ++k) v += std::sin(2 * kPi * k * pitch * t + 0.7 * k) / k;
            stream[i] += static_cast<float>(scale * Edge(t, length, 0.015, 0.040) * v);
        }
    }
    void Tone(double from, double to, double hz, double db) {
        const double a = Amplitude(db) * std::sqrt(2.0);
        for (unsigned i = At(from); i < At(to); ++i) stream[i] += static_cast<float>(a * std::sin(2 * kPi * hz * i / rate));
    }
    // A word: 60 ms of fricative 14 dB below a vowel that fills the rest. Returns its end.
    double Word(double at, double length, double db, double pitch = 120) {
        Fricative(at, at + 0.06, db - 14); Vowel(at + 0.06, at + length, db, pitch); return at + length;
    }
};
Signal signal;

// What the detector delivered: utterances as (first frame, frames, where the end was announced), whether the slices
// were in order and contiguous, and whether every delivered sample is the input sample at that position.
struct Collector {
    struct Item { std::uint64_t begin = 0, frames = 0, closed_at = 0; bool closed = false; };
    Item items[64]{};
    unsigned count = 0, slices = 0;
    bool open = false, orderly = true, exact = true;
    std::uint64_t next = 0;
    static void Sink(void* context, const SpeechSlice& s) noexcept {
        auto& c = *static_cast<Collector*>(context); ++c.slices;
        if (s.state == VadState::SpeechEnd) {
            if (!c.open || s.frames || s.samples || s.utterance != c.count) c.orderly = false;
            if (c.count && c.count <= 64) { c.items[c.count - 1].closed_at = s.first_frame; c.items[c.count - 1].closed = true; }
            c.open = false; return;
        }
        if (s.state == VadState::SpeechStart) {
            if (c.open) c.orderly = false;
            c.open = true; ++c.count; c.next = s.first_frame;
            if (c.count <= 64) c.items[c.count - 1].begin = s.first_frame;
        }
        if (s.state == VadState::Silence || !c.open || s.first_frame != c.next || !s.frames || !s.samples || s.utterance != c.count) c.orderly = false;
        if (s.samples && s.first_frame + s.frames <= signal.frames && std::memcmp(stream + s.first_frame, s.samples, sizeof(float) * s.frames) != 0) c.exact = false;
        c.next = s.first_frame + s.frames;
        if (c.count && c.count <= 64) c.items[c.count - 1].frames += s.frames;
    }
};
core::LinearMemoryArena arena;
// Feeds the whole signal in blocks of `block` frames; measured for allocations.
void Feed(MicroVad& vad, Collector& collector, unsigned block = 480) {
    allocation_probe::Begin();
    for (unsigned at = 0; at < signal.frames; at += block)
        (void)vad.Process({stream + at, (std::min)(block, signal.frames - at)}, &Collector::Sink, &collector);
    allocations += allocation_probe::End();
}
bool Near(std::uint64_t frame, double seconds, double before_ms, double after_ms) {
    const double ms = static_cast<double>(frame) * 1000.0 / signal.rate - seconds * 1000.0;
    return ms >= -before_ms && ms <= after_ms;
}
VadOptions Options(unsigned rate = 16000, unsigned window_ms = 10) { VadOptions o; o.sample_rate = rate; o.window_ms = window_ms; return o; }

void Configuration() {
    MicroVad vad;
    CHECK(vad.Acquire().empty()); CHECK(vad.Process({stream, 16}, nullptr, nullptr) == VadState::Silence); // Unconfigured: inert.
    const auto refused = [](auto change) { MicroVad v; VadOptions o; change(o); return !v.Configure(arena, o); };
    CHECK(refused([](VadOptions& o) { o.sample_rate = 7999; })); CHECK(refused([](VadOptions& o) { o.sample_rate = 192001; }));
    CHECK(refused([](VadOptions& o) { o.window_ms = 9; })); CHECK(refused([](VadOptions& o) { o.window_ms = 21; }));
    CHECK(refused([](VadOptions& o) { o.onset_ms = 5; })); CHECK(refused([](VadOptions& o) { o.hangover_ms = 0; }));
    CHECK(refused([](VadOptions& o) { o.hold_db = 0; })); CHECK(refused([](VadOptions& o) { o.onset_db = 3; }));
    CHECK(refused([](VadOptions& o) { o.lookback_ms = 50; })); CHECK(refused([](VadOptions& o) { o.max_utterance_ms = 100; }));
    CHECK(refused([](VadOptions& o) { o.min_level_db = 1; })); CHECK(refused([](VadOptions& o) { o.zero_cross_max_hz = 50; }));
    CHECK(refused([](VadOptions& o) { o.floor_rise_db_s = std::numeric_limits<float>::quiet_NaN(); }));
    CHECK(refused([](VadOptions& o) { o.stationary_ms = 20; }));
    CHECK(vad.Configure(arena)); CHECK(!vad.Configure(arena)); // Once.
    CHECK(vad.WindowFrames() == 160 && vad.Acquire().size() == 160 && vad.State() == VadState::Silence && !vad.InUtterance());
    CHECK(vad.Commit(161, nullptr, nullptr) == VadState::Silence && vad.Stats().frames == 0); // More than was acquired: refused.
}
// Two utterances in quiet room noise: a single word, then three words with pauses shorter than the hangover.
void Segments(unsigned rate, unsigned window_ms, double noise_db, double onset_before, double onset_after, double end_before, double end_after) {
    signal.Clear(rate, 7.0); signal.Noise(0, 7.0, noise_db);
    const double a0 = 1.0, a1 = signal.Word(a0, 0.40, -20);
    const double b0 = 2.4; double b1 = signal.Word(b0, 0.30, -18, 110);
    b1 = signal.Word(b1 + 0.15, 0.35, -20, 130); b1 = signal.Word(b1 + 0.18, 0.30, -22, 100);
    MicroVad vad; CHECK(vad.Configure(arena, Options(rate, window_ms)));
    Collector c; c = {}; Feed(vad, c);
    // The hangover is whole windows (250 ms rounded up); the pre-roll is exact.
    const auto window = vad.WindowFrames(); const std::uint64_t hangover = static_cast<std::uint64_t>((250 + window_ms - 1) / window_ms) * window, preroll = rate / 10;
    CHECK(c.count == 2 && c.orderly && c.exact && !c.open && vad.Stats().utterances == 2 && vad.State() == VadState::Silence);
    if (c.count == 2) {
        const double truth[2][2]{{a0, a1}, {b0, b1}};
        for (unsigned i = 0; i < 2; ++i) {
            const auto& u = c.items[i];
            // The utterance opens one pre-roll before the first active window, and that window is where speech began.
            CHECK(Near(u.begin + preroll, truth[i][0], onset_before, onset_after));
            // It is announced closed one hangover after the last active window, which is where speech ended.
            CHECK(u.closed && Near(u.closed_at - hangover, truth[i][1], end_before, end_after));
            CHECK(u.frames == u.closed_at - u.begin && u.closed_at % window == 0);
        }
        CHECK(c.items[1].begin >= c.items[0].closed_at); // Nothing is delivered twice.
        const auto& last = vad.Last();
        CHECK(last.index == 2 && last.begin_frame == c.items[1].begin && last.delivered == c.items[1].frames && !last.truncated);
        CHECK(last.onset_frame == last.begin_frame + preroll && last.end_frame + hangover == c.items[1].closed_at && last.active <= last.end_frame - last.onset_frame);
    }
    // Silence is discarded: what was delivered is the two utterances (with pre-roll and hangover), not the 7 seconds.
    const auto& s = vad.Stats();
    CHECK(s.frames == static_cast<std::uint64_t>(signal.frames / window) * window && s.delivered == c.items[0].frames + c.items[1].frames);
    CHECK(s.delivered < s.frames * 40 / 100 && s.delivered > s.frames * 25 / 100 && s.truncated == 0);
    std::printf("Segments %u Hz, %u ms windows, noise %.0f dBFS: [%.3f, %.3f] s and [%.3f, %.3f] s (truth [%.3f, %.3f], [%.3f, %.3f]); %.1f%% of %llu frames delivered\n",
                rate, window_ms, noise_db, static_cast<double>(c.items[0].begin + preroll) / rate, static_cast<double>(c.items[0].closed_at - hangover) / rate,
                static_cast<double>(c.items[1].begin + preroll) / rate, static_cast<double>(c.items[1].closed_at - hangover) / rate, a0, a1, b0, b1,
                100.0 * static_cast<double>(s.delivered) / static_cast<double>(s.frames), static_cast<unsigned long long>(s.frames));
}
// Two words `pause` seconds apart: one utterance when the pause is shorter than the hangover, else two.
unsigned Utterances(double pause, unsigned hangover_ms) {
    signal.Clear(16000, 4.0); signal.Noise(0, 4.0, -60);
    const double end = signal.Word(1.0, 0.30, -20); (void)signal.Word(end + pause, 0.30, -20);
    VadOptions o; o.hangover_ms = hangover_ms;
    MicroVad vad; CHECK(vad.Configure(arena, o));
    Collector c; c = {}; Feed(vad, c);
    CHECK(c.orderly && c.exact && !c.open);
    return c.count;
}
void Hysteresis() {
    CHECK(Utterances(0.20, 250) == 1); CHECK(Utterances(0.30, 250) == 2);
    CHECK(Utterances(0.30, 400) == 1); CHECK(Utterances(0.45, 400) == 2);
    CHECK(Utterances(0.10, 100) == 2); CHECK(Utterances(0.05, 100) == 1);
    // A weak sound never opens an utterance but keeps one open: 8 dB above the floor is between hold_db and onset_db.
    signal.Clear(16000, 4.0); signal.Noise(0, 4.0, -60);
    const double end = signal.Word(1.0, 0.30, -20);
    signal.Vowel(end + 0.05, end + 0.55, -52); // In the utterance: holds it open.
    signal.Vowel(3.0, 3.5, -52);               // In silence: nothing.
    MicroVad vad; CHECK(vad.Configure(arena));
    Collector c; c = {}; Feed(vad, c);
    CHECK(c.count == 1 && c.orderly && Near(c.items[0].closed_at - 4000, end + 0.55, 60, 20));
    // Voiced sounds whose zero crossings follow the pitch, not a formant: a low voice at 90 Hz and a high one at 220 Hz.
    // (The real capture found this: a 150 Hz harmonic word was refused while the voiced band began at 160 Hz.)
    signal.Clear(16000, 4.0); signal.Noise(0, 4.0, -60);
    signal.Hummed(1.0, 1.4, -20, 90); signal.Hummed(2.5, 2.9, -20, 220);
    MicroVad low; CHECK(low.Configure(arena));
    Collector d; Feed(low, d);
    CHECK(d.count == 2 && d.orderly && d.exact);
    if (d.count == 2) CHECK(Near(d.items[0].begin + 1600, 1.0, 10, 20) && Near(d.items[0].closed_at - 4000, 1.4, 20, 15) &&
                            Near(d.items[1].begin + 1600, 2.5, 10, 20) && Near(d.items[1].closed_at - 4000, 2.9, 20, 15));
}
unsigned CountIn(const VadOptions& o, Collector& c, MicroVad& vad) {
    CHECK(vad.Configure(arena, o)); c = {}; Feed(vad, c);
    CHECK(c.orderly && c.exact);
    return c.count;
}
void FalsePositives() {
    MicroVad vads[10]; Collector c; unsigned n = 0;
    // Digital silence, then room noise appearing out of it.
    signal.Clear(16000, 6.0); signal.Noise(2.0, 6.0, -58);
    CHECK(CountIn({}, c, vads[n]) == 0); CHECK(vads[n].Stats().delivered == 0 && vads[n].Features().floor_db > -62); ++n;
    // White noise that steps from -60 to -40 to -20 dBFS and back: loud, but not speech.
    signal.Clear(16000, 10.0); signal.Noise(0, 10.0, -60); signal.Noise(2.0, 8.0, -40); signal.Noise(4.0, 6.0, -20);
    CHECK(CountIn({}, c, vads[n]) == 0); CHECK(vads[n].Stats().active_windows > 0); ++n; // Active (above the floor), never voiced.
    // Mains hum switched on at -20 dBFS.
    signal.Clear(16000, 6.0); signal.Noise(0, 6.0, -60); signal.Tone(1.5, 6.0, 50, -20); signal.Tone(1.5, 6.0, 100, -40);
    CHECK(CountIn({}, c, vads[n]) == 0); ++n;
    // Full-scale clicks and 5 ms tone bursts, three a second: in the voiced band, but never for onset_ms.
    signal.Clear(16000, 6.0); signal.Noise(0, 6.0, -60);
    for (double t = 1.0; t < 5.5; t += 0.33) { stream[signal.At(t)] = 1.0f; signal.Tone(t + 0.15, t + 0.155, 1000, -3); }
    CHECK(CountIn({}, c, vads[n]) == 0); ++n;
    // A steady 300 Hz tone at -30 dBFS looks voiced. It is the known limit of energy and zero
    // crossings: one utterance opens. But a level that holds still is not speech: it closes within about a second,
    // and the tone is then the noise floor, not a second utterance. Speech 12 dB above it is still found.
    signal.Clear(16000, 9.0); signal.Noise(0, 9.0, -60); signal.Tone(1.0, 9.0, 300, -30);
    const double word = 5.0, word_end = signal.Word(word, 0.40, -12);
    CHECK(CountIn({}, c, vads[n]) == 2);
    if (c.count == 2) {
        CHECK(c.items[0].closed && Near(c.items[0].begin + 1600, 1.0, 10, 20) && c.items[0].closed_at - c.items[0].begin <= 16000 * 13 / 10);
        CHECK(Near(c.items[1].begin + 1600, word, 10, 80) && Near(c.items[1].closed_at - 4000, word_end, 40, 20));
    }
    std::printf("Steady tone: utterance of %.2f s, then the floor is %.1f dBFS\n", static_cast<double>(c.items[0].frames) / 16000, static_cast<double>(vads[n].Features().floor_db));
    ++n;
    // A vowel held for three seconds is cut the same way (a limit: sustained singing is not an utterance).
    signal.Clear(16000, 6.0); signal.Noise(0, 6.0, -60); signal.Vowel(1.0, 4.0, -20);
    CHECK(CountIn({}, c, vads[n]) == 1); CHECK(c.items[0].closed && c.items[0].frames < 16000 * 13 / 10); ++n;
    // Speech in noise only 20 dB below it: found, with a looser start (the weak consonant is inside the noise).
    signal.Clear(16000, 5.0); signal.Noise(0, 5.0, -40);
    const double start = 1.5, end = signal.Word(start, 0.45, -20);
    CHECK(CountIn({}, c, vads[n]) == 1);
    if (c.count == 1) CHECK(Near(c.items[0].begin + 1600, start, 10, 90) && Near(c.items[0].closed_at - 4000, end, 40, 20));
    ++n;
    // Too quiet to be speech whatever the floor: a vowel at -62 dBFS over digital silence.
    signal.Clear(16000, 4.0); signal.Vowel(1.0, 1.5, -62); signal.Vowel(2.0, 2.5, -62);
    CHECK(CountIn({}, c, vads[n]) == 0); ++n;
    // The same at -58 dBFS is loud enough to hold an utterance open, and far above a floor of digital silence, but
    // below min_level_db: it does not open one.
    signal.Clear(16000, 4.0); signal.Vowel(1.0, 1.5, -58); signal.Vowel(2.0, 2.5, -58);
    CHECK(CountIn({}, c, vads[n]) == 0); CHECK(vads[n].Stats().active_windows > 50); ++n;
    // The floor comes down as fast as the noise does: a word 36 dB above quiet noise, half a second after loud noise
    // stopped, is found (it is only 6 dB above the loud noise's level).
    signal.Clear(16000, 6.0); signal.Noise(0, 6.0, -60); signal.Noise(0, 3.0, -30);
    const double after = 3.5, after_end = signal.Word(after, 0.40, -24);
    CHECK(CountIn({}, c, vads[n]) == 1);
    if (c.count == 1) CHECK(Near(c.items[0].begin + 1600, after, 10, 10) && Near(c.items[0].closed_at - 4000, after_end, 20, 15));
    ++n;
}
// The same recording, split differently, gives the same utterances to the frame.
struct Uneven {
    std::uint64_t at = 0; Rng rng; unsigned calls = 0;
    std::uint32_t Read(std::span<float> out) noexcept {
        if (++calls % 5 == 0 || at == signal.frames) return 0; // The ring is empty for now.
        const auto count = static_cast<std::uint32_t>((std::min<std::uint64_t>)({out.size(), signal.frames - at, 1 + rng.Bits() % 97}));
        std::memcpy(out.data(), stream + at, sizeof(float) * count); at += count; return count;
    }
};
void Splits() {
    signal.Clear(16000, 7.0); signal.Noise(0, 7.0, -55);
    double t = signal.Word(0.8, 0.35, -20); t = signal.Word(t + 0.12, 0.30, -24, 140); (void)signal.Word(3.0, 0.5, -18, 95);
    (void)signal.Word(4.4, 0.25, -21); (void)signal.Word(5.3, 0.6, -19, 150);
    Collector reference; MicroVad first; CHECK(first.Configure(arena)); reference = {}; Feed(first, reference, 160);
    CHECK(reference.count == 4 && reference.orderly && reference.exact);
    for (const unsigned block : {1u, 7u, 159u, 161u, 333u, 4096u, 112000u}) {
        MicroVad vad; CHECK(vad.Configure(arena)); Collector c; c = {}; Feed(vad, c, block);
        CHECK(c.count == reference.count && c.orderly && c.exact && vad.Stats().delivered == first.Stats().delivered && vad.Stats().windows == first.Stats().windows);
        for (unsigned i = 0; i < c.count && i < reference.count; ++i)
            CHECK(c.items[i].begin == reference.items[i].begin && c.items[i].frames == reference.items[i].frames && c.items[i].closed_at == reference.items[i].closed_at);
        const auto &a = vad.Last(), &b = first.Last();
        CHECK(a.index == b.index && a.begin_frame == b.begin_frame && a.onset_frame == b.onset_frame && a.end_frame == b.end_frame &&
              a.delivered == b.delivered && a.active == b.active && a.truncated == b.truncated && vad.Features().floor_db == first.Features().floor_db);
    }
    // Straight from a source that returns what it has (the capture ring's Read), without a copy of our own.
    MicroVad vad; CHECK(vad.Configure(arena)); Collector c; c = {}; Uneven source;
    allocation_probe::Begin();
    std::uint64_t taken = 0;
    while (source.at < signal.frames) taken += vad.Drain(source, &Collector::Sink, &c);
    allocations += allocation_probe::End();
    CHECK(taken == signal.frames && c.count == reference.count && c.orderly && c.exact && vad.Stats().delivered == first.Stats().delivered);
    for (unsigned i = 0; i < c.count && i < reference.count; ++i) CHECK(c.items[i].begin == reference.items[i].begin && c.items[i].closed_at == reference.items[i].closed_at);
}
void Limits() {
    // Twelve seconds of words 120 ms apart never pause for a hangover: the utterance is cut at max_utterance_ms and
    // the next one opens where it ended.
    signal.Clear(16000, 12.0); signal.Noise(0, 12.0, -60);
    for (double t = 1.0; t < 10.5;) t = signal.Word(t, 0.30, -20, 100 + 10 * static_cast<int>(t)) + 0.12;
    VadOptions o; o.max_utterance_ms = 2000;
    MicroVad vad; CHECK(vad.Configure(arena, o)); Collector c; c = {}; Feed(vad, c);
    CHECK(c.count >= 4 && c.count <= 6 && c.orderly && c.exact && vad.Stats().truncated >= c.count - 1);
    for (unsigned i = 0; i + 1 < c.count; ++i) CHECK(c.items[i].frames == 32000 && c.items[i + 1].begin >= c.items[i].closed_at && c.items[i + 1].begin <= c.items[i].closed_at + 16000 * 9 / 100);
    // Reset in the middle of an utterance: no end is announced for it, positions restart, the floor warms up again.
    signal.Clear(16000, 3.0); signal.Noise(0, 3.0, -60); (void)signal.Word(1.0, 1.0, -20);
    MicroVad second; CHECK(second.Configure(arena)); Collector d; d = {};
    allocation_probe::Begin();
    (void)second.Process({stream, 16000 + 8000}, &Collector::Sink, &d);
    CHECK(second.InUtterance() && d.count == 1 && d.open);
    second.Reset(); d = {};
    CHECK(!second.InUtterance() && second.State() == VadState::Silence && second.Stats().frames == 0 && second.Acquire().size() == 160);
    (void)second.Process({stream, signal.frames}, &Collector::Sink, &d);
    allocations += allocation_probe::End();
    CHECK(d.count == 1 && d.orderly && d.exact && d.items[0].closed && Near(d.items[0].begin + 1600, 1.0, 10, 20));
    // Hostile samples: NaN, infinities, full-scale square waves, denormals. Nothing faults; the features stay finite.
    signal.Clear(16000, 4.0);
    for (unsigned i = 0; i < signal.frames; ++i) {
        const unsigned k = i / 1600;
        stream[i] = k % 5 == 0 ? std::numeric_limits<float>::quiet_NaN() : k % 5 == 1 ? ((i / 20) % 2 ? 1.0f : -1.0f) :
                    k % 5 == 2 ? std::numeric_limits<float>::infinity() : k % 5 == 3 ? std::numeric_limits<float>::denorm_min() * static_cast<float>(i % 3) : -std::numeric_limits<float>::infinity();
    }
    MicroVad third; CHECK(third.Configure(arena)); Collector e; e = {};
    allocation_probe::Begin();
    bool finite = true;
    for (unsigned at = 0; at < signal.frames; at += 160) {
        (void)third.Process({stream + at, 160}, &Collector::Sink, &e);
        const auto& f = third.Features(); finite = finite && std::isfinite(f.level_db) && std::isfinite(f.floor_db) && std::isfinite(f.zero_cross_hz) && f.floor_db >= -80;
    }
    allocations += allocation_probe::End();
    CHECK(finite && e.orderly && third.Stats().windows == 400);
}
// The junction with the command grammar. The "recogniser" here knows nothing about sound: it answers with the next
// scripted phrase once it has been given half a second of an utterance, in pieces as a streaming recogniser would.
struct Script {
    const char* const* phrases = nullptr; unsigned count = 0, next = 0;
    std::uint64_t heard = 0, total = 0; unsigned calls = 0; bool said = false;
    static void Hear(void* context, const SpeechSlice& slice, ai::UtteranceBuffer& text) noexcept {
        auto& s = *static_cast<Script*>(context); ++s.calls; s.total += slice.frames;
        if (slice.state == VadState::SpeechStart) { s.heard = 0; s.said = false; }
        s.heard += slice.frames;
        if (s.said || s.heard < 8000 || s.next >= s.count) return;
        s.said = true;
        const std::string_view phrase = s.phrases[s.next++];
        // Word by word; a phrase of 'x' overflows the buffer.
        if (phrase == "x") { for (unsigned i = 0; i < 300; ++i) (void)text.Push("xx"); return; }
        for (std::size_t at = 0; at < phrase.size();) {
            auto space = phrase.find(' ', at); if (space == std::string_view::npos) space = phrase.size();
            (void)text.Push(phrase.substr(at, space - at)); at = space + 1;
        }
    }
};
struct Commands {
    ai::LocalIntentDispatcher dispatcher; ai::MediaBounds bounds;
    ai::Intent intents[8]{}; unsigned count = 0; double seconds[8]{};
    static void Heard(void* context, std::string_view text, const audio::Utterance& u) noexcept {
        auto& c = *static_cast<Commands*>(context);
        if (c.count < 8) { c.seconds[c.count] = static_cast<double>(u.onset_frame) / 16000; c.intents[c.count++] = c.dispatcher.Dispatch(text, c.bounds); }
    }
    static ai::IntentStatus Execute(void*, const ai::Intent&) noexcept { return ai::IntentStatus::Ok; }
};
void Gate() {
    // Seven sounds: three commands, a knock too short to be a word, a phrase the grammar does not know, a phrase too
    // long for a command, and a fourth command.
    signal.Clear(16000, 11.5); signal.Noise(0, 11.5, -58);
    (void)signal.Word(1.0, 0.45, -20); (void)signal.Word(2.3, 0.60, -19, 105); (void)signal.Word(3.8, 0.70, -21, 135);
    signal.Vowel(5.2, 5.27, -18);
    (void)signal.Word(6.0, 0.5, -20); (void)signal.Word(7.5, 0.5, -20); (void)signal.Word(9.0, 0.5, -20, 90);
    static const char* const phrases[]{"pause", "volume 40", "rewind 30s", "purple elephants", "x", "mute"};
    Script script; script.phrases = phrases; script.count = 6;
    Commands commands; commands.dispatcher.Configure(&Commands::Execute, nullptr);
    commands.bounds.duration_100ns = 6000000000ll; commands.bounds.position_100ns = 1200000000ll; commands.bounds.seekable = true;
    MicroVad vad; CHECK(vad.Configure(arena));
    ai::VoiceGate gate; gate.Configure(vad, &Script::Hear, &script, &Commands::Heard, &commands);
    Uneven source;
    allocation_probe::Begin();
    while (source.at < signal.frames) (void)gate.Pump(source);
    allocations += allocation_probe::End();
    const auto s = gate.Stats(); // A copy: the gate is configured again below.
    CHECK(s.utterances == 7 && s.handed == 5 && s.too_short == 1 && s.overflowed == 1 && s.empty == 0 && gate.Text().Empty());
    // The recogniser was given exactly what the detector delivered: speech, not the 11.5 seconds.
    CHECK(script.total == vad.Stats().delivered && s.speech_frames == script.total && script.total < signal.frames * 60 / 100);
    using ai::IntentKind; using ai::IntentStatus;
    CHECK(commands.count == 5);
    CHECK(commands.intents[0].kind == IntentKind::Pause && commands.intents[0].status == IntentStatus::Ok);
    CHECK(commands.intents[1].kind == IntentKind::VolumeSet && commands.intents[1].status == IntentStatus::Ok && std::fabs(commands.intents[1].volume - 0.40f) < 1e-6f);
    CHECK(commands.intents[2].kind == IntentKind::SeekRelative && commands.intents[2].status == IntentStatus::Ok && commands.intents[2].offset_100ns == -300000000ll);
    CHECK(commands.intents[3].status != IntentStatus::Ok); // Heard, not a command: refused, and nothing searched (Dispatch, not Submit).
    CHECK(commands.intents[4].kind == IntentKind::Mute && commands.intents[4].status == IntentStatus::Ok);
    CHECK(std::fabs(commands.seconds[0] - 1.0) < 0.02 && std::fabs(commands.seconds[4] - 9.0) < 0.02);
    const auto& d = commands.dispatcher.Stats();
    CHECK(d.commands == 5 && d.executed == 4 && d.refused == 1);
    // An abandoned utterance (a gap in the capture) is never handed on.
    Script again; again.phrases = phrases; again.count = 1;
    gate.Configure(vad, &Script::Hear, &again, &Commands::Heard, &commands);
    gate.Reset();
    (void)gate.Process({stream, 16000 + 8000}); CHECK(vad.InUtterance() && !gate.Text().Empty());
    gate.Reset(); CHECK(gate.Text().Empty() && gate.Stats().utterances == 0 && commands.count == 5);
    std::printf("Voice gate: %u utterances, %u commands dispatched (%u executed), %u too short, %u overflowed; recogniser given %.2f s of %.2f s\n",
                s.utterances, s.handed, d.executed, s.too_short, s.overflowed, static_cast<double>(script.total) / 16000, static_cast<double>(signal.frames) / 16000);
}
void Cost() {
    signal.Clear(16000, 10.0); signal.Noise(0, 10.0, -55);
    for (double t = 0.8; t < 9.0;) t = signal.Word(t, 0.4, -20) + 0.5;
    MicroVad vad; CHECK(vad.Configure(arena)); Collector c; c = {};
    double best = 1e9;
    for (unsigned round = 0; round < 5; ++round) {
        vad.Reset(); c = {};
        const auto begin = std::chrono::steady_clock::now();
        Feed(vad, c, 160);
        best = (std::min)(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
    }
    CHECK(vad.Stats().windows == 1000 && c.count >= 8);
    std::printf("Cost: %.2f us per 10 ms window (%.4f%% of real time)\n", best / 1000, best / 1000 / 100);
}
}
int main() {
    CHECK(arena.Initialize(64 * 1024 * 1024));
    Configuration();
    Segments(16000, 10, -60, 10, 10, 20, 15);
    Segments(16000, 20, -60, 20, 20, 30, 25);
    Segments(48000, 10, -60, 10, 10, 20, 15);
    Segments(8000, 20, -55, 20, 20, 30, 25);
    Segments(44100, 15, -60, 15, 15, 25, 20);
    Hysteresis();
    FalsePositives();
    Splits();
    Limits();
    Gate();
    Cost();
    // Storage comes from the arena at Configure only: a sealed arena refuses a new detector.
    CHECK(arena.Seal()); MicroVad late; CHECK(!late.Configure(arena));
    CHECK(allocations == 0);
    std::printf("VAD: %s (%d failures, %llu allocations)\n", failures ? "FAIL" : "PASS", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
