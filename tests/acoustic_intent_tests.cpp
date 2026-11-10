// The acoustic end of the voice path: FilterbankFeatures, AcousticIntentRecognizer and their place between MicroVad /
// VoiceGate and LocalIntentDispatcher.
// - Features: a tone lands in the filter whose centre is nearest; identical vectors however the samples are split;
//   a level change shifts every channel by the same amount; hostile samples stay finite.
// - Matching: level and rate invariance, order sensitivity, the length bounds of the alignment.
// - Synthetic words: formant glides (a->i, i->a, u->e) with steady vowels as fillers, spoken at other pitches, rates,
//   levels and with noise, through MicroVad -> VoiceGate -> recogniser -> dispatcher: the right intents, and nothing
//   for tones, noise bursts and steady vowels.
// - Corpus (tests/data/voice_corpus.bin, from tools/make_voice_templates.cpp): voices that are in no template saying
//   the six commands, and words that are no command, through the same chain with the shipped templates
//   (VoiceTemplates.hpp). Recognition, wrong commands and false accepts are counted and bounded.
// No allocation while streaming.
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ai/VoiceGate.hpp"
#include "media/ai/VoiceTemplates.hpp"
#include "media/audio/FilterbankFeatures.hpp"
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
#include <vector>

using namespace media;
using media::ai::AcousticIntentRecognizer;
using media::audio::FilterbankFeatures;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 40) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kRate = 16000;
std::uint64_t allocations = 0;
struct Rng {
    std::uint32_t state = 0x2468ace1u;
    float Next() noexcept { state = state * 1664525u + 1013904223u; return static_cast<float>(state >> 8) / 8388608.0f - 1.0f; }
};

// ---- Features ----
void Features() {
    static float tone[kRate], out[200 * FilterbankFeatures::kChannels], split[200 * FilterbankFeatures::kChannels];
    // A tone lands in the filter whose centre is nearest to it.
    for (const double hz : {250.0, 700.0, 1500.0, 3200.0, 6000.0}) {
        for (unsigned i = 0; i < kRate; ++i) tone[i] = static_cast<float>(0.3 * std::sin(2 * kPi * hz * i / kRate));
        FilterbankFeatures f; const auto n = f.Push({tone, kRate / 2}, out, 200);
        CHECK(n == 1 + (kRate / 2 - FilterbankFeatures::kFrame) / FilterbankFeatures::kHop);
        unsigned loudest = 0, nearest = 0; const float* v = out + static_cast<std::size_t>(n / 2) * FilterbankFeatures::kChannels;
        for (unsigned c = 0; c < FilterbankFeatures::kChannels; ++c) {
            if (v[c] > v[loudest]) loudest = c;
            if (std::fabs(FilterbankFeatures::CentreHz(c) - hz) < std::fabs(FilterbankFeatures::CentreHz(nearest) - hz)) nearest = c;
        }
        CHECK(loudest == nearest || loudest + 1 == nearest || loudest == nearest + 1);
        CHECK(v[loudest] > v[(loudest + 12) % FilterbankFeatures::kChannels] + 5); // Far channels are far quieter.
        // Leakage: the quietest channel is at least 12 natural-log units (52 dB) below the tone's. The Hann window's
        // sidelobes fall fast; without a window they stay within about 40 dB.
        float quietest = v[0]; for (unsigned c = 1; c < FilterbankFeatures::kChannels; ++c) quietest = (std::min)(quietest, v[c]);
        if (v[loudest] - quietest < 12) std::printf("  %.0f Hz: leakage only %.1f below the tone\n", hz, static_cast<double>(v[loudest] - quietest));
        CHECK(v[loudest] - quietest > 12);
    }
    // The same vectors however the samples are split.
    Rng rng; for (unsigned i = 0; i < kRate; ++i) tone[i] = 0.2f * rng.Next() + static_cast<float>(0.2 * std::sin(i * 0.05));
    FilterbankFeatures whole; const auto n = whole.Push({tone, kRate}, out, 200);
    for (const unsigned block : {1u, 7u, 160u, 399u, 401u, 1000u}) {
        FilterbankFeatures f; std::uint32_t got = 0;
        for (unsigned at = 0; at < kRate; at += block) got += f.Push({tone + at, (std::min)(block, kRate - at)}, split + static_cast<std::size_t>(got) * FilterbankFeatures::kChannels, 200 - got);
        CHECK(got == n && std::memcmp(out, split, sizeof(float) * n * FilterbankFeatures::kChannels) == 0);
    }
    // A capacity too small drops (and counts) the rest.
    FilterbankFeatures small; CHECK(small.Push({tone, kRate}, split, 10) == 10 && small.Dropped() == n - 10 && small.Frames() == n);
    // Four times the amplitude: 16 times the power, every channel ln(16) higher (the floor is far below).
    for (unsigned i = 0; i < kRate; ++i) tone[i] *= 4.0f;
    FilterbankFeatures louder; CHECK(louder.Push({tone, kRate}, split, 200) == n);
    float worst = 0;
    for (std::uint32_t i = 0; i < n * FilterbankFeatures::kChannels; ++i) worst = (std::max)(worst, std::fabs(split[i] - out[i] - std::log(16.0f)));
    CHECK(worst < 1e-3f);
    // Silence, NaN and infinities: finite vectors.
    for (unsigned i = 0; i < kRate; ++i) tone[i] = i % 3 == 0 ? std::numeric_limits<float>::quiet_NaN() : i % 3 == 1 ? std::numeric_limits<float>::infinity() : 0.0f;
    FilterbankFeatures hostile; const auto h = hostile.Push({tone, kRate}, split, 200);
    bool finite = h == n; for (std::uint32_t i = 0; i < h * FilterbankFeatures::kChannels; ++i) finite = finite && std::isfinite(split[i]);
    CHECK(finite);
}

// ---- Synthetic words: formant glides with a harmonic source ----
struct Vowel { double f1, f2; };
constexpr Vowel kA{750, 1200}, kI{300, 2300}, kU{320, 800}, kE{500, 1800}, kO{500, 900};
// A word of `seconds` gliding from vowel `from` to `to` (a steady vowel when they are equal), at pitch `pitch`, peak
// amplitude about `amplitude`, with 20 ms onset and 40 ms release. Appended to `out`.
void Glide(std::vector<float>& out, Vowel from, Vowel to, double seconds, double pitch, double amplitude) {
    const unsigned n = static_cast<unsigned>(seconds * kRate); double phase = 0;
    for (unsigned i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / n, f1 = from.f1 + (to.f1 - from.f1) * t, f2 = from.f2 + (to.f2 - from.f2) * t;
        const double p = pitch * (1.0 + 0.03 * std::sin(2 * kPi * 5 * i / kRate)); phase += 2 * kPi * p / kRate;
        double v = 0;
        for (unsigned k = 1; k * p < 7000; ++k) {
            const double f = k * p, g1 = (f - f1) / 90, g2 = (f - f2) / 120;
            v += (std::exp(-0.5 * g1 * g1) + 0.6 * std::exp(-0.5 * g2 * g2) + 0.02) / std::sqrt(static_cast<double>(k)) * std::sin(k * phase);
        }
        const double edge = (std::min)({1.0, i / (0.02 * kRate), (n - i) / (0.04 * kRate)});
        out.push_back(static_cast<float>(amplitude * 0.25 * edge * v));
    }
}
std::vector<float> Word(Vowel from, Vowel to, double seconds, double pitch, double amplitude = 0.3) {
    std::vector<float> w(1600, 0.0f); Glide(w, from, to, seconds, pitch, amplitude); w.insert(w.end(), 1600, 0.0f); return w;
}
// Features of a rendition, quantised like the template tool does.
struct Built { std::vector<std::int8_t> data; std::uint16_t frames = 0; };
Built Quantise(const std::vector<float>& samples, float scale) {
    static float scratch[AcousticIntentRecognizer::kMaxFrames * FilterbankFeatures::kChannels], features[AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions];
    Built b; b.frames = static_cast<std::uint16_t>(AcousticIntentRecognizer::Describe(samples, scratch, features));
    for (std::size_t i = 0; i < static_cast<std::size_t>(b.frames) * AcousticIntentRecognizer::kDimensions; ++i)
        b.data.push_back(static_cast<std::int8_t>(std::lround((std::clamp)(features[i] / scale, -127.0f, 127.0f))));
    return b;
}
void Matching() {
    static float scratch[AcousticIntentRecognizer::kMaxFrames * FilterbankFeatures::kChannels];
    static float a[AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions], b[AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions];
    static float rows[2 * (AcousticIntentRecognizer::kMaxFrames + 1)];
    const auto distance = [&](const std::vector<float>& x, const std::vector<float>& y) {
        const auto n = AcousticIntentRecognizer::Describe(x, scratch, a), m = AcousticIntentRecognizer::Describe(y, scratch, b);
        return n && m ? AcousticIntentRecognizer::Distance(a, n, b, m, rows, rows + AcousticIntentRecognizer::kMaxFrames + 1) : -2.0f;
    };
    const auto word = Word(kA, kI, 0.4, 120);
    CHECK(distance(word, word) == 0.0f);
    {   // The second half of every vector is 0.3 times the difference of the neighbouring frames' first half.
        const auto frames = AcousticIntentRecognizer::Describe(word, scratch, a); constexpr unsigned C = AcousticIntentRecognizer::kCepstra, D = AcousticIntentRecognizer::kDimensions;
        float worst = 0, spread = 0;
        for (std::uint32_t t = 1; t + 1 < frames; ++t) for (unsigned k = 0; k < C; ++k) {
            const float expected = 0.3f * (a[(t + 1) * D + k] - a[(t - 1) * D + k]);
            worst = (std::max)(worst, std::fabs(a[t * D + C + k] - expected)); spread = (std::max)(spread, std::fabs(expected));
        }
        CHECK(frames > 10 && worst < 1e-5f && spread > 0.1f);
        // Each cepstral coefficient has zero mean and a standard deviation of 4 over the utterance.
        for (unsigned k = 0; k < C; ++k) {
            double mean = 0, square = 0; for (std::uint32_t t = 0; t < frames; ++t) { mean += a[t * D + k]; square += static_cast<double>(a[t * D + k]) * a[t * D + k]; }
            mean /= frames; CHECK(std::fabs(mean) < 1e-3 && std::fabs(std::sqrt(square / frames - mean * mean) - 4.0) < 1e-2);
        }
    }
    {   // The alignment band: the same two sounds, 10 then 30 frames against 30 then 10, align perfectly only along a
        // path 20 frames off the diagonal, which the band (8 frames here) forbids.
        constexpr unsigned D = AcousticIntentRecognizer::kDimensions;
        for (unsigned t = 0; t < 40; ++t) for (unsigned k = 0; k < D; ++k) {
            a[t * D + k] = t < 10 ? (k == 0 ? 3.0f : 0.0f) : (k == 1 ? 3.0f : 0.0f);
            b[t * D + k] = t < 30 ? (k == 0 ? 3.0f : 0.0f) : (k == 1 ? 3.0f : 0.0f);
        }
        const float banded = AcousticIntentRecognizer::Distance(a, 40, b, 40, rows, rows + AcousticIntentRecognizer::kMaxFrames + 1);
        CHECK(banded > 0.5f);
        const float free = AcousticIntentRecognizer::Distance(a, 10, b, 10, rows, rows + AcousticIntentRecognizer::kMaxFrames + 1); // Identical first ten frames: 0.
        CHECK(free == 0.0f);
    }
    // Level: a tenth of the amplitude matches as the same (features are relative to the utterance).
    CHECK(distance(word, Word(kA, kI, 0.4, 120, 0.03)) < 0.05f);
    // Rate: spoken 30 % slower or faster, still far nearer than the reverse glide.
    const float slower = distance(word, Word(kA, kI, 0.52, 120)), faster = distance(word, Word(kA, kI, 0.3, 120));
    const float reverse = distance(word, Word(kI, kA, 0.4, 120)), other = distance(word, Word(kU, kE, 0.4, 120));
    std::printf("Glide a->i against itself 30 %% slower %.3f, faster %.3f; against i->a %.3f, u->e %.3f\n", static_cast<double>(slower), static_cast<double>(faster),
                static_cast<double>(reverse), static_cast<double>(other));
    CHECK(slower < 0.5f * reverse && faster < 0.5f * reverse && slower < 0.5f * other);
    // Too short to describe, and lengths that cannot be aligned.
    CHECK(AcousticIntentRecognizer::Describe(std::vector<float>(1000, 0.1f), scratch, a) == 0);
    CHECK(AcousticIntentRecognizer::Describe(std::vector<float>(kRate * 7, 0.1f), scratch, a) == 0); // More than kMaxFrames.
    const auto n = AcousticIntentRecognizer::Describe(word, scratch, a), m = AcousticIntentRecognizer::Describe(Word(kA, kI, 1.2, 120), scratch, b);
    CHECK(n && m && m > 2 * n && AcousticIntentRecognizer::Distance(a, n, b, m, rows, rows + AcousticIntentRecognizer::kMaxFrames + 1) < 0);
}

// ---- The chain: MicroVad -> VoiceGate -> recogniser -> dispatcher ----
struct Heard {
    struct Item { std::uint64_t onset = 0; char text[64]{}; ai::IntentKind kind = ai::IntentKind::None; ai::IntentStatus status = ai::IntentStatus::Empty; };
    Item items[512]{}; unsigned count = 0;
    ai::LocalIntentDispatcher dispatcher; ai::MediaBounds bounds;
    static void Handle(void* context, std::string_view text, const audio::Utterance& u) noexcept {
        auto& h = *static_cast<Heard*>(context);
        if (h.count >= 512) return;
        auto& item = h.items[h.count++]; item.onset = u.onset_frame;
        const auto length = (std::min)(text.size(), sizeof(item.text) - 1); std::memcpy(item.text, text.data(), length); item.text[length] = '\0';
        const auto intent = h.dispatcher.Dispatch(text, h.bounds); item.kind = intent.kind; item.status = intent.status;
    }
    static ai::IntentStatus Execute(void*, const ai::Intent&) noexcept { return ai::IntentStatus::Ok; }
    Heard() { dispatcher.Configure(&Heard::Execute, nullptr); bounds.duration_100ns = 6000000000ll; bounds.position_100ns = 1200000000ll; bounds.seekable = true;
        bounds.chapters = bounds.scenes = 3; } // A film with chapters and scenes: their spoken commands can run.
};
// A recording: quiet room noise with the utterances placed `gap` seconds apart; returns where each one starts.
struct Placed { std::uint64_t begin, end; };
std::vector<float> Recording(const std::vector<std::vector<float>>& utterances, double gap, std::vector<Placed>& placed, std::uint32_t seed) {
    std::vector<float> out(static_cast<std::size_t>(kRate), 0.0f);
    for (const auto& u : utterances) {
        placed.push_back({out.size(), out.size() + u.size()});
        out.insert(out.end(), u.begin(), u.end()); out.insert(out.end(), static_cast<std::size_t>(gap * kRate), 0.0f);
    }
    Rng rng; rng.state = seed;
    for (auto& s : out) s += 0.0005f * rng.Next(); // About -70 dBFS: the detector needs a floor.
    return out;
}
// Runs a recording through the chain; returns the recogniser's statistics.
struct Chain {
    core::LinearMemoryArena arena; audio::MicroVad vad; ai::AcousticIntentRecognizer recogniser; ai::VoiceGate gate; Heard heard;
    template <class Set> bool Setup(const Set& set) {
        if (!arena.Initialize(16 * 1024 * 1024) || !vad.Configure(arena) || !recogniser.Configure(arena, set)) return false;
        gate.Configure(vad, &AcousticIntentRecognizer::Hear, &recogniser, &Heard::Handle, &heard, 100);
        return arena.Seal();
    }
    void Run(const std::vector<float>& samples, double* seconds = nullptr) {
        allocation_probe::Begin();
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t at = 0; at < samples.size(); at += 160) (void)gate.Process({samples.data() + at, (std::min)(samples.size() - at, std::size_t{160})});
        if (seconds) *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        allocations += allocation_probe::End();
    }
    // The text heard for an utterance placed at [begin, end), or nullptr.
    const Heard::Item* At(const Placed& p) const {
        for (unsigned i = 0; i < heard.count; ++i) if (heard.items[i].onset + 1600 >= p.begin && heard.items[i].onset < p.end) return &heard.items[i];
        return nullptr;
    }
};
void Synthetic() {
    // Three command "words" and four steady vowels as fillers, recorded at pitch 120 Hz, 0.45 s.
    struct Spec { Vowel from, to; const char* text; std::uint16_t command; };
    const Spec specs[]{{kA, kI, "play", 0}, {kI, kA, "pause", 1}, {kU, kE, "mute", 2}, {kA, kA, "", ai::kFiller}, {kI, kI, "", ai::kFiller},
                       {kU, kU, "", ai::kFiller}, {kO, kO, "", ai::kFiller}};
    static Built built[7]; static ai::VoiceTemplate templates[7];
    for (unsigned i = 0; i < 7; ++i) {
        built[i] = Quantise(Word(specs[i].from, specs[i].to, 0.45, 120), 0.15f);
        templates[i] = {specs[i].text, specs[i].command, built[i].frames, built[i].data.data()};
        CHECK(built[i].frames >= AcousticIntentRecognizer::kMinFrames);
    }
    // Accept up to 5: the renditions below land at 0.3 to 3.9 when fed directly, a tone and a noise burst at 5.7 and 6.4.
    ai::VoiceTemplateSet set{templates, 0.15f, 5.0f, 0.10f};
    // Misuse: an empty set, a command without text, a scale of zero.
    {
        core::LinearMemoryArena arena; CHECK(arena.Initialize(1 << 20)); AcousticIntentRecognizer r;
        CHECK(!r.Configure(arena, {std::span<const ai::VoiceTemplate>{}, 0.1f, 4.0f, 0.1f}));
        ai::VoiceTemplate bad = templates[0]; bad.text = "";
        CHECK(!r.Configure(arena, {std::span<const ai::VoiceTemplate>{&bad, 1}, 0.1f, 4.0f, 0.1f}));
        CHECK(!r.Configure(arena, {templates, 0.0f, 4.0f, 0.1f}));
        CHECK(r.Configure(arena, set) && !r.Configure(arena, set));
        std::vector<float> hiss(static_cast<std::size_t>(0.5 * kRate)); Rng h; for (auto& x : hiss) x = 0.2f * h.Next();
        r.Begin(); r.Feed(hiss); CHECK(r.Finish().match == ai::AcousticMatch::Distant);
        r.Begin(); r.Feed(Word(kE, kE, 0.5, 140)); CHECK(r.Finish().match == ai::AcousticMatch::Filler); // A steady vowel: nearest the fillers.
        r.Begin(); r.Feed(Word(kA, kI, 0.45, 120)); const auto result = r.Finish();
        CHECK(result.match == ai::AcousticMatch::Accepted && result.command == 0 && std::string_view(result.text) == "play" && result.distance < 0.1f); // Quantisation only.
    }
    // A packed set is rebuilt into the plain set it stands for. Here the first five templates are packed losslessly
    // for their cepstra (8 bits, the levels being the signed-byte grid itself, every frame kept), so only the deltas
    // differ (made again from the rounded cepstra): both sets must decide alike, at nearly the same distances. Then
    // with every other frame dropped (the lines between), and misuse.
    {
        constexpr unsigned kPacked = 5, C = AcousticIntentRecognizer::kCepstra, D = AcousticIntentRecognizer::kDimensions;
        static ai::VoicePacked packed[kPacked]; static ai::VoiceWord words[kPacked];
        static float levels[C * 256]; static std::uint8_t keys[256], sparse[256], values[8192];
        for (unsigned k = 0; k < C; ++k) for (unsigned c = 0; c < 256; ++c) levels[k * 256 + c] = static_cast<float>(static_cast<int>(c) - 128) * set.scale;
        std::size_t frames = 0, bytes = 0;
        for (unsigned i = 0; i < kPacked; ++i) {
            words[i] = {specs[i].text, specs[i].command, ai::kEnglish}; packed[i] = {static_cast<std::uint16_t>(i), static_cast<std::uint8_t>(built[i].frames), 0};
            for (std::uint32_t f = 0; f < built[i].frames; ++f, ++frames) {
                keys[frames / 8] = static_cast<std::uint8_t>(keys[frames / 8] | (1u << (frames % 8)));
                if (f % 2 == 0 || f + 1 == built[i].frames) sparse[frames / 8] = static_cast<std::uint8_t>(sparse[frames / 8] | (1u << (frames % 8)));
                for (unsigned k = 0; k < C; ++k) values[bytes++] = static_cast<std::uint8_t>(built[i].data[static_cast<std::size_t>(f) * D + k] + 128);
            }
        }
        CHECK(frames < 256 * 8 && bytes <= sizeof(values));
        ai::VoiceTemplateSet first = set; first.templates = {templates, kPacked};
        const ai::VoicePackedSet whole{words, packed, {keys, (frames + 7) / 8}, {values, bytes}, levels, 8, first};
        core::LinearMemoryArena arena; CHECK(arena.Initialize(2 << 20)); AcousticIntentRecognizer plain, rebuilt;
        CHECK(plain.Configure(arena, first) && rebuilt.Configure(arena, whole) && !rebuilt.Configure(arena, whole));
        for (const auto& sound : {Word(kA, kI, 0.45, 120), Word(kI, kA, 0.5, 140), Word(kE, kE, 0.5, 140)}) {
            plain.Begin(); plain.Feed(sound); rebuilt.Begin(); rebuilt.Feed(sound);
            const auto a = plain.Finish(), b = rebuilt.Finish();
            CHECK(a.match == b.match && a.command == b.command && std::fabs(a.distance - b.distance) < 0.05f);
        }
        // Every other frame kept: the values of the kept ones only (the sparse key bits select them from the same codes
        // here, so the stream is rebuilt for them), and the words are still told apart.
        static std::uint8_t sparse_values[8192]; std::size_t sparse_bytes = 0, frame = 0;
        for (unsigned i = 0; i < kPacked; ++i)
            for (std::uint32_t f = 0; f < built[i].frames; ++f, ++frame)
                if ((sparse[frame / 8] >> (frame % 8)) & 1u)
                    for (unsigned k = 0; k < C; ++k) sparse_values[sparse_bytes++] = static_cast<std::uint8_t>(built[i].data[static_cast<std::size_t>(f) * D + k] + 128);
        AcousticIntentRecognizer thinned;
        CHECK(thinned.Configure(arena, ai::VoicePackedSet{words, packed, {sparse, (frames + 7) / 8}, {sparse_values, sparse_bytes}, levels, 8, first}));
        thinned.Begin(); thinned.Feed(Word(kA, kI, 0.45, 120));
        const auto heard = thinned.Finish();
        CHECK(heard.match == ai::AcousticMatch::Accepted && heard.command == 0);
        // Misuse: too few values, too few key bits, a first frame not kept, a level table of another size, 0 bits.
        AcousticIntentRecognizer bad;
        CHECK(!bad.Configure(arena, ai::VoicePackedSet{words, packed, whole.keys, {values, bytes - 1}, levels, 8, first}));
        CHECK(!bad.Configure(arena, ai::VoicePackedSet{words, packed, {keys, frames / 8 - 1}, whole.values, levels, 8, first}));
        static std::uint8_t holed[256]; std::memcpy(holed, keys, sizeof(holed)); holed[0] = static_cast<std::uint8_t>(holed[0] & ~1u);
        CHECK(!bad.Configure(arena, ai::VoicePackedSet{words, packed, {holed, (frames + 7) / 8}, whole.values, levels, 8, first}));
        CHECK(!bad.Configure(arena, ai::VoicePackedSet{words, packed, whole.keys, whole.values, {levels, C * 128}, 8, first}));
        CHECK(!bad.Configure(arena, ai::VoicePackedSet{words, packed, whole.keys, whole.values, {levels, C}, 0, first}));
    }
    // Renditions nothing was recorded from: other pitches, rates, levels, and noise; and sounds that are no command.
    std::vector<std::vector<float>> utterances; std::vector<int> expect; // expect: command, or -1 for nothing.
    Rng noise;
    for (unsigned s = 0; s < 3; ++s)
        for (const double pitch : {95.0, 150.0, 210.0}) for (const double seconds : {0.34, 0.6}) {
            utterances.push_back(Word(specs[s].from, specs[s].to, seconds, pitch, pitch > 200 ? 0.05 : 0.3)); expect.push_back(s);
        }
    for (unsigned s = 0; s < 3; ++s) { // At 20 dB signal-to-noise.
        auto w = Word(specs[s].from, specs[s].to, 0.45, 130); for (auto& x : w) x += 0.02f * noise.Next(); utterances.push_back(w); expect.push_back(s);
    }
    std::vector<float> tone(static_cast<std::size_t>(0.5 * kRate)); for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<float>(0.2 * std::sin(2 * kPi * 440 * i / kRate));
    std::vector<float> burst(static_cast<std::size_t>(0.4 * kRate)); for (auto& x : burst) x = 0.2f * noise.Next();
    utterances.push_back(tone); expect.push_back(-1);                        // A steady tone: stationary, and no word.
    utterances.push_back(burst); expect.push_back(-1);                       // Broadband noise: no voiced onset.
    utterances.push_back(Word(kE, kE, 0.5, 140)); expect.push_back(-1);      // A steady vowel: nearest a filler.
    utterances.push_back(Word(kO, kU, 0.5, 110)); expect.push_back(-1);      // A glide that is no command.
    std::vector<Placed> placed;
    const auto recording = Recording(utterances, 1.0, placed, 7);
    Chain chain; CHECK(chain.Setup(set));
    chain.Run(recording);
    unsigned right = 0, wrong = 0, missed = 0, accepted_noise = 0;
    for (std::size_t i = 0; i < utterances.size(); ++i) {
        const auto* item = chain.At(placed[i]);
        if (expect[i] < 0) { if (item) { ++accepted_noise; std::printf("  synthetic %zu: accepted \"%s\"\n", i, item->text); } continue; }
        if (!item) { ++missed; std::printf("  synthetic %zu (%s): nothing\n", i, specs[expect[i]].text); }
        else if (std::string_view(item->text) == specs[expect[i]].text) ++right;
        else { ++wrong; std::printf("  synthetic %zu (%s): heard \"%s\"\n", i, specs[expect[i]].text, item->text); }
    }
    const auto& s = chain.recogniser.Stats();
    std::printf("Synthetic words: %u right, %u wrong, %u missed of %zu; %u of 4 non-words accepted; recogniser: %u utterances, %u accepted, %u filler, %u distant, %u ambiguous, %u too short\n",
                right, wrong, missed, expect.size() - 4, accepted_noise, s.utterances, s.accepted, s.filler, s.distant, s.ambiguous, s.too_short);
    CHECK(right == 21 && wrong == 0 && missed == 0 && accepted_noise == 0);
    // Every accepted word went on to the grammar and came back as its intent.
    unsigned intents = 0;
    for (unsigned i = 0; i < chain.heard.count; ++i) {
        const auto& h = chain.heard.items[i]; const std::string_view t = h.text;
        const auto kind = t == "play" ? ai::IntentKind::Play : t == "pause" ? ai::IntentKind::Pause : ai::IntentKind::Mute;
        if (h.kind == kind && h.status == ai::IntentStatus::Ok) ++intents;
    }
    CHECK(intents == chain.heard.count && intents == 21);
}

// ---- The corpus: voices in no template, and words that are no command ----
struct CorpusItem { int command; char label[96]; std::vector<float> samples; };
bool LoadCorpus(const char* path, std::vector<CorpusItem>& items) {
    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) f = nullptr;
#else
    f = std::fopen(path, "rb");
#endif
    if (!f) return false;
    char magic[4]{}; std::uint32_t count = 0; bool good = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "MVC1", 4) == 0 && std::fread(&count, 4, 1, f) == 1;
    for (std::uint32_t i = 0; good && i < count; ++i) {
        CorpusItem item{}; std::uint32_t command = 0, label = 0, samples = 0;
        good = std::fread(&command, 4, 1, f) == 1 && std::fread(&label, 4, 1, f) == 1 && label < sizeof(item.label);
        good = good && std::fread(item.label, 1, label, f) == label && std::fread(&samples, 4, 1, f) == 1 && samples < 5u * kRate;
        if (!good) break;
        item.command = static_cast<std::int32_t>(command);
        std::vector<std::int16_t> pcm(samples); good = std::fread(pcm.data(), 2, samples, f) == samples;
        for (auto v : pcm) item.samples.push_back(static_cast<float>(v) / 32768.0f);
        items.push_back(std::move(item));
    }
    std::fclose(f);
    return good && !items.empty();
}
// What the shipped templates must still do: bounds a little below what this chain measured when they were generated
// (docs/voice.md). Regression guards, not targets.
constexpr unsigned kStandalone = 14;                    // Vocabulary words the grammar reads alone, 7 held-out renditions each.
// Measured 2026-10-15 with the packed set and per-language settings: 68 and 46 (69 and 39 on 2026-10-14; 76 and 50
// before the wake word and Hebrew); 2026-10-16 with the chapter and scene words: 67 and 46, 1 wrong at 20 dB.
constexpr unsigned kCleanRight = 65, kNoisyRight = 43;  // Of 98.
constexpr unsigned kNoisyWrong = 3;                     // At 20 dB (measured: mute as unmute, unmute as pause, עצור as skip; 1 before); clean: none.
constexpr unsigned kCleanAccepted = 10, kNoisyAccepted = 8, kUnrelatedAccepted = 6; // Of 45 words that are no command (8 and 5; 4 and 1 unrelated).
// Of 120 command sentences (2026-10-16, with the four chapter and scene sentences: 85, 0 wrong; of them the first 100
// as before 73, 76 with the smaller vocabulary, 62 before that; the new 20: 12) and 35 without one (5).
constexpr unsigned kSentences = 120, kSentencesRight = 82, kSentencesWrong = 0, kPlainHeard = 8;
// The shipped acoustic data (key bits, values, levels, template and word tables): 864,488 bytes measured on 2026-10-16
// (688,602 before the chapter and scene words). The 64 KB the voice directive asked for was reached (3 bits, three
// renditions per word and speaker) but made wrong commands, so the shipped set is the 8-bit one (docs/voice.md, "Packed
// templates"); this bound keeps it from growing unnoticed.
constexpr std::size_t kShippedBytes = 880 * 1024;
void Corpus(const char* path) {
    std::vector<CorpusItem> items;
    if (!LoadCorpus(path, items)) { std::printf("FAIL cannot read the corpus %s\n", path); ++failures; return; }
    // The vocabulary in the tool's order (the corpus numbers it). Words the grammar reads alone are commands said
    // alone; the rest ("turn", "ten", "seconds") exist for sentences and must never run by themselves.
    const char* const texts[]{"play", "pause", "rewind", "mute", "\xd7\xa0\xd7\x92\xd7\x9f", "\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8", "stop", "resume", "back",
                              "forward", "skip", "unmute", "louder", "quieter", "turn", "up", "down", "volume", "go", "one", "five", "ten", "twenty",
                              "thirty", "minute", "minutes", "seconds", "next", "previous", "chapter", "scene"}; // Chapters and scenes: sentence words only.
    constexpr unsigned kWords = sizeof(texts) / sizeof(texts[0]);
    ai::IntentKind kinds[kWords]{}; bool alone[kWords]{}; unsigned standalone = 0;
    const ai::MediaBounds film{24ll * 3600 * ai::LocalIntentDispatcher::kSecond, 3600 * ai::LocalIntentDispatcher::kSecond, 0.5f, false, false, true, true, 0, 0, 1, 1};
    for (unsigned w = 0; w < kWords; ++w) {
        const auto intent = ai::LocalIntentDispatcher::Parse(texts[w], film);
        alone[w] = intent.status == ai::IntentStatus::Ok; kinds[w] = intent.kind; standalone += alone[w] ? 1u : 0u;
    }
    const auto& set = ai::voice_templates::kSet;
    unsigned commands = 0, near_words = 0; for (const auto& t : set.templates) { if (set.words[t.word].command != ai::kFiller) ++commands; }
    std::printf("Shipped templates: %u command renditions, %zu fillers; corpus: %zu utterances\n", commands, set.templates.size() - commands, items.size());
    // The shipped acoustic data (docs/voice.md, "Packed templates"): key bits, values and levels, and the template and
    // word tables.
    const std::size_t acoustic = sizeof(ai::voice_templates::kKeys) + sizeof(ai::voice_templates::kValues) + sizeof(ai::voice_templates::kLevels),
                      tables = sizeof(ai::voice_templates::kTemplates) + sizeof(ai::voice_templates::kWords);
    std::printf("Shipped acoustic data: %zu bytes of key bits, values and levels, %zu bytes of tables: %zu bytes in all\n", acoustic, tables, acoustic + tables);
    CHECK(acoustic + tables <= kShippedBytes);
    for (const double snr : {0.0, 20.0}) {
        std::vector<std::vector<float>> utterances; Rng noise; noise.state = 12345;
        for (const auto& item : items) {
            auto u = item.samples;
            if (snr > 0) { // White noise at the given signal-to-noise ratio over the word's active samples.
                double power = 0; std::size_t active = 0; for (float x : u) if (std::fabs(x) > 1e-4f) { power += static_cast<double>(x) * x; ++active; }
                const float amplitude = static_cast<float>(std::sqrt(power / static_cast<double>(active ? active : 1)) * std::pow(10.0, -snr / 20.0) * std::sqrt(3.0));
                for (auto& x : u) x += amplitude * noise.Next();
            }
            utterances.push_back(std::move(u));
        }
        std::vector<Placed> placed;
        const auto recording = Recording(utterances, 1.0, placed, 99);
        Chain chain; CHECK(chain.Setup(set));
        double seconds = 0; chain.Run(recording, &seconds);
        unsigned right = 0, wrong = 0, refused = 0, held = 0, accepted = 0, impostors = 0, accepted_near = 0, near_count = 0, intents_wrong = 0;
        unsigned parts = 0, parts_run = 0, unknown = 0, synonyms = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto* heard = chain.At(placed[i]);
            if (items[i].command >= 0) {
                const auto w = static_cast<unsigned>(items[i].command);
                if (w >= kWords) { ++unknown; continue; }
                if (!alone[w]) { // A sentence word said alone: nothing may run.
                    ++parts;
                    if (heard) { ++parts_run; std::printf("  %-44s ran as \"%s\"\n", items[i].label, heard->text); }
                    continue;
                }
                ++held;
                if (!heard) { ++refused; continue; }
                // Right: the action the word asks for ("stop" heard for "pause" pauses too, and is counted apart).
                const auto got = ai::LocalIntentDispatcher::Parse(heard->text, film), want = ai::LocalIntentDispatcher::Parse(texts[w], film);
                if (got.kind == want.kind && got.target_100ns == want.target_100ns) {
                    ++right; if (heard->kind != kinds[w] || heard->status != ai::IntentStatus::Ok) ++intents_wrong;
                    if (std::string_view(heard->text) != texts[w]) { ++synonyms; std::printf("  %-44s heard as \"%s\" (the same action)\n", items[i].label, heard->text); }
                } else { ++wrong; std::printf("  %-44s heard as \"%s\" (WRONG COMMAND)\n", items[i].label, heard->text); }
            } else {
                const std::string_view label = items[i].label;
                const bool near_word = label.starts_with("remind") || label.starts_with("stay") || label.starts_with("player") || label.starts_with("nager");
                ++impostors; if (near_word) ++near_count;
                if (heard) { ++accepted; if (near_word) ++accepted_near; std::printf("  %-44s accepted as \"%s\"%s\n", items[i].label, heard->text, near_word ? " (sounds close to a command)" : ""); }
            }
        }
        near_words = near_count;
        const auto& s = chain.recogniser.Stats();
        std::printf("Corpus%s: %u commands said alone by held-out voices: %u right (%u by another word of the same action), %u wrong, %u refused of %u; %u sentence words said alone, %u ran; "
                    "words that are no command accepted %u of %u (%u of %u that sound close to a command); "
                    "%u utterances recognised in %.1f ms (%.2f ms each, detector and features included)\n",
                    snr > 0 ? " at 20 dB SNR" : " (clean)", standalone, right, synonyms, wrong, refused, held, parts, parts_run, accepted, impostors, accepted_near, near_words,
                    s.utterances, seconds * 1000, s.utterances ? seconds * 1000 / s.utterances : 0.0);
        // Bounds a little below what this chain measured when the templates were generated (see docs/voice.md):
        // regression guards, not targets. No command may ever be taken for another.
        CHECK(standalone == kStandalone && held == 7 * kStandalone && parts == 7 * (kWords - kStandalone) && impostors == 45 && unknown == 0);
        CHECK(wrong <= (snr > 0 ? kNoisyWrong : 0u) && intents_wrong == 0 && parts_run == 0);
        CHECK(right >= (snr > 0 ? kNoisyRight : kCleanRight));
        CHECK(accepted <= (snr > 0 ? kNoisyAccepted : kCleanAccepted) && accepted - accepted_near <= kUnrelatedAccepted);
    }
}
}
// ---- Connected words ----
// Phrase: the command among the decoded words, never one that loses an amount.
void Phrasing() {
    const auto phrase = [](std::initializer_list<const char*> words) {
        static char out[256];
        const auto n = AcousticIntentRecognizer::Phrase(std::span<const char* const>{words.begin(), words.size()}, out, sizeof(out));
        return std::string_view{out, n};
    };
    allocation_probe::Begin();
    CHECK(phrase({"pause"}) == "pause" && phrase({"pause", "up"}) == "pause" && phrase({"stop", "up"}) == "stop");
    CHECK(phrase({"volume", "up", "up"}) == "volume up" && phrase({"turn", "up", "volume", "up"}) == "turn up volume");
    CHECK(phrase({"skip", "forward", "thirty", "seconds"}) == "skip forward thirty seconds");
    CHECK(phrase({"five", "skip", "skip", "forward", "thirty", "seconds"}).empty()); // Dropping "five" would lose an amount.
    CHECK(phrase({"go", "back", "one"}).empty());                                  // "go back" would be 10 s, not one minute.
    CHECK(phrase({"go", "back", "ten", "pause"}).empty() && phrase({"go", "back", "twenty"}).empty());
    CHECK(phrase({"rewind", "ten", "turn", "minutes"}).empty());                   // "rewind ten" would be 10 s, not 10 minutes.
    CHECK(phrase({"resume", "play", "back"}) == "resume");                         // A direction is no amount.
    CHECK(phrase({"up", "down"}).empty() && phrase({}).empty() && phrase({"one", "five"}).empty());
    char tiny[4]; const char* const words[]{"pause"};
    CHECK(AcousticIntentRecognizer::Phrase(words, tiny, sizeof(tiny)) == 0 && tiny[0] == 0); // No room: nothing, not a cut word.
    CHECK(allocation_probe::End() == 0);
}
// Decode on synthetic words: two command glides in one breath, a filler before one, and noise.
void Decoding() {
    struct Spec { Vowel from, to; const char* text; std::uint16_t command; };
    const Spec specs[]{{kA, kI, "play", 0}, {kU, kE, "mute", 2}, {kO, kO, "", ai::kFiller}};
    static Built built[3]; static ai::VoiceTemplate templates[3]; std::vector<std::vector<float>> blocks(3); const float* pointers[3];
    for (unsigned i = 0; i < 3; ++i) {
        built[i] = Quantise(Word(specs[i].from, specs[i].to, 0.45, 120), 0.15f);
        templates[i] = {specs[i].text, specs[i].command, built[i].frames, built[i].data.data()};
        for (std::size_t v = 0; v < built[i].data.size(); ++v) blocks[i].push_back(static_cast<float>(built[i].data[v]) * 0.15f);
        pointers[i] = blocks[i].data();
    }
    std::vector<float> scratch(AcousticIntentRecognizer::DecodeScratch(templates));
    static float mel[AcousticIntentRecognizer::kMaxFrames * FilterbankFeatures::kChannels], features[AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions];
    const auto decode = [&](const std::vector<float>& samples, std::string& out) {
        const auto frames = AcousticIntentRecognizer::Describe(samples, mel, features);
        ai::SpottedWord words[AcousticIntentRecognizer::kMaxWords];
        allocation_probe::Begin();
        const auto n = frames ? AcousticIntentRecognizer::Decode(features, frames, templates, pointers, -6.0f, 2.0f, scratch.data(), words, AcousticIntentRecognizer::kMaxWords) : 0u;
        allocations += allocation_probe::End();
        out.clear();
        for (unsigned i = 0; i < n; ++i) { if (!out.empty()) out += ' '; out += templates[words[i].template_index].command == ai::kFiller ? "~" : templates[words[i].template_index].text; }
        return n;
    };
    const auto join = [](std::initializer_list<std::vector<float>> parts) { std::vector<float> all; for (const auto& p : parts) all.insert(all.end(), p.begin(), p.end()); return all; };
    std::string heard;
    // Spoken without a pause between them (each word keeps 0.1 s of its padding: a short breath).
    (void)decode(join({Word(kA, kI, 0.42, 130), Word(kU, kE, 0.5, 110)}), heard);
    std::printf("Decode: play+mute -> \"%s\"", heard.c_str()); CHECK(heard == "play mute");
    (void)decode(join({Word(kU, kE, 0.4, 140), Word(kA, kI, 0.5, 100)}), heard);
    std::printf(", mute+play -> \"%s\"", heard.c_str()); CHECK(heard == "mute play");
    (void)decode(join({Word(kO, kO, 0.45, 120), Word(kA, kI, 0.45, 120)}), heard);
    std::printf(", filler+play -> \"%s\"", heard.c_str()); CHECK(heard == "~ play");
    std::vector<float> hiss(static_cast<std::size_t>(0.8 * kRate)); Rng h; for (auto& x : hiss) x = 0.2f * h.Next();
    (void)decode(hiss, heard);
    std::printf(", noise -> \"%s\"\n", heard.c_str()); CHECK(heard.find("play") == std::string::npos && heard.find("mute") == std::string::npos);
}
// The held-out voices' spoken sentences (tests/data/voice_sentences.bin) through MicroVad -> VoiceGate -> recogniser ->
// dispatcher with the shipped templates: the command each one means, or none for plain talk.
void Sentences(const char* path) {
    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) f = nullptr;
#else
    f = std::fopen(path, "rb");
#endif
    if (!f) { std::printf("FAIL cannot read the sentence corpus %s\n", path); ++failures; return; }
    struct Item { std::string label, means; std::vector<float> samples; };
    std::vector<Item> items;
    char magic[4]{}; std::uint32_t count = 0; bool good = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "MVS1", 4) == 0 && std::fread(&count, 4, 1, f) == 1;
    for (std::uint32_t i = 0; good && i < count; ++i) {
        Item item; std::uint32_t label = 0, means = 0, samples = 0;
        good = std::fread(&label, 4, 1, f) == 1 && label < 512; item.label.resize(label); good = good && std::fread(item.label.data(), 1, label, f) == label;
        good = good && std::fread(&means, 4, 1, f) == 1 && means < 256; item.means.resize(means); good = good && std::fread(item.means.data(), 1, means, f) == means;
        good = good && std::fread(&samples, 4, 1, f) == 1 && samples < 7u * kRate;
        if (!good) break;
        std::vector<std::int16_t> pcm(samples); good = std::fread(pcm.data(), 2, samples, f) == samples;
        for (auto v : pcm) item.samples.push_back(static_cast<float>(v) / 32768.0f);
        items.push_back(std::move(item));
    }
    std::fclose(f);
    if (!good || items.empty()) { std::printf("FAIL the sentence corpus %s is malformed\n", path); ++failures; return; }
    std::vector<std::vector<float>> utterances; for (const auto& item : items) utterances.push_back(item.samples);
    std::vector<Placed> placed;
    const auto recording = Recording(utterances, 1.2, placed, 4242);
    Chain chain; CHECK(chain.Setup(ai::voice_templates::kSet));
    double seconds = 0; chain.Run(recording, &seconds);
    unsigned right = 0, wrong = 0, missed = 0, said = 0, plain = 0, invented = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto* heard = chain.At(placed[i]);
        const bool command = heard && heard->status == ai::IntentStatus::Ok;
        if (items[i].means.empty()) { ++plain; if (command) { ++invented; std::printf("  %-58s heard as \"%s\"\n", items[i].label.c_str(), heard->text); } continue; }
        ++said;
        if (!command) { ++missed; std::printf("  %-58s missed%s%s\n", items[i].label.c_str(), heard ? ": heard as " : "", heard ? heard->text : ""); continue; }
        const auto got = ai::LocalIntentDispatcher::Parse(heard->text, chain.heard.bounds), want = ai::LocalIntentDispatcher::Parse(items[i].means, chain.heard.bounds);
        if (got.kind == want.kind && got.target_100ns == want.target_100ns && got.volume == want.volume) ++right;
        else { ++wrong; std::printf("  %-58s heard as \"%s\" (WRONG COMMAND)\n", items[i].label.c_str(), heard->text); }
    }
    const auto& s = chain.recogniser.Stats();
    std::printf("Sentences by held-out voices: %u of %u commands right, %u wrong, %u missed; %u of %u sentences without a command heard as one; "
                "%u decoded as sentences; %.1f ms for %zu utterances (%.1f ms each)\n", right, said, wrong, missed, invented, plain, s.sentences, seconds * 1000,
                items.size(), seconds * 1000 / static_cast<double>(items.size()));
    // Bounds a little below what the template tool measured on these same renditions: regression guards, not targets.
    CHECK(said == kSentences && plain == 35);
    CHECK(right >= kSentencesRight && wrong <= kSentencesWrong && invented <= kPlainHeard);
}
int main(int argc, char** argv) {
    Features();
    Matching();
    Synthetic();
    Phrasing();
    Decoding();
    Corpus(argc > 1 ? argv[1] : "tests/data/voice_corpus.bin");
    Sentences(argc > 2 ? argv[2] : "tests/data/voice_sentences.bin");
    CHECK(allocations == 0);
    std::printf("Acoustic intents: %s (%d failures, %llu allocations while streaming)\n", failures ? "FAIL" : "PASS", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
