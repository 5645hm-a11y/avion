// Offline tool: builds the voice command templates (include/media/ai/VoiceTemplates.hpp) and the held-out test
// corpora (tests/data/voice_corpus.bin, voice_sentences.bin, voice_wake.bin) from Windows' own speech synthesiser
// (SAPI 5 desktop and OneCore voices) and the engine's formant synthesiser (ai::FormantSynthesizer).
// Nothing here ships in the engine: the engine gets the generated header, which holds features only.
//
//   media_make_voice_templates <repository root> [probe | evaluate | custom-wake] [isolated]
//
// custom-wake: writes only tests/data/voice_custom_wake.bin, every voice saying custom wake words (docs/voice.md, "Custom
// wake words"), alone and before a command, the built-in "avion", and room talk (some of it like the names). No voice's
// rendition of these names is in any template: the engine synthesises a custom name's templates at Prepare.
//
// - Each command word is spoken by several voices at several rates and pitches. The engine's own analysis
//   (AcousticIntentRecognizer::Describe) turns each rendition into a feature sequence.
// - Templates are the renditions of the *template voices*; the *held-out voices* never enter the header. Their
//   renditions, other words by every voice, and noisy copies go into the corpora the test suites replay.
// - The acceptance thresholds in the header are chosen from the distances measured here (template voices only), and
//   the tool prints what they give: per held-out rendition, per impostor word, per sentence.
// English words are spoken by the English voices. No Hebrew voice is installed on the reference machine: Hebrew words
// (media/ai/VoiceLexicon.hpp) are rendered by the formant synthesiser from phonemes, by four synthetic template
// speakers, and spoken by the French voices from French spellings of their sounds ("naguène", "atsor": French
// shares Hebrew's uvular r and final stress). Both are approximations of Hebrew, not Hebrew speech. The wake word
// "avion" is said by every voice (English and French) and by the synthetic speakers in three pronunciations.
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/FormantSynthesizer.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "media/ai/VoiceLexicon.hpp"
#include <Windows.h>
#include <sapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using media::ai::AcousticIntentRecognizer;
namespace lexicon = media::ai::voice_lexicon;
namespace {
struct Voice { ISpObjectToken* token = nullptr; std::wstring name, language; bool onecore = false; };
std::vector<Voice> Voices() {
    std::vector<Voice> voices;
    const wchar_t* categories[]{L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices"};
    for (unsigned c = 0; c < 2; ++c) {
        ISpObjectTokenCategory* category = nullptr; IEnumSpObjectTokens* tokens = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category)))) continue;
        if (SUCCEEDED(category->SetId(categories[c], FALSE)) && SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens))) {
            ISpObjectToken* token = nullptr;
            while (tokens->Next(1, &token, nullptr) == S_OK) {
                Voice v; v.token = token; v.onecore = c == 1;
                ISpDataKey* attributes = nullptr;
                if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes))) {
                    wchar_t* text = nullptr;
                    if (SUCCEEDED(attributes->GetStringValue(L"Name", &text)) && text) { v.name = text; CoTaskMemFree(text); }
                    if (SUCCEEDED(attributes->GetStringValue(L"Language", &text)) && text) { v.language = text; CoTaskMemFree(text); }
                    attributes->Release();
                }
                voices.push_back(v);
            }
            tokens->Release();
        }
        category->Release();
    }
    return voices;
}
// MEDIA_VOICE_CACHE=<file>: synthesised speech kept between runs (the synthesiser is deterministic; it is most of a
// run's time). Experiments only: the shipped header is always made from a fresh run.
std::map<std::wstring, std::vector<float>> g_cache; bool g_cache_dirty = false;
std::wstring CachePath() { wchar_t path[MAX_PATH]{}; return GetEnvironmentVariableW(L"MEDIA_VOICE_CACHE", path, MAX_PATH) ? path : L""; }
void LoadCache() {
    const auto path = CachePath(); FILE* f = nullptr; if (path.empty() || _wfopen_s(&f, path.c_str(), L"rb") || !f) return;
    std::uint32_t count = 0; if (std::fread(&count, 4, 1, f) != 1) count = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0, samples = 0;
        if (std::fread(&key, 4, 1, f) != 1 || key > 4096) break;
        std::wstring k(key, L'\0'); if (std::fread(k.data(), 2, key, f) != key || std::fread(&samples, 4, 1, f) != 1) break;
        std::vector<float> s(samples); if (std::fread(s.data(), 4, samples, f) != samples) break;
        g_cache[k] = std::move(s);
    }
    std::fclose(f);
}
void SaveCache() {
    const auto path = CachePath(); FILE* f = nullptr; if (path.empty() || !g_cache_dirty || _wfopen_s(&f, path.c_str(), L"wb") || !f) return;
    const auto count = static_cast<std::uint32_t>(g_cache.size()); std::fwrite(&count, 4, 1, f);
    for (const auto& [k, s] : g_cache) {
        const auto key = static_cast<std::uint32_t>(k.size()), samples = static_cast<std::uint32_t>(s.size());
        std::fwrite(&key, 4, 1, f); std::fwrite(k.data(), 2, key, f); std::fwrite(&samples, 4, 1, f); std::fwrite(s.data(), 4, samples, f);
    }
    std::fclose(f);
}
std::vector<float> Synthesise(const Voice& voice, const std::wstring& xml);
// Speaks `xml` (SAPI XML) with `voice` into 16 kHz mono float samples.
std::vector<float> Speak(const Voice& voice, const std::wstring& xml) {
    if (CachePath().empty()) return Synthesise(voice, xml);
    const auto key = voice.name + L"|" + xml;
    if (const auto found = g_cache.find(key); found != g_cache.end()) return found->second;
    auto samples = Synthesise(voice, xml); g_cache[key] = samples; g_cache_dirty = true;
    return samples;
}
std::vector<float> Synthesise(const Voice& voice, const std::wstring& xml) {
    std::vector<float> samples;
    ISpVoice* engine = nullptr; ISpStream* stream = nullptr; IStream* base = nullptr;
    WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 1; format.nSamplesPerSec = 16000; format.wBitsPerSample = 16;
    format.nBlockAlign = 2; format.nAvgBytesPerSec = 32000;
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&engine));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &base);
    if (SUCCEEDED(hr)) hr = stream->SetBaseStream(base, SPDFID_WaveFormatEx, &format);
    if (SUCCEEDED(hr)) hr = engine->SetVoice(voice.token);
    if (SUCCEEDED(hr)) hr = engine->SetOutput(stream, TRUE);
    if (SUCCEEDED(hr)) hr = engine->Speak(xml.c_str(), SPF_IS_XML, nullptr);
    if (SUCCEEDED(hr)) {
        STATSTG stat{}; HGLOBAL global = nullptr;
        if (SUCCEEDED(base->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(base, &global))) {
            const auto* pcm = static_cast<const std::int16_t*>(GlobalLock(global));
            const std::size_t count = static_cast<std::size_t>(stat.cbSize.QuadPart / 2);
            if (pcm) { samples.resize(count); for (std::size_t i = 0; i < count; ++i) samples[i] = pcm[i] / 32768.0f; GlobalUnlock(global); }
        }
    }
    if (stream) stream->Release();
    if (base) base->Release();
    if (engine) engine->Release();
    return samples;
}
std::string Narrow(const std::wstring& w) {
    std::string out(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), static_cast<int>(out.size()), nullptr, nullptr);
    while (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}
std::wstring Wide(const std::string& s) {
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), static_cast<int>(out.size()));
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}
// The formant synthesiser: `phonemes` by `voice`, 16 kHz.
std::vector<float> Formant(const std::string& phonemes, const media::ai::SynthVoice& voice) {
    static media::ai::FormantSynthesizer synth;
    std::vector<float> out(synth.Samples(phonemes, voice));
    if (out.empty() || synth.Speak(phonemes, voice, out) != out.size()) { std::printf("FAIL synthesiser: %s\n", phonemes.c_str()); out.clear(); }
    return out;
}
// ---- What is spoken ----
struct Word { const char* text; const wchar_t* spoken; bool french; };
// Commands 0-26 (voice_corpus.bin numbers them): `text` is what the grammar parses. The two Hebrew ones are spoken by
// French voices from French spellings (and by the synthetic speakers, from the lexicon). The first six are the
// single-word commands of the first set; the rest are the words a spoken sentence needs around them.
const Word kCommands[]{
    {"play", L"play", false}, {"pause", L"pause", false}, {"rewind", L"rewind", false}, {"mute", L"mute", false},
    {"\xd7\xa0\xd7\x92\xd7\x9f", L"nagu\x00e8ne", true},            // Hebrew nagen: play.
    {"\xd7\xa2\xd7\xa6\xd7\x95\xd7\xa8", L"atsor", true},           // Hebrew atsor: stop.
    {"stop", L"stop", false}, {"resume", L"resume", false}, {"back", L"back", false}, {"forward", L"forward", false},
    {"skip", L"skip", false}, {"unmute", L"unmute", false}, {"louder", L"louder", false}, {"quieter", L"quieter", false},
    {"turn", L"turn", false}, {"up", L"up", false}, {"down", L"down", false}, {"volume", L"volume", false}, {"go", L"go", false},
    {"one", L"one", false}, {"five", L"five", false}, {"ten", L"ten", false}, {"twenty", L"twenty", false}, {"thirty", L"thirty", false},
    {"minute", L"minute", false}, {"minutes", L"minutes", false}, {"seconds", L"seconds", false},
    // Chapters and scenes (docs/chapters.md): "next chapter", "previous scene".
    {"next", L"next", false}, {"previous", L"previous", false}, {"chapter", L"chapter", false}, {"scene", L"scene", false}};
constexpr int kBaseCommands = static_cast<int>(sizeof(kCommands) / sizeof(kCommands[0]));
// Every command: kCommands, then the lexicon's wake word and Hebrew words (numbers from kBaseCommands).
enum Language { kEnglish = 0, kFrench = 1, kBoth = 2, kNone = -1 };
struct Command { std::string text; std::wstring sapi; int language = kNone; const lexicon::Word* lexical = nullptr; };
std::vector<Command> g_commands; int g_wake = -1;
// Fillers: ordinary words that are no command, spoken by the template voices only; they enter the templates so that
// an utterance nearer to one of them than to any command is rejected.
const Word kFillers[]{
    {"hello", L"hello", false}, {"window", L"window", false}, {"seven", L"seven", false}, {"music", L"music", false},
    {"table", L"table", false}, {"water", L"water", false}, {"coffee", L"coffee", false}, {"morning", L"morning", false},
    {"yes", L"yes", false}, {"no", L"no", false}, {"okay", L"okay", false}, {"thanks", L"thanks", false},
    {"open", L"open", false}, {"close", L"close", false}, {"light", L"light", false}, {"phone", L"phone", false},
    {"people", L"people", false}, {"maybe", L"maybe", false}, {"number", L"number", false},
    // Ordinary words shaped like command words ("better" like "quieter", "step" like "skip").
    {"better", L"better", false}, {"letter", L"letter", false}, {"after", L"after", false}, {"step", L"step", false}, {"speak", L"speak", false},
    // The words people put around a command: they are matched (so they are not taken for a command) and dropped.
    {"please", L"please", false}, {"can", L"can", false}, {"you", L"you", false}, {"the", L"the", false}, {"movie", L"movie", false},
    {"a", L"a", false}, {"bit", L"bit", false}, {"id", L"I'd", false}, {"like", L"like", false}, {"want", L"want", false},
    {"could", L"could", false}, {"would", L"would", false}, {"video", L"video", false}, {"sound", L"sound", false}, {"it", L"it", false},
    {"now", L"now", false}, {"playback", L"playback", false}, {"me", L"me", false}, {"to", L"to", false}, {"make", L"make", false},
    {"this", L"this", false}, {"is", L"is", false},
    {"bonjour", L"bonjour", true}, {"maison", L"maison", true}, {"merci", L"merci", true}, {"oui", L"oui", true},
    {"non", L"non", true}, {"cafe", L"caf\x00e9", true}, {"voiture", L"voiture", true}, {"lumiere", L"lumi\x00e8re", true},
    {"demain", L"demain", true}, {"rouge", L"rouge", true}};
struct Filler { std::string text; std::wstring sapi; int language = kNone; const lexicon::Word* lexical = nullptr; };
std::vector<Filler> g_fillers;
void BuildVocabulary() {
    for (const auto& w : kCommands) g_commands.push_back({w.text, w.spoken, w.french ? kFrench : kEnglish, nullptr});
    for (const auto& w : lexicon::kWords) {
        if (w.role == lexicon::Role::Wake) { g_wake = static_cast<int>(g_commands.size()); g_commands.push_back({w.text, Wide(w.french), kBoth, &w}); }
        else if (w.role == lexicon::Role::Command) {
            bool known = false;
            for (auto& c : g_commands) if (c.text == w.text) { c.lexical = &w; known = true; } // נגן, עצור: their SAPI renditions stay.
            if (!known) g_commands.push_back({w.text, w.french ? Wide(w.french) : L"", w.french ? kFrench : kNone, &w});
        }
    }
    for (const auto& w : kFillers) g_fillers.push_back({w.text, w.spoken, w.french ? kFrench : kEnglish, nullptr});
    for (const auto& w : lexicon::kWords) if (w.role == lexicon::Role::Filler) g_fillers.push_back({w.text, w.french ? Wide(w.french) : L"", w.french ? kFrench : kNone, &w});
}
// Impostors: words that are no command and are in no template, spoken by every voice: what the test replays. The
// first group sounds close to a command on purpose (remind/rewind, stay and player/play, nager/naguene).
const Word kImpostors[]{
    {"remind", L"remind", false}, {"stay", L"stay", false}, {"player", L"player", false}, {"nager", L"nager", true},
    {"computer", L"computer", false}, {"rain", L"rain", false}, {"garden", L"garden", false}, {"money", L"money", false},
    {"button", L"button", false}, {"paper", L"paper", false}, {"later", L"later", false},
    {"alors", L"alors", true}, {"fenetre", L"fen\x00eatre", true}, {"chanson", L"chanson", true}, {"bateau", L"bateau", true}};
// The shipped set's representation (docs/voice.md, "Packed templates"): renditions kept per word and speaker (alone, in
// context; 0: all), bits per cepstrum, and the key frames' tolerance (0: every frame that is not exactly on a line).
// Every rendition, 8 bits, every frame: the smaller sets measured (down to 64 KB: 3 alone and 3 in context, 3 bits,
// tolerance 4.5, the best of them on the template voices' cross-validated score) made wrong commands on the held-out
// voices, which this one does not.
constexpr unsigned kKeepAlone = 0, kKeepInContext = 0, kPackBits = 8;
// The connected-word settings and beam kept from the 2026-10-15 grid (see where they are applied).
constexpr float kShippedSettings[6]{2.2f, 15.0f, 1.10f, 70.0f, 0.0f, 100.0f};
constexpr float kShippedBeam = 50.0f;
constexpr float kPackTolerance = 0.0f;
// Speakers whose voices never enter the templates.
const wchar_t* const kHeldOut[]{L"Mark", L"Julie"};
// Spoken sentences: what is said, and the command the grammar must read from what is recognised (nullptr: no command).
struct Sentence { const wchar_t* spoken; const char* means; };
const Sentence kSentences[]{
    {L"can you please pause the movie", "pause"}, {L"please pause", "pause"}, {L"pause the video please", "pause"},
    {L"stop the movie", "stop"}, {L"could you play the movie", "play"}, {L"resume playback please", "resume"},
    {L"I'd like you to skip forward thirty seconds", "skip forward 30 seconds"}, {L"skip forward ten seconds", "skip forward 10 seconds"},
    {L"go back ten seconds please", "go back 10 seconds"}, {L"rewind twenty seconds", "rewind 20 seconds"}, {L"go back one minute", "go back 1 minute"},
    {L"skip forward five minutes", "skip forward 5 minutes"}, {L"turn up the volume a bit", "turn up"}, {L"turn the volume down please", "turn down"},
    {L"louder please", "louder"}, {L"can you make it quieter", "quieter"}, {L"mute the sound", "mute"}, {L"unmute please", "unmute"},
    {L"volume up", "volume up"}, {L"rewind", "rewind"},
    {L"next chapter", "next chapter"}, {L"previous chapter please", "previous chapter"}, {L"next scene please", "next scene"},
    {L"previous scene", "previous scene"},
    {L"what time is it", nullptr}, {L"I like this movie a lot", nullptr}, {L"can you hear me", nullptr}, {L"the weather is nice today", nullptr},
    {L"where are my keys", nullptr}, {L"thank you very much", nullptr}, {L"this is a good movie", nullptr}};
// Room talk that begins like the wake word (none is a command, none wakes).
const wchar_t* const kNearWake[]{
    L"have you seen my phone", L"a million people came", L"an onion soup please", L"every one of them", L"a villain in the movie",
    L"avenue of the stars", L"I've been there before", L"a vision of the future"};
// Training sentences (embedded training): said by the template voices only, worded unlike the sentences above, every
// word in the vocabulary. Each is aligned to its words, and the words as said inside it become templates too: a word in
// running speech is shorter and shaped by its neighbours, and an isolated rendition does not look like it.
const wchar_t* const kTraining[]{
    L"please skip forward ten seconds", L"go back thirty seconds", L"rewind one minute please", L"can you stop", L"could you pause it",
    L"play it now", L"resume the movie", L"skip back twenty seconds", L"turn up the sound", L"turn down the volume a bit",
    L"make it louder", L"make it quieter please", L"mute it please", L"can you unmute the video", L"volume down",
    L"go forward five seconds", L"skip forward twenty seconds please", L"go back five minutes now", L"would you pause the video",
    L"turn the sound up please", L"volume up please", L"stop it now", L"pause it please", L"you can play the video",
    L"can you stop the playback", L"make the sound louder", L"mute the video now",
    // The wake word before a command, in one breath.
    L"avion pause it please", L"avion skip forward ten seconds", L"avion turn up the sound", L"avion can you stop", L"avion mute the video",
    L"avion go back thirty seconds", L"avion resume the movie",
    // Chapters and scenes.
    L"next chapter please", L"skip to the next chapter", L"previous chapter now", L"the next scene please", L"previous scene now",
    L"avion next chapter", L"avion previous scene"};
constexpr int kFillerWord = -2; // Rendition::command of a filler.
struct Rendition {
    int command = -1;           // Index into g_commands, -1 for an impostor word, kFillerWord for a filler.
    int filler = -1;            // Index into g_fillers for a filler.
    std::string label;          // "play | Mark OneCore rate -4 pitch 0".
    std::wstring speaker;
    bool held_out = false;
    bool in_context = false;    // Cut from a training sentence.
    bool dropped = false;       // Not kept as a template (MEDIA_VOICE_KEEP).
    std::vector<std::uint32_t> keys;   // Packed: the frames kept (the rest lie on lines between them),
    std::vector<std::uint8_t> codes;   // and their cepstra as level numbers (kCepstra per kept frame).
    bool synth = false;         // The formant synthesiser's (rate in hundredths, `alternative`: the pronunciation).
    int rate = 0, pitch = 0, alternative = 0;
    std::vector<float> samples; // Trimmed to the word plus 100 ms either side.
    std::vector<float> features; std::uint32_t frames = 0;
};
// Forced alignment: the sentence's frames as the given words in order (each matched against one rendition of it by
// the same speaker, DTW steps as in Decode), with optional pauses between them at `pause` per frame. `spans` gets each
// word's [first, last] frame. False when no alignment exists.
bool Align(const float* f, std::uint32_t frames, const std::vector<const Rendition*>& words, float pause, std::vector<std::pair<std::uint32_t, std::uint32_t>>& spans) {
    constexpr float kFar = 1e30f; constexpr auto D = AcousticIntentRecognizer::kDimensions;
    const std::size_t n = words.size();
    std::vector<std::vector<float>> cost(n), next(n); std::vector<std::vector<int>> start(n), start_next(n), before(n), before_next(n);
    for (std::size_t i = 0; i < n; ++i) {
        cost[i].assign(words[i]->frames, kFar); next[i] = cost[i];
        start[i].assign(words[i]->frames, 0); start_next[i] = start[i]; before[i] = start[i]; before_next[i] = start[i];
    }
    std::vector<float> rest(n + 1, kFar), rest_next(n + 1); std::vector<int> rest_from(n + 1, -1), rest_from_next(n + 1);
    rest[0] = 0; // A pause before the first word, from frame -1.
    std::vector<std::vector<float>> end_cost(n, std::vector<float>(frames, kFar));
    std::vector<std::vector<int>> end_start(n, std::vector<int>(frames, 0)), end_before(n, std::vector<int>(frames, -1));
    for (std::uint32_t t = 0; t < frames; ++t) {
        const float* x = f + static_cast<std::size_t>(t) * D;
        for (std::size_t i = 0; i < n; ++i) {
            const auto length = words[i]->frames; const float* y = words[i]->features.data();
            // Entering word i at t: from the pause before it, or straight from the end of word i - 1 at t - 1.
            float entry = rest[i]; int entry_before = rest_from[i];
            if (i > 0 && t > 0 && end_cost[i - 1][t - 1] < entry) { entry = end_cost[i - 1][t - 1]; entry_before = static_cast<int>(t) - 1; }
            for (std::uint32_t j = 0; j < length; ++j) {
                float c = cost[i][j]; int s = start[i][j], b = before[i][j];
                if (j >= 1 && cost[i][j - 1] < c) { c = cost[i][j - 1]; s = start[i][j - 1]; b = before[i][j - 1]; }
                if (j >= 2 && cost[i][j - 2] < c) { c = cost[i][j - 2]; s = start[i][j - 2]; b = before[i][j - 2]; }
                if (j == 0 && entry < c) { c = entry; s = static_cast<int>(t); b = entry_before; }
                if (c >= kFar) { next[i][j] = kFar; continue; }
                float sum = 0; for (unsigned d = 0; d < D; ++d) { const float e = x[d] - y[j * D + d]; sum += e * e; }
                next[i][j] = c + std::sqrt(sum); start_next[i][j] = s; before_next[i][j] = b;
            }
            const float last = next[i][length - 1]; const int span = static_cast<int>(t) - start_next[i][length - 1] + 1;
            if (last < kFar && 2 * span >= static_cast<int>(length) && span <= 2 * static_cast<int>(length)) {
                end_cost[i][t] = last; end_start[i][t] = start_next[i][length - 1]; end_before[i][t] = before_next[i][length - 1];
            }
        }
        // Pauses: before word i (after word i - 1 ended), and after the last word.
        for (std::size_t i = 0; i <= n; ++i) {
            float c = rest[i]; int from = rest_from[i];
            if (i > 0 && end_cost[i - 1][t] < kFar && (c >= kFar || end_cost[i - 1][t] < c)) { c = end_cost[i - 1][t]; from = static_cast<int>(t); }
            rest_next[i] = c < kFar ? c + pause : kFar; rest_from_next[i] = from;
        }
        std::swap(cost, next); std::swap(start, start_next); std::swap(before, before_next); std::swap(rest, rest_next); std::swap(rest_from, rest_from_next);
    }
    // The last word ends at the last frame, or before a final pause.
    int end = -1;
    if (end_cost[n - 1][frames - 1] < kFar && (rest[n] >= kFar || end_cost[n - 1][frames - 1] <= rest[n])) end = static_cast<int>(frames) - 1;
    else if (rest[n] < kFar) end = rest_from[n];
    if (end < 0) return false;
    spans.assign(n, {0, 0});
    for (std::size_t i = n; i-- > 0;) {
        if (end < 0 || end_cost[i][static_cast<std::size_t>(end)] >= kFar) return false;
        spans[i] = {static_cast<std::uint32_t>(end_start[i][static_cast<std::size_t>(end)]), static_cast<std::uint32_t>(end)};
        end = end_before[i][static_cast<std::size_t>(end)];
    }
    return true;
}
std::wstring Speaker(const Voice& v) { // "Microsoft David Desktop" -> "David".
    const auto first = v.name.find(L' '); if (first == std::wstring::npos) return v.name;
    const auto second = v.name.find(L' ', first + 1);
    return v.name.substr(first + 1, second == std::wstring::npos ? std::wstring::npos : second - first - 1);
}
bool IsFrench(const Voice& v) { return v.language.find(L"40C") != std::wstring::npos || v.language.find(L"40c") != std::wstring::npos; }
// The word with 100 ms of silence either side (the synthesiser pads unevenly).
std::vector<float> Trim(const std::vector<float>& samples) {
    float peak = 0; for (float s : samples) peak = (std::max)(peak, std::fabs(s));
    std::size_t first = 0, last = samples.size();
    while (first < samples.size() && std::fabs(samples[first]) < 0.02f * peak) ++first;
    while (last > first && std::fabs(samples[last - 1]) < 0.02f * peak) --last;
    std::vector<float> out(1600, 0.0f);
    out.insert(out.end(), samples.begin() + static_cast<std::ptrdiff_t>(first), samples.begin() + static_cast<std::ptrdiff_t>(last));
    out.insert(out.end(), 1600, 0.0f);
    return out;
}
bool Analyse(Rendition& r) {
    static std::vector<float> scratch(AcousticIntentRecognizer::kMaxFrames * media::audio::FilterbankFeatures::kChannels);
    r.features.assign(AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions, 0.0f);
    r.frames = AcousticIntentRecognizer::Describe(r.samples, scratch.data(), r.features.data());
    r.features.resize(static_cast<std::size_t>(r.frames) * AcousticIntentRecognizer::kDimensions);
    return r.frames != 0;
}
float Distance(const Rendition& a, const Rendition& b) {
    static float rows[2 * (AcousticIntentRecognizer::kMaxFrames + 1)];
    return AcousticIntentRecognizer::Distance(a.features.data(), a.frames, b.features.data(), b.frames, rows, rows + AcousticIntentRecognizer::kMaxFrames + 1);
}
std::vector<float> Noisy(const std::vector<float>& samples, double snr_db, std::uint32_t seed) {
    double power = 0; std::size_t active = 0;
    for (float s : samples) if (std::fabs(s) > 1e-4f) { power += static_cast<double>(s) * s; ++active; }
    const double rms = std::sqrt(power / static_cast<double>(active ? active : 1)) * std::pow(10.0, -snr_db / 20.0);
    auto out = samples; std::uint32_t state = seed;
    for (auto& s : out) { state = state * 1664525u + 1013904223u; s += static_cast<float>(rms * 1.7320508 * (static_cast<double>(state >> 8) / 8388608.0 - 1.0)); }
    return out;
}
// The `count` levels (sorted) that best represent `values` (one-dimensional k-means from the quantiles). Deterministic.
std::vector<float> Levels(std::vector<float> values, unsigned count) {
    std::vector<float> levels(count, 0.0f);
    if (values.empty()) return levels;
    std::sort(values.begin(), values.end());
    for (unsigned l = 0; l < count; ++l) levels[l] = values[static_cast<std::size_t>((l + 0.5) / count * static_cast<double>(values.size()))];
    for (unsigned iteration = 0; iteration < 40; ++iteration) {
        std::vector<double> sums(count, 0.0); std::vector<std::size_t> counts(count, 0);
        unsigned at = 0;
        for (const float v : values) { // Sorted: the nearest level only moves up.
            while (at + 1 < count && std::fabs(levels[at + 1] - v) <= std::fabs(levels[at] - v)) ++at;
            sums[at] += v; ++counts[at];
        }
        for (unsigned l = 0; l < count; ++l) if (counts[l]) levels[l] = static_cast<float>(sums[l] / static_cast<double>(counts[l]));
        std::sort(levels.begin(), levels.end());
    }
    return levels;
}
unsigned NearestLevel(float v, const std::vector<float>& levels) {
    unsigned at = 0; for (unsigned l = 1; l < levels.size(); ++l) if (std::fabs(levels[l] - v) < std::fabs(levels[at] - v)) at = l;
    return at;
}
void Put32(FILE* f, std::uint32_t v) { std::fwrite(&v, 4, 1, f); }
void PutSamples(FILE* f, const std::vector<float>& samples) {
    Put32(f, static_cast<std::uint32_t>(samples.size()));
    for (float x : samples) { const auto v = static_cast<std::int16_t>(std::lround((std::clamp)(x, -1.0f, 1.0f) * 32767.0f)); std::fwrite(&v, 2, 1, f); }
}
// "avion" at the start of a recognised text: the wake word, and what follows it.
bool Woke(const std::string& text, std::string& rest) {
    const std::string wake = lexicon::kWakeText;
    if (text.compare(0, wake.size(), wake) != 0 || (text.size() > wake.size() && text[wake.size()] != ' ')) { rest = text; return false; }
    rest = text.size() > wake.size() ? text.substr(wake.size() + 1) : std::string{};
    return true;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::puts("Usage: media_make_voice_templates <repository root> [probe | evaluate | custom-wake] [isolated]"); return 2; }
    const bool probe = argc > 2 && std::wcscmp(argv[2], L"probe") == 0;
    const bool evaluate = argc > 2 && std::wcscmp(argv[2], L"evaluate") == 0;
    const bool embedded = !(argc > 3 && std::wcscmp(argv[3], L"isolated") == 0); // "isolated": no in-context templates (comparison).
    // MEDIA_VOICE_HEBREW=synth|french|both: which renditions make the Hebrew words' templates (comparison; default both).
    int hebrew_sources = 3; { wchar_t value[16]{}; if (GetEnvironmentVariableW(L"MEDIA_VOICE_HEBREW", value, 16)) hebrew_sources = std::wcscmp(value, L"synth") == 0 ? 1 : std::wcscmp(value, L"french") == 0 ? 2 : 3; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    LoadCache();
    BuildVocabulary();
    const int command_count = static_cast<int>(g_commands.size());
    auto voices = Voices();
    // One voice per speaker: the desktop and OneCore versions of the same speaker render identical samples here.
    for (std::size_t i = 0; i < voices.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (Speaker(voices[j]) == Speaker(voices[i])) { voices[i].token->Release(); voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i)); --i; break; }
    std::printf("%zu speakers:", voices.size());
    for (const auto& v : voices) std::printf(" %s (%s)", Narrow(Speaker(v)).c_str(), v.onecore ? "OneCore" : "desktop");
    std::printf("; %zu synthetic template speakers, %zu held out; %d commands (wake word %d), %zu fillers\n", std::size(lexicon::kTemplateSpeakers),
                std::size(lexicon::kHeldOutSpeakers), command_count, g_wake, g_fillers.size());
    const auto held_speaker = [](const std::wstring& speaker) { return std::any_of(std::begin(kHeldOut), std::end(kHeldOut), [&](const wchar_t* name) { return speaker == name; }); };
    if (argc > 2 && std::wcscmp(argv[2], L"custom-wake") == 0) {
        // What each voice says, and what was said (the test decides, per configured name, what must happen).
        struct Said { const wchar_t* spoken; const char* means; bool english_only; };
        const Said said[]{
            {L"Falcon", "falcon", false}, {L"Falcon pause", "falcon pause", true}, {L"Falcon mute", "falcon mute", true},
            {L"Falcon next chapter", "falcon next chapter", true}, {L"Jarvis", "jarvis", false}, {L"Jarvis pause", "jarvis pause", true},
            {L"Orion", "orion", false}, {L"Orion mute", "orion mute", true}, {L"Med View", "medview", false},
            {L"Avion", "avion", false}, {L"Avion pause", "avion pause", true},
            {L"what time is it", "", true}, {L"where are my keys", "", true}, {L"the weather is nice today", "", true},
            {L"a falcon is a bird of prey", "", true}, {L"follow the money", "", true}, {L"the fallen leaves are red", "", true},
            {L"pause", "pause", true}, {L"I saw a movie about Jarvis", "", true}};
        struct Variant { int rate, pitch; };
        const Variant variants[]{{0, 0}, {-3, 0}, {3, 0}};
        unsigned failed = 0;
        struct Out { std::string label, means; std::vector<float> samples; };
        std::vector<Out> items;
        for (const auto& v : voices) {
            const bool french = IsFrench(v);
            for (const auto& s : said) {
                if (s.english_only && french) continue;
                for (const auto& variant : variants) {
                    wchar_t xml[256];
                    std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/><pitch absmiddle=\"%d\"/>%ls", variant.rate, variant.pitch, s.spoken);
                    char label[200];
                    std::snprintf(label, sizeof(label), "%s | %s rate %d", Narrow(s.spoken).c_str(), Narrow(Speaker(v)).c_str(), variant.rate);
                    auto samples = Trim(Speak(v, xml));
                    if (samples.size() <= 3200) { std::printf("FAIL no speech: %s\n", label); ++failed; continue; }
                    items.push_back({label, s.means, std::move(samples)});
                }
            }
        }
        std::wstring root = argv[1];
        FILE* f = nullptr; _wfopen_s(&f, (root + L"\\tests\\data\\voice_custom_wake.bin").c_str(), L"wb");
        if (!f) { std::puts("FAIL cannot write voice_custom_wake.bin"); return 1; }
        std::fwrite("MVS1", 1, 4, f); Put32(f, static_cast<std::uint32_t>(items.size()));
        for (const auto& item : items) {
            Put32(f, static_cast<std::uint32_t>(item.label.size())); std::fwrite(item.label.data(), 1, item.label.size(), f);
            Put32(f, static_cast<std::uint32_t>(item.means.size())); std::fwrite(item.means.data(), 1, item.means.size(), f);
            PutSamples(f, item.samples);
        }
        std::fclose(f);
        std::printf("Custom wake corpus: %zu utterances by %zu voices, %u failed\n", items.size(), voices.size(), failed);
        SaveCache();
        for (auto& v : voices) v.token->Release();
        CoUninitialize();
        return failed ? 1 : 0;
    }
    // ---- Renditions: every command and impostor word, by every voice of its language ----
    std::vector<Rendition> renditions;
    struct Variant { int rate, pitch; };
    const Variant template_variants[]{{0, 0}, {-3, 0}, {3, 0}};
    const Variant held_variants[]{{0, 0}, {-4, 0}, {-2, 0}, {2, 0}, {4, 0}, {0, -5}, {0, 5}};
    const Variant once[]{{0, 0}};
    unsigned failed = 0;
    const auto speak = [&](const char* text, const std::wstring& spoken, int language, int command, int filler = -1) {
        if (language == kNone || spoken.empty()) return;
        for (const auto& v : voices) {
            const bool french = IsFrench(v);
            if (language != kBoth && french != (language == kFrench)) continue;
            const auto speaker = Speaker(v);
            const bool held = held_speaker(speaker);
            // Fillers only by the template voices, once; impostor words once per voice; commands in every variant of
            // the voice's role.
            if (command == kFillerWord && held) continue;
            const Variant* variants = command < 0 ? once : held ? held_variants : template_variants;
            const std::size_t count = command < 0 ? 1 : held ? std::size(held_variants) : std::size(template_variants);
            for (std::size_t i = 0; i < count; ++i) {
                wchar_t xml[256];
                std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/><pitch absmiddle=\"%d\"/>%ls", variants[i].rate, variants[i].pitch, spoken.c_str());
                Rendition r; r.command = command; r.filler = filler; r.speaker = speaker; r.held_out = held; r.rate = variants[i].rate; r.pitch = variants[i].pitch;
                char label[200];
                std::snprintf(label, sizeof(label), "%s | %s %s rate %d pitch %d", text, Narrow(speaker).c_str(), v.onecore ? "OneCore" : "desktop", variants[i].rate, variants[i].pitch);
                r.label = label;
                r.samples = Trim(Speak(v, xml));
                if (r.samples.size() <= 3200 || !Analyse(r)) { std::printf("  no speech: %s\n", label); ++failed; continue; }
                if (probe) std::printf("  %-52s %.2f s, %u frames\n", label, static_cast<double>(r.samples.size()) / 16000.0, r.frames);
                renditions.push_back(std::move(r));
            }
        }
    };
    // The formant synthesiser's renditions of a lexicon word: every template speaker, pronunciation and rate (fillers
    // at the plain rate only); the held-out speakers' (measured here, never stored) at their rates.
    const auto synthesise = [&](const lexicon::Word& word, int command, int filler) {
        for (const bool held : {false, true}) {
            if (held && command == kFillerWord) continue;
            const auto* speakers = held ? lexicon::kHeldOutSpeakers : lexicon::kTemplateSpeakers;
            const std::size_t speaker_count = held ? std::size(lexicon::kHeldOutSpeakers) : std::size(lexicon::kTemplateSpeakers);
            const float* rates = held ? lexicon::kHeldOutRates : lexicon::kTemplateRates;
            const std::size_t rate_count = command == kFillerWord ? 1 : held ? std::size(lexicon::kHeldOutRates) : std::size(lexicon::kTemplateRates);
            for (std::size_t s = 0; s < speaker_count; ++s)
                for (unsigned alt = 0; alt < lexicon::Pronunciations(word); ++alt)
                    for (std::size_t k = 0; k < rate_count; ++k) {
                        const float rate = command == kFillerWord ? 1.0f : rates[k];
                        char phonemes[256]; if (!lexicon::Pronunciation(word, alt, phonemes, sizeof(phonemes))) continue;
                        auto voice = speakers[s].voice; voice.rate = rate; voice.seed += 7919u * alt + 104729u * static_cast<unsigned>(k);
                        Rendition r; r.command = command; r.filler = filler; r.speaker = Wide(speakers[s].name); r.held_out = held; r.synth = true;
                        r.rate = static_cast<int>(std::lround(rate * 100)); r.alternative = static_cast<int>(alt);
                        char label[200]; std::snprintf(label, sizeof(label), "%s | %s rate %.2f pron %u", word.text, speakers[s].name, static_cast<double>(rate), alt);
                        r.label = label; r.samples = Trim(Formant(phonemes, voice));
                        if (r.samples.size() <= 3200 || !Analyse(r)) { std::printf("  no speech: %s\n", label); ++failed; continue; }
                        renditions.push_back(std::move(r));
                    }
        }
    };
    for (int c = 0; c < command_count; ++c) {
        const auto& command = g_commands[c];
        const bool hebrew = command.lexical && command.lexical->role == media::ai::voice_lexicon::Role::Command;
        if (!hebrew || (hebrew_sources & 2) || c < kBaseCommands) speak(command.text.c_str(), command.sapi, command.language, c);
        if (command.lexical && (!hebrew || (hebrew_sources & 1))) synthesise(*command.lexical, c, -1);
    }
    for (int f = 0; f < static_cast<int>(g_fillers.size()); ++f) {
        speak(g_fillers[f].text.c_str(), g_fillers[f].sapi, g_fillers[f].language, kFillerWord, f);
        if (g_fillers[f].lexical) synthesise(*g_fillers[f].lexical, kFillerWord, f);
    }
    for (const auto& word : kImpostors) speak(word.text, word.spoken, word.french ? kFrench : kEnglish, -1);
    std::printf("%zu renditions (%u failed)\n", renditions.size(), failed);
    if (probe) { for (auto& v : voices) v.token->Release(); CoUninitialize(); return 0; }
    // ---- Embedded training: the template voices' training sentences, aligned to their words ----
    {
        // A word of a training sentence: a command or a filler (by its text).
        const auto find = [&](const std::string& w, int& command, int& filler) {
            command = -1; filler = -1;
            for (int c = 0; c < command_count; ++c) if (w == g_commands[c].text) command = c;
            for (int f = 0; f < static_cast<int>(g_fillers.size()); ++f) if (w == g_fillers[f].text || Narrow(g_fillers[f].sapi) == w) filler = f;
            return command >= 0 || filler >= 0;
        };
        const auto split = [](const std::string& text) { std::vector<std::string> list; std::string w; for (const char c : text + " ") { if (c == ' ') { if (!w.empty()) list.push_back(w); w.clear(); } else w += c; } return list; };
        unsigned aligned = 0, words_cut = 0, unaligned = 0;
        std::vector<Rendition> context;
        // Each word's own isolated rendition by this speaker at the plain rate (pronunciation `alt` for the synthesiser):
        // what the alignment matches.
        const auto references = [&](const std::vector<std::string>& list, const std::wstring& speaker, bool synth, int alt,
                                    std::vector<const Rendition*>& refs, std::vector<std::pair<int, int>>& ids) {
            refs.clear(); ids.clear();
            for (const auto& w : list) {
                int command = -1, filler = -1;
                if (!find(w, command, filler)) return false;
                const Rendition* ref = nullptr;
                for (const auto& r : renditions)
                    if (r.speaker == speaker && r.synth == synth && r.rate == (synth ? 100 : 0) && r.pitch == 0 && !r.in_context &&
                        (!synth || command != g_wake || r.alternative == alt) &&
                        ((command >= 0 && r.command == command) || (command < 0 && r.command == kFillerWord && r.filler == filler))) { ref = &r; break; }
                if (!ref) return false;
                refs.push_back(ref); ids.push_back({command, filler});
            }
            return true;
        };
        const auto cut = [&](const Rendition& whole, const std::vector<const Rendition*>& refs, const std::vector<std::pair<int, int>>& ids,
                             const std::vector<std::string>& list, const std::string& sentence, const std::wstring& speaker, bool synth, int rate) {
            std::vector<std::pair<std::uint32_t, std::uint32_t>> spans;
            if (!Align(whole.features.data(), whole.frames, refs, 2 * 5.9f, spans)) { ++unaligned; return; }
            ++aligned;
            for (std::size_t i = 0; i < spans.size(); ++i) {
                const auto first = spans[i].first, last = spans[i].second, frames = last - first + 1;
                if (frames < AcousticIntentRecognizer::kMinTemplateFrames || frames > AcousticIntentRecognizer::kMaxTemplateFrames) continue;
                Rendition r; r.command = ids[i].first >= 0 ? ids[i].first : kFillerWord; r.filler = ids[i].second; r.speaker = speaker;
                r.in_context = true; r.synth = synth; r.rate = rate; r.frames = frames;
                r.features.assign(whole.features.begin() + static_cast<std::ptrdiff_t>(first) * AcousticIntentRecognizer::kDimensions,
                                  whole.features.begin() + static_cast<std::ptrdiff_t>(last + 1) * AcousticIntentRecognizer::kDimensions);
                char label[240]; std::snprintf(label, sizeof(label), "%s (in \"%s\") | %s rate %d", list[i].c_str(), sentence.c_str(), Narrow(speaker).c_str(), rate);
                r.label = label; context.push_back(std::move(r)); ++words_cut;
            }
        };
        std::vector<const Rendition*> refs; std::vector<std::pair<int, int>> ids;
        // English training sentences by the English template voices.
        for (const auto* sentence : kTraining) {
            const auto text = Narrow(sentence); const auto list = split(text);
            for (const auto& v : voices) {
                const auto speaker = Speaker(v);
                if (IsFrench(v) || held_speaker(speaker)) continue;
                if (!references(list, speaker, false, 0, refs, ids)) { std::printf("  training sentence has a word without a rendition: %s\n", text.c_str()); break; }
                for (const int rate : {-2, 0, 2}) {
                    wchar_t xml[512]; std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/>%ls", rate, sentence);
                    Rendition whole; whole.samples = Trim(Speak(v, xml));
                    if (!Analyse(whole)) { ++unaligned; continue; }
                    cut(whole, refs, ids, list, text, speaker, false, rate);
                }
            }
        }
        // Hebrew training sentences: by the synthetic template speakers, and by the French template voices in French spellings.
        for (const auto* sentence : lexicon::kTrainingSentences) {
            const std::string text = sentence; const auto list = split(text);
            if (hebrew_sources & 1)
                for (const auto& s : lexicon::kTemplateSpeakers) {
                    const auto speaker = Wide(s.name);
                    if (!references(list, speaker, true, 2, refs, ids)) { std::printf("  training sentence has a word without a synthetic rendition: %s\n", text.c_str()); break; }
                    char phonemes[1024]; if (!lexicon::SentencePhonemes(text, 2, phonemes, sizeof(phonemes))) { std::printf("  FAIL phonemes: %s\n", text.c_str()); continue; }
                    for (std::size_t k = 0; k < std::size(lexicon::kTemplateRates); ++k) {
                        auto voice = s.voice; voice.rate = lexicon::kTemplateRates[k]; voice.seed += 31337u * static_cast<unsigned>(k + 1);
                        Rendition whole; whole.samples = Trim(Formant(phonemes, voice));
                        if (!Analyse(whole)) { ++unaligned; continue; }
                        cut(whole, refs, ids, list, text, speaker, true, static_cast<int>(std::lround(lexicon::kTemplateRates[k] * 100)));
                    }
                }
            if (hebrew_sources & 2)
                for (const auto& v : voices) {
                    const auto speaker = Speaker(v);
                    if (!IsFrench(v) || held_speaker(speaker)) continue;
                    char french[512]; if (!lexicon::SentenceFrench(text, french, sizeof(french))) continue;
                    if (!references(list, speaker, false, 0, refs, ids)) { std::printf("  training sentence has a word without a French rendition: %s\n", text.c_str()); break; }
                    for (const int rate : {-2, 0, 2}) {
                        wchar_t xml[512]; std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/>%ls", rate, Wide(french).c_str());
                        Rendition whole; whole.samples = Trim(Speak(v, xml));
                        if (!Analyse(whole)) { ++unaligned; continue; }
                        cut(whole, refs, ids, list, text, speaker, false, rate);
                    }
                }
        }
        std::printf("Embedded training: %u sentences aligned (%u not), %u words cut as in-context templates\n", aligned, unaligned, words_cut);
        if (embedded) for (auto& r : context) renditions.push_back(std::move(r));
    }
    // ---- Templates: the commands by the template voices ----
    // Representative renditions (docs/voice.md, "Packed templates"): of each word's renditions by one template speaker
    // (one pronunciation), alone or as said in sentences, only kKeep are kept: chosen one by one, each the rendition
    // that brings the group's renditions nearest to what is kept (the sum of their distances to the nearest kept one).
    // MEDIA_VOICE_KEEP=<alone>,<in context> (experiments): other numbers; 0,0 keeps every rendition.
    unsigned keep[2]{kKeepAlone, kKeepInContext};
    {
        wchar_t value[32]{};
        if (GetEnvironmentVariableW(L"MEDIA_VOICE_KEEP", value, 32)) (void)swscanf_s(value, L"%u,%u", &keep[0], &keep[1]);
        if (keep[0] || keep[1]) {
            std::map<std::string, std::vector<Rendition*>> groups;
            for (auto& r : renditions) {
                if (!(r.in_context || r.command == kFillerWord || (r.command >= 0 && !r.held_out))) continue;
                char key[160]; std::snprintf(key, sizeof(key), "%d|%d|%d|%s|%d|%d", r.command, r.filler, r.in_context ? 1 : 0, Narrow(r.speaker).c_str(), r.synth ? 1 : 0, r.alternative);
                groups[key].push_back(&r);
            }
            std::size_t before = 0, after = 0, frames_before = 0, frames_after = 0;
            for (auto& [key, group] : groups) {
                const unsigned k = keep[group[0]->in_context ? 1 : 0];
                before += group.size(); for (const auto* r : group) frames_before += r->frames;
                if (k && group.size() > k) {
                    const std::size_t n = group.size();
                    std::vector<float> d(n * n, 0.0f);
                    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = i + 1; j < n; ++j) {
                        float v = Distance(*group[i], *group[j]); if (v < 0) v = 1e6f; d[i * n + j] = d[j * n + i] = v;
                    }
                    std::vector<float> nearest(n, 1e30f); std::vector<bool> kept(n, false);
                    for (unsigned pick = 0; pick < k; ++pick) {
                        double best = 1e300; std::size_t at = 0;
                        for (std::size_t c = 0; c < n; ++c) {
                            if (kept[c]) continue;
                            double total = 0; for (std::size_t i = 0; i < n; ++i) total += (std::min)(nearest[i], d[c * n + i]);
                            if (total < best) { best = total; at = c; }
                        }
                        kept[at] = true; for (std::size_t i = 0; i < n; ++i) nearest[i] = (std::min)(nearest[i], d[at * n + i]);
                    }
                    for (std::size_t i = 0; i < n; ++i) group[i]->dropped = !kept[i];
                }
                for (const auto* r : group) if (!r->dropped) { ++after; frames_after += r->frames; }
            }
            std::printf("Kept renditions (%u alone, %u in context per word and speaker): %zu of %zu, %zu of %zu frames\n", keep[0], keep[1], after, before, frames_after, frames_before);
        }
    }
    // Packing (docs/voice.md, "Packed templates"): each stored template as the shipped set keeps it: the cepstra of its
    // key frames, each value one of 2^kPackBits levels of its coefficient (the levels that best represent all of them),
    // the frames between on straight lines, and the deltas made again from the result. Key frames: the first, then
    // each next one as far on as the line to it stays within kPackTolerance of every frame's cepstra on the way.
    // MEDIA_VOICE_PACK=<bits>,<tolerance> (experiments): others; 0 packs nothing (then no header can be written).
    unsigned pack_bits = kPackBits; float tolerance = kPackTolerance;
    std::vector<std::vector<float>> levels;
    {
        wchar_t value[32]{};
        if (GetEnvironmentVariableW(L"MEDIA_VOICE_PACK", value, 32)) { pack_bits = 0; (void)swscanf_s(value, L"%u,%f", &pack_bits, &tolerance); }
        if (pack_bits) {
            const unsigned bits = pack_bits;
            constexpr unsigned D = AcousticIntentRecognizer::kDimensions, C = AcousticIntentRecognizer::kCepstra;
            const auto stored_side = [](const Rendition& r) { return !r.dropped && (r.in_context || r.command == kFillerWord || (r.command >= 0 && !r.held_out)); };
            levels.assign(C, {});
            for (unsigned k = 0; k < C; ++k) {
                std::vector<float> values;
                for (const auto& r : renditions) if (stored_side(r)) for (std::uint32_t t = 0; t < r.frames; ++t) values.push_back(r.features[static_cast<std::size_t>(t) * D + k]);
                levels[k] = Levels(std::move(values), 1u << bits);
            }
            double error = 0; std::size_t frames = 0, kept = 0;
            for (auto& r : renditions) {
                if (!stored_side(r)) continue;
                const auto original = r.features;
                const std::uint32_t n = r.frames;
                auto& keys = r.keys; keys.assign(1, 0);
                {
                    while (keys.back() + 1 < n) {
                        const std::uint32_t from = keys.back(); std::uint32_t to = from + 1;
                        for (std::uint32_t e = from + 2; e < n; ++e) { // The longest segment whose line fits.
                            bool fits = true;
                            for (std::uint32_t t = from + 1; t < e && fits; ++t) {
                                const float w = static_cast<float>(t - from) / static_cast<float>(e - from); float sum = 0;
                                for (unsigned k = 0; k < C; ++k) {
                                    const float line = (1 - w) * original[static_cast<std::size_t>(from) * D + k] + w * original[static_cast<std::size_t>(e) * D + k];
                                    const float d = line - original[static_cast<std::size_t>(t) * D + k]; sum += d * d;
                                }
                                fits = std::sqrt(sum) <= tolerance;
                            }
                            if (!fits) break;
                            to = e;
                        }
                        keys.push_back(to);
                    }
                }
                kept += keys.size();
                r.codes.clear();
                for (const auto t : keys) for (unsigned k = 0; k < C; ++k) {
                    float& v = r.features[static_cast<std::size_t>(t) * D + k];
                    const auto level = NearestLevel(v, levels[k]); r.codes.push_back(static_cast<std::uint8_t>(level)); v = levels[k][level];
                }
                for (std::size_t i = 0; i + 1 < keys.size(); ++i)
                    for (std::uint32_t t = keys[i] + 1; t < keys[i + 1]; ++t) {
                        const float w = static_cast<float>(t - keys[i]) / static_cast<float>(keys[i + 1] - keys[i]);
                        for (unsigned k = 0; k < C; ++k) r.features[static_cast<std::size_t>(t) * D + k] = (1 - w) * r.features[static_cast<std::size_t>(keys[i]) * D + k] + w * r.features[static_cast<std::size_t>(keys[i + 1]) * D + k];
                    }
                AcousticIntentRecognizer::Deltas(r.features.data(), n);
                for (std::size_t v = 0; v < original.size(); ++v) { const double e = original[v] - r.features[v]; error += e * e; }
                frames += n;
            }
            std::printf("Packed (%u bits per cepstrum, tolerance %.2f): %zu of %zu frames kept, %zu bytes of values and %zu of key bits; RMS error %.3f per frame\n", bits,
                        static_cast<double>(tolerance), kept, frames, (kept * C * bits + 7) / 8, (frames + 7) / 8, std::sqrt(error / static_cast<double>(frames ? frames : 1)));
        }
    }
    std::vector<const Rendition*> templates, held, impostors, fillers, contextual;
    for (const auto& r : renditions) if (!r.dropped) (r.in_context ? contextual : r.command == kFillerWord ? fillers : r.command < 0 ? impostors : r.held_out ? held : templates).push_back(&r);
    {
        std::size_t frames[3]{};
        for (const auto* r : templates) frames[0] += r->frames;
        for (const auto* r : fillers) frames[1] += r->frames;
        for (const auto* r : contextual) frames[2] += r->frames;
        std::printf("Stored frames: %zu in %zu command renditions, %zu in %zu fillers, %zu in %zu in-context words\n", frames[0], templates.size(), frames[1], fillers.size(), frames[2], contextual.size());
    }
    // Thresholds from the template voices alone (the held-out voices are for measuring, not for tuning): for each
    // template, the nearest rendition of its word by another speaker (genuine), and the nearest rendition of any
    // other word (impostor). The acceptance distance is taken over the first set's words by the SAPI voices, as it
    // always was; the wake word's over its own renditions by every template speaker.
    std::vector<float> genuine, other, wake_genuine;
    const auto first_set = [&](const Rendition* r) { return !r->synth && r->command < kBaseCommands; };
    for (const auto* a : templates) {
        float same = -1, different = -1;
        for (const auto* b : templates) {
            if (a == b || (first_set(a) && !first_set(b))) continue;
            const float d = Distance(*a, *b); if (d < 0) continue;
            if (b->command == a->command) { if (b->speaker != a->speaker && (same < 0 || d < same)) same = d; }
            else if (different < 0 || d < different) different = d;
        }
        if (same >= 0 && a->command == g_wake) wake_genuine.push_back(same);
        if (a->synth || a->command >= kBaseCommands) continue;
        if (same >= 0) genuine.push_back(same);
        if (different >= 0) other.push_back(different);
    }
    // Feature quality without any threshold: how often a template's nearest rendition by *another speaker* is the same
    // word (this is what the feature choices are judged on; the held-out voices are only measured).
    for (const bool synth : {false, true}) {
        unsigned cross_right = 0, cross_total = 0;
        for (const auto* a : templates) {
            if (a->synth != synth || (!synth && a->command >= kBaseCommands)) continue;
            float nearest = -1; int command = -2;
            for (const auto* b : templates) {
                if (b->speaker == a->speaker || b->synth != synth) continue;
                const float d = Distance(*a, *b);
                if (d >= 0 && (nearest < 0 || d < nearest)) { nearest = d; command = b->command; }
            }
            ++cross_total; if (command == a->command) ++cross_right;
        }
        std::printf("%s: nearest rendition by another speaker is the same word for %u of %u\n", synth ? "Synthetic template speakers" : "Template voices (first set)", cross_right, cross_total);
    }
    std::sort(genuine.begin(), genuine.end()); std::sort(other.begin(), other.end()); std::sort(wake_genuine.begin(), wake_genuine.end());
    const auto percentile = [](const std::vector<float>& v, double p) { return v.empty() ? 0.0f : v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))]; };
    std::printf("Template voices, cross-speaker: same word nearest distance median %.3f, 90th %.3f, max %.3f; other word nearest min %.3f, 10th %.3f, median %.3f\n",
                static_cast<double>(percentile(genuine, 0.5)), static_cast<double>(percentile(genuine, 0.9)), static_cast<double>(genuine.empty() ? 0 : genuine.back()),
                static_cast<double>(other.empty() ? 0 : other.front()), static_cast<double>(percentile(other, 0.1)), static_cast<double>(percentile(other, 0.5)));
    // Accept up to the worst cross-speaker match of a word among the template voices (a new speaker should be no
    // further from the templates than the template speakers are from each other); the best command must beat the
    // next by a tenth of its distance. Neither is tuned on the held-out voices. The wake word: up to the worst
    // cross-speaker match of its own renditions, but never beyond the commands' distance.
    const float accept = genuine.empty() ? 1.0f : genuine.back(), margin = 0.10f;
    const float wake_accept = wake_genuine.empty() ? accept : (std::min)(accept, wake_genuine.back());
    std::printf("Wake word: %zu template renditions, cross-speaker nearest median %.3f, max %.3f; wake distance %.4f\n", wake_genuine.size(),
                static_cast<double>(percentile(wake_genuine, 0.5)), static_cast<double>(wake_genuine.empty() ? 0 : wake_genuine.back()), static_cast<double>(wake_accept));
    // Quantisation: one signed byte per value.
    std::vector<const Rendition*> stored = templates; stored.insert(stored.end(), fillers.begin(), fillers.end());
    stored.insert(stored.end(), contextual.begin(), contextual.end()); // Words as said in sentences (embedded training).
    float largest = 0;
    for (const auto* t : stored) for (float v : t->features) largest = (std::max)(largest, std::fabs(v));
    const float scale = largest / 127.0f;
    std::vector<std::int8_t> data; std::vector<std::size_t> offsets;
    for (const auto* t : stored) {
        offsets.push_back(data.size());
        for (float v : t->features) data.push_back(static_cast<std::int8_t>(std::lround((std::clamp)(v / scale, -127.0f, 127.0f))));
    }
    // ---- Measure with the recogniser itself, configured from the quantised templates ----
    // Languages: English words (and the French fillers, kept from the first set) are English; the Hebrew words and
    // their fillers Hebrew; the wake word both.
    const auto languages_of = [&](const Rendition& r) -> std::uint8_t {
        if (r.command == g_wake) return media::ai::kEnglish | media::ai::kHebrew;
        if (r.command >= 0) return g_commands[r.command].lexical || g_commands[r.command].language == kFrench ? media::ai::kHebrew : media::ai::kEnglish;
        const auto& f = g_fillers[r.filler];
        return f.lexical ? media::ai::kHebrew : f.language == kFrench ? static_cast<std::uint8_t>(media::ai::kEnglish | media::ai::kHebrew) : media::ai::kEnglish;
    };
    std::vector<media::ai::VoiceTemplate> set_templates;
    for (std::size_t i = 0; i < stored.size(); ++i) {
        const bool filler = stored[i]->command == kFillerWord;
        set_templates.push_back({filler ? "" : g_commands[stored[i]->command].text.c_str(), filler ? media::ai::kFiller : static_cast<std::uint16_t>(stored[i]->command),
                                 static_cast<std::uint16_t>(stored[i]->frames), data.data() + offsets[i], stored[i]->in_context, languages_of(*stored[i])});
    }
    media::ai::VoiceTemplateSet set{set_templates, scale, accept, margin};
    set.wake = g_wake; set.wake_distance = wake_accept;
    media::core::LinearMemoryArena arena; AcousticIntentRecognizer recogniser;
    if (!arena.Initialize(256 * 1024 * 1024) || !recogniser.Configure(arena, set)) { std::puts("FAIL recogniser setup"); return 1; }
    std::printf("Prefix pairs:"); for (int c = 0; c < command_count; ++c) for (int d = 0; d < command_count; ++d) {
        const auto& x = g_commands[c].text; const auto& y = g_commands[d].text;
        if (x.size() > y.size() && x.compare(x.size() - y.size(), y.size(), y) == 0 && x.substr(0, x.size() - y.size()).find(' ') == std::string::npos) std::printf(" %s/%s", y.c_str(), x.c_str());
    }
    std::printf(" (%u in the recogniser)\n", recogniser.PrefixPairs());
    const auto hear = [&](const std::vector<float>& samples) { recogniser.Begin(); recogniser.Feed(samples); return recogniser.Finish(); };
    struct Tally { unsigned right = 0, wrong = 0, refused = 0; } clean, noisy20, noisy10;
    const auto name_of = [&](int c) { return c == media::ai::kFiller ? std::string("(filler)") : c >= 0 && c < command_count ? g_commands[c].text : std::string("-"); };
    const auto judge = [&](const media::ai::AcousticResult& r, int command, Tally& tally, const char* label, const char* condition) {
        // A word that is no command alone (Partial) is right when it is that word: nothing done is what it asks for.
        const bool accepted = r.match == media::ai::AcousticMatch::Accepted, found = accepted || r.match == media::ai::AcousticMatch::Partial;
        if (found && r.command == command) ++tally.right; else if (accepted) ++tally.wrong; else ++tally.refused;
        if (!found || r.command != command)
            std::printf("    %-48s %-9s -> %s (%s %.3f, next %s %.3f)\n", label, condition, accepted ? "WRONG COMMAND" : "refused", r.text ? r.text : "-", static_cast<double>(r.distance),
                        name_of(r.runner_up).c_str(), static_cast<double>(r.runner_up_distance));
    };
    std::puts("Held-out voices (never in the templates), the first set's words:");
    for (const auto* h : held) {
        if (h->synth || h->command >= kBaseCommands) continue;
        judge(hear(h->samples), h->command, clean, h->label.c_str(), "clean");
        judge(hear(Noisy(h->samples, 20, 17)), h->command, noisy20, h->label.c_str(), "20 dB SNR");
        judge(hear(Noisy(h->samples, 10, 29)), h->command, noisy10, h->label.c_str(), "10 dB SNR");
    }
    unsigned first_count = 0; for (const auto* h : held) first_count += !h->synth && h->command < kBaseCommands ? 1u : 0u;
    std::printf("  clean: %u of %u right, %u wrong command, %u refused; 20 dB: %u right, %u wrong, %u refused; 10 dB: %u right, %u wrong, %u refused\n",
                clean.right, first_count, clean.wrong, clean.refused, noisy20.right, noisy20.wrong, noisy20.refused, noisy10.right, noisy10.wrong, noisy10.refused);
    // The new words alone: the wake word by Mark and Julie, the Hebrew words by Julie (French spellings) and by the
    // synthetic held-out speakers.
    for (const bool synth : {false, true}) {
        Tally words{}; unsigned wakes = 0, wake_said = 0, wake_false = 0;
        for (const auto* h : held) {
            if (h->synth != synth || h->command < kBaseCommands) continue;
            const auto r = hear(h->samples);
            if (h->command == g_wake) { ++wake_said; if (r.wake) ++wakes; else std::printf("    %-48s wake word missed (%s %.3f, next %s %.3f)\n", h->label.c_str(), r.text ? r.text : "-", static_cast<double>(r.distance), name_of(r.runner_up).c_str(), static_cast<double>(r.runner_up_distance)); continue; }
            if (r.wake) ++wake_false;
            judge(r, h->command, words, h->label.c_str(), "clean");
        }
        std::printf("  %s: wake word alone %u of %u; Hebrew words alone %u right, %u wrong, %u refused; %u woke instead\n",
                    synth ? "synthetic held-out speakers" : "Mark and Julie", wakes, wake_said, words.right, words.wrong, words.refused, wake_false);
    }
    unsigned false_accepts = 0, impostor_wakes = 0;
    std::puts("Words that are no command:");
    for (const auto* w : impostors) {
        const auto r = hear(w->samples);
        if (r.wake) ++impostor_wakes;
        if (r.match == media::ai::AcousticMatch::Accepted) { ++false_accepts; std::printf("    %-48s ACCEPTED as %s (%.3f)\n", w->label.c_str(), r.text, static_cast<double>(r.distance)); }
        else std::printf("    %-48s refused (%s)\n", w->label.c_str(), r.match == media::ai::AcousticMatch::Filler ? "nearest a filler" : r.match == media::ai::AcousticMatch::Ambiguous ? "ambiguous" :
                                                                      r.match == media::ai::AcousticMatch::Partial ? "no command alone" : "distant");
    }
    std::printf("  %u of %zu accepted as a command, %u as the wake word\n", false_accepts, impostors.size(), impostor_wakes);
    // The template voices' own renditions through the quantised set (a sanity check: each must find itself).
    unsigned self_wrong = 0;
    for (const auto* t : templates) { const auto r = hear(t->samples);
        if ((r.match != media::ai::AcousticMatch::Accepted && r.match != media::ai::AcousticMatch::Partial) || r.command != t->command) ++self_wrong; }
    std::printf("Templates heard back: %u of %zu not recognised as themselves\n", self_wrong, templates.size());
    std::printf("Set: %zu command templates and %zu fillers, %zu values (%zu bytes), scale %.5f, accept %.4f, margin %.2f\n", templates.size(), fillers.size(), data.size(), data.size(), static_cast<double>(scale),
                static_cast<double>(accept), static_cast<double>(margin));
    // ---- Connected words: sentences, decoded word by word (AcousticIntentRecognizer::Decode) ----
    std::vector<std::vector<float>> dequantised(stored.size());
    std::vector<const float*> blocks(stored.size());
    for (std::size_t i = 0; i < stored.size(); ++i) {
        for (std::uint32_t v = 0; v < stored[i]->frames * AcousticIntentRecognizer::kDimensions; ++v) dequantised[i].push_back(static_cast<float>(data[offsets[i] + v]) * scale);
        blocks[i] = dequantised[i].data();
    }
    std::vector<float> decode_scratch(AcousticIntentRecognizer::DecodeScratch(set_templates));
    // A spoken sentence: what was said, what it must do (`means`, nullptr: nothing), and whether it begins with the
    // wake word. `family`: 0 the first set's English sentences, 1 English with the wake word, 2 English room talk that
    // begins like it, 3 Hebrew (synthesised), 4 Hebrew (French voices).
    struct Spoken { std::string text, label; const char* means; bool wake; int family; std::wstring speaker; bool held_out; std::vector<float> samples, features; std::uint32_t frames = 0; };
    std::vector<Spoken> spoken;
    const auto add_spoken = [&](Spoken s) {
        static std::vector<float> scratch_mel(AcousticIntentRecognizer::kMaxFrames * media::audio::FilterbankFeatures::kChannels);
        s.features.assign(AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions, 0.0f);
        s.frames = AcousticIntentRecognizer::Describe(s.samples, scratch_mel.data(), s.features.data());
        if (!s.frames) { std::printf("  sentence not analysable: %s\n", s.label.c_str()); return; }
        spoken.push_back(std::move(s));
    };
    for (const auto& sentence : kSentences)
        for (const auto& v : voices) {
            if (IsFrench(v)) continue;
            const auto speaker = Speaker(v); const bool unseen = held_speaker(speaker);
            for (const int rate : unseen ? std::initializer_list<int>{-3, -1, 0, 1, 3} : std::initializer_list<int>{-2, 0, 2}) {
                wchar_t xml[512]; std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/>%ls", rate, sentence.spoken);
                char label[200]; std::snprintf(label, sizeof(label), "%s | %s rate %d", Narrow(sentence.spoken).c_str(), Narrow(speaker).c_str(), rate);
                add_spoken({Narrow(sentence.spoken), label, sentence.means, false, 0, speaker, unseen, Trim(Speak(v, xml)), {}});
            }
        }
    for (const auto& v : voices) {
        if (IsFrench(v)) continue;
        const auto speaker = Speaker(v); const bool unseen = held_speaker(speaker);
        // The wake word and a command in one breath (no comma: SAPI pauses at one).
        for (const auto& sentence : kSentences) {
            if (!sentence.means) continue;
            for (const int rate : unseen ? std::initializer_list<int>{-1, 1} : std::initializer_list<int>{0}) {
                wchar_t xml[512]; std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/>avion %ls", rate, sentence.spoken);
                char label[200]; std::snprintf(label, sizeof(label), "avion %s | %s rate %d", Narrow(sentence.spoken).c_str(), Narrow(speaker).c_str(), rate);
                add_spoken({"avion " + Narrow(sentence.spoken), label, sentence.means, true, 1, speaker, unseen, Trim(Speak(v, xml)), {}});
            }
        }
        for (const auto* sentence : kNearWake)
            for (const int rate : unseen ? std::initializer_list<int>{-1, 1} : std::initializer_list<int>{0}) {
                wchar_t xml[512]; std::swprintf(xml, std::size(xml), L"<rate absspeed=\"%d\"/>%ls", rate, sentence);
                char label[200]; std::snprintf(label, sizeof(label), "%s | %s rate %d", Narrow(sentence).c_str(), Narrow(speaker).c_str(), rate);
                add_spoken({Narrow(sentence), label, nullptr, false, 2, speaker, unseen, Trim(Speak(v, xml)), {}});
            }
    }
    // Hebrew: the test sentences (alone and after the wake word) and room chatter, by the synthetic speakers (template
    // ones for tuning, held-out ones measured) and by the French voices (Julie held out).
    std::vector<std::pair<std::string, const char*>> hebrew; // Text and meaning; "avion ..." wakes.
    for (const auto& s : lexicon::kTestSentences) { hebrew.push_back({s.text, s.means}); hebrew.push_back({std::string("avion ") + s.text, s.means}); }
    for (const auto* s : lexicon::kChatter) hebrew.push_back({s, nullptr});
    for (const auto& [text, means] : hebrew) {
        const bool wake = text.rfind("avion ", 0) == 0;
        char phonemes[1024]; if (!lexicon::SentencePhonemes(text, 2, phonemes, sizeof(phonemes))) { std::printf("  FAIL phonemes: %s\n", text.c_str()); continue; }
        for (const bool unseen : {false, true}) {
            const auto* speakers = unseen ? lexicon::kHeldOutSpeakers : lexicon::kTemplateSpeakers;
            for (std::size_t i = 0; i < (unseen ? std::size(lexicon::kHeldOutSpeakers) : std::size(lexicon::kTemplateSpeakers)); ++i) {
                auto voice = speakers[i].voice; voice.seed += 977u;
                char label[300]; std::snprintf(label, sizeof(label), "%s | %s", text.c_str(), speakers[i].name);
                add_spoken({text, label, means, wake, 3, Wide(speakers[i].name), unseen, Trim(Formant(phonemes, voice)), {}});
            }
        }
        char french[512]; if (!lexicon::SentenceFrench(text, french, sizeof(french))) continue;
        for (const auto& v : voices) {
            if (!IsFrench(v)) continue;
            const auto speaker = Speaker(v); const bool unseen = held_speaker(speaker);
            wchar_t xml[512]; std::swprintf(xml, std::size(xml), L"<rate absspeed=\"0\"/>%ls", Wide(french).c_str());
            char label[400]; std::snprintf(label, sizeof(label), "%s (\"%s\") | %s rate 0", text.c_str(), french, Narrow(speaker).c_str());
            add_spoken({text, label, means, wake, 4, speaker, unseen, Trim(Speak(v, xml)), {}});
        }
    }
    // Cross-validation between the template speakers: for each, a recogniser of everything the *other* speakers
    // recorded, so that what is tuned below is tuned as for a speaker the templates have never heard.
    struct Fold {
        std::wstring speaker; std::vector<media::ai::VoiceTemplate> templates; std::vector<const float*> blocks;
        media::core::LinearMemoryArena arena; AcousticIntentRecognizer recogniser;
    };
    std::vector<std::unique_ptr<Fold>> folds;
    std::vector<std::wstring> fold_speakers{L"David", L"Zira", L"Hortense", L"Paul"};
    for (const auto& s : lexicon::kTemplateSpeakers) fold_speakers.push_back(Wide(s.name));
    for (const auto& name : fold_speakers) {
        auto fold = std::make_unique<Fold>(); fold->speaker = name;
        std::size_t values = 0;
        for (std::size_t i = 0; i < stored.size(); ++i)
            if (stored[i]->speaker != fold->speaker) { fold->templates.push_back(set_templates[i]); fold->blocks.push_back(blocks[i]); values += stored[i]->frames * AcousticIntentRecognizer::kDimensions; }
        media::ai::VoiceTemplateSet subset{fold->templates, scale, accept, margin}; subset.wake = g_wake; subset.wake_distance = wake_accept;
        const std::size_t bytes = 4 * (values + AcousticIntentRecognizer::DecodeScratch(fold->templates)) + (8u << 20);
        if (!fold->arena.Initialize(bytes) || !fold->recogniser.Configure(fold->arena, subset)) { std::puts("FAIL fold setup"); return 1; }
        folds.push_back(std::move(fold));
    }
    const auto fold_of = [&](const std::wstring& speaker) -> Fold* { for (auto& f : folds) if (f->speaker == speaker) return f.get(); return nullptr; };
    const auto hear_with = [](AcousticIntentRecognizer& r, const std::vector<float>& samples) { r.Begin(); r.Feed(samples); return r.Finish(); };
    // The engine's own policy (AcousticIntentRecognizer::Finish): one command word first, else connected words. What it
    // makes of a sentence: the text it hands on, whether it woke, and what the grammar reads after the wake word.
    // Template voices go through their fold; the held-out voices through the full set.
    // 1: right (the wake as expected and the command, or nothing, as meant); 0: missed; -1: a command where none was
    // said; -2: another command; -3: woke where it must not (room talk), -4: did not wake where it must.
    const media::ai::MediaBounds film{7200ll * 10000000, 600ll * 10000000, 0.5f, false, false, true, true, 0, 0, 1, 1};
    const auto judge_sentence = [&](const Spoken& s, std::string* text) {
        auto* fold = s.held_out ? nullptr : fold_of(s.speaker);
        const auto result = fold ? hear_with(fold->recogniser, s.samples) : hear(s.samples);
        const std::string heard = result.match == media::ai::AcousticMatch::Accepted && result.text ? result.text : "";
        if (text) *text = heard;
        std::string rest; const bool woke = Woke(heard, rest);
        if (woke && !s.wake) return -3;
        const auto got = media::ai::LocalIntentDispatcher::Parse(rest, film);
        if (!s.means) return got.status == media::ai::IntentStatus::Ok ? -1 : 1;
        if (s.wake && !woke) return got.status == media::ai::IntentStatus::Ok ? -4 : 0;
        const auto want = media::ai::LocalIntentDispatcher::Parse(s.means, film);
        if (got.status != media::ai::IntentStatus::Ok) return 0; // Missed.
        const bool same = got.kind == want.kind && got.target_100ns == want.target_100ns && got.volume == want.volume;
        return same ? 1 : -2;
    };
    // Tuning on the template voices only (cross-validated through the folds). The search settings (garbage, word
    // penalty) are shared by both language lanes, so that their decodings stay comparable by cost; each lane's word
    // limit and sentence floor are chosen on that lane's speech alone: English (the English families and the English
    // voices' words that are no command), Hebrew (the Hebrew families and the French voices' words). A wrong command, a
    // command heard in plain talk, a wake in room talk or a word taken for a command counts double against.
    struct Lane { float limit = 0; std::uint32_t frames = 0; };
    struct Setting { float garbage, penalty; Lane lanes[2]; std::vector<float> typical; } best{0, 0, {}, {}};
    double best_score = -1e30;
    std::vector<std::wstring> french_speakers; for (const auto& v : voices) if (IsFrench(v)) french_speakers.push_back(Speaker(v));
    const auto lane_of_word = [&](const Rendition* w) { return std::find(french_speakers.begin(), french_speakers.end(), w->speaker) != french_speakers.end() ? 1 : 0; };
    const auto lane_of = [](const Spoken& s) { return s.family >= 3 ? 1 : 0; };
    // Each command word's typical cost in running speech: the median of its costs where the template voices'
    // sentences say it (those sentences cut no template, so nothing is matched against itself). `all_families`: the
    // Hebrew and wake sentences too (the first set's tuning uses only its own).
    // Runs `work(fold index)` for every fold at once, one thread each (folds share nothing).
    const auto for_each_fold = [&](const auto& work) {
        std::vector<std::thread> threads;
        for (std::size_t f = 0; f < folds.size(); ++f) threads.emplace_back([&, f] { work(f); });
        for (auto& t : threads) t.join();
    };
    const auto typical_costs = [&](float garbage, float penalty, bool all_families) {
        // Per command and per kind of rendition (0: SAPI voices, 1: the formant synthesiser), gathered per fold.
        struct Gathered { std::vector<std::vector<float>> costs, by_source[2]; };
        std::vector<Gathered> gathered(folds.size());
        for_each_fold([&](std::size_t f) {
            auto& g = gathered[f]; g.costs.assign(command_count, {}); g.by_source[0].assign(command_count, {}); g.by_source[1].assign(command_count, {});
            std::vector<float> scratch(AcousticIntentRecognizer::DecodeScratch(folds[f]->templates));
            for (const auto& s : spoken) {
                if (s.held_out || !s.means || (!all_families && s.family != 0) || s.speaker != folds[f]->speaker) continue;
                const auto* fold = folds[f].get();
                const std::string said = " " + s.text + " ";
                media::ai::SpottedWord words[AcousticIntentRecognizer::kMaxWords];
                const auto n = AcousticIntentRecognizer::DecodeBest(s.features.data(), s.frames, fold->templates, fold->blocks.data(), garbage, penalty, scratch.data(), words, AcousticIntentRecognizer::kMaxWords);
                for (unsigned i = 0; i < n; ++i) {
                    const auto& t = fold->templates[words[i].template_index];
                    if (t.command != media::ai::kFiller && said.find(std::string(" ") + t.text + " ") != std::string::npos) {
                        g.costs[t.command].push_back(words[i].cost);
                        g.by_source[s.speaker.rfind(L"Synth", 0) == 0 ? 1 : 0][t.command].push_back(words[i].cost);
                    }
                }
            }
        });
        std::vector<std::vector<float>> costs(command_count), by_source[2]{std::vector<std::vector<float>>(command_count), std::vector<std::vector<float>>(command_count)};
        for (const auto& g : gathered) // In fold order: the medians below do not depend on the threads.
            for (int c = 0; c < command_count; ++c) {
                costs[c].insert(costs[c].end(), g.costs[c].begin(), g.costs[c].end());
                for (int k = 0; k < 2; ++k) by_source[k][c].insert(by_source[k][c].end(), g.by_source[k][c].begin(), g.by_source[k][c].end());
            }
        std::vector<float> all, out(command_count, 0.0f);
        for (int c = 0; c < command_count; ++c) {
            auto& v = costs[c]; if (v.empty()) continue;
            std::sort(v.begin(), v.end()); out[c] = v[v.size() / 2]; all.insert(all.end(), v.begin(), v.end());
        }
        std::sort(all.begin(), all.end());
        for (auto& x : out) if (x == 0 && !all.empty()) x = all[all.size() / 2]; // Words no sentence says: the median word.
        // The wake word and the Hebrew words have renditions of two kinds whose costs differ (the synthesiser's match
        // each other more closely than SAPI voices do): their limit must cover both, as the median covers one kind.
        for (int c = kBaseCommands; c < command_count; ++c)
            for (auto& source : by_source) {
                auto& v = source[c]; if (v.empty()) continue;
                std::sort(v.begin(), v.end()); out[c] = (std::max)(out[c], v[v.size() / 2]);
            }
        return out;
    };
    // The connected-word settings are the ones the grid chose on 2026-10-15 (garbage 2.2 x accept, penalty 15, English
    // word limit 1.10 from 70 frames, Hebrew no limit from 100 frames; kShippedSettings). Re-running the grid when the
    // vocabulary grew (2026-10-16: the chapter and scene words) chose garbage 1.6 x accept and penalty 5, with which the
    // template voices' English made 13 wrong commands and room talk was heard as commands; with the kept settings the
    // held-out voices made none. MEDIA_VOICE_GRID=1 runs the grid instead (it is still printed);
    // MEDIA_VOICE_SETTINGS=g,p,l,f[,l,f] (garbage factor, penalty, word limit and sentence frames for English[, and for
    // Hebrew]) sets others (experiments).
    float fixed[6]{kShippedSettings[0], kShippedSettings[1], kShippedSettings[2], kShippedSettings[3], kShippedSettings[4], kShippedSettings[5]};
    bool given = GetEnvironmentVariableW(L"MEDIA_VOICE_GRID", nullptr, 0) == 0;
    {
        wchar_t value[96]{};
        if (GetEnvironmentVariableW(L"MEDIA_VOICE_SETTINGS", value, 96)) {
            const int read = swscanf_s(value, L"%f,%f,%f,%f,%f,%f", &fixed[0], &fixed[1], &fixed[2], &fixed[3], &fixed[4], &fixed[5]);
            if (read == 4) { fixed[4] = fixed[2]; fixed[5] = fixed[3]; }
            given = read == 4 || read == 6;
        }
    }
    if (given) { best = {-fixed[0] * accept, fixed[1], {{fixed[2], static_cast<std::uint32_t>(fixed[3])}, {fixed[4], static_cast<std::uint32_t>(fixed[5])}}, {}}; best_score = 0; }
    // The typical costs the recogniser uses: the first set's words from their own sentences, the new words from all.
    const auto composed_typical = [&](float garbage, float penalty) {
        auto typical = typical_costs(garbage, penalty, false);
        const auto all = typical_costs(garbage, penalty, true);
        for (int c = kBaseCommands; c < command_count; ++c) typical[c] = all[c];
        return typical;
    };
    // Every template voice's utterances through its fold, scored per lane: right +1, missed 0, wrong -2 (see above).
    struct LaneScores { int score[2]{}; int count[2]{}; };
    const auto lane_scores = [&]() {
        std::vector<LaneScores> per_fold(folds.size());
        for_each_fold([&](std::size_t k) {
            auto& out = per_fold[k];
            for (const auto& s : spoken) {
                if (s.held_out || s.speaker != folds[k]->speaker) continue;
                const int r = judge_sentence(s, nullptr), lane = lane_of(s);
                out.score[lane] += r == 1 ? 1 : (r == 0 || r == -4) ? 0 : -2; ++out.count[lane];
            }
            for (const auto* w : impostors) {
                if (w->held_out || w->speaker != folds[k]->speaker) continue;
                const int lane = lane_of_word(w);
                if (hear_with(folds[k]->recogniser, w->samples).match == media::ai::AcousticMatch::Accepted) out.score[lane] -= 2;
                ++out.count[lane];
            }
        });
        LaneScores total;
        for (const auto& f : per_fold) for (int l = 0; l < 2; ++l) { total.score[l] += f.score[l]; total.count[l] += f.count[l]; }
        return total;
    };
    // Both lanes weigh the same, whatever their numbers of utterances: the score of a lane is per utterance.
    const auto balanced = [](const LaneScores& s) {
        double v = 0; for (int l = 0; l < 2; ++l) v += s.count[l] ? static_cast<double>(s.score[l]) / s.count[l] : 0.0;
        return v;
    };
    const auto apply = [&](const Setting& setting, auto& r) {
        r.SetDecoding(setting.garbage, setting.penalty, setting.lanes[0].limit, setting.lanes[0].frames, setting.typical);
        for (unsigned l = 0; l < 2; ++l) r.SetLane(l, setting.lanes[l].limit, static_cast<std::int32_t>(setting.lanes[l].frames));
    };
    if (!given) for (const float g : {1.6f, 1.9f, 2.2f})
        for (const float p : {5.0f, 15.0f}) {
            const auto typical = composed_typical(-g * accept, p);
            // Both lanes swept together; each keeps the limit and floor that score best on its own speech (the lanes
            // hardly meet: an utterance is decided by the lane that wins it).
            int lane_best[2]{INT32_MIN, INT32_MIN}, lane_count[2]{}; Lane chosen[2]{};
            for (const float l : {0.0f, 1.10f, 1.20f, 1.35f, 1.50f})
                for (const std::uint32_t f : {0u, 70u, 100u}) {
                    const Setting trial{-g * accept, p, {{l, f}, {l, f}}, typical};
                    for (auto& fold : folds) apply(trial, fold->recogniser);
                    const auto scores = lane_scores();
                    std::printf("  grid: garbage %.2f x accept, penalty %.0f, limit %.2f, sentences from %u frames: English %d of %d, Hebrew %d of %d\n", static_cast<double>(g),
                                static_cast<double>(p), static_cast<double>(l), f, scores.score[0], scores.count[0], scores.score[1], scores.count[1]);
                    for (int lane = 0; lane < 2; ++lane)
                        if (scores.score[lane] > lane_best[lane]) { lane_best[lane] = scores.score[lane]; lane_count[lane] = scores.count[lane]; chosen[lane] = {l, f}; }
                }
            const Setting candidate{-g * accept, p, {chosen[0], chosen[1]}, typical};
            for (auto& fold : folds) apply(candidate, fold->recogniser);
            const auto scores = lane_scores(); const double score = balanced(scores);
            std::printf("  grid: garbage %.2f x accept, penalty %.0f; English limit %.2f from %u frames, Hebrew limit %.2f from %u frames: English %d of %d, Hebrew %d of %d, balanced %.4f\n",
                        static_cast<double>(g), static_cast<double>(p), static_cast<double>(chosen[0].limit), chosen[0].frames, static_cast<double>(chosen[1].limit), chosen[1].frames,
                        scores.score[0], scores.count[0], scores.score[1], scores.count[1], score);
            (void)lane_count;
            if (score > best_score) { best_score = score; best = candidate; }
        }
    // A floor on what counts as a sentence, above the tuning's (MEDIA_VOICE_FRAMES): single words must not be
    // decoded as sentences more often than the one-word matcher accepts them.
    { wchar_t value[16]{}; if (GetEnvironmentVariableW(L"MEDIA_VOICE_FRAMES", value, 16)) for (auto& lane : best.lanes) lane.frames = (std::max)(lane.frames, static_cast<std::uint32_t>(_wtoi(value))); }
    // Typical costs of every word, the new ones included, at the chosen settings.
    best.typical = composed_typical(best.garbage, best.penalty);
    apply(best, recogniser);
    for (auto& fold : folds) apply(best, fold->recogniser);
    for (int c = 0; c < command_count; ++c) std::printf("  typical cost of \"%s\": %.2f\n", g_commands[c].text.c_str(), static_cast<double>(best.typical[c]));
    // The beam (VoiceTemplateSet::beam), tuned like the settings above: the narrowest whose template-voice score is no
    // lower in either lane than the full search's. How many answers it changes is printed (changes go both ways: a
    // pruned path is sometimes the wrong word). MEDIA_VOICE_BEAM=<beam> overrides it (0: none; experiments only).
    float beam = 0;
    {
        const auto answers = [&](float b, LaneScores& scores, unsigned long long& ms) {
            for (auto& fold : folds) fold->recogniser.SetBeam(b);
            std::vector<std::vector<std::string>> out(folds.size());
            const auto t0 = GetTickCount64();
            for_each_fold([&](std::size_t f) {
                const auto answer = [&](const std::vector<float>& samples) {
                    const auto r = hear_with(folds[f]->recogniser, samples);
                    out[f].push_back(std::to_string(static_cast<int>(r.match)) + "|" + (r.text ? r.text : ""));
                };
                for (const auto& s : spoken) if (!s.held_out && s.speaker == folds[f]->speaker) answer(s.samples);
                for (const auto* w : impostors) if (!w->held_out && w->speaker == folds[f]->speaker) answer(w->samples);
            });
            ms = GetTickCount64() - t0;
            scores = lane_scores();
            return out;
        };
        LaneScores full_scores; unsigned long long full_ms = 0;
        const auto full = answers(0, full_scores, full_ms);
        std::size_t total = 0; for (const auto& f : full) total += f.size();
        wchar_t value[32]{};
        // Like the settings: the 2026-10-15 beam (kShippedBeam) unless the grid is asked for.
        if (GetEnvironmentVariableW(L"MEDIA_VOICE_BEAM", value, 32)) beam = static_cast<float>(_wtof(value));
        else if (GetEnvironmentVariableW(L"MEDIA_VOICE_GRID", nullptr, 0) == 0) beam = kShippedBeam;
        else for (const float b : {20.0f, 25.0f, 30.0f, 35.0f, 40.0f, 50.0f, 60.0f, 80.0f, 120.0f}) {
            LaneScores scores; unsigned long long ms = 0;
            const auto pruned = answers(b, scores, ms);
            std::size_t differ = 0;
            for (std::size_t f = 0; f < full.size(); ++f) for (std::size_t i = 0; i < full[f].size(); ++i) differ += pruned[f][i] != full[f][i] ? 1u : 0u;
            std::printf("  beam %.0f: English %d, Hebrew %d (full search %d, %d); %zu of %zu template-voice answers differ; %llu ms (full %llu ms)\n", static_cast<double>(b),
                        scores.score[0], scores.score[1], full_scores.score[0], full_scores.score[1], differ, total, ms, full_ms);
            if (scores.score[0] >= full_scores.score[0] && scores.score[1] >= full_scores.score[1]) { beam = b; break; }
        }
        recogniser.SetBeam(beam); for (auto& fold : folds) fold->recogniser.SetBeam(beam);
        std::printf("Beam %.1f\n", static_cast<double>(beam));
    }
    // The score representations are chosen on (MEDIA_VOICE_KEEP, MEDIA_VOICE_PACK): the template
    // voices through their folds, as the settings, plus every isolated command rendition by a template voice (kept
    // as a template or not: through its fold, none of its speaker's templates is there): +1 right, -2 another command.
    {
        auto scores = lane_scores();
        std::vector<LaneScores> per_fold(folds.size());
        for_each_fold([&](std::size_t k) {
            for (const auto& r : renditions) {
                if (r.held_out || r.in_context || r.command < 0 || r.speaker != folds[k]->speaker) continue;
                const int lane = g_commands[r.command].lexical && r.command != g_wake ? 1 : g_commands[r.command].language == kFrench ? 1 : 0;
                const auto result = hear_with(folds[k]->recogniser, r.samples);
                const bool found = result.match == media::ai::AcousticMatch::Accepted || result.match == media::ai::AcousticMatch::Partial;
                if (found && !result.sentence && result.command == r.command) ++per_fold[k].score[lane];
                else if (result.match == media::ai::AcousticMatch::Accepted) per_fold[k].score[lane] -= 2;
                ++per_fold[k].count[lane];
            }
        });
        int words[2]{}, word_count[2]{};
        for (const auto& f : per_fold) for (int l = 0; l < 2; ++l) { words[l] += f.score[l]; word_count[l] += f.count[l]; scores.score[l] += f.score[l]; scores.count[l] += f.count[l]; }
        std::printf("Cross-validated template-voice score: English %d of %d (words alone %d of %d), Hebrew %d of %d (words alone %d of %d); balanced %.4f\n",
                    scores.score[0], scores.count[0], words[0], word_count[0], scores.score[1], scores.count[1], words[1], word_count[1], balanced(scores));
    }
    std::printf("Connected words: garbage %.4f per frame, word penalty %.1f; English lane: word limit %.4f, sentences from %u frames; Hebrew lane: word limit %.4f, "
                "sentences from %u frames; beam %.1f; balanced tuning score %.4f (template voices)\n", static_cast<double>(-best.garbage), static_cast<double>(best.penalty),
                static_cast<double>(best.lanes[0].limit), best.lanes[0].frames, static_cast<double>(best.lanes[1].limit), best.lanes[1].frames, static_cast<double>(beam), best_score);
    static const char* const families[]{"English", "English after the wake word", "English room talk like the wake word", "Hebrew (synthesised)", "Hebrew (French voices)"};
    for (int family = 0; family < 5; ++family)
        for (const bool unseen : {false, true}) {
            unsigned right = 0, missed = 0, wrong = 0, said = 0, plain = 0, invented = 0, woke_wrongly = 0, unwoken = 0, count = 0;
            for (const auto& s : spoken) {
                if (s.held_out != unseen || s.family != family) continue;
                ++count;
                std::string text; const int r = judge_sentence(s, &text);
                if (r == -3) ++woke_wrongly;
                if (r == -4) ++unwoken;
                if (s.means) { ++said; if (r == 1) ++right; else if (r == 0 || r == -4) ++missed; else ++wrong; }
                else { ++plain; if (r == -1 || r == -3) ++invented; }
                if (unseen && r != 1) std::printf("    %-70s heard \"%s\"%s\n", s.label.c_str(), text.c_str(), r == 0 ? " (no command)" : r == -1 ? " (A COMMAND)" : r == -3 ? " (WOKE)" : r == -4 ? " (DID NOT WAKE)" : " (WRONG COMMAND)");
            }
            if (!count) continue;
            std::printf("  %s, %s: %u of %u commands right, %u missed, %u wrong; %u of %u without a command heard as one; %u woke wrongly, %u should have woken\n",
                        families[family], unseen ? "held out" : "template (cross-validated)", right, said, missed, wrong, invented, plain, woke_wrongly, unwoken);
        }
    // The decoded words and their costs (MEDIA_VOICE_WORDS=1): how true and false sentence results differ.
    if (GetEnvironmentVariableW(L"MEDIA_VOICE_WORDS", nullptr, 0)) {
        std::vector<float> scratch(AcousticIntentRecognizer::DecodeScratch(set_templates));
        for (const auto& s : spoken) {
            if (!s.held_out) continue;
            media::ai::SpottedWord words[AcousticIntentRecognizer::kMaxWords];
            const auto n = AcousticIntentRecognizer::DecodeBest(s.features.data(), s.frames, set_templates, blocks.data(), best.garbage, best.penalty, scratch.data(), words, AcousticIntentRecognizer::kMaxWords,
                                                                nullptr, beam);
            std::string line;
            for (unsigned i = 0; i < n; ++i) {
                const auto& t = set_templates[words[i].template_index]; char piece[160];
                std::snprintf(piece, sizeof(piece), " %s%s:%.2f[%u]", t.command == media::ai::kFiller ? "~" : "", t.command == media::ai::kFiller ? stored[words[i].template_index]->label.substr(0, stored[words[i].template_index]->label.find(' ')).c_str() : t.text,
                              static_cast<double>(words[i].cost), static_cast<unsigned>(words[i].end - words[i].begin + 1));
                line += piece;
            }
            std::printf("  WORDS %-60s %s\n", s.label.c_str(), line.c_str());
        }
    }
    // Single words through the same policy (the held-out corpus, clean).
    {
        unsigned right = 0, refused = 0, wrong = 0, impostors_accepted = 0, total = 0;
        for (const auto* h : held) {
            if (h->synth || h->command >= kBaseCommands) continue;
            ++total;
            const auto r = hear(h->samples);
            const bool partial = r.match == media::ai::AcousticMatch::Partial; // That word, which is no command alone: right.
            if (r.match != media::ai::AcousticMatch::Accepted && !(partial && r.command == h->command)) ++refused;
            else if (partial || (r.text && g_commands[h->command].text == r.text)) ++right; else ++wrong;
        }
        unsigned by_sentence = 0;
        for (const auto* w : impostors) {
            const auto r = hear(w->samples);
            if (r.match == media::ai::AcousticMatch::Accepted) { ++impostors_accepted; if (r.sentence) ++by_sentence; std::printf("    word %-40s accepted as \"%s\"%s (%u frames)\n", w->label.c_str(), r.text, r.sentence ? " by the sentence path" : "", r.frames); }
        }
        std::printf("  single words: %u of %u right, %u refused, %u wrong; %u of %zu words that are no command accepted (%u by the sentence path); sentences from %u frames\n",
                    right, total, refused, wrong, impostors_accepted, impostors.size(), by_sentence, best.lanes[0].frames);
    }
    SaveCache();
    if (evaluate) { for (auto& v : voices) v.token->Release(); CoUninitialize(); return 0; } // Measured, nothing written.
    // ---- Write the header: the packed set (media::ai::VoicePackedSet) ----
    if (!pack_bits) { std::puts("FAIL nothing packed (MEDIA_VOICE_PACK=0 is for measuring only)"); return 1; }
    const std::wstring root = argv[1];
    FILE* header = nullptr; _wfopen_s(&header, (root + L"\\include\\media\\ai\\VoiceTemplates.hpp").c_str(), L"wb");
    if (!header) { std::puts("FAIL cannot write the header"); return 1; }
    unsigned synthetic = 0; for (const auto* t : stored) synthetic += t->synth ? 1u : 0u;
    // Words: the commands by number, then the fillers.
    const auto word_of = [&](const Rendition& r) { return r.command == kFillerWord ? static_cast<std::size_t>(command_count) + static_cast<std::size_t>(r.filler) : static_cast<std::size_t>(r.command); };
    const auto escape = [](const std::string& text) {
        std::string escaped;
        for (const char c : text) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 128) escaped += c; else { char byte[12]; std::snprintf(byte, sizeof(byte), "\\x%02x\" \"", u); escaped += byte; }
        }
        return escaped;
    };
    std::vector<std::uint8_t> word_languages(static_cast<std::size_t>(command_count) + g_fillers.size(), 0);
    for (std::size_t i = 0; i < stored.size(); ++i) word_languages[word_of(*stored[i])] = set_templates[i].languages;
    // The key bits and the values, least significant bit first.
    std::vector<std::uint8_t> keys, values; std::size_t key_bits = 0, value_bits = 0, frames = 0;
    const auto put = [](std::vector<std::uint8_t>& out, std::size_t& at, unsigned v, unsigned bits) {
        for (unsigned b = 0; b < bits; ++b, ++at) { if (at / 8 >= out.size()) out.push_back(0); out[at / 8] = static_cast<std::uint8_t>(out[at / 8] | (((v >> b) & 1u) << (at % 8))); }
    };
    for (const auto* t : stored) {
        for (std::uint32_t f = 0, k = 0; f < t->frames; ++f) { const bool kept = k < t->keys.size() && t->keys[k] == f; if (kept) ++k; put(keys, key_bits, kept ? 1u : 0u, 1); }
        for (const auto code : t->codes) put(values, value_bits, code, pack_bits);
        frames += t->frames;
    }
    char kept_phrase[160] = "";
    if (keep[0] || keep[1]) std::snprintf(kept_phrase, sizeof(kept_phrase), ": of each word by each template speaker, its %u most representative renditions alone and %u in context", keep[0], keep[1]);
    std::fprintf(header, "#pragma once\n// GENERATED by tools/make_voice_templates.cpp from Windows speech synthesiser voices and the engine's formant\n"
        "// synthesiser (ai::FormantSynthesizer, media/ai/VoiceLexicon.hpp). Do not edit.\n"
        "// %zu renditions of %d command words and %zu of %zu filler words (kFiller: words that are no command), %u of them\n"
        "// synthesised, plus %zu words as said inside the template voices' training sentences (embedded training: aligned,\n"
        "// then cut)%s.\n"
        "// Packed (docs/voice.md, \"Packed templates\"): %zu frames, of which %zu key frames hold their %u cepstra at %u bits\n"
        "// each; the rest lie on lines between them, and the deltas are made again when the set is configured.\n"
        "// English words by the English voices; Hebrew words by French voices from French spellings of their sounds and by\n"
        "// synthetic speakers from phonemes, since no Hebrew voice was installed: approximations, not Hebrew speech. The wake\n"
        "// word \"%s\" (command %d) by every voice. The voices %s and %s and the synthetic held-out speakers are in no\n"
        "// template (tests/data/voice_corpus.bin, voice_sentences.bin and voice_wake.bin hold the recorded ones). Thresholds\n"
        "// come from the template voices alone; the connected-word settings and beam are those the cross-validation grid chose\n"
        "// on 2026-10-15 (kept when the vocabulary grew: tools/make_voice_templates.cpp, kShippedSettings).\n"
        "#include \"media/ai/AcousticIntentRecognizer.hpp\"\n\nnamespace media::ai::voice_templates {\n",
        templates.size(), command_count, fillers.size(), g_fillers.size(), synthetic, contextual.size(), kept_phrase, frames, value_bits / (AcousticIntentRecognizer::kCepstra * pack_bits),
        AcousticIntentRecognizer::kCepstra, pack_bits, lexicon::kWakeText, g_wake, Narrow(kHeldOut[0]).c_str(), Narrow(kHeldOut[1]).c_str());
    std::fprintf(header, "inline constexpr VoiceWord kWords[] = {\n");
    for (int c = 0; c < command_count; ++c) std::fprintf(header, "    {\"%s\", %d, %u},\n", escape(g_commands[c].text).c_str(), c, static_cast<unsigned>(word_languages[c]));
    for (std::size_t f = 0; f < g_fillers.size(); ++f) {
        std::string ascii = g_fillers[f].text; for (auto& ch : ascii) if (static_cast<unsigned char>(ch) >= 128) ch = '?';
        std::fprintf(header, "    {\"\", kFiller, %u}, // %s\n", static_cast<unsigned>(word_languages[static_cast<std::size_t>(command_count) + f]), ascii.c_str());
    }
    std::fprintf(header, "};\ninline constexpr float kLevels[] = {");
    for (std::size_t k = 0; k < levels.size(); ++k) for (std::size_t l = 0; l < levels[k].size(); ++l) std::fprintf(header, "%s%#.9gf,", l ? " " : "\n    ", static_cast<double>(levels[k][l]));
    const auto bytes = [&](const char* name, const std::vector<std::uint8_t>& data) {
        std::fprintf(header, "\n};\ninline constexpr std::uint8_t %s[] = {", name);
        for (std::size_t i = 0; i < data.size(); ++i) std::fprintf(header, "%s%u,", i % 32 ? "" : "\n    ", static_cast<unsigned>(data[i]));
    };
    bytes("kKeys", keys); bytes("kValues", values);
    std::fprintf(header, "\n};\ninline constexpr VoicePacked kTemplates[] = {\n");
    for (const auto* t : stored) {
        std::string ascii_label = t->label; for (auto& ch : ascii_label) if (static_cast<unsigned char>(ch) >= 128) ch = '?';
        std::fprintf(header, "    {%zu, %u, %u}, // %s\n", word_of(*t), t->frames, t->in_context ? 1u : 0u, ascii_label.c_str());
    }
    // Connected words: the tuned settings, and each command's typical cost in running speech (by command number).
    std::fprintf(header, "};\ninline constexpr float kTypical[] = {");
    for (int c = 0; c < command_count; ++c) std::fprintf(header, "%s%#.6gf", c ? ", " : "", static_cast<double>(best.typical[c]));
    std::fprintf(header, "};\ninline constexpr std::uint32_t kBits = %u;\ninline constexpr float kScale = %#.9gf, kAcceptDistance = %#.9gf, kMargin = %#.9gf;\n"
        "inline constexpr float kGarbage = %#.9gf, kWordPenalty = %#.9gf, kBeam = %#.9gf;\n"
        "// Language lanes: English, Hebrew.\ninline constexpr float kEnglishWordLimit = %#.9gf, kHebrewWordLimit = %#.9gf;\n"
        "inline constexpr std::int32_t kEnglishSentenceFrames = %u, kHebrewSentenceFrames = %u;\n"
        "inline constexpr std::int32_t kWake = %d; // \"%s\"\ninline constexpr float kWakeDistance = %#.9gf;\n"
        "inline constexpr VoicePackedSet kSet{kWords, kTemplates, kKeys, kValues, kLevels, kBits,\n"
        "    VoiceTemplateSet{{}, kScale, kAcceptDistance, kMargin, kGarbage, kWordPenalty, kEnglishWordLimit, kEnglishSentenceFrames, kTypical, kWake, kWakeDistance, kBeam,\n"
        "                     {kEnglishWordLimit, kHebrewWordLimit}, {kEnglishSentenceFrames, kHebrewSentenceFrames}}};\n}\n",
        pack_bits, static_cast<double>(scale), static_cast<double>(accept), static_cast<double>(margin), static_cast<double>(best.garbage), static_cast<double>(best.penalty),
        static_cast<double>(beam), static_cast<double>(best.lanes[0].limit), static_cast<double>(best.lanes[1].limit), best.lanes[0].frames, best.lanes[1].frames,
        g_wake, lexicon::kWakeText, static_cast<double>(wake_accept));
    const std::size_t levels_bytes = levels.size() * (levels.empty() ? 0 : levels[0].size()) * sizeof(float);
    std::printf("Header: %zu templates (%zu bytes of table), %zu bytes of key bits, %zu bytes of values, %zu of levels, %zu words (%zu bytes of table); %zu bytes in all\n",
                stored.size(), stored.size() * sizeof(media::ai::VoicePacked), keys.size(), values.size(), levels_bytes, word_languages.size(),
                word_languages.size() * sizeof(media::ai::VoiceWord), stored.size() * sizeof(media::ai::VoicePacked) + keys.size() + values.size() + levels_bytes + word_languages.size() * sizeof(media::ai::VoiceWord));
    std::fclose(header);
    // ---- The held-out sentences (MVS1): what was said, what it means, and the samples ----
    {
        FILE* sentences = nullptr; _wfopen_s(&sentences, (root + L"\\tests\\data\\voice_sentences.bin").c_str(), L"wb");
        if (!sentences) { std::puts("FAIL cannot write the sentence corpus"); return 1; }
        unsigned count = 0; for (const auto& s : spoken) count += s.held_out && s.family == 0 ? 1u : 0u;
        std::fwrite("MVS1", 1, 4, sentences); Put32(sentences, count);
        double seconds = 0;
        for (const auto& s : spoken) {
            if (!s.held_out || s.family != 0) continue;
            const std::string means = s.means ? s.means : "";
            Put32(sentences, static_cast<std::uint32_t>(s.label.size())); std::fwrite(s.label.data(), 1, s.label.size(), sentences);
            Put32(sentences, static_cast<std::uint32_t>(means.size())); std::fwrite(means.data(), 1, means.size(), sentences);
            PutSamples(sentences, s.samples);
            seconds += static_cast<double>(s.samples.size()) / 16000.0;
        }
        std::fclose(sentences);
        std::printf("Sentence corpus: %u held-out sentences, %.1f s\n", count, seconds);
    }
    // ---- The wake-word corpus (MVS1): the held-out SAPI voices' wake word, Hebrew words and sentences. `means`:
    // "avion" (the wake word alone), "avion <command>" (both in one breath), "<command>" (a command without the wake
    // word), "" (nothing to do); for a Hebrew word alone, its text. ----
    {
        FILE* wake = nullptr; _wfopen_s(&wake, (root + L"\\tests\\data\\voice_wake.bin").c_str(), L"wb");
        if (!wake) { std::puts("FAIL cannot write the wake corpus"); return 1; }
        struct Item { std::string label, means; const std::vector<float>* samples; };
        std::vector<Item> items;
        for (const auto* h : held) {
            if (h->synth || h->command < kBaseCommands) continue;
            // Every variant of the wake word; three of each Hebrew word (the plain rate and two others).
            if (h->command != g_wake && !(h->pitch == 0 && (h->rate == 0 || h->rate == -2 || h->rate == 2))) continue;
            items.push_back({h->label, g_commands[h->command].text, &h->samples});
        }
        for (const auto& s : spoken) {
            if (!s.held_out || s.family == 0 || s.family == 3) continue; // Mark's plain sentences are in voice_sentences.bin; synthesised ones are made by the test.
            std::string means = s.wake ? std::string(lexicon::kWakeText) : std::string{};
            if (s.means) means += (means.empty() ? "" : " ") + std::string(s.means);
            items.push_back({s.label, means, &s.samples});
        }
        std::fwrite("MVS1", 1, 4, wake); Put32(wake, static_cast<std::uint32_t>(items.size()));
        double seconds = 0;
        for (const auto& item : items) {
            Put32(wake, static_cast<std::uint32_t>(item.label.size())); std::fwrite(item.label.data(), 1, item.label.size(), wake);
            Put32(wake, static_cast<std::uint32_t>(item.means.size())); std::fwrite(item.means.data(), 1, item.means.size(), wake);
            PutSamples(wake, *item.samples);
            seconds += static_cast<double>(item.samples->size()) / 16000.0;
        }
        std::fclose(wake);
        std::printf("Wake corpus: %zu held-out utterances, %.1f s\n", items.size(), seconds);
    }
    // ---- Write the held-out corpus: the held-out voices' commands of the first set, and every impostor word ----
    FILE* corpus = nullptr; _wfopen_s(&corpus, (root + L"\\tests\\data\\voice_corpus.bin").c_str(), L"wb");
    if (!corpus) { std::puts("FAIL cannot write the corpus (tests/data must exist)"); return 1; }
    std::vector<const Rendition*> listed;
    for (const auto* r : held) if (!r->synth && r->command < kBaseCommands) listed.push_back(r);
    for (const auto* r : impostors) listed.push_back(r);
    std::fwrite("MVC1", 1, 4, corpus); Put32(corpus, static_cast<std::uint32_t>(listed.size()));
    std::size_t corpus_samples = 0;
    for (const auto* r : listed) {
        Put32(corpus, static_cast<std::uint32_t>(r->command)); Put32(corpus, static_cast<std::uint32_t>(r->label.size()));
        std::fwrite(r->label.data(), 1, r->label.size(), corpus);
        PutSamples(corpus, r->samples);
        corpus_samples += r->samples.size();
    }
    std::fclose(corpus);
    std::printf("Corpus: %zu utterances, %.1f s\n", listed.size(), static_cast<double>(corpus_samples) / 16000.0);
    for (auto& v : voices) v.token->Release();
    CoUninitialize();
    return 0;
}
