#pragma once
#include "media/ai/AvionWakeGate.hpp"
#include "media/ai/UtteranceBuffer.hpp"
#include "media/audio/FilterbankFeatures.hpp"
#include "media/audio/MicroVad.hpp"
#include "media/core/linear_memory_arena.hpp"
#include <cstdint>
#include <span>

namespace media::ai {
// One recorded rendition of a command: `frames` vectors of kDimensions signed bytes (value x VoiceTemplateSet::scale).
struct VoiceTemplate {
    const char* text;            // The command as typed (UTF-8): what LocalIntentDispatcher parses ("" for a filler).
    std::uint16_t command;       // Renditions of the same command share this number; kFiller: a word that is no command.
    std::uint16_t frames;
    const std::int8_t* features; // frames x AcousticIntentRecognizer::kDimensions.
    bool running = false;        // Cut from running speech (embedded training): matched by Decode only, never alone.
    // Languages it belongs to, one bit each (kEnglish, kHebrew; the wake word all). A sentence is decoded once per
    // language with that language's templates only, and the cheaper decoding is taken: a Hebrew word never competes
    // inside an English sentence. Isolated words are matched against every template.
    std::uint8_t languages = 1;
};
inline constexpr std::uint8_t kEnglish = 1, kHebrew = 2, kAllLanguages = 0xFF;
// Fillers: ordinary words that are no command. An utterance nearer to a filler than to every command is none.
inline constexpr std::uint16_t kFiller = 0xFFFF;
struct VoiceTemplateSet {
    std::span<const VoiceTemplate> templates;
    float scale = 1;             // Feature value of one quantisation step.
    float accept_distance = 0;   // An utterance further than this from every template is no command.
    float margin = 0;            // The best command must beat the best other command by this fraction of its distance.
    // Connected words (Decode): the cost of one frame of speech that is no known word, and of starting a word. 0: the
    // set recognises one word per utterance only. A command word counts only when its mean cost per frame is at most
    // `word_limit` (0: any), and an utterance is decoded only when the isolated match refused it and it has at least
    // `sentence_frames` frames of speech.
    float garbage = 0, word_penalty = 0, word_limit = 0;
    std::uint32_t sentence_frames = 0;
    // Per command number: its typical mean cost per frame when decoded right in running speech. A command word then
    // counts only at most `word_limit` times its own typical cost (some words always match worse than others); without
    // the table `word_limit` is an absolute cost.
    std::span<const float> typical{};
    // The wake word (docs/voice.md, "Wake word"): the command number of its templates (-1: none), and the distance a word
    // said alone must be within to wake (0: accept_distance). Its text is what the recogniser writes first ("avion",
    // or "avion pause the movie" said in one breath): AvionWakeGate reads it there.
    std::int32_t wake = -1;
    float wake_distance = 0;
    // Decode's beam: a path costlier than the frame's cheapest by more than this is dropped (0: none). Only a fixed
    // garbage cost (garbage < 0) is unchanged by it; the online one is the mean of the nearest frames it evaluates.
    float beam = 0;
    // Language lanes (docs/voice.md, "Language lanes"): a sentence is decoded in each language's templates alone, the
    // cheapest decoding wins, and then that language's own word limit and sentence floor decide (indexed by language
    // bit: English 0, Hebrew 1; negative: word_limit, sentence_frames). Each language's are tuned on its own speech.
    float lane_word_limit[2]{-1, -1};
    std::int32_t lane_sentence_frames[2]{-1, -1};
};
// A packed template set (docs/voice.md, "Packed templates"): what the shipped header holds. A template is a word, a
// length, and the cepstra of some of its frames, each value one of 2^bits levels of its coefficient; the frames between
// two kept ones lie on the straight line between them, and the deltas are made from the cepstra (Deltas). Configure
// rebuilds the frames in the arena as a VoiceTemplateSet's (rounded to `settings.scale`); recognition is then the same.
struct VoiceWord { const char* text; std::uint16_t command; std::uint8_t languages; }; // A filler: "" and kFiller.
struct VoicePacked { std::uint16_t word; std::uint8_t frames; std::uint8_t running; };   // Its bits follow the previous template's.
struct VoicePackedSet {
    std::span<const VoiceWord> words;
    std::span<const VoicePacked> templates;
    std::span<const std::uint8_t> keys;   // One bit per template frame (least significant first, templates in order): kept.
    std::span<const std::uint8_t> values; // `bits` bits per cepstrum of every kept frame, in the same order.
    std::span<const float> levels;        // kCepstra rows of 2^bits values: what each code of a coefficient stands for.
    std::uint32_t bits = 0;
    VoiceTemplateSet settings;            // All but `templates`.
};
// A word found in an utterance by Decode: its template, its frames [begin, end] (10 ms each), and its mean cost per frame.
struct SpottedWord { std::uint16_t template_index = 0; std::uint16_t begin = 0, end = 0; float cost = 0; };
enum class AcousticMatch : std::uint8_t {
    None,       // Nothing heard yet.
    Accepted,   // The command's text was written.
    TooShort,   // Less speech than a word.
    TooLong,    // More than the longest utterance held: not a command word.
    Distant,    // Nearest template beyond accept_distance.
    Ambiguous,  // Two commands (or a command and a filler) about equally near.
    Filler,     // Nearest to a word that is no command.
    Partial,    // Nearest to a word that is a command only with others ("turn", "ten", "seconds"), said alone.
};
struct AcousticResult {
    AcousticMatch match = AcousticMatch::None;
    std::int32_t command = -1, runner_up = -1;   // Best and best other template `command` number (kFiller included).
    float distance = 0, runner_up_distance = 0;  // Their distances (per aligned frame).
    std::uint32_t frames = 0;                    // Speech frames matched (after trimming), 10 ms each.
    const char* text = nullptr;                  // The best command's text, accepted or not; a sentence's command words.
    bool sentence = false;                       // Accepted from connected words (Decode), not as one word.
    unsigned words = 0;                          // Sentence: command words decoded (before Phrase chose among them).
    bool wake = false;                           // The utterance began with the wake word (`text` starts with it).
    bool prefix_resolved = false;                // A prefix pair ("mute"/"unmute") was decided on the words' onsets.
};

// The acoustic end of the voice path (docs/voice.md): the speech of one utterance in, a command's text out.
//
//   VoiceGate -> AcousticIntentRecognizer::Hear -> UtteranceBuffer -> LocalIntentDispatcher -> PlaybackCoordinator::Command
//
// It recognises a small closed set of command words by matching against recorded renditions (templates), not by
// modelling speech: no phonemes, no language model, no dictation.
// 1. While the utterance arrives, FilterbankFeatures turns it into 24 log-mel energies per 10 ms.
// 2. At its end the frames are trimmed to the speech (within 22 dB of the loudest frame; the detector's pre-roll and
//    hangover are silence), floored 20 dB below that frame (noise and the quiet ends of words look alike), reduced to
//    6 cepstral coefficients (the spectral envelope: higher coefficients carried more of the speaker than of the
//    word), normalised over the utterance to zero mean and equal variance (microphone, level and much of the speaker
//    drop out), and given 6 delta coefficients: kDimensions = 12 per frame. These choices were compared on how often
//    a template's nearest rendition by another speaker is the same word (docs/voice.md).
// 3. Dynamic time warping aligns the utterance with every template (rate of speech drops out) inside a band around
//    the diagonal; the distance is the mean Euclidean distance per aligned step.
// 4. The nearest command is written to the utterance's text when it is near enough (accept_distance) and nearer
//    than any other command and than every filler (templates of ordinary words that are no command) by the margin,
//    and the grammar reads that word alone as a command ("turn" or "ten" said alone is Partial).
//    Otherwise nothing is written, and VoiceGate hands nothing on.
// Storage comes from the arena at Configure; after that nothing allocates. One owner thread (VoiceGate's).
class AcousticIntentRecognizer final {
public:
    static constexpr std::uint32_t kCepstra = 6, kDimensions = 2 * kCepstra;
    static constexpr std::uint32_t kMaxFrames = 600;  // 6 s of utterance (a spoken sentence), pre-roll and hangover included.
    static constexpr std::uint32_t kMaxTemplateFrames = 255; // A template is one word.
    static constexpr std::uint32_t kMinTemplateFrames = 6;   // 60 ms: a short word as said inside a sentence.
    static constexpr unsigned kMaxWords = 16;         // Words Decode reports per utterance.
    static constexpr std::uint32_t kMinFrames = 12;   // 120 ms of speech after trimming.
    [[nodiscard]] bool Configure(core::LinearMemoryArena&, const VoiceTemplateSet&) noexcept;
    [[nodiscard]] bool Configure(core::LinearMemoryArena&, const VoicePackedSet&) noexcept;
    // A custom wake word (docs/voice.md, "Custom wake words"): the packed set with `wake` (CustomWakeWord's templates, one
    // text) as its wake word. The built-in name's renditions become fillers, so "avion" is heard and refused, not taken
    // for the name; the wake distance and the in-sentence cost limits are the set's. An empty `wake`: the set as it is.
    // A custom name has no in-context renditions (nothing was cut from sentences, so its in-sentence cost is not known):
    // when a sentence decoding begins with it, its span must also pass as the name said alone (VerifyWake).
    [[nodiscard]] bool Configure(core::LinearMemoryArena&, const VoicePackedSet&, std::span<const VoiceTemplate> wake) noexcept;
    [[nodiscard]] bool CustomWake() const noexcept { return custom_wake_; }
    // VoiceGate's recogniser callback (`context` is the recogniser): SpeechStart begins an utterance, the slices are
    // analysed as they come, SpeechEnd matches and writes the command's text, or nothing.
    static void Hear(void* context, const audio::SpeechSlice&, UtteranceBuffer& text) noexcept;
    // The listening window (docs/voice.md, "Two steps"): after the name alone, an utterance that begins while `gate`'s
    // window is open is a command addressed to the player, so a sentence is accepted from `frames` of speech in every lane
    // (at most: a lane's own floor may be lower). That is the English lane's floor; it lets two-word Hebrew commands
    // ("פרק הבא") through after "Avion", without lowering the floor of speech nobody addressed to the player. Hear asks
    // the gate at each utterance's start; 0 frames or no gate: the lanes' floors only.
    // In the window the utterance is taken as addressed to the player, so room talk said there is the risk: a sentence
    // decoded there counts only when every command word it holds belongs to the one command (no "pause ... go"), and not
    // when that command is a direction word alone ("אחורה", "back": said alone, it is matched as a word, not a sentence).
    static constexpr std::uint32_t kWindowSentenceFrames = 70;
    void FollowWake(const AvionWakeGate* gate, std::uint32_t frames = kWindowSentenceFrames) noexcept { wake_gate_ = gate; window_frames_ = frames; }
    void SetListening(bool open) noexcept { listening_ = open; } // By hand (with Begin): the window is open for this utterance.
    void SetWindowRules(bool on) noexcept { window_rules_ = on; } // Tests: the window's sentence rules off, for comparison.
    // The same by hand: Begin, Feed any number of times, then Finish.
    void Begin() noexcept;
    void Feed(std::span<const float> samples) noexcept;
    AcousticResult Finish() noexcept;
    [[nodiscard]] const AcousticResult& Last() const noexcept { return last_; }
    // The last utterance's connected-word decoding (its words in order, fillers and the wake word included; empty when it
    // was not decoded), and a template's text ("" for a filler).
    [[nodiscard]] std::span<const SpottedWord> LastDecoding() const noexcept { return {decoded_, decoded_count_}; }
    [[nodiscard]] const char* TemplateText(std::uint16_t template_index) const noexcept { return template_index < set_.templates.size() ? set_.templates[template_index].text : ""; }
    // The matching features of a whole utterance (steps 1 and 2): `out` holds kMaxFrames x kDimensions values; returns
    // the frames written, 0 when there is less speech than kMinFrames or more audio than kMaxFrames. `scratch` holds
    // kMaxFrames x FilterbankFeatures::kChannels values. What the template tool stores is this, quantised.
    [[nodiscard]] static std::uint32_t Describe(std::span<const float> samples, float* scratch, float* out) noexcept;
    // The delta coefficients of `frames` feature vectors from their cepstra (the last kCepstra values of each are
    // rewritten): how Describe makes them, and how a compact set's templates get theirs back.
    static void Deltas(float* features, std::uint32_t frames) noexcept;
    // Distance between two feature sequences (step 3); a negative value when their lengths cannot be aligned.
    // `abandon` > 0: the alignment stops as soon as it can no longer end below that distance, and returns a value at
    // least `abandon` (a lower bound of the distance, not the distance).
    [[nodiscard]] static float Distance(const float* a, std::uint32_t a_frames, const float* b, std::uint32_t b_frames, float* row_a, float* row_b, float abandon = 0) noexcept;
    // The same over the first kPrefixShare of each (at least kMinFrames / 2 frames): how two words that differ only in
    // their beginning ("mute" and "unmute") are told apart.
    static constexpr float kPrefixShare = 0.4f, kPrefixMargin = 0.15f;
    [[nodiscard]] static float PrefixDistance(const float* a, std::uint32_t a_frames, const float* b, std::uint32_t b_frames, float* row_a, float* row_b) noexcept;
    // Prefix pairs: two commands of which one's text is the other's with something before it ("unmute" = "un" +
    // "mute"). Found at Configure from the template texts.
    static constexpr unsigned kMaxPairs = 8, kMaxCommands = 128;
    [[nodiscard]] unsigned PrefixPairs() const noexcept { return pair_count_; }
    // Connected-word decoding (one pass, frame-synchronous): the utterance as the cheapest sequence of template words
    // and stretches of "garbage" (speech or noise that is no known word, `garbage` per frame), each word entered at
    // `penalty`. A word's template is warped by DTW steps (repeat, advance, skip one: a word between half and twice its
    // recorded length). `blocks`: the dequantised template features; `scratch`: DecodeScratch(templates) floats.
    // Returns the words found in order (at most `capacity`), fillers included.
    // The command in the words a decoding found (their texts, in order; fillers left out): repeats merged ("up up"),
    // then the first span the grammar reads as a whole command, from the earliest word on and the longest first
    // ("pause up" -> "pause"). A span that would leave out a number or a unit is never taken (the amount would
    // change). Writes it to `out` (NUL-terminated); returns its bytes, 0 for none.
    [[nodiscard]] static std::size_t Phrase(std::span<const char* const> words, char* out, std::size_t capacity) noexcept;
    // Whether a word carries an amount (a number or a unit: leaving it out would change what the command does).
    // Decode's words dropped by the cost limit are checked too: a sentence that lost one is refused, not shortened.
    [[nodiscard]] static bool CarriesAmount(const char* word) noexcept;
    // The words (repeats merged) read as one whole command; a command that is a direction word alone.
    [[nodiscard]] static bool WholeCommand(std::span<const char* const> words) noexcept;
    [[nodiscard]] static bool DirectionAlone(const char* text) noexcept;
    [[nodiscard]] static std::size_t DecodeScratch(std::span<const VoiceTemplate> templates) noexcept;
    // `languages`: only templates of these (VoiceTemplate::languages); `cost`: the decoding's total cost per frame.
    [[nodiscard]] static unsigned Decode(const float* features, std::uint32_t frames, std::span<const VoiceTemplate> templates, const float* const* blocks,
                                         float garbage, float penalty, float* scratch, SpottedWord* out, unsigned capacity,
                                         std::uint8_t languages = kAllLanguages, float* cost = nullptr, float beam = 0) noexcept;
    // Decode once per language the templates have, keeping the cheapest decoding (`language`: its bit).
    [[nodiscard]] static unsigned DecodeBest(const float* features, std::uint32_t frames, std::span<const VoiceTemplate> templates, const float* const* blocks,
                                             float garbage, float penalty, float* scratch, SpottedWord* out, unsigned capacity, std::uint8_t* language = nullptr,
                                             float beam = 0) noexcept;
    // Utterances that may be the wake word and a short command ("avion stop") are decoded from this many frames,
    // below sentence_frames; such a decoding counts only when it begins with the wake word.
    // A decoding that begins with the wake word counts only with a command after it: the name alone is the isolated
    // matcher's to decide (its acceptance distance and margin), never the sentence decoder's.
    static constexpr std::uint32_t kWakeSentenceFrames = 60;
    // window: sentences accepted only thanks to the listening window's floor; window_refused: sentences the window's rules
    // refused (Finish: command words left over, or a direction word alone).
    struct Statistics { std::uint32_t utterances = 0, accepted = 0, too_short = 0, too_long = 0, distant = 0, ambiguous = 0, filler = 0, partial = 0, sentences = 0, wakes = 0, window = 0,
                        window_refused = 0; };
    [[nodiscard]] const Statistics& Stats() const noexcept { return stats_; }
    // Tools and tests: other connected-word settings than the set's (see VoiceTemplateSet).
    void SetDecoding(float garbage, float word_penalty, float word_limit, std::uint32_t sentence_frames, std::span<const float> typical = {}) noexcept {
        set_.garbage = garbage; set_.word_penalty = word_penalty; set_.word_limit = word_limit; set_.sentence_frames = sentence_frames; set_.typical = typical;
    }
    void SetBeam(float beam) noexcept { set_.beam = beam; }
    void SetLane(unsigned lane, float word_limit, std::int32_t sentence_frames) noexcept {
        if (lane < 2) { set_.lane_word_limit[lane] = word_limit; set_.lane_sentence_frames[lane] = sentence_frames; }
    }
    // A language lane's word limit and sentence floor (VoiceTemplateSet::lane_word_limit, lane_sentence_frames).
    [[nodiscard]] float LaneWordLimit(unsigned lane) const noexcept { return lane < 2 && set_.lane_word_limit[lane] >= 0 ? set_.lane_word_limit[lane] : set_.word_limit; }
    [[nodiscard]] std::uint32_t LaneSentenceFrames(unsigned lane) const noexcept {
        return lane < 2 && set_.lane_sentence_frames[lane] >= 0 ? static_cast<std::uint32_t>(set_.lane_sentence_frames[lane]) : set_.sentence_frames;
    }
private:
    static std::uint32_t Reduce(const float* logmel, std::uint32_t frames, float* out) noexcept;
    // The partner of `command` in a prefix pair, or -1.
    [[nodiscard]] std::int32_t Partner(std::int32_t command) const noexcept;
    // Of a prefix pair, which one `features` are (`running`: in-context templates too), or -1 for neither for sure.
    [[nodiscard]] std::int32_t ResolvePrefix(const float* features, std::uint32_t frames, std::int32_t a, std::int32_t b, bool running) noexcept;
    [[nodiscard]] const char* TextOf(std::int32_t command) const noexcept;
    std::int32_t pairs_[kMaxPairs][2]{};
    unsigned pair_count_ = 0;
    VoiceTemplateSet set_{};
    const float* const* templates_ = nullptr; // Dequantised features, one block per template.
    float* logmel_ = nullptr;   // kMaxFrames x kChannels.
    float* features_ = nullptr; // kMaxFrames x kDimensions.
    float* rows_ = nullptr;     // Two DTW rows of kMaxFrames + 1.
    float* decode_ = nullptr;   // DecodeScratch floats.
    char spoken_[512]{};        // A sentence's command (Phrase), after the wake word when there is one.
    audio::FilterbankFeatures filterbank_;
    std::uint32_t frames_ = 0;
    bool overflowed_ = false;
    const AvionWakeGate* wake_gate_ = nullptr;
    std::uint32_t window_frames_ = 0;
    bool listening_ = false; // The window was open when this utterance began.
    bool window_rules_ = true;
    bool custom_wake_ = false;
    // Whether frames [begin, end] of the utterance's features, matched alone against the isolated templates, are nearest
    // to the wake word, within its distance and by the margin (a custom name spotted in a sentence).
    [[nodiscard]] bool VerifyWake(std::uint32_t begin, std::uint32_t end) noexcept;
    AcousticResult last_{};
    SpottedWord decoded_[kMaxWords]{}; unsigned decoded_count_ = 0;
    Statistics stats_{};
};
}
