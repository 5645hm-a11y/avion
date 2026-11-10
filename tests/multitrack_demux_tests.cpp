// media::demux::IsobmffParser with several tracks (docs/tracks.md): every trak is catalogued (kind, language, name,
// alternate group, sample entry); tracks the engine cannot use are refused one by one while the file plays on; which
// tracks are published is chosen per track; a switch between two audio tracks lands on the right access unit while
// the video continues untouched; subtitle samples are read by table lookup. No allocation after setup.
#include "media/demux/IsobmffParser.hpp"
#include "allocation_probe.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)

// ---- A multi-track file writer (setup only) ----
struct Writer {
    std::vector<std::byte> b;
    void B(unsigned v) { b.push_back(static_cast<std::byte>(v & 255)); }
    void U16(unsigned v) { B(v >> 8); B(v); }
    void U32(std::uint32_t v) { U16(v >> 16); U16(v); }
    void Zero(unsigned n) { while (n--) B(0); }
    void Text(const char* s) { while (*s) B(static_cast<unsigned char>(*s++)); }
    std::size_t Box(const char* tag) { const auto at = b.size(); U32(0); Text(tag); return at; }
    void End(std::size_t at) { Patch(at, static_cast<std::uint32_t>(b.size() - at)); }
    void Patch(std::size_t at, std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::byte>((v >> (24 - 8 * i)) & 255); }
};
// What one track is. Every sample is its own chunk.
struct Spec {
    const char* handler; const char* entry; std::uint32_t id; const char* language; const char* name;
    bool enabled = true; unsigned group = 0, timescale = 1000, duration = 100, count = 4;
    bool two_edits = false, broken_table = false, two_entries = false;
    std::vector<std::string> texts{}; // tx3g samples; else payloads of id-derived bytes.
    std::vector<std::uint32_t> sync{};        // stss (1-based sample numbers); empty: every sample is a sync sample.
    std::vector<std::uint32_t> composition{}; // ctts, one offset per sample; empty: none.
};
std::uint16_t Packed(const char* language) {
    if (!language) return 0x55c4; // "und"
    return static_cast<std::uint16_t>(((language[0] - 0x60) << 10) | ((language[1] - 0x60) << 5) | (language[2] - 0x60));
}
void Entry(Writer& w, const Spec& s) {
    const std::string type = s.entry;
    const auto entry = w.Box(s.entry); w.Zero(6); w.U16(1);
    if (type == "avc1") {
        w.Zero(16); w.U16(64); w.U16(64); w.Zero(50);
        const auto c = w.Box("avcC"); w.B(1); w.B(66); w.B(0); w.B(30); w.B(255); w.B(225); w.U16(2); w.B(0x67); w.B(0x42); w.B(1); w.U16(2); w.B(0x68); w.B(0); w.End(c);
    } else if (type == "mp4a" || type == "ac-3") {
        w.Zero(8); w.U16(2); w.U16(16); w.Zero(4); w.U32(48000u << 16);
        if (type == "mp4a") {
            const auto c = w.Box("esds");
            w.U32(0); w.B(3); w.B(25); w.U16(1); w.B(0); w.B(4); w.B(17); w.B(0x40); w.B(0x15); w.Zero(11); w.B(5); w.B(2); w.B(0x11); w.B(0x90); w.B(6); w.B(1); w.B(2);
            w.End(c);
        } else { const auto c = w.Box("dac3"); w.B(0x10); w.B(0x3d); w.B(0xe0); w.End(c); }
    } else if (type == "tx3g") {
        w.U32(0); w.B(1); w.B(0xff); w.U32(0); w.Zero(8); w.U16(0); w.U16(0); w.U16(1); w.B(0); w.B(18); w.U32(0xffffffff);
        const auto f = w.Box("ftab"); w.U16(1); w.U16(1); w.B(4); w.Text("Sans"); w.End(f);
    } else if (type == "wvtt") { const auto c = w.Box("vttC"); w.Text("WEBVTT"); w.End(c); }
    else if (type == "tmcd") { w.Zero(4); w.U32(0); w.U32(25); w.U32(1); w.B(25); w.B(0); }
    w.End(entry);
}
// The file: ftyp, mdat holding every sample, then moov. Returns the offset of each track's first sample.
std::vector<std::byte> Build(const std::vector<Spec>& tracks, std::vector<std::vector<std::uint32_t>>* offsets = nullptr) {
    Writer w; auto box = w.Box("ftyp"); w.Text("isom"); w.U32(0); w.Text("isom"); w.End(box);
    std::vector<std::vector<std::uint32_t>> at(tracks.size()), sizes(tracks.size());
    box = w.Box("mdat");
    // Interleaved by sample index, as a muxer does.
    unsigned most = 0; for (const auto& t : tracks) most = std::max(most, t.count);
    for (unsigned i = 0; i < most; ++i)
        for (std::size_t t = 0; t < tracks.size(); ++t) {
            const auto& s = tracks[t]; if (i >= s.count) continue;
            at[t].push_back(static_cast<std::uint32_t>(w.b.size()));
            if (!s.texts.empty()) { const auto& text = s.texts[i]; w.U16(static_cast<unsigned>(text.size())); w.Text(text.c_str()); sizes[t].push_back(static_cast<std::uint32_t>(2 + text.size())); }
            else { for (unsigned k = 0; k < 6; ++k) w.B(s.id * 16 + i); sizes[t].push_back(6); }
        }
    w.End(box);
    const auto moov = w.Box("moov");
    box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(1000); w.U32(0x10000); w.U16(0x100); w.Zero(10); w.Zero(36); w.Zero(24); w.U32(9); w.End(box);
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const auto& s = tracks[t];
        const auto trak = w.Box("trak");
        box = w.Box("tkhd"); w.U32(s.enabled ? 3u : 2u); w.Zero(8); w.U32(s.id); w.U32(0); w.U32(s.count * s.duration); w.Zero(8);
        w.U16(0); w.U16(s.group); w.U16(0); w.U16(0); w.Zero(36); w.U32(0); w.U32(0); w.End(box);
        if (s.two_edits) {
            const auto edts = w.Box("edts"); box = w.Box("elst"); w.U32(0); w.U32(2);
            // Two normal edits (a cut inside the track): still refused. An empty edit before one normal edit is a delay and
            // is admitted since 2026-11-04 (demux_tests, EditList); this track used to be exactly that.
            w.U32(100); w.U32(0); w.U32(0x10000); w.U32(300); w.U32(0); w.U32(0x10000); w.End(box); w.End(edts);
        }
        const auto mdia = w.Box("mdia");
        box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(s.timescale); w.U32(s.count * s.duration); w.U16(Packed(s.language)); w.U16(0); w.End(box);
        box = w.Box("hdlr"); w.U32(0); w.U32(0); w.Text(s.handler); w.Zero(12); if (s.name) w.Text(s.name); w.B(0); w.End(box);
        const auto minf = w.Box("minf");
        const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
        const auto stbl = w.Box("stbl");
        box = w.Box("stsd"); w.U32(0); w.U32(s.two_entries ? 2 : 1); Entry(w, s); if (s.two_entries) Entry(w, s); w.End(box);
        box = w.Box("stts"); w.U32(0); w.U32(1); w.U32(s.broken_table ? s.count + 5 : s.count); w.U32(s.duration); w.End(box);
        if (!s.composition.empty()) {
            box = w.Box("ctts"); w.U32(0); w.U32(static_cast<std::uint32_t>(s.composition.size()));
            for (const auto offset : s.composition) { w.U32(1); w.U32(offset); }
            w.End(box);
        }
        if (!s.sync.empty()) { box = w.Box("stss"); w.U32(0); w.U32(static_cast<std::uint32_t>(s.sync.size())); for (const auto n : s.sync) w.U32(n); w.End(box); }
        box = w.Box("stsc"); w.U32(0); w.U32(1); w.U32(1); w.U32(1); w.U32(1); w.End(box);
        box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(s.count); for (const auto size : sizes[t]) w.U32(size); w.End(box);
        box = w.Box("stco"); w.U32(0); w.U32(s.count); for (const auto o : at[t]) w.U32(o); w.End(box);
        w.End(stbl); w.End(minf); w.End(mdia); w.End(trak);
    }
    w.End(moov);
    if (offsets) *offsets = at;
    return w.b;
}
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
    unsigned Active() const noexcept { unsigned n = 0; for (const auto& s : slots_) n += s.active ? 1u : 0u; return n; }
private:
    struct Slot { platform::BlockView view{}; std::uint32_t generation = 0; bool active = false; };
    const std::vector<std::byte>& data_;
    Slot slots_[4]{};
};
struct Packet { std::uint32_t stream = 0; std::int64_t pts = 0; std::uint32_t epoch = 0; std::byte first{}; };
// Steps until Done/Failed (or `limit` packets), recording each packet and returning its lease at once.
demux::ParseStatus Drain(demux::IsobmffParser& parser, MemorySource& source, Packet* out, unsigned& count, unsigned limit = 256) {
    core::SpscRingBuffer<core::MediaPacket, 2> queue; count = 0;
    auto status = demux::ParseStatus::Progress;
    for (unsigned step = 0; step < 20000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed && count < limit; ++step) {
        status = parser.Step(queue);
        while (const auto* p = queue.PeekReadSlot()) {
            platform::BlockView view{};
            if (count < 256 && source.Poll({p->payload_block_id, p->payload_generation}, view) == platform::IoStatus::Ready)
                out[count] = {p->stream_id, p->pts_100ns, p->playback_epoch, view.data[p->payload_offset]};
            ++count; CHECK(parser.ReturnPacket(*p)); CHECK(queue.ReleaseReadSlot());
        }
    }
    return status;
}
// The catalogue file: video, two alternative AAC tracks (English enabled, Hebrew not), two subtitle tracks, and four
// tracks the engine cannot use (timecode, AC-3, an audio track with a multi-segment edit list, WebVTT in MP4).
std::vector<Spec> Catalogue() {
    std::vector<Spec> t;
    t.push_back({"vide", "avc1", 1, "und", "VideoHandler", true, 0, 1000, 100, 10});
    t.push_back({"soun", "mp4a", 2, "eng", "English stereo", true, 1, 48000, 1024, 48});
    t.push_back({"soun", "mp4a", 3, "heb", "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa", false, 1, 48000, 1024, 48}); // "עברית"
    Spec heb_text{"sbtl", "tx3g", 4, "heb", "Hebrew subtitles", true, 2, 1000, 500, 2};
    heb_text.texts = {"\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", "\xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d"}; // שלום, עולם
    t.push_back(heb_text);
    Spec eng_text{"text", "tx3g", 5, "eng", "English", false, 2, 1000, 500, 2};
    eng_text.texts = {"Hello", "World"};
    t.push_back(eng_text);
    t.push_back({"tmcd", "tmcd", 6, "und", "TimeCodeHandler", true, 0, 1000, 1000, 1});
    t.push_back({"soun", "ac-3", 7, "spa", "Spanish AC-3", false, 1, 48000, 1536, 32});
    Spec edited{"soun", "mp4a", 8, "fra", "Commentary", false, 1, 48000, 1024, 48}; edited.two_edits = true;
    t.push_back(edited);
    Spec vtt{"text", "wvtt", 9, "deu", nullptr, false, 2, 1000, 500, 2}; vtt.texts = {"Hallo", "Welt"};
    t.push_back(vtt);
    return t;
}
void Cataloguing() {
    const auto file = Build(Catalogue());
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1}));
    CHECK(arena.Seal());
    static Packet packets[256]; unsigned count = 0;
    allocation_probe::Begin();
    const auto status = Drain(parser, source, packets, count);
    CHECK(allocation_probe::End() == 0);
    CHECK(status == demux::ParseStatus::Done && parser.TrackCount() == 9 && parser.SkippedTracks() == 0);
    using demux::TrackKind; using demux::ParseError; using demux::Codec;
    struct Want { TrackKind kind; Codec codec; const char* language; ParseError refusal; bool enabled; unsigned group; const char* entry; };
    const Want want[]{
        {TrackKind::Video, Codec::H264, "und", ParseError::None, true, 0, "avc1"}, {TrackKind::Audio, Codec::Aac, "eng", ParseError::None, true, 1, "mp4a"},
        {TrackKind::Audio, Codec::Aac, "heb", ParseError::None, false, 1, "mp4a"}, {TrackKind::Subtitle, Codec::Tx3g, "heb", ParseError::None, true, 2, "tx3g"},
        {TrackKind::Subtitle, Codec::Tx3g, "eng", ParseError::None, false, 2, "tx3g"}, {TrackKind::Other, Codec::Unknown, "und", ParseError::Unsupported, true, 0, "tmcd"},
        {TrackKind::Audio, Codec::Unknown, "spa", ParseError::Unsupported, false, 1, "ac-3"}, {TrackKind::Audio, Codec::Aac, "fra", ParseError::Unsupported, false, 1, "mp4a"},
        {TrackKind::Subtitle, Codec::Unknown, "deu", ParseError::Unsupported, false, 2, "wvtt"}};
    for (unsigned i = 0; i < 9; ++i) {
        const auto* t = parser.Track(i); if (!t) { CHECK(t); continue; }
        const auto fourcc = [](const char* s) { return static_cast<std::uint32_t>(s[0]) << 24 | static_cast<std::uint32_t>(s[1]) << 16 | static_cast<std::uint32_t>(s[2]) << 8 | static_cast<std::uint32_t>(s[3]); };
        const bool good = t->id == i + 1 && t->kind == want[i].kind && t->refusal == want[i].refusal && std::strcmp(t->language, want[i].language) == 0 &&
                          t->enabled == want[i].enabled && t->alternate_group == want[i].group && t->sample_entry == fourcc(want[i].entry) &&
                          (want[i].refusal != ParseError::None || t->codec == want[i].codec);
        if (!good) { ++failures; std::printf("FAIL track %u: kind %u codec %u language %s refusal %s enabled %d group %u\n", i + 1, static_cast<unsigned>(t->kind),
                                             static_cast<unsigned>(t->codec), t->language, demux::IsobmffParser::Name(t->refusal), t->enabled, t->alternate_group); }
        CHECK(parser.Emitting(i) == (want[i].refusal == ParseError::None && (want[i].kind == TrackKind::Video || want[i].kind == TrackKind::Audio)));
    }
    CHECK(std::strcmp(parser.Track(1)->name, "English stereo") == 0 && std::strcmp(parser.Track(2)->name, "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa") == 0);
    CHECK(parser.Track(8)->name[0] == 0 && parser.Track(3)->sample_count == 2);
    // Published by default: the video and both usable audio tracks, in decode order; nothing of the refused tracks.
    unsigned per[10]{};
    for (unsigned i = 0; i < count; ++i) if (packets[i].stream < 10) ++per[packets[i].stream];
    CHECK(count == 10 + 48 + 48 && per[1] == 10 && per[2] == 48 && per[3] == 48 && per[4] == 0 && per[6] == 0 && per[7] == 0 && per[8] == 0);
    for (unsigned i = 1; i < count; ++i) CHECK(packets[i].pts >= 0);
    // Subtitle samples by lookup: offsets, sizes, times.
    demux::IsobmffParser::SampleRecord s{};
    CHECK(parser.SampleAt(3, 1, s) && s.bytes == 2 + 8 && s.pts_100ns == 5000000 && s.duration_100ns == 5000000);
    CHECK(file[static_cast<std::size_t>(s.offset) + 1] == std::byte{8});
    CHECK(!parser.SampleAt(3, 2, s) && !parser.SampleAt(5, 0, s) && !parser.SampleAt(99, 0, s));
    // A refused track cannot be published.
    CHECK(!parser.SetEmission(6, true) && !parser.Emitting(6));
    CHECK(source.Active() == 0);
}
// One audio track at a time: the other is not published, and a switch continues the video untouched while the new
// track starts at the access unit presenting at the switch time, with the pre-roll asked for.
void Switching() {
    const auto file = Build(Catalogue());
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1}));
    CHECK(arena.Seal());
    static Packet packets[256]; unsigned count = 0;
    allocation_probe::Begin();
    // Metadata first (no packets are published before it); then English only.
    core::SpscRingBuffer<core::MediaPacket, 2> queue;
    for (unsigned step = 0; step < 2000 && !parser.MetadataReady(); ++step) (void)parser.Step(queue);
    CHECK(parser.MetadataReady() && parser.SetEmission(2, false) && !parser.Emitting(2));
    CHECK(!parser.SwitchTrack(1, 2, 0)); // Not before the scan has admitted every sample.
    CHECK(Drain(parser, source, packets, count, 30) != demux::ParseStatus::Failed && count == 30);
    unsigned english = 0, video = 0; std::int64_t last_video = -1, last_english = -1;
    for (unsigned i = 0; i < count; ++i) {
        CHECK(packets[i].stream == 1 || packets[i].stream == 2);
        if (packets[i].stream == 2) { ++english; last_english = packets[i].pts; }
        if (packets[i].stream == 1) { ++video; last_video = packets[i].pts; }
    }
    CHECK(english > 0 && video > 0 && parser.Seekable());
    // Switch at 0.3 s: sample 14 of the Hebrew track presents at 14 * 1024 / 48000 = 0.2987 s; one unit of pre-roll.
    const std::int64_t at = 3000000;
    CHECK(parser.SwitchTrack(1, 2, at, 1) && !parser.Emitting(1) && parser.Emitting(2));
    CHECK(!parser.SwitchTrack(1, 2, at) && !parser.SwitchTrack(2, 2, at) && !parser.SwitchTrack(2, 6, at)); // Not published / same / refused.
    static Packet after[256]; unsigned rest = 0;
    CHECK(Drain(parser, source, after, rest) == demux::ParseStatus::Done);
    unsigned hebrew = 0, more_video = 0; std::int64_t first_hebrew = INT64_MIN, previous_video = last_video;
    bool video_in_order = true; std::byte first_payload{};
    for (unsigned i = 0; i < rest; ++i) {
        CHECK(after[i].stream == 1 || after[i].stream == 3);
        if (after[i].stream == 3) { if (!hebrew++) { first_hebrew = after[i].pts; first_payload = after[i].first; } }
        if (after[i].stream == 1) { ++more_video; video_in_order = video_in_order && after[i].pts > previous_video; previous_video = after[i].pts; }
        CHECK(after[i].epoch == 1);
    }
    // 13 * 1024 / 48000 s = 2773333 (100 ns); the payloads carry the track id and sample index.
    CHECK(first_hebrew == 2773333 && first_payload == std::byte{3 * 16 + 13} && hebrew == 48 - 13);
    CHECK(more_video == 10 - video && video_in_order && last_english >= 0);
    std::printf("Switch: English until %.4f s, Hebrew from %.4f s (one unit of pre-roll before 0.3 s); video %u + %u pictures, none repeated\n",
                static_cast<double>(last_english) / 1e7, static_cast<double>(first_hebrew) / 1e7, video, more_video);
    // After the end of the stream: switching back publishes English again from the switch time.
    CHECK(parser.SwitchTrack(2, 1, 9000000, 0) && Drain(parser, source, after, rest) == demux::ParseStatus::Done);
    CHECK(rest == 48 - 42 && after[0].stream == 2 && after[0].first == std::byte{2 * 16 + 42});
    // A seek positions every usable track, published or not; enabling one afterwards starts it there.
    CHECK(parser.Seek(5000000, 2) && parser.SetEmission(2, true));
    CHECK(Drain(parser, source, after, rest) == demux::ParseStatus::Done);
    unsigned both[4]{};
    for (unsigned i = 0; i < rest; ++i) if (after[i].stream < 4) ++both[after[i].stream];
    CHECK(both[2] == both[3] && both[2] == 48 - 23 && after[0].epoch == 2);
    CHECK(allocation_probe::End() == 0 && source.Active() == 0);
}
// A broken secondary track is refused, not the file; a file whose every track is refused fails with the first reason.
void Refusals() {
    static Packet packets[256]; unsigned count = 0;
    {
        auto tracks = Catalogue(); tracks.resize(3); tracks[2].broken_table = true; // stts counts more samples than stsz.
        const auto file = Build(tracks);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024)); MemorySource source(file);
        demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1})); CHECK(arena.Seal());
        CHECK(Drain(parser, source, packets, count) == demux::ParseStatus::Done && count == 10 + 48);
        CHECK(parser.Track(2)->refusal == demux::ParseError::InvalidTable && parser.Track(1)->refusal == demux::ParseError::None);
    }
    {
        auto tracks = Catalogue(); tracks.resize(2); tracks[1].two_entries = true; // Two sample descriptions.
        const auto file = Build(tracks);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024)); MemorySource source(file);
        demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1})); CHECK(arena.Seal());
        CHECK(Drain(parser, source, packets, count) == demux::ParseStatus::Done && count == 10);
        CHECK(parser.Track(1)->refusal == demux::ParseError::Unsupported && parser.Track(1)->sample_entry == 0x6d703461);
    }
    {
        std::vector<Spec> only{{"tmcd", "tmcd", 1, "und", nullptr}, {"soun", "ac-3", 2, "eng", nullptr, true, 0, 48000, 1536, 4}};
        const auto file = Build(only);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024)); MemorySource source(file);
        demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1})); CHECK(arena.Seal());
        CHECK(Drain(parser, source, packets, count) == demux::ParseStatus::Failed && parser.Diagnostic().error == demux::ParseError::Unsupported);
        CHECK(source.Active() == 0);
    }
    {
        // More tracks than catalogued: the rest are skipped, the file plays.
        auto tracks = Catalogue();
        for (std::uint32_t id = 10; id < 20; ++id) tracks.push_back({"soun", "mp4a", id, "ita", nullptr, false, 1, 48000, 1024, 2});
        const auto file = Build(tracks);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024)); MemorySource source(file);
        demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1})); CHECK(arena.Seal());
        CHECK(Drain(parser, source, packets, count) == demux::ParseStatus::Done && parser.TrackCount() == 16 && parser.SkippedTracks() == 3);
    }
}
// Forward continuation (docs/seeking.md): a seek whose target lies ahead in the group of pictures being read keeps the
// video track where it is and repositions the audio track. Video: 20 pictures at 10 fps, sync samples 0 and 10 (two
// 1 s groups), B-pictures in decode order I P B B P B B P B B, so sample i presents at (d[i] + 1) x 0.1 s with d the
// display index. Audio: AAC-sized units at 48 kHz.
void Continuation() {
    static constexpr std::uint32_t display[10]{0, 3, 1, 2, 6, 4, 5, 9, 7, 8};
    Spec video{"vide", "avc1", 1, "und", nullptr, true, 0, 1000, 100, 20};
    video.sync = {1, 11};
    for (std::uint32_t i = 0; i < 20; ++i) video.composition.push_back((display[i % 10] + 1 - i % 10) * 100);
    const auto file = Build({video, {"soun", "mp4a", 2, "eng", nullptr, true, 0, 48000, 1024, 94}});
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser; CHECK(parser.Initialize(arena, source, {16, 1 << 20, 1}));
    CHECK(arena.Seal());
    static Packet after[256]; unsigned rest = 0;
    allocation_probe::Begin();
    core::SpscRingBuffer<core::MediaPacket, 2> queue;
    std::uint32_t resumed = 99;
    for (unsigned step = 0; step < 2000 && !parser.MetadataReady(); ++step) (void)parser.Step(queue);
    CHECK(parser.MetadataReady() && !parser.Continue(0, 7500000, 2, 1, resumed) && resumed == 0); // Not before the scan.
    // Read until four pictures (samples 0-3: shown 0.1-0.2, 0.4-0.5, 0.2-0.3, 0.3-0.4 s) have been published.
    unsigned pictures = 0, audio = 0;
    for (unsigned step = 0; step < 20000 && pictures < 4; ++step) {
        CHECK(parser.Step(queue) != demux::ParseStatus::Failed);
        while (const auto* p = queue.PeekReadSlot()) {
            (p->stream_id == 1 ? pictures : audio) += 1;
            CHECK(parser.ReturnPacket(*p)); CHECK(queue.ReleaseReadSlot());
        }
    }
    CHECK(pictures == 4 && parser.Seekable() && parser.Epoch() == 1);
    // Refused, nothing moved: an epoch that is not newer; the audio track; a target in the next group of pictures; a
    // target before the end of a published picture (sample 1 shows 0.4-0.5 s, so 0.4999999 s would show it).
    CHECK(!parser.Continue(0, 7500000, 1, 1, resumed) && !parser.Continue(1, 7500000, 2, 1, resumed) && !parser.Continue(9, 7500000, 2, 1, resumed));
    CHECK(!parser.Continue(0, 12500000, 2, 1, resumed) && !parser.Continue(0, 4999999, 2, 1, resumed) && !parser.Continue(0, 3500000, 2, 1, resumed));
    CHECK(parser.Epoch() == 1 && resumed == 0);
    // 0.5 s: every published picture has ended, sample 5 (shown 0.5-0.6 s) is still to come. Video carries on with
    // sample 4 (shown 0.7 s) in the new epoch; audio restarts one unit before the unit presenting at 0.5 s (unit 23,
    // 0.4907 s), at unit 22 = 22 x 1024 / 48000 s.
    CHECK(parser.Continue(0, 5000000, 2, 1, resumed) && resumed == 4 && parser.Epoch() == 2);
    CHECK(Drain(parser, source, after, rest) == demux::ParseStatus::Done);
    unsigned more_pictures = 0, more_audio = 0; std::int64_t first_picture = -1, first_audio = -1; bool epochs = true;
    for (unsigned i = 0; i < rest && i < 256; ++i) {
        epochs = epochs && after[i].epoch == 2;
        if (after[i].stream == 1) { if (!more_pictures++) first_picture = after[i].pts; }
        else if (!more_audio++) first_audio = after[i].pts;
    }
    CHECK(epochs && more_pictures == 16 && first_picture == 7000000 && first_audio == 22 * 1024 * 10000000ll / 48000 && more_audio == 94 - 22);
    // At the end of the video track there is nothing to carry on from: an ordinary seek is needed.
    CHECK(!parser.Continue(0, 15000000, 3, 1, resumed) && parser.Seek(15000000, 3, 1));
    CHECK(allocation_probe::End() == 0 && source.Active() == 0);
    std::printf("Continuation: 4 pictures published, carried on at sample 4 (0.7 s) for a 0.5 s target, audio from %.4f s; %u + %u pictures\n",
                static_cast<double>(first_audio) / 1e7, pictures, more_pictures);
    {
        // Without stss every sample is a sync sample: a seek starts at the target itself, there is nothing to carry on.
        Spec intra{"vide", "avc1", 1, "und", nullptr, true, 0, 1000, 100, 20};
        const auto plain = Build({intra});
        core::LinearMemoryArena intra_arena; CHECK(intra_arena.Initialize(4 * 1024 * 1024)); MemorySource intra_source(plain);
        demux::IsobmffParser intra_parser; CHECK(intra_parser.Initialize(intra_arena, intra_source, {16, 1 << 20, 1})); CHECK(intra_arena.Seal());
        CHECK(Drain(intra_parser, intra_source, after, rest, 3) != demux::ParseStatus::Failed && intra_parser.Seekable());
        CHECK(!intra_parser.Continue(0, 15000000, 2, 0, resumed) && intra_parser.Epoch() == 1);
    }
}
// Every single-byte corruption of the catalogue file terminates without reading out of bounds or allocating.
void Corruption() {
    auto file = Build(Catalogue()); // Mutated in place, one byte at a time, and restored.
    static Packet packets[256]; unsigned count = 0, parsed = 0, failed = 0;
    for (std::size_t i = 0; i < file.size(); i += 2) {
        file[i] ^= std::byte{0xff};
        {
            core::LinearMemoryArena arena; MemorySource source(file);
            demux::IsobmffParser parser;
            if (!arena.Initialize(1536 * 1024) || !parser.Initialize(arena, source, {16, 1 << 20, 1}) || !arena.Seal()) { ++failures; break; }
            allocation_probe::Begin();
            const auto status = Drain(parser, source, packets, count);
            CHECK(allocation_probe::End() == 0);
            CHECK(status == demux::ParseStatus::Done || status == demux::ParseStatus::Failed);
            (status == demux::ParseStatus::Done ? parsed : failed) += 1;
            CHECK(source.Active() == 0);
        }
        file[i] ^= std::byte{0xff};
    }
    std::printf("Corruption: %u mutations admitted, %u refused, 0 out-of-bounds, 0 allocations\n", parsed, failed);
}
}
int main() {
    std::puts("Multi-track demux tests"); // The first output allocates stdout's buffer (seen by the probe in MSVC Debug): before any probe.
    Cataloguing(); Switching(); Refusals(); Continuation(); Corruption();
    std::printf("Multi-track demux: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
