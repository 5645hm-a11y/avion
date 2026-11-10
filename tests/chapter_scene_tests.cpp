// Chapters, scenes and the scrub preview's demux side (docs/chapters.md, docs/seeking.md "Scrub preview"):
// - IsobmffParser: Nero chapters (moov/udta/chpl) and a QuickTime chapter track (trak/tref/chap, a 'text' entry), and
//   nothing malformed or oversized in udta or tref ever refuses the file; SyncSample/DecodeDistance/SyncCount/SyncAt;
// - timeline::ChapterTimeline: ordering, titles, scene merging, next/previous with the landing slack and restart window;
// - timeline::Synthesize: cadence, early and size-jump sync samples, dialogue gaps, sections, determinism;
// - timeline::CutDetector: presentation order from decode order, reported cuts, epochs;
// - ui::TimelineMarks packing, KineticHud's Scrub commands and preview cue;
// - LocalIntentDispatcher: next/previous chapter/scene in nine languages, and their validation.
// No allocation after setup.
#include "media/demux/IsobmffParser.hpp"
#include "media/timeline/ChapterTimeline.hpp"
#include "media/ui/KineticHud.hpp"
#include "media/ai/LocalIntentDispatcher.hpp"
#include "allocation_probe.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)

// ---- A small file writer (setup only) ----
struct Writer {
    std::vector<std::byte> b;
    void B(unsigned v) { b.push_back(static_cast<std::byte>(v & 255)); }
    void U16(unsigned v) { B(v >> 8); B(v); }
    void U32(std::uint32_t v) { U16(v >> 16); U16(v); }
    void U64(std::uint64_t v) { U32(static_cast<std::uint32_t>(v >> 32)); U32(static_cast<std::uint32_t>(v)); }
    void Zero(unsigned n) { while (n--) B(0); }
    void Text(const char* s) { while (*s) B(static_cast<unsigned char>(*s++)); }
    std::size_t Box(const char* tag) { const auto at = b.size(); U32(0); Text(tag); return at; }
    void End(std::size_t at) { const auto v = static_cast<std::uint32_t>(b.size() - at); for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::byte>((v >> (24 - 8 * i)) & 255); }
};
class MemorySource final : public platform::IBlockSource {
public:
    explicit MemorySource(const std::vector<std::byte>& data) noexcept : data_(data) {}
    std::uint64_t Size() const noexcept override { return data_.size(); }
    std::uint32_t MaxRangeBytes() const noexcept override { return 4096; }
    platform::IoStatus Request(std::uint64_t offset, std::uint32_t bytes, platform::BlockToken& token) noexcept override {
        token = {};
        if (!bytes || offset > Size() || bytes > Size() - offset) return platform::IoStatus::OutOfRange;
        if (bytes > MaxRangeBytes()) return platform::IoStatus::TooLarge;
        for (std::uint32_t i = 0; i < 4; ++i) {
            auto& slot = slots_[i];
            if (slot.active) continue;
            slot.active = true; ++slot.generation; token = {i, slot.generation};
            slot.view = {data_.data() + offset, offset, bytes, token};
            return platform::IoStatus::Pending;
        }
        return platform::IoStatus::WouldBlock;
    }
    platform::IoStatus Poll(platform::BlockToken token, platform::BlockView& view) noexcept override {
        if (token.id >= 4 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return platform::IoStatus::InvalidToken;
        view = slots_[token.id].view; return platform::IoStatus::Ready;
    }
    bool Release(platform::BlockToken token) noexcept override {
        if (token.id >= 4 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return false;
        slots_[token.id].active = false; return true;
    }
private:
    struct Slot { platform::BlockView view{}; std::uint32_t generation = 0; bool active = false; };
    const std::vector<std::byte>& data_;
    Slot slots_[4]{};
};
// One video track of `count` samples, 100 ticks each at timescale 1000 (0.1 s), with `sync` (1-based) and optional ctts;
// optionally a QuickTime chapter track (id 2, 'text' entry, three 0.4 s samples) named by the video's tref; then `udta`.
struct FileSpec {
    unsigned count = 12;
    std::vector<std::uint32_t> sync{1, 7};
    std::vector<std::uint32_t> composition{};
    bool chapter_track = false;
    std::vector<std::byte> udta{}; // Payload of moov/udta (empty: no udta).
    std::vector<std::byte> tref{}; // Payload of the video's tref (empty: the chapter track's chap, when there is one).
};
std::vector<std::byte> Build(const FileSpec& f) {
    Writer w; auto box = w.Box("ftyp"); w.Text("isom"); w.U32(0); w.Text("isom"); w.End(box);
    std::vector<std::uint32_t> video_at, text_at;
    const char* names[]{"Intro", "Middle", "End"};
    box = w.Box("mdat");
    for (unsigned i = 0; i < f.count; ++i) { video_at.push_back(static_cast<std::uint32_t>(w.b.size())); for (unsigned k = 0; k < 6; ++k) w.B(i); }
    if (f.chapter_track) for (const char* n : names) { text_at.push_back(static_cast<std::uint32_t>(w.b.size())); w.U16(static_cast<unsigned>(std::strlen(n))); w.Text(n); }
    w.End(box);
    const auto moov = w.Box("moov");
    box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(f.count * 100); w.U32(0x10000); w.U16(0x100); w.Zero(10); w.Zero(36); w.Zero(24); w.U32(3); w.End(box);
    const auto track = [&](unsigned id, bool video) {
        const unsigned count = video ? f.count : 3, delta = video ? 100 : 400;
        const auto trak = w.Box("trak");
        box = w.Box("tkhd"); w.U32(video ? 3u : 2u); w.Zero(8); w.U32(id); w.U32(0); w.U32(count * delta); w.Zero(8); w.U16(0); w.U16(0); w.U16(0); w.U16(0); w.Zero(36); w.U32(0); w.U32(0); w.End(box);
        if (video && (f.chapter_track || !f.tref.empty())) {
            const auto refs = w.Box("tref");
            if (!f.tref.empty()) w.b.insert(w.b.end(), f.tref.begin(), f.tref.end());
            else { const auto chap = w.Box("chap"); w.U32(2); w.End(chap); }
            w.End(refs);
        }
        const auto mdia = w.Box("mdia");
        box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(count * delta); w.U16(0x55c4); w.U16(0); w.End(box);
        box = w.Box("hdlr"); w.U32(0); w.U32(0); w.Text(video ? "vide" : "text"); w.Zero(12); w.B(0); w.End(box);
        const auto minf = w.Box("minf");
        const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
        const auto stbl = w.Box("stbl");
        box = w.Box("stsd"); w.U32(0); w.U32(1);
        const auto entry = w.Box(video ? "avc1" : "text"); w.Zero(6); w.U16(1);
        if (video) {
            w.Zero(16); w.U16(64); w.U16(64); w.Zero(50);
            const auto c = w.Box("avcC"); w.B(1); w.B(66); w.B(0); w.B(30); w.B(255); w.B(225); w.U16(2); w.B(0x67); w.B(0x42); w.B(1); w.U16(2); w.B(0x68); w.B(0); w.End(c);
        } else { w.U32(0); w.U32(0); w.Zero(6); w.Zero(8); w.Zero(8); w.U16(0); w.U16(0); w.B(0); w.U16(0); w.Zero(6); w.B(0); }
        w.End(entry); w.End(box);
        box = w.Box("stts"); w.U32(0); w.U32(1); w.U32(count); w.U32(delta); w.End(box);
        if (video && !f.composition.empty()) {
            box = w.Box("ctts"); w.U32(0); w.U32(static_cast<std::uint32_t>(f.composition.size()));
            for (const auto offset : f.composition) { w.U32(1); w.U32(offset); }
            w.End(box);
        }
        if (video && !f.sync.empty()) { box = w.Box("stss"); w.U32(0); w.U32(static_cast<std::uint32_t>(f.sync.size())); for (const auto n : f.sync) w.U32(n); w.End(box); }
        box = w.Box("stsc"); w.U32(0); w.U32(1); w.U32(1); w.U32(1); w.U32(1); w.End(box);
        box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(count);
        for (unsigned i = 0; i < count; ++i) w.U32(video ? 6u : static_cast<unsigned>(2 + std::strlen(names[i])));
        w.End(box);
        box = w.Box("stco"); w.U32(0); w.U32(count); for (const auto o : video ? video_at : text_at) w.U32(o); w.End(box);
        w.End(stbl); w.End(minf); w.End(mdia); w.End(trak);
    };
    track(1, true);
    if (f.chapter_track) track(2, false);
    if (!f.udta.empty()) { const auto user = w.Box("udta"); w.b.insert(w.b.end(), f.udta.begin(), f.udta.end()); w.End(user); }
    w.End(moov);
    return w.b;
}
// A Nero chpl box: version 1 (with its 4 extra bytes), `count` claimed, `entries` written.
std::vector<std::byte> Chpl(unsigned count, const std::vector<std::pair<std::uint64_t, std::string>>& entries, bool terminator) {
    Writer w; const auto box = w.Box("chpl"); w.U32(0x01000000); w.U32(0); w.B(count);
    for (const auto& [start, title] : entries) { w.U64(start); w.B(static_cast<unsigned>(title.size())); w.Text(title.c_str()); }
    w.End(box);
    if (terminator) w.Zero(4);
    return w.b;
}
demux::ParseStatus Scan(demux::IsobmffParser& parser) {
    core::SpscRingBuffer<core::MediaPacket, 2> queue;
    auto status = demux::ParseStatus::Progress;
    for (unsigned step = 0; step < 20000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed; ++step) {
        status = parser.Step(queue);
        while (const auto* p = queue.PeekReadSlot()) { CHECK(parser.ReturnPacket(*p)); CHECK(queue.ReleaseReadSlot()); }
    }
    return status;
}

void NeroChapters() {
    const std::string hebrew = "\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\xa9\xd7\xa0\xd7\x99", long_title(70, 'x');
    FileSpec spec; spec.udta = Chpl(3, {{0, "Opening"}, {4000000, hebrew}, {8000000, long_title}}, true);
    const auto file = Build(spec);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {4, 1 << 20, 1}));
    CHECK(arena.Seal());
    allocation_probe::Begin();
    const auto status = Scan(parser);
    CHECK(allocation_probe::End() == 0);
    CHECK(status == demux::ParseStatus::Done && parser.TrackCount() == 1);
    CHECK(parser.ChapterCount() == 3);
    CHECK(parser.ChapterAt(0).start_100ns == 0 && std::strcmp(parser.ChapterAt(0).title, "Opening") == 0);
    CHECK(parser.ChapterAt(1).start_100ns == 4000000 && hebrew == parser.ChapterAt(1).title);
    CHECK(parser.ChapterAt(2).start_100ns == 8000000 && std::strlen(parser.ChapterAt(2).title) == sizeof(demux::ChapterEntry::title) - 1); // Cut to fit.
    CHECK(parser.Track(0)->chapter_track == 0);
}
void ToleratedUdta() {
    // A count larger than the entries written, a broken child, a child box of size 3, and an oversized udta: never a refusal.
    struct Case { std::vector<std::byte> udta; unsigned chapters; };
    std::vector<Case> cases;
    cases.push_back({Chpl(9, {{0, "A"}, {1000000, "B"}}, false), 2});
    { Writer w; w.U32(3); w.Text("junk"); cases.push_back({w.b, 0}); }
    { Writer w; const auto meta = w.Box("meta"); w.U32(0); w.Zero(12); w.End(meta); auto chpl = Chpl(1, {{5000000, "After meta"}}, false); w.b.insert(w.b.end(), chpl.begin(), chpl.end()); cases.push_back({w.b, 1}); }
    { Writer w; w.U32(0x7fffffff); w.Text("chpl"); w.Zero(8); cases.push_back({w.b, 0}); } // A size past the end.
    { Writer w; const auto chpl = w.Box("chpl"); w.U32(0); w.B(1); w.U64(1); w.B(200); w.Text("short"); w.End(chpl); cases.push_back({w.b, 0}); } // Title past the box.
    { auto big = Chpl(1, {{0, "Big"}}, false); big.resize(5000); cases.push_back({big, 0}); } // Larger than the source's range: not read.
    for (const auto& c : cases) {
        FileSpec spec; spec.udta = c.udta;
        const auto file = Build(spec);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
        MemorySource source(file);
        demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {4, 1 << 20, 1}));
        CHECK(Scan(parser) == demux::ParseStatus::Done);
        CHECK(parser.Track(0) && parser.Track(0)->refusal == demux::ParseError::None);
        CHECK(parser.ChapterCount() == c.chapters);
    }
}
void QuickTimeChapterTrack() {
    FileSpec spec; spec.chapter_track = true;
    const auto file = Build(spec);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {4, 1 << 20, 1}));
    CHECK(Scan(parser) == demux::ParseStatus::Done && parser.TrackCount() == 2);
    const auto* video = parser.Track(0); const auto* text = parser.Track(1);
    CHECK(video && video->chapter_track == 2);
    CHECK(text && text->kind == demux::TrackKind::Subtitle && text->codec == demux::Codec::Tx3g && text->refusal == demux::ParseError::None &&
          text->sample_entry == 0x74657874u); // 'text'
    demux::IsobmffParser::SampleRecord s{};
    CHECK(parser.SampleAt(1, 1, s) && s.pts_100ns == 4000000 && s.bytes == 8);
    CHECK(parser.ChapterCount() == 0);
    // A tref with garbage around a chap box, and one whose chap is empty: tolerated.
    FileSpec odd; odd.tref = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3}};
    const auto odd_file = Build(odd);
    MemorySource odd_source(odd_file);
    core::LinearMemoryArena odd_arena; CHECK(odd_arena.Initialize(4 * 1024 * 1024));
    demux::IsobmffParser odd_parser; CHECK(odd_parser.Initialize(odd_arena, odd_source, {4, 1 << 20, 1}));
    CHECK(Scan(odd_parser) == demux::ParseStatus::Done && odd_parser.Track(0)->chapter_track == 0 && odd_parser.Track(0)->refusal == demux::ParseError::None);
}
void DecodeDistances() {
    // 12 pictures of 0.1 s in groups of four, decoded I P B B and presented I B B P (ctts 1, 3, 0, 0 units of 0.1 s), with
    // a sync sample at each I (samples 1, 5, 9).
    FileSpec spec; spec.count = 12; spec.sync = {1, 5, 9};
    spec.composition = {100, 300, 0, 0, 100, 300, 0, 0, 100, 300, 0, 0};
    const auto file = Build(spec);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {4, 1 << 20, 1}));
    CHECK(Scan(parser) == demux::ParseStatus::Done && parser.Seekable());
    // PTS of each decode index (100 ns), from SampleAt, and the expected distance from sync sample 0 or 6.
    std::int64_t pts[12]{}; demux::IsobmffParser::SampleRecord s{};
    for (unsigned i = 0; i < 12; ++i) { CHECK(parser.SampleAt(0, i, s)); pts[i] = s.pts_100ns; }
    CHECK(parser.SyncCount(0) == 3 && parser.SyncAt(0, 0) == 0 && parser.SyncAt(0, 1) == 4 && parser.SyncAt(0, 2) == 8 && parser.SyncAt(0, 3) == UINT32_MAX);
    allocation_probe::Begin();
    for (unsigned i = 0; i < 12; ++i) {
        const auto target = pts[i] + 1; // Inside picture i's interval.
        const auto sync = parser.SyncSample(0, target);
        const unsigned want_sync = i / 4 * 4; // The group's I picture.
        CHECK(sync == want_sync);
        const auto distance = parser.DecodeDistance(0, sync, target, 20);
        CHECK(distance == i - sync); // Every sample before i in decode order, from the sync sample.
        CHECK(parser.DecodeDistance(0, sync, target, 1) == (i - sync <= 1 ? i - sync : 2u)); // Bounded: limit + 1.
    }
    CHECK(parser.DecodeDistance(0, 0, -5, 4) == 5); // Before the first picture: not found.
    CHECK(parser.DecodeDistance(0, 40, pts[3], 4) == 5); // From past the end.
    CHECK(allocation_probe::End() == 0);
}

void TimelineBasics() {
    static timeline::ChapterTimeline t; t.Clear(); t.SetDuration(600000000); // 60 s.
    allocation_probe::Begin();
    CHECK(t.AddChapter(300000000, "Third"));
    CHECK(t.AddChapter(0, "First"));
    CHECK(t.AddChapter(100000000, "Second"));
    CHECK(!t.AddChapter(100000000, "Duplicate") && !t.AddChapter(-1, "") && !t.AddChapter(600000000, "At the end"));
    CHECK(t.Chapters() == 3 && t.ChapterAt(0).start == 0 && t.ChapterAt(1).start == 100000000 && t.ChapterAt(2).start == 300000000);
    CHECK(std::strcmp(t.ChapterAt(1).title, "Second") == 0);
    CHECK(t.AddChapter(400000000, "\xff\xfe broken"));                 // Kept, untitled.
    CHECK(t.ChapterAt(3).title[0] == 0);
    CHECK(t.AddChapter(450000000, "tab\there"));                       // Control characters dropped.
    CHECK(std::strcmp(t.ChapterAt(4).title, "tabhere") == 0);
    CHECK(t.ChapterIndex(-1) == -1 && t.ChapterIndex(0) == 0 && t.ChapterIndex(99999999) == 0 && t.ChapterIndex(100000000) == 1);
    unsigned i = 0;
    // Next: strictly beyond the landing slack.
    CHECK(t.NextChapter(0, i) && i == 1);
    CHECK(t.NextChapter(100000000 - 2000000, i) && i == 2); // 0.2 s before chapter 2: within the slack, so the one after.
    CHECK(t.NextChapter(100000000 - 3000000, i) && i == 1);
    CHECK(!t.NextChapter(460000000, i));
    // Previous: restart the current one when more than kRestart into it, else the one before.
    CHECK(t.PreviousChapter(100000000 + 10000000, i) && i == 0);  // 1 s into chapter 2.
    CHECK(t.PreviousChapter(100000000 + 40000000, i) && i == 1);  // 4 s into it: its own start.
    CHECK(t.PreviousChapter(100000000 - 1000000, i) && i == 0);   // Landed just before chapter 2's mark: chapter 2 is current.
    CHECK(t.PreviousChapter(20000000, i) && i == 0);              // Inside the first: its start.
    CHECK(!t.PreviousChapter(1000000, i));                         // At the first one's start already.
    // Scenes: merging, and a live cut correcting a structural guess.
    CHECK(t.AddScene(200000000, timeline::SceneOrigin::Structure));
    CHECK(!t.AddScene(205000000, timeline::SceneOrigin::Dialogue));          // 0.5 s away: the same scene.
    CHECK(t.Scenes() == 1 && t.SceneAt(0).origin == timeline::SceneOrigin::Structure);
    CHECK(!t.AddScene(202000000, timeline::SceneOrigin::Live));              // Corrects it.
    CHECK(t.Scenes() == 1 && t.SceneAt(0).at == 202000000 && t.SceneAt(0).origin == timeline::SceneOrigin::Live && t.LiveScenes() == 1);
    CHECK(t.AddScene(250000000, timeline::SceneOrigin::Live) && t.AddScene(150000000, timeline::SceneOrigin::Dialogue));
    CHECK(t.Scenes() == 3 && t.SceneAt(0).at == 150000000 && t.SceneAt(2).at == 250000000 && t.LiveScenes() == 2);
    CHECK(!t.AddScene(-5, timeline::SceneOrigin::Live) && !t.AddScene(700000000, timeline::SceneOrigin::Live));
    CHECK(t.NextScene(160000000, i) && i == 1 && t.PreviousScene(260000000, i) && i == 1);
    CHECK(allocation_probe::End() == 0);
}
std::uint32_t Jitter(unsigned k) { return 50000 + (k * 7919u) % 9000u - 4500u; } // +-9 %: never a size jump.
void Synthesis() {
    // Two hours: a sync sample every 2 s, plus encoder cuts (early sync samples) at 600.4, 1800.25 and 3000.4 s, and a
    // content change (sync samples 2.2 times as large from 4200 s on). Dialogue every 3 s from 1 s, silent for 10 s from
    // 1200 s and for 30 s from 5000 s.
    constexpr std::int64_t kS = 10000000;
    std::vector<timeline::Keyframe> keys; std::vector<timeline::Interval> cues;
    for (unsigned k = 0; k * 2 < 7200; ++k) {
        const std::int64_t at = std::int64_t{k} * 2 * kS;
        for (const std::int64_t cut : {6004 * kS / 10, 180025 * kS / 100, 30004 * kS / 10})
            if (cut > at - 2 * kS && cut < at) keys.push_back({cut, Jitter(k + 100000)});
        keys.push_back({at, at >= 4200 * kS ? Jitter(k) * 22 / 10 : Jitter(k)});
    }
    for (std::int64_t at = kS; at < 7190 * kS; at += 3 * kS) {
        if ((at >= 1200 * kS && at < 1210 * kS) || (at >= 5000 * kS && at < 5030 * kS)) continue;
        cues.push_back({at, at + 2 * kS});
    }
    static timeline::ChapterTimeline t; t.Clear();
    const timeline::SynthesisInput input{keys.data(), static_cast<unsigned>(keys.size()), cues.data(), static_cast<unsigned>(cues.size()), 7200 * kS};
    allocation_probe::Begin();
    const auto r = timeline::Synthesize(input, t);
    CHECK(allocation_probe::End() == 0);
    CHECK(r.cadence > 19 * kS / 10 && r.cadence < 21 * kS / 10);
    CHECK(r.structural == 4 && r.dialogue == 2 && t.Source() == timeline::ChapterSource::Synthesized);
    const auto has_scene = [&](std::int64_t at, std::int64_t slack) {
        for (unsigned i = 0; i < t.Scenes(); ++i) if (std::llabs(t.SceneAt(i).at - at) <= slack) return true;
        return false;
    };
    CHECK(has_scene(6004 * kS / 10, 0) && has_scene(180025 * kS / 100, 0) && has_scene(30004 * kS / 10, 0) && has_scene(4200 * kS, 0));
    // Dialogue resumes at 1210 and 5032 s (cues start every 3 s from 1 s): the sync samples at those instants, inside the gap.
    CHECK(has_scene(1210 * kS, 0) && has_scene(5032 * kS, 0));
    // Sections: the start, every pair at least `spacing` apart, none within spacing / 2 of the end; the dialogue resumes
    // (strength 2) and the cuts far enough from them (600.4 s, 3000.4 s, 4200 s) are among them; then sync samples fill
    // what is left (1810, 5632, 6232, 6832 s): ten in all.
    CHECK(r.spacing == 600 * kS && t.ChapterAt(0).start == 0 && t.Chapters() == r.sections && r.sections == 10);
    bool spaced = true;
    for (unsigned c = 1; c < t.Chapters(); ++c) spaced = spaced && t.ChapterAt(c).start - t.ChapterAt(c - 1).start >= r.spacing;
    CHECK(spaced && t.ChapterAt(t.Chapters() - 1).start <= 7200 * kS - r.spacing / 2);
    const auto has_chapter = [&](std::int64_t at) { for (unsigned c = 0; c < t.Chapters(); ++c) if (t.ChapterAt(c).start == at) return true; return false; };
    CHECK(has_chapter(1210 * kS) && has_chapter(5032 * kS) && has_chapter(6004 * kS / 10) && has_chapter(30004 * kS / 10) && has_chapter(4200 * kS));
    CHECK(has_chapter(1810 * kS) && has_chapter(5632 * kS) && has_chapter(6232 * kS) && has_chapter(6832 * kS));
    CHECK(!has_chapter(180025 * kS / 100)); // 590 s from the dialogue section at 1210 s.
    // Deterministic.
    static timeline::ChapterTimeline again; again.Clear();
    const auto r2 = timeline::Synthesize(input, again);
    bool same = r2.sections == r.sections && again.Scenes() == t.Scenes();
    for (unsigned c = 0; same && c < t.Chapters(); ++c) same = again.ChapterAt(c).start == t.ChapterAt(c).start;
    for (unsigned c = 0; same && c < t.Scenes(); ++c) same = again.SceneAt(c).at == t.SceneAt(c).at && again.SceneAt(c).origin == t.SceneAt(c).origin;
    CHECK(same);
    // A file with chapters keeps them: scenes only.
    static timeline::ChapterTimeline named; named.Clear(); named.SetDuration(7200 * kS); CHECK(named.AddChapter(0, "One") && named.AddChapter(3600 * kS, "Two"));
    named.SetSource(timeline::ChapterSource::Nero);
    const auto r3 = timeline::Synthesize(input, named);
    CHECK(r3.sections == 0 && named.Chapters() == 2 && named.Source() == timeline::ChapterSource::Nero && named.Scenes() == t.Scenes());
    // Three seconds with a sync sample every two pictures and no dialogue: one section, no scene.
    std::vector<timeline::Keyframe> short_keys; for (unsigned k = 0; k < 36; ++k) short_keys.push_back({std::int64_t{k} * 833333, 3000});
    static timeline::ChapterTimeline brief; brief.Clear();
    const auto r4 = timeline::Synthesize({short_keys.data(), 36, nullptr, 0, 3 * kS}, brief);
    CHECK(r4.sections == 1 && brief.Chapters() == 1 && brief.Scenes() == 0 && r4.structural == 0);
    // Nothing at all.
    brief.Clear(); const auto r5 = timeline::Synthesize({nullptr, 0, nullptr, 0, 0}, brief);
    CHECK(r5.sections == 0 && brief.Chapters() == 0 && r5.cadence == 0);
}
void Cuts() {
    // Decode order I P B B (presentation 0 3 1 2, in steps of 1), a cut from luma 0.2 to 0.8 at presentation index 10.
    timeline::CutDetector d; std::int64_t cut = -1; unsigned found = 0; std::int64_t at = -1;
    allocation_probe::Begin();
    for (unsigned g = 0; g < 6; ++g) {
        const unsigned base = g * 4, order[4]{base, base + 3, base + 1, base + 2};
        for (const unsigned p : order) {
            const float luma = p < 10 ? 0.2f : 0.8f;
            if (d.Push(p * 100, luma, false, cut)) { ++found; at = cut; }
        }
    }
    CHECK(found == 1 && at == 1000 && d.Cuts() == 1);
    // A cut the tone mapper reported, with no luma change.
    d.Reset(); found = 0;
    for (unsigned p = 0; p < 12; ++p) if (d.Push(p * 100, 0.5f, p == 7, cut)) { ++found; at = cut; }
    CHECK(found == 1 && at == 700);
    // An epoch: the first picture after Reset has no predecessor.
    d.Reset(); found = 0;
    for (unsigned p = 0; p < 8; ++p) if (d.Push(p * 100, p == 0 ? 0.9f : 0.1f, false, cut)) { ++found; at = cut; }
    CHECK(found == 1 && at == 100); // 0.9 -> 0.1 between pictures 0 and 1 of the same epoch.
    d.Reset(); found = 0;
    for (unsigned p = 0; p < 8; ++p) if (d.Push(p * 100, 0.1f, false, cut)) ++found;
    CHECK(found == 0);
    CHECK(allocation_probe::End() == 0);
}
void Marks() {
    ui::TimelineMarks m;
    CHECK(m.Add(0.25, true) && m.Add(0.5, false) && m.Add(1.0, true) && m.Add(0.0, false));
    CHECK(!m.Add(-0.1, true) && !m.Add(1.5, true) && !m.Add(std::nan(""), true));
    CHECK(m.count == 4 && m.Chapter(0) && !m.Chapter(1) && m.Chapter(2) && !m.Chapter(3));
    CHECK(std::fabs(m.Fraction(0) - 0.25) < 1e-4 && std::fabs(m.Fraction(1) - 0.5) < 1e-4 && m.Fraction(2) == 1.0 && m.Fraction(3) == 0.0);
    CHECK((m.packed[0] & 0xffffu) != 0 && (m.packed[1] >> 16) != 0 && m.packed[2] == 0); // 0 ends the list in the shader.
    for (unsigned i = 4; i < ui::TimelineMarks::kMax; ++i) CHECK(m.Add(0.9, false));
    CHECK(!m.Add(0.9, false) && m.count == ui::TimelineMarks::kMax);
}
void HudScrub() {
    ui::KineticHud hud; hud.Configure(1280, 720, 600000000, true);
    const auto& u = hud.Uniforms();
    const float y = u.center_y + u.timeline_y * u.scale, half = (u.dock_half_width - 28) * u.scale;
    allocation_probe::Begin();
    auto a = hud.Input({ui::PointerEvent::Kind::Down, u.center_x - half / 2, y, 1000}, false);
    CHECK(a.command == ui::HudCommand::None && hud.Dragging()); // A press alone seeks nothing.
    a = hud.Input({ui::PointerEvent::Kind::Move, u.center_x, y, 2000}, false);
    CHECK(a.command == ui::HudCommand::Scrub && a.seek_target == 300000000);
    a = hud.Input({ui::PointerEvent::Kind::Move, u.center_x, y + 3, 3000}, false); // Same position along the track.
    CHECK(a.command == ui::HudCommand::None);
    a = hud.Input({ui::PointerEvent::Kind::Move, u.center_x + half, y, 4000}, false);
    CHECK(a.command == ui::HudCommand::Scrub && a.seek_target == 600000000);
    a = hud.Input({ui::PointerEvent::Kind::Up, u.center_x + half, y, 5000}, false);
    CHECK(a.command == ui::HudCommand::Seek && a.seek_target == 600000000 && !hud.Dragging());
    // The preview cue glides in while set and out once cleared; reduced motion snaps.
    hud.SetPreview(true); float last = 0; bool rising = true;
    for (unsigned k = 1; k <= 100; ++k) { (void)hud.Advance(100000 * k, false, 0, 0); rising = rising && u.preview >= last; last = u.preview; }
    CHECK(rising && u.preview == 1.0f && hud.Preview()); // Settled exactly (the glide snaps within 1e-3).
    hud.SetPreview(false); for (unsigned k = 101; k <= 200; ++k) (void)hud.Advance(100000 * k, false, 0, 0);
    CHECK(u.preview == 0.0f);
    hud.SetReducedMotion(true); hud.SetPreview(true); (void)hud.Advance(20100000, false, 0, 0);
    CHECK(u.preview == 1.0f);
    CHECK(allocation_probe::End() == 0);
}
void Grammar() {
    ai::MediaBounds bounds; bounds.duration_100ns = 36000000000; bounds.position_100ns = 6000000000; bounds.seekable = true;
    bounds.chapters = 3; bounds.scenes = 4; bounds.subtitles = true;
    using K = ai::IntentKind;
    struct Case { const char* text; K kind; };
    const Case cases[]{
        {"next chapter", K::NextChapter}, {"Previous chapter, please", K::PreviousChapter}, {"next scene", K::NextScene}, {"go to the previous scene", K::PreviousScene},
        {"skip to the next chapter", K::NextChapter}, {"next section", K::NextChapter},
        {"\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90", K::NextChapter},                                      // פרק הבא
        {"\xd7\x94\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\xa7\xd7\x95\xd7\x93\xd7\x9d", K::PreviousChapter},           // הפרק הקודם
        {"\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\x94\xd7\x91\xd7\x90\xd7\x94", K::NextScene},                        // סצנה הבאה
        {"\xd7\xa1\xd7\xa6\xd7\xa0\xd7\x94 \xd7\xa7\xd7\x95\xd7\x93\xd7\x9e\xd7\xaa", K::PreviousScene},            // סצנה קודמת
        {"\xd7\xaa\xd7\xa2\xd7\x91\xd7\x95\xd7\xa8 \xd7\x9c\xd7\xa4\xd7\xa8\xd7\xa7 \xd7\x94\xd7\x91\xd7\x90", K::NextChapter}, // תעבור לפרק הבא
        {"\xd8\xa7\xd9\x84\xd9\x81\xd8\xb5\xd9\x84 \xd8\xa7\xd9\x84\xd8\xaa\xd8\xa7\xd9\x84\xd9\x8a", K::NextChapter},     // الفصل التالي
        {"\xd8\xa7\xd9\x84\xd9\x85\xd8\xb4\xd9\x87\xd8\xaf \xd8\xa7\xd9\x84\xd8\xb3\xd8\xa7\xd8\xa8\xd9\x82", K::PreviousScene}, // المشهد السابق
        {"siguiente cap\xc3\xadtulo", K::NextChapter}, {"escena anterior", K::PreviousScene},
        {"chapitre pr\xc3\xa9" "c\xc3\xa9" "dent", K::PreviousChapter}, {"sc\xc3\xa8ne suivante", K::NextScene},
        {"n\xc3\xa4" "chstes Kapitel", K::NextChapter}, {"vorherige Szene", K::PreviousScene},
        {"\xd1\x81\xd0\xbb\xd0\xb5\xd0\xb4\xd1\x83\xd1\x8e\xd1\x89\xd0\xb0\xd1\x8f \xd0\xb3\xd0\xbb\xd0\xb0\xd0\xb2\xd0\xb0", K::NextChapter}, // следующая глава
        {"\xd0\xbf\xd1\x80\xd0\xb5\xd0\xb4\xd1\x8b\xd0\xb4\xd1\x83\xd1\x89\xd0\xb0\xd1\x8f \xd1\x81\xd1\x86\xd0\xb5\xd0\xbd\xd0\xb0", K::PreviousScene}, // предыдущая сцена
        {"\xe4\xb8\x8b\xe4\xb8\x80\xe7\xab\xa0", K::NextChapter},                 // 下一章
        {"\xe4\xb8\x8a\xe4\xb8\x80\xe4\xb8\xaa\xe5\x9c\xba\xe6\x99\xaf", K::PreviousScene}, // 上一个场景
        {"\xe6\xac\xa1\xe3\x81\xae\xe7\xab\xa0", K::NextChapter},                 // 次の章
        {"\xe5\x89\x8d\xe3\x81\xae\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3", K::PreviousScene}, // 前のシーン
    };
    allocation_probe::Begin();
    for (const auto& c : cases) {
        const auto intent = ai::LocalIntentDispatcher::Parse(c.text, bounds);
        if (intent.kind != c.kind || intent.status != ai::IntentStatus::Ok)
            std::printf("FAIL grammar: \"%s\" -> %s %s\n", c.text, ai::Name(intent.kind), ai::Name(intent.status));
        CHECK(intent.kind == c.kind && intent.status == ai::IntentStatus::Ok);
    }
    // The command bar reads it as a command, not as a quote to look for.
    CHECK(ai::LocalIntentDispatcher::Interpret("next chapter", bounds).kind == K::NextChapter);
    // Validation: nothing to go to, not seekable, a surplus word.
    auto none = bounds; none.chapters = 0;
    CHECK(ai::LocalIntentDispatcher::Parse("next chapter", none).status == ai::IntentStatus::Unavailable);
    CHECK(ai::LocalIntentDispatcher::Parse("next scene", none).status == ai::IntentStatus::Ok);
    none.scenes = 0; CHECK(ai::LocalIntentDispatcher::Parse("previous scene", none).status == ai::IntentStatus::Unavailable);
    auto fixed = bounds; fixed.seekable = false;
    CHECK(ai::LocalIntentDispatcher::Parse("next chapter", fixed).status == ai::IntentStatus::Unavailable);
    CHECK(ai::LocalIntentDispatcher::Parse("next chapter five", bounds).status == ai::IntentStatus::Malformed);
    // Other commands are untouched: "next" alone is no track or chapter command; "skip" is still a relative seek.
    CHECK(ai::LocalIntentDispatcher::Parse("skip", bounds).kind == K::SeekRelative);
    CHECK(ai::LocalIntentDispatcher::Parse("next subtitles", bounds).kind == K::SelectSubtitles);
    CHECK(allocation_probe::End() == 0);
    CHECK(ai::LocalIntentDispatcher::TablesConsistent([](const char* rule, std::string_view word) noexcept {
        std::printf("FAIL table rule \"%s\": %.*s\n", rule, static_cast<int>(word.size()), word.data());
    }));
    CHECK(std::strcmp(ai::Name(K::NextChapter), "NextChapter") == 0 && std::strcmp(ai::Name(K::PreviousScene), "PreviousScene") == 0);
}
}
int main() {
    NeroChapters(); ToleratedUdta(); QuickTimeChapterTrack(); DecodeDistances();
    TimelineBasics(); Synthesis(); Cuts(); Marks(); HudScrub(); Grammar();
    std::printf(failures ? "chapter/scene tests: %d FAILED\n" : "chapter/scene tests: all passed\n", failures);
    return failures ? 1 : 0;
}
