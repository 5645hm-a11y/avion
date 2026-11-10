// Subtitle quote search (subtitle::SearchIndex over text::Normalize): folding rules script by script, matching and
// ordering on a multilingual track, every answer on a full-size track checked against a brute-force scan, zero
// allocations, and the lookup time.
#include "media/subtitle/SubtitleSearch.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
using namespace media::subtitle;
namespace text = media::text;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#x); } } while(false)
// Lookup times are asserted in optimised, uninstrumented builds only (printed everywhere).
#if defined(__SANITIZE_ADDRESS__) || (defined(_MSC_VER) && defined(_DEBUG)) || (!defined(_MSC_VER) && !defined(__OPTIMIZE__))
constexpr bool kTimed = false;
#else
constexpr bool kTimed = true;
#endif
std::string Form(std::string_view in, unsigned flags) {
    std::string out(in.size(), '\0');
    const auto n = text::Normalize(in, out.data(), out.size(), flags);
    if (n == text::kInvalid) return "<invalid>";
    out.resize(n); return out;
}
std::string Fold(std::string_view in) { return Form(in, text::kFold); }
double Microseconds(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

void Folding() {
    CHECK(Fold("Hello  WORLD\r\n\tNext") == "hello world next");
    CHECK(Fold("ÀÉÎÕÜ Þ ß × Ÿ İ ſ Ž Ĳ") == "àéîõü þ ß × ÿ i s ž ĳ");               // Latin-1 and Latin Extended-A.
    CHECK(Fold("Σίσυφος ΆΈΉΊΌΎΏ ΑΩ") == "σίσυφοσ άέήίόύώ αω");                     // Greek, final sigma, tonos capitals.
    CHECK(Fold("ПРИВЕТ Ёлка Ґанок ЃЉЊ") == "привет ёлка ґанок ѓљњ");               // Cyrillic, all three case layouts.
    CHECK(Fold("ԲԱՐԵՒ ＡＢＣ") == "բարեւ ａｂｃ");                                     // Armenian, fullwidth Latin.
    CHECK(Fold("שָׁלוֹם עוֹלָם") == "שלום עולם");                                        // Hebrew points.
    CHECK(Fold("בְּרֵאשִׁ֖ית") == "בראשית");                                           // ... and cantillation.
    CHECK(Fold("מה־זה") == "מה־זה");                                               // The maqaf is punctuation: kept.
    CHECK(Fold("مَرْحَبًا بِالْعَالَمِ") == "مرحبا بالعالم");                             // Arabic harakat.
    CHECK(Fold("مـــرحبا") == "مرحبا" && Fold("أهلاً إلى آخر") == "اهلا الى اخر");    // Tatweel; alef with hamza or madda.
    CHECK(Fold("\xe2\x80\x8f" "שלום" "\xe2\x80\x8e") == "שלום");                    // Right-to-left and left-to-right marks.
    CHECK(Fold("a\xc2\xa0" "b\xe3\x80\x80" "c\xe2\x80\x8b" "d\xc2\xad" "e") == "a b cde");    // No-break and ideographic spaces; zero-width space, soft hyphen.
    CHECK(Fold("日本語 한국어 हिन्दी ภาษาไทย 🌍") == "日本語 한국어 हिन्दी ภาษาไทย 🌍");        // Caseless scripts pass through.
    // The unfolded form keeps case and points, and still drops what cannot be seen.
    CHECK(Form("Hello\n\xe2\x80\x8f" "שָׁלוֹם", 0) == "Hello שָׁלוֹם");
    // Cue markup: tags dropped, entities decoded. A query is plain text: its '<' and '&' stay.
    CHECK(Form("<i>Hi &amp; bye</i>&nbsp;&lrm;<b>You</b> &lt;3", text::kFold | text::kMarkup) == "hi & bye you <3");
    CHECK(Fold("<3 &amp; you") == "<3 &amp; you" && Form("a < b", text::kMarkup) == "a < b");
    char small[4];
    CHECK(text::Normalize("\xc0\xaf", small, sizeof(small), text::kFold) == text::kInvalid);   // Overlong.
    CHECK(text::Normalize("\xed\xa0\x80", small, sizeof(small), text::kFold) == text::kInvalid); // Surrogate.
    CHECK(text::Normalize("abcde", small, sizeof(small), text::kFold) == text::kInvalid);       // Capacity below the input.
    CHECK(text::Normalize("", small, sizeof(small), text::kFold) == 0);
    // Every scalar: the folded form is never longer than the input, is well formed, and folding it again changes nothing.
    unsigned longer = 0, unstable = 0, round_trip = 0;
    for (std::uint32_t scalar = 1; scalar < 0x30000; ++scalar) {
        if (scalar >= 0xd800 && scalar <= 0xdfff) continue;
        char in[4], out[4], again[4];
        const unsigned bytes = text::Encode(scalar, in);
        std::string_view view{in, bytes}; std::uint32_t decoded = 0;
        if (!text::Utf8(view, decoded) || decoded != scalar || !view.empty()) ++round_trip;
        const auto n = text::Normalize({in, bytes}, out, sizeof(out), text::kFold);
        if (n == text::kInvalid || n > bytes) { ++longer; continue; }
        const auto m = text::Normalize({out, n}, again, sizeof(again), text::kFold);
        if (m != n || std::string_view{out, n} != std::string_view{again, m}) ++unstable;
    }
    CHECK(longer == 0 && unstable == 0 && round_trip == 0);
}

// A small track in seven scripts, deliberately out of order in the file: matches come back in time order.
void Multilingual() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    constexpr auto source =
        "9\n01:02:03,004 --> 01:02:05,000\nthe quiet END — שלום\n\n"
        "5\n00:00:20,250 --> 00:00:24,000\n日本語の字幕です\n中文字幕也在这里\n\n"
        "1\n00:00:01,000 --> 00:00:04,000\nThe quiet between the frames.\n<i>A little more room</i> for the story.\n\n"
        "3\n00:00:09,500 --> 00:00:12,000\nאמרתי לך: שלום, ולא להתראות\n\n"
        "8\n00:00:40,000 --> 00:00:44,000\nQuiet now. Tom &amp; Jerry said HELLO.\n\n"
        "2\n00:00:05,000 --> 00:00:08,000\n\xe2\x80\x8f" "שָׁלוֹם עוֹלָם\n\n"
        "4\n00:00:14,000 --> 00:00:18,000\nمَرْحَبًا بِالْعَالَمِ\n\n"
        "7\n00:00:31,000 --> 00:00:35,000\nΚαλημέρα ΚΟΣΜΕ — Привет, МИР!\n\n"
        "6\n00:00:26,000 --> 00:00:30,000\n한국어 자막 · हिन्दी उपशीर्षक\n";
    SubtitleTrack track; SearchIndex index; SearchMatch match;
    CHECK(!index.Ready() && !index.Find("quiet", 0, match) && index.Count("quiet") == 0); // Not prepared: nothing matches.
    CHECK(!index.Prepare(arena, track));                                                  // An empty track has no index.
    CHECK(track.Prepare(arena, source) && track.Cues().size() == 9);
    allocation_probe::Begin();
    CHECK(index.Prepare(arena, track)); CHECK(!index.Prepare(arena, track));
    const auto prepare_allocations = allocation_probe::End(); CHECK(prepare_allocations == 0);
    CHECK(arena.Seal() && index.Ready() && index.Bytes() > 9 * 32);
    const auto cues = track.Cues();
    const std::string long_query(SearchIndex::kMaxQueryBytes + 1, 'q'), limit_query(SearchIndex::kMaxQueryBytes, 'q');
    allocation_probe::Begin();
    // English: three cues in time order, each landing on its cue's exact start.
    CHECK(index.Count("quiet") == 3);
    CHECK(index.Find("quiet", 0, match) && match.cue == 0 && match.start == 10000000 && match.end == 40000000);
    CHECK(index.Find("QUIET", 1, match) && match.cue == 7 && match.start == 400000000);
    CHECK(index.Find("quiet", 2, match) && match.cue == 8 && match.start == 37230040000ll && match.start == cues[8].start);
    CHECK(!index.Find("quiet", 3, match));
    CHECK(index.Count("quiet", MatchMode::Exact) == 2 && index.Find("Quiet", 0, match, MatchMode::Exact) && match.cue == 7);
    CHECK(index.Find("HELLO", 0, match, MatchMode::Exact) && match.cue == 7 && !index.Find("hello", 0, match, MatchMode::Exact));
    CHECK(index.Find("hello", 0, match) && match.cue == 7);
    // Hebrew: a query typed without points finds the pointed line too; an exact search tells them apart.
    CHECK(index.Count("שלום") == 3 && index.Find("שלום", 0, match) && match.cue == 1 && match.start == 50000000);
    CHECK(index.Find("שלום", 1, match) && match.cue == 2 && match.start == 95000000);
    CHECK(index.Find("שלום", 2, match) && match.cue == 8);
    CHECK(index.Count("שלום", MatchMode::Exact) == 2 && index.Find("שלום", 0, match, MatchMode::Exact) && match.cue == 2);
    CHECK(index.Find("שָׁלוֹם", 0, match, MatchMode::Exact) && match.cue == 1 && index.Count("שָׁלוֹם") == 3);
    CHECK(index.Find("שלום עולם", 0, match) && match.cue == 1 && index.Find("ולא להתראות", 0, match) && match.cue == 2);
    // Arabic: harakat are optional in the query, required by an exact search.
    CHECK(index.Find("مرحبا بالعالم", 0, match) && match.cue == 3 && match.start == 140000000);
    CHECK(index.Count("مرحبا", MatchMode::Exact) == 0 && index.Find("مَرْحَبًا", 0, match, MatchMode::Exact) && match.cue == 3);
    // CJK, Korean, Devanagari: no spaces to rely on, one ideograph is a query, a line break is a space.
    CHECK(index.Find("日本語", 0, match) && match.cue == 4 && match.start == 202500000);
    CHECK(index.Count("字幕") == 1 && index.Find("中文字幕", 0, match) && match.cue == 4 && index.Find("字", 0, match) && match.cue == 4);
    CHECK(index.Find("字幕です 中文", 0, match) && match.cue == 4 && !index.Find("字幕です中文", 0, match));
    CHECK(index.Find("한국어", 0, match) && match.cue == 5 && index.Find("उपशीर्षक", 0, match) && match.cue == 5 && match.start == 260000000);
    // Greek and Cyrillic fold case.
    CHECK(index.Find("καλημέρα κοσμε", 0, match) && match.cue == 6 && index.Find("привет, мир", 0, match) && match.cue == 6);
    CHECK(index.Find("ПРИВЕТ", 0, match) && match.cue == 6 && !index.Find("ПРИВЕТ", 0, match, MatchMode::Exact));
    // The text searched is the text displayed: across a tag and a line break, with entities decoded.
    CHECK(index.Find("room for the story", 0, match) && match.cue == 0 && index.Find("frames. a little", 0, match) && match.cue == 0);
    CHECK(index.Find("tom & jerry", 0, match) && match.cue == 7 && !index.Find("&amp;", 0, match) && !index.Find("<i>", 0, match));
    // Refused queries.
    CHECK(!index.Find("", 0, match) && !index.Find("   ", 0, match) && !index.Find("\n", 0, match) && index.Count(" ") == 0);
    CHECK(!index.Find("\xc0\xaf", 0, match) && !index.Find("qui\xff", 0, match) && !index.Find("\xe2\x80\x8f", 0, match));
    CHECK(!index.Find(long_query, 0, match) && !index.Find(limit_query, 0, match) && index.Count("zebra") == 0);
    CHECK(allocation_probe::End() == 0);
}

// A full-size track (SubtitleTrack::kMaxCues cues, about 1 MiB of text in nine scripts): every answer equals a
// brute-force scan of the normalised cues, and the slowest lookup is timed.
struct Corpus {
    std::string source;
    std::vector<std::string> pairs; // Consecutive words of some cues, joined by one space.
};
constexpr const char* kWords[]{
    "the", "quiet", "between", "frames", "story", "never", "again", "Hello", "WORLD", "night", "river", "light",
    "שלום", "עולם", "אהבה", "לילה", "אור", "הנהר", "שֶׁקֶט", "בבקשה",
    "مرحبا", "العالم", "ليل", "نور", "حُبّ",
    "日本語", "字幕", "中文", "世界", "夜", "光",
    "한국어", "자막", "हिन्दी", "Καλημέρα", "κόσμε", "Привет", "МИР", "ночь", "ภาษาไทย"};
Corpus Build(unsigned cues, unsigned min_words, unsigned extra_words) {
    Corpus corpus; std::uint32_t state = 0x2545f491u;
    const auto next = [&] { state = state * 1664525u + 1013904223u; return state >> 8; };
    for (unsigned i = 0; i < cues; ++i) {
        const unsigned start = i * 2000, end = start + 1500; char timing[96];
        std::snprintf(timing, sizeof(timing), "%u\n%02u:%02u:%02u,%03u --> %02u:%02u:%02u,%03u\n", i + 1, start / 3600000, start / 60000 % 60,
                      start / 1000 % 60, start % 1000, end / 3600000, end / 60000 % 60, end / 1000 % 60, end % 1000);
        corpus.source += timing;
        const unsigned words = min_words + next() % (extra_words + 1), italic = next() % 10 == 0 ? next() % words : words;
        const char* previous = nullptr;
        for (unsigned w = 0; w < words; ++w) {
            const char* word = kWords[next() % std::size(kWords)];
            if (w) corpus.source += next() % 8 == 0 ? "\n" : " ";
            if (w == italic) corpus.source += "<i>";
            corpus.source += word;
            if (w == italic) corpus.source += "</i>";
            if (previous && i % 997 == 0 && corpus.pairs.size() < 24) corpus.pairs.push_back(std::string(previous) + " " + word);
            previous = word;
        }
        corpus.source += "\n\n";
    }
    return corpus;
}
void FullTrack() {
    const auto corpus = Build(SubtitleTrack::kMaxCues, 4, 6);
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(24 * 1024 * 1024));
    SubtitleTrack track; SearchIndex index;
    CHECK(track.Prepare(arena, corpus.source) && track.Cues().size() == SubtitleTrack::kMaxCues);
    const auto prepare_begin = std::chrono::steady_clock::now();
    CHECK(index.Prepare(arena, track));
    const auto prepare = Microseconds(std::chrono::steady_clock::now() - prepare_begin);
    CHECK(arena.Seal());
    const auto cues = track.Cues();
    std::vector<std::string> folded, plain;
    for (const auto& cue : cues) { folded.push_back(Form(cue.text, text::kFold | text::kMarkup)); plain.push_back(Form(cue.text, text::kMarkup)); }
    std::vector<std::string> queries(std::begin(kWords), std::end(kWords));
    queries.insert(queries.end(), corpus.pairs.begin(), corpus.pairs.end());
    for (const char* extra : {"a", "e", "ש", "ו", "字", "о", "ا", " ", "th", "zzzq", "אבגדהוז", "不存在", "quiet storm never", "HELLO world", "שקט", "حب", "καλημέρα", "привет мир"}) queries.emplace_back(extra);
    // Expected cue lists from the brute-force scan, for both modes.
    std::vector<std::vector<unsigned>> expected[2];
    for (unsigned mode = 0; mode < 2; ++mode)
        for (const auto& query : queries) {
            const auto needle = Form(query, mode ? 0u : unsigned{text::kFold}), folded_needle = Fold(query); std::vector<unsigned> list;
            if (folded_needle != " ")
                for (unsigned i = 0; i < cues.size(); ++i)
                    if (folded[i].find(folded_needle) != std::string::npos && (mode ? plain[i] : folded[i]).find(needle) != std::string::npos) list.push_back(i);
            expected[mode].push_back(std::move(list));
        }
    unsigned wrong = 0, matched_queries = 0; std::size_t total_matches = 0;
    double slowest = 0; std::size_t slowest_query = 0;
    allocation_probe::Begin();
    for (unsigned mode = 0; mode < 2; ++mode)
        for (std::size_t q = 0; q < queries.size(); ++q) {
            const auto& list = expected[mode][q]; const auto kind = mode ? MatchMode::Exact : MatchMode::Folded;
            const auto size = static_cast<unsigned>(list.size());
            if (index.Count(queries[q], kind) != size) ++wrong;
            SearchMatch match;
            for (const unsigned k : {0u, size / 2, size ? size - 1 : 0u}) {
                if (k >= size) continue;
                if (!index.Find(queries[q], k, match, kind) || match.cue != list[k] || match.start != cues[list[k]].start || match.end != cues[list[k]].end) ++wrong;
            }
            if (index.Find(queries[q], size, match, kind)) ++wrong;
            if (mode) continue;
            if (size) { ++matched_queries; total_matches += size; }
            // Time: the first match, the last one (the scan that reads the most) and one past it (the whole track).
            for (const unsigned k : {0u, size ? size - 1 : 0u, size}) {
                double best = 1e9;
                for (unsigned run = 0; run < 7; ++run) {
                    const auto begin = std::chrono::steady_clock::now();
                    (void)index.Find(queries[q], k, match);
                    best = (std::min)(best, Microseconds(std::chrono::steady_clock::now() - begin));
                }
                if (best > slowest) { slowest = best; slowest_query = q; }
            }
        }
    const auto allocations = allocation_probe::End();
    CHECK(wrong == 0 && allocations == 0 && matched_queries > 50);
    std::printf("Full track: %zu cues, %zu bytes of text, index %zu bytes built in %.1f ms; %zu queries (%zu with matches, %zu matches) agree with brute force\n",
                cues.size(), corpus.source.size(), index.Bytes(), prepare / 1000, queries.size(), static_cast<std::size_t>(matched_queries), total_matches);
    std::printf("Slowest lookup: %.1f us (\"%s\", %zu matching cues)%s\n", slowest, queries[slowest_query].c_str(), expected[0][slowest_query].size(),
                kTimed ? "" : " [not asserted in this build]");
    if (kTimed) CHECK(slowest < 1000);
}
// The least favourable track the parser admits: 8 MiB of cues near the 4096-byte limit, whose signatures all fill up,
// searched for words that are not there. Reported, not asserted: see docs/subtitle-search.md.
void Saturated() {
    const auto corpus = Build(1900, 400, 0);
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(40 * 1024 * 1024));
    SubtitleTrack track; SearchIndex index; SearchMatch match;
    CHECK(corpus.source.size() < 8u * 1024 * 1024 && track.Prepare(arena, corpus.source) && index.Prepare(arena, track) && arena.Seal());
    double worst = 0;
    for (const char* query : {"never story again", "שלום שלום עולם עולם", "光 夜 世界 中文"}) {
        double best = 1e9;
        for (unsigned run = 0; run < 5; ++run) {
            const auto begin = std::chrono::steady_clock::now();
            (void)index.Find(query, 1u << 30, match);
            best = (std::min)(best, Microseconds(std::chrono::steady_clock::now() - begin));
        }
        worst = (std::max)(worst, best);
    }
    std::printf("Saturated track: %zu cues, %zu bytes; slowest full scan %.1f us\n", track.Cues().size(), corpus.source.size(), worst);
}
// Several languages as channels: a quote is looked for in the language shown, and the index follows a switch at once.
void Channels() {
    media::core::LinearMemoryArena arena; CHECK(arena.Initialize(1 << 20));
    SubtitleTrack track; SearchIndex index;
    const std::string_view sources[]{"1\n00:00:01,000 --> 00:00:02,000\nשלום, world\n\n2\n00:00:05,000 --> 00:00:06,000\nלילה טוב\n",
                                     "1\n00:00:01,500 --> 00:00:02,500\nHello world\n\n2\n00:00:05,500 --> 00:00:06,500\nGood night world\n"};
    CHECK(track.Prepare(arena, sources) && index.Prepare(arena, track));
    SearchMatch match{};
    allocation_probe::Begin();
    CHECK(index.Count("world") == 3);                     // Every channel is shown after Prepare.
    track.Show(1u << 1);
    CHECK(index.Count("world") == 2 && index.Find("world", 0, match) && match.start == 15000000 && index.Count("שלום") == 0);
    track.Show(1u << 0);
    CHECK(index.Count("world") == 1 && index.Find("לילה", 0, match) && match.start == 50000000 && !index.Find("good night", 0, match));
    track.Show(0);
    CHECK(index.Count("world") == 0);
    CHECK(allocation_probe::End() == 0);
}
}
int main() {
    Folding(); Multilingual(); FullTrack(); Saturated(); Channels();
    std::printf("Subtitle search: %d failures\n", failures); return failures ? 1 : 0;
}
