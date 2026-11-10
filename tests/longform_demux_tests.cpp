// Long-form sample indexes. Multi-hour, multi-gigabyte MP4s are synthesised: the moov (with sample tables of up to
// a few hundred thousand entries each) is real bytes, while the mdat payload is virtual. Each read of it is stamped
// with its own file offset, so packet identity is still checked without gigabytes of memory. An independent model
// of every sample checks each emitted packet, and every seek landing against a brute-force search.
#include "media/demux/IsobmffParser.hpp"
#include "allocation_probe.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { if (failures < 20) std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)

std::uint64_t Mix(std::uint64_t x) noexcept {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; return x ^ (x >> 33);
}
std::byte Stamp(const std::uint64_t offset) noexcept { return static_cast<std::byte>(Mix(offset) & 255); }

struct Spec {
    const char* name;
    std::uint32_t seconds;
    std::uint32_t timescale, delta; // Nominal video frame duration.
    bool vfr;                       // Jittered durations: one stts run per 1-3 samples.
    bool bframes;                   // Per-sample ctts (I P B pattern) and a single-entry edit list.
    bool wide;                      // co64, payload past 4 GiB.
    std::uint32_t video_chunk;      // Nominal samples per video chunk, varied by +-1 (stsc runs).
    std::uint32_t video_bytes;      // Mean video sample size.
    bool full_pass;                 // Emit and check every packet of the file.
};
struct TrackModel {
    std::uint32_t id = 0, timescale = 0;
    std::uint64_t edit = 0;
    bool audio = false;
    std::vector<std::uint64_t> offset, dts;
    std::vector<std::uint32_t> size, duration;
    std::vector<std::int64_t> composition, pts_100ns;
    std::vector<std::uint8_t> key;
    std::vector<std::uint32_t> chunk_first, chunk_count; // First sample and sample count of each chunk.
    std::size_t Count() const noexcept { return size.size(); }
};
struct Synthetic {
    std::vector<std::byte> head; // ftyp, moov and the mdat header; everything after is virtual payload.
    std::uint64_t size = 0;
    TrackModel tracks[2];
};

struct Writer {
    std::vector<std::byte>& b;
    void B(unsigned v) { b.push_back(static_cast<std::byte>(v & 255)); }
    void U16(unsigned v) { B(v >> 8); B(v); }
    void U32(std::uint32_t v) { B(v >> 24); B(v >> 16); B(v >> 8); B(v); }
    void U64(std::uint64_t v) { U32(static_cast<std::uint32_t>(v >> 32)); U32(static_cast<std::uint32_t>(v)); }
    void Zero(unsigned n) { while (n--) B(0); }
    std::size_t Box(const char* tag) { const auto at = b.size(); U32(0); for (unsigned i = 0; i < 4; ++i) B(static_cast<unsigned char>(tag[i])); return at; }
    void End(std::size_t at) {
        const auto size = static_cast<std::uint32_t>(b.size() - at);
        for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::byte>((size >> (24 - 8 * i)) & 255);
    }
};

void Samples(const Spec& spec, Synthetic& file) {
    std::uint64_t seed = 0x5eed;
    auto next = [&seed](std::uint32_t range) { seed = Mix(seed + 0x9e3779b97f4a7c15ull); return static_cast<std::uint32_t>(seed % range); };
    auto& video = file.tracks[0];
    video.id = 1; video.timescale = spec.timescale; video.edit = spec.bframes ? 2ull * spec.delta : 0;
    const auto frames = static_cast<std::uint32_t>(std::uint64_t{spec.seconds} * spec.timescale / spec.delta);
    const std::uint32_t gop = (spec.timescale + spec.delta / 2) / spec.delta; // One second.
    std::uint64_t dts = 0; std::uint32_t run_left = 0, run_delta = spec.delta;
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (spec.vfr && run_left == 0) { run_left = 1 + next(3); run_delta = spec.delta - 1 + next(3); }
        const auto duration = spec.vfr ? run_delta : spec.delta;
        if (spec.vfr) --run_left;
        const auto position = i % gop;
        const std::int64_t composition = spec.bframes ? static_cast<std::int64_t>(position % 3 == 0 ? 2 : position % 3 == 1 ? 4 : 1) * spec.delta : 0;
        video.dts.push_back(dts); video.duration.push_back(duration); video.composition.push_back(composition);
        video.key.push_back(position == 0);
        video.size.push_back(position == 0 ? spec.video_bytes * 4 + next(spec.video_bytes) : spec.video_bytes / 2 + next(spec.video_bytes));
        dts += duration;
    }
    auto& audio = file.tracks[1];
    audio.id = 2; audio.timescale = 48000; audio.audio = true;
    const auto frames_audio = static_cast<std::uint32_t>(std::uint64_t{spec.seconds} * 48000 / 1024);
    for (std::uint32_t i = 0; i < frames_audio; ++i) {
        audio.dts.push_back(std::uint64_t{i} * 1024); audio.duration.push_back(1024); audio.composition.push_back(0);
        audio.key.push_back(1); audio.size.push_back(150 + next(300));
    }
    // Chunks: video varies around its nominal size, audio holds 24 access units (about half a second).
    for (std::uint32_t i = 0; i < frames;) {
        const auto count = (std::min)(frames - i, spec.video_chunk > 1 ? spec.video_chunk - 1 + next(3) : 1u);
        video.chunk_first.push_back(i); video.chunk_count.push_back(count); i += count;
    }
    for (std::uint32_t i = 0; i < frames_audio;) {
        const auto count = (std::min)(frames_audio - i, 24u);
        audio.chunk_first.push_back(i); audio.chunk_count.push_back(count); i += count;
    }
    // Interleave chunks by start time; offsets are relative to the payload for now.
    std::uint64_t position = 0; std::size_t v = 0, a = 0;
    video.offset.resize(frames); audio.offset.resize(frames_audio);
    while (v < video.chunk_first.size() || a < audio.chunk_first.size()) {
        const bool take_video = a == audio.chunk_first.size() || (v < video.chunk_first.size() &&
            video.dts[video.chunk_first[v]] * 48000 <= audio.dts[audio.chunk_first[a]] * video.timescale);
        auto& track = take_video ? video : audio;
        auto& c = take_video ? v : a;
        for (std::uint32_t k = 0; k < track.chunk_count[c]; ++k) {
            const auto s = track.chunk_first[c] + k; track.offset[s] = position; position += track.size[s];
        }
        ++c;
    }
    file.size = position; // Payload bytes, until the header is known.
}

void Track(Writer& w, const TrackModel& t, const Spec& spec, std::uint64_t base) {
    const auto count = static_cast<std::uint32_t>(t.Count());
    const auto trak = w.Box("trak");
    auto box = w.Box("tkhd"); w.U32(7); w.Zero(8); w.U32(t.id); w.Zero(68); w.End(box);
    const auto media_duration = t.dts.back() + t.duration.back();
    if (t.edit) {
        const auto edts = w.Box("edts"); box = w.Box("elst"); w.U32(0); w.U32(1);
        w.U32(static_cast<std::uint32_t>((media_duration - t.edit) * 1000 / t.timescale)); w.U32(static_cast<std::uint32_t>(t.edit)); w.U32(0x10000);
        w.End(box); w.End(edts);
    }
    const auto mdia = w.Box("mdia");
    box = w.Box("mdhd"); w.U32(0); w.Zero(8); w.U32(t.timescale); w.U32(static_cast<std::uint32_t>(media_duration)); w.U16(0x55c4); w.U16(0); w.End(box);
    box = w.Box("hdlr"); w.U32(0); w.U32(0); for (const char c : std::string_view(t.audio ? "soun" : "vide")) w.B(static_cast<unsigned char>(c)); w.Zero(12); w.End(box);
    const auto minf = w.Box("minf");
    const auto dinf = w.Box("dinf"); box = w.Box("dref"); w.U32(0); w.U32(1); const auto url = w.Box("url "); w.U32(1); w.End(url); w.End(box); w.End(dinf);
    const auto stbl = w.Box("stbl");
    box = w.Box("stsd"); w.U32(0); w.U32(1);
    const auto entry = w.Box(t.audio ? "mp4a" : "avc1"); w.Zero(6); w.U16(1);
    if (!t.audio) {
        w.Zero(16); w.U16(1920); w.U16(1080); w.Zero(50);
        const auto config = w.Box("avcC"); w.B(1); w.B(66); w.B(0); w.B(30); w.B(255); w.B(225); w.U16(2); w.B(0x67); w.B(0x42); w.B(1); w.U16(2); w.B(0x68); w.B(0); w.End(config);
    } else {
        w.Zero(8); w.U16(2); w.U16(16); w.Zero(4); w.U32(48000u << 16);
        const auto config = w.Box("esds"); w.U32(0); w.B(3); w.B(25); w.U16(2); w.B(0);
        w.B(4); w.B(17); w.B(0x40); w.B(0x15); w.Zero(11); w.B(5); w.B(2); w.B(0x11); w.B(0x90); w.B(6); w.B(1); w.B(2); w.End(config);
    }
    w.End(entry); w.End(box);
    // stts / ctts: one row per run of equal values, exactly as a muxer writes them.
    auto runs = [&](const char* tag, const auto& values, unsigned version) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto value = static_cast<std::uint32_t>(values[i]);
            if (!rows.empty() && rows.back().second == value) ++rows.back().first;
            else rows.push_back({1, value});
        }
        const auto b = w.Box(tag); w.U32(version << 24); w.U32(static_cast<std::uint32_t>(rows.size()));
        for (const auto& [n, value] : rows) { w.U32(n); w.U32(value); }
        w.End(b);
    };
    runs("stts", t.duration, 0);
    if (!t.audio && spec.bframes) runs("ctts", t.composition, 0);
    box = w.Box("stsc"); {
        std::vector<std::array<std::uint32_t, 2>> rows;
        for (std::uint32_t c = 0; c < t.chunk_count.size(); ++c)
            if (rows.empty() || rows.back()[1] != t.chunk_count[c]) rows.push_back({c + 1, t.chunk_count[c]});
        w.U32(0); w.U32(static_cast<std::uint32_t>(rows.size()));
        for (const auto& r : rows) { w.U32(r[0]); w.U32(r[1]); w.U32(1); }
    } w.End(box);
    box = w.Box("stsz"); w.U32(0); w.U32(0); w.U32(count); for (const auto s : t.size) w.U32(s); w.End(box);
    box = w.Box(spec.wide ? "co64" : "stco"); w.U32(0); w.U32(static_cast<std::uint32_t>(t.chunk_first.size()));
    for (const auto first : t.chunk_first) {
        if (spec.wide) w.U64(base + t.offset[first]); else w.U32(static_cast<std::uint32_t>(base + t.offset[first]));
    }
    w.End(box);
    if (!t.audio) {
        std::uint32_t keys = 0; for (const auto k : t.key) keys += k;
        box = w.Box("stss"); w.U32(0); w.U32(keys); for (std::uint32_t i = 0; i < count; ++i) if (t.key[i]) w.U32(i + 1); w.End(box);
    }
    w.End(stbl); w.End(minf); w.End(mdia); w.End(trak);
}

Synthetic Build(const Spec& spec) {
    Synthetic file; Samples(spec, file);
    const auto payload = file.size;
    std::uint64_t base = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        // The moov's size does not depend on offset values: the first pass measures it, the second writes them.
        file.head.clear();
        Writer w{file.head};
        auto box = w.Box("ftyp"); w.U32(0x69736f6d); w.U32(0); w.U32(0x69736f6d); w.End(box);
        const auto moov = w.Box("moov");
        box = w.Box("mvhd"); w.U32(0); w.Zero(8); w.U32(1000); w.U32(spec.seconds * 1000); w.Zero(80); w.End(box);
        Track(w, file.tracks[0], spec, base); Track(w, file.tracks[1], spec, base);
        w.End(moov);
        const bool large = payload + 8 > UINT32_MAX;
        base = file.head.size() + (large ? 16 : 8);
        if (pass == 1) {
            if (large) { w.U32(1); w.B('m'); w.B('d'); w.B('a'); w.B('t'); w.U64(payload + 16); }
            else { w.U32(static_cast<std::uint32_t>(payload + 8)); w.B('m'); w.B('d'); w.B('a'); w.B('t'); }
        }
    }
    for (auto& t : file.tracks) {
        for (auto& o : t.offset) o += base;
        t.pts_100ns.resize(t.Count());
        for (std::size_t i = 0; i < t.Count(); ++i)
            CHECK(demux::To100ns(static_cast<std::int64_t>(t.dts[i]) + t.composition[i] - static_cast<std::int64_t>(t.edit), t.timescale, t.pts_100ns[i]));
    }
    file.size = base + payload;
    return file;
}

// Serves the real header bytes, and stamps the first bytes of every virtual payload read with their offsets.
class SyntheticSource final : public platform::IBlockSource {
public:
    SyntheticSource(const Synthetic& file, std::uint32_t max_range) : file_(file), max_range_(max_range) {
        for (auto& slot : slots_) slot.buffer.resize(max_range);
    }
    std::uint64_t Size() const noexcept override { return file_.size; }
    std::uint32_t MaxRangeBytes() const noexcept override { return max_range_; }
    platform::IoStatus Request(std::uint64_t offset, std::uint32_t bytes, platform::BlockToken& token) noexcept override {
        token = {};
        if (!bytes || offset > Size() || bytes > Size() - offset) return platform::IoStatus::OutOfRange;
        if (bytes > max_range_) return platform::IoStatus::TooLarge;
        for (std::uint32_t i = 0; i < 8; ++i) {
            auto& slot = slots_[i];
            if (slot.active) continue;
            slot.active = true; ++slot.generation; token = {i, slot.generation};
            std::uint32_t done = 0;
            if (offset < file_.head.size()) {
                done = static_cast<std::uint32_t>((std::min<std::uint64_t>)(bytes, file_.head.size() - offset));
                std::memcpy(slot.buffer.data(), file_.head.data() + offset, done);
            }
            for (std::uint32_t k = done; k < bytes && k < done + 16; ++k) slot.buffer[k] = Stamp(offset + k);
            slot.view = {slot.buffer.data(), offset, bytes, token};
            ++requests; if (bytes > largest) largest = bytes;
            return platform::IoStatus::Pending;
        }
        return platform::IoStatus::WouldBlock;
    }
    platform::IoStatus Poll(platform::BlockToken token, platform::BlockView& view) noexcept override {
        if (token.id >= 8 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return platform::IoStatus::InvalidToken;
        view = slots_[token.id].view; return platform::IoStatus::Ready;
    }
    bool Release(platform::BlockToken token) noexcept override {
        if (token.id >= 8 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return false;
        slots_[token.id].active = false; return true;
    }
    unsigned Active() const noexcept { unsigned n = 0; for (const auto& slot : slots_) n += slot.active ? 1u : 0u; return n; }
    std::uint64_t requests = 0; std::uint32_t largest = 0;
private:
    struct Slot { std::vector<std::byte> buffer; platform::BlockView view{}; std::uint32_t generation = 0; bool active = false; };
    const Synthetic& file_;
    std::uint32_t max_range_;
    Slot slots_[8]{};
};

using Queue = core::SpscRingBuffer<core::MediaPacket, 4>;
struct Emission {
    std::size_t next[2]{}, first[2]{SIZE_MAX, SIZE_MAX}; // first: the landing sample, found from its file offset.
    std::uint64_t packets = 0;
    std::int64_t last_dts = INT64_MIN;
    std::uint32_t epoch = 1;
};
// Steps until `limit` packets (or Done), checking each packet against the model in emission order.
demux::ParseStatus Drain(demux::IsobmffParser& parser, Queue& queue, SyntheticSource& source, const Synthetic& file,
                         Emission& e, std::uint64_t limit) {
    auto status = demux::ParseStatus::Progress;
    const auto stop = e.packets + limit;
    while (e.packets < stop && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed) {
        status = parser.Step(queue);
        while (const auto* packet = queue.PeekReadSlot()) {
            const unsigned t = packet->stream_id == 1 ? 0u : 1u;
            const auto& m = file.tracks[t];
            platform::BlockView view{};
            CHECK(source.Poll({packet->payload_block_id, packet->payload_generation}, view) == platform::IoStatus::Ready);
            const auto at = view.file_offset + packet->payload_offset;
            if (e.first[t] == SIZE_MAX) { // A track's offsets ascend with its sample numbers.
                e.first[t] = static_cast<std::size_t>(std::lower_bound(m.offset.begin(), m.offset.end(), at) - m.offset.begin());
                e.next[t] = e.first[t];
            }
            const auto i = e.next[t]++;
            CHECK(packet->stream_id == m.id && packet->playback_epoch == e.epoch);
            CHECK(i < m.Count());
            if (i < m.Count()) {
                std::int64_t dts = 0, end = 0;
                (void)demux::To100ns(static_cast<std::int64_t>(m.dts[i]) - static_cast<std::int64_t>(m.edit), m.timescale, dts);
                (void)demux::To100ns(static_cast<std::int64_t>(m.dts[i] + m.duration[i]) - static_cast<std::int64_t>(m.edit), m.timescale, end);
                CHECK(packet->dts_100ns == dts && packet->pts_100ns == m.pts_100ns[i] && packet->duration_100ns == end - dts);
                CHECK(packet->payload_bytes == m.size[i]);
                CHECK((packet->flags == core::PacketFlags::KeyFrame) == (m.key[i] != 0));
                CHECK(at == m.offset[i]);
                CHECK(view.data[packet->payload_offset] == Stamp(m.offset[i])); // The bytes read for this offset.
            }
            CHECK(packet->dts_100ns >= e.last_dts); // Tracks interleave by decode time.
            e.last_dts = packet->dts_100ns; ++e.packets;
            CHECK(parser.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot());
        }
    }
    return status;
}
// Brute force over the model: the sample Seek must start each track from.
std::size_t Expected(const TrackModel& m, const std::int64_t target, const std::uint32_t preroll) {
    std::size_t found = SIZE_MAX, first_key = SIZE_MAX;
    for (std::size_t i = 0; i < m.Count(); ++i) {
        if (!m.key[i]) continue;
        if (first_key == SIZE_MAX) first_key = i;
        if (m.pts_100ns[i] <= target) found = i;
    }
    if (found == SIZE_MAX) found = first_key;
    if (m.audio) found -= (std::min<std::size_t>)(found, preroll);
    return found;
}

void LongForm(const Spec& spec) {
    const auto built = std::chrono::steady_clock::now();
    const auto file = Build(spec);
    const auto& video = file.tracks[0]; const auto& audio = file.tracks[1];
    const auto samples = video.Count() + audio.Count();
    const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - built).count();
    // Production defaults: the pool size the coordinator reserves.
    core::LinearMemoryArena arena; CHECK(arena.Initialize(demux::ParserLimits{}.index_bytes + 1024 * 1024));
    SyntheticSource source(file, 256 * 1024);
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, source, {2, demux::ParserLimits{}.index_bytes, 1}));
    Queue queue;
    CHECK(arena.Seal());
    const auto arena_allocations = arena.Statistics().allocation_count;
    allocation_probe::Begin();
    // Scan and validation: metadata, then every sample checked in bounded slices before the first packet.
    auto clock = std::chrono::steady_clock::now();
    double longest_step = 0; std::uint64_t steps = 0;
    auto status = demux::ParseStatus::Progress;
    while (!parser.Seekable() && status != demux::ParseStatus::Failed && status != demux::ParseStatus::Done && steps < 10000000) {
        const auto before = std::chrono::steady_clock::now();
        status = parser.Step(queue); ++steps;
        longest_step = (std::max)(longest_step, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
        if (queue.PeekReadSlot()) break; // First packet: the scan is complete.
    }
    const double parse_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clock).count();
    CHECK(status != demux::ParseStatus::Failed && parser.MetadataReady() && parser.Seekable());
    CHECK(parser.Track(0)->sample_count == video.Count() && parser.Track(1)->sample_count == audio.Count());
    if (!spec.full_pass) while (const auto* packet = queue.PeekReadSlot()) { CHECK(parser.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot()); }
    const auto index = parser.IndexBytes();
    // Full pass: every packet of the file, in order.
    Emission e{};
    double pass_s = 0;
    if (spec.full_pass) {
        clock = std::chrono::steady_clock::now();
        status = Drain(parser, queue, source, file, e, UINT64_MAX);
        pass_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock).count();
        CHECK(status == demux::ParseStatus::Done && e.next[0] == video.Count() && e.next[1] == audio.Count() && e.packets == samples);
        CHECK(e.first[0] == 0 && e.first[1] == 0);
    }
    // Seeks across the whole duration (1 h, 2 h, 3 h 30 min when present), then scattered and edge targets. Each
    // lands on the brute-force answer for both tracks and then plays on sample for sample.
    std::int64_t targets[24]{}; unsigned target_count = 0;
    for (const std::int64_t hours_x2 : {2, 4, 7}) {
        const auto t = hours_x2 * 1800 * 10000000ll + 1234567; // Half hours.
        if (t < std::int64_t{spec.seconds} * 10000000 - 50000000) targets[target_count++] = t;
    }
    const unsigned to_end = target_count; // Plays on from 20 s before the end to end of stream.
    CHECK(to_end >= 2); // 1 h and 2 h, and 3 h 30 min in the longer files.
    targets[target_count++] = std::int64_t{spec.seconds - 20} * 10000000 + 3333333;
    for (std::uint64_t k = 0; k < 12; ++k) targets[target_count++] = static_cast<std::int64_t>(Mix(k + 17) % (std::uint64_t{spec.seconds} * 10000000));
    targets[target_count++] = 0; targets[target_count++] = -50000000; targets[target_count++] = INT64_MAX; targets[target_count++] = 6000000;
    double worst_seek_us = 0, total_seek_us = 0;
    std::uint32_t epoch = 1;
    std::size_t landed_video[24]{}, landed_audio[24]{}, want_video[24]{}, want_audio[24]{};
    for (unsigned s = 0; s < target_count; ++s) {
        want_video[s] = Expected(video, targets[s], 2); want_audio[s] = Expected(audio, targets[s], 2);
        const auto before = std::chrono::steady_clock::now();
        CHECK(parser.Seek(targets[s], ++epoch, 2));
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count();
        worst_seek_us = (std::max)(worst_seek_us, us); total_seek_us += us;
        Emission after{}; after.epoch = epoch;
        // Several hundred packets: past the first chunk boundaries and GOPs after the landing.
        status = Drain(parser, queue, source, file, after, targets[s] == INT64_MAX || s == to_end ? UINT64_MAX : 600);
        landed_video[s] = after.first[0]; landed_audio[s] = after.first[1];
        CHECK(after.first[0] == want_video[s] && after.first[1] == want_audio[s]);
        if (s == to_end) CHECK(status == demux::ParseStatus::Done && after.next[0] == video.Count() && after.next[1] == audio.Count());
    }
    const auto heap = allocation_probe::End();
    CHECK(heap == 0 && arena.Statistics().allocation_count == arena_allocations);
    CHECK(source.Active() <= 1); // At most the read of the next sample.
    CHECK(samples > 3 * 131072); // Three times the former per-track ceiling.
    std::printf("%s: %zu video + %zu audio samples, file %.2f GiB, moov %.1f MiB (built in %.1f s)\n", spec.name, video.Count(),
                audio.Count(), static_cast<double>(file.size) / (1u << 30), static_cast<double>(file.head.size()) / (1u << 20), build_s);
    std::printf("  index %.2f MiB = %.2f B/sample (flat 40 B/sample: %.1f MiB; pool %.0f MiB); scan+validate %.0f ms in %llu steps, longest %.2f ms; %llu reads, largest %u B\n",
                static_cast<double>(index) / (1u << 20), static_cast<double>(index) / static_cast<double>(samples),
                static_cast<double>(samples) * 40 / (1u << 20), static_cast<double>(demux::ParserLimits{}.index_bytes) / (1u << 20), parse_ms,
                static_cast<unsigned long long>(steps), longest_step, static_cast<unsigned long long>(source.requests), source.largest);
    if (spec.full_pass) std::printf("  full pass: %llu packets in %.2f s, each checked\n", static_cast<unsigned long long>(e.packets), pass_s);
    std::printf("  %u seeks: mean %.1f us, worst %.1f us; heap allocations %llu\n", target_count, total_seek_us / target_count, worst_seek_us,
                static_cast<unsigned long long>(heap));
    for (unsigned s = 0; s <= to_end; ++s) // The hour marks and the play-to-end landing.
        std::printf("  seek %.1f s -> video sample %zu (PTS %.3f s, want %zu), audio sample %zu (want %zu)\n", static_cast<double>(targets[s]) / 1e7,
                    landed_video[s], landed_video[s] < video.Count() ? static_cast<double>(video.pts_100ns[landed_video[s]]) / 1e7 : -1.0,
                    want_video[s], landed_audio[s], want_audio[s]);
    CHECK(index < samples * 24ull && index < demux::ParserLimits{}.index_bytes);
}

// A pool too small for the index fails cleanly with LimitExceeded, without heap use or leaked reads.
void Exhausted() {
    const Spec spec{"exhausted", 3600, 90000, 1500, true, true, false, 30, 3000, false};
    const auto file = Build(spec);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(4 * 1024 * 1024));
    SyntheticSource source(file, 64 * 1024);
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, source, {2, 2 * 1024 * 1024, 1}));
    Queue queue; CHECK(arena.Seal());
    allocation_probe::Begin();
    auto status = demux::ParseStatus::Progress;
    for (unsigned step = 0; step < 100000 && status != demux::ParseStatus::Failed && status != demux::ParseStatus::Done; ++step) status = parser.Step(queue);
    CHECK(allocation_probe::End() == 0);
    CHECK(status == demux::ParseStatus::Failed && parser.Diagnostic().error == demux::ParseError::LimitExceeded && source.Active() == 0);
    CHECK(parser.IndexBytes() <= 2 * 1024 * 1024);
}
}

int main() {
    // 3 h 30 min, 60 fps VFR with B-frames, 6 KB mean frames: 4.8 GiB, 64-bit chunk offsets and mdat size.
    LongForm({"3h30m 60fps VFR+B co64", 12630, 90000, 1500, true, true, true, 30, 6000, true});
    // A 2-hour 23.976 fps film with B-frames and 32-bit offsets.
    LongForm({"2h 23.976fps film stco", 7230, 24000, 1001, false, true, false, 12, 2500, true});
    // Worst case for the index: 4 h, 60 fps VFR, per-sample ctts, one video sample per chunk, co64.
    LongForm({"4h 60fps worst-case index", 14403, 90000, 1500, true, true, true, 1, 3000, false});
    Exhausted();
    std::printf("Long-form demux: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
