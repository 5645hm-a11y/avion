#pragma once

#include "media/core/linear_memory_arena.hpp"
#include "media/core/media_descriptors.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/platform/IBlockSource.hpp"
#include <cstdint>

namespace media::demux {
// Tx3g: 3GPP timed text (ISO/IEC 14496-17), the subtitle format of MP4 files. Pcm: uncompressed audio, ISO/IEC 23003-5
// (`ipcm` integer, `fpcm` float, with their `pcmC`) and QuickTime's `sowt` (16-bit little-endian) and `twos` (big-endian).
// Text: UTF-8 text cues (Matroska S_TEXT/UTF8 and S_TEXT/WEBVTT, API 17); Ssa: SubStation Alpha events (S_TEXT/ASS, S_TEXT/SSA).
enum class Codec : std::uint8_t { Unknown, H264, Hevc, Av1, Aac, Opus, Tx3g, Pcm, Text, Ssa };
// What a track carries, from its handler (hdlr): vide, soun, text/sbtl/subt, anything else (hint, meta, tmcd, ...).
enum class TrackKind : std::uint8_t { Other, Video, Audio, Subtitle };
enum class ParseError : std::uint8_t {
    None, NotInitialized, BadBox, Truncated, Unsupported, LimitExceeded,
    InvalidTable, InvalidCodec, InvalidTimestamp, InvalidSampleRange, IoFailure
};
struct ByteRange { std::uint64_t offset; std::uint64_t bytes; };
struct CodecParameter { ByteRange range; std::uint8_t type; };
// A video sample entry's colour description (colr nclx/nclc: ISO/IEC 23091-2 code points, 2 = unspecified) and HDR
// static metadata (clli, mdcv). The engine resolves it against the bitstream's VUI (video::ResolveColor).
struct ColorInfo {
    std::uint16_t primaries = 2, transfer = 2, matrix = 2;
    std::uint16_t max_cll = 0, max_fall = 0; // clli, cd/m2; 0 without one.
    bool described = false, full_range = false, mastering_display = false;
    bool range_described = false; // `nclx` carries the full-range flag; QuickTime's `nclc` does not.
};
struct TrackInfo {
    std::uint32_t id = 0;
    std::uint32_t timescale = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t config_id = 0;
    std::uint16_t width = 0, height = 0, channels = 0;
    Codec codec = Codec::Unknown;
    std::uint8_t nal_length_bytes = 0;
    ByteRange configuration{}; // Complete codec record payload, immutable source range.
    CodecParameter parameters[64]{}; // SPS/PPS/VPS, AV1 OBUs, AAC ASC, or Opus dOps.
    std::uint32_t parameter_count = 0;
    std::uint32_t max_sample_bytes = 0; // Largest classic-table sample; 0 when unknown (fragmented).
    std::uint64_t edit_media_time = 0;  // The elst's normal edit: its media_time, media timescale ticks (0 without an edit).
    std::int64_t edit_delay_100ns = 0;  // Its leading empty edits: the track's presentation starts this late (0 without).
    std::int64_t duration_100ns = 0;    // Declared mdhd duration after the admitted edit; 0 means unknown/live.
    std::uint32_t sample_count = 0;     // Classic sample tables: samples in the track; 0 when fragmented.
    ColorInfo color{};                  // Video tracks.
    // Video tracks: the sample entry's pixel aspect ratio (`pasp`: hSpacing, vSpacing), the shape of one sample;
    // 0:0 without the box or with a zero term.
    std::uint32_t pixel_aspect_width = 0, pixel_aspect_height = 0;
    // Audio roll recovery (sgpd 'roll'/'prol'): access units before a random-access point that only prime the
    // decoder, as a negative count; 0 without the group. Seek's audio_preroll should cover it.
    std::int16_t roll_distance = 0;
    // Catalogue: what the track is and how the file labels it.
    TrackKind kind = TrackKind::Other;
    std::uint32_t handler = 0;       // hdlr handler_type (fourcc).
    std::uint32_t sample_entry = 0;  // Type of the first stsd entry (fourcc; 0 without one), also when refused.
    char language[4]{};              // mdhd language, ISO 639-2 lower case (QuickTime Macintosh codes mapped); "und": none.
    char name[32]{};                 // hdlr name, UTF-8 cut at a character boundary ("" without one).
    std::uint16_t alternate_group = 0; // tkhd: tracks of one group are alternatives of each other (0: none).
    bool enabled = true;             // tkhd track_enabled: the file's default choice among alternatives.
    // tkhd's transformation matrix, its linear part (a, b, c, d: x' = a x + c y, y' = b x + d y in picture coordinates, y down):
    // how the picture is to be shown, each term -1, 0 or 1. Phones write a quarter turn (90 degrees clockwise: 0, 1, -1, 0),
    // some a mirror. A matrix that is not a quarter turn or mirror of the axes (a free angle, a scale) is kept as the identity
    // and `transform_ignored` is set (2026-10-28; docs/picture.md, "Rotation").
    std::int8_t transform[4]{1, 0, 0, 1};
    bool transform_ignored = false;
    // Not None: the track cannot be used, and why (an unknown codec or handler, an edit that changes the timeline, a
    // broken table, ...). Its samples are never published; the other tracks play as if it were not there.
    ParseError refusal = ParseError::None;
    // Where (2026-11-04, media_probe's reasons): the box being read (its fourcc) and its file offset when the track was refused.
    std::uint32_t refusal_box = 0;
    std::uint64_t refusal_offset = 0;
    // tref 'chap' (QuickTime chapters): the id of the text track whose samples name this track's chapters; 0 without one.
    std::uint32_t chapter_track = 0;
    // Codec::Pcm: bits per sample (8, 16, 24 or 32; 32 or 64 for float), and how they are stored. A sample of the track is
    // any whole number of frames (channels x bits / 8 bytes each), its duration that many frames at sample_rate.
    std::uint8_t pcm_bits = 0;
    bool pcm_float = false, pcm_little_endian = false;
};
// A Nero chapter (moov/udta/chpl): its start on the presentation timeline and its title as the file stores it (UTF-8 in
// practice; cut to fit, NUL-terminated).
struct ChapterEntry { std::int64_t start_100ns = 0; char title[56]{}; };
struct ParserLimits {
    // Tracks catalogued (at most 16). A classic file with more has the rest skipped, never refused.
    std::uint32_t tracks = 8;
    // One pool, reserved at Initialize, holds every track's sample index. Classic files keep their tables in it as
    // the file stores them (runs for stts/ctts/stsc, one entry per chunk and per sync sample, per-sample sizes only
    // when stsz has no fixed size): typically 5-10 bytes per sample, 24 at worst (VFR, per-sample ctts, one sample
    // per chunk). The default holds a 4-hour 60 fps film with its audio several times over. Fragmented files split
    // it into per-track windows for the current fragment's samples instead. Exhaustion fails with LimitExceeded.
    std::uint64_t index_bytes = 48ull * 1024 * 1024;
    std::uint32_t playback_epoch = 1;
    // Matroska (API 17): the index scan reads ahead of the packets whenever it can (a local file: the whole index in a fraction of
    // a second); false keeps it to what playback needs (a network source: no download of the whole file).
    bool scan_ahead = true;
};
enum class ParseStatus : std::uint8_t { Progress, NeedInput, Backpressure, PacketReady, Done, Failed };
struct ParserDiagnostic { ParseError error; std::uint64_t offset; std::uint32_t box_type; };
// One sample of a usable track (both demuxers' SampleAt): its bytes in the file and its presentation.
struct SampleRecord { std::uint64_t offset = 0; std::uint32_t bytes = 0; std::int64_t pts_100ns = 0, duration_100ns = 0; bool key = true; };

// Integer-only checked rescale, toward zero for signed values. INT64_MIN is
// reserved by MediaPacket and is never produced for a known timestamp.
[[nodiscard]] bool To100ns(std::int64_t ticks, std::uint32_t timescale,
                          std::int64_t& result) noexcept;

class IsobmffParser final {
public:
    IsobmffParser() noexcept = default;
    ~IsobmffParser() noexcept;
    IsobmffParser(const IsobmffParser&) = delete;
    IsobmffParser& operator=(const IsobmffParser&) = delete;
    [[nodiscard]] bool Initialize(core::LinearMemoryArena&, platform::IBlockSource&,
                                  ParserLimits = {}) noexcept;
    template <std::size_t Capacity>
    [[nodiscard]] ParseStatus Step(core::SpscRingBuffer<core::MediaPacket, Capacity>& output) noexcept {
        return StepImpl(&output, [](void* context, const core::MediaPacket& packet) noexcept {
            auto& queue = *static_cast<core::SpscRingBuffer<core::MediaPacket, Capacity>*>(context);
            auto* slot = queue.AcquireWriteSlot();
            if (slot == nullptr) return false;
            *slot = packet;
            return queue.CommitWriteSlot();
        });
    }
    // Read only on owner thread, or publish after initialization metadata is ready.
    [[nodiscard]] const TrackInfo* Track(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t TrackCount() const noexcept { return track_count_; }
    [[nodiscard]] bool MetadataReady() const noexcept { return movie_ready_; }
    [[nodiscard]] ParserDiagnostic Diagnostic() const noexcept { return diagnostic_; }
    // Call on source/parser owner after downstream has finished with the bytes. Packets of any earlier
    // playback epoch are accepted: their block leases stay valid across a seek.
    [[nodiscard]] bool ReturnPacket(const core::MediaPacket& packet) noexcept;
    // Classic (non-fragmented) files after metadata: random access by table lookup, no I/O and no allocation.
    [[nodiscard]] bool Seekable() const noexcept { return movie_ready_ && !fragmented_ && (phase_ == Phase::Emit || phase_ == Phase::Done); }
    // Repositions every track: tracks with sync samples start at the last sync sample presenting at or before
    // `target_100ns` (the first one if none does); other tracks start at the sample presenting at or before it.
    // Any in-flight read is released, and later packets carry `epoch`, which must exceed the current one.
    // Audio tracks (AAC/Opus) start `audio_preroll` further access units earlier, when available, so the decoder's
    // overlap state is primed before the target; the caller discards their output.
    [[nodiscard]] bool Seek(std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll = 0) noexcept;
    // Seek within the current epoch (GPU device-loss recovery): the same repositioning, packets keep their epoch.
    // The caller must have returned every packet it holds, as nothing distinguishes them from later ones.
    [[nodiscard]] bool Reposition(std::int64_t target_100ns, std::uint32_t audio_preroll = 0) noexcept;
    // Forward continuation (docs/seeking.md): a Seek in which published track `video` keeps its position, because the
    // decoder already holds everything the target needs. It applies when the target lies ahead in the group of pictures
    // being read: the sync sample a Seek would choose has already been published (so no sync sample lies between the
    // next sample and the target), and no sample published since that sync sample is shown at or after the target
    // (pts + duration > max(target, 0), the 100 ns values its packet carried). Then every other track is positioned as
    // by Seek, later packets carry `epoch`, and `resumed` is the number of samples from that sync sample to the next one
    // (what a Seek would have published again). Otherwise nothing changes and the result is false. No I/O, no
    // allocation; the check walks those samples once (at most one group of pictures).
    [[nodiscard]] bool Continue(std::uint32_t video, std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll,
                                std::uint32_t& resumed) noexcept;
    // A Seek in which track `keep` keeps its position unconditionally (Continue without its checks): the caller already
    // holds that track's pictures for the target (docs/seeking.md, "Queue seek"). False only when not Seekable(), for an
    // epoch that is not newer, or an unknown or unpublished track.
    [[nodiscard]] bool SeekOthers(std::uint32_t keep, std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll) noexcept;
    // Scrub preview (docs/seeking.md): the sample a Seek to `target_100ns` starts track `index` at, before audio pre-roll
    // (its last sync sample presenting at or before the target, the first one if none does; without stss the last sample
    // presenting at or before it). UINT32_MAX: not Seekable(), an unknown or unusable track, or no samples.
    [[nodiscard]] std::uint32_t SyncSample(std::uint32_t index, std::int64_t target_100ns) const noexcept;
    // The samples, in decode order from sample `from`, that come before the one shown at the target (pts <= target <
    // pts + duration): the pictures a decoder starting at `from` decodes before the target's own. At most `limit` + 1
    // samples are read; `limit` + 1 when the target's sample is not among them (or `from` is past the end). No I/O, no
    // allocation; the track's position is unchanged.
    [[nodiscard]] std::uint32_t DecodeDistance(std::uint32_t index, std::uint32_t from, std::int64_t target_100ns, std::uint32_t limit) noexcept;
    // Chapters (docs/chapters.md), final once MetadataReady(): the Nero list of moov/udta/chpl in file order (at most
    // kMaxChapters). Anything malformed inside udta or tref is ignored: it never refuses the file or a track.
    static constexpr unsigned kMaxChapters = 255; // chpl counts them in one byte.
    [[nodiscard]] unsigned ChapterCount() const noexcept { return chapter_count_; }
    [[nodiscard]] const ChapterEntry& ChapterAt(unsigned i) const noexcept { return chapters_[i < chapter_count_ ? i : 0]; }
    // The sync samples of a usable track of a classic file, after the scan: their count, and the sample number of the
    // i-th (0-based). Without stss every sample is one. UINT32_MAX: no such track or sample.
    [[nodiscard]] std::uint32_t SyncCount(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t SyncAt(std::uint32_t index, std::uint32_t i) const noexcept;
    [[nodiscard]] std::uint32_t Epoch() const noexcept { return limits_.playback_epoch; }
    [[nodiscard]] bool Fragmented() const noexcept { return fragmented_; } // Final once MetadataReady().
    [[nodiscard]] std::uint64_t IndexBytes() const noexcept { return pool_used_; } // Pool bytes the sample index holds.
    // Tracks beyond ParserLimits::tracks that were skipped (classic files; fragmented files fail instead).
    [[nodiscard]] std::uint32_t SkippedTracks() const noexcept { return skipped_tracks_; }

    // ---- Several tracks (docs/tracks.md) ----
    // Every trak is catalogued (Track()). A track the engine cannot use is refused (TrackInfo::refusal) and the file
    // plays on with the others; parsing fails only when no track can be used, with the first refusal's error.
    // Which usable tracks Step publishes: video and audio tracks by default, subtitle tracks not (they are read with
    // SampleAt). Owner thread; takes effect from the next sample chosen. A newly published track continues from its
    // own position: sample 0, or where the last Seek or Reposition put it (every usable track is positioned by them).
    [[nodiscard]] bool SetEmission(std::uint32_t index, bool emit) noexcept;
    [[nodiscard]] bool Emitting(std::uint32_t index) const noexcept;
    // Classic files after the scan (Seekable()): track `from` is no longer published and track `to` is, from its last
    // sample presenting at or before `at_100ns`, `audio_preroll` access units earlier for audio, in the current epoch.
    // The other tracks continue where they are. No I/O, no allocation; an in-flight read is released.
    [[nodiscard]] bool SwitchTrack(std::uint32_t from, std::uint32_t to, std::int64_t at_100ns, std::uint32_t audio_preroll = 0) noexcept;
    // One sample of a usable track of a classic file, after the scan: by table lookup, no I/O.
    using SampleRecord = demux::SampleRecord;
    [[nodiscard]] bool SampleAt(std::uint32_t index, std::uint32_t sample, SampleRecord&) const noexcept;
    [[nodiscard]] static const char* Name(ParseError) noexcept;

private:
    // One sample, derived from the compact tables (stored as such only in a fragment's window).
    struct Sample {
        std::uint64_t offset = 0, dts = 0;
        std::int64_t composition = 0;
        std::uint32_t bytes = 0, duration = 0;
        bool key = true;
    };
    struct TimeRun { std::uint32_t count, value; };              // stts (delta) or ctts (offset) run.
    struct TimeMark { std::uint64_t dts; std::uint32_t sample, unused; }; // Start of every kMarkRows-th run.
    struct ChunkRun { std::uint32_t first_chunk, per_chunk, first_sample; }; // stsc run and its first sample.
    // One sample's position in every table: emission advances it in O(1), Seek derives it by binary search.
    struct Cursor {
        std::uint64_t dts = 0, offset = 0;
        std::uint32_t sample = 0, timing = 0, timing_left = 0, composition = 0, composition_left = 0;
        std::uint32_t mapping = 0, chunk = 0, chunk_left = 0, sync = 0;
    };
    struct TrackState {
        TrackInfo info{};
        // Classic tables, in the index pool. `sizes` is null when stsz declares one fixed size; one of the chunk
        // arrays is set (stco or co64). Marks bound a random-access lookup to kMarkRows runs.
        std::uint32_t* sizes = nullptr;
        std::uint32_t* chunks32 = nullptr;
        std::uint64_t* chunks64 = nullptr;
        ChunkRun* mapping = nullptr;
        TimeRun* timing = nullptr;
        TimeMark* timing_marks = nullptr;
        TimeRun* composition = nullptr;
        TimeMark* composition_marks = nullptr;
        std::uint32_t* sync = nullptr;
        Sample* fragment = nullptr; // Fragmented files: the current fragment's samples, also in the pool.
        Cursor cursor{};            // Next sample to validate, then to emit.
        std::uint32_t sample_count = 0, fixed_size = 0, fragment_capacity = 0;
        std::uint32_t timing_count = 0, composition_count = 0, mapping_count = 0;
        std::uint32_t chunk_count = 0, sync_count = 0, seen = 0, handler = 0, mdat = 0;
        std::uint32_t default_duration = 0, default_size = 0, default_flags = 0;
        std::uint64_t next_dts = 0;
        std::uint64_t edit = 0; // Subtracted from DTS and PTS: presentation starts at media_time (two's complement: negative after a delay).
        std::uint64_t edit_delay = 0; // Leading empty edits, movie timescale: the track starts this late (resolved into `edit` by FinishMovie).
        bool has_trex = false, fragment_seen = false, composition_signed = false;
        bool emit = false;               // Step publishes this track's samples.
        ParserDiagnostic refused{ParseError::None, 0, 0}; // Where and why the track was refused.
    };
    // A sample table box read in windows of at most kTableWindow bytes, so its size is bounded by the pool alone.
    struct TableRead { std::uint64_t position = 0; std::uint32_t stride = 0, count = 0, done = 0; bool active = false; };
    struct Container { std::uint64_t end; std::uint32_t type; std::int32_t track; };
    struct Trex { std::uint32_t id, description, duration, bytes, flags; };
    enum class Phase : std::uint8_t { Scan, Validate, Emit, Done, Failed };
    using Publish = bool (*)(void*, const core::MediaPacket&) noexcept;
    [[nodiscard]] ParseStatus StepImpl(void*, Publish) noexcept;
    [[nodiscard]] ParseStatus Scan() noexcept;
    [[nodiscard]] ParseStatus Table() noexcept;
    [[nodiscard]] bool TableHeader(TrackState&, const std::byte*, std::uint64_t) noexcept;
    [[nodiscard]] bool TableEntries(TrackState&, const std::byte*, std::uint32_t) noexcept;
    [[nodiscard]] ParseStatus Validate() noexcept;
    [[nodiscard]] ParseStatus Emit(void*, Publish) noexcept;
    [[nodiscard]] void* Reserve(std::uint64_t bytes) noexcept;
    [[nodiscard]] Sample Current(const TrackState&) const noexcept;
    void Advance(TrackState&) const noexcept;
    void Locate(const TrackState&, std::uint32_t sample, Cursor&, bool offset) const noexcept;
    [[nodiscard]] bool Relocate(std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll) noexcept;
    void Position(TrackState&, std::int64_t target_100ns, std::uint32_t audio_preroll) const noexcept;
    // The sample a Seek starts the track at, before audio pre-roll: its last sync sample presenting at or before the
    // target (the first one if none does), or without stss its last sample presenting at or before it.
    [[nodiscard]] std::uint32_t StartSample(const TrackState&, std::int64_t target_100ns) const noexcept;
    void Start(TrackState&) const noexcept;
    [[nodiscard]] bool Covered(TrackState&, const Sample&) const noexcept;
    [[nodiscard]] const std::byte* View(std::uint64_t, std::uint32_t) noexcept;
    void DropView() noexcept;
    [[nodiscard]] bool Fail(ParseError) noexcept;
    // A problem confined to one track: the track is refused and parsing goes on (false: stop reading this box).
    [[nodiscard]] bool Refuse(TrackState&, ParseError) noexcept;
    [[nodiscard]] bool Refuse(ParseError) noexcept; // The current track; Fail outside a trak.
    [[nodiscard]] bool Usable(const TrackState& t) const noexcept { return t.refused.error == ParseError::None; }
    [[nodiscard]] bool AnyUsable() noexcept; // False (and Failed, with the first refusal as diagnostic) when none is.
    [[nodiscard]] bool Leaf(std::uint32_t, const std::byte*, std::uint32_t, std::uint64_t) noexcept;
    [[nodiscard]] bool Description(TrackState&, const std::byte*, std::uint32_t, std::uint64_t) noexcept;
    [[nodiscard]] bool CodecConfig(TrackInfo&, std::uint32_t, const std::byte*,
                                   std::uint32_t, std::uint64_t) noexcept;
    [[nodiscard]] bool FinishMovie() noexcept;
    void ChapterBoxes(std::uint32_t type, const std::byte*, std::uint32_t bytes) noexcept; // udta, tref: lenient.
    [[nodiscard]] bool FinishTrack(TrackState&) noexcept;
    [[nodiscard]] bool SamplesCovered() const noexcept;
    [[nodiscard]] bool PrepareEmit() noexcept;
    [[nodiscard]] std::int32_t FindTrack(std::uint32_t) const noexcept;
    [[nodiscard]] std::int64_t Presentation(const TrackState&, std::uint32_t sample) const noexcept;
    std::uint32_t first_epoch_ = 1;
    platform::IBlockSource* source_ = nullptr;
    TrackState* tracks_ = nullptr;
    std::byte* pool_ = nullptr;
    std::uint64_t pool_bytes_ = 0, pool_used_ = 0;
    TableRead table_{};
    std::uint32_t validate_track_ = 0;
    ParserLimits limits_{};
    Container stack_[16]{};
    Trex trex_[16]{};
    ByteRange mdats_[64]{};
    std::uint32_t depth_ = 0, track_count_ = 0, trex_count_ = 0, mdat_count_ = 0, skipped_tracks_ = 0;
    std::uint64_t cursor_ = 0, box_end_ = 0, box_payload_ = 0, moof_offset_ = 0;
    std::uint32_t box_type_ = 0, movie_timescale_ = 0, last_sequence_ = 0;
    std::int32_t current_track_ = -1, emit_track_ = -1;
    platform::BlockToken token_{};
    platform::BlockView view_{};
    std::uint64_t requested_offset_ = 0;
    std::uint32_t requested_bytes_ = 0;
    bool requested_ = false, pending_ = false;
    bool box_loaded_ = false;
    bool movie_ready_ = false, fragmented_ = false, fragment_pending_ = false;
    bool ftyp_seen_ = false, moov_seen_ = false, mvhd_seen_ = false, mfhd_seen_ = false;
    bool sequence_seen_ = false, fragment_has_traf_ = false;
    bool tfhd_seen_ = false, tfdt_seen_ = false, trun_seen_ = false;
    std::uint64_t fragment_base_ = 0, run_end_ = 0, fragment_dts_ = 0;
    std::uint32_t fragment_duration_ = 0, fragment_size_ = 0, fragment_flags_ = 0;
    Phase phase_ = Phase::Scan;
    ParseStatus wait_status_ = ParseStatus::NeedInput;
    ParserDiagnostic diagnostic_{ParseError::None, 0, 0};
    ChapterEntry chapters_[kMaxChapters]{};
    unsigned chapter_count_ = 0;
};
} // namespace media::demux
