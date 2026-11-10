// Custom wake words (docs/voice.md, "Custom wake words"): ai::SpellPhonemes and ai::CustomWakeWord on their own, then the
// whole voice path with a synthesised name in place of "Avion",
// MicroVad -> VoiceGate -> AcousticIntentRecognizer (shipped templates + the name's) -> AvionWakeGate -> LocalIntentDispatcher,
// on voices that made none of the name's templates: every Windows voice of the reference machine
// (tests/data/voice_custom_wake.bin, tools/make_voice_templates.cpp custom-wake) and the formant synthesiser's held-out speakers.
// Portable; MSVC and GCC.
//
//   media_custom_wake_tests [voice_custom_wake.bin]
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/AvionWakeGate.hpp"
#include "media/ai/CustomWakeWord.hpp"
#include "media/ai/FormantSynthesizer.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ai/VoiceGate.hpp"
#include "media/ai/VoiceLexicon.hpp"
#include "media/ai/VoiceTemplates.hpp"
#include "media/audio/MicroVad.hpp"
#if defined(_WIN32)
#include "media/platform/windows/SapiSpeech.hpp"
#endif
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace media;
namespace lexicon = ai::voice_lexicon;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 60) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)
constexpr unsigned kRate = 16000;
constexpr std::uint32_t kWindowMs = 4000;
struct Rng { std::uint32_t state = 1; float Next() noexcept { state = state * 1664525u + 1013904223u; return static_cast<float>(state >> 8) / 8388608.0f - 1.0f; } };

// ---- Spelling to sound ----
std::string Spell(const char* word, unsigned variant = 0) {
    char out[256]; const auto n = ai::SpellPhonemes(word, variant, out, sizeof(out));
    return n ? std::string(out, n) : std::string();
}
bool AllKnown(const std::string& phonemes) {
    std::string_view rest{phonemes};
    while (!rest.empty()) {
        const auto space = rest.find(' ');
        auto token = rest.substr(0, space);
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        if (!token.empty() && token[0] == '\'') token.remove_prefix(1);
        if (token != "|" && !ai::FormantSynthesizer::Known(token)) return false;
    }
    return !phonemes.empty();
}
void Spelling() {
    struct Expected { const char* word; const char* reading0; const char* reading1; };
    const Expected expected[]{
        {"Falcon", "f 'ae l k @ n", "f 'a l k a n"},
        {"Jarvis", "d Z 'a r v I s", "d Z 'a r v I s"},
        {"MedView", "m 'E d v j u", "m 'E d v j u"},
        {"Orion", "'o r a I @ n", "'o r a I a n"},
        {"Mike", "m 'a I k", "m 'a I k"},
        {"Nova", "n 'o v @", "n 'o v @"},
        {"Hey Jarvis", "h 'e I | d Z 'a r v I s", "h 'e I | d Z 'a r v I s"},
        {"\xd7\x90\xd7\x91\xd7\x99\xd7\x95\xd7\x9f", "a v i 'o n", "a v i 'o n"},          // אביון
        {"\xd7\x93\xd7\x9f", "d a 'n", nullptr},                                             // דן (checked below)
    };
    for (const auto& e : expected) {
        const auto r0 = Spell(e.word, 0), r1 = Spell(e.word, 1);
        std::printf("  %-12s -> \"%s\" / \"%s\"\n", e.word, r0.c_str(), r1.c_str());
        CHECK(AllKnown(r0) && AllKnown(r1));
        if (e.reading1) CHECK(r0 == e.reading0 && r1 == e.reading1);
    }
    CHECK(Spell("\xd7\x93\xd7\x9f") == "d 'a n");                                        // דן: the vowel nobody wrote.
    CHECK(Spell("\xd7\x99\xd7\x95\xd7\xa0\xd7\xaa\xd7\x9f") == "j o n a t 'a n");        // יונתן
    CHECK(Spell("  FALCON ") == Spell("falcon"));                                         // Case and spaces.
    // Unreadable: digits, other scripts, punctuation, nothing, a vowel-less word.
    for (const char* w : {"R2D2", "\xe6\x97\xa5\xe6\x9c\xac", "hal!", "", "   ", "brr", "falcon\xd7\x90"}) CHECK(Spell(w).empty() && !ai::CustomWakeWord::Readable(w));
    CHECK(ai::CustomWakeWord::Readable("Falcon") && ai::CustomWakeWord::Readable("Hey Jarvis") && ai::CustomWakeWord::Readable("\xd7\x93\xd7\x9f"));
    CHECK(!ai::CustomWakeWord::Readable("a name that is much longer than thirty-one bytes"));
}

// ---- The templates ----
void Templates() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8u << 20));
    for (const char* name : {"Falcon", "Jarvis", "Orion", "MedView", "\xd7\x93\xd7\xa0\xd7\x94"}) {
        ai::CustomWakeWord word;
        const auto before = arena.Statistics().used_bytes;
        allocation_probe::Begin();
        const auto begin = std::chrono::steady_clock::now();
        const bool made = word.Synthesize(arena, name, ai::voice_templates::kSet.settings.scale);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        const auto heap = allocation_probe::End();
        unsigned shortest = 9999, longest = 0;
        for (const auto& t : word.Templates()) { shortest = (std::min)(shortest, static_cast<unsigned>(t.frames)); longest = (std::max)(longest, static_cast<unsigned>(t.frames)); }
        std::printf("  %-10s %u templates, %u-%u frames, %.1f KB of arena, %.1f ms, %llu heap allocations\n", word.Text(), static_cast<unsigned>(word.Templates().size()),
                    shortest, longest, static_cast<double>(arena.Statistics().used_bytes - before) / 1024.0, ms, static_cast<unsigned long long>(heap));
        CHECK(made && heap == 0 && word.Templates().size() >= ai::CustomWakeWord::kSpeakers * ai::CustomWakeWord::kRates);
        for (const auto& t : word.Templates()) CHECK(std::string_view(t.text) == word.Text() && !t.running && t.languages == ai::kAllLanguages);
    }
    // The text is the name as the wake gate compares it.
    ai::CustomWakeWord w; CHECK(w.Synthesize(arena, "  FalCon ", 1.0f) && std::string_view(w.Text()) == "falcon");
    CHECK(!w.Synthesize(arena, "R2D2", 1.0f) && w.Templates().empty());
}

// ---- The whole path ----
struct Acted {
    struct Item { std::uint64_t onset = 0; ai::WakeDecision decision{}; char text[96]{}; ai::IntentStatus status = ai::IntentStatus::Empty; };
    std::vector<Item> items;
    ai::LocalIntentDispatcher dispatcher; ai::MediaBounds bounds;
    static void Handle(void* context, ai::WakeDecision decision, std::string_view text, const audio::Utterance& u) noexcept {
        auto& a = *static_cast<Acted*>(context);
        if (a.items.size() == a.items.capacity()) return; // Reserved before running: no allocation in the path.
        Item item; item.onset = u.onset_frame; item.decision = decision;
        const auto length = (std::min)(text.size(), sizeof(item.text) - 1); if (length) std::memcpy(item.text, text.data(), length); item.text[length] = '\0';
        if (decision != ai::WakeDecision::Listening) item.status = a.dispatcher.Dispatch(text, a.bounds).status;
        a.items.push_back(item);
    }
    static ai::IntentStatus Execute(void*, const ai::Intent&) noexcept { return ai::IntentStatus::Ok; }
    Acted() { items.reserve(2048); dispatcher.Configure(&Acted::Execute, nullptr); bounds.duration_100ns = 6000000000ll; bounds.position_100ns = 1200000000ll;
              bounds.seekable = true; bounds.chapters = bounds.scenes = 3; }
};
struct Placed { std::uint64_t begin, end; };
struct Item { std::string label, means; std::vector<float> samples; bool recorded = false; };
std::vector<float> Recording(const std::vector<Item>& items, std::vector<Placed>& placed, std::uint32_t seed) {
    // 4.5 s between utterances: more than the listening window, so each is decided on its own.
    std::vector<float> out(static_cast<std::size_t>(kRate), 0.0f);
    for (const auto& item : items) {
        placed.push_back({out.size(), out.size() + item.samples.size()});
        out.insert(out.end(), item.samples.begin(), item.samples.end());
        out.insert(out.end(), static_cast<std::size_t>(4.5 * kRate), 0.0f);
    }
    Rng rng; rng.state = seed;
    for (auto& s : out) s += 0.0005f * rng.Next(); // About -70 dBFS: the detector needs a floor.
    return out;
}
std::uint64_t allocations = 0;
struct Chain {
    core::LinearMemoryArena arena; audio::MicroVad vad; ai::AcousticIntentRecognizer recogniser; ai::VoiceGate gate; ai::AvionWakeGate wake; Acted acted;
    ai::CustomWakeWord custom; double seconds = 0;
    // `name` null: the built-in "Avion".
    bool Setup(const char* name, const ai::SpeechSource* source) {
        if (!arena.Initialize(64u << 20) || !vad.Configure(arena)) return false;
        if (name) {
            if (!custom.Synthesize(arena, name, ai::voice_templates::kSet.settings.scale, source) ||
                !recogniser.Configure(arena, ai::voice_templates::kSet, custom.Templates())) return false;
        } else if (!recogniser.Configure(arena, ai::voice_templates::kSet)) return false;
        recogniser.FollowWake(&wake);
        wake.Configure({vad.Options().sample_rate, kWindowMs, true}, &Acted::Handle, &acted);
        if (name && !wake.SetWord(custom.Text())) return false;
        gate.Configure(vad, &ai::AcousticIntentRecognizer::Hear, &recogniser, &ai::AvionWakeGate::Handle, &wake, 100);
        return arena.Seal();
    }
    void Run(const std::vector<float>& samples) {
        allocation_probe::Begin();
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t at = 0; at < samples.size(); at += 160) (void)gate.Process({samples.data() + at, (std::min)(samples.size() - at, std::size_t{160})});
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        allocations += allocation_probe::End();
    }
    const Acted::Item* For(const Placed& p) const {
        for (const auto& a : acted.items) if (a.onset + 1600 >= p.begin && a.onset < p.end) return &a;
        return nullptr;
    }
};
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
        Item item; item.recorded = true; std::uint32_t label = 0, means = 0, samples = 0;
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
// Held-out synthetic speakers (none of them made a template): each name alone and before a Hebrew command (the
// vocabulary the synthesiser speaks), in both readings, "avion" in its three pronunciations, and the Hebrew room chatter.
std::vector<Item> Synthesised() {
    std::vector<Item> items;
    auto synth = std::make_unique<ai::FormantSynthesizer>();
    std::vector<float> buffer(kRate * 12);
    unsigned seed = 9000;
    const auto say = [&](const std::string& phonemes, const char* text, const lexicon::Speaker& speaker, float rate, const std::string& means) {
        auto voice = speaker.voice; voice.rate = rate; voice.seed += ++seed;
        const auto n = synth->Speak(phonemes, voice, buffer);
        if (!n) { std::printf("FAIL synthesis of %s\n", phonemes.c_str()); ++failures; return; }
        char label[400]; std::snprintf(label, sizeof(label), "%s | %s rate %.2f", text, speaker.name, static_cast<double>(rate));
        items.push_back({label, means, std::vector<float>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n)), false});
    };
    const char* names[]{"falcon", "jarvis", "orion"};
    const char* const commands[]{"\xd7\xa0\xd7\x92\xd7\x9f", "\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8", "\xd7\xaa\xd7\xa9\xd7\xaa\xd7\x99\xd7\xa7"}; // נגן עצור תשתיק
    char pronunciation[128];
    for (const auto& speaker : lexicon::kHeldOutSpeakers)
        for (const float rate : lexicon::kHeldOutRates) {
            for (const char* name : names)
                for (unsigned v = 0; v < 2; ++v) {
                    const auto reading = Spell(name, v);
                    say(reading, name, speaker, rate, name);
                    if (rate != 1.0f) continue;
                    for (const char* c : commands) {
                        const auto* w = lexicon::Find(c);
                        if (!w || !lexicon::Pronunciation(*w, 0, pronunciation, sizeof(pronunciation))) { ++failures; continue; }
                        say(reading + " | " + pronunciation, (std::string(name) + " " + c).c_str(), speaker, rate, std::string(name) + " " + c);
                    }
                }
            for (unsigned alt = 0; alt < 3; ++alt) {
                const auto* w = lexicon::Find(lexicon::kWakeText);
                if (w && lexicon::Pronunciation(*w, alt, pronunciation, sizeof(pronunciation))) say(pronunciation, "avion", speaker, rate, "avion");
            }
            if (rate != 1.0f) continue;
            char phonemes[1024];
            for (const auto* s : lexicon::kChatter) if (lexicon::SentencePhonemes(s, 0, phonemes, sizeof(phonemes))) say(phonemes, s, speaker, rate, "");
        }
    return items;
}
// What a configured name must make of an item: `means` is what was said ("falcon pause", "avion", "" for room talk).
struct Tally {
    unsigned alone = 0, alone_woke = 0, named = 0, named_right = 0, named_wrong = 0, named_listened = 0;
    unsigned avion = 0, avion_woke = 0, others = 0, others_woke = 0, chatter = 0, chatter_woke = 0, unnamed = 0, unnamed_acted = 0;
    unsigned talk = 0, talk_acted = 0; // Room talk that begins with the name ("a falcon is a bird of prey"): waking is right, a command is not.
    void Print(const char* what) const {
        std::printf("    %-30s name alone %u/%u woke; name + command %u/%u right (%u wrong, %u only woke); \"avion\" woke %u/%u; other names %u/%u; "
                    "room talk %u/%u; talk beginning with the name acted %u/%u; a command without the name acted %u/%u\n", what, alone_woke, alone, named_right,
                    named, named_wrong, named_listened, avion_woke, avion, others_woke, others, chatter_woke, chatter, talk_acted, talk, unnamed_acted, unnamed);
    }
    [[nodiscard]] unsigned False() const noexcept { return named_wrong + avion_woke + others_woke + chatter_woke + unnamed_acted; }
};
// Who said an item: synthetic held-out speakers, the two Windows voices no template is ever made from (Mark, Julie), or the
// other four (David, Zira, Hortense, Paul: the voices a recorded or system source renders the name with).
enum class Group : std::uint8_t { Synthetic, HeldOutVoices, SourceVoices };
Group GroupOf(const Item& item) {
    if (!item.recorded) return Group::Synthetic;
    return item.label.find("| Mark ") != std::string::npos || item.label.find("| Julie ") != std::string::npos ? Group::HeldOutVoices : Group::SourceVoices;
}
void Judge(const Chain& chain, const std::vector<Item>& items, const std::vector<Placed>& placed, Group group, Tally& t) {
    const std::string name = chain.custom.Text();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (GroupOf(items[i]) != group) continue;
        const auto& means = items[i].means;
        const auto* a = chain.For(placed[i]);
        const bool acted = a != nullptr;
        const std::string first = means.substr(0, means.find(' '));
        const std::string rest = means.size() > first.size() ? means.substr(first.size() + 1) : std::string();
        if (first == name) {
            if (rest.empty()) { ++t.alone; if (acted && a->decision == ai::WakeDecision::Listening) ++t.alone_woke; continue; }
            ++t.named;
            const auto want = ai::LocalIntentDispatcher::Parse(rest, chain.acted.bounds);
            const auto got = acted && a->decision == ai::WakeDecision::Command ? ai::LocalIntentDispatcher::Parse(a->text, chain.acted.bounds) : ai::Intent{};
            if (got.status == ai::IntentStatus::Ok && got.kind == want.kind && got.target_100ns == want.target_100ns) ++t.named_right;
            else if (got.status == ai::IntentStatus::Ok) { ++t.named_wrong; std::printf("      %-56s WRONG: \"%s\"\n", items[i].label.c_str(), a->text); }
            else if (acted && a->decision == ai::WakeDecision::Listening) ++t.named_listened;
            continue;
        }
        std::string spoken = items[i].label.substr(0, items[i].label.find(" |"));
        for (auto& c : spoken) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (means.empty() && spoken.rfind("a " + name + " ", 0) == 0) {
            ++t.talk;
            if (acted && a->decision != ai::WakeDecision::Listening) { ++t.talk_acted; std::printf("      %-56s TALK WITH THE NAME ACTED ON: \"%s\"\n", items[i].label.c_str(), a->text); }
            continue;
        }
        // Not this name: nothing may happen (the utterances are further apart than the window).
        unsigned* said = first == "avion" ? &t.avion : (first == "falcon" || first == "jarvis" || first == "orion" || first == "medview") ? &t.others :
                         means.empty() ? &t.chatter : &t.unnamed;
        unsigned* woke = said == &t.avion ? &t.avion_woke : said == &t.others ? &t.others_woke : said == &t.chatter ? &t.chatter_woke : &t.unnamed_acted;
        ++*said;
        if (acted) { ++*woke; std::printf("      %-56s %s: %s \"%s\"\n", items[i].label.c_str(), means.empty() ? "ROOM TALK WOKE" : "NOT THE NAME, WOKE", ai::Name(a->decision), a->text); }
    }
}
// A host's text-to-speech, as the SDK's callback would give it: the corpus's renditions of the name by the source voices
// (David, Zira, Hortense, Paul), at their three rates. Mark and Julie are never a source.
struct RecordedVoices {
    const std::vector<Item>* items = nullptr;
    static constexpr const char* kVoices[]{"David", "Zira", "Hortense", "Paul"};
    static std::size_t Speak(void* context, const char* text, unsigned voice, int rate, float* out, std::size_t capacity) noexcept {
        const auto& self = *static_cast<const RecordedVoices*>(context);
        if (voice >= std::size(kVoices)) return 0;
        std::string folded(text); for (auto& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        char label_end[64]; std::snprintf(label_end, sizeof(label_end), "| %s rate %d", kVoices[voice], 3 * rate);
        for (const auto& item : *self.items)
            if (item.recorded && item.means == folded && item.label.size() >= std::strlen(label_end) &&
                item.label.compare(item.label.size() - std::strlen(label_end), std::string::npos, label_end) == 0) {
                const auto n = (std::min)(capacity, item.samples.size());
                std::copy(item.samples.begin(), item.samples.begin() + static_cast<std::ptrdiff_t>(n), out);
                return n;
            }
        return 0;
    }
};
struct Config { const char* what; const ai::SpeechSource* source; };
void Path(const char* corpus_path) {
    std::vector<Item> items;
    const bool corpus = Load(corpus_path, items);
    if (!corpus) { std::printf("FAIL cannot read the custom wake corpus %s\n", corpus_path); ++failures; return; }
    const auto recorded = items.size();
    auto synthetic = Synthesised();
    for (auto& s : synthetic) items.push_back(std::move(s));
    std::printf("Utterances: %zu recorded (6 Windows voices), %zu synthesised (held-out synthetic speakers)\n", recorded, items.size() - recorded);
    std::vector<Placed> placed;
    const auto recording = Recording(items, placed, 314);
    RecordedVoices voices{&items};
    const ai::SpeechSource recorded_source{&RecordedVoices::Speak, &voices, static_cast<unsigned>(std::size(RecordedVoices::kVoices))};
    std::vector<Config> configs{{"formant only", nullptr}, {"formant + 4 recorded voices", &recorded_source}};
#if defined(_WIN32)
    platform::windows::SapiSpeech sapi;
    const wchar_t* const held_out[]{L"Mark", L"Julie"};
    ai::SpeechSource system{};
    if (sapi.Open(held_out, 2)) {
        system = sapi.Source();
        std::printf("System voices (Mark and Julie left out):"); for (unsigned i = 0; i < sapi.Voices(); ++i) std::printf(" %ls", sapi.VoiceName(i)); std::printf("\n");
        configs.push_back({"formant + system voices", &system});
    } else std::printf("No system voice: the live SAPI source is not measured\n");
#endif
    const char* names[]{"Falcon", "Jarvis", "Orion"};
    for (const auto& config : configs) {
        std::printf("Templates: %s\n", config.what);
        for (const char* name : names) {
            // Formant renditions alone: measured for one name (the finding is the same for each; it keeps Debug and ASan runs short).
            if (!config.source && std::strcmp(name, "Falcon") != 0) continue;
            // The live system voices are the recorded ones rendered again: one name shows they behave the same.
            if (config.source != &recorded_source && config.source && std::strcmp(name, "Falcon") != 0) continue;
            auto chain = std::make_unique<Chain>();
            const auto begin = std::chrono::steady_clock::now();
            CHECK(chain->Setup(name, config.source));
            const double setup = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            chain->Run(recording);
            std::printf("  \"%s\": %u templates (%u from the source), made in %.0f ms; %.1f s for %.0f s of audio\n", name, static_cast<unsigned>(chain->custom.Templates().size()),
                        chain->custom.SourceTemplates(), setup, chain->seconds, static_cast<double>(recording.size()) / kRate);
            Tally synth, held, sources;
            Judge(*chain, items, placed, Group::Synthetic, synth); synth.Print("synthetic held-out speakers");
            Judge(*chain, items, placed, Group::HeldOutVoices, held); held.Print("Mark and Julie (held out)");
            Judge(*chain, items, placed, Group::SourceVoices, sources); sources.Print(config.source ? "David, Zira, Hortense, Paul *" : "David, Zira, Hortense, Paul");
            // Never anything wrong, never a false wake: from any voice, with any templates.
            CHECK(synth.False() == 0 && held.False() == 0 && sources.False() == 0);
            // Measured: one of the 12 (David, fastest rate, recorded source: "prey" decoded as "play"; 0 with the system voices).
            CHECK(synth.talk_acted + held.talk_acted + sources.talk_acted <= 1);
            // Regression bounds, a little below what was measured (docs/voice.md, "Custom wake words").
            CHECK(synth.alone_woke * 10 >= synth.alone * 9 && synth.named_right * 10 >= synth.named * 6);
            if (config.source) CHECK(held.alone_woke * 10 >= held.alone * 5);
        }
    }
    // The built-in name, unchanged by any of this: "avion" still wakes the default engine; the custom names do not.
    {
        auto chain = std::make_unique<Chain>(); CHECK(chain->Setup(nullptr, nullptr)); chain->Run(recording);
        unsigned avion = 0, woke = 0, names_said = 0, names_woke = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& m = items[i].means; const auto* a = chain->For(placed[i]);
            if (m == "avion") { ++avion; if (a && a->decision == ai::WakeDecision::Listening) ++woke; }
            if (m == "falcon" || m == "jarvis" || m == "orion" || m == "medview") { ++names_said; if (a) ++names_woke; }
        }
        std::printf("Built-in \"Avion\": \"avion\" alone woke %u/%u; the custom names woke %u/%u\n", woke, avion, names_woke, names_said);
        CHECK(woke * 10 >= avion * 7 && names_woke == 0);
    }
    std::printf("Allocations while listening: %llu\n", static_cast<unsigned long long>(allocations));
    CHECK(allocations == 0);
}
}

int main(int argc, char** argv) {
    std::printf("Spelling:\n"); Spelling();
    std::printf("Templates:\n"); Templates();
    Path(argc > 1 ? argv[1] : "tests/data/voice_custom_wake.bin");
    std::printf("Custom wake words: %d failures\n", failures);
    return failures ? 1 : 0;
}
