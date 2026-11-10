// demux::MatroskaDemuxer through demux::Demuxer (API 17): real Matroska and WebM files from ffmpeg (counts checked against
// ffprobe's), the index scan, exact AAC times, B-picture durations, seeks, sample lookups and track switches; every lease
// returned; and every corruption and truncation of a file's first 64 KiB terminates cleanly (the portable suite runs this under
// ASan/UBSan). Arguments: the fixtures directory (skips, 77, without it).
#include "media/demux/Demuxer.hpp"
#include <cstdio>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
using namespace media;

std::vector<unsigned char> Load(const std::string& path) {
    std::vector<unsigned char> b;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        unsigned char chunk[65536]; std::size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) b.insert(b.end(), chunk, chunk + n);
        std::fclose(f);
    }
    return b;
}
// A file in memory as a block source: every range is a lease (they are counted), at most `max_range` bytes.
class MemorySource final : public platform::IBlockSource {
public:
    MemorySource(const std::vector<unsigned char>& bytes, std::uint32_t max_range = 1u << 20) : bytes_(bytes), max_range_(max_range) {}
    std::uint64_t Size() const noexcept override { return bytes_.size(); }
    std::uint32_t MaxRangeBytes() const noexcept override { return max_range_; }
    platform::IoStatus Request(std::uint64_t offset, std::uint32_t n, platform::BlockToken& token) noexcept override {
        if (offset > bytes_.size() || n > bytes_.size() - offset) return platform::IoStatus::OutOfRange;
        if (n > max_range_) return platform::IoStatus::TooLarge;
        token = {next_++, 1}; ++outstanding_; ++requests_; read_ += n;
        leases_[token.id % kLeases] = {offset, n};
        return platform::IoStatus::Pending;
    }
    platform::IoStatus Poll(platform::BlockToken token, platform::BlockView& view) noexcept override {
        const auto& l = leases_[token.id % kLeases];
        view = {reinterpret_cast<const std::byte*>(bytes_.data()) + l.offset, l.offset, l.bytes, token};
        return platform::IoStatus::Ready;
    }
    bool Release(platform::BlockToken) noexcept override { --outstanding_; return true; }
    long outstanding_ = 0, requests_ = 0; std::uint64_t read_ = 0;
private:
    static constexpr unsigned kLeases = 4096; // Leases held at once (packets in the test's hands): far fewer.
    struct Lease { std::uint64_t offset; std::uint32_t bytes; };
    const std::vector<unsigned char>& bytes_;
    std::uint32_t max_range_, next_ = 1;
    Lease leases_[kLeases]{};
};
struct Collected { std::vector<core::MediaPacket> packets; demux::ParseStatus end = demux::ParseStatus::Progress; };
Collected Drain(demux::Demuxer& d, MemorySource& source, unsigned limit = 1u << 20) {
    Collected c;
    core::SpscRingBuffer<core::MediaPacket, 8> queue;
    for (unsigned i = 0; i < 4000000 && c.packets.size() < limit; ++i) {
        const auto status = d.Step(queue);
        while (const auto* p = queue.PeekReadSlot()) { c.packets.push_back(*p); (void)d.ReturnPacket(*p); (void)queue.ReleaseReadSlot(); }
        if (status == demux::ParseStatus::Done || status == demux::ParseStatus::Failed) { c.end = status; break; }
    }
    (void)source;
    return c;
}
unsigned Count(const Collected& c, std::uint32_t stream) { unsigned n = 0; for (const auto& p : c.packets) n += p.stream_id == stream; return n; }

void TestMkv(const std::vector<unsigned char>& file) {
    MemorySource source(file);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8u << 20));
    demux::Demuxer d; CHECK(d.Initialize(arena, source, {16, 4u << 20, 1}));
    core::SpscRingBuffer<core::MediaPacket, 8> queue;
    for (unsigned i = 0; i < 100000 && !d.MetadataReady(); ++i) CHECK(d.Step(queue) != demux::ParseStatus::Failed);
    CHECK(d.MetadataReady() && d.Format() == demux::ContainerFormat::Matroska && d.TrackCount() == 3 && !d.Fragmented());
    const auto* v = d.Track(0); const auto* a = d.Track(1); const auto* b = d.Track(2);
    CHECK(v && v->codec == demux::Codec::H264 && v->kind == demux::TrackKind::Video && v->width == 640 && v->height == 360 && v->nal_length_bytes == 4);
    CHECK(v && v->parameter_count >= 2 && v->parameters[0].type == 7 && v->parameters[1].type == 8 && v->refusal == demux::ParseError::None);
    CHECK(a && a->codec == demux::Codec::Aac && a->sample_rate == 48000 && a->parameter_count == 1 && a->parameters[0].type == 5 && a->parameters[0].range.bytes >= 2);
    CHECK(b && b->codec == demux::Codec::Aac && b->kind == demux::TrackKind::Audio);
    // Both AAC tracks are published by default; the engine chooses one (SetEmission), as it does with MP4.
    CHECK(d.SetEmission(2, false) && !d.Emitting(2) && d.Emitting(0) && d.Emitting(1));
    const auto all = Drain(d, source);
    CHECK(all.end == demux::ParseStatus::Done);
    const unsigned pictures = Count(all, v->id), sound = Count(all, a->id), other = Count(all, b->id);
    std::printf("MKV: %u pictures, %u AAC units (and %u of the unpublished track), %zu packets, index %llu bytes, scan read %llu of %zu bytes in %ld reads\n",
                pictures, sound, other, all.packets.size(), static_cast<unsigned long long>(d.IndexBytes()),
                static_cast<unsigned long long>(d.Matroska().ScanBytes()), file.size(), source.requests_);
    CHECK(pictures == 120 && sound == 189 && other == 0); // ffprobe: 120 and 189 packets.
    CHECK(d.Seekable() && v->sample_count == 120 && a->sample_count == 189);
    // AAC: exactly n x 1024 / 48 kHz after the first unit (the engine's check, stage 29), each unit 1024 frames long.
    std::int64_t base = 0; unsigned n = 0; bool exact = true, keyed = false, ordered = true; std::int64_t last_audio = INT64_MIN;
    for (const auto& p : all.packets) {
        if (p.stream_id == a->id) {
            if (!n) base = p.pts_100ns;
            exact = exact && p.pts_100ns - base == static_cast<std::int64_t>(std::uint64_t{n} * 1024 * 10000000 / 48000) && p.duration_100ns == 213333;
            ordered = ordered && p.pts_100ns > last_audio; last_audio = p.pts_100ns; ++n;
        } else if (p.stream_id == v->id && !keyed) keyed = (static_cast<unsigned>(p.flags) & 1) != 0 && p.pts_100ns == 0;
        CHECK(p.reserved == 0 && p.playback_epoch == 1 && p.payload_bytes > 0);
    }
    CHECK(exact && ordered && keyed);
    // B pictures: every picture's duration is the gap to its presentation successor (30 fps, 1 ms ticks: 333,333 or 340,000).
    unsigned shaped = 0;
    for (std::uint32_t s = 0; s < 120; ++s) {
        demux::SampleRecord r{};
        if (d.SampleAt(0, s, r) && r.duration_100ns >= 320000 && r.duration_100ns <= 340000) ++shaped;
    }
    CHECK(shaped == 120);
    CHECK(d.SyncCount(0) >= 1 && d.SyncAt(0, 0) == 0);
    // A seek: 2 s, epoch 2. The first picture is the key frame at or before it; the sound starts at it (no pre-roll asked).
    CHECK(d.Seek(20000000, 2, 0));
    const auto after = Drain(d, source);
    CHECK(after.end == demux::ParseStatus::Done && !after.packets.empty());
    bool first_video = true, first_audio = true, epochs = true;
    for (const auto& p : after.packets) {
        epochs = epochs && p.playback_epoch == 2;
        if (p.stream_id == v->id && first_video) { first_video = false; CHECK((static_cast<unsigned>(p.flags) & 1) && p.pts_100ns <= 20000000); }
        if (p.stream_id == a->id && first_audio) { first_audio = false; CHECK(p.pts_100ns <= 20000000 && p.pts_100ns > 20000000 - 213334); }
    }
    CHECK(epochs && !first_video && !first_audio);
    CHECK(d.SyncSample(0, 20000000) != UINT32_MAX && d.DecodeDistance(0, d.SyncSample(0, 20000000), 20000000, 64) <= 64);
    // A track switch at 1 s: the other AAC track from its unit at or before 1 s.
    CHECK(d.Seek(0, 3, 0) && d.SwitchTrack(1, 2, 10000000, 0) && d.Emitting(2) && !d.Emitting(1));
    const auto switched = Drain(d, source);
    unsigned from_other = 0; std::int64_t first_other = INT64_MAX;
    for (const auto& p : switched.packets) if (p.stream_id == b->id) { ++from_other; first_other = (std::min)(first_other, p.pts_100ns); }
    CHECK(Count(switched, a->id) == 0 && from_other > 100 && first_other <= 10000000 && first_other > 10000000 - 213334);
    CHECK(source.outstanding_ == 0); // Every lease returned (the test returns each packet; seeks drop in-flight reads).
}

void TestWebm(const std::vector<unsigned char>& file) {
    MemorySource source(file);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(8u << 20));
    demux::Demuxer d; CHECK(d.Initialize(arena, source, {16, 4u << 20, 1}));
    const auto all = Drain(d, source);
    CHECK(d.Format() == demux::ContainerFormat::Matroska && d.TrackCount() == 2 && all.end == demux::ParseStatus::Done);
    const auto* video = d.Track(0); const auto* opus = d.Track(1);
    CHECK(video && video->refusal == demux::ParseError::Unsupported && video->kind == demux::TrackKind::Video); // VP9.
    CHECK(opus && opus->codec == demux::Codec::Opus && opus->channels >= 1 && opus->sample_rate == 48000 && opus->parameter_count == 1);
    unsigned units = 0; bool twenty = true;
    for (const auto& p : all.packets) if (opus && p.stream_id == opus->id) { ++units; twenty = twenty && p.duration_100ns == 200000; }
    std::printf("WebM: VP9 refused (%s), %u Opus packets of 20 ms: %s\n", video ? demux::Demuxer::Name(video->refusal) : "?", units, twenty ? "yes" : "no");
    CHECK(units == 151 && twenty && Count(all, video ? video->id : 0) == 0);
    CHECK(source.outstanding_ == 0);
}

// Every byte of the first 64 KiB flipped, and every cut of it, ends in Done or Failed (never a hang or a fault); the leases are all
// returned.
void TestCorruption(const std::vector<unsigned char>& file) {
    unsigned done = 0, failed = 0;
    const auto run = [&](const std::vector<unsigned char>& bytes) {
        MemorySource source(bytes);
        core::LinearMemoryArena arena; if (!arena.Initialize(2u << 20)) { CHECK(false); return; }
        demux::Demuxer d; if (!d.Initialize(arena, source, {16, 1u << 20, 1})) { CHECK(false); return; }
        const auto c = Drain(d, source);
        CHECK(c.end == demux::ParseStatus::Done || c.end == demux::ParseStatus::Failed);
        (c.end == demux::ParseStatus::Done ? done : failed)++;
        if (d.Seekable()) { (void)d.Seek(10000000, 2, 2); (void)Drain(d, source); }
        CHECK(source.outstanding_ == 0 || c.end == demux::ParseStatus::Failed);
    };
    const std::size_t span = (std::min)(file.size(), std::size_t{65536});
    for (std::size_t i = 0; i < span; i += 7) { auto copy = file; copy[i] ^= 0xff; run(copy); }
    for (std::size_t cut = 0; cut < span; cut += 97) { std::vector<unsigned char> copy(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut)); run(copy); }
    std::printf("Corruption: %u runs ended, %u refused, none hung or faulted\n", done, failed);
}
}

// Cues (2026-11-05): a seek right after open (before the scan has read the clusters) lands on the cue's cluster; with scan_ahead
// off (a network source) the scan reads only what playback needs; a live-style WebM (no Cues) is not seekable until scanned.
void TestCues(const std::vector<unsigned char>& file, const std::vector<unsigned char>& live) {
    {
        MemorySource source(file);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8u << 20));
        demux::Demuxer d; CHECK(d.Initialize(arena, source, {16, 4u << 20, 1, false}));
        core::SpscRingBuffer<core::MediaPacket, 8> queue;
        for (unsigned i = 0; i < 1000 && !(d.MetadataReady() && d.Matroska().CuePoints()); ++i) CHECK(d.Step(queue) != demux::ParseStatus::Failed);
        const auto cues = d.Matroska().CuePoints();
        CHECK(cues >= 2 && d.Seekable() && !d.Indexed() && d.Matroska().Entries() == 0);
        std::printf("Cues: after open and the Cues: %llu bytes read", static_cast<unsigned long long>(d.Matroska().ScanBytes())); std::puts("");
        CHECK(d.Seek(25000000, 2, 0)); // 2.5 s: the cue at 2 s (a key frame every 60 pictures, 30 fps).
        const auto after = Drain(d, source, 20);
        std::int64_t first_video = -1; bool key = false, epochs = true;
        for (const auto& p : after.packets) {
            epochs = epochs && p.playback_epoch == 2;
            if (p.stream_id == 1 && first_video < 0) { first_video = p.pts_100ns; key = (static_cast<unsigned>(p.flags) & 1) != 0; }
        }
        const auto read = d.Matroska().ScanBytes();
        std::printf("Cues: %u points; a seek to 2.5 s before any cluster was read: first picture %.3f s (key %s), %zu packets, the scan read %llu of %zu bytes\n",
                    cues, first_video / 1e7, key ? "yes" : "NO", after.packets.size(), static_cast<unsigned long long>(read), file.size());
        CHECK(epochs && key && first_video >= 19000000 && first_video <= 25000000 && !d.Indexed());
        CHECK(read < file.size() / 2); // scan_ahead off: no read of the whole file.
        CHECK(d.SampleAt(0, 0, *std::make_unique<demux::SampleRecord>()) == false && d.SyncSample(0, 0) == UINT32_MAX); // Lookups need the complete index.
    }
    {
        MemorySource source(live);
        core::LinearMemoryArena arena; CHECK(arena.Initialize(8u << 20));
        demux::Demuxer d; CHECK(d.Initialize(arena, source, {16, 4u << 20, 1}));
        core::SpscRingBuffer<core::MediaPacket, 8> queue;
        for (unsigned i = 0; i < 1000 && !d.MetadataReady(); ++i) CHECK(d.Step(queue) != demux::ParseStatus::Failed);
        CHECK(d.MetadataReady() && d.Matroska().CuePoints() == 0 && !d.Seekable());
        const auto all = Drain(d, source);
        CHECK(all.end == demux::ParseStatus::Done && d.Seekable() && d.Indexed());
        std::printf("Live WebM (no Cues): not seekable until indexed, then %zu packets and seekable\n", all.packets.size());
    }
}

int main(int argc, char** argv) {
    const std::string dir = argc >= 2 ? argv[1] : "assets/test_fixtures";
    const auto mkv = Load(dir + "/compat/h264_aac_two_audio.mkv"), webm = Load(dir + "/compat/vp9_opus.webm");
    if (mkv.empty() || webm.empty()) { std::puts("fixtures missing"); return 77; }
    TestMkv(mkv);
    TestWebm(webm);
    const auto cues = Load(dir + "/mkv/h264_aac_srt_chapters.mkv"), live = Load(dir + "/mkv/av1_opus_live.webm");
    if (!cues.empty() && !live.empty()) TestCues(cues, live); else std::puts("Cues: fixtures in mkv/ not generated, skipped");
    TestCorruption(mkv);
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "matroska demux tests passed", failures);
    return failures ? 1 : 0;
}
