#pragma once
// Matroska / WebM metadata (2026-11-04, API 16; docs/sdk-reference.md, "What plays"): the EBML header, the segment's Info and
// Tracks, read through a caller's range reader with fixed storage only (no allocation, no exceptions). media_probe uses it to say
// what a .mkv/.webm holds; demux::MatroskaDemuxer (API 17) reads the same elements through it from its block views (ReadInfoAt,
// ReadTracksAt, ReadChaptersAt) and indexes the clusters itself.
#include <cstddef>
#include <cstdint>

namespace media::demux {
// Reads `bytes` at `offset` into `out`; returns the bytes read (fewer at the end of the file).
using RangeReader = std::size_t (*)(void* context, std::uint64_t offset, void* out, std::size_t bytes) noexcept;

enum class MatroskaStatus : std::uint8_t { Ok, NotMatroska, Truncated, Malformed, NoTracks };
enum class MatroskaTrackType : std::uint8_t { Other = 0, Video = 1, Audio = 2, Subtitle = 17 };

struct MatroskaTrack {
    std::uint64_t number = 0, uid = 0;
    MatroskaTrackType type = MatroskaTrackType::Other;
    char codec_id[32]{};            // "V_MPEG4/ISO/AVC", "A_AAC", "S_TEXT/UTF8", ... (cut at 31 bytes).
    char language[8]{};             // Language (ISO 639-2) or LanguageBCP47; "eng" when absent (the specification's default).
    char name[64]{};                // UTF-8, cut at a character boundary.
    std::uint64_t private_offset = 0; // CodecPrivate in the file (0 bytes: none): avcC, hvcC, av1C, AudioSpecificConfig, ...
    std::uint32_t private_bytes = 0;
    std::uint32_t width = 0, height = 0, channels = 1;
    double sample_rate = 8000;      // The specification's default.
    bool is_default = true, enabled = true, interlaced = false;
    bool encoded = false;           // ContentEncodings: compression or encryption on its frames.
    // API 17 (playback): DefaultDuration (ns per frame; 0: not given), CodecDelay and SeekPreRoll (ns, Opus), the display size
    // (DisplayWidth/Height in pixels when DisplayUnit is 0; 0: not given) and the Colour element (ISO/IEC 23091-2 code points,
    // 2 unspecified; Range 1 broadcast, 2 full, 0 unspecified).
    std::uint64_t default_duration_ns = 0, codec_delay_ns = 0, seek_preroll_ns = 0;
    std::uint32_t display_width = 0, display_height = 0;
    std::uint16_t primaries = 2, transfer = 2, matrix = 2, range = 0, max_cll = 0, max_fall = 0;
    bool colour = false, mastering = false;
};
// A chapter of the first edition (API 17): its start and its first display string.
struct MatroskaChapter { std::int64_t start_100ns = 0; char title[56]{}; };

class MatroskaReader final {
public:
    static constexpr unsigned kMaxTracks = 16;
    // Reads the EBML header and the segment's metadata, up to the first Cluster.
    [[nodiscard]] MatroskaStatus Read(RangeReader read, void* context, std::uint64_t file_bytes) noexcept;
    [[nodiscard]] bool WebM() const noexcept { return webm_; }                       // DocType "webm".
    [[nodiscard]] unsigned TrackCount() const noexcept { return track_count_; }
    [[nodiscard]] const MatroskaTrack& Track(unsigned i) const noexcept { return tracks_[i < kMaxTracks ? i : 0]; }
    [[nodiscard]] std::uint64_t TimecodeScale() const noexcept { return timecode_scale_; } // Nanoseconds per tick (default 1,000,000).
    [[nodiscard]] std::int64_t Duration100ns() const noexcept { return duration_100ns_; } // 0: not given.
    [[nodiscard]] std::uint64_t SegmentOffset() const noexcept { return segment_; }         // The segment's data start.
    [[nodiscard]] std::uint64_t FirstCluster() const noexcept { return first_cluster_; }    // 0: none (no cluster in the file).
    [[nodiscard]] std::uint64_t CuesOffset() const noexcept { return cues_; }               // From the SeekHead; 0: unknown.
    // API 17: one element's children read through `read` (the demuxer's view of it), `data`/`size` its payload in the file.
    // ReadInfoAt sets the timecode scale and duration; ReadTracksAt replaces the track list; ReadChaptersAt the chapter list
    // (the first edition's top-level atoms, at most kMaxChapters, hidden ones left out). False: malformed.
    static constexpr unsigned kMaxChapters = 255;
    [[nodiscard]] bool ReadInfoAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size) noexcept;
    [[nodiscard]] bool ReadTracksAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size) noexcept;
    [[nodiscard]] bool ReadChaptersAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size) noexcept;
    // A SeekHead's Cues entry (CuesOffset), `segment` the segment's data start.
    void ReadSeekHeadAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size, std::uint64_t segment) noexcept;
    [[nodiscard]] unsigned ChapterCount() const noexcept { return chapter_count_; }
    [[nodiscard]] const MatroskaChapter& Chapter(unsigned i) const noexcept { return chapters_[i < kMaxChapters ? i : 0]; }

private:
    struct Element { std::uint32_t id = 0; std::uint64_t size = 0, data = 0; bool unknown_size = false; };
    bool Header(std::uint64_t at, std::uint64_t end, Element& e) noexcept;
    bool Unsigned(const Element& e, std::uint64_t& v) noexcept;
    bool Float(const Element& e, double& v) noexcept;
    void Text(const Element& e, char* out, std::size_t capacity) noexcept;
    bool ReadTracks(const Element& tracks) noexcept;
    bool ReadTrack(const Element& entry, MatroskaTrack& t) noexcept;
    bool ReadInfo(const Element& info) noexcept;
    bool ReadColour(const Element& colour, MatroskaTrack& t) noexcept;
    bool ReadEncodings(const Element& encodings, MatroskaTrack& t) noexcept;
    bool ReadAtom(const Element& atom) noexcept;
    void ReadSeekHead(const Element& seek) noexcept;
    RangeReader read_ = nullptr; void* context_ = nullptr; std::uint64_t file_bytes_ = 0;
    MatroskaTrack tracks_[kMaxTracks]{};
    unsigned track_count_ = 0;
    std::uint64_t timecode_scale_ = 1000000, segment_ = 0, first_cluster_ = 0, cues_ = 0;
    double duration_ticks_ = 0; std::int64_t duration_100ns_ = 0;
    bool webm_ = false;
    MatroskaChapter chapters_[kMaxChapters]{};
    unsigned chapter_count_ = 0;
};
}
