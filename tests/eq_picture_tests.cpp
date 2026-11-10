// Mastering EQ, audio zoom, picture controls and micro-motion (docs/audio-dsp.md "Mastering EQ and audio zoom",
// docs/picture.md "Picture controls and micro-motion"):
// - EQ: random band gains at 48 and 44.1 kHz match the analytic response of the ten biquads within 0.05 dB; a band at +6 dB
//   reads +6.00 dB at its centre; flat is bit-identical, also after a band went to +6 dB and back;
// - notch killer: a 120 Hz drone and 50 Hz mains hum (and its 150 Hz harmonic) under speech lose more than 18 dB while the
//   speech is kept (third-octave spectrum within 0.5 dB of the clean voice, level within 0.25 dB); speech alone engages none;
// - speech-protected ducking: a bass line 12 dB over a voice is lowered while the voice speaks and released when it stops; a
//   user cut on 2 kHz is held near -3 dB while the voice speaks;
// - iso-loudness: ISO 226 at 1 kHz is the phon level; quiet playback lifts the bass, not 1 kHz;
// - audio zoom: zoomed to the left, a source on the left keeps its level and one on the right loses ~11 dB (the model's
//   gain), and the other way round; zoom below 1.2 is bit-identical; panning glides without a step;
// - cures (ui::PictureAudioCures with the grammar): "חשוך", "יש זמזום", "הדיבור נבלע", "too dark", "noisy", "muffled", and
//   "בטל"/"undo" restores exactly what was there before; the cure is undone before any jump;
// - GPU (Windows): shadow lift raises shadows and leaves highlights bit-identical, saturation 0 is grey, neutral controls are
//   bit-identical to no controls; micro-motion turns a +-2/255 pulse into visible contrast and leaves a fast edge without a
//   ghost (bit-identical to the conversion without it);
// - nothing allocates.
#include "media/audio/DspProcessor.hpp"
#include "media/audio/MasteringEq.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ui/PictureAudioCures.hpp"
#include "allocation_probe.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include "eq_picture_gpu.hpp"
#endif
using media::audio::AudioFocus; using media::audio::DspProcessor; using media::audio::MasteringEq; using media::audio::SpectralTracker;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kRate = 48000, kMaxSeconds = 8;
float buffer[kRate * kMaxSeconds * 2], clean[kRate * kMaxSeconds * 2], work[kRate * kMaxSeconds * 2]; // Stereo, static: no heap.
std::uint32_t noise_state = 777;
float Noise() noexcept { noise_state = noise_state * 1664525u + 1013904223u; return static_cast<float>(static_cast<std::int32_t>(noise_state)) / 2147483648.0f; }
// Amplitude of the `hz` component of `n` frames of channel `channel` (correlation over a whole window).
double ToneLevel(const float* x, std::size_t first, std::size_t n, unsigned channel, double hz, unsigned rate = kRate) {
    n = static_cast<std::size_t>(std::floor(std::floor(static_cast<double>(n) * hz / rate) * rate / hz + 0.5)); // Whole cycles.
    double re = 0, im = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 2 * kPi * hz * static_cast<double>(first + i) / rate, v = x[(first + i) * 2 + channel];
        re += v * std::cos(w); im += v * std::sin(w);
    }
    return 2 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}
double Energy(const float* x, std::size_t first, std::size_t n) {
    double e = 0; for (std::size_t i = first * 2; i < (first + n) * 2; ++i) e += static_cast<double>(x[i]) * x[i]; return e;
}
double Db(double ratio) { return 20 * std::log10(ratio + 1e-30); }
// A voice-like signal, `seconds` long at -26 dBFS-ish: a glottal source of harmonics (1/k) under three formants that change
// with every syllable, a pitch gliding 165-235 Hz, syllables of 220 ms with 80 ms gaps.
void Speech(float* x, double seconds, double amplitude, unsigned rate = kRate) {
    static constexpr double kVowels[4][3]{{730, 1090, 2440}, {270, 2290, 3010}, {530, 1840, 2480}, {570, 840, 2410}};
    const auto n = static_cast<std::size_t>(seconds * rate);
    double phase = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate, cycle = std::fmod(t, 0.3);
        const double f0 = 200 + 25 * std::sin(2 * kPi * 0.7 * t) + 10 * std::sin(2 * kPi * 1.9 * t); // 165-235 Hz.
        phase += 2 * kPi * f0 / rate;
        const double envelope = cycle < 0.22 ? std::sin(kPi * cycle / 0.22) : 0.0;
        const auto& vowel = kVowels[static_cast<unsigned>(t / 0.3) % 4];
        double v = 0;
        for (unsigned k = 1; k * f0 < 4000; ++k) {
            double gain = 0;
            for (double formant : vowel) { const double d = (k * f0 - formant) / 90.0; gain += 1 / (1 + d * d); }
            v += gain / k * std::sin(k * phase);
        }
        const float s = static_cast<float>(amplitude * envelope * v);
        x[i * 2] = s; x[i * 2 + 1] = s;
    }
}
void AddSine(float* x, double seconds, double hz, double amplitude, unsigned mask = 3, unsigned rate = kRate) {
    const auto n = static_cast<std::size_t>(seconds * rate);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = static_cast<float>(amplitude * std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
        for (unsigned c = 0; c < 2; ++c) if ((mask >> c) & 1u) x[i * 2 + c] += v;
    }
}
// The analysis is the caller's (DspProcessor::AttachAnalysis, 112 KB): static, a few in turn (at most two processors live at once).
SpectralTracker trackers[4];
unsigned next_tracker = 0;
bool Prepare(DspProcessor& dsp, unsigned rate, unsigned channels) {
    dsp.AttachAnalysis(&trackers[next_tracker++ % 4]);
    return dsp.Configure(rate, channels);
}
void Run(DspProcessor& dsp, float* x, std::size_t frames, unsigned block = 480) {
    for (std::size_t f = 0; f < frames; f += block) dsp.Process(x + f * 2, static_cast<unsigned>((std::min<std::size_t>)(block, frames - f)));
}

void TestEqAccuracy() {
    for (unsigned rate : {48000u, 44100u}) {
        float gains[MasteringEq::kBands];
        for (unsigned b = 0; b < MasteringEq::kBands; ++b) gains[b] = std::round(Noise() * 12.0f * 4) / 4;
        DspProcessor dsp; CHECK(Prepare(dsp, rate, 2));
        for (unsigned b = 0; b < MasteringEq::kBands; ++b) dsp.Eq().SetBand(b, gains[b]);
        std::memset(work, 0, sizeof(float) * rate * 2);
        Run(dsp, work, rate); // One second of silence: every band's ramp has landed.
        for (unsigned b = 0; b < MasteringEq::kBands; ++b) CHECK(dsp.Eq().AppliedDb(b) == (SpectralTracker::kBandCentres[b] * 2.4f < rate ? gains[b] : 0.0f));
        double worst = 0;
        for (double hz : {40.0, 63.0, 90.0, 125.0, 180.0, 250.0, 400.0, 700.0, 1000.0, 1500.0, 2200.0, 3300.0, 5000.0, 8000.0, 11000.0, 14000.0}) {
            const std::size_t n = rate / 2;
            for (std::size_t i = 0; i < n; ++i) work[i * 2] = work[i * 2 + 1] = static_cast<float>(0.05 * std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
            Run(dsp, work, n);
            const double measured = Db(ToneLevel(work, n / 2, n / 2, 0, hz, rate) / 0.05);
            double expected = 0;
            for (unsigned b = 0; b < MasteringEq::kBands; ++b)
                if (SpectralTracker::kBandCentres[b] * 2.4f < rate) expected += MasteringEq::PeakingResponseDb(hz, SpectralTracker::kBandCentres[b], gains[b], MasteringEq::kBandQ, rate);
            worst = (std::max)(worst, std::fabs(measured - expected));
        }
        std::printf("EQ %u Hz: worst |measured - analytic| %.4f dB\n", rate, worst);
        CHECK(worst < 0.05);
    }
    // One band at +6 dB reads +6 dB at its centre.
    DspProcessor dsp; CHECK(Prepare(dsp, kRate, 2)); dsp.Eq().SetBand(5, 6.0f);
    std::memset(work, 0, sizeof(float) * kRate * 2); Run(dsp, work, kRate);
    for (std::size_t i = 0; i < kRate; ++i) work[i * 2] = work[i * 2 + 1] = static_cast<float>(0.05 * std::sin(2 * kPi * 1000.0 * static_cast<double>(i) / kRate));
    Run(dsp, work, kRate);
    const double centre = Db(ToneLevel(work, kRate / 2, kRate / 2, 1, 1000.0) / 0.05);
    std::printf("EQ 1 kHz band at +6 dB: %.3f dB at 1 kHz\n", centre);
    CHECK(std::fabs(centre - 6.0) < 0.02);
}

void TestBypass() {
    // Flat is bit-identical, with the analysis running too (telemetry).
    for (std::size_t i = 0; i < kRate * 2; ++i) { buffer[i * 2] = 0.3f * Noise(); buffer[i * 2 + 1] = 0.3f * Noise(); }
    std::memcpy(work, buffer, sizeof(float) * kRate * 4);
    DspProcessor dsp; CHECK(Prepare(dsp, kRate, 2)); dsp.SetAnalysis(true);
    Run(dsp, work, kRate * 2, 333);
    CHECK(std::memcmp(work, buffer, sizeof(float) * kRate * 4) == 0);
    CHECK(dsp.Analysis()->Analyses() > 0 && !dsp.Active());
    // A band to +6 dB and back: once it has landed and decayed, bit-identical again.
    dsp.Eq().SetBand(3, 6.0f); Run(dsp, work, kRate / 2); dsp.Eq().SetBand(3, 0.0f);
    std::memcpy(work, buffer, sizeof(float) * kRate * 4); Run(dsp, work, kRate);
    std::memcpy(work, buffer, sizeof(float) * kRate * 4); Run(dsp, work, kRate);
    CHECK(!dsp.Active());
    CHECK(std::memcmp(work, buffer, sizeof(float) * kRate * 2) == 0);
    // Audio zoom: off, and on below 1.2x, is bit-identical.
    for (float zoom : {1.0f, 1.1f, 1.2f}) {
        DspProcessor z; CHECK(Prepare(z, kRate, 2));
        z.Focus().Set(zoom > 1.05f, zoom, 0.1f);
        std::memcpy(work, buffer, sizeof(float) * kRate * 2); Run(z, work, kRate);
        CHECK(std::memcmp(work, buffer, sizeof(float) * kRate * 2) == 0);
    }
}

void TestNotchKiller() {
    const double seconds = 7, speech_amplitude = 0.02;
    const auto n = static_cast<std::size_t>(seconds * kRate);
    Speech(clean, seconds, speech_amplitude);
    std::memcpy(buffer, clean, sizeof(float) * n * 2);
    AddSine(buffer, seconds, 120.0, 0.1);  // A generator's drone, -20 dBFS.
    AddSine(buffer, seconds, 50.0, 0.05);  // Mains hum, -26 dBFS ...
    AddSine(buffer, seconds, 150.0, 0.02); // ... and its third harmonic.
    std::memcpy(work, buffer, sizeof(float) * n * 2);
    DspProcessor dsp; CHECK(Prepare(dsp, kRate, 2)); dsp.Eq().SetNotchKiller(true);
    allocation_probe::Begin();
    unsigned engaged[4]{}; // When 1, 2 and 3 notches were engaged (in quarter seconds).
    for (std::size_t f = 0; f < n; f += kRate / 4) {
        Run(dsp, work + f * 2, static_cast<unsigned>((std::min<std::size_t>)(kRate / 4, n - f)), 441);
        const unsigned count = (std::min)(dsp.Eq().Notches(), 3u);
        if (count && !engaged[count]) engaged[count] = static_cast<unsigned>(f / (kRate / 4)) + 1;
    }
    CHECK(allocation_probe::End() == 0);
    std::printf("Notches engaged: first at %.2f s, second at %.2f s, third at %.2f s\n", engaged[1] / 4.0, engaged[2] / 4.0, engaged[3] / 4.0);
    const std::size_t from = 4 * kRate, span = 3 * kRate; // The last 3 s.
    std::printf("Notch killer: %u notches:", dsp.Eq().Notches());
    for (unsigned i = 0; i < MasteringEq::kMaxNotches; ++i) if (dsp.Eq().NotchDepth(i) > 0) std::printf(" %.3f Hz", dsp.Eq().NotchHz(i));
    std::printf("\n");
    for (const double hz : {120.0, 50.0, 150.0}) {
        const double before = ToneLevel(buffer, from, span, 0, hz), after = ToneLevel(work, from, span, 0, hz);
        std::printf("  %.0f Hz: %.1f dB\n", hz, Db(after / before));
        CHECK(Db(after / before) < -18.0);
    }
    // The speech: its spectrum (third-octave band energies from 200 Hz to 4 kHz) against the clean voice's, and its level.
    // The waveform differs a little more (a notch turns the phase of the voice's fundamental near it): reported, bounded loosely.
    double residual = 0;
    for (std::size_t i = from * 2; i < (from + span) * 2; ++i) { const double d = static_cast<double>(work[i]) - clean[i]; residual += d * d; }
    const double voice = Energy(clean, from, span), level = 10 * std::log10(Energy(work, from, span) / voice);
    double worst = 0;
    for (double centre = 200; centre < 4100; centre *= 1.2599210498948732) {
        // A fourth-order band (two cascaded RBJ band-passes) in double, run over both signals.
        const double w = 2 * kPi * centre / kRate, alpha = std::sin(w) / (2 * 4.32), a0 = 1 + alpha;
        const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2 * std::cos(w) / a0, a2 = (1 - alpha) / a0;
        double energy[2]{};
        for (unsigned which = 0; which < 2; ++which) {
            const float* x = which ? work : clean;
            double z[2][2]{};
            for (std::size_t i = from - kRate / 2; i < from + span; ++i) {
                double v = x[i * 2];
                for (auto& s : z) { const double y = b0 * v + s[0]; s[0] = -a1 * y + s[1]; s[1] = b2 * v - a2 * y; v = y; }
                if (i >= from) energy[which] += v * v;
            }
        }
        worst = (std::max)(worst, std::fabs(10 * std::log10(energy[1] / energy[0])));
    }
    std::printf("  speech: third-octave bands 200 Hz-4 kHz within %.2f dB of the clean voice, level %+.2f dB, waveform residual %.1f dB\n",
                worst, level, 10 * std::log10(residual / voice));
    CHECK(worst < 0.5 && std::fabs(level) < 0.25 && 10 * std::log10(residual / voice) < -10.0);
    // Speech alone: no notch.
    std::memcpy(work, clean, sizeof(float) * n * 2);
    DspProcessor alone; CHECK(Prepare(alone, kRate, 2)); alone.Eq().SetNotchKiller(true);
    Run(alone, work, n);
    std::printf("  speech alone: %u notches, speech %.2f\n", alone.Eq().Notches(), alone.Analysis()->Speech());
    CHECK(alone.Eq().Notches() == 0);
}

void TestDucking() {
    const double seconds = 7;
    const auto n = static_cast<std::size_t>(seconds * kRate);
    // A bass line (a new note every 250 ms, 55-110 Hz) 12 dB over a voice; the voice stops at 4 s.
    Speech(buffer, 4.0, 0.02);
    for (std::size_t i = static_cast<std::size_t>(4.0 * kRate); i < n; ++i) buffer[i * 2] = buffer[i * 2 + 1] = 0;
    static constexpr double kNotes[]{55, 73.4, 82.4, 98, 110, 65.4, 87.3, 61.7};
    double phase = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        phase += 2 * kPi * kNotes[static_cast<unsigned>(t / 0.25) % 8] / kRate;
        const float v = static_cast<float>(0.25 * (std::sin(phase) + 0.3 * std::sin(2 * phase)));
        buffer[i * 2] += v; buffer[i * 2 + 1] += v;
    }
    DspProcessor dsp; CHECK(Prepare(dsp, kRate, 2)); dsp.Eq().SetSpeechDucking(true); dsp.Eq().SetBand(6, -9.0f);
    float while_speaking[MasteringEq::kBands]{}, protected_db = 0, speech = 0;
    Run(dsp, buffer, static_cast<std::size_t>(3.5 * kRate));
    for (unsigned b = 0; b < MasteringEq::kBands; ++b) while_speaking[b] = dsp.Eq().DuckDb(b);
    protected_db = dsp.Eq().AppliedDb(6); speech = dsp.Analysis()->Speech();
    Run(dsp, buffer + static_cast<std::size_t>(3.5 * kRate) * 2, n - static_cast<std::size_t>(3.5 * kRate));
    std::printf("Ducking while speaking (speech %.2f): 31.5 %.1f, 63 %.1f, 125 %.1f, 250 %.1f, 1k %.1f dB; 2 kHz cut -9 held at %.1f dB\n",
                speech, while_speaking[0], while_speaking[1], while_speaking[2], while_speaking[3], while_speaking[5], protected_db);
    std::printf("  3 s after the voice: 63 Hz %.2f dB, 2 kHz %.1f dB, speech %.2f\n", dsp.Eq().DuckDb(1), dsp.Eq().AppliedDb(6), dsp.Analysis()->Speech());
    CHECK(speech > 0.5f);
    CHECK(while_speaking[1] < -3.0f && while_speaking[1] >= -6.0f);
    CHECK(while_speaking[5] == 0 && while_speaking[6] == 0 && while_speaking[7] == 0);
    CHECK(protected_db > -6.0f);
    CHECK(dsp.Eq().DuckDb(1) > -0.5f && dsp.Eq().AppliedDb(6) < -8.9f);
}

void TestIsoLoudness() {
    CHECK(std::fabs(MasteringEq::EqualLoudness(1000, 40) - 40) < 0.2 && std::fabs(MasteringEq::EqualLoudness(1000, 80) - 80) < 0.2);
    const float bass = MasteringEq::IsoGain(1, -30), mid = MasteringEq::IsoGain(5, -30), deep = MasteringEq::IsoGain(0, -45);
    std::printf("Iso-loudness at -30 dB: 63 Hz +%.1f dB, 1 kHz +%.2f dB; 31.5 Hz at -45 dB +%.1f dB\n", bass, mid, deep);
    CHECK(bass > 4.0f && bass <= 12.0f && mid < 0.5f && deep >= bass);
    for (unsigned b = 0; b < MasteringEq::kBands; ++b) CHECK(MasteringEq::IsoGain(b, 0) == 0);
    CHECK(MasteringEq::IsoGain(1, -10) <= MasteringEq::IsoGain(1, -20) && MasteringEq::IsoGain(1, -20) <= MasteringEq::IsoGain(1, -40));
}

void TestAudioZoom() {
    const double seconds = 1.5;
    const auto n = static_cast<std::size_t>(seconds * kRate);
    std::memset(buffer, 0, sizeof(float) * n * 2);
    AddSine(buffer, seconds, 600.0, 0.1, 1);  // Hard left.
    AddSine(buffer, seconds, 650.0, 0.1, 2);  // Hard right.
    for (float target : {0.1f, 0.9f}) {
        DspProcessor dsp; CHECK(Prepare(dsp, kRate, 2)); dsp.Focus().Set(true, 3.0f, target);
        std::memcpy(work, buffer, sizeof(float) * n * 2);
        Run(dsp, work, n);
        const std::size_t from = kRate / 2, span = kRate;
        const auto power = [&](double hz) {
            const double l = ToneLevel(work, from, span, 0, hz), r = ToneLevel(work, from, span, 1, hz); return std::sqrt(l * l + r * r) / 0.1; };
        const double left = Db(power(600)), right = Db(power(650));
        const double model_left = Db(AudioFocus::SourceGain(0, target, 1)), model_right = Db(AudioFocus::SourceGain(1, target, 1));
        std::printf("Audio zoom at x=%.1f: left source %+.2f dB (model %+.2f, + presence), right source %+.2f dB (model %+.2f)\n",
                    target, left, model_left, right, model_right);
        const double on = target < 0.5f ? left : right, off = target < 0.5f ? right : left;
        CHECK(on - off > 9.0);
        CHECK(std::fabs(off - (target < 0.5f ? model_right : model_left)) < 1.5);
        CHECK(on > -1.0 && on < 2.0);
    }
    // Panning glides: moving the target across the picture changes no sample by more than the signal itself can.
    DspProcessor dsp; CHECK(Prepare(dsp, kRate, 2)); dsp.Focus().Set(true, 3.0f, 0.0f);
    std::memcpy(work, buffer, sizeof(float) * n * 2);
    Run(dsp, work, kRate / 2);
    dsp.Focus().Set(true, 3.0f, 1.0f);
    float before = work[(kRate / 2 - 1) * 2], worst = 0;
    Run(dsp, work + (kRate / 2) * 2, kRate / 2);
    for (std::size_t i = kRate / 2; i < kRate; ++i) { worst = (std::max)(worst, std::fabs(work[i * 2] - before)); before = work[i * 2]; }
    std::printf("  pan glide: largest step %.4f (signal's own up to %.4f)\n", worst, static_cast<float>(2 * 0.1 * 2 * kPi * 650 / kRate * 1.5));
    CHECK(worst < static_cast<float>(2 * 0.1 * 2 * kPi * 650 / kRate * 1.5));
}

void TestCures() {
    using media::ai::IntentKind; using media::ai::LocalIntentDispatcher; using media::ui::PictureAudioCures;
    media::ai::MediaBounds bounds{}; bounds.duration_100ns = 600 * LocalIntentDispatcher::kSecond; bounds.seekable = true;
    const struct { const char* text; IntentKind kind; } said[]{
        {"\xd7\x97\xd7\xa9\xd7\x95\xd7\x9a", IntentKind::CureDark},                                       // חשוך
        {"too dark", IntentKind::CureDark},
        {"\xd7\x99\xd7\xa9 \xd7\x96\xd7\x9e\xd7\x96\xd7\x95\xd7\x9d", IntentKind::CureNoise},             // יש זמזום
        {"noisy", IntentKind::CureNoise},
        {"\xd7\x94\xd7\x93\xd7\x99\xd7\x91\xd7\x95\xd7\xa8 \xd7\xa0\xd7\x91\xd7\x9c\xd7\xa2", IntentKind::CureMuffled}, // הדיבור נבלע
        {"muffled", IntentKind::CureMuffled},
        {"audio zoom", IntentKind::AudioZoomOn},
        {"\xd7\x96\xd7\x95\xd7\x9d \xd7\x90\xd7\xa7\xd7\x95\xd7\xa1\xd7\x98\xd7\x99", IntentKind::AudioZoomOn}, // זום אקוסטי
        {"micro motion", IntentKind::MicroMotionOn},
        {"tactical vision", IntentKind::MicroMotionOn},
        {"\xd7\x94\xd7\x93\xd7\x92\xd7\xa9 \xd7\xaa\xd7\xa0\xd7\x95\xd7\xa2\xd7\x94 \xd7\x96\xd7\xa2\xd7\x99\xd7\xa8\xd7\x94", IntentKind::MicroMotionOn}, // הדגש תנועה זעירה
        {"\xd7\x91\xd7\x98\xd7\x9c \xd7\x96\xd7\x95\xd7\x9d \xd7\x90\xd7\xa7\xd7\x95\xd7\xa1\xd7\x98\xd7\x99", IntentKind::AudioZoomOff}, // בטל זום אקוסטי
        {"tactical vision off", IntentKind::MicroMotionOff},
        // One in each other language: Arabic, Spanish, French, German, Russian, Chinese, Japanese (as typed: accents, ё/й).
        {"\xd8\xaa\xd9\x83\xd8\xa8\xd9\x8a\xd8\xb1 \xd8\xa7\xd9\x84\xd8\xb5\xd9\x88\xd8\xaa", IntentKind::AudioZoomOn},              // تكبير الصوت
        {"zoom ac\xc3\xbastico", IntentKind::AudioZoomOn},                                                                           // zoom acústico
        {"trop sombre", IntentKind::CureDark},
        {"es brummt", IntentKind::CureNoise},
        {"\xd0\xb3\xd1\x83\xd0\xb4\xd0\xb8\xd1\x82", IntentKind::CureNoise},                                                           // гудит
        {"\xd1\x82\xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd0\xba\xd0\xbe\xd0\xb5 \xd0\xb7\xd1\x80\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5", IntentKind::MicroMotionOn}, // тактическое зрение
        {"\xe5\xaf\xb9\xe7\x99\xbd\xe5\x90\xac\xe4\xb8\x8d\xe6\xb8\x85", IntentKind::CureMuffled},                                     // 对白听不清
        {"\xe6\x9a\x97\xe3\x81\x99\xe3\x81\x8e\xe3\x82\x8b", IntentKind::CureDark},                                                    // 暗すぎる
    };
    for (const auto& s : said) {
        const auto intent = LocalIntentDispatcher::Parse(s.text, bounds);
        if (intent.kind != s.kind || intent.status != media::ai::IntentStatus::Ok)
            std::printf("  \"%s\" -> %s (%s)\n", s.text, media::ai::Name(intent.kind), media::ai::Name(intent.status));
        CHECK(intent.kind == s.kind && intent.status == media::ai::IntentStatus::Ok);
    }
    // Each cure, then "בטל" (or "undo"): exactly what was there before.
    media::ai::ConversationState conversation;
    PictureAudioCures cures;
    PictureAudioCures::Settings start{}; start.picture.brightness = 0.02f; start.audio.eq_db[6] = -2.0f; start.audio.clarity = 1;
    for (IntentKind kind : {IntentKind::CureDark, IntentKind::CureNoise, IntentKind::CureMuffled}) {
        PictureAudioCures::Settings now = start;
        CHECK(cures.Apply(kind, now));
        CHECK(std::memcmp(&now, &start, sizeof(now)) != 0);
        if (kind == IntentKind::CureDark) CHECK(now.picture.shadow_lift > 0 && now.picture.gamma > 1);
        if (kind == IntentKind::CureNoise) CHECK(now.audio.notch_killer);
        if (kind == IntentKind::CureMuffled) CHECK(now.audio.eq_db[6] > start.audio.eq_db[6] && now.audio.speech_ducking && now.audio.clarity >= 2);
        media::ai::Intent done{}; done.kind = kind; done.status = media::ai::IntentStatus::Ok;
        conversation.NoteJump(5 * LocalIntentDispatcher::kSecond); // A jump before the cure: undo is the cure's.
        conversation.Record("cure", done);
        const auto undo = conversation.Resolve(LocalIntentDispatcher::Parse(kind == IntentKind::CureNoise ? "undo" : "\xd7\x91\xd7\x98\xd7\x9c", bounds), bounds);
        CHECK(undo.kind == IntentKind::UndoCure && undo.status == media::ai::IntentStatus::Ok);
        CHECK(cures.Undo(now));
        CHECK(std::memcmp(&now, &start, sizeof(now)) == 0);
        CHECK(!cures.Undo(now)); // One step.
        conversation.Record("undo", undo);
        // With the cure undone, "undo" is the jump's again.
        const auto jump = conversation.Resolve(LocalIntentDispatcher::Parse("undo", bounds), bounds);
        CHECK(jump.kind == IntentKind::SeekAbsolute);
    }
    // Two cures: undo takes back the second only.
    PictureAudioCures::Settings now = start;
    CHECK(cures.Apply(IntentKind::CureDark, now)); const auto dark = now;
    CHECK(cures.Apply(IntentKind::CureMuffled, now));
    CHECK(cures.Undo(now) && std::memcmp(&now, &dark, sizeof(now)) == 0);
    std::printf("Cures: %zu phrases, three cures each undone exactly\n", sizeof(said) / sizeof(said[0]));
}
}

int main() {
    TestEqAccuracy();
    TestBypass();
    TestNotchKiller();
    TestDucking();
    TestIsoLoudness();
    TestAudioZoom();
    TestCures();
    allocation_probe::Begin();
    {
        DspProcessor dsp; (void)Prepare(dsp, kRate, 2);
        dsp.Eq().SetNotchKiller(true); dsp.Eq().SetSpeechDucking(true); dsp.Eq().SetIsoLoudness(true, -20); dsp.Focus().Set(true, 2.0f, 0.3f);
        Run(dsp, buffer, kRate);
    }
    const auto allocations = allocation_probe::End();
#if defined(_WIN32)
    const int gpu = eq_picture_gpu::Run(failures);
#else
    const int gpu = 0;
#endif
    std::printf("Mastering EQ, audio zoom and picture controls: %d failures, %llu allocations%s\n", failures,
                static_cast<unsigned long long>(allocations), gpu < 0 ? " (no GPU: picture checks skipped)" : "");
    CHECK(allocations == 0);
    return failures ? 1 : 0;
}
