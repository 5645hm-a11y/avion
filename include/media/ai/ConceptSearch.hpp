#pragma once
#include "media/subtitle/SubtitleSearch.hpp"
#include <cstdint>
#include <span>
#include <string_view>

namespace media::ai {
// Concept search over subtitles (docs/subtitle-search.md, "Concept search"): what a quote search falls back to when the
// words typed or said are in no cue. "danger" finds "careful", "watch out", "זהירות" and "מלכודת"; "רכב" finds "driving",
// "automobile", "מכונית" and "משאית".
//
// ConceptLexicon: a fixed table of about a hundred concepts. Each one lists its member words in English and Hebrew (and
// a few in Arabic), each with a weight: 3 says the same thing (danger: "careful", "סכנה"), 2 is closely tied (danger:
// "trap"), 1 is loosely related. Stop words ("the", "של") have weight 0 and no concept. The table is one source text,
// indexed once (on first use, in static storage) into entries sorted by word; nothing is allocated, nothing is written
// after that, and the whole of it is Bytes() (under 32 KB).
// Words are read from folded text (text::Normalize with kFold: lower case, no Hebrew points or Arabic harakat):
// - Hebrew final letters match their ordinary forms, and up to four prefix letters (ו ה ב ל מ ש כ) are taken off a word
//   that is not itself in the table ("והמכונית" is "מכונית"; "כלב" stays "dog", never "לב"). A few stop words only keep
//   a word from that reading ("שים" is not ש + "ים", sea);
// - Arabic: the article and the conjunction/preposition prefixes (ال و ف ب ل ك) likewise;
// - English: plural, possessive, -ing, -ed and -ly forms ("driving" is "drive", "carefully" is "careful");
// - two-word phrases are members too ("watch out", "שים לב").
// Chinese and Japanese text, which has no spaces between words, has no concepts.
struct ConceptHit { std::uint8_t id = 0, weight = 0; }; // Concept ids start at 1.
class ConceptLexicon final {
public:
    static constexpr unsigned kMaxConcepts = 128;
    // The concepts in `folded` text: each distinct concept once, with the strongest weight any of its words had, in the
    // order first seen. Returns how many there are (at most out.size(): the rest are left out).
    static unsigned Analyse(std::string_view folded, std::span<ConceptHit> out) noexcept;
    // One word or two-word phrase (folded): its concepts as Analyse reads them. 0 also for a stop word (`stop` set).
    static unsigned Word(std::string_view folded, std::span<ConceptHit> out, bool* stop = nullptr) noexcept;
    static unsigned Concepts() noexcept;     // Concepts defined.
    static unsigned Entries() noexcept;      // Words and phrases indexed (stop words included).
    static std::size_t Bytes() noexcept;     // The source text and its index.
    static std::string_view Name(unsigned id) noexcept; // "danger", "vehicle", ... ("" out of range).
    // The table is well formed: every member parses, nothing overflows the index, no word is listed twice in one
    // concept, and every word is already in folded form. `report` is told each violation.
    using TableReport = void (*)(const char* rule, std::string_view word) noexcept;
    static bool TablesConsistent(TableReport report = nullptr) noexcept;
};

// The cues of a prepared subtitle::SearchIndex ranked by what a query means. Prepare reads every cue once and keeps its
// concepts in the caller's arena (4 bytes a cue and 2 a concept found); a query is read on the stack, and nothing is
// allocated or written after Prepare, so any thread may search.
// A query is made of terms, one per word that is no stop word (a phrase of the table is one word):
// - a word of the table stands for its concepts, and a cue scores the best product of the cue's weight and the query's
//   weight over them: "rabbit" (rabbit 3, animal 1) scores 9 in a cue saying "ארנב" and only 1 in one saying "dog";
// - any other word (a name, a word the table does not know) must be in the cue as a word, with the prefixes and endings
//   above, and scores 9.
// A term counts in a cue from a score of 2. A cue's coverage is the number of terms that count in it, its score their sum.
// Cues rank by coverage, then score, then time; a cue where one term counts is a match. Only the channels the track shows
// are searched, as the quote search does. Confidence is the score over the best possible (9 a term).
struct ConceptMatch {
    unsigned cue = 0; std::int64_t start = 0, end = 0;
    unsigned coverage = 0, terms = 0, score = 0;
    float confidence = 0;
    bool by_concept = false; // A concept counted (else only words the query and the cue share).
};
class ConceptIndex final {
public:
    static constexpr unsigned kMaxTerms = 12, kMaxCueConcepts = 24;
    // Preparing only. The index (and its track) must outlive this.
    bool Prepare(core::LinearMemoryArena&, const subtitle::SearchIndex&) noexcept;
    bool Ready() const noexcept { return offsets_ != nullptr; }
    std::size_t Bytes() const noexcept { return bytes_; } // Arena bytes held.
    // The `match_index`-th cue in rank order. False: no such cue, or a query that is blank, too long, not UTF-8, or has no
    // term (only stop words).
    bool Find(std::string_view query, unsigned match_index, ConceptMatch&) const noexcept;
    unsigned Count(std::string_view query) const noexcept;
    // The concepts found in cue `cue` (tests and diagnostics).
    std::span<const ConceptHit> CueConcepts(unsigned cue) const noexcept;
private:
    struct Terms;
    bool Read(std::string_view query, Terms&) const noexcept;
    std::uint64_t Rank(const Terms&, unsigned cue, ConceptMatch*) const noexcept; // 0: the cue has no term.
    unsigned Scan(const Terms&, unsigned match_index, ConceptMatch*) const noexcept;
    const subtitle::SearchIndex* search_ = nullptr;
    std::uint32_t* offsets_ = nullptr; // Cue i's concepts are hits_[offsets_[i], offsets_[i + 1]).
    ConceptHit* hits_ = nullptr;
    unsigned cues_ = 0;
    std::size_t bytes_ = 0;
};
}
