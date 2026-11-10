#pragma once
#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/FormantSynthesizer.hpp"
#include "media/core/linear_memory_arena.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace media::ai {
// Spelling to sound for a name (docs/voice.md, "Custom wake words"): a deterministic letter-to-phoneme reading of a
// UTF-8 word or words, in FormantSynthesizer notation. No dictionary: a few dozen rules, enough for a name to be said
// the way most people would read it.
// - Latin letters (any case) are read as English: digraphs first (ch sh th ph ck qu ng wh, ee oo ou ai ay ...), then a
//   vowel is long in an open syllable (one consonant and a vowel after it, or a silent final e: "Mike") and short in a
//   closed one; "r" after a vowel colours it (ar, or, er/ir/ur); c and g soften before e, i, y. The first vowel of each
//   word is stressed; an unstressed short vowel reduces to a schwa (i to I).
// - Hebrew letters are read as Israeli Hebrew without points: ו and י are vowels inside a word (o, i) and consonants at
//   its start; ב כ פ are stops at the start of a word and fricatives after a vowel; a vowel "a" goes between two
//   consonants with none written; the last vowel is stressed.
// `variant` 1 is a second reading (short vowels kept full, the stressed short "a" as in "father"), for templates that
// cover both. Returns the bytes written (NUL-terminated), 0 for a word with a character it cannot read (digits,
// other scripts), no vowel, or one that does not fit.
[[nodiscard]] std::size_t SpellPhonemes(std::string_view utf8, unsigned variant, char* out, std::size_t capacity) noexcept;

// Another speech synthesiser's renditions of a name: a host's text-to-speech (the SDK's media_speak_callback), or on
// Windows the system's voices (platform::windows::SapiSpeech). `speak` writes `text` said by its voice number `voice` (0 to
// voices - 1) at `rate` (-1 slower, 0, +1 faster) into `out` as 16 kHz mono samples and returns how many (0: none).
struct SpeechSource {
    std::size_t (*speak)(void* context, const char* text, unsigned voice, int rate, float* out, std::size_t capacity) noexcept = nullptr;
    void* context = nullptr;
    unsigned voices = 0;
};
// A custom wake word's acoustic templates (the SDK's MEDIA_WAKE_CUSTOM made hearable). Two kinds of rendition:
// - the formant synthesiser's, everywhere: the name read by SpellPhonemes, said by kSpeakers virtual speakers (pitch
//   92-240 Hz, vocal tract 0.94-1.20, three in rooms) at kRates rates in both readings;
// - a SpeechSource's, when there is one: each of its voices (at most kMaxSourceVoices) at three rates. Measured
//   (docs/voice.md): formant renditions alone are heard from synthetic voices but hardly from real ones (a real "Falcon"
//   is further from them than a real "Avion" is); real voices' renditions are what makes the name hearable from people.
// Each rendition is described by the recogniser's own analysis (Describe) and quantised like the shipped templates;
// AcousticIntentRecognizer::Configure takes them in place of the built-in "avion". Everything comes from the arena
// given to Synthesize (about 0.5 MB, scratch included); nothing else allocates (a source may, inside its own call). Setup
// only (Prepare, before the arena is sealed): a formant rendition takes about a millisecond.
class CustomWakeWord final {
public:
    static constexpr unsigned kSpeakers = 6, kVariants = 2, kRates = 3, kMaxSourceVoices = 8;
    static constexpr unsigned kMaxTemplates = kSpeakers * kVariants * kRates + kMaxSourceVoices * 3;
    static constexpr std::size_t kMaxText = 31; // AvionWakeGate::kMaxWord.
    // False when the word cannot be read (SpellPhonemes), is too long or too short to be a template, or the arena is
    // exhausted; then Templates() is empty. `source` (optional): renditions by its voices too.
    [[nodiscard]] bool Synthesize(core::LinearMemoryArena& arena, std::string_view word, float scale, const SpeechSource* source = nullptr) noexcept;
    [[nodiscard]] unsigned SourceTemplates() const noexcept { return source_count_; } // Of Templates(), the source's.
    [[nodiscard]] std::span<const VoiceTemplate> Templates() const noexcept { return {templates_, count_}; }
    // The name as the recogniser writes it and the wake gate reads it: trimmed, ASCII letters in lower case.
    [[nodiscard]] const char* Text() const noexcept { return text_; }
    [[nodiscard]] const char* Phonemes(unsigned variant) const noexcept { return variant < kVariants ? phonemes_[variant] : ""; }
    // Whether `word` can be made hearable (SpellPhonemes reads it, and it is not too long).
    [[nodiscard]] static bool Readable(std::string_view word) noexcept;
    [[nodiscard]] static const SynthVoice& Speaker(unsigned i) noexcept;
private:
    VoiceTemplate templates_[kMaxTemplates]{};
    unsigned count_ = 0, source_count_ = 0;
    char text_[kMaxText + 1]{};
    char spoken_[kMaxText + 1]{}; // The name as given (trimmed), for a source to say.
    char phonemes_[kVariants][256]{};
};
}
