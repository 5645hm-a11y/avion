#pragma once
#include "media/subtitle/SubtitleTrack.hpp"

namespace media::subtitle {
// Both modes search the cue text as displayed (tags dropped, entities decoded, line breaks as spaces): see
// text::Normalize. Folded also ignores letter case, Hebrew points and Arabic harakat; Exact keeps them.
enum class MatchMode : std::uint8_t { Folded, Exact };
struct SearchMatch { unsigned cue = 0; std::int64_t start = 0, end = 0; }; // Cue index in the track, and its interval.

// Substring search over a prepared track, built once in the caller's arena:
// - the folded text of every cue, back to back (never more than the cue text itself), with one offset per cue;
// - one 256-bit signature per cue of the letters and letter pairs in that folded text.
// A query is folded on the stack and compared with the signatures in cue order; only cues whose signature admits
// it are searched. Nothing is allocated and nothing is written after Prepare, so any thread may search.
// Matches are cues, in the track's order (start time): a cue containing the query twice counts once. Only the cues of
// the channels the track shows are searched (SubtitleTrack::Show): a quote is looked for in the language on screen.
class SearchIndex final {
public:
    static constexpr unsigned kMaxQueryBytes = 256;
    // Preparing only. The track must outlive the index.
    bool Prepare(core::LinearMemoryArena&, const SubtitleTrack&) noexcept;
    bool Ready() const noexcept { return signatures_ != nullptr; }
    std::size_t Bytes() const noexcept { return bytes_; } // Arena bytes held.
    // The `match_index`-th matching cue. False: no such match, or a query that is blank, longer than kMaxQueryBytes
    // or not UTF-8.
    bool Find(std::string_view query, unsigned match_index, SearchMatch&, MatchMode = MatchMode::Folded) const noexcept;
    unsigned Count(std::string_view query, MatchMode = MatchMode::Folded) const noexcept;
    // Cue `cue`'s text in folded form ("" for a cue that does not normalise), and the track (after Prepare).
    std::string_view Folded(unsigned cue) const noexcept;
    const SubtitleTrack* Track() const noexcept { return track_; }
private:
    struct Signature { std::uint64_t letters = 0, pairs[3]{}; };
    static bool Sign(std::string_view folded, Signature&) noexcept;
    unsigned Scan(std::string_view query, unsigned match_index, SearchMatch*, MatchMode) const noexcept;
    const SubtitleTrack* track_ = nullptr;
    Signature* signatures_ = nullptr;
    std::uint32_t* offsets_ = nullptr; // Cue i's folded text is folded_[offsets_[i], offsets_[i + 1]).
    char* folded_ = nullptr;
    std::size_t bytes_ = 0;
};
}
