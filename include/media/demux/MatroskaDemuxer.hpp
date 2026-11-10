#pragma once
// Matroska / WebM playback (2026-11-05, SDK API 17; docs/matroska.md): the same contract as IsobmffParser (zero-copy packets that
// lease their bytes from the block source, epochs, seeks by table lookup, track switches, sample lookups) for .mkv and .webm.
//
// - Metadata: the EBML header, then the segment's top-level elements up to the first Cluster (Info, Tracks, Chapters, SeekHead;
//   everything else skipped by its size), each read whole into one view and parsed by MatroskaReader. MetadataReady() then.
// - The index: every block of every usable track, in file order (SimpleBlock and BlockGroup, Xiph, EBML and fixed-size lacing:
//   one entry a frame), built by a sequential scan of the clusters in windows of up to kScanWindow bytes, interleaved with the
//   packets: playback starts from the part already indexed and the scan keeps ahead of it. Seekable() once the scan is complete
//   (as a fragmented MP4 before). Clusters of unknown size end at the next top-level element.
// - Cues (2026-11-05): the SeekHead's Cues element (front or end of the file, one ranged read) gives seek points at open. A Seek
//   before the scan is complete starts the scan again at the cue's cluster: instant on a local or remote file. The index then
//   covers only what is read from there (Indexed() false for good): sample lookups (SampleAt, SyncSample, DecodeDistance,
//   Continue) need the complete index. Seekable(): complete, or Cues.
//   Each entry is 32 bytes from the index pool (ParserLimits::index_bytes), plus 4 a sample once the scan is complete (the per-track
//   sample lists and their key frames); exhaustion fails with LimitExceeded.
// - Times: Matroska stores presentation times only, in TimecodeScale ticks (usually 1 ms). Packets carry dts = pts. Video
//   durations: the gap to the next presentation time among the kDurationWindow frames decoded before and after it (a B picture's
//   successor in presentation order can be decoded before it), DefaultDuration only for the last frame: with 1 ms times a default
//   33.333 ms would overlap the next picture and make two pictures cover a seek target. Audio:
//   AAC access units are exactly 1024 samples and Opus packets their TOC's duration: their times are regenerated from the first
//   block of a run (n x 1024 / rate, 100 ns exact, as the MP4 tables give them), resynchronised when a block is kResync away.
// - Cues are not needed (the index is complete); unusable tracks (VP9, Vorbis, compressed or encrypted frames, ...) are refused
//   with a reason in TrackInfo::refusal and their blocks never indexed.
#include "media/demux/IsobmffParser.hpp"
#include "media/demux/MatroskaReader.hpp"

namespace media::demux {
class MatroskaDemuxer final {
public:
    using Publish = bool (*)(void*, const core::MediaPacket&) noexcept;
    static constexpr std::uint32_t kScanWindow = 1u << 20;  // Bytes a scan view covers at most (and at most the source's range).
    static constexpr unsigned kDurationWindow = 16;         // Later frames looked at for a video frame's successor.
    static constexpr std::int64_t kResync = 20000;          // 2 ms: an audio block this far from its regenerated time starts a new run.
    static constexpr unsigned kLead = 64;                   // Entries the scan keeps ahead of the next packet while it runs.
    static constexpr std::uint32_t kMaxHeaderElement = 8u << 20; // Info, Tracks, Chapters larger than this are refused.
    MatroskaDemuxer() noexcept = default;
    ~MatroskaDemuxer() noexcept;
    MatroskaDemuxer(const MatroskaDemuxer&) = delete;
    MatroskaDemuxer& operator=(const MatroskaDemuxer&) = delete;
    [[nodiscard]] bool Initialize(core::LinearMemoryArena&, platform::IBlockSource&, ParserLimits = {}) noexcept;
    [[nodiscard]] ParseStatus StepImpl(void* context, Publish) noexcept;
    [[nodiscard]] const TrackInfo* Track(std::uint32_t index) const noexcept { return index < track_count_ ? &tracks_[index].info : nullptr; }
    [[nodiscard]] std::uint32_t TrackCount() const noexcept { return track_count_; }
    [[nodiscard]] bool MetadataReady() const noexcept { return ready_; }
    [[nodiscard]] ParserDiagnostic Diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] bool ReturnPacket(const core::MediaPacket& packet) noexcept;
    [[nodiscard]] bool Seekable() const noexcept { return (indexed_ || cue_count_) && ready_ && phase_ != Phase::Failed; }
    // The complete index (every frame from the first cluster): sample lookups need it.
    [[nodiscard]] bool Indexed() const noexcept { return indexed_ && phase_ != Phase::Failed; }
    [[nodiscard]] std::uint32_t CuePoints() const noexcept { return cue_count_; }
    static constexpr std::uint32_t kMaxCuePoints = 65536;
    [[nodiscard]] bool Seek(std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll = 0) noexcept;
    [[nodiscard]] bool Reposition(std::int64_t target_100ns, std::uint32_t audio_preroll = 0) noexcept;
    [[nodiscard]] bool Continue(std::uint32_t video, std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll, std::uint32_t& resumed) noexcept;
    [[nodiscard]] bool SeekOthers(std::uint32_t keep, std::int64_t target_100ns, std::uint32_t epoch, std::uint32_t audio_preroll) noexcept;
    [[nodiscard]] std::uint32_t SyncSample(std::uint32_t index, std::int64_t target_100ns) const noexcept;
    [[nodiscard]] std::uint32_t DecodeDistance(std::uint32_t index, std::uint32_t from, std::int64_t target_100ns, std::uint32_t limit) noexcept;
    [[nodiscard]] unsigned ChapterCount() const noexcept { return chapter_count_; }
    [[nodiscard]] const ChapterEntry& ChapterAt(unsigned i) const noexcept { return chapters_[i < chapter_count_ ? i : 0]; }
    [[nodiscard]] std::uint32_t SyncCount(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t SyncAt(std::uint32_t index, std::uint32_t i) const noexcept;
    [[nodiscard]] std::uint32_t Epoch() const noexcept { return limits_.playback_epoch; }
    [[nodiscard]] std::uint64_t IndexBytes() const noexcept { return pool_used_; }
    [[nodiscard]] std::uint32_t SkippedTracks() const noexcept { return skipped_tracks_; }
    [[nodiscard]] bool SetEmission(std::uint32_t index, bool emit) noexcept;
    [[nodiscard]] bool Emitting(std::uint32_t index) const noexcept { return index < track_count_ && tracks_[index].emit; }
    [[nodiscard]] bool SwitchTrack(std::uint32_t from, std::uint32_t to, std::int64_t at_100ns, std::uint32_t audio_preroll = 0) noexcept;
    [[nodiscard]] bool SampleAt(std::uint32_t index, std::uint32_t sample, SampleRecord&) const noexcept;
    [[nodiscard]] bool WebM() const noexcept { return reader_.WebM(); }
    // The scan: entries indexed so far, bytes it read, and whether it is complete.
    [[nodiscard]] std::uint32_t Entries() const noexcept { return entry_count_; }
    [[nodiscard]] std::uint64_t ScanBytes() const noexcept { return scan_read_; }

private:
    static constexpr std::uint32_t kNone = UINT32_MAX;
    // One frame, in file order. `where`: the file offset (low 48 bits), the track (bits 48-55), key (bit 56), given duration (bit 57).
    struct Entry { std::uint64_t where = 0; std::int64_t pts = 0; std::uint32_t bytes = 0, duration = 0, next = kNone, gap = 0; };
    // Entry::gap (video): the least positive distance to another picture's time among the kDurationWindow pictures decoded before and
    // after it (100 ns; 0: none yet), i.e. the gap to the next presentation time. Filled as the pictures are indexed.
    static_assert(sizeof(Entry) == 32);
    struct TrackState {
        TrackInfo info{};
        std::uint64_t number = 0;            // TrackNumber.
        std::uint64_t default_duration = 0;  // 100 ns; 0: not given.
        std::int64_t codec_delay = 0;        // 100 ns (Opus pre-skip): subtracted from every block's time, as the specification says.
        std::uint32_t first = kNone, last = kNone, count = 0; // Entries of the track, linked through Entry::next.
        std::uint32_t cursor = kNone;        // The next entry to publish (kNone: past `after`, or the end).
        std::uint32_t after = kNone;         // While the scan runs: the entry the next one follows (kNone with cursor kNone: the first).
        std::uint32_t* samples = nullptr;    // After the scan: entry of each sample (decode order).
        std::uint32_t* keys = nullptr;       // After the scan: the key samples' numbers.
        std::uint32_t key_count = 0;
        std::uint32_t frame_samples = 0;     // Audio: 1024 for AAC; 0 Opus (per packet) or anything else.
        std::int64_t run_base = 0;           // Audio runs: the run's first time, and the samples since it.
        std::uint64_t run_samples = 0;
        bool run = false, emit = false, usable = false;
        bool key_wait = false;               // After a cue seek, a picture track's frames are left out until a key frame.
        bool waiting = true;                 // The cursor waits for the scan to index the entry after `after`.
        std::uint32_t window = kNone;        // Video: the oldest of the last kDurationWindow + 1 entries (Entry::gap), and their count.
        std::uint32_t window_count = 0;
    };
    enum class Phase : std::uint8_t { Head, Top, Cues, Index, Failed };
    struct CuePoint { std::int64_t time = 0; std::uint64_t cluster = 0; }; // 100 ns; the cluster's element offset in the file.
    struct Window { std::uint64_t offset = 0; std::uint32_t bytes = 0; platform::BlockToken token{}; platform::BlockView view{}; bool requested = false, pending = false; };
    // A view of [offset, offset + bytes) in `w` (nullptr while it is read; wait_ says what to return).
    const std::byte* View(Window& w, std::uint64_t offset, std::uint32_t bytes) noexcept;
    void Drop(Window& w) noexcept;
    bool Fail(ParseError, std::uint64_t at = 0) noexcept;
    ParseStatus Head() noexcept;
    ParseStatus Top() noexcept;
    ParseStatus Index() noexcept;
    ParseStatus Cues() noexcept;                       // Reads the Cues element (cues_offset_), then on to Index.
    void ReadCues(const std::byte* data, std::uint64_t offset, std::uint64_t bytes) noexcept;
    bool CueSeek(std::int64_t target_100ns) noexcept;  // The scan from the cue's cluster (the index restarted there).
    ParseStatus Emit(void* context, Publish) noexcept;
    bool Tracks(const std::byte* data, std::uint64_t offset, std::uint64_t bytes) noexcept;
    void Describe(TrackState& t, const MatroskaTrack& m, const std::byte* tracks, std::uint64_t tracks_offset, std::uint64_t tracks_bytes) noexcept;
    bool CodecRecord(TrackInfo& info, const std::byte* data, std::uint32_t bytes, std::uint64_t offset) noexcept;
    bool Block(const std::byte* data, std::uint64_t offset, std::uint64_t bytes, bool simple, bool group_key, std::int64_t group_duration) noexcept;
    bool Add(TrackState& t, std::uint64_t offset, std::uint32_t bytes, std::int64_t pts, bool key, std::int64_t duration, const std::byte* payload, std::uint32_t available) noexcept;
    bool Complete() noexcept;
    [[nodiscard]] std::int32_t Find(std::uint64_t number) const noexcept;
    [[nodiscard]] std::uint64_t Offset(const Entry& e) const noexcept { return e.where & ((std::uint64_t{1} << 48) - 1); }
    [[nodiscard]] unsigned TrackOf(const Entry& e) const noexcept { return static_cast<unsigned>((e.where >> 48) & 0xff); }
    [[nodiscard]] bool Key(const Entry& e) const noexcept { return (e.where >> 56) & 1; }
    // An entry's duration (100 ns): given, the track's default, or (video) the gap to its presentation successor.
    [[nodiscard]] std::int64_t Duration(std::uint32_t entry) const noexcept;
    [[nodiscard]] bool DurationKnown(std::uint32_t entry) const noexcept; // Its successors are indexed (or the scan is complete).
    [[nodiscard]] std::uint32_t StartSample(const TrackState&, std::int64_t target_100ns) const noexcept;
    void Position(TrackState&, std::int64_t target_100ns, std::uint32_t audio_preroll) noexcept;
    [[nodiscard]] std::uint32_t SampleOf(const TrackState&, std::uint32_t entry) const noexcept; // Its sample number (kNone: none).
    void Resolve(TrackState&) noexcept; // A cursor waiting for the scan: the entry after `after`, once indexed.
    [[nodiscard]] void* Reserve(std::uint64_t bytes) noexcept;
    platform::IBlockSource* source_ = nullptr;
    ParserLimits limits_{};
    std::uint32_t first_epoch_ = 1;
    MatroskaReader reader_{};
    TrackState tracks_[MatroskaReader::kMaxTracks]{};
    std::uint32_t track_count_ = 0, skipped_tracks_ = 0;
    std::byte* pool_ = nullptr;
    std::uint64_t pool_bytes_ = 0, pool_used_ = 0;
    Entry* entries_ = nullptr;
    std::uint32_t entry_count_ = 0, entry_capacity_ = 0;
    Phase phase_ = Phase::Head;
    bool ready_ = false, indexed_ = false, tracks_seen_ = false;
    bool scan_done_ = false, partial_ = false;   // The scan reached the end; the index started at a cue's cluster (never complete).
    CuePoint* cue_points_ = nullptr; std::uint32_t cue_count_ = 0, cue_track_ = 0; std::uint64_t cues_offset_ = 0; bool cues_read_ = false;
    std::uint64_t segment_ = 0, segment_end_ = 0, cursor_ = 0, scan_read_ = 0;
    std::uint64_t cluster_end_ = 0, group_end_ = 0; // 0: not inside one.
    std::uint64_t scan_start_ = 0;                  // The first cluster.
    bool cluster_unknown_ = false;
    std::int64_t cluster_time_ = 0;              // Ticks.
    // A BlockGroup's Block, added when the group ends (its ReferenceBlock and BlockDuration may follow it).
    std::uint64_t group_block_ = 0, group_block_bytes_ = 0; bool group_has_block_ = false, group_referenced_ = false; std::int64_t group_duration_ = -1;
    Window scan_{}, packet_{};
    ParseStatus wait_ = ParseStatus::NeedInput;
    ParserDiagnostic diagnostic_{ParseError::None, 0, 0};
    ChapterEntry chapters_[IsobmffParser::kMaxChapters]{};
    unsigned chapter_count_ = 0;
    std::int32_t emit_track_ = -1;
};
} // namespace media::demux
