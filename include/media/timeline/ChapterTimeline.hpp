#pragma once
#include <cstdint>
#include <string_view>

namespace media::timeline {
// Chapters and scenes of the media (docs/chapters.md). Portable, fixed storage, no allocation, single owner (the UI
// thread after Prepare): no synchronisation.
//
// - Chapters come from the file (Nero `udta/chpl`, or a QuickTime chapter track named by `tref/chap`) or, when it has
//   none, are synthesized at Prepare from its structure (Synthesize): they are then "sections".
// - Scenes are finer marks: structural ones (a sync sample out of the encoder's cadence, a jump in sync-sample size),
//   dialogue resuming after more than kDialogueGap of silence, and cuts seen while playing (AddScene, Live).
enum class ChapterSource : std::uint8_t { None, Nero, QuickTime, Synthesized };
enum class SceneOrigin : std::uint8_t { Structure, Dialogue, Live };
struct Chapter { std::int64_t start = 0; char title[56]{}; }; // 100 ns; UTF-8, NUL-terminated ("" without one).
struct Scene { std::int64_t at = 0; SceneOrigin origin = SceneOrigin::Structure; };

class ChapterTimeline final {
public:
    static constexpr unsigned kMaxChapters = 256, kMaxScenes = 1024;
    // "Previous" goes to the start of the current chapter or scene when more than this far into it, else to the one before.
    static constexpr std::int64_t kRestart = 30000000;
    // Two scenes closer than this are one (the earlier one stays, a live cut replaces a structural guess).
    static constexpr std::int64_t kSceneMerge = 10000000;
    void Clear() noexcept { *this = ChapterTimeline{}; }
    void SetDuration(std::int64_t duration) noexcept { duration_ = duration > 0 ? duration : 0; }
    std::int64_t Duration() const noexcept { return duration_; }
    // Kept in time order; a start already present (or past the duration, or negative) is refused. The title is cut at a
    // character boundary to fit, control characters are dropped, and a title that is not UTF-8 is left empty.
    bool AddChapter(std::int64_t start, std::string_view title) noexcept;
    void SetSource(ChapterSource source) noexcept { source_ = source; }
    ChapterSource Source() const noexcept { return source_; }
    unsigned Chapters() const noexcept { return chapter_count_; }
    const Chapter& ChapterAt(unsigned i) const noexcept { return chapters_[i < chapter_count_ ? i : 0]; }
    // A scene mark, merged with one already within kSceneMerge (a live cut takes the place of a structural one). False:
    // merged into an existing mark, out of range, or full.
    bool AddScene(std::int64_t at, SceneOrigin origin) noexcept;
    unsigned Scenes() const noexcept { return scene_count_; }
    const Scene& SceneAt(unsigned i) const noexcept { return scenes_[i < scene_count_ ? i : 0]; }
    unsigned LiveScenes() const noexcept { return live_; }
    // The chapter shown at `position` (the last one starting at or before it); -1 before the first.
    int ChapterIndex(std::int64_t position) const noexcept;
    // Navigation from `position`: the next mark strictly after it (beyond kLanding, so a landing frame that starts a
    // little before its mark does not find the same one again), or for Previous the current one's start when more than
    // kRestart into it, else the one before. False: none (the first or the last already).
    static constexpr std::int64_t kLanding = 2500000; // 0.25 s.
    bool NextChapter(std::int64_t position, unsigned& index) const noexcept;
    bool PreviousChapter(std::int64_t position, unsigned& index) const noexcept;
    bool NextScene(std::int64_t position, unsigned& index) const noexcept;
    bool PreviousScene(std::int64_t position, unsigned& index) const noexcept;
private:
    Chapter chapters_[kMaxChapters]{};
    Scene scenes_[kMaxScenes]{};
    unsigned chapter_count_ = 0, scene_count_ = 0, live_ = 0;
    std::int64_t duration_ = 0;
    ChapterSource source_ = ChapterSource::None;
};

// Structural synthesis at Prepare (docs/chapters.md, "Synthesis"). Deterministic: a pure function of its input.
struct Keyframe { std::int64_t pts = 0; std::uint32_t bytes = 0; };   // Sync samples, in decode order.
struct Interval { std::int64_t start = 0, end = 0; };                  // Dialogue cues, in time order.
struct SynthesisInput {
    const Keyframe* keyframes = nullptr; unsigned keyframe_count = 0;
    const Interval* cues = nullptr; unsigned cue_count = 0;
    std::int64_t duration = 0;
};
struct SynthesisResult { unsigned structural = 0, dialogue = 0, sections = 0; std::int64_t cadence = 0, spacing = 0; };
// More than this without dialogue, and the dialogue that resumes starts a scene (and is a section candidate).
constexpr std::int64_t kDialogueGap = 40000000;
// Scenes into `timeline` (Structure, Dialogue) and, when it has no chapters yet, sections as Synthesized chapters:
// - The cadence is the median interval between sync samples (a 10 ms histogram, up to 20 s). A sync sample that comes
//   before 3/4 of it is an encoder's scene cut (encoders insert one at a cut rather than wait for the cadence); so is one
//   at least 1.8 times as large as the previous one or smaller than 0.55 of it (a structural change of content).
// - Dialogue that resumes after more than kDialogueGap starts a scene at the sync sample at or before the resuming cue,
//   when one lies inside the gap, else at the cue itself.
// - Sections: one at the start, then candidates by strength (dialogue scene and cut together 3, dialogue 2, cut 1),
//   earlier first among equals, each at least `spacing` = clamp(duration / 12, 60 s, 10 min) from every section kept,
//   and none closer than spacing / 2 to the end. Files shorter than two spacings get no section after the start.
SynthesisResult Synthesize(const SynthesisInput&, ChapterTimeline&) noexcept;

// Cuts seen while playing (docs/chapters.md, "Live cuts"): each converted picture's mean luma (0 to 1), in decode order.
// Pictures are put back in presentation order through a window of kDepth before neighbours are compared: a cut is a
// change of more than kCutLuma from the previous picture, or one the tone mapper reported (HDR). Reset at every epoch:
// the first picture after a seek has no neighbour to compare with.
class CutDetector final {
public:
    static constexpr unsigned kDepth = 4;
    static constexpr float kCutLuma = 0.12f;
    void Reset() noexcept { count_ = 0; have_previous_ = false; }
    // True with the cut's PTS in `cut` when the picture leaving the window starts a new shot.
    bool Push(std::int64_t pts, float luma, bool reported, std::int64_t& cut) noexcept;
    unsigned Cuts() const noexcept { return cuts_; }
private:
    struct Entry { std::int64_t pts; float luma; bool reported; };
    Entry window_[kDepth + 1]{};
    unsigned count_ = 0, cuts_ = 0;
    float previous_ = 0;
    bool have_previous_ = false;
};
const char* Name(ChapterSource) noexcept;
const char* Name(SceneOrigin) noexcept;
}
