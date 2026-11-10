#include "media/ai/CustomWakeWord.hpp"
#include "media/ai/VoiceLexicon.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace media::ai {
namespace {
// One sound of a word being read: a phoneme string ("tS", "e I") and what the vowel rules need to know about it.
struct Unit {
    char phonemes[8]{};
    char letter = 0;        // A vowel unit from one letter (a e i o u y): its quality is decided by the syllable.
    bool vowel = false;
    bool weight = false;    // A consonant that closes a syllable by itself (a doubled letter, x, ck): "Anna", "Max".
    bool coloured = false;  // A vowel before "r" that closes its syllable (ar, or, er, ir, ur).
};
constexpr unsigned kMaxUnits = 48;
struct Units {
    Unit u[kMaxUnits]{}; unsigned n = 0; bool overflow = false;
    Unit* Add(const char* p, bool vowel = false, char letter = 0) noexcept {
        if (n == kMaxUnits) { overflow = true; return nullptr; }
        auto& x = u[n++]; x = {}; std::size_t i = 0;
        for (; p[i] && i + 1 < sizeof(x.phonemes); ++i) x.phonemes[i] = p[i];
        x.phonemes[i] = 0; x.vowel = vowel; x.letter = letter;
        return &x;
    }
};
// Copies a NUL-terminated string into a fixed array, cut to fit.
template <std::size_t N> void Copy(char (&to)[N], const char* from, std::size_t at = 0) noexcept {
    std::size_t i = at; for (; *from && i + 1 < N; ++i) to[i] = *from++;
    to[i] = 0;
}
bool IsVowelLetter(char c) noexcept { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y'; }
bool Soft(char c) noexcept { return c == 'e' || c == 'i' || c == 'y'; }
// Appends `token` (and a space before it when `out` is not empty); false when it does not fit.
bool Put(char* out, std::size_t capacity, std::size_t& size, std::string_view token) noexcept {
    if (size + (size ? 1 : 0) + token.size() + 1 > capacity) return false;
    if (size) out[size++] = ' ';
    std::memcpy(out + size, token.data(), token.size()); size += token.size(); out[size] = 0;
    return true;
}
// A word of lower-case ASCII letters, read as English.
bool ReadLatin(std::string_view w, unsigned variant, char* out, std::size_t capacity, std::size_t& size) noexcept {
    Units units;
    const auto at = [&](std::size_t i) { return i < w.size() ? w[i] : '\0'; };
    const auto starts = [&](std::size_t i, std::string_view s) { return w.substr(i, s.size()) == s; };
    // A final "e" after a consonant is silent when another vowel comes before it ("mike", not "be").
    std::size_t end = w.size();
    if (end >= 3 && w[end - 1] == 'e' && !IsVowelLetter(w[end - 2])) {
        for (std::size_t i = 0; i + 2 < end; ++i) if (IsVowelLetter(w[i]) && w[i] != 'y') { end = w.size() - 1; break; }
    }
    bool silent_e = end < w.size();
    for (std::size_t i = 0; i < end && !units.overflow;) {
        const char c = w[i], next = at(i + 1);
        // Vowel digraphs and trigraphs.
        if (starts(i, "iew") || starts(i, "eau")) { (void)units.Add(starts(i, "iew") ? "j u" : "o", true); i += 3; continue; }
        if (starts(i, "ee") || starts(i, "ea") || starts(i, "ie")) { (void)units.Add("i", true); i += 2; continue; }
        if (starts(i, "oo")) { (void)units.Add("u", true); i += 2; continue; }
        if (starts(i, "ou") || (starts(i, "ow") && !IsVowelLetter(at(i + 2)))) { (void)units.Add("a U", true); i += 2; continue; }
        if (starts(i, "ai") || starts(i, "ay") || starts(i, "ei") || starts(i, "ey")) { (void)units.Add("e I", true); i += 2; continue; }
        if (starts(i, "oa")) { (void)units.Add("o", true); i += 2; continue; }
        if (starts(i, "oi") || starts(i, "oy")) { (void)units.Add("O I", true); i += 2; continue; }
        if (starts(i, "au") || (starts(i, "aw") && !IsVowelLetter(at(i + 2)))) { (void)units.Add("O", true); i += 2; continue; }
        if (starts(i, "eu") || (starts(i, "ew") && !IsVowelLetter(at(i + 2))) || (starts(i, "ue") && i + 2 >= end)) { (void)units.Add("j u", true); i += 2; continue; }
        if (c == 'y' && i == 0 && IsVowelLetter(next)) { (void)units.Add("j"); ++i; continue; } // "y" before a vowel at the start.
        if (IsVowelLetter(c)) {
            // A vowel and an "r" that closes its syllable: one coloured vowel ("ar", "or", "er"), the r kept.
            if (next == 'r' && !IsVowelLetter(at(i + 2)) && at(i + 2) != 'r') {
                auto* v = units.Add(c == 'a' ? "a" : c == 'o' ? "O" : "@", true, c); if (v) v->coloured = true;
                (void)units.Add("r"); i += 2; continue;
            }
            (void)units.Add("", true, c); ++i; continue;
        }
        // Consonants: digraphs, then letters. A doubled consonant is one sound that closes the syllable before it.
        if (starts(i, "tch")) { auto* x = units.Add("tS"); if (x) x->weight = true; i += 3; continue; }
        if (starts(i, "ch")) { (void)units.Add("tS"); i += 2; continue; }
        if (starts(i, "sh")) { (void)units.Add("S"); i += 2; continue; }
        if (starts(i, "th")) { (void)units.Add("t"); i += 2; continue; }
        if (starts(i, "ph")) { (void)units.Add("f"); i += 2; continue; }
        if (starts(i, "ck")) { auto* x = units.Add("k"); if (x) x->weight = true; i += 2; continue; }
        if (starts(i, "qu")) { (void)units.Add("k"); (void)units.Add("w"); i += 2; continue; }
        if (starts(i, "wh")) { (void)units.Add("w"); i += 2; continue; }
        if (i == 0 && (starts(i, "kn") || starts(i, "wr"))) { (void)units.Add(c == 'k' ? "n" : "r"); i += 2; continue; }
        if (starts(i, "gh")) { if (i == 0) (void)units.Add("g"); i += 2; continue; }
        if (starts(i, "ng") && !IsVowelLetter(at(i + 2))) { (void)units.Add("N"); i += 2; continue; }
        if (starts(i, "sc") && Soft(at(i + 2))) { (void)units.Add("s"); i += 2; continue; }
        if (c == 'h' && i > 0 && i + 1 >= end) { ++i; continue; } // "sarah".
        if (c == 'x') { if (i == 0) (void)units.Add("z"); else { (void)units.Add("k"); auto* x = units.Add("s"); if (x) x->weight = true; } ++i; continue; }
        const char* p = nullptr;
        switch (c) {
        case 'b': p = "b"; break; case 'd': p = "d"; break; case 'f': p = "f"; break; case 'h': p = "h"; break;
        case 'k': p = "k"; break; case 'l': p = "l"; break; case 'm': p = "m"; break; case 'n': p = "n"; break;
        case 'p': p = "p"; break; case 'r': p = "r"; break; case 's': p = "s"; break; case 't': p = "t"; break;
        case 'v': p = "v"; break; case 'w': p = "w"; break; case 'z': p = "z"; break; case 'q': p = "k"; break;
        case 'j': p = "d Z"; break;
        case 'c': p = Soft(next) ? "s" : "k"; break;
        case 'g': p = Soft(next) ? "d Z" : "g"; break;
        default: return false;
        }
        auto* x = units.Add(p);
        if (next == c) { if (x) x->weight = true; i += 2; } else ++i;
    }
    if (units.overflow || !units.n) return false;
    // Vowel quality: long in an open syllable (a vowel, or one consonant and a vowel, after it; or one consonant and the
    // silent e), short in a closed one; a final vowel is its own (a schwa for "a").
    static constexpr struct { char letter; const char *open, *closed, *closed_other, *final; } kVowels[]{
        {'a', "e I", "ae", "a", "@"}, {'e', "i", "E", "E", "i"}, {'i', "a I", "I", "I", "i"},
        {'o', "o", "O", "a", "o"}, {'u', "u", "V", "V", "u"}, {'y', "a I", "I", "I", "i"}};
    int first_vowel = -1;
    for (unsigned k = 0; k < units.n; ++k) {
        auto& x = units.u[k];
        if (!x.vowel) continue;
        if (first_vowel < 0) first_vowel = static_cast<int>(k);
        if (!x.letter || x.coloured) continue;
        unsigned consonants = 0; bool vowel_after = false;
        for (unsigned j = k + 1; j < units.n; ++j) { if (units.u[j].vowel) { vowel_after = true; break; } consonants += units.u[j].weight ? 2 : 1; }
        const bool last = k + 1 == units.n;
        const bool open = (vowel_after && consonants <= 1) || (!vowel_after && silent_e && consonants == 1);
        for (const auto& v : kVowels) {
            if (v.letter != x.letter) continue;
            const char* p = last ? (k == static_cast<unsigned>(first_vowel) && x.letter == 'a' ? "a" : v.final) : open ? v.open : variant ? v.closed_other : v.closed;
            Copy(x.phonemes, p);
        }
    }
    if (first_vowel < 0) return false;
    // Stress on the first vowel; elsewhere a short vowel reduces (reading 0 only).
    for (unsigned k = 0; k < units.n; ++k) {
        auto& x = units.u[k];
        if (!x.vowel || static_cast<int>(k) == first_vowel || variant) continue;
        if (!std::strcmp(x.phonemes, "ae") || !std::strcmp(x.phonemes, "E") || !std::strcmp(x.phonemes, "O") || !std::strcmp(x.phonemes, "V")) Copy(x.phonemes, "@");
    }
    for (unsigned k = 0; k < units.n; ++k) {
        const auto& x = units.u[k];
        if (!x.phonemes[0]) continue;
        if (static_cast<int>(k) == first_vowel) {
            char stressed[10]{'\''}; Copy(stressed, x.phonemes, 1);
            if (!Put(out, capacity, size, stressed)) return false;
        } else if (!Put(out, capacity, size, x.phonemes)) return false;
    }
    return true;
}
// A word of Hebrew letters (U+05D0..U+05EA), read without points.
bool ReadHebrew(std::string_view w, char* out, std::size_t capacity, std::size_t& size) noexcept {
    // Letters as indices 0..26 (א..ת).
    unsigned letters[32]; unsigned count = 0;
    if (w.size() % 2) return false;
    for (std::size_t i = 0; i < w.size(); i += 2) {
        const auto lead = static_cast<unsigned char>(w[i]), trail = static_cast<unsigned char>(w[i + 1]);
        if (lead != 0xD7 || trail < 0x90 || trail > 0xAA || count == 32) return false;
        letters[count++] = trail - 0x90u;
    }
    enum : unsigned { Alef = 0, Bet = 1, He = 4, Vav = 5, Yod = 9, FinalKaf = 10, Kaf = 11, Ayin = 18, FinalPe = 19, Pe = 20 };
    static constexpr const char* kFixed[27]{"?", "b", "g", "d", "h", "v", "z", "X", "t", "j", "X", "k", "l", "m", "m", "n", "n", "s", "?",
                                            "f", "p", "ts", "ts", "k", "R", "S", "t"};
    Units units;
    for (unsigned k = 0; k < count && !units.overflow; ++k) {
        const unsigned l = letters[k];
        const bool start = k == 0, last = k + 1 == count;
        const bool after_vowel = units.n && units.u[units.n - 1].vowel;
        if (l == Vav || l == Yod) {
            if (k + 1 < count && letters[k + 1] == l) { (void)units.Add(l == Vav ? "v" : "j"); ++k; continue; } // וו, יי.
            if (start) (void)units.Add(l == Vav ? "v" : "j");
            else (void)units.Add(l == Vav ? "o" : "i", true);
            continue;
        }
        if (l == Alef || l == Ayin) { if (!(k + 1 < count && (letters[k + 1] == Vav || letters[k + 1] == Yod) && start)) (void)units.Add("a", true); continue; }
        if (l == He && last && !start) { (void)units.Add("a", true); continue; }
        const char* p = kFixed[l];
        if (l == Bet) p = start ? "b" : "v";
        if (l == Kaf || l == FinalKaf) p = start ? "k" : "X";
        if (l == Pe || l == FinalPe) p = start ? "p" : "f";
        // A consonant after a consonant: the vowel nobody wrote ("a").
        if (units.n && !after_vowel) (void)units.Add("a", true);
        (void)units.Add(p);
    }
    if (units.overflow || !units.n) return false;
    int stressed = -1;
    for (unsigned k = 0; k < units.n; ++k) if (units.u[k].vowel) stressed = static_cast<int>(k);
    if (stressed < 0) return false;
    for (unsigned k = 0; k < units.n; ++k) {
        const auto& x = units.u[k];
        if (static_cast<int>(k) == stressed) {
            char s[10]{'\''}; Copy(s, x.phonemes, 1);
            if (!Put(out, capacity, size, s)) return false;
        } else if (!Put(out, capacity, size, x.phonemes)) return false;
    }
    return true;
}
// The extra virtual speakers beside the lexicon's four template speakers (none is a held-out speaker's voice).
constexpr SynthVoice kExtraSpeakers[]{
    {.f0 = 150, .formant_scale = 1.06f, .breathiness = 0.09f, .open_quotient = 0.63f, .tilt = 0.35f, .reverb_seconds = 0.45f, .reverb_mix = 0.2f, .seed = 53},
    {.f0 = 175, .formant_scale = 1.10f, .breathiness = 0.07f, .open_quotient = 0.57f, .tilt = 0.50f, .seed = 67},
};
static_assert(std::size(voice_lexicon::kTemplateSpeakers) + std::size(kExtraSpeakers) == CustomWakeWord::kSpeakers);
static_assert(std::size(voice_lexicon::kTemplateRates) == CustomWakeWord::kRates);
// The name folded as the wake gate compares it: trimmed, ASCII letters in lower case. False when blank or too long.
bool Fold(std::string_view word, char* out, std::size_t capacity) noexcept {
    std::size_t first = 0, last = word.size();
    while (first < last && (word[first] == ' ' || word[first] == '\t')) ++first;
    while (last > first && (word[last - 1] == ' ' || word[last - 1] == '\t')) --last;
    if (first == last || last - first + 1 > capacity) return false;
    for (std::size_t i = first; i < last; ++i) { const char c = word[i]; out[i - first] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
    out[last - first] = 0;
    return true;
}
}

std::size_t SpellPhonemes(std::string_view utf8, unsigned variant, char* out, std::size_t capacity) noexcept {
    if (!out || capacity < 2) return 0;
    char folded[64];
    if (!Fold(utf8, folded, sizeof(folded))) return 0;
    std::string_view rest{folded};
    std::size_t size = 0; out[0] = 0;
    while (!rest.empty()) {
        // Words are separated by spaces and hyphens; apostrophes inside a Latin name are dropped ("O'Neil").
        const auto split = rest.find_first_of(" \t-");
        const auto token = rest.substr(0, split);
        rest = split == std::string_view::npos ? std::string_view{} : rest.substr(split + 1);
        if (token.empty()) continue;
        char latin[64]; std::size_t n = 0; bool hebrew = false, other = false;
        for (const char ch : token) {
            const auto c = static_cast<unsigned char>(ch);
            if (c >= 'a' && c <= 'z') latin[n++] = static_cast<char>(c);
            else if (c == '\'') continue;
            else if (c >= 0x80) hebrew = true;
            else other = true;
        }
        if (other || (hebrew && n)) return 0;
        if (size && !Put(out, capacity, size, "|")) return 0;
        if (hebrew ? !ReadHebrew(token, out, capacity, size) : !ReadLatin({latin, n}, variant, out, capacity, size)) return 0;
    }
    return size;
}
const SynthVoice& CustomWakeWord::Speaker(unsigned i) noexcept {
    constexpr unsigned base = static_cast<unsigned>(std::size(voice_lexicon::kTemplateSpeakers));
    return i < base ? voice_lexicon::kTemplateSpeakers[i].voice : kExtraSpeakers[(i - base) % std::size(kExtraSpeakers)];
}
bool CustomWakeWord::Readable(std::string_view word) noexcept {
    char folded[kMaxText + 1], phonemes[256];
    if (!Fold(word, folded, sizeof(folded)) || !SpellPhonemes(folded, 0, phonemes, sizeof(phonemes))) return false;
    std::string_view rest{phonemes};
    while (!rest.empty()) { // Every token one the synthesiser says.
        const auto space = rest.find(' ');
        auto token = rest.substr(0, space);
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        if (!token.empty() && token[0] == '\'') token.remove_prefix(1);
        if (token != "|" && !FormantSynthesizer::Known(token)) return false;
    }
    return true;
}
bool CustomWakeWord::Synthesize(core::LinearMemoryArena& arena, std::string_view word, float scale, const SpeechSource* source) noexcept {
    count_ = source_count_ = 0;
    if (!(scale > 0) || !Fold(word, text_, sizeof(text_))) return false;
    {
        std::size_t first = 0, last = word.size();
        while (first < last && (word[first] == ' ' || word[first] == '\t')) ++first;
        while (last > first && (word[last - 1] == ' ' || word[last - 1] == '\t')) --last;
        std::memcpy(spoken_, word.data() + first, last - first); spoken_[last - first] = 0; // Fold checked the length.
    }
    unsigned variants = 0;
    for (unsigned v = 0; v < kVariants; ++v) {
        if (!SpellPhonemes(text_, v, phonemes_[v], sizeof(phonemes_[v]))) { phonemes_[v][0] = 0; continue; }
        if (v && !std::strcmp(phonemes_[v], phonemes_[0])) { phonemes_[v][0] = 0; continue; } // The same reading twice.
        ++variants;
    }
    if (!phonemes_[0][0]) return false;
    // Scratch: the synthesiser (its reverberator included), one rendition, and Describe's two buffers.
    constexpr std::size_t kMaxSamples = 6 * FormantSynthesizer::kSampleRate;
    auto* synth = arena.Create<FormantSynthesizer>();
    auto* samples = static_cast<float*>(arena.Allocate(sizeof(float) * kMaxSamples));
    auto* logmel = static_cast<float*>(arena.Allocate(sizeof(float) * AcousticIntentRecognizer::kMaxFrames * audio::FilterbankFeatures::kChannels));
    auto* features = static_cast<float*>(arena.Allocate(sizeof(float) * AcousticIntentRecognizer::kMaxFrames * AcousticIntentRecognizer::kDimensions));
    if (!synth || !samples || !logmel || !features) return false;
    // One rendition in `samples` -> one template (or none, when it is no word's length).
    const auto keep = [&](std::size_t n) {
        if (!n || count_ == kMaxTemplates) return true;
        const auto frames = AcousticIntentRecognizer::Describe({samples, n}, logmel, features);
        if (frames < AcousticIntentRecognizer::kMinTemplateFrames || frames > AcousticIntentRecognizer::kMaxTemplateFrames) return true;
        const std::size_t values = static_cast<std::size_t>(frames) * AcousticIntentRecognizer::kDimensions;
        auto* q = static_cast<std::int8_t*>(arena.Allocate(values, 1));
        if (!q) return false;
        for (std::size_t i = 0; i < values; ++i) q[i] = static_cast<std::int8_t>(std::lround((std::clamp)(features[i] / scale, -127.0f, 127.0f)));
        templates_[count_++] = VoiceTemplate{text_, 0, static_cast<std::uint16_t>(frames), q, false, kAllLanguages};
        return true;
    };
    for (unsigned v = 0; v < kVariants; ++v) {
        if (!phonemes_[v][0]) continue;
        for (unsigned s = 0; s < kSpeakers; ++s)
            for (unsigned r = 0; r < kRates; ++r) {
                SynthVoice voice = Speaker(s);
                voice.rate = voice_lexicon::kTemplateRates[r]; voice.seed += 7u * r + 131u * v;
                if (!keep(synth->Speak(phonemes_[v], voice, {samples, kMaxSamples}))) return false;
            }
    }
    if (source && source->speak) {
        const unsigned formant = count_;
        for (unsigned voice = 0; voice < (std::min)(source->voices, kMaxSourceVoices); ++voice)
            for (int rate = -1; rate <= 1; ++rate)
                if (!keep(source->speak(source->context, spoken_, voice, rate, samples, kMaxSamples))) return false;
        source_count_ = count_ - formant;
    }
    (void)variants;
    return count_ > 0;
}
}
