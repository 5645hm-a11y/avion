// The wake word (docs/voice.md, "Wake word"): ai::AvionWakeGate on its own, then the whole voice path with it,
// MicroVad -> VoiceGate -> AcousticIntentRecognizer (the shipped templates) -> AvionWakeGate -> LocalIntentDispatcher,
// on speakers that are in no template: Mark and Julie (tests/data/voice_wake.bin, voice_sentences.bin) and the formant
// synthesiser's held-out speakers (media/ai/VoiceLexicon.hpp, synthesised here). Portable; MSVC and GCC.
//
//   media_avion_wake_tests [voice_wake.bin] [voice_sentences.bin]
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/AvionWakeGate.hpp"
#include "media/ai/FormantSynthesizer.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ai/VoiceGate.hpp"
#include "media/ai/VoiceLexicon.hpp"
#include "media/ai/VoiceTemplates.hpp"
#include "media/audio/MicroVad.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace media;
namespace lexicon = ai::voice_lexicon;
namespace {
int failures = 0;
std::uint64_t allocations = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr unsigned kRate = 16000;
constexpr std::uint32_t kWindowMs = 4000;
// Regression bounds: set a little below what this path measured with the shipped templates (docs/voice.md).
// Measured 2026-10-15 (MSVC Release): Mark's name alone 6 of 7, name + command 30 of 40; Julie 7 of 7 and 10 of 10;
// synthetic 36 of 36 and 40 of 40; without the name required, synthetic Hebrew 40 of 40 and Julie's Hebrew commands
// 24 of 25; no false wake (אווירון woke Julie once and every synthetic speaker on 2026-10-14, before it was a filler).
constexpr unsigned kMarkWakeAlone = 5, kMarkInline = 27, kJulieWakeAlone = 6, kJulieInline = 9, kSynthWakeAlone = 34, kSynthInline = 38;
constexpr unsigned kSynthPlainRight = 38, kJuliePlainRight = 22;
constexpr unsigned kJulieFalseWakes = 0, kSynthFalseWakes = 0;
// Chapter and scene commands said alone (2026-10-16): regression bounds a little below what was measured (see the test).
constexpr unsigned kChapterMarkRight = 9, kChapterSynthRight = 4; // Measured: 11 of 20, 5 of 16 (Julie 0 of 4); 0 wrong.
constexpr unsigned kWindowSynthRight = 12; // The same Hebrew commands after the name alone, in its window: measured 14 of 16.
struct Rng { std::uint32_t state = 1; float Next() noexcept { state = state * 1664525u + 1013904223u; return static_cast<float>(state >> 8) / 8388608.0f - 1.0f; } };
audio::Utterance At(std::uint64_t onset, std::uint64_t end) { audio::Utterance u; u.onset_frame = onset; u.begin_frame = onset; u.end_frame = end; return u; }

// ---- The gate alone: names and windows ----
void Names() {
    std::string_view rest;
    CHECK(ai::AvionWakeGate::StripWake("Avion, \xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a 10 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94", rest));
    CHECK(rest == "\xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a 10 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94");
    CHECK(ai::AvionWakeGate::StripWake("avion please pause the movie", rest) && rest == "please pause the movie");
    CHECK(ai::AvionWakeGate::StripWake("AVION... pause", rest) && rest == "pause");
    CHECK(ai::AvionWakeGate::StripWake("  avion", rest) && rest.empty());
    CHECK(ai::AvionWakeGate::StripWake("\xd7\x90\xd7\x91\xd7\x99\xd7\x95\xd7\x9f \xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7", rest) && rest == "\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7");
    CHECK(!ai::AvionWakeGate::StripWake("avionics please", rest) && rest == "avionics please");
    CHECK(!ai::AvionWakeGate::StripWake("avions", rest));
    CHECK(!ai::AvionWakeGate::StripWake("pause the movie avion", rest));
    CHECK(!ai::AvionWakeGate::StripWake("avio", rest) && !ai::AvionWakeGate::StripWake("", rest));
    // What the grammar makes of the directive's sentences once the name is stripped.
    const ai::MediaBounds film{24ll * 3600 * ai::LocalIntentDispatcher::kSecond, 3600 * ai::LocalIntentDispatcher::kSecond, 0.5f, false, false, true, true, 0, 0, 1, 1};
    CHECK(ai::AvionWakeGate::StripWake("Avion, \xd7\x90\xd7\xa9\xd7\x9e\xd7\x97 \xd7\xa9\xd7\xaa\xd7\x9e\xd7\xa9\xd7\x99\xd7\x9a 10 \xd7\xa9\xd7\xa0\xd7\x99\xd7\x95\xd7\xaa \xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94", rest));
    const auto forward = ai::LocalIntentDispatcher::Parse(rest, film);
    CHECK(forward.kind == ai::IntentKind::SeekRelative && forward.target_100ns == 3610 * ai::LocalIntentDispatcher::kSecond);
    CHECK(ai::AvionWakeGate::StripWake("Avion, please pause the movie", rest) && ai::LocalIntentDispatcher::Parse(rest, film).kind == ai::IntentKind::Pause);
}
struct Collected {
    struct Item { ai::WakeDecision decision; std::string command; std::uint64_t onset; };
    Item items[64]; unsigned count = 0;
    static void Handle(void* context, ai::WakeDecision decision, std::string_view command, const audio::Utterance& u) noexcept {
        auto& c = *static_cast<Collected*>(context);
        if (c.count < 64) c.items[c.count++] = {decision, std::string(command), u.onset_frame};
    }
};
void Windows() {
    const std::uint64_t window = kRate * kWindowMs / 1000;
    ai::AvionWakeGate gate; Collected got; gate.Configure({kRate, kWindowMs, true}, &Collected::Handle, &got);
    std::string_view command;
    // Room talk: ignored, nothing handed on.
    CHECK(gate.Decide("pause", At(1000, 9000), command) == ai::WakeDecision::Ignored && command.empty());
    // One breath: the command after the name.
    CHECK(gate.Decide("avion pause", At(20000, 40000), command) == ai::WakeDecision::Command && command == "pause");
    CHECK(!gate.Listening());
    // The name alone opens a window from the end of that utterance; a command starting inside it runs and closes it.
    CHECK(gate.Decide("avion", At(50000, 58000), command) == ai::WakeDecision::Listening && command.empty());
    CHECK(gate.Listening() && gate.WindowEnd() == 58000 + window && gate.OpenAt(58000 + window) && !gate.OpenAt(58001 + window));
    CHECK(gate.Decide("mute", At(58000 + window, 58000 + window + 9000), command) == ai::WakeDecision::WindowCommand && command == "mute");
    CHECK(!gate.Listening());
    CHECK(gate.Decide("unmute", At(200000, 209000), command) == ai::WakeDecision::Ignored); // Closed after one command.
    // A command one frame after the window: ignored, and counted as expired.
    CHECK(gate.Decide("avion", At(300000, 306000), command) == ai::WakeDecision::Listening);
    CHECK(gate.Decide("play", At(306001 + window, 320000 + window), command) == ai::WakeDecision::Ignored && gate.Stats().expired == 1);
    // The name again inside a window starts a new one.
    CHECK(gate.Decide("avion", At(700000, 705000), command) == ai::WakeDecision::Listening);
    CHECK(gate.Decide("avion", At(705000 + window - 10, 712000 + window), command) == ai::WakeDecision::Listening && gate.WindowEnd() == 712000 + 2 * window);
    CHECK(gate.Decide("avion skip forward 10 seconds", At(720000 + window, 740000 + window), command) == ai::WakeDecision::Command && command == "skip forward 10 seconds");
    CHECK(!gate.Listening());
    const auto& s = gate.Stats();
    CHECK(s.utterances == 10 && s.commands == 2 && s.wakes == 4 && s.window_commands == 1 && s.ignored == 3 && s.expired == 1);
    // As VoiceGate's handler: Ignored is never handed on.
    ai::AvionWakeGate::Handle(&gate, "pause", At(900000, 905000));
    ai::AvionWakeGate::Handle(&gate, "avion pause", At(950000, 960000));
    CHECK(got.count == 1 && got.items[0].decision == ai::WakeDecision::Command && got.items[0].command == "pause");
    // Not required: every text runs, the name stripped.
    ai::AvionWakeGate open; open.Configure({kRate, kWindowMs, false}, nullptr, nullptr);
    CHECK(open.Decide("pause", At(0, 100), command) == ai::WakeDecision::Command && command == "pause");
    CHECK(open.Decide("avion mute", At(0, 100), command) == ai::WakeDecision::Command && command == "mute");
    CHECK(std::string_view(ai::Name(ai::WakeDecision::WindowCommand)) == "WindowCommand");
}

// ---- The whole path ----
struct Acted {
    struct Item { std::uint64_t onset = 0; ai::WakeDecision decision{}; char text[96]{}; ai::IntentKind kind = ai::IntentKind::None; ai::IntentStatus status = ai::IntentStatus::Empty;
                  ai::AcousticResult heard{}; ai::SpottedWord words[8]{}; unsigned word_count = 0; }; // What the recogniser made of it.
    const ai::AcousticIntentRecognizer* recogniser = nullptr;
    Item items[1024]{}; unsigned count = 0;
    ai::LocalIntentDispatcher dispatcher; ai::MediaBounds bounds;
    static void Handle(void* context, ai::WakeDecision decision, std::string_view text, const audio::Utterance& u) noexcept {
        auto& a = *static_cast<Acted*>(context);
        if (a.count >= 1024) return;
        auto& item = a.items[a.count++]; item.onset = u.onset_frame; item.decision = decision;
        if (a.recogniser) {
            item.heard = a.recogniser->Last(); item.heard.text = nullptr;
            const auto words = a.recogniser->LastDecoding();
            item.word_count = 0; for (const auto& w : words) if (item.word_count < 8) item.words[item.word_count++] = w;
        }
        const auto length = (std::min)(text.size(), sizeof(item.text) - 1); if (length) std::memcpy(item.text, text.data(), length); item.text[length] = '\0';
        if (decision != ai::WakeDecision::Listening) { const auto intent = a.dispatcher.Dispatch(text, a.bounds); item.kind = intent.kind; item.status = intent.status; }
    }
    static ai::IntentStatus Execute(void*, const ai::Intent&) noexcept { return ai::IntentStatus::Ok; }
    Acted() { dispatcher.Configure(&Acted::Execute, nullptr); bounds.duration_100ns = 6000000000ll; bounds.position_100ns = 1200000000ll; bounds.seekable = true;
        bounds.chapters = bounds.scenes = 3; } // A film with chapters and scenes: their spoken commands can run.
};
struct Placed { std::uint64_t begin, end; };
std::vector<float> Recording(const std::vector<std::vector<float>>& utterances, const std::vector<double>& gaps, std::vector<Placed>& placed, std::uint32_t seed) {
    std::vector<float> out(static_cast<std::size_t>(kRate), 0.0f);
    for (std::size_t i = 0; i < utterances.size(); ++i) {
        placed.push_back({out.size(), out.size() + utterances[i].size()});
        out.insert(out.end(), utterances[i].begin(), utterances[i].end());
        out.insert(out.end(), static_cast<std::size_t>(gaps[i < gaps.size() ? i : gaps.size() - 1] * kRate), 0.0f);
    }
    Rng rng; rng.state = seed;
    for (auto& s : out) s += 0.0005f * rng.Next(); // About -70 dBFS: the detector needs a floor.
    return out;
}
struct Chain {
    core::LinearMemoryArena arena; audio::MicroVad vad; ai::AcousticIntentRecognizer recogniser; ai::VoiceGate gate; ai::AvionWakeGate wake; Acted acted;
    bool Setup(bool required, std::uint32_t window_frames = ai::AcousticIntentRecognizer::kWindowSentenceFrames) {
        acted.recogniser = &recogniser;
        if (!arena.Initialize(64u << 20) || !vad.Configure(arena) || !recogniser.Configure(arena, ai::voice_templates::kSet)) return false;
        recogniser.FollowWake(&wake, window_frames);
        wake.Configure({vad.Options().sample_rate, kWindowMs, required}, &Acted::Handle, &acted);
        gate.Configure(vad, &ai::AcousticIntentRecognizer::Hear, &recogniser, &ai::AvionWakeGate::Handle, &wake, 100);
        return arena.Seal();
    }
    double Run(const std::vector<float>& samples) {
        allocation_probe::Begin();
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t at = 0; at < samples.size(); at += 160) (void)gate.Process({samples.data() + at, (std::min)(samples.size() - at, std::size_t{160})});
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        allocations += allocation_probe::End();
        return seconds;
    }
    // What was done for an utterance placed at [begin, end): nullptr when nothing (ignored, or nothing recognised).
    const Acted::Item* For(const Placed& p) const {
        for (unsigned i = 0; i < acted.count; ++i) if (acted.items[i].onset + 1600 >= p.begin && acted.items[i].onset < p.end) return &acted.items[i];
        return nullptr;
    }
};
const ai::MediaBounds kFilm{6000000000ll, 1200000000ll, 0.5f, false, false, true, true, 0, 0, 1, 1};
bool Same(const char* heard, std::string_view means) {
    const auto got = ai::LocalIntentDispatcher::Parse(heard, kFilm), want = ai::LocalIntentDispatcher::Parse(means, kFilm);
    return got.status == ai::IntentStatus::Ok && got.kind == want.kind && got.target_100ns == want.target_100ns && got.volume == want.volume;
}
struct Item { std::string label, means; std::vector<float> samples; };
bool Load(const char* path, std::vector<Item>& items) {
    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) f = nullptr;
#else
    f = std::fopen(path, "rb");
#endif
    if (!f) return false;
    char magic[4]{}; std::uint32_t count = 0; bool good = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "MVS1", 4) == 0 && std::fread(&count, 4, 1, f) == 1;
    for (std::uint32_t i = 0; good && i < count; ++i) {
        Item item; std::uint32_t label = 0, means = 0, samples = 0;
        good = std::fread(&label, 4, 1, f) == 1 && label < 1024; item.label.resize(label); good = good && std::fread(item.label.data(), 1, label, f) == label;
        good = good && std::fread(&means, 4, 1, f) == 1 && means < 512; item.means.resize(means); good = good && std::fread(item.means.data(), 1, means, f) == means;
        good = good && std::fread(&samples, 4, 1, f) == 1 && samples < 9u * kRate;
        if (!good) break;
        std::vector<std::int16_t> pcm(samples); good = std::fread(pcm.data(), 2, samples, f) == samples;
        for (auto v : pcm) item.samples.push_back(static_cast<float>(v) / 32768.0f);
        items.push_back(std::move(item));
    }
    std::fclose(f);
    return good && !items.empty();
}
// The expectation an item's `means` carries (tools/make_voice_templates.cpp): the name alone, the name and a
// command, a command without the name, or nothing.
struct Expect { bool wake = false; std::string command; };
Expect Expectation(const std::string& means) {
    std::string_view rest; Expect e;
    e.wake = ai::AvionWakeGate::StripWake(means, rest); e.command = std::string(rest);
    return e;
}
// Tallies of one group of utterances through the path.
struct Tally {
    unsigned items = 0, wake_alone = 0, woke_alone = 0, inline_said = 0, inline_right = 0, inline_wrong = 0, inline_listened = 0;
    unsigned unnamed = 0, unnamed_acted = 0, chatter = 0, chatter_acted = 0, chatter_woke = 0; // chatter_woke: a listening window only.
    void Print(const char* name) const {
        std::printf("  %-44s name alone %u of %u woke; name + command %u of %u right, %u wrong, %u only woke; without the name %u of %u acted on; chatter %u of %u acted on, %u woke\n",
                    name, woke_alone, wake_alone, inline_right, inline_said, inline_wrong, inline_listened, unnamed_acted, unnamed, chatter_acted, chatter, chatter_woke);
    }
};
void Judge(const Chain& chain, const std::vector<Item>& items, const std::vector<Placed>& placed, Tally& t, bool verbose) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto* acted = chain.For(placed[i]);
        const auto e = Expectation(items[i].means);
        ++t.items;
        if (e.wake && e.command.empty()) {
            ++t.wake_alone;
            if (acted && acted->decision == ai::WakeDecision::Listening) ++t.woke_alone;
            else if (verbose) std::printf("    %-64s name alone: %s\n", items[i].label.c_str(), acted ? ai::Name(acted->decision) : "nothing");
        } else if (e.wake) {
            ++t.inline_said;
            if (acted && acted->decision == ai::WakeDecision::Command && Same(acted->text, e.command)) ++t.inline_right;
            else if (acted && acted->decision == ai::WakeDecision::Command && acted->status == ai::IntentStatus::Ok) { ++t.inline_wrong; std::printf("    %-64s WRONG: \"%s\"\n", items[i].label.c_str(), acted->text); }
            else {
                if (acted && acted->decision == ai::WakeDecision::Listening) ++t.inline_listened;
                if (verbose) std::printf("    %-64s missed: %s \"%s\"\n", items[i].label.c_str(), acted ? ai::Name(acted->decision) : "nothing", acted ? acted->text : "");
            }
        } else if (!e.command.empty()) {
            ++t.unnamed;
            if (acted) { ++t.unnamed_acted; std::printf("    %-64s ACTED WITHOUT THE NAME: %s \"%s\"\n", items[i].label.c_str(), ai::Name(acted->decision), acted->text); }
        } else {
            ++t.chatter;
            if (acted && acted->decision == ai::WakeDecision::Listening) { ++t.chatter_woke; std::printf("    %-64s chatter woke the player (listening only)\n", items[i].label.c_str()); }
            else if (acted) { ++t.chatter_acted; std::printf("    %-64s CHATTER ACTED ON: %s \"%s\"\n", items[i].label.c_str(), ai::Name(acted->decision), acted->text); }
        }
    }
}
// The same utterances with the name not required: what the recogniser itself makes of commands (the grammar's reading
// against what was meant).
void Raw(const std::vector<Item>& items, double gap, unsigned& said, unsigned& right, unsigned& wrong, bool verbose) {
    std::vector<std::vector<float>> utterances; for (const auto& item : items) utterances.push_back(item.samples);
    std::vector<Placed> placed; const auto recording = Recording(utterances, {gap}, placed, 77);
    Chain chain; CHECK(chain.Setup(false)); (void)chain.Run(recording);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto e = Expectation(items[i].means);
        // Commands only: a sentence word said alone ("אני") is no command, and nothing must run for it.
        if (e.command.empty() || ai::LocalIntentDispatcher::Parse(e.command, kFilm).status != ai::IntentStatus::Ok) continue;
        ++said;
        const auto* acted = chain.For(placed[i]);
        if (acted && Same(acted->text, e.command)) ++right;
        else if (acted && acted->status == ai::IntentStatus::Ok) { ++wrong; std::printf("    %-64s raw WRONG: \"%s\"\n", items[i].label.c_str(), acted->text); }
        else if (verbose) std::printf("    %-64s raw missed%s%s\n", items[i].label.c_str(), acted ? ": " : "", acted ? acted->text : "");
    }
}
// Held-out synthetic speakers (made here): the Hebrew test sentences alone and after the name (Israeli
// pronunciation), the name alone in its three pronunciations, and room chatter.
std::vector<Item> Synthesised() {
    std::vector<Item> items;
    static ai::FormantSynthesizer synth;
    std::vector<float> buffer(kRate * 12);
    char phonemes[1024];
    const auto say = [&](const std::string& text, unsigned alt, const lexicon::Speaker& speaker, float rate, std::uint32_t seed, const std::string& means) {
        if (!lexicon::SentencePhonemes(text, alt, phonemes, sizeof(phonemes))) { std::printf("FAIL phonemes for %s\n", text.c_str()); ++failures; return; }
        auto voice = speaker.voice; voice.rate = rate; voice.seed += seed;
        const auto n = synth.Speak(phonemes, voice, buffer);
        if (!n) { std::printf("FAIL synthesis of %s\n", text.c_str()); ++failures; return; }
        char label[400]; std::snprintf(label, sizeof(label), "%s | %s rate %.2f", text.c_str(), speaker.name, static_cast<double>(rate));
        items.push_back({label, means, std::vector<float>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n))});
    };
    unsigned seed = 5000;
    // The name alone at every held-out rate; sentences and chatter at the plain rate (the template tool measures the
    // others: the test must stay within a CTest budget in Debug too).
    for (const auto& speaker : lexicon::kHeldOutSpeakers)
        for (const float rate : lexicon::kHeldOutRates) {
            for (unsigned alt = 0; alt < 3; ++alt) say("avion", alt, speaker, rate, ++seed, "avion");
            if (rate != 1.0f) continue;
            for (const auto& s : lexicon::kTestSentences) {
                say(s.text, 2, speaker, rate, ++seed, s.means);
                say(std::string("avion ") + s.text, 2, speaker, rate, ++seed, std::string("avion ") + s.means);
            }
            for (const auto* s : lexicon::kChatter) say(s, 2, speaker, rate, ++seed, "");
        }
    return items;
}
// Measured bounds (see docs/voice.md): a little below what this path measured when the templates were generated.
struct Bounds {
    unsigned wake_alone, inline_right; // Lower bounds.
    unsigned inline_wrong, unnamed_acted, chatter_acted; // Upper bounds.
};
void Corpus(const char* wake_path, const char* sentences_path) {
    std::vector<Item> recorded, sentences;
    if (!Load(wake_path, recorded)) { std::printf("FAIL cannot read the wake corpus %s\n", wake_path); ++failures; return; }
    if (!Load(sentences_path, sentences)) { std::printf("FAIL cannot read the sentence corpus %s\n", sentences_path); ++failures; return; }
    const auto synthetic = Synthesised();
    std::printf("Corpora: %zu recorded (Mark, Julie), %zu of Mark's sentences without the name, %zu synthesised (held-out synthetic speakers)\n",
                recorded.size(), sentences.size(), synthetic.size());
    // Utterances 4.5 s apart: more than the window, so each is decided on its own.
    const auto run = [&](const std::vector<Item>& items, Tally& t, std::uint32_t seed, double* seconds) {
        std::vector<std::vector<float>> utterances; for (const auto& item : items) utterances.push_back(item.samples);
        std::vector<Placed> placed; const auto recording = Recording(utterances, {4.5}, placed, seed);
        Chain chain; CHECK(chain.Setup(true));
        const double s = chain.Run(recording); if (seconds) *seconds = s;
        Judge(chain, items, placed, t, true);
        return chain.recogniser.Stats();
    };
    // Recorded: Mark's name alone and with English commands, his room talk that begins like the name; Julie's name
    // alone, her Hebrew (French spellings) with and without the name, Hebrew words alone, Hebrew chatter.
    std::vector<Item> english, hebrew_french;
    for (const auto& item : recorded) (item.label.find("| Mark") != std::string::npos ? english : hebrew_french).push_back(item);
    Tally mark{}, julie{}, unnamed{}, synth{};
    double seconds = 0;
    std::puts("Mark (English):");
    (void)run(english, mark, 11, &seconds);
    mark.Print("Mark");
    std::puts("Julie (Hebrew in French spellings):");
    (void)run(hebrew_french, julie, 13, nullptr);
    julie.Print("Julie");
    std::puts("Mark's sentences without the name (voice_sentences.bin):");
    const auto stats = run(sentences, unnamed, 17, nullptr);
    unnamed.Print("Mark, no name");
    std::puts("Held-out synthetic speakers (Hebrew):");
    double synth_seconds = 0;
    (void)run(synthetic, synth, 19, &synth_seconds);
    synth.Print("synthetic");
    std::printf("  recogniser on Mark's sentences: %u utterances, %u decoded as sentences, %u woke\n", stats.utterances, stats.sentences, stats.wakes);
    // What the recogniser alone makes of the Hebrew commands (the name not required).
    unsigned said = 0, right = 0, wrong = 0;
    std::vector<Item> plain_hebrew; for (const auto& item : synthetic) if (!item.means.empty() && item.means.rfind("avion", 0) != 0) plain_hebrew.push_back(item);
    Raw(plain_hebrew, 1.2, said, right, wrong, false);
    std::printf("  synthetic Hebrew sentences, name not required: %u of %u right, %u wrong\n", right, said, wrong);
    unsigned fsaid = 0, fright = 0, fwrong = 0;
    std::vector<Item> french_plain; for (const auto& item : hebrew_french) if (!item.means.empty() && item.means.rfind("avion", 0) != 0) french_plain.push_back(item);
    Raw(french_plain, 1.2, fsaid, fright, fwrong, true);
    std::printf("  Julie's Hebrew (words and sentences), name not required: %u of %u right, %u wrong\n", fright, fsaid, fwrong);
    double english_seconds = 0, synth_audio = 0;
    for (const auto& item : english) english_seconds += static_cast<double>(item.samples.size()) / kRate;
    for (const auto& item : synthetic) synth_audio += static_cast<double>(item.samples.size()) / kRate;
    std::printf("  Mark's %zu utterances (%.1f s of speech, 4.5 s apart) through the path in %.2f s; %zu synthesised (%.1f s) in %.2f s\n",
                english.size(), english_seconds, seconds, synthetic.size(), synth_audio, synth_seconds);
    // Room talk never runs a command, and nothing said without the name runs; nor does it wake the player. אווירון
    // ("aviron", aeroplane: the name with one more consonant, in the chatter) did, every time, until the templates had it
    // as a filler (docs/voice.md, "Wake word"): the held-out speakers' renditions of it are in no template.
    CHECK(mark.chatter_acted == 0 && julie.chatter_acted == 0 && synth.chatter_acted == 0);
    CHECK(mark.chatter_woke == 0 && julie.chatter_woke <= kJulieFalseWakes && synth.chatter_woke <= kSynthFalseWakes);
    CHECK(mark.unnamed_acted == 0 && julie.unnamed_acted == 0 && unnamed.unnamed_acted == 0 && unnamed.chatter_acted == 0 && synth.unnamed_acted == 0);
    CHECK(unnamed.unnamed == 120 && unnamed.chatter == 35); // 100 sentences, and 20 of chapters and scenes since 2026-10-16.
    // No command is ever taken for another, with or without the name.
    CHECK(mark.inline_wrong == 0 && julie.inline_wrong == 0 && synth.inline_wrong == 0 && wrong == 0 && fwrong == 0);
    // Regression bounds (measured: see docs/voice.md).
    const Bounds mark_bounds{kMarkWakeAlone, kMarkInline, 0, 0, 0}, julie_bounds{kJulieWakeAlone, kJulieInline, 0, 0, 0}, synth_bounds{kSynthWakeAlone, kSynthInline, 0, 0, 0};
    CHECK(mark.woke_alone >= mark_bounds.wake_alone && mark.inline_right >= mark_bounds.inline_right);
    CHECK(julie.woke_alone >= julie_bounds.wake_alone && julie.inline_right >= julie_bounds.inline_right);
    CHECK(synth.woke_alone >= synth_bounds.wake_alone && synth.inline_right >= synth_bounds.inline_right);
    CHECK(right >= kSynthPlainRight && fright >= kJuliePlainRight);
    // Chapters and scenes (docs/chapters.md): the spoken commands alone, name not required, by the held-out voices:
    // Mark's English ("next chapter", "previous chapter please", "next scene please", "previous scene"), the synthetic
    // speakers' and Julie's Hebrew (פרק הבא, פרק קודם, סצנה הבאה, סצנה קודמת). Never another command.
    const auto chapter_or_scene = [](const std::string& means) {
        return means.find("chapter") != std::string::npos || means.find("scene") != std::string::npos ||
               means.find("\xd7\xa4\xd7\xa8\xd7\xa7") != std::string::npos || means.find("\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94") != std::string::npos;
    };
    std::vector<Item> spoken_en, spoken_he, spoken_fr;
    for (const auto& item : sentences) if (chapter_or_scene(item.means)) spoken_en.push_back(item);
    for (const auto& item : plain_hebrew) if (chapter_or_scene(item.means)) spoken_he.push_back(item);
    for (const auto& item : french_plain) if (chapter_or_scene(item.means)) spoken_fr.push_back(item);
    unsigned esaid = 0, eright = 0, ewrong = 0, hsaid = 0, hright = 0, hwrong = 0, csaid = 0, cright = 0, cwrong = 0;
    Raw(spoken_en, 1.2, esaid, eright, ewrong, true);
    Raw(spoken_he, 1.2, hsaid, hright, hwrong, true);
    Raw(spoken_fr, 1.2, csaid, cright, cwrong, true);
    std::printf("  chapter and scene commands, name not required: Mark %u of %u right, %u wrong; synthetic Hebrew %u of %u, %u wrong; Julie's Hebrew %u of %u, %u wrong\n",
                eright, esaid, ewrong, hright, hsaid, hwrong, cright, csaid, cwrong);
    CHECK(esaid == 20 && hsaid == 16 && csaid == 4 && ewrong == 0 && hwrong == 0 && cwrong == 0);
    CHECK(eright >= kChapterMarkRight && hright >= kChapterSynthRight);
    // In two steps (docs/voice.md, "Two steps"): the speaker's own name alone, 1 s of silence, then the same chapter and scene
    // commands and the Hebrew chatter, inside the window: with its 70-frame floor and, for comparison, without (the lanes'
    // own floors). A command must never become another, and chatter heard in the window must not act.
    const auto speaker = [](const std::string& label) { const auto bar = label.find(" | "); const auto rate = label.find(" rate", bar);
                                                       return bar == std::string::npos ? std::string{} : label.substr(bar + 3, rate == std::string::npos ? std::string::npos : rate - bar - 3); };
    const auto name_of = [&](const std::vector<Item>& pool, const std::string& who) -> const Item* {
        for (const auto& item : pool) if (item.means == "avion" && speaker(item.label).rfind(who.substr(0, who.find(' ')), 0) == 0) return &item;
        return nullptr;
    };
    std::vector<Item> chatter_he; for (const auto& item : synthetic) if (item.means.empty()) chatter_he.push_back(item);
    struct Window { unsigned said = 0, woke = 0, right = 0, wrong = 0, chatter = 0, chatter_acted = 0, window = 0, refused = 0; };
    const auto windowed = [&](const std::vector<Item>& commands, const std::vector<Item>& names_pool, std::uint32_t floor, bool chatter, bool rules) {
        Window w; std::vector<std::vector<float>> parts; std::vector<double> gaps; std::vector<const Item*> said;
        for (const auto& item : commands) {
            const auto* name = name_of(names_pool, speaker(item.label));
            if (!name) { std::printf("    no name alone for %s\n", item.label.c_str()); continue; }
            parts.push_back(name->samples); gaps.push_back(1.0); parts.push_back(item.samples); gaps.push_back(4.5); said.push_back(&item);
        }
        std::vector<Placed> placed; const auto recording = Recording(parts, gaps, placed, 29);
        Chain chain; CHECK(chain.Setup(true, floor)); chain.recogniser.SetWindowRules(rules); (void)chain.Run(recording);
        for (std::size_t i = 0; i < said.size(); ++i) {
            const auto* heard_name = chain.For(placed[2 * i]); const auto* acted = chain.For(placed[2 * i + 1]);
            const auto describe = [&](const Acted::Item& x) {
                std::printf(" {");
                for (unsigned k = 0; k < x.word_count; ++k) {
                    const char* text = chain.recogniser.TemplateText(x.words[k].template_index);
                    std::printf(" [%u-%u %s %.2f]", x.words[k].begin, x.words[k].end, text[0] ? text : "<f>", static_cast<double>(x.words[k].cost));
                }
                std::printf(" }");
                std::printf(" [%s, %u frames, %u command words]\n", x.heard.sentence ? "sentence" : "word", x.heard.frames, x.heard.words);
            };
            if (chatter) { ++w.chatter; if (acted && acted->status == ai::IntentStatus::Ok) { ++w.chatter_acted; std::printf("    %-64s window CHATTER ACTED: \"%s\"", said[i]->label.c_str(), acted->text); describe(*acted); } continue; }
            const auto e = Expectation(said[i]->means);
            if (e.command.empty() || ai::LocalIntentDispatcher::Parse(e.command, kFilm).status != ai::IntentStatus::Ok) continue; // A word alone.
            ++w.said; if (heard_name && heard_name->decision == ai::WakeDecision::Listening) ++w.woke;
            if (acted && acted->decision == ai::WakeDecision::WindowCommand && Same(acted->text, e.command)) ++w.right;
            else if (acted && acted->status == ai::IntentStatus::Ok) { ++w.wrong; std::printf("    %-64s window WRONG: \"%s\"\n", said[i]->label.c_str(), acted->text); }
        }
        w.window = chain.recogniser.Stats().window; w.refused = chain.recogniser.Stats().window_refused;
        return w;
    };
    std::vector<Item> names_all = recorded; names_all.insert(names_all.end(), synthetic.begin(), synthetic.end());
    // Three configurations: the lanes' floors and no window rules (before 2026-10-19), the window's floor alone, and the
    // window's floor and rules (shipped).
    struct Config { std::uint32_t floor; bool rules; const char* name; };
    unsigned chatter_before = 0;
    for (const Config& config : {Config{0u, false, "lanes' floors, no window rules"}, Config{ai::AcousticIntentRecognizer::kWindowSentenceFrames, false, "70-frame floor, no window rules"},
                                 Config{ai::AcousticIntentRecognizer::kWindowSentenceFrames, true, "70-frame floor and window rules"}}) {
        const auto en = windowed(spoken_en, names_all, config.floor, false, config.rules), he = windowed(spoken_he, names_all, config.floor, false, config.rules);
        const auto fr = windowed(spoken_fr, names_all, config.floor, false, config.rules);
        const auto talk = windowed(chatter_he, names_all, config.floor, true, config.rules);
        std::printf("  two steps (name, 1 s, command), %s: Mark %u of %u right, %u wrong; synthetic Hebrew %u of %u, %u wrong; Julie %u of %u, %u wrong; "
                    "names heard %u of %u; Hebrew chatter acted on %u of %u; let through by the floor %u, refused by the rules %u (chatter %u)\n",
                    config.name, en.right, en.said, en.wrong, he.right, he.said, he.wrong, fr.right, fr.said, fr.wrong, en.woke + he.woke + fr.woke,
                    en.said + he.said + fr.said, talk.chatter_acted, talk.chatter, en.window + he.window + fr.window + talk.window,
                    en.refused + he.refused + fr.refused + talk.refused, talk.refused);
        CHECK(en.wrong == 0 && he.wrong == 0 && fr.wrong == 0);
        if (!config.floor) chatter_before = talk.chatter_acted;
        // The floor adds no chatter; the rules take it to at most one in forty without losing the short Hebrew commands.
        if (config.floor && !config.rules) CHECK(he.right >= kWindowSynthRight && talk.window == 0 && talk.chatter_acted <= chatter_before);
        if (config.rules) CHECK(he.right >= kWindowSynthRight && talk.chatter_acted <= 1 && en.right >= kChapterMarkRight);
    }
}
// Two steps: the name alone, a pause, then the command (inside the window, after it, and a second command).
void TwoSteps(const char* wake_path, const char* sentences_path) {
    std::vector<Item> recorded, sentences;
    if (!Load(wake_path, recorded) || !Load(sentences_path, sentences)) return; // Reported by Corpus.
    const Item* name = nullptr; const Item* pause = nullptr; const Item* play = nullptr;
    for (const auto& item : recorded) if (!name && item.means == "avion" && item.label.find("Mark") != std::string::npos && item.label.find("rate 0 pitch 0") != std::string::npos) name = &item;
    for (const auto& item : sentences) {
        if (!pause && item.means == "pause" && item.label.rfind("can you please pause the movie | Mark rate 0", 0) == 0) pause = &item;
        if (!play && item.means == "play" && item.label.rfind("could you play the movie | Mark rate 0", 0) == 0) play = &item;
    }
    CHECK(name && pause && play);
    if (!name || !pause || !play) return;
    // Synthetic: H2 says the name, then תשתיק (mute).
    static ai::FormantSynthesizer synth;
    std::vector<float> buffer(kRate * 6); char phonemes[256];
    const auto say = [&](const char* text, unsigned alt) {
        CHECK(lexicon::SentencePhonemes(text, alt, phonemes, sizeof(phonemes)) != 0);
        auto voice = lexicon::kHeldOutSpeakers[1].voice; voice.seed += 99;
        const auto n = synth.Speak(phonemes, voice, buffer);
        return std::vector<float>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    };
    const auto avion = say("avion", 2), mute = say("\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7", 0);
    struct Case { const char* what; std::vector<std::vector<float>> parts; std::vector<double> gaps; std::vector<ai::WakeDecision> want; const char* command; };
    const Case cases[]{
        {"name, 0.8 s, \"can you please pause the movie\"", {name->samples, pause->samples}, {0.8, 2.0}, {ai::WakeDecision::Listening, ai::WakeDecision::WindowCommand}, "pause"},
        {"name, 2.5 s, \"can you please pause the movie\"", {name->samples, pause->samples}, {2.5, 2.0}, {ai::WakeDecision::Listening, ai::WakeDecision::WindowCommand}, "pause"},
        {"name, 5 s, \"can you please pause the movie\" (too late)", {name->samples, pause->samples}, {5.0, 2.0}, {ai::WakeDecision::Listening}, nullptr},
        {"name, 0.8 s, pause, 0.8 s, play (one command per window)", {name->samples, pause->samples, play->samples}, {0.8, 0.8, 2.0}, {ai::WakeDecision::Listening, ai::WakeDecision::WindowCommand}, "pause"},
        {"synthetic: name (Israeli), 1 s, \xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7", {avion, mute}, {1.0, 2.0}, {ai::WakeDecision::Listening, ai::WakeDecision::WindowCommand}, "\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7"},
    };
    for (const auto& c : cases) {
        std::vector<Placed> placed; const auto recording = Recording(c.parts, c.gaps, placed, 23);
        Chain chain; CHECK(chain.Setup(true)); (void)chain.Run(recording);
        bool good = chain.acted.count == c.want.size();
        for (unsigned i = 0; good && i < chain.acted.count; ++i) good = chain.acted.items[i].decision == c.want[i];
        if (good && c.command) good = Same(chain.acted.items[chain.acted.count - 1].text, c.command);
        std::printf("  two steps, %-58s %u decisions:", c.what, chain.acted.count);
        for (unsigned i = 0; i < chain.acted.count; ++i) std::printf(" %s%s%s%s", ai::Name(chain.acted.items[i].decision), chain.acted.items[i].text[0] ? " \"" : "", chain.acted.items[i].text, chain.acted.items[i].text[0] ? "\"" : "");
        std::printf("%s\n", good ? "" : "  UNEXPECTED");
        CHECK(good);
    }
}
}
int main(int argc, char** argv) {
    std::puts("media_avion_wake_tests");
    Names();
    Windows();
    const char* wake = argc > 1 ? argv[1] : "tests/data/voice_wake.bin";
    const char* sentences = argc > 2 ? argv[2] : "tests/data/voice_sentences.bin";
    Corpus(wake, sentences);
    TwoSteps(wake, sentences);
    CHECK(allocations == 0);
    std::printf("Avion wake word: %s (%d failures, %llu allocations while streaming)\n", failures ? "FAIL" : "PASS", failures, static_cast<unsigned long long>(allocations));
    return failures ? 1 : 0;
}
