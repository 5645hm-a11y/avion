// Concept search over subtitles (ai::ConceptLexicon, ai::ConceptIndex; docs/subtitle-search.md, "Concept search"): the
// table itself (well formed, under 32 KB), how words are read (prefixes, final letters, endings, phrases, the Arabic
// article), ranking on a dialogue track in Hebrew and English where the quote search finds nothing, the channels shown,
// refusals, zero allocations and the lookup time. With a path argument (assets/test_fixtures/big_buck_bunny.srt), the
// same on the real file's Hebrew, English and Arabic lines.
#include "media/ai/ConceptSearch.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
using namespace media;
using media::ai::ConceptHit; using media::ai::ConceptIndex; using media::ai::ConceptLexicon; using media::ai::ConceptMatch;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (false)
#if defined(__SANITIZE_ADDRESS__) || (defined(_MSC_VER) && defined(_DEBUG)) || (!defined(_MSC_VER) && !defined(__OPTIMIZE__))
constexpr bool kTimed = false;
#else
constexpr bool kTimed = true;
#endif
// The concepts of one word or phrase, by name ("" when it names none).
std::string Concepts(std::string_view word) {
    ConceptHit hits[8]; std::string names;
    const auto n = ConceptLexicon::Word(word, hits);
    for (unsigned i = 0; i < n; ++i) {
        if (!names.empty()) names += ',';
        names += ConceptLexicon::Name(hits[i].id); names += ':'; names += static_cast<char>('0' + hits[i].weight);
    }
    return names;
}
bool Names(std::string_view word, std::string_view name, unsigned weight) {
    ConceptHit hits[8];
    const auto n = ConceptLexicon::Word(word, hits);
    for (unsigned i = 0; i < n; ++i) if (ConceptLexicon::Name(hits[i].id) == name) return hits[i].weight == weight;
    std::printf("  \"%.*s\" -> [%s], not %.*s\n", static_cast<int>(word.size()), word.data(), Concepts(word).c_str(), static_cast<int>(name.size()), name.data());
    return false;
}

void Lexicon() {
    CHECK(ConceptLexicon::TablesConsistent([](const char* rule, std::string_view word) noexcept {
        std::printf("  table: %s: \"%.*s\"\n", rule, static_cast<int>(word.size()), word.data());
    }));
    std::printf("Lexicon: %u concepts, %u words and phrases, %zu bytes (source text and index)\n", ConceptLexicon::Concepts(), ConceptLexicon::Entries(),
                ConceptLexicon::Bytes());
    CHECK(ConceptLexicon::Concepts() >= 90 && ConceptLexicon::Concepts() < ConceptLexicon::kMaxConcepts);
    CHECK(ConceptLexicon::Entries() >= 1000);
    CHECK(ConceptLexicon::Bytes() < 32 * 1024);
    CHECK(ConceptLexicon::Name(1) == "danger" && ConceptLexicon::Name(0).empty() && ConceptLexicon::Name(1000).empty());
    // The directive's examples, both ways.
    for (const char* w : {"danger", "dangerous", "careful", "carefully", "caution", "beware", "סכנה", "מסוכן", "זהירות", "תיזהרו", "خطر", "انتبه"})
        CHECK(Names(w, "danger", 3));
    for (const char* w : {"trap", "traps", "מלכודת", "מלכודות", "במלכודת", "فخ"}) CHECK(Names(w, "danger", 2));
    for (const char* w : {"car", "cars", "automobile", "truck", "רכב", "מכונית", "מכוניות", "משאית", "והמכונית", "ברכב", "سيارة", "بالسيارة"})
        CHECK(Names(w, "vehicle", 3));
    for (const char* w : {"drive", "driving", "drove", "driver", "נהיגה", "נוהג"}) CHECK(Names(w, "vehicle", 2));
    // Phrases, alone and inside text.
    CHECK(Names("watch out", "danger", 3) && Names("שימו לב", "danger", 3) && Names("ושים לב", "danger", 3));
    ConceptHit hits[ConceptIndex::kMaxCueConcepts];
    auto n = ConceptLexicon::Analyse("hey, watch out! the car is coming", hits);
    CHECK(n == 3 && ConceptLexicon::Name(hits[0].id) == "danger" && ConceptLexicon::Name(hits[1].id) == "vehicle" &&
          ConceptLexicon::Name(hits[2].id) == "arrival");
    n = ConceptLexicon::Analyse("שימו לב! המשאית מגיעה", hits);
    CHECK(n == 3 && ConceptLexicon::Name(hits[0].id) == "danger" && ConceptLexicon::Name(hits[1].id) == "vehicle");
    // One concept once, with its strongest weight: a trap (2) and then caution (3) is danger at 3.
    n = ConceptLexicon::Analyse("a trap. careful!", hits);
    CHECK(n == 1 && hits[0].weight == 3);
    // Hebrew prefixes are taken off only when the word itself is not in the table, the shortest reading first.
    CHECK(Names("כלב", "dog", 3) && Concepts("כלב").find("love") == std::string::npos); // Never כ + לב.
    CHECK(Concepts("שים").empty() && Concepts("שבת").empty() && Concepts("בקרב").empty()); // Stop words, not ש + ים, ש + בת, ב + קרב.
    CHECK(Names("הארנב", "rabbit", 3) && Names("וכשהארנב", "rabbit", 3) && Concepts("ושכהבלמארנב").empty()); // At most four prefixes.
    CHECK(Names("בבית", "home", 3) && Names("לבן", "color", 3) && Concepts("לבן").find("child") == std::string::npos);
    // Final letters: "מלך" is listed with its final letter and found through a prefix.
    CHECK(Names("ממלכה", "royal", 3) && Names("מלך", "royal", 3) && Names("המלך", "royal", 3) && Names("והמלך", "royal", 3));
    CHECK(Names("לבית הספר", "school", 3) && Names("בבית החולים", "medical", 3)); // A phrase with the article inside.
    // English endings and the possessive.
    CHECK(Names("rabbit's", "rabbit", 3) && Names("bunnies", "rabbit", 3) && Concepts("stopped").empty());
    CHECK(Names("snapped", "breaking", 3) && Names("chasing", "escape", 3) && Names("hurrying", "escape", 2));
    // Arabic: the article and prefixes.
    CHECK(Names("السناجب", "squirrel", 3) && Names("والسيارة", "vehicle", 3) && Names("المشاغبة", "annoyance", 3));
    // Stop words are no concept and say so.
    bool stop = false;
    CHECK(ConceptLexicon::Word("the", hits, &stop) == 0 && stop);
    CHECK(ConceptLexicon::Word("של", hits, &stop) == 0 && stop);
    CHECK(ConceptLexicon::Word("xylophone", hits, &stop) == 0 && !stop);
    // Text with no spaces between words has no concepts (and is not misread).
    CHECK(ConceptLexicon::Analyse("危险！小心！", hits) == 0);
}

// Dialogue in Hebrew and English (two channels), none of which contains the words searched for.
constexpr const char* kHebrew =
    "1\n00:00:01,000 --> 00:00:03,000\nבוקר טוב! הכלב מתעורר.\n\n"
    "2\n00:00:05,000 --> 00:00:07,000\nזהירות! הגשר עלול להתמוטט.\n\n"
    "3\n00:00:09,000 --> 00:00:11,000\nהוא הניח מלכודת ליד הנהר.\n\n"
    "4\n00:00:13,000 --> 00:00:15,000\nהמכונית האדומה חונה בחוץ.\n\n"
    "5\n00:00:17,000 --> 00:00:19,000\nמשאית ענקית חוסמת את הכביש.\n\n"
    "6\n00:00:21,000 --> 00:00:23,000\nהארנב ישן מתחת לעץ.\n\n"
    "7\n00:00:25,000 --> 00:00:27,000\nאמא, אני רעב! מה יש לארוחת ערב?\n\n"
    "8\n00:00:29,000 --> 00:00:31,000\nדני הגיע מאוחר לבית הספר.\n\n";
constexpr const char* kEnglish =
    "1\n00:00:02,000 --> 00:00:04,000\nWatch out, the bridge is falling apart!\n\n"
    "2\n00:00:06,000 --> 00:00:08,000\nShe was driving too fast on the highway.\n\n"
    "3\n00:00:10,000 --> 00:00:12,000\nAn old automobile rusts in the barn.\n\n"
    "4\n00:00:14,000 --> 00:00:16,000\nThe bunny is sleeping under the oak.\n\n"
    "5\n00:00:18,000 --> 00:00:20,000\nMy dog chased a squirrel all morning.\n\n"
    "6\n00:00:22,000 --> 00:00:24,000\nDanny's mother made pizza for dinner.\n\n"
    "7\n00:00:26,000 --> 00:00:28,000\nBe careful with that knife.\n\n";
// The track sorts cues by start: Hebrew cue n starts at 4n - 3 s, English cue n at 4n - 2 s.
constexpr unsigned He(unsigned n) { return 2 * (n - 1); }
constexpr unsigned En(unsigned n) { return 2 * (n - 1) + 1; }
std::vector<unsigned> All(const ConceptIndex& index, std::string_view query) {
    std::vector<unsigned> cues; ConceptMatch m;
    for (unsigned k = 0; index.Find(query, k, m); ++k) cues.push_back(m.cue);
    return cues;
}
bool First(const ConceptIndex& index, std::string_view query, unsigned cue) {
    ConceptMatch m;
    if (index.Find(query, 0, m) && m.cue == cue) return true;
    std::printf("  \"%.*s\": first match cue %d, expected %u\n", static_cast<int>(query.size()), query.data(), index.Find(query, 0, m) ? static_cast<int>(m.cue) : -1, cue);
    return false;
}
bool Set(const ConceptIndex& index, std::string_view query, std::vector<unsigned> expected) {
    auto got = All(index, query);
    std::vector<unsigned> sorted = got; std::sort(sorted.begin(), sorted.end()); std::sort(expected.begin(), expected.end());
    if (sorted == expected && got.size() == index.Count(query)) return true;
    std::printf("  \"%.*s\": cues", static_cast<int>(query.size()), query.data());
    for (const auto c : got) std::printf(" %u", c);
    std::printf(" (count %u), expected", index.Count(query));
    for (const auto c : expected) std::printf(" %u", c);
    std::printf("\n");
    return false;
}

void Dialogue() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    subtitle::SubtitleTrack track; subtitle::SearchIndex search; ConceptIndex index;
    const std::string_view sources[]{kHebrew, kEnglish};
    CHECK(track.Prepare(arena, sources) && search.Prepare(arena, track));
    CHECK(!index.Ready() && index.Count("danger") == 0);
    CHECK(index.Prepare(arena, search) && index.Ready());
    CHECK(!index.Prepare(arena, search)); // Once.
    std::printf("Dialogue track: %zu cues, concept index %zu bytes\n", track.Cues().size(), index.Bytes());
    CHECK(track.Cues().size() == 15 && index.Bytes() < 15 * 4 + 4 + 15 * 2 * ConceptIndex::kMaxCueConcepts);
    // The quote search finds none of these: this is what the fallback is for. ("car" it finds, in "careful": a quote is
    // a substring.)
    for (const char* q : {"danger", "סכנה", "רכב", "automobiles", "rabbit sleeping", "animal", "food"}) CHECK(search.Count(q) == 0);
    CHECK(search.Count("car") == 1);
    // "danger" / "סכנה": caution first (weight 3, in time order), then the trap (2).
    CHECK(Set(index, "danger", {He(2), En(1), En(7), He(3)}));
    CHECK(All(index, "danger") == (std::vector<unsigned>{En(1), He(2), En(7), He(3)}));
    CHECK(All(index, "סכנה") == All(index, "danger") && All(index, "خطر") == All(index, "danger"));
    ConceptMatch m;
    CHECK(index.Find("סכנה", 1, m) && m.cue == He(2) && m.by_concept && m.coverage == 1 && m.terms == 1 && m.score == 9 && m.confidence == 1.0f &&
          m.start == track.Cues()[He(2)].start && m.end == track.Cues()[He(2)].end);
    CHECK(index.Find("סכנה", 3, m) && m.cue == He(3) && m.score == 6 && m.confidence > 0.66f && m.confidence < 0.67f);
    CHECK(!index.Find("סכנה", 4, m));
    // "car" / "רכב": the cars and the truck (3), then driving (2); the road (1) alone does not count.
    CHECK(All(index, "car") == (std::vector<unsigned>{En(3), He(4), He(5), En(2)}));
    CHECK(All(index, "רכב") == All(index, "car") && All(index, "automobile") == All(index, "car"));
    // Two terms: the cues that have both come first (the earlier of two equal ones first), in either language.
    CHECK(All(index, "rabbit sleeping") == (std::vector<unsigned>{En(4), He(6)}) && All(index, "ארנב ישן") == All(index, "rabbit sleeping"));
    CHECK(index.Find("sleeping rabbit", 0, m) && m.coverage == 2 && m.terms == 2);
    // "rabbit" is no "dog": the general animal tie (1 x 1) does not count; "animal" itself finds every animal.
    CHECK(Set(index, "rabbit", {He(6), En(4)}));
    CHECK(Set(index, "animal", {He(1), He(6), En(4), En(5)}));
    CHECK(Set(index, "חיות", {He(1), He(6), En(4), En(5)}));
    // Hebrew and English meet on a concept: mother, food, school, time.
    CHECK(Set(index, "mom", {He(7), En(6)}) && Set(index, "אמא", {He(7), En(6)}));
    CHECK(Set(index, "hungry", {He(7), En(6)}));
    CHECK(Set(index, "late", {He(8)}) && Set(index, "school", {He(8)}));
    // A word the table does not know is looked for as a word, with prefixes and endings: "גשר" in "הגשר", "bridge".
    CHECK(Set(index, "גשר", {He(2)}) && Set(index, "bridges", {En(1)}) && Set(index, "bridge", {En(1)}) && Set(index, "barns", {En(3)}));
    CHECK(index.Find("bridge", 0, m) && !m.by_concept && m.score == 9);
    // A word and a concept: the cue with both first.
    CHECK(First(index, "danger bridge", En(1)) && First(index, "סכנה גשר", He(2)));
    // Refusals: nothing but stop words, blank, too long, not UTF-8.
    CHECK(index.Count("the of and") == 0 && !index.Find("the", 0, m) && index.Count("של את") == 0);
    CHECK(index.Count("") == 0 && index.Count("   ") == 0 && index.Count("\xff\xfe") == 0);
    CHECK(index.Count(std::string(subtitle::SearchIndex::kMaxQueryBytes + 1, 'a')) == 0);
    CHECK(index.Count("zzzz qqqq") == 0);
    // Only the channels shown are searched: English alone, then Hebrew alone.
    track.Show(2u);
    CHECK(All(index, "danger") == (std::vector<unsigned>{En(1), En(7)}) && Set(index, "רכב", {En(2), En(3)}));
    track.Show(1u);
    CHECK(All(index, "danger") == (std::vector<unsigned>{He(2), He(3)}));
    track.Show(0u);
    CHECK(index.Count("danger") == 0);
    track.Show(3u);
    // Every lookup reads the stack only.
    allocation_probe::Begin();
    unsigned seen = 0;
    for (unsigned k = 0; k < 4; ++k) seen += index.Find("סכנה", k, m) ? 1u : 0u;
    seen += index.Count("car") + index.Count("rabbit sleeping") + index.Count("גשר");
    const auto allocations = allocation_probe::End();
    CHECK(seen == 4 + 4 + 2 + 1 && allocations == 0);
}

// A full-size track: 4000 cues of mixed dialogue, every ranking in non-increasing order, distinct cues, and the time.
void Scale() {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8 * 1024 * 1024));
    std::string source;
    const char* lines[]{"Careful, the truck is coming!", "אמא מבשלת ארוחת ערב", "The rabbit sleeps.", "שימו לב לכביש", "What a quiet morning",
                        "הוא נהג במונית כל הלילה", "Nothing to see here", "השוטר עצר את הגנב", "I love this song", "מזל טוב ליום ההולדת"};
    for (unsigned i = 0; i < 4000; ++i) {
        const unsigned s = i * 3;
        char header[96];
        std::snprintf(header, sizeof(header), "%u\n%02u:%02u:%02u,000 --> %02u:%02u:%02u,500\n", i + 1, s / 3600, s / 60 % 60, s % 60, s / 3600, s / 60 % 60, s % 60 + 1);
        source += header; source += lines[(i * 7) % 10]; source += " #"; source += std::to_string(i); source += "\n\n";
    }
    subtitle::SubtitleTrack track; subtitle::SearchIndex search; ConceptIndex index;
    CHECK(track.Prepare(arena, source) && search.Prepare(arena, track) && index.Prepare(arena, search));
    for (const char* q : {"danger", "רכב", "food", "police", "celebration", "danger car", "quiet"}) {
        const auto count = index.Count(q);
        ConceptMatch previous{}, m{}; unsigned listed = 0; bool ordered = true;
        for (unsigned k = 0; k < 12 && index.Find(q, k, m); ++k, ++listed) {
            if (k && (m.coverage > previous.coverage || (m.coverage == previous.coverage && (m.score > previous.score ||
                (m.score == previous.score && m.cue <= previous.cue))))) ordered = false;
            previous = m;
        }
        CHECK(ordered && listed == (count < 12 ? count : 12u) && count > 0);
    }
    CHECK(index.Count("danger") == 800 && index.Count("police") == 400 && index.Count("celebration") == 400 && index.Count("רכב") == 1200);
    // Lookup time: the first match and the count, over 4000 cues.
    double best_find = 1e9, best_count = 1e9;
    for (unsigned r = 0; r < 20; ++r) {
        ConceptMatch m;
        auto t0 = std::chrono::steady_clock::now(); (void)index.Find("סכנה רכב", 0, m);
        auto t1 = std::chrono::steady_clock::now(); (void)index.Count("סכנה רכב");
        auto t2 = std::chrono::steady_clock::now();
        best_find = (std::min)(best_find, std::chrono::duration<double, std::micro>(t1 - t0).count());
        best_count = (std::min)(best_count, std::chrono::duration<double, std::micro>(t2 - t1).count());
    }
    std::printf("4000 cues: concept index %zu bytes; first match %.1f us, count %.1f us\n", index.Bytes(), best_find, best_count);
    if (kTimed) CHECK(best_find < 2000 && best_count < 2000);
}

// The real file: Hebrew cues with English, Arabic and Chinese lines (assets/test_fixtures/big_buck_bunny.srt).
void Fixture(const char* path) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path, "rb") || !f) { std::printf("Fixture: %s not readable, skipped\n", path); return; }
#else
    if (!(f = std::fopen(path, "rb"))) { std::printf("Fixture: %s not readable, skipped\n", path); return; }
#endif
    std::string source; char buffer[4096]; std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) source.append(buffer, n);
    std::fclose(f);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    subtitle::SubtitleTrack track; subtitle::SearchIndex search; ConceptIndex index;
    CHECK(track.Prepare(arena, source) && search.Prepare(arena, track) && index.Prepare(arena, search));
    // Cue indices as in the file, from 0: 5 "זהירות! שלושת הסנאים...", 9 "...מלכודות...", 10 "...נלכד במלכודת" / "trap".
    CHECK(search.Count("סכנה") == 0 && search.Count("danger") == 0);
    CHECK(First(index, "סכנה", 5) && First(index, "danger", 5) && Set(index, "danger", {5, 9, 10}));
    CHECK(First(index, "sleeping rabbit", 2) && First(index, "ארנב ישן", 2)); // "הארנב ישן שנת ישרים."
    CHECK(First(index, "fruit", 4) && First(index, "revenge", 9) && First(index, "נקמה", 9)); // The apple; "זמן לנקמה!".
    CHECK(First(index, "butterfly", 7) && First(index, "protect the butterfly", 7)); // "הגנו על הפרפר!"
    CHECK(index.Count("car") == 0 && index.Count("רכב") == 0); // No vehicle in this film.
    ConceptMatch m;
    CHECK(index.Find("סכנה", 0, m) && m.by_concept && m.confidence == 1.0f);
    std::printf("Fixture: %zu cues; \"סכנה\" -> cue %u of %u matches; concept index %zu bytes\n", track.Cues().size(), m.cue, index.Count("סכנה"), index.Bytes());
}
}
int main(int argc, char** argv) {
    Lexicon();
    Dialogue();
    Scale();
    if (argc > 1) Fixture(argv[1]);
    std::printf("Semantic search: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
