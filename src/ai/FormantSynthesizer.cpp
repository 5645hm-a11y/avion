#include "media/ai/FormantSynthesizer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::ai {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kRate = static_cast<float>(FormantSynthesizer::kSampleRate);
enum class Kind : std::uint8_t { Vowel, Approximant, Nasal, Fricative, Plosive, Glottal, Affricate, Aspirate };
// Articulatory targets of one phoneme (formants for an adult male voice; Peterson & Barney, Klatt 1980, and the Hebrew
// and French vowel literature for the five-vowel systems). Plosives and fricatives carry their place's locus: where
// a neighbouring vowel's formants start or end.
struct Phoneme {
    const char* code; Kind kind;
    float f1, f2, f3, b1, b2, b3;
    float ms;                        // Duration at rate 1, unstressed.
    float voicing, frication, fric_hz, fric_bw, nasal;
    float burst_hz = 0;              // Plosives: the burst's noise band.
    bool voiced_stop = false;
};
constexpr Phoneme kPhonemes[]{
    // Vowels.
    {"a", Kind::Vowel, 730, 1300, 2500, 80, 100, 150, 95, 1, 0, 0, 0, 0},
    {"e", Kind::Vowel, 480, 1850, 2550, 70, 100, 150, 90, 1, 0, 0, 0, 0},
    {"i", Kind::Vowel, 300, 2250, 3000, 60, 100, 150, 85, 1, 0, 0, 0, 0},
    {"o", Kind::Vowel, 500, 900, 2450, 70, 90, 150, 90, 1, 0, 0, 0, 0},
    {"u", Kind::Vowel, 330, 800, 2300, 70, 90, 150, 85, 1, 0, 0, 0, 0},
    {"ae", Kind::Vowel, 690, 1660, 2490, 80, 100, 150, 110, 1, 0, 0, 0, 0},
    {"E", Kind::Vowel, 580, 1800, 2500, 70, 100, 150, 90, 1, 0, 0, 0, 0},
    {"I", Kind::Vowel, 400, 1900, 2550, 70, 100, 150, 70, 1, 0, 0, 0, 0},
    {"O", Kind::Vowel, 600, 920, 2450, 80, 90, 150, 100, 1, 0, 0, 0, 0},
    {"V", Kind::Vowel, 640, 1190, 2390, 80, 100, 150, 80, 1, 0, 0, 0, 0},
    {"@", Kind::Vowel, 500, 1500, 2500, 80, 100, 150, 55, 0.9f, 0, 0, 0, 0},
    {"U", Kind::Vowel, 450, 1030, 2380, 70, 90, 150, 70, 1, 0, 0, 0, 0},
    {"o~", Kind::Vowel, 520, 850, 2400, 120, 100, 150, 120, 1, 0, 0, 0, 0.6f},
    {"a~", Kind::Vowel, 700, 1200, 2500, 120, 100, 150, 120, 1, 0, 0, 0, 0.6f},
    // Nasals and approximants.
    {"m", Kind::Nasal, 280, 1000, 2200, 100, 200, 200, 70, 0.55f, 0, 0, 0, 1},
    {"n", Kind::Nasal, 280, 1600, 2600, 100, 200, 200, 65, 0.55f, 0, 0, 0, 1},
    {"N", Kind::Nasal, 280, 2000, 2600, 100, 200, 200, 70, 0.55f, 0, 0, 0, 1},
    {"l", Kind::Approximant, 360, 1100, 2800, 80, 120, 200, 60, 0.75f, 0, 0, 0, 0},
    {"j", Kind::Approximant, 280, 2200, 2950, 60, 100, 200, 55, 0.7f, 0, 0, 0, 0},
    {"w", Kind::Approximant, 300, 650, 2200, 60, 100, 200, 55, 0.7f, 0, 0, 0, 0},
    {"R", Kind::Approximant, 450, 1200, 2300, 120, 150, 200, 65, 0.55f, 0.2f, 1300, 900, 0},
    {"r", Kind::Approximant, 350, 1150, 1600, 80, 120, 150, 60, 0.7f, 0, 0, 0, 0},
    // Fricatives (voicing > 0: voiced).
    {"f", Kind::Fricative, 300, 1000, 2300, 100, 150, 200, 95, 0, 0.3f, 4500, 3500, 0},
    {"v", Kind::Fricative, 250, 1100, 2300, 100, 150, 200, 65, 0.5f, 0.15f, 4500, 3500, 0},
    {"s", Kind::Fricative, 300, 1700, 2600, 100, 150, 200, 105, 0, 0.65f, 5600, 1600, 0},
    {"z", Kind::Fricative, 250, 1700, 2600, 100, 150, 200, 75, 0.45f, 0.35f, 5600, 1600, 0},
    {"S", Kind::Fricative, 300, 1900, 2500, 100, 150, 200, 110, 0, 0.7f, 3000, 1500, 0},
    {"Z", Kind::Fricative, 250, 1900, 2500, 100, 150, 200, 80, 0.45f, 0.4f, 3000, 1500, 0},
    {"X", Kind::Fricative, 400, 1300, 2400, 100, 150, 200, 100, 0, 0.55f, 1500, 1000, 0},
    {"h", Kind::Aspirate, 0, 0, 0, 0, 0, 0, 55, 0, 0, 0, 0, 0},
    // Plosives: f1-f3 are the closure's locus.
    {"p", Kind::Plosive, 250, 900, 2200, 100, 150, 200, 60, 0, 0, 0, 0, 0, 1000},
    {"b", Kind::Plosive, 250, 900, 2200, 100, 150, 200, 50, 0, 0, 0, 0, 0, 1000, true},
    {"t", Kind::Plosive, 250, 1700, 2700, 100, 150, 200, 55, 0, 0, 0, 0, 0, 4000},
    {"d", Kind::Plosive, 250, 1700, 2700, 100, 150, 200, 45, 0, 0, 0, 0, 0, 3800, true},
    {"k", Kind::Plosive, 250, 2000, 2300, 100, 150, 200, 60, 0, 0, 0, 0, 0, 2200},
    {"g", Kind::Plosive, 250, 2000, 2300, 100, 150, 200, 50, 0, 0, 0, 0, 0, 2200, true},
    {"?", Kind::Glottal, 0, 0, 0, 0, 0, 0, 35, 0, 0, 0, 0, 0},
    // Affricates: a closure at the locus, then the fricative.
    {"ts", Kind::Affricate, 250, 1700, 2700, 100, 150, 200, 40, 0, 0.65f, 5600, 1600, 0},
    {"tS", Kind::Affricate, 250, 1900, 2500, 100, 150, 200, 40, 0, 0.7f, 3000, 1500, 0},
};
const Phoneme* Find(std::string_view token) noexcept {
    for (const auto& p : kPhonemes) if (token == p.code) return &p;
    return nullptr;
}
// A deterministic noise source: xorshift32, roughly Gaussian (sum of two uniforms), unit variance.
struct Noise {
    std::uint32_t state = 1;
    float Uniform() noexcept { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f; }
    float Next() noexcept { return (Uniform() + Uniform()) * 1.2247449f; }
};
float Smooth(float x) noexcept { x = (std::clamp)(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); }
}

void FormantSynthesizer::Resonator::Set(float hz, float bandwidth) noexcept {
    // Klatt (1980): y[n] = A x[n] + B y[n-1] + C y[n-2], unity gain at DC.
    hz = (std::min)(hz, 0.45f * kRate);
    const float r = std::exp(-kPi * bandwidth / kRate);
    c = -r * r; b = 2 * r * std::cos(2 * kPi * hz / kRate); a = 1 - b - c;
}
bool FormantSynthesizer::Known(std::string_view token) noexcept {
    if (!token.empty() && token.front() == '\'') token.remove_prefix(1);
    return Find(token) != nullptr;
}
bool FormantSynthesizer::Parse(std::string_view text, const SynthVoice& voice) noexcept {
    count_ = 0; words_ = 0; total_ = 0;
    const float rate = (std::clamp)(voice.rate, 0.4f, 2.5f);
    const auto samples = [&](float ms) { return static_cast<std::uint32_t>(std::lround(ms / rate * kRate / 1000.0f)); };
    unsigned word = 0; bool in_word = false;
    const auto add = [&](const Segment& s) { if (count_ >= kMaxSegments) return false; segments_[count_++] = s; return true; };
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && text[at] == ' ') ++at;
        if (at >= text.size()) break;
        std::size_t end = at; while (end < text.size() && text[end] != ' ') ++end;
        std::string_view token = text.substr(at, end - at); at = end;
        if (token == "|") { if (in_word) { ++word; in_word = false; } continue; }
        if (word >= kMaxWords) return false;
        bool stressed = false;
        if (token.front() == '\'') { stressed = true; token.remove_prefix(1); }
        const auto* p = Find(token);
        if (!p) return false;
        in_word = true;
        Segment s{};
        s.f[0] = p->f1; s.f[1] = p->f2; s.f[2] = p->f3; s.f[3] = 3500; s.f[4] = 4500;
        s.b[0] = p->b1; s.b[1] = p->b2; s.b[2] = p->b3;
        s.word = static_cast<std::uint8_t>(word);
        switch (p->kind) {
        case Kind::Vowel: case Kind::Approximant: case Kind::Nasal: case Kind::Fricative:
            s.voicing = p->voicing; s.frication = p->frication; s.fric_hz = p->fric_hz; s.fric_bw = p->fric_bw; s.nasal = p->nasal;
            s.vowel = p->kind == Kind::Vowel; s.stress = stressed ? 1.0f : 0.0f;
            s.samples = samples(p->ms * (stressed ? 1.35f : 1.0f));
            if (!add(s)) return false;
            break;
        case Kind::Aspirate:
            s.aspiration = 0.6f; s.samples = samples(p->ms); s.f[0] = 0; // Formants of the next sound (filled below).
            if (!add(s)) return false;
            break;
        case Kind::Glottal:
            s.samples = samples(p->ms); s.f[0] = 0; // A closure: silence, formants carried over.
            if (!add(s)) return false;
            break;
        case Kind::Plosive: case Kind::Affricate: {
            // Closure (silence, or a low voice bar), then the release.
            Segment closure = s; closure.voicing = p->voiced_stop ? 0.12f : 0.0f; closure.b[0] = 150;
            closure.samples = samples(p->ms);
            if (!add(closure)) return false;
            if (p->kind == Kind::Affricate) {
                Segment release = s; release.frication = p->frication; release.fric_hz = p->fric_hz; release.fric_bw = p->fric_bw;
                release.samples = samples(70); release.transition_fast = true;
                if (!add(release)) return false;
                break;
            }
            Segment burst = s; burst.frication = p->voiced_stop ? 0.35f : 0.6f; burst.fric_hz = p->burst_hz; burst.fric_bw = p->burst_hz * 0.6f;
            burst.samples = samples(p->voiced_stop ? 6.0f : 10.0f); burst.transition_fast = true;
            if (!add(burst)) return false;
            if (!p->voiced_stop) { // Aspiration: noise through the next vowel's formants, from the locus.
                Segment aspiration = s; aspiration.aspiration = 0.45f; aspiration.samples = samples(35); aspiration.f[0] = -1;
                if (!add(aspiration)) return false;
            }
            break;
        }
        }
    }
    words_ = in_word ? word + 1 : word;
    if (!count_) return false;
    // Formants taken from the next sound that has its own (aspiration, h) or from the previous one (glottal stops).
    for (unsigned i = 0; i < count_; ++i) {
        auto& s = segments_[i];
        if (s.f[0] > 0) continue;
        const bool from_next = s.f[0] < 0 || s.aspiration > 0;
        const Segment* source = nullptr;
        if (from_next) for (unsigned j = i + 1; j < count_ && !source; ++j) if (segments_[j].f[0] > 0 && (segments_[j].vowel || segments_[j].voicing > 0.5f)) source = &segments_[j];
        for (unsigned j = i; j-- > 0 && !source;) if (segments_[j].f[0] > 0) source = &segments_[j];
        if (!source) for (unsigned j = i + 1; j < count_ && !source; ++j) if (segments_[j].f[0] > 0) source = &segments_[j];
        if (!source) { s.f[0] = 500; s.f[1] = 1500; s.f[2] = 2500; s.f[3] = 3500; s.f[4] = 4500; s.b[0] = 80; s.b[1] = 100; s.b[2] = 150; continue; }
        std::memcpy(s.f, source->f, sizeof(s.f)); std::memcpy(s.b, source->b, sizeof(s.b));
    }
    // The last vowel of the utterance is lengthened (phrase-final lengthening).
    for (unsigned i = count_; i-- > 0;) if (segments_[i].vowel) { segments_[i].samples = segments_[i].samples * 6 / 5; break; }
    total_ = kLeadSamples + kTailSamples;
    for (unsigned i = 0; i < count_; ++i) total_ += segments_[i].samples;
    return true;
}
std::size_t FormantSynthesizer::Samples(std::string_view phonemes, const SynthVoice& voice) noexcept {
    return Parse(phonemes, voice) ? total_ : 0;
}
std::size_t FormantSynthesizer::Speak(std::string_view phonemes, const SynthVoice& voice, std::span<float> out, SynthWord* words,
                                      unsigned word_capacity, unsigned* word_count) noexcept {
    if (word_count) *word_count = 0;
    if (!Parse(phonemes, voice) || out.size() < total_) return 0;
    std::fill(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(total_), 0.0f);
    const float scale = (std::clamp)(voice.formant_scale, 0.7f, 1.5f);
    const float f0 = (std::clamp)(voice.f0, 50.0f, 500.0f);
    const float oq = (std::clamp)(voice.open_quotient, 0.3f, 0.85f), tp = oq * 0.66f;
    const float tilt = (std::clamp)(voice.tilt, 0.0f, 0.95f);
    Noise noise; noise.state = voice.seed ? voice.seed * 2654435761u : 1u;
    Noise jitter; jitter.state = (voice.seed + 0x9e37u) * 2246822519u | 1u;
    Resonator cascade[5], nasal_pole, frication;
    nasal_pole.Set(270 * scale, 100);
    std::size_t speech = 0; for (unsigned i = 0; i < count_; ++i) speech += segments_[i].samples;
    // Running parameters, gliding toward each segment's targets.
    Segment previous = segments_[0];
    previous.voicing = previous.aspiration = previous.frication = 0;
    float phase = 0, period_scale = 1, amplitude = 1, flow_last = 0, tilt_state = 0, stress_env = 0;
    std::size_t n = kLeadSamples;
    unsigned spans = 0;
    for (unsigned k = 0; k < count_; ++k) {
        const auto& s = segments_[k];
        if (words && (k == 0 || segments_[k - 1].word != s.word) && spans < word_capacity) { words[spans].begin = static_cast<std::uint32_t>(n); words[spans].end = static_cast<std::uint32_t>(n); }
        // Formants move over 35 ms or 40 % of the segment (a burst at once); source amplitudes over 6 ms.
        const float glide = s.transition_fast ? 0.003f * kRate : (std::min)(0.035f * kRate, 0.4f * static_cast<float>(s.samples));
        const float level = (std::min)(0.006f * kRate, 0.5f * static_cast<float>(s.samples));
        for (std::uint32_t t = 0; t < s.samples; t += 40) { // Parameters per 2.5 ms block.
            const float g = Smooth(static_cast<float>(t) / (std::max)(glide, 1.0f)), a = Smooth(static_cast<float>(t) / (std::max)(level, 1.0f));
            float f[5], b[3];
            for (unsigned j = 0; j < 5; ++j) f[j] = (previous.f[j] + (s.f[j] - previous.f[j]) * g) * scale;
            for (unsigned j = 0; j < 3; ++j) b[j] = previous.b[j] + (s.b[j] - previous.b[j]) * g;
            const float voicing = previous.voicing + (s.voicing - previous.voicing) * a;
            const float aspiration = previous.aspiration + (s.aspiration - previous.aspiration) * a;
            const float fric = previous.frication + (s.frication - previous.frication) * a;
            const float nasal = previous.nasal + (s.nasal - previous.nasal) * g;
            const float fric_hz = (s.frication > 0 ? s.fric_hz : previous.fric_hz) * scale, fric_bw = (s.frication > 0 ? s.fric_bw : previous.fric_bw) * scale;
            cascade[0].Set(f[0], b[0]); cascade[1].Set(f[1], b[1]); cascade[2].Set(f[2], b[2]);
            cascade[3].Set(f[3], 250); cascade[4].Set(f[4], 300);
            if (fric_hz > 0) frication.Set(fric_hz, fric_bw);
            const std::uint32_t block = (std::min)(40u, s.samples - t);
            for (std::uint32_t i = 0; i < block; ++i, ++n) {
                // Pitch: declination over the utterance, a rise on stressed vowels, a final fall.
                const float u = static_cast<float>(n - kLeadSamples) / static_cast<float>(speech ? speech : 1);
                stress_env += ((s.vowel ? s.stress : 0.0f) - stress_env) * (1.0f / (0.05f * kRate));
                float pitch = f0 * (1 + voice.intonation * (0.08f - 0.16f * u)) * (1 + voice.intonation * 0.12f * stress_env);
                if (u > 0.85f) pitch *= 1 - voice.intonation * 0.10f * (u - 0.85f) / 0.15f;
                // Glottal source: Rosenberg pulse, differentiated (lip radiation), with jitter and shimmer per period.
                phase += pitch * period_scale / kRate;
                if (phase >= 1) { phase -= 1; period_scale = 1 + voice.jitter * jitter.Uniform(); amplitude = 1 + voice.shimmer * jitter.Uniform(); }
                float flow = 0;
                if (phase < tp) flow = 0.5f * (1 - std::cos(kPi * phase / tp));
                else if (phase < oq) flow = std::cos(0.5f * kPi * (phase - tp) / (oq - tp));
                float source = (flow - flow_last) * amplitude * 8.0f; flow_last = flow;
                tilt_state = (1 - tilt) * source + tilt * tilt_state; source = tilt_state;
                const float hiss = noise.Next();
                const float excitation = voicing * (source + voice.breathiness * flow * hiss) + aspiration * 0.08f * hiss;
                float x = excitation;
                if (nasal > 0) x = (1 - nasal) * x + nasal * nasal_pole.Run(x);
                for (auto& r : cascade) x = r.Run(x);
                if (fric > 0 && fric_hz > 0) x += fric * 0.05f * frication.Run(noise.Next());
                out[n] = x;
            }
        }
        if (words && spans < word_capacity) words[spans].end = static_cast<std::uint32_t>(n);
        if (k + 1 == count_ || segments_[k + 1].word != s.word) ++spans;
        previous = s;
    }
    if (word_count) *word_count = (std::min)(spans, word_capacity);
    // Room: a Schroeder reverberator (four parallel combs, two all-passes) on the whole utterance and its tail.
    if (voice.reverb_seconds > 0) {
        static constexpr unsigned comb_delay[kCombs]{397, 431, 467, 503}, allpass_delay[kAllpasses]{89, 233};
        std::memset(comb_, 0, sizeof(comb_)); std::memset(allpass_, 0, sizeof(allpass_));
        float gain[kCombs];
        for (unsigned c = 0; c < kCombs; ++c) gain[c] = std::pow(10.0f, -3.0f * static_cast<float>(comb_delay[c]) / ((std::max)(voice.reverb_seconds, 0.05f) * kRate));
        const float mix = (std::clamp)(voice.reverb_mix, 0.0f, 1.0f);
        for (std::size_t i = 0; i < total_; ++i) {
            const float dry = out[i];
            float wet = 0;
            for (unsigned c = 0; c < kCombs; ++c) {
                float& slot = comb_[c][i % comb_delay[c]];
                const float y = slot; slot = dry + gain[c] * y; wet += y;
            }
            wet *= 0.25f;
            for (unsigned a = 0; a < kAllpasses; ++a) {
                float& slot = allpass_[a][i % allpass_delay[a]];
                const float y = slot - 0.5f * wet; slot = wet + 0.5f * y; wet = y;
            }
            out[i] = (1 - mix) * dry + mix * wet;
        }
    }
    float peak = 0; for (std::size_t i = 0; i < total_; ++i) peak = (std::max)(peak, std::fabs(out[i]));
    if (peak > 0 && std::isfinite(peak)) { const float g = 0.5f / peak; for (std::size_t i = 0; i < total_; ++i) out[i] *= g; }
    return total_;
}
}
