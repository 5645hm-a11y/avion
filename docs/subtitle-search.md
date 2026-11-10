# Subtitle quote search (`subtitle::SearchIndex`, `PlaybackCoordinator::SeekToSubtitle`)

Type a few words of a line, land on the moment it is spoken. The search runs over the subtitle track already loaded for display, in any script, and ends in an ordinary engine seek to the cue's start.

```cpp
bool PlaybackCoordinator::SeekToSubtitle(const char* query, unsigned match_index = 0) noexcept;
```

`match_index` counts matching cues in time order: 0 is the first cue containing the quote, 1 the next. The call returns false when there are no subtitles, no such match, or the seek was refused; `LastSubtitleSeek()` tells which, with the cue, its start and the lookup time.

When no cue contains the quote, the cues that **mean** it are searched instead, best first ([concept search](#concept-search-2026-10-20)): `LastSubtitleSeek().by_concept` and `.confidence` say so.

## What is searched

The text a viewer sees, not the file's bytes. `text::Normalize` (`include/media/text/Utf8.hpp`) produces it, for cues and queries alike:

| Rule | Applies to | Example |
|---|---|---|
| Any white space is one space, and a run is one space | both modes | a line break inside a cue matches a typed space |
| Invisible format controls are dropped: soft hyphen, zero-width characters, bidi marks, embeddings and isolates | both modes | the right-to-left marks common in Hebrew subtitle files |
| Cue markup: `<tags>` dropped, the WebVTT entities decoded | cues only | `<i>Tom &amp; Jerry</i>` is found by `tom & jerry` |
| Letter case: ASCII, Latin-1, Latin Extended-A, Greek (final sigma too), Cyrillic, Armenian, fullwidth Latin | Folded only | `ПРИВЕТ` finds `Привет` |
| Hebrew points and cantillation, Arabic harakat and tatweel; alef with madda or hamza is a bare alef | Folded only | `שלום` finds `שָׁלוֹם`; `مرحبا` finds `مَرْحَبًا` |

`MatchMode::Folded` is what `SeekToSubtitle` uses. `MatchMode::Exact` keeps case and points: `שלום` then does not match the pointed line.

A query is plain text, so its `<` and `&` stay as typed. The folded form is never longer than its input, and folding it again changes nothing; the test checks both for every scalar up to U+2FFFF.

Matching is by substring of scalars, with no word segmentation, so one ideograph is a valid query and Thai or Japanese need no dictionary. A cue containing the quote twice is one match.

## The index

Built once in `Preparing`, from the arena, right after the track is parsed:

- the folded text of every cue, back to back (never more than the cue text itself), with one 4-byte offset per cue;
- one 256-bit signature per cue: a 64-bit set of hashed letters and a 192-bit set of hashed letter pairs of that folded text.

A lookup folds the query on the stack (at most 256 bytes), builds its signature, and walks the cues in time order. A cue whose signature lacks any bit of the query's cannot contain it and is skipped without touching its text; the others are searched in their folded text. Exact mode then confirms a folded match against the cue's unfolded text, normalised on the stack.

Nothing is allocated and nothing is written after `Prepare`, so the index is safe to read from any thread. It is independent of the GPU subtitle pipeline: search works even when that pipeline failed.

Queries that are blank, longer than 256 bytes or not UTF-8 match nothing.

## Landing

`SeekToSubtitle` sends a normal epoch seek to the cue's `start` ([seeking](seeking.md)). The landing frame is the frame on screen at that instant: `pts ≤ start < pts + duration`. A cue rarely starts on a frame boundary, so that frame usually begins slightly before the cue.

To make the quote visible on the frame the search lands on, a landing frame now stands for its seek target: when the target lies inside the frame's interval, captions and the dock's position are those of the target, not of the frame's first instant (`D3D12Renderer::BeginEpoch(epoch, now, target)`). This holds while paused too, so a search from a paused player shows the line it found. Every seek gets this, not only searches.

## Measured

Release, MSVC x64, on the development machine; `media_subtitle_search_tests` and `media_playback_intent_test`.

| Track | Text | Index | Build | Slowest lookup |
|---|---|---|---|---|
| 16384 cues (the track's maximum), nine scripts | 1.6 MiB | 1.5 MiB | 16 ms | 0.31 ms (MSVC), 0.42 ms (GCC `-O2`, WSL) |
| The fixture's 3 cues, during playback | — | 223 bytes | 0.003 ms | 6–7 µs from command text to queued seek |

"Slowest" is the largest, over 82 queries, of three lookups each: the first match, the last match (the scan that reads the most) and one past it (the whole track). Each lookup is timed seven times and the fastest kept, so the figure is the computation, not the scheduler. The slowest queries were a single ideograph present in 2610 cues and a single Hebrew letter present in 6816.

Unoptimised builds are slower and are not asserted: 0.88 ms in MSVC Debug, 1.5 ms under ASan/UBSan.

**The one case above a millisecond.** A track built against the index: 1900 cues of about 3.5 KiB each (6.7 MiB, near the parser's 8 MiB and 4096-byte limits), every cue using the whole vocabulary so that all signatures fill up, searched for a phrase that is almost never there. The signatures then exclude nothing and the lookup reads all the folded text: 1.1 ms with MSVC, 3.6 ms with GCC. Reported by the test, not asserted. No real subtitle file looks like this: a feature film is 1500 to 2500 cues and about 100 KiB.

## Verification

| Test | Checks |
|---|---|
| `media_subtitle_search_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | **Folding** script by script (Latin, Greek, Cyrillic, Armenian, Hebrew, Arabic, CJK, Korean, Devanagari, Thai), markup, invalid UTF-8, and the two properties above for every scalar. **Multilingual track** deliberately out of order in the file: matches in time order, each with its cue's exact start and end; Folded against Exact in English, Hebrew and Arabic; quotes across a tag and a line break; refused queries. **Full-size track:** every answer of 82 queries in both modes (count, first, middle and last match, one past the last) equals a brute-force scan; slowest lookup under 1 ms (asserted in optimised builds). Zero allocations in `Prepare` and in every lookup. |
| `media_playback_intent_test` | Two real quote searches during playback, one while playing and one while paused (`מצא עברית`, `find 日本語`): each lands on the frame containing its cue's start, and that frame is composited with the quoted cue. Zero allocations. |
| `media_playback_subtitle_test`, `media_playback_subtitle_scrub_test` | Seek and scrub landings show the cues of the seek target. |

```powershell
ctest --test-dir build -C Release -R "subtitle_search|intent" --output-on-failure
.\build\Release\media_player_demo.exe film.mp4 --subtitles film.srt --command "find the quiet between"
```

## Concept search (2026-10-20)

A quote that is in no cue is looked for by what it means. "danger" finds the cues saying "careful", "watch out", "זהירות" or "מלכודת"; "רכב" finds "driving", "automobile", "מכונית", "משאית". It is a fallback: `SeekToSubtitle` (and so the command bar, `find …`, `מצא …` and the follow-ups) tries the quote first, and only when **no** cue contains it does it ask `ai::ConceptIndex`. The landing is the same seek to the cue's start, and a status cue shows the line found, since the words asked for are not in it: `Concept match: "זהירות! שלושת הסנאים המציקים…"` (follow-ups: `Concept match 2 of 3: "…"`).

**The lexicon** (`ai::ConceptLexicon`, `src/ai/ConceptSearch.cpp`, portable):

- One source text of **92 concepts**, a line each: 1,739 words and two-word phrases (stop words included), English and Hebrew throughout, and Arabic for most concepts. Each word has a weight in its concept:
  - 3 says the same thing (danger: "careful", "סכנה");
  - 2 is closely tied (danger: "trap", "מלכודת"; vehicle: "driving");
  - 1 is loosely related (vehicle: "road").
- A word may belong to several concepts. Specific ones stand apart from their general concept, which lists them at weight 1: "rabbit" is its own concept and only loosely "animal", so it never finds "dog".
- Stop words ("the", "של", "في") have no concept.
- Indexed once, on first use, into static storage: entries sorted by word, each an offset into the source text. Nothing is allocated, and nothing is written after that.
- The source text and its index take **28,294 bytes** (under the 32 KB budget). `TablesConsistent` checks that every member parses, nothing overflows, no word is listed twice in a concept, and every word is in folded form.

**Reading words** (from folded text):

- **Hebrew:** a final letter matches its ordinary form. Up to four prefix letters (ו ה ב ל מ ש כ) are taken off a word that is not itself in the table, the shortest reading first: "והמכונית" is "מכונית", while "כלב" stays "dog" and never becomes "לב". A few stop words only keep a word from a false reading: "שים" is not ש + "ים" (sea), "שבת" not ש + "בת", "בקרב" not ב + "קרב".
- **Arabic:** the conjunctions (و ف), the prepositions (ب ل ك), the article (ال) and "لل".
- **English:** plural, possessive, -ies, -ing and -ed (with a doubled consonant: "snapped" is "snap") and -ly.
- **Phrases:** a two-word phrase is read before its words ("watch out", "שימו לב", "בית הספר" with the article inside).
- **Chinese and Japanese**, written without spaces, have no concepts.

**Ranking** (`ai::ConceptIndex`). `Prepare` reads every cue once and keeps its concepts in the arena: 4 bytes a cue and 2 a concept found (the film's 13 cues: 178 bytes; 4,000 cues: 32,804 bytes). A query is read on the stack into terms, one per word that is no stop word:

- a word of the table stands for its concepts, and a cue scores the best product of the cue's weight and the query's weight over them;
- any other word (a name, a word the table does not know) must be in the cue as a word, with the same prefixes and endings ("גשר" finds "הגשר", "bridges" finds "bridge"), and scores 9.

A term counts in a cue from a score of 2, so a weight-1 tie alone finds nothing. Cues rank by the number of terms they have, then by score, then by time; any cue where one term counts is a match. The ranking is computed by successive maxima, so no scratch is needed and any thread may search. Only the channels the track shows are searched, as with quotes.

**Measured** (Release): a two-concept query over 4,000 cues takes 63 µs for the first match and 59 µs for the count.

On the film's real lines (`big_buck_bunny.srt`):

| Query | Lands on |
|---|---|
| "סכנה" / "danger" | `זהירות! שלושת הסנאים המציקים מגיעים!` (weight 3), then the two trap cues (2) |
| "revenge" | `זמן לנקמה!` |
| "fruit falling" | `תפוח נופל מן העץ` |
| "protect the butterfly" | `הגנו על הפרפר!` |

No cue in it is about vehicles, and "car" finds nothing.

**Verification:**

| Test | Checks |
|---|---|
| `media_semantic_search_tests` (MSVC, GCC `-Werror`, ASan/UBSan) | The table: well formed, ≥ 90 concepts, ≥ 1,000 entries, under 32 KB. Word readings: the directive's examples both ways in three languages, phrases, prefixes (`כלב` not `לב`, at most four), finals, endings, the Arabic article, stop words. A two-channel Hebrew/English dialogue track where the quote search finds none of the queries: exact match order and scores for `danger`/`סכנה`/`خطر`, `car`/`רכב`/`automobile`, two-term queries, `rabbit` never finding `dog`, `animal`, `mom`/`אמא`, `hungry`, `late`, `school`, unknown words as words, a word and a concept together, refusals (stop words only, blank, too long, not UTF-8), each channel alone, none. 4,000 cues: every ranking in order, counts exact, lookups under 2 ms (asserted in optimised builds). The real film's lines when the file is present. 0 allocations. |
| `media_fixture_subtitle_search_test` | During real 4K HEVC playback of Big Buck Bunny, after the four quotes: "find revenge" (spoken-style) and "fruit falling" (typed into the bar) land exactly on their cues (5:00 and 1:23), with the target's captions on the landing frame, each marked as a concept match with its `Concept match: "…"` cue; the four quotes are not. 0 drops, 0 allocations. |

## Open

- Only sidecar SRT/WebVTT tracks are searchable, as only those are loaded.
- Concept search knows what its table lists: about 92 everyday concepts in English and Hebrew (Arabic for most), no other language, no sense of context ("kind of" is no kindness only because "kind" is a stop word). It is a closed table, not a language model.
- The quote search is a substring search: "car" finds "careful" before concept search is ever asked.
- In the window, searches are typed into the [command bar](command-palette.md) (`/`): text that is no command is looked for as a quote, and the bar shows the number of matching cues while typing. Enter lands on the first match; there is no next-match key yet.
- Case folding covers the scripts listed; Georgian, Cherokee, Deseret and the other cased scripts are matched exactly. Accented Latin letters are not matched by their base letter (`e` does not find `é`).
- No Unicode normalisation: a precomposed `é` does not match `e` followed by a combining accent.
- A lookup is bounded by the track, not by a time budget: see the saturated case above.
