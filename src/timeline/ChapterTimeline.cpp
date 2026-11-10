#include "media/timeline/ChapterTimeline.hpp"
#include "media/text/Utf8.hpp"
#include <algorithm>
#include <cstring>

namespace media::timeline {
bool ChapterTimeline::AddChapter(std::int64_t start, std::string_view title) noexcept {
    if (chapter_count_ == kMaxChapters || start < 0 || (duration_ > 0 && start >= duration_)) return false;
    unsigned at = chapter_count_;
    while (at && chapters_[at - 1].start >= start) {
        if (chapters_[at - 1].start == start) return false;
        --at;
    }
    // The title, whole characters only, without control characters.
    Chapter chapter{}; chapter.start = start;
    std::size_t n = 0; std::string_view rest = title; std::uint32_t c = 0;
    while (!rest.empty()) {
        const auto before = rest.size();
        if (!text::Utf8(rest, c)) { n = 0; break; } // Not UTF-8 (a legacy code page): the chapter stays, untitled.
        const auto bytes = before - rest.size();
        if (c < 0x20 || c == 0x7f) continue;
        if (n + bytes >= sizeof(chapter.title)) break;
        std::memcpy(chapter.title + n, title.data() + (title.size() - before), bytes); n += bytes;
    }
    chapter.title[n] = 0;
    for (unsigned i = chapter_count_; i > at; --i) chapters_[i] = chapters_[i - 1];
    chapters_[at] = chapter; ++chapter_count_;
    return true;
}
bool ChapterTimeline::AddScene(std::int64_t at, SceneOrigin origin) noexcept {
    if (at < 0 || (duration_ > 0 && at >= duration_)) return false;
    unsigned i = 0;
    while (i < scene_count_ && scenes_[i].at < at) ++i;
    // A neighbour within kSceneMerge is the same scene: a live cut corrects where a guess put it.
    for (const unsigned k : {i, i - 1}) {
        if (k >= scene_count_) continue;
        const auto distance = scenes_[k].at > at ? scenes_[k].at - at : at - scenes_[k].at;
        if (distance >= kSceneMerge) continue;
        // Marks are kept at least kSceneMerge apart, so the corrected one stays in order between its neighbours.
        if (origin == SceneOrigin::Live && scenes_[k].origin != SceneOrigin::Live) { ++live_; scenes_[k] = {at, origin}; }
        return false;
    }
    if (scene_count_ == kMaxScenes) return false;
    for (unsigned k = scene_count_; k > i; --k) scenes_[k] = scenes_[k - 1];
    scenes_[i] = {at, origin}; ++scene_count_;
    if (origin == SceneOrigin::Live) ++live_;
    return true;
}
int ChapterTimeline::ChapterIndex(std::int64_t position) const noexcept {
    int index = -1;
    for (unsigned i = 0; i < chapter_count_ && chapters_[i].start <= position; ++i) index = static_cast<int>(i);
    return index;
}
namespace {
template <class T, class Start>
bool Next(const T* marks, unsigned count, std::int64_t position, unsigned& index, Start start) noexcept {
    for (unsigned i = 0; i < count; ++i)
        if (start(marks[i]) > position + ChapterTimeline::kLanding) { index = i; return true; }
    return false;
}
template <class T, class Start>
bool Previous(const T* marks, unsigned count, std::int64_t position, unsigned& index, Start start) noexcept {
    // The current mark: the last one at or before the position, a landing's worth of slack included.
    int current = -1;
    for (unsigned i = 0; i < count && start(marks[i]) <= position + ChapterTimeline::kLanding; ++i) current = static_cast<int>(i);
    if (current < 0) return false;
    const auto into = position - start(marks[current]);
    if (into > ChapterTimeline::kRestart) { index = static_cast<unsigned>(current); return true; }
    if (current > 0) { index = static_cast<unsigned>(current - 1); return true; }
    if (into > ChapterTimeline::kLanding) { index = 0; return true; } // The first one, from inside it.
    return false;
}
}
bool ChapterTimeline::NextChapter(std::int64_t position, unsigned& index) const noexcept {
    return Next(chapters_, chapter_count_, position, index, [](const Chapter& c) { return c.start; });
}
bool ChapterTimeline::PreviousChapter(std::int64_t position, unsigned& index) const noexcept {
    return Previous(chapters_, chapter_count_, position, index, [](const Chapter& c) { return c.start; });
}
bool ChapterTimeline::NextScene(std::int64_t position, unsigned& index) const noexcept {
    return Next(scenes_, scene_count_, position, index, [](const Scene& s) { return s.at; });
}
bool ChapterTimeline::PreviousScene(std::int64_t position, unsigned& index) const noexcept {
    return Previous(scenes_, scene_count_, position, index, [](const Scene& s) { return s.at; });
}

SynthesisResult Synthesize(const SynthesisInput& in, ChapterTimeline& timeline) noexcept {
    SynthesisResult result;
    const auto* keys = in.keyframes; const unsigned key_count = keys ? in.keyframe_count : 0;
    timeline.SetDuration(in.duration);
    // 1. The cadence: the median interval between sync samples, from a 10 ms histogram up to 20 s.
    constexpr unsigned kBins = 2000; constexpr std::int64_t kBin = 100000;
    std::uint32_t histogram[kBins]{}; unsigned intervals = 0;
    for (unsigned k = 1; k < key_count; ++k) {
        const auto interval = keys[k].pts - keys[k - 1].pts;
        if (interval <= 0) continue;
        ++histogram[(std::min)(static_cast<std::uint64_t>(interval / kBin), std::uint64_t{kBins - 1})]; ++intervals;
    }
    if (intervals >= 3) {
        unsigned seen = 0, bin = 0;
        while (bin < kBins && (seen += histogram[bin]) * 2 < intervals) ++bin;
        result.cadence = static_cast<std::int64_t>(bin) * kBin + kBin / 2;
    }
    // 2. Structural scenes: a sync sample out of cadence, or a jump in sync-sample size. Strength is remembered for the
    // sections below (scene marks merge, candidates do not).
    struct Candidate { std::int64_t at; unsigned strength; };
    static constexpr unsigned kCandidates = ChapterTimeline::kMaxScenes;
    Candidate candidates[kCandidates]; unsigned candidate_count = 0;
    const auto candidate = [&](std::int64_t at, unsigned strength) {
        for (unsigned i = 0; i < candidate_count; ++i) {
            const auto d = candidates[i].at > at ? candidates[i].at - at : at - candidates[i].at;
            if (d < ChapterTimeline::kSceneMerge) { candidates[i].strength |= strength; return; } // Cut (1) and dialogue (2): 3.
        }
        if (candidate_count < kCandidates) candidates[candidate_count++] = {at, strength};
    };
    for (unsigned k = 1; k < key_count; ++k) {
        const auto interval = keys[k].pts - keys[k - 1].pts;
        const bool early = result.cadence > 0 && interval > 0 && interval * 4 < result.cadence * 3;
        const std::uint64_t now = keys[k].bytes, before = keys[k - 1].bytes;
        const bool jump = now && before && (now * 10 >= before * 18 || now * 100 < before * 55);
        if (!early && !jump) continue;
        if (keys[k].pts <= 0) continue;
        ++result.structural; (void)timeline.AddScene(keys[k].pts, SceneOrigin::Structure); candidate(keys[k].pts, 1);
    }
    // 3. Dialogue resuming after silence: at the sync sample at or before the resuming cue when it lies in the gap.
    std::int64_t spoken = 0; // End of the dialogue so far.
    for (unsigned i = 0; in.cues && i < in.cue_count; ++i) {
        const auto& cue = in.cues[i];
        if (cue.start - spoken > kDialogueGap) {
            std::int64_t at = cue.start;
            unsigned low = 0, high = key_count; // First sync sample after the cue's start.
            while (low < high) { const auto middle = low + (high - low) / 2; if (keys[middle].pts <= cue.start) low = middle + 1; else high = middle; }
            if (low && keys[low - 1].pts > spoken) at = keys[low - 1].pts;
            if (at > 0) { ++result.dialogue; (void)timeline.AddScene(at, SceneOrigin::Dialogue); candidate(at, 2); }
        }
        spoken = (std::max)(spoken, cue.end);
    }
    // 4. Sections, when the file names no chapters: the start, then the strongest candidates far enough apart, then sync
    // samples (strength 0) to fill what is left.
    if (timeline.Chapters() || in.duration <= 0) return result;
    const std::int64_t minute = 600000000;
    result.spacing = std::clamp(in.duration / 12, minute, 10 * minute);
    (void)timeline.AddChapter(0, {}); ++result.sections;
    const auto far = [&](std::int64_t at) {
        if (at <= 0 || at > in.duration - result.spacing / 2) return false;
        for (unsigned c = 0; c < timeline.Chapters(); ++c) {
            const auto d = timeline.ChapterAt(c).start > at ? timeline.ChapterAt(c).start - at : at - timeline.ChapterAt(c).start;
            if (d < result.spacing) return false;
        }
        return true;
    };
    for (unsigned strength = 3; strength >= 1; --strength) {
        // Earlier first among equals: candidates are visited in time order.
        for (;;) {
            int best = -1;
            for (unsigned i = 0; i < candidate_count; ++i)
                if (candidates[i].strength == strength && far(candidates[i].at) && (best < 0 || candidates[i].at < candidates[best].at)) best = static_cast<int>(i);
            if (best < 0) break;
            if (timeline.AddChapter(candidates[best].at, {})) ++result.sections;
            candidates[best].strength = 0xff; // Taken.
        }
    }
    for (unsigned k = 0; k < key_count; ++k)
        if (far(keys[k].pts) && timeline.AddChapter(keys[k].pts, {})) ++result.sections;
    timeline.SetSource(ChapterSource::Synthesized);
    return result;
}

bool CutDetector::Push(std::int64_t pts, float luma, bool reported, std::int64_t& cut) noexcept {
    unsigned at = count_;
    while (at && window_[at - 1].pts > pts) { window_[at] = window_[at - 1]; --at; }
    window_[at] = {pts, luma, reported}; ++count_;
    if (count_ <= kDepth) return false;
    const auto front = window_[0];
    for (unsigned i = 1; i < count_; ++i) window_[i - 1] = window_[i];
    --count_;
    const bool jump = have_previous_ && (front.reported || (front.luma > previous_ ? front.luma - previous_ : previous_ - front.luma) > kCutLuma);
    previous_ = front.luma; have_previous_ = true;
    if (!jump) return false;
    cut = front.pts; ++cuts_;
    return true;
}
const char* Name(ChapterSource source) noexcept {
    static constexpr const char* names[]{"none", "Nero chpl", "QuickTime chapter track", "synthesized"};
    return static_cast<unsigned>(source) < std::size(names) ? names[static_cast<unsigned>(source)] : "?";
}
const char* Name(SceneOrigin origin) noexcept {
    static constexpr const char* names[]{"structure", "dialogue", "live"};
    return static_cast<unsigned>(origin) < std::size(names) ? names[static_cast<unsigned>(origin)] : "?";
}
}
