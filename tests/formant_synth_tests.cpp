// ai::FormantSynthesizer (docs/voice.md, "Synthesised speech"): what it renders is measured, not listened to.
// Formants by LPC, pitch by autocorrelation, fricative bands by spectral centroid, durations, word spans,
// determinism, reverberation and allocation. Portable; MSVC and GCC (-O2, ASan/UBSan).
#include "media/ai/FormantSynthesizer.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using media::ai::FormantSynthesizer;
using media::ai::SynthVoice;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 40) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = FormantSynthesizer::kSampleRate;
FormantSynthesizer synth; // ~40 KB: not on the stack.
std::vector<float> Say(const char* phonemes, const SynthVoice& voice = {}) {
    std::vector<float> out(FormantSynthesizer::kSampleRate * 8);
    const auto n = synth.Speak(phonemes, voice, out);
    out.resize(n);
    return out;
}
// Formant frequencies of a frame: peaks of the order-14 LPC envelope (autocorrelation method, Hann window,
// pre-emphasis), up to 5 kHz.
std::vector<double> Formants(const std::vector<float>& x, std::size_t centre, std::size_t length = 1024) {
    constexpr unsigned order = 14;
    std::vector<double> w(length);
    for (std::size_t i = 0; i < length; ++i) {
        const std::size_t at = centre - length / 2 + i;
        const double pre = x[at] - 0.97 * x[at - 1];
        w[i] = pre * (0.5 - 0.5 * std::cos(2 * kPi * i / (length - 1)));
    }
    double r[order + 1]{};
    for (unsigned k = 0; k <= order; ++k) for (std::size_t i = k; i < length; ++i) r[k] += w[i] * w[i - k];
    double a[order + 1]{1}, e = r[0] * (1 + 1e-9);
    for (unsigned i = 1; i <= order; ++i) {
        double acc = r[i]; for (unsigned j = 1; j < i; ++j) acc -= a[j] * r[i - j];
        const double k = acc / e; double next[order + 1]; std::memcpy(next, a, sizeof(a));
        next[i] = k; for (unsigned j = 1; j < i; ++j) next[j] = a[j] - k * a[i - j];
        std::memcpy(a, next, sizeof(a)); e *= 1 - k * k;
    }
    std::vector<double> envelope;
    for (double hz = 0; hz <= 5000; hz += 5) {
        double re = 1, im = 0;
        for (unsigned j = 1; j <= order; ++j) { re -= a[j] * std::cos(2 * kPi * hz * j / kRate); im += a[j] * std::sin(2 * kPi * hz * j / kRate); }
        envelope.push_back(1 / (re * re + im * im));
    }
    std::vector<double> peaks;
    for (std::size_t i = 1; i + 1 < envelope.size(); ++i) if (envelope[i] > envelope[i - 1] && envelope[i] >= envelope[i + 1] && i * 5 > 150) peaks.push_back(i * 5.0);
    return peaks;
}
bool Near(const std::vector<double>& peaks, double hz, double tolerance) {
    for (double p : peaks) if (std::fabs(p - hz) <= tolerance * hz) return true;
    return false;
}
double Pitch(const std::vector<float>& x, std::size_t centre) {
    // Normalised autocorrelation between 60 and 400 Hz; the shortest lag within 3 % of the best (multiples of the
    // period correlate as well).
    const unsigned low = static_cast<unsigned>(kRate / 400), high = static_cast<unsigned>(kRate / 60);
    std::vector<double> v(high + 1, -1); double best = -1;
    for (unsigned l = low; l <= high; ++l) {
        double c = 0, e0 = 0, e1 = 0;
        for (std::size_t i = centre - 400; i < centre + 400; ++i) { c += x[i] * x[i + l]; e0 += x[i] * x[i]; e1 += x[i + l] * x[i + l]; }
        v[l] = c / std::sqrt(e0 * e1 + 1e-30); best = (std::max)(best, v[l]);
    }
    for (unsigned l = low + 1; l < high; ++l) if (v[l] >= 0.97 * best && v[l] >= v[l - 1] && v[l] >= v[l + 1]) return kRate / l;
    return 0;
}
double Centroid(const std::vector<float>& x, std::size_t centre, std::size_t length = 512) { // Spectral centroid, Hz (DFT).
    double num = 0, den = 0;
    for (unsigned bin = 1; bin < length / 2; ++bin) {
        double re = 0, im = 0;
        for (std::size_t i = 0; i < length; ++i) {
            const double v = x[centre - length / 2 + i] * (0.5 - 0.5 * std::cos(2 * kPi * i / (length - 1)));
            re += v * std::cos(2 * kPi * bin * i / length); im -= v * std::sin(2 * kPi * bin * i / length);
        }
        const double power = re * re + im * im, hz = bin * kRate / length;
        num += hz * power; den += power;
    }
    return num / den;
}
double Rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double s = 0; for (std::size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(to - from));
}
void Vowels() {
    // A held vowel's formants are its targets: F1 within 12 % (pitch harmonics are 115 Hz apart), F2 within 8 %.
    struct V { const char* text; double f1, f2; } vowels[]{{"'a", 730, 1300}, {"'e", 480, 1850}, {"'i", 300, 2250}, {"'o", 500, 900}, {"'u", 330, 800}, {"'ae", 690, 1660}};
    for (const auto& v : vowels) {
        SynthVoice voice; voice.jitter = 0; voice.shimmer = 0; voice.breathiness = 0;
        const auto x = Say(v.text, voice);
        CHECK(!x.empty());
        if (x.empty()) continue;
        const auto peaks = Formants(x, x.size() / 2 - 300);
        std::printf("  /%s/ peaks:", v.text + 1); for (double p : peaks) std::printf(" %.0f", p); std::printf("\n");
        CHECK(Near(peaks, v.f1, 0.12)); CHECK(Near(peaks, v.f2, 0.08));
    }
    // A shorter vocal tract (formant_scale 1.15, as a woman's or a child's) raises them in proportion.
    SynthVoice small; small.formant_scale = 1.15f; small.f0 = 210; small.jitter = 0; small.shimmer = 0; small.breathiness = 0;
    const auto x = Say("'i", small);
    const auto peaks = Formants(x, x.size() / 2 - 300);
    CHECK(Near(peaks, 2250 * 1.15, 0.06));
}
void Source() {
    // Pitch: a flat contour (no intonation, no jitter) at the asked frequency; with intonation the start is higher than
    // the end (declination and final fall).
    for (float f0 : {95.0f, 150.0f, 230.0f}) {
        SynthVoice voice; voice.f0 = f0; voice.intonation = 0; voice.jitter = 0;
        const auto x = Say("'a", voice);
        const double p = Pitch(x, x.size() / 2 - 400);
        std::printf("  f0 %.0f Hz: measured %.1f Hz\n", static_cast<double>(f0), p);
        CHECK(std::fabs(p - f0) < 0.04 * f0);
    }
    SynthVoice voice; voice.f0 = 150; voice.jitter = 0;
    const auto x = Say("a a a a a a", voice);
    const double early = Pitch(x, FormantSynthesizer::kLeadSamples + 900), late = Pitch(x, x.size() - FormantSynthesizer::kTailSamples - 900);
    CHECK(early > late * 1.1);
}
void Noise() {
    // Fricatives in their bands: s high, sh in the middle, the uvular-velar ch (Hebrew ח) low; s is quieter than a
    // vowel, but not by more than 20 dB.
    const auto at = [](const std::vector<float>&) { return static_cast<std::size_t>(FormantSynthesizer::kLeadSamples) + 800; };
    const auto s = Say("s"), sh = Say("S"), ch = Say("X"), a = Say("a");
    const double cs = Centroid(s, at(s)), csh = Centroid(sh, at(sh)), cch = Centroid(ch, at(ch));
    std::printf("  centroids: s %.0f Hz, sh %.0f Hz, ch %.0f Hz\n", cs, csh, cch);
    CHECK(cs > 4300); CHECK(csh > 2300 && csh < 3900); CHECK(cch < 2300); CHECK(cs > csh && csh > cch);
    // Levels before the peak normalisation are not visible; within one utterance they are.
    const auto as = Say("'a s 'a");
    const std::size_t lead = FormantSynthesizer::kLeadSamples;
    const auto vowel = Rms(as, lead + 400, lead + 1600);
    std::size_t quiet = lead + 2300; // Inside the /s/ (a stressed /a/ lasts 128 ms = 2052 samples).
    const auto fric = Rms(as, quiet, quiet + 800);
    const double db = 20 * std::log10(fric / vowel);
    std::printf("  /s/ is %.1f dB below /a/\n", db);
    CHECK(db < -3 && db > -24);
}
void Timing() {
    // Rate divides every duration; the lead and tail silences stay; words are spans in order.
    SynthVoice slow, fast; fast.rate = 1.25f;
    const char* text = "a n 'i | R o ts 'e | S e t a ? a ts 'o R";
    const auto a = synth.Samples(text, slow), b = synth.Samples(text, fast);
    const std::size_t pad = FormantSynthesizer::kLeadSamples + FormantSynthesizer::kTailSamples;
    CHECK(a > pad && b > pad);
    CHECK(std::fabs(static_cast<double>(a - pad) / static_cast<double>(b - pad) - 1.25) < 0.03);
    std::vector<float> out(a);
    media::ai::SynthWord words[8]; unsigned count = 0;
    CHECK(synth.Speak(text, slow, out, words, 8, &count) == a);
    CHECK(count == 3);
    CHECK(words[0].begin == FormantSynthesizer::kLeadSamples);
    for (unsigned i = 0; i < count; ++i) CHECK(words[i].end > words[i].begin);
    for (unsigned i = 1; i < count; ++i) CHECK(words[i].begin == words[i - 1].end);
    CHECK(words[count - 1].end == a - FormantSynthesizer::kTailSamples);
    // Silence where there is no speech; speech peak-normalised to 0.5.
    CHECK(Rms(out, 0, 1500) == 0.0); CHECK(Rms(out, a - 2000, a) < 1e-6);
    float peak = 0; for (float v : out) { peak = (std::max)(peak, std::fabs(v)); CHECK(std::isfinite(v)); }
    CHECK(std::fabs(peak - 0.5f) < 1e-6f);
    // Refusals: an unknown token, a stray stress mark, too small a buffer, nothing at all.
    std::vector<float> small(100);
    CHECK(synth.Samples("a xq e", slow) == 0); CHECK(synth.Samples("'", slow) == 0); CHECK(synth.Samples("", slow) == 0);
    CHECK(synth.Speak(text, slow, small) == 0);
    CHECK(FormantSynthesizer::Known("'o~") && FormantSynthesizer::Known("ts") && !FormantSynthesizer::Known("th"));
}
void Variety() {
    // The same seed gives the same samples; another seed other samples of the same length; reverberation adds a tail.
    SynthVoice voice; voice.seed = 7;
    const auto a = Say("t a S t 'i k", voice), b = Say("t a S t 'i k", voice);
    voice.seed = 8; const auto c = Say("t a S t 'i k", voice);
    CHECK(a == b); CHECK(a.size() == c.size() && a != c);
    SynthVoice room = voice; room.reverb_seconds = 0.6f; room.reverb_mix = 0.35f;
    const auto d = Say("t a S t 'i k", room);
    const std::size_t tail = d.size() - FormantSynthesizer::kTailSamples;
    CHECK(Rms(d, tail, tail + 800) > 10 * Rms(c, tail, tail + 800) + 1e-6);
}
void Allocation() {
    std::vector<float> out(FormantSynthesizer::kSampleRate * 4);
    SynthVoice voice; voice.reverb_seconds = 0.4f;
    allocation_probe::Begin();
    const auto n = synth.Speak("t a m S 'i X | 'e s e R | S n i j 'o t | k a d 'i m a", voice, out);
    CHECK(allocation_probe::End() == 0);
    CHECK(n > 16000);
}
}
int main() {
    std::puts("media_formant_synth_tests");
    Vowels(); Source(); Noise(); Timing(); Variety(); Allocation();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
