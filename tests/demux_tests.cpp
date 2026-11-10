#include "media/demux/IsobmffParser.hpp"
#include "media/platform/IAudioSink.hpp"
#include "media/platform/IGpuContext.hpp"
#include "allocation_probe.hpp"
#include "isobmff_fixture.hpp"
#include <cstdio>

using namespace media;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (false)

// A range provider over immutable bytes: deliberately small, overlapping windows
// and delayed completions exercise the same portable contract as IOCP/HTTP ranges.
class MemorySource final : public platform::IBlockSource {
public:
    explicit MemorySource(const fixture::File& file) noexcept : file_(file) {}
    std::uint64_t Size() const noexcept override { return file_.size; }
    std::uint32_t MaxRangeBytes() const noexcept override { return 4096; }
    platform::IoStatus Request(std::uint64_t offset, std::uint32_t bytes, platform::BlockToken& token) noexcept override {
        token = {};
        if (!bytes || offset > Size() || bytes > Size() - offset) return platform::IoStatus::OutOfRange;
        if (bytes > MaxRangeBytes()) return platform::IoStatus::TooLarge;
        for (std::uint32_t i = 0; i < 4; ++i) {
            auto& slot = slots_[i];
            if (slot.active) continue;
            slot.active = true; slot.delayed = true; ++slot.generation;
            token = {i, slot.generation};
            const auto start = offset & ~std::uint64_t{7};
            auto length = static_cast<std::uint32_t>(offset - start) + bytes;
            if (length < 13) length = 13;
            if (length > Size() - start) length = static_cast<std::uint32_t>(Size() - start);
            slot.view = {file_.data.data() + start, start, length, token};
            ++requests;
            return platform::IoStatus::Pending;
        }
        return platform::IoStatus::WouldBlock;
    }
    platform::IoStatus Poll(platform::BlockToken token, platform::BlockView& view) noexcept override {
        if (token.id >= 4 || !slots_[token.id].active || slots_[token.id].generation != token.generation)
            return platform::IoStatus::InvalidToken;
        auto& slot = slots_[token.id];
        if (slot.delayed) { slot.delayed = false; return platform::IoStatus::Pending; }
        view = slot.view;
        if (truncate) view.valid_bytes = 0;
        return platform::IoStatus::Ready;
    }
    bool Release(platform::BlockToken token) noexcept override {
        if (token.id >= 4 || !slots_[token.id].active || slots_[token.id].generation != token.generation) return false;
        slots_[token.id].active = false; return true;
    }
    unsigned Active() const noexcept { unsigned n = 0; for (const auto& slot : slots_) n += slot.active ? 1u : 0u; return n; }
    std::uint32_t requests = 0;
    bool truncate = false;
private:
    struct Slot { platform::BlockView view{}; std::uint32_t generation = 0; bool active = false, delayed = false; };
    const fixture::File& file_;
    Slot slots_[4]{};
};

void ValidFile(demux::Codec codec, bool fragmented, bool wide = false, bool tail = false,
               bool extended_header = false, bool open_ended_mdat = false) {
    fixture::File file;
    if (extended_header) { file.U32(1); file.U32(0x66726565); file.U64(16); }
    if (fragmented) file.Fragmented(codec); else file.Classic(codec, wide, tail);
    if (open_ended_mdat) file.Patch32(static_cast<std::uint32_t>(file.samples[0] - 8), 0);
    core::LinearMemoryArena arena;
    CHECK(arena.Initialize(1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, source, {2, 65536, 7}));
    core::SpscRingBuffer<core::MediaPacket, 1> queue;
    CHECK(arena.Seal());
    const auto allocations = arena.Statistics().allocation_count;
    core::MediaPacket packets[16]{};
    std::uint32_t count = 0, backpressure = 0;
    demux::ParseStatus status = demux::ParseStatus::Progress;
    allocation_probe::Begin();
    for (unsigned tick = 0; tick < 10000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed; ++tick) {
        status = parser.Step(queue);
        if (status == demux::ParseStatus::Backpressure) ++backpressure;
        if (status == demux::ParseStatus::Backpressure || status == demux::ParseStatus::Done) {
            const auto* packet = queue.PeekReadSlot();
            if (!packet) continue;
            CHECK(count < 16);
            if (count == 16) break;
            packets[count] = *packet;
            platform::BlockView view{};
            CHECK(source.Poll({packet->payload_block_id, packet->payload_generation}, view) == platform::IoStatus::Ready);
            CHECK(view.file_offset + packet->payload_offset == file.samples[count]);
            CHECK(view.data + packet->payload_offset == file.data.data() + file.samples[count]); // Identity, not merely equal bytes.
            CHECK(packet->payload_bytes == (fragmented ? 4u : count == 0 ? 4u : count == 1 ? 6u : 5u));
            CHECK(packet->stream_id == 1 && packet->codec_config_id == 1 && packet->playback_epoch == 7);
            CHECK(queue.ReleaseReadSlot());
            CHECK(parser.ReturnPacket(packets[count]));
            CHECK(!parser.ReturnPacket(packets[count]));
            ++count;
        }
    }
    const auto heap_allocations = allocation_probe::End();
    if (status == demux::ParseStatus::Failed) {
        const auto diagnostic = parser.Diagnostic();
        std::printf("Parse error %u at %llu box %08x codec %u fragmented %u\n", static_cast<unsigned>(diagnostic.error),
            static_cast<unsigned long long>(diagnostic.offset), diagnostic.box_type, static_cast<unsigned>(codec), fragmented ? 1u : 0u);
    }
    CHECK(status == demux::ParseStatus::Done && count == 3 && backpressure > 0);
    CHECK(heap_allocations == 0 && arena.Statistics().allocation_count == allocations);
    CHECK(source.Active() == 0 && source.requests > 10);
    CHECK(parser.MetadataReady() && parser.TrackCount() == 1 && parser.Track(1) == nullptr);
    const auto* track = parser.Track(0);
    CHECK(track != nullptr && track->codec == codec && track->configuration.offset == file.config);
    CHECK(track != nullptr && track->parameter_count == (codec == demux::Codec::H264 ? 2u : codec == demux::Codec::Hevc ? 3u : 1u));
    if (count == 3) {
        CHECK(packets[0].dts_100ns < packets[1].dts_100ns && packets[1].dts_100ns < packets[2].dts_100ns);
        if (!fragmented) {
            CHECK(packets[0].pts_100ns == -166666 && packets[1].pts_100ns == 1000333 && packets[2].pts_100ns == 334000);
            CHECK(packets[1].dts_100ns == 333666 && packets[2].dts_100ns == 667333);
            CHECK(packets[0].duration_100ns == 333666 && packets[1].duration_100ns == 333667);
        } else {
            CHECK(packets[0].pts_100ns == 20000000 && packets[1].pts_100ns == 5000000);
            CHECK(packets[2].dts_100ns == 30000000 && packets[2].duration_100ns == 10000000);
        }
        CHECK(packets[0].flags == core::PacketFlags::KeyFrame && packets[1].flags == core::PacketFlags::None &&
              packets[2].flags == core::PacketFlags::KeyFrame);
    }
}

demux::ParseError ParseMalformed(const fixture::File& file, bool require_failure = true) {
    core::LinearMemoryArena arena;
    CHECK(arena.Initialize(1024 * 1024));
    MemorySource source(file);
    demux::ParseError error{};
    {
        demux::IsobmffParser parser;
        CHECK(parser.Initialize(arena, source, {2, 65536, 1}));
        core::SpscRingBuffer<core::MediaPacket, 2> queue;
        CHECK(arena.Seal());
        auto status = demux::ParseStatus::Progress;
        allocation_probe::Begin();
        for (unsigned step = 0; step < 5000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed; ++step) {
            status = parser.Step(queue);
            while (const auto* packet = queue.PeekReadSlot()) {
                CHECK(parser.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot());
            }
        }
        CHECK(allocation_probe::End() == 0);
        CHECK(status == demux::ParseStatus::Done || status == demux::ParseStatus::Failed);
        if (require_failure) CHECK(status == demux::ParseStatus::Failed);
        error = parser.Diagnostic().error;
    }
    CHECK(source.Active() == 0);
    return error;
}

void Malformed() {
    fixture::File original; original.Classic(demux::Codec::H264);
    auto file = original; file.Patch32(0, 7); CHECK(ParseMalformed(file) == demux::ParseError::BadBox);
    file = original; file.Patch32(0, UINT32_MAX); CHECK(ParseMalformed(file) == demux::ParseError::BadBox);
    file = original; file.size -= 1; (void)ParseMalformed(file);
    file = original; file.Patch32(file.chunk_table, 1); CHECK(ParseMalformed(file) == demux::ParseError::InvalidSampleRange);
    file = original; file.Patch32(file.timing_table + 8, 200); CHECK(ParseMalformed(file) == demux::ParseError::InvalidTable);
    file = original; file.Patch32(file.timing_table + 4, UINT32_MAX); CHECK(ParseMalformed(file) == demux::ParseError::LimitExceeded);
    file = original; file.data[file.config] = std::byte{0}; CHECK(ParseMalformed(file) == demux::ParseError::InvalidCodec);
    file = original; file.Patch32(file.sample_entry, 8); CHECK(ParseMalformed(file) == demux::ParseError::InvalidCodec);
    // 'edts' is a trak-level container: inside stbl it is a grammar error.
    file = original; file.Patch32(file.timing_table - 4, 0x65647473); CHECK(ParseMalformed(file) == demux::ParseError::BadBox);
    file = original; file.Patch32(file.timing_table - 4, 0x73747a32); CHECK(ParseMalformed(file) == demux::ParseError::Unsupported); // stz2
    // Deterministic corruption sweep: successful admission is permitted for mutations
    // of informational fields, but every case must terminate without OOB/heap use.
    for (std::uint32_t i = 0; i < original.size; ++i) {
        file = original; file.data[i] ^= std::byte{0xff}; (void)ParseMalformed(file, false);
    }
    for (std::uint32_t length = 0; length < original.size; length += 7) {
        file = original; file.size = length; (void)ParseMalformed(file);
    }
    fixture::File fragment; fragment.Fragmented(demux::Codec::H264);
    for (std::uint32_t i = 0; i < fragment.size; ++i) {
        file = fragment; file.data[i] ^= std::byte{0xff}; (void)ParseMalformed(file, false);
    }
    for (std::uint32_t length = 0; length < fragment.size; length += 7) {
        // Ending at a complete initialization or media segment is legal.
        file = fragment; file.size = length; (void)ParseMalformed(file, false);
    }
    core::LinearMemoryArena arena;
    CHECK(arena.Initialize(1024 * 1024));
    MemorySource broken(original); broken.truncate = true;
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, broken, {1, 65536, 1}));
    core::SpscRingBuffer<core::MediaPacket, 1> queue;
    CHECK(arena.Seal());
    CHECK(parser.Step(queue) == demux::ParseStatus::NeedInput);
    CHECK(parser.Step(queue) == demux::ParseStatus::NeedInput);
    CHECK(parser.Step(queue) == demux::ParseStatus::Failed);
    CHECK(parser.Diagnostic().error == demux::ParseError::Truncated && broken.Active() == 0);
}

// Parses to completion, returning every packet (leases returned immediately).
demux::ParseStatus Collect(const fixture::File& file, core::MediaPacket* packets, unsigned& count, demux::TrackInfo& info,
                           demux::ParseError& error) {
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, source, {2, 65536, 1}));
    core::SpscRingBuffer<core::MediaPacket, 2> queue;
    CHECK(arena.Seal());
    auto status = demux::ParseStatus::Progress; count = 0;
    allocation_probe::Begin();
    for (unsigned step = 0; step < 5000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed; ++step) {
        status = parser.Step(queue);
        while (const auto* packet = queue.PeekReadSlot()) {
            if (count < 16) packets[count++] = *packet;
            CHECK(parser.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot());
        }
    }
    CHECK(allocation_probe::End() == 0);
    if (parser.TrackCount()) info = *parser.Track(0);
    error = parser.Diagnostic().error;
    CHECK(source.Active() == 0);
    return status;
}
void EditList() {
    core::MediaPacket packets[16]{}; unsigned count = 0; demux::TrackInfo info{}; demux::ParseError error{};
    // Ticks at 30000/s: DTS 0,1001,2002; CTS -500,2000,-1000; media_time 1001 shifts both timelines.
    for (const unsigned version : {0u, 1u}) {
        const fixture::Edit edit{version, 1, 1001};
        fixture::File file; file.Classic(demux::Codec::Hevc, false, false, &edit);
        CHECK(Collect(file, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
        CHECK(info.edit_media_time == 1001 && info.max_sample_bytes == 6);
        CHECK(packets[0].dts_100ns == -333666 && packets[1].dts_100ns == 0 && packets[2].dts_100ns == 333666);
        CHECK(packets[0].pts_100ns == -500333 && packets[1].pts_100ns == 666666 && packets[2].pts_100ns == 333);
        CHECK(packets[0].duration_100ns == 333666 && packets[2].duration_100ns == 667334); // Endpoint rounding: 1001000 - 333666.
    }
    // A leading empty edit (2026-11-04: `ffmpeg -itsoffset`, editors, screen recorders; refused at the open before): 250 movie
    // ticks at 1000/s start the track 250 ms (7500 ticks at 30000/s) late, on top of the media_time shift: the shift is
    // 1001 - 7500 = -6499 ticks. DTS 0, 1001, 2002 and PTS -500, 3001, 1002 ticks become 6499, 7500, 8501 and 5999, 9500, 7501.
    for (const unsigned version : {0u, 1u}) {
        const fixture::Edit delayed{version, 1, 1001, 0x10000, false, 250};
        fixture::File file; file.Classic(demux::Codec::Hevc, false, false, &delayed);
        CHECK(Collect(file, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
        CHECK(info.edit_media_time == 1001 && info.edit_delay_100ns == 2500000);
        CHECK(packets[0].dts_100ns == 2166333 && packets[1].dts_100ns == 2500000 && packets[2].dts_100ns == 2833666);
        CHECK(packets[0].pts_100ns == 1999666 && packets[1].pts_100ns == 3166666 && packets[2].pts_100ns == 2500333);
    }
    // A delay alone: the shift is -7500 ticks, every timestamp 250 ms later (PTS -500 + 7500 = 7000 ticks).
    const fixture::Edit delay_only{0, 1, 0, 0x10000, false, 250};
    fixture::File delayed_file; delayed_file.Classic(demux::Codec::Hevc, false, false, &delay_only);
    CHECK(Collect(delayed_file, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
    CHECK(packets[0].dts_100ns == 2500000 && packets[0].pts_100ns == 2333333);
    // QuickTime's layout (2026-11-04: every .mov from an iPhone or ffmpeg was refused as a duplicate handler): the data handler
    // in minf is skipped, an 'alis' self-contained reference is admitted, the samples are the same.
    fixture::File quicktime; quicktime.quicktime = true; quicktime.Classic(demux::Codec::Hevc);
    CHECK(Collect(quicktime, packets, count, info, error) == demux::ParseStatus::Done && count == 3 && info.kind == demux::TrackKind::Video);
    CHECK(packets[0].pts_100ns == -166666 && packets[0].dts_100ns == 0);
    // The last sample of no duration (2026-11-04: mpv's encoder wrote stts (1, 0) and the video track was refused): it lasts as
    // long as the one before (1001 ticks); its DTS is unchanged. A zero delta before the end still refuses.
    fixture::File open_end; open_end.last_delta = 0; open_end.Classic(demux::Codec::Hevc);
    CHECK(Collect(open_end, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
    CHECK(packets[2].dts_100ns == 667333 && packets[2].duration_100ns == packets[1].duration_100ns && packets[2].duration_100ns > 0);
    fixture::File zero_inside; zero_inside.first_delta = 0; zero_inside.Classic(demux::Codec::Hevc);
    CHECK(Collect(zero_inside, packets, count, info, error) == demux::ParseStatus::Failed && error == demux::ParseError::InvalidTable);
    // Without an edit: unchanged timeline (regression).
    fixture::File plain; plain.Classic(demux::Codec::Hevc);
    CHECK(Collect(plain, packets, count, info, error) == demux::ParseStatus::Done && info.edit_media_time == 0);
    CHECK(packets[0].pts_100ns == -166666 && packets[0].dts_100ns == 0);
    // Fragmented, 1000/s: the shift also applies to tfdt-anchored samples.
    const fixture::Edit fragment_edit{0, 1, 500};
    fixture::File fragmented; fragmented.Fragmented(demux::Codec::Hevc, &fragment_edit);
    CHECK(Collect(fragmented, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
    CHECK(packets[0].pts_100ns == 15000000 && packets[1].pts_100ns == 0 && packets[2].dts_100ns == 25000000);
    CHECK(info.max_sample_bytes == 0);
    // Interpretation-changing edits reject explicitly.
    // Two normal edits, an empty edit alone, rate edits, none at all; an empty edit before a rate edit.
    const fixture::Edit rejected[]{{0, 2, 1001}, {0, 1, -1}, {1, 1, -1}, {0, 1, 1001, 0x20000}, {0, 1, 1001, 0x10001}, {0, 0, 0},
                                   {0, 1, 1001, 0x20000, false, 250}, {0, 2, 1001, 0x10000, false, 250}};
    for (const auto& bad : rejected) {
        fixture::File file; file.Classic(demux::Codec::Hevc, false, false, &bad);
        CHECK(Collect(file, packets, count, info, error) == demux::ParseStatus::Failed && error == demux::ParseError::Unsupported);
    }
    const fixture::Edit twice{0, 1, 1001, 0x10000, true};
    fixture::File duplicate; duplicate.Classic(demux::Codec::Hevc, false, false, &twice);
    CHECK(Collect(duplicate, packets, count, info, error) == demux::ParseStatus::Failed && error == demux::ParseError::InvalidTable);
    // Corruption sweep over a file carrying an edit list: every case terminates without OOB or heap use.
    const fixture::Edit sweep_edit{1, 1, 1001};
    fixture::File original; original.Classic(demux::Codec::Hevc, false, false, &sweep_edit);
    for (std::uint32_t i = 0; i < original.size; ++i) {
        auto file = original; file.data[i] ^= std::byte{0xff}; (void)ParseMalformed(file, false);
    }
}
// Random access by table lookup. The classic fixture: PTS -166666 (sync), 1000333, 334000 (sync).
void Seek() {
    fixture::File file; file.Classic(demux::Codec::H264);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(1024 * 1024));
    MemorySource source(file);
    demux::IsobmffParser parser;
    CHECK(parser.Initialize(arena, source, {2, 65536, 1}));
    core::SpscRingBuffer<core::MediaPacket, 1> queue;
    CHECK(arena.Seal());
    allocation_probe::Begin();
    CHECK(!parser.Seekable() && !parser.Seek(0, 2) && !parser.Reposition(0)); // Not before the scan admitted every sample.
    // Drains to Done (or up to `limit` packets, leaving the rest unread), recording PTS and epoch.
    std::int64_t pts[8]{}; std::uint32_t epochs[8]{}; unsigned count = 0;
    auto run = [&](unsigned limit) {
        count = 0; auto status = demux::ParseStatus::Progress;
        for (unsigned step = 0; step < 5000 && status != demux::ParseStatus::Done && status != demux::ParseStatus::Failed && count < limit; ++step) {
            status = parser.Step(queue);
            while (const auto* packet = queue.PeekReadSlot()) {
                if (count < 8) { pts[count] = packet->pts_100ns; epochs[count] = packet->playback_epoch; }
                ++count; CHECK(parser.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot());
            }
        }
        return status;
    };
    CHECK(run(8) == demux::ParseStatus::Done && count == 3 && parser.Seekable());
    // Exactly on the second sync sample: only it remains, in the new epoch.
    CHECK(parser.Seek(334000, 2) && parser.Epoch() == 2);
    CHECK(run(8) == demux::ParseStatus::Done && count == 1 && pts[0] == 334000 && epochs[0] == 2);
    // Just before it: back to the first sync sample, and the non-sync sample between them is emitted again.
    CHECK(parser.Seek(333999, 3));
    CHECK(run(8) == demux::ParseStatus::Done && count == 3 && pts[0] == -166666 && pts[1] == 1000333 && epochs[2] == 3);
    // Before the first sample and past the last: first sync sample, and the last sync sample.
    CHECK(parser.Seek(-10000000, 4) && run(8) == demux::ParseStatus::Done && count == 3);
    CHECK(parser.Seek(INT64_MAX, 5) && run(8) == demux::ParseStatus::Done && count == 1 && pts[0] == 334000);
    // Epochs only move forward.
    CHECK(!parser.Seek(0, 5) && parser.Epoch() == 5);
    // Mid-stream: a published packet of the old epoch is still returnable after the seek; an in-flight read is dropped.
    CHECK(parser.Seek(0, 6));
    auto status = demux::ParseStatus::Progress; core::MediaPacket stale{};
    for (unsigned step = 0; step < 100 && !queue.PeekReadSlot(); ++step) status = parser.Step(queue);
    CHECK(queue.PeekReadSlot() != nullptr && status != demux::ParseStatus::Failed);
    if (const auto* packet = queue.PeekReadSlot()) { stale = *packet; (void)queue.ReleaseReadSlot(); }
    (void)parser.Step(queue); // Starts the next sample's read.
    CHECK(parser.Seek(334000, 7));
    CHECK(stale.playback_epoch == 6 && parser.ReturnPacket(stale) && !parser.ReturnPacket(stale));
    CHECK(run(8) == demux::ParseStatus::Done && count == 1 && epochs[0] == 7);
    // Audio pre-roll applies to audio tracks only: this video track ignores it.
    CHECK(parser.Seek(334000, 8, 2) && run(8) == demux::ParseStatus::Done && count == 1);
    // Device-loss reposition: the same lookup, in the same epoch, also after the end of the stream.
    CHECK(parser.Reposition(333999) && parser.Epoch() == 8);
    CHECK(run(8) == demux::ParseStatus::Done && count == 3 && pts[0] == -166666 && epochs[0] == 8 && epochs[2] == 8);
    CHECK(parser.Reposition(334000) && run(8) == demux::ParseStatus::Done && count == 1 && pts[0] == 334000 && epochs[0] == 8);
    CHECK(parser.Seek(0, 9)); // A later seek still needs a newer epoch.
    CHECK(allocation_probe::End() == 0 && source.Active() == 0);
    {
        // AAC: the same layout; pre-roll steps back whole access units, bounded by the first sample.
        fixture::File audio; audio.Classic(demux::Codec::Aac);
        core::LinearMemoryArena audio_arena; CHECK(audio_arena.Initialize(1024 * 1024));
        MemorySource audio_source(audio);
        demux::IsobmffParser aac;
        CHECK(aac.Initialize(audio_arena, audio_source, {2, 65536, 1}));
        CHECK(audio_arena.Seal());
        auto drain = [&](unsigned& n, std::int64_t& first) {
            n = 0; auto s = demux::ParseStatus::Progress;
            for (unsigned step = 0; step < 5000 && s != demux::ParseStatus::Done && s != demux::ParseStatus::Failed; ++step) {
                s = aac.Step(queue);
                while (const auto* packet = queue.PeekReadSlot()) {
                    if (!n++) first = packet->pts_100ns;
                    CHECK(aac.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot());
                }
            }
            return s;
        };
        unsigned n = 0; std::int64_t first = 0;
        CHECK(drain(n, first) == demux::ParseStatus::Done && n == 3);
        CHECK(aac.Seek(334000, 2, 1) && drain(n, first) == demux::ParseStatus::Done && n == 2 && first == 1000333);
        CHECK(aac.Seek(334000, 3, 5) && drain(n, first) == demux::ParseStatus::Done && n == 3 && first == -166666);
        CHECK(aac.Seek(334000, 4, 0) && drain(n, first) == demux::ParseStatus::Done && n == 1 && first == 334000);
        CHECK(audio_source.Active() == 0);
    }
    // Fragmented input has no complete sample table: not seekable.
    fixture::File fragment; fragment.Fragmented(demux::Codec::H264);
    core::LinearMemoryArena fragment_arena; CHECK(fragment_arena.Initialize(1024 * 1024));
    MemorySource fragment_source(fragment);
    demux::IsobmffParser fragmented;
    CHECK(fragmented.Initialize(fragment_arena, fragment_source, {2, 65536, 1}));
    CHECK(fragment_arena.Seal());
    for (unsigned step = 0; step < 5000; ++step) {
        const auto s = fragmented.Step(queue);
        while (const auto* packet = queue.PeekReadSlot()) { CHECK(fragmented.ReturnPacket(*packet)); CHECK(queue.ReleaseReadSlot()); }
        if (s == demux::ParseStatus::Done || s == demux::ParseStatus::Failed) break;
    }
    CHECK(fragmented.Fragmented() && !fragmented.Seekable() && !fragmented.Seek(0, 2));
}
// Boxes that real muxers add: audio roll-recovery sample groups (admitted, the distance reported), any other
// sample group (rejected), and the sample entry's colour description with HDR metadata (reported).
void RealWorldBoxes() {
    core::MediaPacket packets[16]{}; unsigned count = 0; demux::ParseError error{};
    constexpr std::uint32_t roll = 0x726f6c6c, prol = 0x70726f6c, seig = 0x73656967, rap = 0x72617020;
    for (const auto codec : {demux::Codec::Aac, demux::Codec::Opus})
        for (const auto type : {roll, prol})
            for (const unsigned version : {0u, 1u}) {
                demux::TrackInfo info{};
                fixture::File file; file.group = {type, version, 2, codec == demux::Codec::Aac ? -1 : -4}; file.Classic(codec);
                CHECK(Collect(file, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
                CHECK(info.roll_distance == (codec == demux::Codec::Aac ? -1 : -4));
            }
    {
        demux::TrackInfo info{};
        fixture::File plain; plain.Classic(demux::Codec::Aac);
        CHECK(Collect(plain, packets, count, info, error) == demux::ParseStatus::Done && info.roll_distance == 0 && !info.color.described);
    }
    auto rejected = [&](demux::Codec codec, fixture::File::Group group) {
        demux::TrackInfo info{};
        fixture::File file; file.group = group; file.Classic(codec);
        return Collect(file, packets, count, info, error) == demux::ParseStatus::Failed && error == demux::ParseError::Unsupported;
    };
    CHECK(rejected(demux::Codec::Aac, {seig, 1, 2, -1}));   // Encryption.
    CHECK(rejected(demux::Codec::Aac, {rap, 1, 2, -1}));
    CHECK(rejected(demux::Codec::Aac, {roll, 2, 2, -1}));   // Version 2: default description index.
    CHECK(rejected(demux::Codec::Aac, {roll, 1, 4, -1}));   // Entries that are not one int16.
    CHECK(rejected(demux::Codec::H264, {roll, 1, 2, -1}));  // Video: gradual decoding refresh changes random access.
    CHECK(rejected(demux::Codec::Hevc, {rap, 1, 2, -1}));
    fixture::File grouped; grouped.group = {roll, 1, 2, -1}; grouped.Classic(demux::Codec::Aac);
    for (std::uint32_t i = 0; i < grouped.size; ++i) {
        auto file = grouped; file.data[i] ^= std::byte{0xff}; (void)ParseMalformed(file, false);
    }
    for (const auto codec : {demux::Codec::H264, demux::Codec::Hevc, demux::Codec::Av1}) {
        demux::TrackInfo info{};
        fixture::File file; file.color = true; file.Classic(codec);
        CHECK(Collect(file, packets, count, info, error) == demux::ParseStatus::Done && count == 3);
        CHECK(info.color.described && info.color.primaries == 9 && info.color.transfer == 16 && info.color.matrix == 9 && info.color.full_range);
        CHECK(info.color.range_described && info.pixel_aspect_width == 0 && info.pixel_aspect_height == 0);
        CHECK(info.color.max_cll == 839 && info.color.max_fall == 39 && info.color.mastering_display);
        for (std::uint32_t i = file.sample_entry; i < file.sample_entry + 200 && codec == demux::Codec::H264; ++i) {
            auto broken = file; broken.data[i] ^= std::byte{0xff}; (void)ParseMalformed(broken, false);
        }
    }
    // Pixel aspect ratio (pasp) and the QuickTime colour description (nclc: no range flag).
    for (const auto codec : {demux::Codec::H264, demux::Codec::Hevc}) {
        demux::TrackInfo shaped{};
        fixture::File file; file.pasp_width = 16; file.pasp_height = 11; file.nclc = true; file.Classic(codec);
        CHECK(Collect(file, packets, count, shaped, error) == demux::ParseStatus::Done && count == 3);
        CHECK(shaped.pixel_aspect_width == 16 && shaped.pixel_aspect_height == 11);
        CHECK(shaped.color.described && !shaped.color.range_described && !shaped.color.full_range && shaped.color.primaries == 1 &&
              shaped.color.transfer == 1 && shaped.color.matrix == 1);
        // nclc then nclx: the first description stands.
        fixture::File both; both.nclc = true; both.color = true; both.Classic(codec);
        CHECK(Collect(both, packets, count, shaped, error) == demux::ParseStatus::Done && shaped.color.primaries == 1 && !shaped.color.range_described);
        // A zero term says nothing; large terms are kept as written.
        constexpr std::uint32_t zeros[3][2]{{0, 11}, {16, 0}, {0, 0}};
        for (const auto& pair : zeros) {
            fixture::File zero; zero.pasp_width = pair[0]; zero.pasp_height = pair[1]; zero.pasp_written = true; zero.Classic(codec);
            CHECK(Collect(zero, packets, count, shaped, error) == demux::ParseStatus::Done && shaped.pixel_aspect_width == 0 && shaped.pixel_aspect_height == 0);
        }
        fixture::File large; large.pasp_width = 0xfffffff0u; large.pasp_height = 3; large.Classic(codec);
        CHECK(Collect(large, packets, count, shaped, error) == demux::ParseStatus::Done && shaped.pixel_aspect_width == 0xfffffff0u && shaped.pixel_aspect_height == 3);
        for (std::uint32_t i = file.sample_entry; i < file.sample_entry + 220 && codec == demux::Codec::H264; ++i) {
            auto broken = file; broken.data[i] ^= std::byte{0xff}; (void)ParseMalformed(broken, false);
        }
    }
    demux::TrackInfo info{};
    fixture::File audio; audio.color = true; audio.Classic(demux::Codec::Aac);
    CHECK(Collect(audio, packets, count, info, error) == demux::ParseStatus::Done && !info.color.described);
}
void Timing() {
    std::int64_t out = 0;
    CHECK(demux::To100ns(1001, 30000, out) && out == 333666);
    CHECK(demux::To100ns(-1001, 30000, out) && out == -333666);
    CHECK(!demux::To100ns(1, 0, out));
    CHECK(!demux::To100ns(INT64_MAX, 1, out));
    CHECK(!demux::To100ns(INT64_MIN, 10000000, out));
    CHECK(demux::To100ns(INT64_MAX, 10000000, out) && out == INT64_MAX);
}
}

int main() {
    Timing();
    for (const auto codec : {demux::Codec::H264, demux::Codec::Hevc, demux::Codec::Av1, demux::Codec::Aac, demux::Codec::Opus}) {
        ValidFile(codec, false);
        ValidFile(codec, true);
    }
    ValidFile(demux::Codec::H264, false, true);
    ValidFile(demux::Codec::H264, false, true, true);
    ValidFile(demux::Codec::H264, false, false, false, true);
    ValidFile(demux::Codec::H264, false, false, false, false, true);
    Malformed();
    EditList();
    RealWorldBoxes();
    Seek();
    std::printf("Demux: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
