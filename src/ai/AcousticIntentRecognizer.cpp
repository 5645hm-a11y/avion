#include "media/ai/AcousticIntentRecognizer.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define MEDIA_VOICE_SSE 1
#endif

namespace media::ai {
namespace {
using audio::FilterbankFeatures;
constexpr std::uint32_t kChannels = FilterbankFeatures::kChannels;
constexpr double kPi = 3.14159265358979323846;
// Frames whose mean log energy is within this of the loudest frame are speech; the rest, at both ends, is trimmed.
// 5 in natural-log power is 22 dB: below a word's weakest consonant, above the detector's pre-roll and hangover.
constexpr float kTrimBelowPeak = 5.0f;
constexpr std::uint32_t kTrimPad = 2; // Frames kept either side of the first and last speech frame.
// Every channel is floored 20 dB (4.6 natural-log units) below the loudest frame's mean level: what is quieter than
// that (noise, the tails of the word) is not matched on.
constexpr float kFloorBelowPeak = 4.6051702f;
constexpr float kDeltaWeight = 0.6f;   // Deltas weigh less than the coefficients themselves: they are noisier.
constexpr float kNormalizedScale = 4;  // Each coefficient's standard deviation over the utterance after normalisation.
// Normalisation window (frames either side) for utterances longer than it; 0: the whole utterance always.
constexpr std::uint32_t kNormalizationWindow = 35;
struct Tables {
    float dct[AcousticIntentRecognizer::kCepstra][kChannels];
    Tables() noexcept {
        // DCT-II rows 1..kCepstra (row 0 is the level, which is not matched).
        for (unsigned k = 0; k < AcousticIntentRecognizer::kCepstra; ++k)
            for (unsigned j = 0; j < kChannels; ++j)
                dct[k][j] = static_cast<float>(std::sqrt(2.0 / kChannels) * std::cos(kPi * (k + 1) * (j + 0.5) / kChannels));
    }
};
const Tables& Dct() noexcept { static const Tables tables; return tables; }
}

std::uint32_t AcousticIntentRecognizer::Reduce(const float* logmel, std::uint32_t frames, float* out) noexcept {
    if (frames < kMinFrames || frames > kMaxFrames) return 0;
    // Trim to the speech: the first and last frame near the loudest one, padded.
    float peak = -std::numeric_limits<float>::infinity();
    float level[kMaxFrames];
    for (std::uint32_t t = 0; t < frames; ++t) {
        float sum = 0; for (unsigned j = 0; j < kChannels; ++j) sum += logmel[t * kChannels + j];
        level[t] = sum / kChannels; peak = (std::max)(peak, level[t]);
    }
    std::uint32_t first = 0, last = frames - 1;
    while (first < frames && level[first] < peak - kTrimBelowPeak) ++first;
    while (last > first && level[last] < peak - kTrimBelowPeak) --last;
    first = first > kTrimPad ? first - kTrimPad : 0; last = (std::min)(last + kTrimPad, frames - 1);
    const std::uint32_t count = last - first + 1;
    if (count < kMinFrames) return 0;
    // Floored log energies, then cepstra.
    const auto& tables = Dct();
    const float floor = peak - kFloorBelowPeak;
    float mean[kCepstra]{}, square[kCepstra]{};
    for (std::uint32_t t = 0; t < count; ++t) {
        const float* frame = logmel + static_cast<std::size_t>(first + t) * kChannels;
        float floored[kChannels];
        for (unsigned j = 0; j < kChannels; ++j) {
            // log(e^x + e^floor), computed without overflow.
            const float high = (std::max)(frame[j], floor), low = (std::min)(frame[j], floor);
            floored[j] = high + std::log1p(std::exp(low - high));
        }
        float* vector = out + static_cast<std::size_t>(t) * kDimensions;
        for (unsigned k = 0; k < kCepstra; ++k) {
            float c = 0; for (unsigned j = 0; j < kChannels; ++j) c += tables.dct[k][j] * floored[j];
            vector[k] = c; mean[k] += c; square[k] += c * c;
        }
    }
    if (kNormalizationWindow == 0 || count <= 2 * kNormalizationWindow + 1) {
        // Zero mean and equal variance per coefficient over the utterance.
        for (unsigned k = 0; k < kCepstra; ++k) {
            mean[k] /= static_cast<float>(count);
            const float variance = (std::max)(square[k] / static_cast<float>(count) - mean[k] * mean[k], 0.0f);
            const float gain = kNormalizedScale / std::sqrt(variance + 1e-6f);
            for (std::uint32_t t = 0; t < count; ++t) { float& c = out[static_cast<std::size_t>(t) * kDimensions + k]; c = (c - mean[k]) * gain; }
        }
    } else {
        // A longer utterance (a sentence) is normalised around each frame, over about a word's length: a word in it is
        // then measured as its template, recorded alone, was. Running sums are taken first, then every frame rewritten.
        float sum[kMaxFrames + 1][kCepstra], sum2[kMaxFrames + 1][kCepstra]; // 29 KB of stack (the voice thread's).
        for (unsigned k = 0; k < kCepstra; ++k) sum[0][k] = sum2[0][k] = 0;
        for (std::uint32_t t = 0; t < count; ++t)
            for (unsigned k = 0; k < kCepstra; ++k) {
                const float c = out[static_cast<std::size_t>(t) * kDimensions + k];
                sum[t + 1][k] = sum[t][k] + c; sum2[t + 1][k] = sum2[t][k] + c * c;
            }
        for (std::uint32_t t = 0; t < count; ++t) {
            const std::uint32_t low = t > kNormalizationWindow ? t - kNormalizationWindow : 0, high = (std::min)(count, t + kNormalizationWindow + 1);
            const float n = static_cast<float>(high - low);
            for (unsigned k = 0; k < kCepstra; ++k) {
                const float m = (sum[high][k] - sum[low][k]) / n;
                const float variance = (std::max)((sum2[high][k] - sum2[low][k]) / n - m * m, 0.0f);
                float& c = out[static_cast<std::size_t>(t) * kDimensions + k];
                c = (c - m) * kNormalizedScale / std::sqrt(variance + 1e-6f);
            }
        }
    }
    Deltas(out, count);
    return count;
}
void AcousticIntentRecognizer::Deltas(float* features, std::uint32_t frames) noexcept {
    // The slope over the neighbouring frames (the sequence's ends repeat).
    for (std::uint32_t t = 0; t < frames; ++t) {
        const float* before = features + static_cast<std::size_t>(t ? t - 1 : 0) * kDimensions;
        const float* after = features + static_cast<std::size_t>(t + 1 < frames ? t + 1 : frames - 1) * kDimensions;
        float* vector = features + static_cast<std::size_t>(t) * kDimensions;
        for (unsigned k = 0; k < kCepstra; ++k) vector[kCepstra + k] = kDeltaWeight * 0.5f * (after[k] - before[k]);
    }
}
std::uint32_t AcousticIntentRecognizer::Describe(std::span<const float> samples, float* scratch, float* out) noexcept {
    FilterbankFeatures filterbank;
    const auto frames = filterbank.Push(samples, scratch, kMaxFrames);
    return filterbank.Dropped() ? 0 : Reduce(scratch, frames, out);
}
namespace {
constexpr float kFar = 1e30f;
// The band Distance aligns in: two renditions of a word differ in rate by less than two to one, and
// cell (i, j) may be reached only near the diagonal (within a fifth of the longer sequence, or eight frames).
struct Band {
    std::uint32_t n, m, band;
    Band(std::uint32_t a, std::uint32_t b) noexcept : n(a), m(b), band((std::max)((std::max)(a, b) / 5, 8u) + (a > b ? a - b : b - a)) {}
    [[nodiscard]] bool Alignable() const noexcept { return n && m && n <= 2 * m && m <= 2 * n; }
    void Row(std::uint32_t i, std::uint32_t& low, std::uint32_t& high) const noexcept {
        const auto centre = static_cast<std::uint32_t>((static_cast<std::uint64_t>(i) * m + n / 2) / n);
        low = (std::max)(centre > band ? centre - band : 1u, 1u); high = (std::min)(m, centre + band);
    }
};
// The Euclidean distance of two frames: what every alignment spends its time on (three 4-wide lanes when SSE2 is
// there; the same sums, in the same order, either way).
static_assert(AcousticIntentRecognizer::kDimensions == 12, "Local is written for 12 dimensions");
float Local(const float* x, const float* y) noexcept {
#if defined(MEDIA_VOICE_SSE)
    const __m128 a = _mm_sub_ps(_mm_loadu_ps(x), _mm_loadu_ps(y)), b = _mm_sub_ps(_mm_loadu_ps(x + 4), _mm_loadu_ps(y + 4)),
                 c = _mm_sub_ps(_mm_loadu_ps(x + 8), _mm_loadu_ps(y + 8));
    __m128 v = _mm_add_ps(_mm_add_ps(_mm_mul_ps(a, a), _mm_mul_ps(b, b)), _mm_mul_ps(c, c));
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
    return _mm_cvtss_f32(_mm_sqrt_ss(v));
#else
    float lane[4]{};
    for (unsigned k = 0; k < 4; ++k) { const float a = x[k] - y[k], b = x[k + 4] - y[k + 4], c = x[k + 8] - y[k + 8]; lane[k] = a * a + b * b + c * c; }
    return std::sqrt((lane[0] + lane[2]) + (lane[1] + lane[3]));
#endif
}
}
float AcousticIntentRecognizer::Distance(const float* a, std::uint32_t n, const float* b, std::uint32_t m, float* previous, float* current, float abandon) noexcept {
    const Band shape{n, m};
    if (!shape.Alignable()) return -1;
    // Every path to (n, m) crosses each row, and costs only add: a row's cheapest cell bounds the end from below.
    const float give_up = abandon > 0 ? abandon * static_cast<float>(n + m) : kFar;
    for (std::uint32_t j = 0; j <= m; ++j) previous[j] = kFar;
    previous[0] = 0;
    for (std::uint32_t i = 1; i <= n; ++i) {
        std::uint32_t low = 0, high = 0; shape.Row(i, low, high);
        for (std::uint32_t j = 0; j <= m; ++j) current[j] = kFar;
        const float* x = a + static_cast<std::size_t>(i - 1) * kDimensions;
        float row = kFar;
        for (std::uint32_t j = low; j <= high; ++j) {
            const float best = (std::min)(previous[j - 1], (std::min)(previous[j], current[j - 1]));
            if (best < kFar) { current[j] = best + Local(x, b + static_cast<std::size_t>(j - 1) * kDimensions); row = (std::min)(row, current[j]); }
        }
        std::swap(previous, current);
        if (row >= kFar) return -1;
        if (row >= give_up) return row / static_cast<float>(n + m);
    }
    return previous[m] < kFar ? previous[m] / static_cast<float>(n + m) : -1;
}
float AcousticIntentRecognizer::PrefixDistance(const float* a, std::uint32_t n, const float* b, std::uint32_t m, float* previous, float* current) noexcept {
    const auto part = [](std::uint32_t frames) {
        const auto p = static_cast<std::uint32_t>(std::ceil(static_cast<float>(frames) * kPrefixShare));
        return (std::min)(frames, (std::max)(p, kMinFrames / 2));
    };
    return Distance(a, part(n), b, part(m), previous, current);
}
std::int32_t AcousticIntentRecognizer::Partner(std::int32_t command) const noexcept {
    for (unsigned i = 0; i < pair_count_; ++i) {
        if (pairs_[i][0] == command) return pairs_[i][1];
        if (pairs_[i][1] == command) return pairs_[i][0];
    }
    return -1;
}
std::int32_t AcousticIntentRecognizer::ResolvePrefix(const float* features, std::uint32_t frames, std::int32_t a, std::int32_t b, bool running) noexcept {
    // The evidence is one-sided: the longer word ("unmute") has a beginning the shorter lacks, and an utterance that
    // has it matches the longer word's onset clearly better (measured on held-out "unmute": 4.1-4.5 against 6.8-7.4);
    // an utterance that lacks it matches neither onset well ("mute": 5.2-5.7 against both). Noise before a word can
    // pass for that weak beginning (a noisy "mute" then matches "unmute"'s onset better), so the longer word also has
    // to win as a whole by the usual margin. The shorter word is taken when the onset shows no prefix and the whole
    // words favour it; anything else is neither (-1).
    const std::size_t la = std::strlen(TextOf(a)), lb = std::strlen(TextOf(b));
    const std::int32_t shorter = la < lb ? a : b, longer = la < lb ? b : a;
    float onset[2]{-1, -1}, whole[2]{-1, -1};
    for (std::size_t i = 0; i < set_.templates.size(); ++i) {
        const auto& t = set_.templates[i];
        if ((t.running && !running) || (t.command != a && t.command != b)) continue;
        const unsigned w = t.command == static_cast<std::uint16_t>(longer) ? 1u : 0u;
        const float p = PrefixDistance(features, frames, templates_[i], t.frames, rows_, rows_ + kMaxFrames + 1);
        const float d = Distance(features, frames, templates_[i], t.frames, rows_, rows_ + kMaxFrames + 1);
        if (p >= 0 && (onset[w] < 0 || p < onset[w])) onset[w] = p;
        if (d >= 0 && (whole[w] < 0 || d < whole[w])) whole[w] = d;
    }
    const bool prefix = onset[1] >= 0 && (onset[0] < 0 || onset[1] < (1 - kPrefixMargin) * onset[0]);
    if (prefix) return whole[1] >= 0 && (whole[0] < 0 || whole[1] < (1 - set_.margin) * whole[0]) ? longer : -1;
    if (whole[0] >= 0 && (whole[1] < 0 || whole[0] <= whole[1])) return shorter;
    return -1;
}
const char* AcousticIntentRecognizer::TextOf(std::int32_t command) const noexcept {
    for (const auto& t : set_.templates) if (static_cast<std::int32_t>(t.command) == command) return t.text;
    return "";
}
bool AcousticIntentRecognizer::CarriesAmount(const char* word) noexcept {
    // A word that carries an amount: it moves a rewind somewhere else than the same rewind without it ("rewind
    // minute" against "rewind", "rewind 1 minute" against "rewind 1", "rewind ten minute" against "rewind minute"), or a
    // number that needs its unit ("one", read like "a": "rewind one seconds" is a command, "rewind one" is not).
    // A direction ("rewind 1 back") moves nothing.
    if (!word) return false;
    // Whether the text is a command at all, not whether this media allows it: a film with chapters and scenes (the
    // grammar refuses chapter and scene commands as Unavailable without them; the engine's own bounds decide later).
    const MediaBounds film{24ll * 3600 * LocalIntentDispatcher::kSecond, 12ll * 3600 * LocalIntentDispatcher::kSecond, 0.5f, false, false, true, true, 0, 0, 1, 1};
    char probe[96];
    const auto parse = [&](std::string_view before, std::string_view after) { // before + word + after (no formatting: it may allocate).
        const std::string_view middle{word};
        if (before.size() + middle.size() + after.size() > sizeof(probe)) return Intent{};
        std::memcpy(probe, before.data(), before.size());
        std::memcpy(probe + before.size(), middle.data(), middle.size());
        std::memcpy(probe + before.size() + middle.size(), after.data(), after.size());
        return LocalIntentDispatcher::Parse({probe, before.size() + middle.size() + after.size()}, film);
    };
    struct Form { std::string_view before, after, without; };
    static constexpr Form forms[]{{"rewind ", "", "rewind"}, {"rewind 1 ", "", "rewind 1"}, {"rewind ", " minute", "rewind minute"}};
    for (const auto& form : forms) {
        const auto with = parse(form.before, form.after);
        if (with.status == IntentStatus::Ok && with.target_100ns != LocalIntentDispatcher::Parse(form.without, film).target_100ns) return true;
    }
    return parse("rewind ", " seconds").status == IntentStatus::Ok && parse("rewind ", "").status != IntentStatus::Ok;
}
std::size_t AcousticIntentRecognizer::Phrase(std::span<const char* const> words, char* out, std::size_t capacity) noexcept {
    if (!out || !capacity) return 0;
    out[0] = 0;
    // Repeats merged; at most kMaxWords words.
    const char* list[kMaxWords]; unsigned n = 0;
    for (const char* w : words) if (w && *w && n < kMaxWords && (!n || std::strcmp(list[n - 1], w) != 0)) list[n++] = w;
    // Whether the grammar reads the words [first, last) as one whole command. Structural bounds: a long seekable film
    // with subtitles, so only the words decide.
    const MediaBounds film{24ll * 3600 * LocalIntentDispatcher::kSecond, 12ll * 3600 * LocalIntentDispatcher::kSecond, 0.5f, false, false, true, true, 0, 0, 1, 1};
    char text[LocalIntentDispatcher::kMaxCommandBytes];
    const auto join = [&](unsigned first, unsigned last, char* into, std::size_t room) -> std::size_t {
        std::size_t size = 0;
        for (unsigned i = first; i < last; ++i) {
            const auto length = std::strlen(list[i]);
            if (size + length + 2 > room) return 0;
            if (size) into[size++] = ' ';
            std::memcpy(into + size, list[i], length); size += length;
        }
        into[size] = 0; return size;
    };
    const auto reads = [&](unsigned first, unsigned last) {
        const auto size = join(first, last, text, sizeof(text));
        return size && LocalIntentDispatcher::Parse({text, size}, film).status == IntentStatus::Ok;
    };
    const auto amount = [](const char* word) { return CarriesAmount(word); };
    for (unsigned first = 0; first < n; ++first)
        for (unsigned last = n; last > first; --last) {
            if (!reads(first, last)) continue;
            bool loses = false; // Nothing that carries an amount may be left out.
            for (unsigned i = 0; i < n && !loses; ++i) if ((i < first || i >= last) && amount(list[i])) loses = true;
            if (loses) return 0;
            return join(first, last, out, capacity);
        }
    return 0;
}
bool AcousticIntentRecognizer::WholeCommand(std::span<const char* const> words) noexcept {
    const char* list[kMaxWords]; unsigned n = 0; // Repeats merged, as in Phrase.
    for (const char* w : words) if (w && *w && n < kMaxWords && (!n || std::strcmp(list[n - 1], w) != 0)) list[n++] = w;
    char text[LocalIntentDispatcher::kMaxCommandBytes]; std::size_t size = 0;
    for (unsigned i = 0; i < n; ++i) {
        const auto length = std::strlen(list[i]);
        if (size + length + 2 > sizeof(text)) return false;
        if (size) text[size++] = ' ';
        std::memcpy(text + size, list[i], length); size += length;
    }
    const MediaBounds film{24ll * 3600 * LocalIntentDispatcher::kSecond, 12ll * 3600 * LocalIntentDispatcher::kSecond, 0.5f, false, false, true, true, 0, 0, 1, 1};
    return size && LocalIntentDispatcher::Parse({text, size}, film).status == IntentStatus::Ok;
}
bool AcousticIntentRecognizer::DirectionAlone(const char* text) noexcept {
    if (!text) return false;
    for (const char* word : {"back", "forward", "ahead", "\xd7\x90\xd7\x97\xd7\x95\xd7\xa8\xd7\x94", "\xd7\xa7\xd7\x93\xd7\x99\xd7\x9e\xd7\x94"}) // אחורה, קדימה
        if (std::strcmp(text, word) == 0) return true;
    return false;
}
std::size_t AcousticIntentRecognizer::DecodeScratch(std::span<const VoiceTemplate> templates) noexcept {
    std::size_t cells = 0;
    for (const auto& t : templates) cells += t.frames;
    return 4 * cells + 2 * templates.size() + 3 * std::size_t{kMaxFrames};
}
unsigned AcousticIntentRecognizer::DecodeBest(const float* features, std::uint32_t frames, std::span<const VoiceTemplate> templates, const float* const* blocks,
                                              float garbage, float penalty, float* scratch, SpottedWord* out, unsigned capacity, std::uint8_t* language,
                                              float beam) noexcept {
    std::uint8_t present = 0;
    for (const auto& t : templates) present = static_cast<std::uint8_t>(present | t.languages);
    if (language) *language = present;
    if ((present & (present - 1)) == 0) return Decode(features, frames, templates, blocks, garbage, penalty, scratch, out, capacity, kAllLanguages, nullptr, beam);
    unsigned best = 0; float best_cost = 0; bool have = false;
    SpottedWord words[kMaxWords];
    for (unsigned bit = 0; bit < 8; ++bit) {
        const auto mask = static_cast<std::uint8_t>(1u << bit);
        if (!(present & mask)) continue;
        float cost = 0;
        const auto n = Decode(features, frames, templates, blocks, garbage, penalty, scratch, words, kMaxWords, mask, &cost, beam);
        if (!have || cost < best_cost) {
            have = true; best_cost = cost; best = (std::min)(n, capacity);
            for (unsigned i = 0; i < best; ++i) out[i] = words[i];
            if (language) *language = mask;
        }
    }
    return best;
}
unsigned AcousticIntentRecognizer::Decode(const float* features, std::uint32_t frames, std::span<const VoiceTemplate> templates, const float* const* blocks,
                                          float garbage, float penalty, float* scratch, SpottedWord* out, unsigned capacity, std::uint8_t languages, float* total,
                                          float beam) noexcept {
    if (total) *total = 0;
    if (!frames || frames > kMaxFrames || templates.empty()) return 0;
    float* boundary = scratch;                       // Cheapest cost of a word boundary after frame t.
    float* ended = scratch + kMaxFrames;             // The template that ended there (-1: garbage).
    float* began = scratch + 2 * std::size_t{kMaxFrames}; // And where that word began.
    // Per template: the cells of the last frame's row that may hold a path, [first, last] (none: first > last), then
    // two rows of path costs and two of the frame each path's word started at (rows alternate by frame). Only cells
    // up to two past a live one can be reached, so a word nobody is saying costs its first cell per frame, not its row.
    float* live = scratch + 3 * std::size_t{kMaxFrames};
    float* rows = live + 2 * templates.size();
    for (std::size_t k = 0; k < templates.size(); ++k) { live[2 * k] = 1; live[2 * k + 1] = 0; }
    float before = 0;   // The utterance starts at a boundary.
    float limit = kFar; // The beam: a path of the last frame dearer than this is dropped.
    // Online garbage: a frame of no known word costs the mean of its kBest closest template frames, plus `garbage`.
    // It follows how well anything matches here, which a fixed cost cannot (garbage < 0 selects a fixed -garbage,
    // and then nothing is gathered for it).
    constexpr unsigned kBest = 8;
    const bool online = garbage >= 0;
    for (std::uint32_t t = 0; t < frames; ++t) {
        const float* x = features + static_cast<std::size_t>(t) * kDimensions;
        float best = kFar; int best_template = -1; float best_start = 0, cheapest = kFar;
        float closest[kBest]; for (auto& c : closest) c = kFar;
        const auto keep = [&](float local) { // Insertion into the sorted few.
            if (local >= closest[kBest - 1]) return;
            unsigned at = kBest - 1;
            while (at > 0 && closest[at - 1] > local) { closest[at] = closest[at - 1]; --at; }
            closest[at] = local;
        };
        const float entry = before + penalty;
        const bool enter = entry <= limit;
        std::size_t offset = 0;
        for (std::size_t k = 0; k < templates.size(); ++k) {
            const auto length = static_cast<int>(templates[k].frames);
            float* base = rows + offset; offset += 4u * static_cast<std::size_t>(length);
            if (!(templates[k].languages & languages)) continue; // Another language's word: not in this decoding.
            const int first = static_cast<int>(live[2 * k]), last = static_cast<int>(live[2 * k + 1]);
            if (first > last && !enter) continue;
            float* previous = base + (t & 1 ? length : 0); float* current = base + (t & 1 ? 0 : length);
            float* previous_start = base + 2 * length + (t & 1 ? length : 0); float* current_start = base + 2 * length + (t & 1 ? 0 : length);
            const auto held = [&](int i) { return i >= first && i <= last && previous[i] <= limit; };
            const float* y = blocks[k];
            int reached_first = length, reached_last = -1;
            const int to = first <= last ? (std::min)(last + 2, length - 1) : 0;
            for (int j = enter ? 0 : first; j <= to; ++j) {
                float cost = kFar, start = 0;
                if (held(j)) { cost = previous[j]; start = previous_start[j]; }                                              // Repeat the template frame.
                if (j >= 1 && held(j - 1) && previous[j - 1] < cost) { cost = previous[j - 1]; start = previous_start[j - 1]; } // Advance.
                if (j >= 2 && held(j - 2) && previous[j - 2] < cost) { cost = previous[j - 2]; start = previous_start[j - 2]; } // Skip one.
                if (j == 0 && enter && entry < cost) { cost = entry; start = static_cast<float>(t); }                        // Enter the word.
                if (cost >= kFar) { current[j] = kFar; continue; }
                const float local = Local(x, y + static_cast<std::size_t>(j) * kDimensions);
                current[j] = cost + local; current_start[j] = start;
                if (online) keep(local);
                reached_first = (std::min)(reached_first, j); reached_last = j;
                cheapest = (std::min)(cheapest, current[j]);
            }
            live[2 * k] = static_cast<float>(reached_last >= 0 ? reached_first : 1); live[2 * k + 1] = static_cast<float>(reached_last >= 0 ? reached_last : 0);
            // A word ends when its template's last frame is reached, between half and twice its recorded length.
            if (reached_last == length - 1 && current[length - 1] < best) {
                const auto span = static_cast<float>(t) - current_start[length - 1] + 1;
                if (2 * span >= static_cast<float>(length) && span <= 2.0f * static_cast<float>(length)) { best = current[length - 1]; best_template = static_cast<int>(k); best_start = current_start[length - 1]; }
            }
        }
        float nearby = 0; for (const float c : closest) nearby += c < kFar ? c : 0.0f;
        const float noise = before + (garbage < 0 ? -garbage : nearby / kBest + garbage);
        if (best < noise) { boundary[t] = best; ended[t] = static_cast<float>(best_template); began[t] = best_start; }
        else { boundary[t] = noise; ended[t] = -1; began[t] = static_cast<float>(t); }
        before = boundary[t];
        limit = beam > 0 ? (std::min)(cheapest, before) + beam : kFar;
    }
    if (total) *total = boundary[frames - 1] / static_cast<float>(frames);
    // Back from the end: words and garbage, then in order.
    unsigned count = 0;
    for (std::int64_t t = static_cast<std::int64_t>(frames) - 1; t >= 0;) {
        const auto k = static_cast<int>(ended[t]);
        if (k < 0) { --t; continue; }
        const auto start = static_cast<std::int64_t>(began[t]);
        if (count < capacity) {
            const float entry = start ? boundary[start - 1] : 0.0f;
            out[count++] = {static_cast<std::uint16_t>(k), static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(t),
                            (boundary[t] - entry - penalty) / static_cast<float>(t - start + 1)};
        }
        t = start - 1;
    }
    for (unsigned i = 0; i < count / 2; ++i) std::swap(out[i], out[count - 1 - i]);
    return count;
}
bool AcousticIntentRecognizer::Configure(core::LinearMemoryArena& arena, const VoiceTemplateSet& set) noexcept {
    if (templates_ || set.templates.empty() || !(set.scale > 0) || !(set.accept_distance > 0) || set.margin < 0 ||
        set.wake >= static_cast<std::int32_t>(kMaxCommands) || set.wake_distance < 0 || set.beam < 0) return false;
    std::size_t values = 0;
    bool wake_found = set.wake < 0;
    for (const auto& t : set.templates) {
        if (!t.text || !t.features || t.frames < kMinTemplateFrames || t.frames > kMaxTemplateFrames || (t.command != kFiller && !*t.text) ||
            (t.command != kFiller && t.command >= kMaxCommands)) return false;
        values += static_cast<std::size_t>(t.frames) * kDimensions;
        wake_found = wake_found || t.command == set.wake;
    }
    if (!wake_found) return false;
    // Prefix pairs: one command's text ends with another's, after a word-internal prefix ("unmute", "mute").
    pair_count_ = 0;
    for (const auto& x : set.templates) {
        if (x.command == kFiller) continue;
        for (const auto& y : set.templates) {
            if (y.command == kFiller || y.command == x.command) continue;
            const std::string_view longer{x.text}, shorter{y.text};
            if (longer.size() <= shorter.size() || longer.substr(longer.size() - shorter.size()) != shorter) continue;
            if (longer.substr(0, longer.size() - shorter.size()).find(' ') != std::string_view::npos) continue; // "turn up" is no prefix of "up".
            bool known = false;
            for (unsigned i = 0; i < pair_count_; ++i) known = known || (pairs_[i][0] == y.command && pairs_[i][1] == x.command);
            if (!known && pair_count_ < kMaxPairs) { pairs_[pair_count_][0] = y.command; pairs_[pair_count_][1] = x.command; ++pair_count_; }
        }
    }
    auto* blocks = static_cast<const float**>(arena.Allocate(sizeof(const float*) * set.templates.size()));
    auto* store = static_cast<float*>(arena.Allocate(sizeof(float) * values));
    logmel_ = static_cast<float*>(arena.Allocate(sizeof(float) * kMaxFrames * kChannels));
    features_ = static_cast<float*>(arena.Allocate(sizeof(float) * kMaxFrames * kDimensions));
    rows_ = static_cast<float*>(arena.Allocate(sizeof(float) * 2 * (kMaxFrames + 1)));
    decode_ = static_cast<float*>(arena.Allocate(sizeof(float) * DecodeScratch(set.templates)));
    if (!blocks || !store || !logmel_ || !features_ || !rows_ || !decode_) { logmel_ = features_ = rows_ = decode_ = nullptr; return false; }
    for (std::size_t i = 0; i < set.templates.size(); ++i) {
        const auto& t = set.templates[i];
        blocks[i] = store;
        for (std::size_t v = 0; v < static_cast<std::size_t>(t.frames) * kDimensions; ++v) *store++ = static_cast<float>(t.features[v]) * set.scale;
    }
    (void)Dct(); // Built here, not inside the first utterance.
    templates_ = blocks; set_ = set; last_ = {}; stats_ = {}; frames_ = 0; overflowed_ = false;
    return true;
}
bool AcousticIntentRecognizer::Configure(core::LinearMemoryArena& arena, const VoicePackedSet& set) noexcept { return Configure(arena, set, {}); }
bool AcousticIntentRecognizer::Configure(core::LinearMemoryArena& arena, const VoicePackedSet& set, std::span<const VoiceTemplate> wake) noexcept {
    // The packed set as an ordinary one: each template's frames rebuilt (kept cepstra, lines between them, deltas) and
    // rounded to the set's scale, as the template tool measured them.
    if (templates_ || set.templates.empty() || set.words.empty() || set.bits < 1 || set.bits > 8 || !(set.settings.scale > 0) ||
        set.levels.size() != (std::size_t{kCepstra} << set.bits)) return false;
    std::size_t frames = 0, kept = 0;
    const auto key = [&](std::size_t frame) { return (set.keys[frame / 8] >> (frame % 8)) & 1u; };
    for (const auto& t : set.templates) {
        if (t.word >= set.words.size() || t.frames < kMinTemplateFrames || t.frames > kMaxTemplateFrames) return false;
        if ((frames + t.frames + 7) / 8 > set.keys.size() || !key(frames) || !key(frames + t.frames - 1)) return false; // Both ends are kept.
        for (unsigned f = 0; f < t.frames; ++f) kept += key(frames + f);
        frames += t.frames;
    }
    if ((kept * kCepstra * set.bits + 7) / 8 > set.values.size()) return false;
    if (!wake.empty() && set.settings.wake < 0) return false;
    for (const auto& t : wake) if (!t.text || !*t.text || !t.features || t.running) return false;
    auto* templates = static_cast<VoiceTemplate*>(arena.Allocate(sizeof(VoiceTemplate) * (set.templates.size() + wake.size())));
    auto* values = static_cast<std::int8_t*>(arena.Allocate(frames * kDimensions));
    if (!templates || !values) return false;
    std::size_t frame = 0, bit = 0;
    const auto code = [&] { // The next `bits` bits of the values.
        unsigned v = 0;
        for (unsigned b = 0; b < set.bits; ++b, ++bit) v |= ((set.values[bit / 8] >> (bit % 8)) & 1u) << b;
        return v;
    };
    std::int8_t* out = values;
    float rebuilt[kMaxTemplateFrames * kDimensions];
    for (std::size_t i = 0; i < set.templates.size(); ++i) {
        const auto& t = set.templates[i]; const auto& word = set.words[t.word];
        std::uint32_t last = 0;
        for (std::uint32_t f = 0; f < t.frames; ++f) {
            if (!key(frame + f)) continue;
            float* v = rebuilt + static_cast<std::size_t>(f) * kDimensions;
            for (unsigned k = 0; k < kCepstra; ++k) v[k] = set.levels[(static_cast<std::size_t>(k) << set.bits) + code()];
            for (std::uint32_t g = last + 1; g < f; ++g) { // The frames since the last kept one, on the line.
                const float w = static_cast<float>(g - last) / static_cast<float>(f - last);
                for (unsigned k = 0; k < kCepstra; ++k)
                    rebuilt[static_cast<std::size_t>(g) * kDimensions + k] = (1 - w) * rebuilt[static_cast<std::size_t>(last) * kDimensions + k] + w * v[k];
            }
            last = f;
        }
        Deltas(rebuilt, t.frames);
        for (std::size_t v = 0; v < static_cast<std::size_t>(t.frames) * kDimensions; ++v)
            out[v] = static_cast<std::int8_t>(std::lround((std::clamp)(rebuilt[v] / set.settings.scale, -127.0f, 127.0f)));
        // With a custom name, the built-in one is an ordinary word: heard, and refused like any filler.
        const bool replaced = !wake.empty() && static_cast<std::int32_t>(word.command) == set.settings.wake;
        std::construct_at(templates + i, VoiceTemplate{replaced ? "" : word.text, replaced ? kFiller : word.command, t.frames, out, t.running != 0,
                                                       replaced ? kAllLanguages : word.languages});
        out += static_cast<std::size_t>(t.frames) * kDimensions; frame += t.frames;
    }
    for (std::size_t i = 0; i < wake.size(); ++i) {
        auto t = wake[i]; t.command = static_cast<std::uint16_t>(set.settings.wake); t.languages = kAllLanguages;
        std::construct_at(templates + set.templates.size() + i, t);
    }
    VoiceTemplateSet expanded = set.settings;
    expanded.templates = {templates, set.templates.size() + wake.size()};
    if (!Configure(arena, expanded)) return false;
    custom_wake_ = !wake.empty();
    return true;
}
bool AcousticIntentRecognizer::VerifyWake(std::uint32_t begin, std::uint32_t end) noexcept {
    if (set_.wake < 0 || end < begin || end - begin + 1 < kMinFrames) return false;
    const std::uint32_t frames = end - begin + 1;
    const float* span = features_ + static_cast<std::size_t>(begin) * kDimensions;
    // The nearest template of the wake word, and of everything else (any command or filler), as the isolated matcher finds them.
    const float accept = set_.wake_distance > 0 ? set_.wake_distance : set_.accept_distance;
    const float bound = (1 + set_.margin) * accept * 1.001f;
    float name = -1, other = -1;
    for (std::size_t i = 0; i < set_.templates.size(); ++i) {
        const auto& t = set_.templates[i];
        if (t.running) continue;
        const bool wake = static_cast<std::int32_t>(t.command) == set_.wake;
        float& nearest = wake ? name : other;
        const float d = Distance(span, frames, templates_[i], t.frames, rows_, rows_ + kMaxFrames + 1, nearest >= 0 ? (std::min)(nearest, bound) : bound);
        if (d >= 0 && (nearest < 0 || d < nearest)) nearest = d;
    }
    return name >= 0 && name <= accept && (other < 0 || other - name >= set_.margin * name);
}
void AcousticIntentRecognizer::Begin() noexcept { filterbank_.Reset(); frames_ = 0; overflowed_ = false; decoded_count_ = 0; }
void AcousticIntentRecognizer::Feed(std::span<const float> samples) noexcept {
    if (!templates_) return;
    frames_ += filterbank_.Push(samples, logmel_ + static_cast<std::size_t>(frames_) * kChannels, kMaxFrames - frames_);
    overflowed_ = overflowed_ || filterbank_.Dropped() != 0;
}
AcousticResult AcousticIntentRecognizer::Finish() noexcept {
    AcousticResult result;
    if (!templates_) return result;
    ++stats_.utterances;
    const std::uint32_t frames = overflowed_ ? 0 : Reduce(logmel_, frames_, features_);
    result.frames = frames;
    if (overflowed_) { result.match = AcousticMatch::TooLong; ++stats_.too_long; }
    else if (!frames) { result.match = AcousticMatch::TooShort; ++stats_.too_short; }
    else {
        // The nearest template of each command (and of all fillers together, as one class).
        constexpr std::int32_t kFillerClass = static_cast<std::int32_t>(kMaxCommands);
        float nearest[kMaxCommands + 1]; const char* names[kMaxCommands + 1]{};
        for (auto& d : nearest) d = -1;
        // An alignment is abandoned once it cannot beat its class's nearest, nor end within `bound`: beyond that a
        // distance changes no decision below (accepted at most within the acceptance distance; a rival counts only
        // within the margin of it). Every class nearer than `bound` keeps its exact distance.
        const float bound = (1 + set_.margin) * (std::max)(set_.accept_distance, set_.wake_distance) * 1.001f;
        for (std::size_t i = 0; i < set_.templates.size(); ++i) {
            const auto& t = set_.templates[i];
            if (t.running) continue; // A word as said inside a sentence is no model of the word said alone.
            const auto c = t.command == kFiller ? kFillerClass : static_cast<std::int32_t>(t.command);
            const float d = Distance(features_, frames, templates_[i], t.frames, rows_, rows_ + kMaxFrames + 1, nearest[c] >= 0 ? (std::min)(nearest[c], bound) : bound);
            if (d < 0) continue;
            if (nearest[c] < 0 || d < nearest[c]) { nearest[c] = d; names[c] = t.text; }
        }
        // The best class, and the best other one but `skip` (a prefix pair's loser does not compete).
        const auto pick = [&](std::int32_t not_a, std::int32_t not_b) {
            std::int32_t at = -1;
            for (std::int32_t c = 0; c <= kFillerClass; ++c)
                if (c != not_a && c != not_b && nearest[c] >= 0 && (at < 0 || nearest[c] < nearest[at])) at = c;
            return at;
        };
        std::int32_t best_class = pick(-1, -1), other_class = best_class >= 0 ? pick(best_class, -1) : -1;
        // Two commands that differ only in their beginning ("mute", "unmute") are decided on their onsets: the whole
        // word's distance hardly tells "mute" from the "mute" inside "unmute". Whenever the nearest command is one of a
        // pair: the pair's winner then has no rival in its partner, and an utterance that is neither for sure is
        // refused, however far the other commands are.
        bool undecided = false;
        if (best_class >= 0 && best_class != kFillerClass) {
            const auto partner = Partner(best_class);
            if (partner >= 0 && nearest[partner] >= 0) {
                const auto chosen = ResolvePrefix(features_, frames, best_class, partner, false);
                if (chosen < 0) undecided = true;
                else { const auto loser = chosen == partner ? best_class : partner; best_class = chosen; other_class = pick(chosen, loser); result.prefix_resolved = true; }
            }
        }
        const float best = best_class >= 0 ? nearest[best_class] : -1, other = other_class >= 0 ? nearest[other_class] : -1;
        const auto number = [&](std::int32_t c) { return c == kFillerClass ? static_cast<std::int32_t>(kFiller) : c; };
        const char* text = best_class >= 0 ? names[best_class] : nullptr;
        const std::int32_t best_command = best_class >= 0 ? number(best_class) : -1, other_command = other_class >= 0 ? number(other_class) : -1;
        result.command = best_command; result.distance = best; result.runner_up = other_command; result.runner_up_distance = other; result.text = text;
        const bool wake = set_.wake >= 0 && best_command == set_.wake;
        const float accept = wake && set_.wake_distance > 0 ? set_.wake_distance : set_.accept_distance;
        if (best < 0 || best > accept) result.match = AcousticMatch::Distant;
        else if (best_command == kFiller) result.match = AcousticMatch::Filler;
        else if (undecided || (other >= 0 && other - best < set_.margin * best)) result.match = AcousticMatch::Ambiguous;
        else result.match = AcousticMatch::Accepted;
        if (result.match == AcousticMatch::Accepted && wake) result.wake = true; // The name alone: a listening window opens.
        // A word of the vocabulary that is no command by itself ("turn", "ten") said alone: nothing to do.
        if (result.match == AcousticMatch::Accepted && !wake) {
            const char* const alone[]{text};
            if (!Phrase(alone, spoken_, sizeof(spoken_))) result.match = AcousticMatch::Partial;
        }
        // Not one command word: as connected words, when it is long enough to be a sentence. With a wake word, a long
        // utterance taken for one command is decoded too: it may be the name and a command in one breath.
        const bool decode = result.match != AcousticMatch::Accepted || (set_.wake >= 0 && !result.wake);
        // Shorter utterances only for the wake word and a short command after it ("avion stop"). Each language lane has
        // its own floor: decoded from the lowest, kept only from the winning lane's.
        const std::uint32_t lowest = (std::min)(LaneSentenceFrames(0), LaneSentenceFrames(1));
        const std::uint32_t floor = set_.wake >= 0 ? (std::min)(lowest, kWakeSentenceFrames) : lowest;
        if (decode && decode_ && set_.garbage != 0 && frames >= floor) {
            SpottedWord words[kMaxWords]; std::uint8_t language = kEnglish;
            const auto n = DecodeBest(features_, frames, set_.templates, templates_, set_.garbage, set_.word_penalty, decode_, words, kMaxWords, &language, set_.beam);
            decoded_count_ = n; for (unsigned i = 0; i < n; ++i) decoded_[i] = words[i];
            const unsigned lane = language == kHebrew ? 1u : 0u;
            const float word_limit = LaneWordLimit(lane);
            const auto counts = [&](const SpottedWord& w, std::int32_t command) {
                const float reference = command >= 0 && static_cast<std::size_t>(command) < set_.typical.size() && set_.typical[command] > 0 ? set_.typical[command] : 1.0f;
                return word_limit <= 0 || w.cost <= word_limit * reference;
            };
            // The wake word: the first word found, starting within the utterance's first kWakeLead frames.
            constexpr std::uint16_t kWakeLead = 12;
            unsigned first = 0;
            const bool woke = set_.wake >= 0 && n && set_.templates[words[0].template_index].command == static_cast<std::uint16_t>(set_.wake) &&
                              words[0].begin <= kWakeLead && counts(words[0], set_.wake) && (!custom_wake_ || VerifyWake(words[0].begin, words[0].end));
            if (woke) first = 1;
            const char* texts[kMaxWords]; unsigned count = 0; bool resolved = false, lost_amount = false;
            for (unsigned i = first; i < n; ++i) {
                const auto& t = set_.templates[words[i].template_index];
                if (t.command == kFiller || static_cast<std::int32_t>(t.command) == set_.wake) continue;
                std::int32_t command = t.command; const char* name = t.text;
                // A prefix pair's member: its span's onset decides which of the two it is.
                if (const auto partner = Partner(command); partner >= 0) {
                    const auto chosen = ResolvePrefix(features_ + static_cast<std::size_t>(words[i].begin) * kDimensions, words[i].end - words[i].begin + 1u, command, partner, true);
                    if (chosen < 0) continue; // Neither word for sure: left out (a sentence then reads as no command).
                    if (chosen != command) { name = TextOf(chosen); command = chosen; }
                    resolved = true;
                }
                if (counts(words[i], t.command)) texts[count++] = name;
                else if (CarriesAmount(name)) lost_amount = true; // "skip forward [five] minutes": never shortened.
            }
            std::size_t size = 0;
            if (woke && count) { // "avion" and then the command; without one, this decoding says nothing.
                const char* wake_text = set_.templates[words[0].template_index].text;
                const auto name = std::strlen(wake_text);
                std::memcpy(spoken_, wake_text, name); spoken_[name] = ' ';
                const auto phrase = Phrase({texts, count}, spoken_ + name + 1, sizeof(spoken_) - name - 1);
                size = phrase ? name + 1 + phrase : 0; spoken_[size] = 0;
            } else if (count && !woke) size = Phrase({texts, count}, spoken_, sizeof(spoken_));
            if (lost_amount) size = 0; // An amount heard too poorly to keep: nothing is done rather than another amount.
            // In a listening window (the name was said alone before): every command word must belong to the command, and a
            // direction word alone is no sentence (docs/voice.md, "Room talk in the window").
            if (size && listening_ && window_rules_ && !woke && (!WholeCommand({texts, count}) || DirectionAlone(spoken_))) { size = 0; ++stats_.window_refused; }
            // The lane's floor, or the listening window's when the utterance began inside one (FollowWake).
            const std::uint32_t lane_floor = LaneSentenceFrames(lane);
            const std::uint32_t floor_now = listening_ && window_frames_ ? (std::min)(lane_floor, window_frames_) : lane_floor;
            if (size && !woke && result.match != AcousticMatch::Accepted && frames >= floor_now && frames < lane_floor) ++stats_.window;
            if (size && (woke || (result.match != AcousticMatch::Accepted && frames >= floor_now))) {
                result.match = AcousticMatch::Accepted; result.text = spoken_; result.sentence = true; result.words = count; result.wake = woke;
                result.prefix_resolved = result.prefix_resolved || resolved;
                ++stats_.sentences;
            }
        }
        if (result.wake) ++stats_.wakes;
        switch (result.match) {
        case AcousticMatch::Distant: ++stats_.distant; break;
        case AcousticMatch::Filler: ++stats_.filler; break;
        case AcousticMatch::Ambiguous: ++stats_.ambiguous; break;
        case AcousticMatch::Partial: ++stats_.partial; break;
        default: ++stats_.accepted; break;
        }
    }
    last_ = result;
    return result;
}
void AcousticIntentRecognizer::Hear(void* context, const audio::SpeechSlice& slice, UtteranceBuffer& text) noexcept {
    auto& self = *static_cast<AcousticIntentRecognizer*>(context);
    if (slice.state == audio::VadState::SpeechStart) { self.Begin(); self.listening_ = self.wake_gate_ && self.wake_gate_->OpenAt(slice.first_frame); }
    if (slice.state == audio::VadState::SpeechEnd) {
        const auto result = self.Finish();
        if (result.match == AcousticMatch::Accepted && result.text) (void)text.Replace(result.text);
        return;
    }
    if (slice.samples && slice.frames) self.Feed({slice.samples, slice.frames});
}
}
