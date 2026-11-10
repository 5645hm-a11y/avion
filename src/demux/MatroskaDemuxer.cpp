#include "media/demux/MatroskaDemuxer.hpp"

#include <algorithm>
#include <cstring>

namespace media::demux {
namespace {
constexpr std::uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kInfo = 0x1549A966, kTracks = 0x1654AE6B,
                        kChapters = 0x1043A770, kCluster = 0x1F43B675, kCues = 0x1C53BB6B, kTags = 0x1254C367, kAttachments = 0x1941A469,
                        kSeekHead = 0x114D9B74, kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1,
                        kBlockDuration = 0x9B, kReferenceBlock = 0xFB;
constexpr std::uint32_t kBlockHead = 8192; // A block's header and lacing are read from at most this many of its first bytes.
constexpr unsigned kElementsPerStep = 4096;

bool TopLevel(std::uint32_t id) noexcept {
    return id == kCluster || id == kCues || id == kTags || id == kAttachments || id == kChapters || id == kSeekHead || id == kInfo || id == kTracks;
}
// An element header in memory: the id (its marker kept, 1-4 bytes) and the size (1-8 bytes; all ones: unknown).
bool Header(const std::byte* p, std::uint64_t avail, std::uint32_t& id, std::uint64_t& size, unsigned& length, bool& unknown) noexcept {
    if (avail < 2) return false;
    const auto b0 = std::to_integer<unsigned>(p[0]);
    unsigned n = 1; while (n <= 4 && !(b0 & (0x80u >> (n - 1)))) ++n;
    if (n > 4 || avail < n + 1) return false;
    id = 0; for (unsigned i = 0; i < n; ++i) id = (id << 8) | std::to_integer<unsigned>(p[i]);
    const auto s0 = std::to_integer<unsigned>(p[n]);
    unsigned m = 1; while (m <= 8 && !(s0 & (0x80u >> (m - 1)))) ++m;
    if (m > 8 || avail < n + m) return false;
    size = s0 & (0xffu >> m); bool ones = size == (0xffu >> m);
    for (unsigned i = 1; i < m; ++i) { const auto b = std::to_integer<unsigned>(p[n + i]); size = (size << 8) | b; ones = ones && b == 0xff; }
    unknown = ones; length = n + m;
    return true;
}
// A variable-size integer (a block's track number, EBML lacing sizes): its value with the marker removed.
bool Vint(const std::byte* p, std::uint64_t avail, std::uint64_t& value, unsigned& length) noexcept {
    if (!avail) return false;
    const auto b0 = std::to_integer<unsigned>(p[0]);
    unsigned n = 1; while (n <= 8 && !(b0 & (0x80u >> (n - 1)))) ++n;
    if (n > 8 || avail < n) return false;
    value = b0 & (0xffu >> n);
    for (unsigned i = 1; i < n; ++i) value = (value << 8) | std::to_integer<unsigned>(p[i]);
    length = n;
    return true;
}
std::uint64_t Unsigned(const std::byte* p, std::uint64_t bytes) noexcept {
    std::uint64_t v = 0; for (std::uint64_t i = 0; i < bytes && i < 8; ++i) v = (v << 8) | std::to_integer<unsigned>(p[i]);
    return v;
}
std::uint16_t U16(const std::byte* p) noexcept { return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0]) << 8 | std::to_integer<unsigned>(p[1])); }
// A view as MatroskaReader's range reader.
struct Memory { const std::byte* data; std::uint64_t base, bytes; };
std::size_t ReadMemory(void* context, std::uint64_t offset, void* out, std::size_t bytes) noexcept {
    const auto& m = *static_cast<const Memory*>(context);
    if (offset < m.base || offset >= m.base + m.bytes) return 0;
    const auto n = static_cast<std::size_t>((std::min)(std::uint64_t{bytes}, m.base + m.bytes - offset));
    std::memcpy(out, m.data + (offset - m.base), n);
    return n;
}
// Opus (RFC 6716, 3.1): the samples at 48 kHz of a packet, from its TOC byte (and the frame count byte of code 3).
unsigned OpusSamples(const std::byte* p, std::uint32_t available) noexcept {
    if (!available) return 960;
    const auto toc = std::to_integer<unsigned>(p[0]), config = toc >> 3;
    static constexpr unsigned silk[4]{480, 960, 1920, 2880}, hybrid[2]{480, 960}, celt[4]{120, 240, 480, 960};
    const unsigned frame = config < 12 ? silk[config & 3] : config < 16 ? hybrid[config & 1] : celt[config & 3];
    const unsigned code = toc & 3;
    const unsigned frames = code == 0 ? 1 : code < 3 ? 2 : available > 1 ? (std::to_integer<unsigned>(p[1]) & 63) : 1;
    return frame * (frames ? frames : 1);
}
bool Prefix(const char* text, const char* prefix) noexcept { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; }
}

MatroskaDemuxer::~MatroskaDemuxer() noexcept { Drop(scan_); Drop(packet_); }

bool MatroskaDemuxer::Initialize(core::LinearMemoryArena& arena, platform::IBlockSource& source, ParserLimits limits) noexcept {
    if (source_ != nullptr || limits.tracks == 0 || limits.tracks > MatroskaReader::kMaxTracks || limits.index_bytes < 4096 || arena.IsSealed()) return false;
    pool_ = static_cast<std::byte*>(arena.Allocate(static_cast<std::size_t>(limits.index_bytes), 64));
    if (!pool_) return false;
    pool_bytes_ = limits.index_bytes; pool_used_ = 0;
    // Entries from the start; once the scan is complete, the per-track sample and key lists after them (4 + 4 bytes an entry at most).
    entry_capacity_ = static_cast<std::uint32_t>((std::min)(pool_bytes_ / (sizeof(Entry) + 8), std::uint64_t{UINT32_MAX - 1}));
    entries_ = reinterpret_cast<Entry*>(pool_);
    cue_points_ = static_cast<CuePoint*>(arena.Allocate(sizeof(CuePoint) * kMaxCuePoints, alignof(CuePoint)));
    if (!cue_points_) return false;
    limits_ = limits; first_epoch_ = limits.playback_epoch; source_ = &source;
    return true;
}

void* MatroskaDemuxer::Reserve(std::uint64_t bytes) noexcept {
    const auto start = (pool_used_ + 7) & ~std::uint64_t{7};
    if (start > pool_bytes_ || bytes > pool_bytes_ - start) return nullptr;
    pool_used_ = start + bytes;
    return pool_ + start;
}

void MatroskaDemuxer::Drop(Window& w) noexcept {
    if (w.requested && source_) (void)source_->Release(w.token);
    w = {};
}

bool MatroskaDemuxer::Fail(ParseError error, std::uint64_t at) noexcept {
    diagnostic_ = {error, at ? at : cursor_, 0};
    phase_ = Phase::Failed;
    Drop(scan_); Drop(packet_);
    return false;
}

const std::byte* MatroskaDemuxer::View(Window& w, std::uint64_t offset, std::uint32_t bytes) noexcept {
    const auto size = source_->Size();
    if (offset > size || bytes > size - offset) { (void)Fail(ParseError::Truncated, offset); return nullptr; }
    if (w.pending) {
        const auto status = source_->Poll(w.token, w.view);
        if (status == platform::IoStatus::Pending) { wait_ = ParseStatus::NeedInput; return nullptr; }
        if (status != platform::IoStatus::Ready || w.view.data == nullptr || w.view.file_offset > w.offset ||
            w.offset - w.view.file_offset > w.view.valid_bytes || w.bytes > w.view.valid_bytes - (w.offset - w.view.file_offset)) {
            (void)Fail(status == platform::IoStatus::Ready ? ParseError::Truncated : ParseError::IoFailure, offset); return nullptr;
        }
        w.pending = false;
    }
    if (w.requested && offset >= w.view.file_offset) {
        const auto within = offset - w.view.file_offset;
        if (within <= w.view.valid_bytes && bytes <= w.view.valid_bytes - within) return w.view.data + static_cast<std::size_t>(within);
    }
    Drop(w);
    // The scan reads ahead in large windows; a packet's read is exactly its bytes.
    std::uint32_t ask = bytes;
    if (&w == &scan_) {
        // A network source (scan_ahead off): 64 KiB windows, so that a seek on the Cues reads little more than what plays.
        const auto window = (std::min)(limits_.scan_ahead ? kScanWindow : 65536u, source_->MaxRangeBytes());
        ask = static_cast<std::uint32_t>((std::max)(std::uint64_t{bytes}, (std::min)(std::uint64_t{window}, size - offset)));
    }
    const auto status = source_->Request(offset, ask, w.token);
    if (status == platform::IoStatus::WouldBlock) { wait_ = ParseStatus::Backpressure; return nullptr; }
    if (status != platform::IoStatus::Pending) {
        (void)Fail(status == platform::IoStatus::TooLarge ? ParseError::LimitExceeded : ParseError::IoFailure, offset); return nullptr;
    }
    w.requested = w.pending = true; w.offset = offset; w.bytes = ask;
    if (&w == &scan_) scan_read_ += ask;
    wait_ = ParseStatus::NeedInput;
    return nullptr;
}

ParseStatus MatroskaDemuxer::Head() noexcept {
    const auto size = source_->Size();
    const auto* p = View(scan_, 0, static_cast<std::uint32_t>((std::min)(size, std::uint64_t{4096})));
    if (!p) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
    const auto avail = (std::min)(size, std::uint64_t{4096});
    std::uint32_t id = 0; std::uint64_t bytes = 0; unsigned length = 0; bool unknown = false;
    if (!Header(p, avail, id, bytes, length, unknown) || id != kEbml || unknown || length + bytes > avail) { (void)Fail(ParseError::BadBox, 0); return ParseStatus::Failed; }
    bool doc = false;
    for (std::uint64_t at = length; at < length + bytes;) {
        std::uint32_t cid = 0; std::uint64_t csize = 0; unsigned clength = 0; bool cunknown = false;
        if (!Header(p + at, length + bytes - at, cid, csize, clength, cunknown) || cunknown || at + clength + csize > length + bytes) { (void)Fail(ParseError::BadBox, at); return ParseStatus::Failed; }
        if (cid == kDocType) {
            char type[16]{}; std::memcpy(type, p + at + clength, static_cast<std::size_t>((std::min)(csize, std::uint64_t{15})));
            doc = std::strcmp(type, "matroska") == 0 || std::strcmp(type, "webm") == 0;
        }
        at += clength + csize;
    }
    if (!doc) { (void)Fail(ParseError::Unsupported, 0); return ParseStatus::Failed; }
    const auto at = length + bytes;
    if (!Header(p + at, avail - at, id, bytes, length, unknown) || id != kSegment) { (void)Fail(ParseError::BadBox, at); return ParseStatus::Failed; }
    segment_ = at + length;
    segment_end_ = unknown || bytes > size - segment_ ? size : segment_ + bytes; // A truncated file: what is there.
    cursor_ = segment_; phase_ = Phase::Top;
    return ParseStatus::Progress;
}

ParseStatus MatroskaDemuxer::Top() noexcept {
    for (unsigned n = 0; n < 256; ++n) {
        if (cursor_ >= segment_end_) {
            // No cluster: a file of metadata only.
            if (!tracks_seen_) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
            ready_ = true; phase_ = Phase::Index; return Complete() ? ParseStatus::Progress : ParseStatus::Failed;
        }
        const auto avail = static_cast<std::uint32_t>((std::min)(segment_end_ - cursor_, std::uint64_t{12}));
        const auto* p = View(scan_, cursor_, avail);
        if (!p) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
        std::uint32_t id = 0; std::uint64_t size = 0; unsigned length = 0; bool unknown = false;
        if (!Header(p, avail, id, size, length, unknown)) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
        const auto data = cursor_ + length;
        if (id == kCluster) {
            if (!tracks_seen_) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
            ready_ = true; scan_start_ = cursor_; phase_ = cues_offset_ && !cues_read_ ? Phase::Cues : Phase::Index;
            return ParseStatus::Progress;
        }
        if (unknown || size > segment_end_ - data) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
        if (id == kSeekHead && size <= 65536 && !cues_offset_) { // Where the Cues are (API 17).
            const auto* body = View(scan_, data, static_cast<std::uint32_t>(size));
            if (!body) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
            Memory memory{body, data, size};
            reader_.ReadSeekHeadAt(ReadMemory, &memory, data, size, segment_);
            if (reader_.CuesOffset() > segment_ && reader_.CuesOffset() < segment_end_) cues_offset_ = reader_.CuesOffset();
        } else if (id == kCues && !cues_read_ && size <= source_->MaxRangeBytes()) { // Cues before the clusters (mkvmerge's choice).
            const auto* body = View(scan_, data, static_cast<std::uint32_t>(size));
            if (!body) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
            ReadCues(body, data, size);
        } else if (id == kInfo || id == kTracks || id == kChapters) {
            if (size > kMaxHeaderElement || size > source_->MaxRangeBytes()) { (void)Fail(ParseError::LimitExceeded); return ParseStatus::Failed; }
            const auto* body = View(scan_, data, static_cast<std::uint32_t>(size));
            if (!body) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
            Memory memory{body, data, size};
            if (id == kInfo) {
                if (!reader_.ReadInfoAt(ReadMemory, &memory, data, size)) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
                for (unsigned t = 0; t < track_count_; ++t) if (!tracks_[t].info.duration_100ns) tracks_[t].info.duration_100ns = reader_.Duration100ns();
            } else if (id == kTracks) {
                if (tracks_seen_) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
                if (!Tracks(body, data, size)) return ParseStatus::Failed;
            } else if (!chapter_count_ && reader_.ReadChaptersAt(ReadMemory, &memory, data, size)) { // Malformed chapters are ignored.
                for (unsigned c = 0; c < reader_.ChapterCount() && c < IsobmffParser::kMaxChapters; ++c) {
                    chapters_[c].start_100ns = reader_.Chapter(c).start_100ns;
                    static_assert(sizeof(chapters_[c].title) == sizeof(reader_.Chapter(c).title));
                    std::memcpy(chapters_[c].title, reader_.Chapter(c).title, sizeof(chapters_[c].title));
                    chapter_count_ = c + 1;
                }
            }
        }
        cursor_ = data + size;
    }
    return ParseStatus::Progress;
}

bool MatroskaDemuxer::Tracks(const std::byte* data, std::uint64_t offset, std::uint64_t bytes) noexcept {
    Memory memory{data, offset, bytes};
    if (!reader_.ReadTracksAt(ReadMemory, &memory, offset, bytes)) return Fail(ParseError::BadBox, offset);
    tracks_seen_ = true;
    bool any = false;
    for (unsigned i = 0; i < reader_.TrackCount(); ++i) {
        if (track_count_ == limits_.tracks) { ++skipped_tracks_; continue; }
        auto& t = tracks_[track_count_];
        t = {};
        t.info.config_id = track_count_ + 1;
        Describe(t, reader_.Track(i), data, offset, bytes);
        any = any || t.usable;
        ++track_count_;
    }
    if (!any) {
        // Nothing to play: the first refusal says why, as IsobmffParser does.
        ParseError why = ParseError::Unsupported;
        for (unsigned t = 0; t < track_count_; ++t) if (tracks_[t].info.refusal != ParseError::None) { why = tracks_[t].info.refusal; break; }
        return Fail(why, offset);
    }
    return true;
}

void MatroskaDemuxer::Describe(TrackState& t, const MatroskaTrack& m, const std::byte* tracks, std::uint64_t tracks_offset, std::uint64_t tracks_bytes) noexcept {
    auto& info = t.info;
    t.number = m.number;
    info.id = m.number <= UINT32_MAX ? static_cast<std::uint32_t>(m.number) : 0;
    info.timescale = 10000000;
    info.kind = m.type == MatroskaTrackType::Video ? TrackKind::Video : m.type == MatroskaTrackType::Audio ? TrackKind::Audio :
                m.type == MatroskaTrackType::Subtitle ? TrackKind::Subtitle : TrackKind::Other;
    // Language: ISO 639-2 as written, or a BCP 47 tag's language subtag.
    unsigned k = 0;
    for (; k < 3 && m.language[k] && m.language[k] != '-'; ++k) info.language[k] = static_cast<char>(m.language[k] >= 'A' && m.language[k] <= 'Z' ? m.language[k] + 32 : m.language[k]);
    info.language[k] = 0;
    if (!k || std::strcmp(info.language, "und") == 0) std::memcpy(info.language, "und", 4);
    // The name, cut at a character boundary.
    unsigned n = 0;
    for (; m.name[n] && n + 1 < sizeof(info.name); ++n) info.name[n] = m.name[n];
    while (n && (static_cast<unsigned char>(info.name[n - 1]) & 0xc0) == 0x80) --n;
    if (n && static_cast<unsigned char>(info.name[n - 1]) >= 0xc0 && m.name[n]) --n;
    info.name[n] = 0;
    info.enabled = m.enabled && m.is_default;
    info.duration_100ns = reader_.Duration100ns();
    t.default_duration = m.default_duration_ns / 100;
    t.codec_delay = static_cast<std::int64_t>((std::min)(m.codec_delay_ns, std::uint64_t{1000000000}) / 100);
    const auto refuse = [&](ParseError why) { info.refusal = why; info.refusal_offset = tracks_offset; t.usable = false; };
    if (!info.id || info.kind == TrackKind::Other) { refuse(ParseError::Unsupported); return; }
    const char* id = m.codec_id;
    if (std::strcmp(id, "V_MPEG4/ISO/AVC") == 0) info.codec = Codec::H264;
    else if (std::strcmp(id, "V_MPEGH/ISO/HEVC") == 0) info.codec = Codec::Hevc;
    else if (std::strcmp(id, "V_AV1") == 0) info.codec = Codec::Av1;
    else if (Prefix(id, "A_AAC")) info.codec = Codec::Aac;
    else if (std::strcmp(id, "A_OPUS") == 0) info.codec = Codec::Opus;
    else if (std::strcmp(id, "S_TEXT/UTF8") == 0 || std::strcmp(id, "S_TEXT/WEBVTT") == 0) info.codec = Codec::Text;
    else if (std::strcmp(id, "S_TEXT/ASS") == 0 || std::strcmp(id, "S_TEXT/SSA") == 0 || std::strcmp(id, "S_ASS") == 0 || std::strcmp(id, "S_SSA") == 0) info.codec = Codec::Ssa;
    else { refuse(ParseError::Unsupported); return; } // VP8/VP9, Vorbis, FLAC, AC-3, PGS, ...
    if (m.encoded) { refuse(ParseError::Unsupported); return; } // Header stripping, zlib, encryption: not the codec's frames.
    if (info.kind == TrackKind::Video) {
        if (!m.width || !m.height || m.width > 65535 || m.height > 65535) { refuse(ParseError::InvalidCodec); return; }
        info.width = static_cast<std::uint16_t>(m.width); info.height = static_cast<std::uint16_t>(m.height);
        if (m.display_width && m.display_height && (m.display_width != m.width || m.display_height != m.height)) {
            // The sample's shape: (display width / pixel width) : (display height / pixel height), reduced.
            std::uint64_t a = std::uint64_t{m.display_width} * m.height, b = std::uint64_t{m.display_height} * m.width;
            std::uint64_t x = a, y = b; while (y) { const auto r = x % y; x = y; y = r; }
            if (x) { a /= x; b /= x; }
            if (a <= UINT32_MAX && b <= UINT32_MAX) { info.pixel_aspect_width = static_cast<std::uint32_t>(a); info.pixel_aspect_height = static_cast<std::uint32_t>(b); }
        }
        if (m.colour) {
            info.color.primaries = m.primaries; info.color.transfer = m.transfer; info.color.matrix = m.matrix;
            info.color.described = m.primaries != 2 || m.transfer != 2 || m.matrix != 2;
            info.color.range_described = m.range == 1 || m.range == 2; info.color.full_range = m.range == 2;
            info.color.max_cll = m.max_cll; info.color.max_fall = m.max_fall; info.color.mastering_display = m.mastering;
        }
    } else if (info.kind == TrackKind::Audio) {
        info.channels = static_cast<std::uint16_t>((std::min)(m.channels, 255u));
        info.sample_rate = static_cast<std::uint32_t>(m.sample_rate + 0.5);
        if (!info.channels || !info.sample_rate) { refuse(ParseError::InvalidCodec); return; }
        t.frame_samples = info.codec == Codec::Aac ? 1024 : 0;
    }
    // The codec record, inside the Tracks element just read.
    if (info.codec == Codec::H264 || info.codec == Codec::Hevc || info.codec == Codec::Av1 || info.codec == Codec::Aac || info.codec == Codec::Opus) {
        if (!m.private_bytes || m.private_offset < tracks_offset || m.private_offset + m.private_bytes > tracks_offset + tracks_bytes) { refuse(ParseError::InvalidCodec); return; }
        if (!CodecRecord(info, tracks + (m.private_offset - tracks_offset), m.private_bytes, m.private_offset)) { refuse(ParseError::InvalidCodec); return; }
    }
    t.usable = true;
    t.emit = info.kind == TrackKind::Video || info.kind == TrackKind::Audio; // As IsobmffParser: subtitles are read with SampleAt.
}

bool MatroskaDemuxer::CodecRecord(TrackInfo& info, const std::byte* data, std::uint32_t bytes, std::uint64_t offset) noexcept {
    info.configuration = {offset, bytes};
    const auto parameter = [&](std::uint32_t start, std::uint32_t length, std::uint8_t kind) {
        if (!length || start > bytes || length > bytes - start || info.parameter_count == 64) return false;
        info.parameters[info.parameter_count++] = {{offset + start, length}, kind};
        return true;
    };
    const auto byte = [&](std::uint32_t i) { return std::to_integer<unsigned>(data[i]); };
    switch (info.codec) {
    case Codec::H264: { // avcC, as in MP4 (ISO/IEC 14496-15 5.3.3.1).
        if (bytes < 7 || byte(0) != 1) return false;
        info.nal_length_bytes = static_cast<std::uint8_t>((byte(4) & 3) + 1);
        if (info.nal_length_bytes == 3) return false;
        std::uint32_t at = 6;
        const auto nals = [&](unsigned count, std::uint8_t kind) {
            for (unsigned i = 0; i < count; ++i) {
                if (bytes - at < 2) return false;
                const auto length = U16(data + at); at += 2;
                if (!parameter(at, length, kind)) return false;
                at += length;
            }
            return true;
        };
        if (!nals(byte(5) & 31, 7) || at >= bytes) return false;
        const auto pps = byte(at++);
        return nals(pps, 8); // The chroma/bit-depth extension, if any, is not needed.
    }
    case Codec::Hevc: { // hvcC.
        if (bytes < 23 || byte(0) != 1) return false;
        info.nal_length_bytes = static_cast<std::uint8_t>((byte(21) & 3) + 1);
        if (info.nal_length_bytes == 3) return false;
        std::uint32_t at = 23;
        const unsigned arrays = byte(22);
        for (unsigned a = 0; a < arrays; ++a) {
            if (bytes - at < 3) return false;
            const auto kind = static_cast<std::uint8_t>(byte(at++) & 63);
            const auto count = U16(data + at); at += 2;
            for (unsigned i = 0; i < count; ++i) {
                if (bytes - at < 2) return false;
                const auto length = U16(data + at); at += 2;
                if (!parameter(at, length, kind)) return false;
                at += length;
            }
        }
        return true;
    }
    case Codec::Av1: // av1C: the marker and version, then the configuration OBUs (the sequence header).
        if (bytes < 4 || byte(0) != 0x81) return false;
        return bytes == 4 || parameter(4, bytes - 4, 1);
    case Codec::Aac: // The AudioSpecificConfig itself (MP4 wraps it in esds).
        return bytes >= 2 && bytes <= 64 && parameter(0, bytes, 5);
    case Codec::Opus: // OpusHead (RFC 7845 5.1): MP4's dOps carries the same fields big-endian, without the magic.
        if (bytes < 19 || std::memcmp(data, "OpusHead", 8) != 0 || !byte(9)) return false;
        info.channels = static_cast<std::uint16_t>(byte(9)); info.sample_rate = 48000;
        return parameter(0, bytes, 0);
    default: return true;
    }
}

std::int32_t MatroskaDemuxer::Find(std::uint64_t number) const noexcept {
    for (std::uint32_t i = 0; i < track_count_; ++i) if (tracks_[i].number == number) return static_cast<std::int32_t>(i);
    return -1;
}

bool MatroskaDemuxer::Block(const std::byte* p, std::uint64_t offset, std::uint64_t bytes, bool simple, bool group_key, std::int64_t group_duration) noexcept {
    const auto avail = (std::min)(bytes, std::uint64_t{kBlockHead});
    std::uint64_t number = 0; unsigned length = 0;
    if (!Vint(p, avail, number, length) || avail < length + 3u) return true; // A malformed block is skipped, not fatal.
    const auto track = Find(number);
    if (track < 0 || !tracks_[track].usable) return true;
    auto& t = tracks_[track];
    const auto relative = static_cast<std::int16_t>(U16(p + length));
    const auto flags = std::to_integer<unsigned>(p[length + 2]);
    std::uint32_t at = length + 3;
    const bool key = simple ? (flags & 0x80) != 0 : group_key;
    const auto scale = reader_.TimecodeScale();
    const std::int64_t ticks = cluster_time_ + relative;
    const auto limit = static_cast<std::int64_t>(static_cast<std::uint64_t>(INT64_MAX) / (scale ? scale : 1));
    if (ticks > limit || ticks < -limit) return Fail(ParseError::InvalidTimestamp, offset);
    const std::int64_t pts = ticks * static_cast<std::int64_t>(scale) / 100;
    const std::int64_t duration = group_duration >= 0 ? group_duration * static_cast<std::int64_t>(scale) / 100 : -1;
    // Lacing: the frames' sizes (the last one is what is left).
    std::uint32_t sizes[256]; unsigned count = 1;
    const unsigned lacing = (flags >> 1) & 3;
    if (lacing) {
        if (avail <= at) return true;
        count = std::to_integer<unsigned>(p[at++]) + 1;
        std::uint64_t used = 0;
        if (lacing == 1) { // Xiph.
            for (unsigned i = 0; i + 1 < count; ++i) {
                std::uint32_t size = 0; unsigned b = 255;
                while (b == 255) { if (at >= avail) return true; b = std::to_integer<unsigned>(p[at++]); size += b; }
                sizes[i] = size; used += size;
            }
        } else if (lacing == 3) { // EBML: the first size, then signed differences.
            std::uint64_t first = 0; unsigned n = 0;
            if (!Vint(p + at, avail - at, first, n)) return true;
            at += n; sizes[0] = static_cast<std::uint32_t>(first); used = first;
            std::int64_t previous = static_cast<std::int64_t>(first);
            for (unsigned i = 1; i + 1 < count; ++i) {
                std::uint64_t raw = 0;
                if (!Vint(p + at, avail - at, raw, n)) return true;
                at += n;
                const std::int64_t delta = static_cast<std::int64_t>(raw) - ((std::int64_t{1} << (7 * n - 1)) - 1);
                previous += delta;
                if (previous < 0 || previous > UINT32_MAX) return true;
                sizes[i] = static_cast<std::uint32_t>(previous); used += sizes[i];
            }
            if (count == 1) used = 0;
        } else { // Fixed-size.
            if ((bytes - at) % count) return true;
            for (unsigned i = 0; i + 1 < count; ++i) { sizes[i] = static_cast<std::uint32_t>((bytes - at) / count); used += sizes[i]; }
        }
        if (used >= bytes - at) return true;
        sizes[count - 1] = static_cast<std::uint32_t>(bytes - at - used);
    } else {
        if (bytes - at > UINT32_MAX) return true;
        sizes[0] = static_cast<std::uint32_t>(bytes - at);
    }
    std::uint64_t position = offset + at;
    for (unsigned i = 0; i < count; ++i) {
        const auto within = position - offset;
        const auto available = within < avail ? static_cast<std::uint32_t>(avail - within) : 0u;
        // A later frame of a lace: its time from the frames before it (audio times are regenerated anyway).
        const std::int64_t frame_pts = pts + static_cast<std::int64_t>(i) * static_cast<std::int64_t>(t.default_duration);
        if (sizes[i] && !Add(t, position, sizes[i], frame_pts, key, i == 0 ? duration : -1, within < avail ? p + within : nullptr, available)) return false;
        position += sizes[i];
    }
    return true;
}

bool MatroskaDemuxer::Add(TrackState& t, std::uint64_t offset, std::uint32_t bytes, std::int64_t pts, bool key, std::int64_t duration, const std::byte* payload,
                          std::uint32_t available) noexcept {
    if (entry_count_ == entry_capacity_) return Fail(ParseError::LimitExceeded, offset);
    if (bytes > source_->MaxRangeBytes()) return Fail(ParseError::LimitExceeded, offset); // As IsobmffParser: a sample is one read.
    if (offset >= (std::uint64_t{1} << 48)) return Fail(ParseError::LimitExceeded, offset);
    if (t.key_wait) { if (!key) return true; t.key_wait = false; } // After a cue seek: from the first key frame.
    const auto index = static_cast<unsigned>(&t - tracks_);
    auto& e = entries_[entry_count_];
    e = {};
    // Audio: times regenerated from the run's first block (see the header), exact in 100 ns as the MP4 tables give them.
    if (t.info.kind == TrackKind::Audio && (t.info.codec == Codec::Aac || t.info.codec == Codec::Opus)) {
        const unsigned samples = t.info.codec == Codec::Aac ? t.frame_samples : OpusSamples(payload, payload ? available : 0);
        const auto rate = std::uint64_t{t.info.sample_rate};
        pts -= t.codec_delay;
        if (t.run) {
            const auto expected = t.run_base + static_cast<std::int64_t>(t.run_samples * 10000000 / rate);
            if (pts - expected > kResync || expected - pts > kResync) t.run = false;
        }
        if (!t.run) { t.run = true; t.run_base = pts; t.run_samples = 0; }
        pts = t.run_base + static_cast<std::int64_t>(t.run_samples * 10000000 / rate);
        duration = static_cast<std::int64_t>(std::uint64_t{samples} * 10000000 / rate);
        t.run_samples += samples;
    }
    e.where = offset | (std::uint64_t{index} << 48) | (key ? std::uint64_t{1} << 56 : 0) | (duration >= 0 ? std::uint64_t{1} << 57 : 0);
    e.pts = pts; e.bytes = bytes;
    e.duration = duration >= 0 ? static_cast<std::uint32_t>((std::min)(duration, std::int64_t{UINT32_MAX})) : 0;
    const auto id = entry_count_++;
    pool_used_ = std::uint64_t{entry_count_} * sizeof(Entry);
    if (t.last != kNone) entries_[t.last].next = id; else t.first = id;
    t.last = id; ++t.count;
    if (bytes > t.info.max_sample_bytes) t.info.max_sample_bytes = bytes;
    if (t.info.kind == TrackKind::Video) {
        // The gap to the next presentation time, both ways in decode order: the picture after a B picture in presentation order is
        // often decoded before it (I P B B: the second B's successor is the P).
        if (t.window == kNone) t.window = id;
        for (auto k = t.window; k != id; k = entries_[k].next) {
            auto& w = entries_[k];
            const auto later = e.pts > w.pts ? &w : w.pts > e.pts ? &e : nullptr;
            if (!later) continue; // The same time: no gap.
            const auto distance = static_cast<std::uint32_t>((std::min)(e.pts > w.pts ? e.pts - w.pts : w.pts - e.pts, std::int64_t{UINT32_MAX}));
            if (!later->gap || distance < later->gap) later->gap = distance;
        }
        if (++t.window_count > kDurationWindow + 1) { t.window = entries_[t.window].next; --t.window_count; }
    }
    return true;
}

ParseStatus MatroskaDemuxer::Cues() noexcept {
    // The Cues element the SeekHead names: its header, then the whole element (at most the source's range; larger Cues are not used).
    const auto size = source_->Size();
    const auto at = cues_offset_;
    const auto* p = at + 12 <= size ? View(scan_, at, 12) : nullptr;
    if (!p) {
        if (phase_ == Phase::Failed) return ParseStatus::Failed;
        if (at + 12 > size) { cues_read_ = true; phase_ = Phase::Index; cursor_ = scan_start_; return ParseStatus::Progress; }
        return wait_;
    }
    std::uint32_t id = 0; std::uint64_t bytes = 0; unsigned length = 0; bool unknown = false;
    const auto done = [&] { cues_read_ = true; phase_ = Phase::Index; cursor_ = scan_start_; Drop(scan_); return ParseStatus::Progress; };
    if (!Header(p, 12, id, bytes, length, unknown) || id != kCues || unknown || bytes > size - (at + length) || bytes > source_->MaxRangeBytes()) return done();
    const auto* body = View(scan_, at + length, static_cast<std::uint32_t>(bytes));
    if (!body) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
    ReadCues(body, at + length, bytes);
    return done();
}
void MatroskaDemuxer::ReadCues(const std::byte* data, std::uint64_t offset, std::uint64_t bytes) noexcept {
    // CuePoint (0xBB): CueTime (0xB3, ticks) and CueTrackPositions (0xB7): CueTrack (0xF7), CueClusterPosition (0xF1, from the
    // segment's data). The points of one track are kept: the picture track's (the first usable one), else the first usable track's.
    cues_read_ = true; cue_count_ = 0; cue_track_ = 0;
    for (std::uint32_t t = 0; t < track_count_ && !cue_track_; ++t) if (tracks_[t].usable && tracks_[t].info.kind == TrackKind::Video) cue_track_ = static_cast<std::uint32_t>(tracks_[t].number);
    for (std::uint32_t t = 0; t < track_count_ && !cue_track_; ++t) if (tracks_[t].usable) cue_track_ = static_cast<std::uint32_t>(tracks_[t].number);
    const auto scale = reader_.TimecodeScale();
    std::uint64_t at = 0;
    while (at < bytes && cue_count_ < kMaxCuePoints) {
        std::uint32_t id = 0; std::uint64_t size = 0; unsigned length = 0; bool unknown = false;
        if (!Header(data + at, bytes - at, id, size, length, unknown) || unknown || size > bytes - at - length) break;
        const auto* point = data + at + length;
        if (id == 0xBB) {
            std::uint64_t time = 0, track = 0, cluster = 0; bool have_time = false, have_position = false;
            for (std::uint64_t in = 0; in < size;) {
                std::uint32_t cid = 0; std::uint64_t csize = 0; unsigned clength = 0; bool cunknown = false;
                if (!Header(point + in, size - in, cid, csize, clength, cunknown) || cunknown || csize > size - in - clength) break;
                const auto* body = point + in + clength;
                if (cid == 0xB3 && csize <= 8) { time = Unsigned(body, csize); have_time = true; }
                else if (cid == 0xB7) {
                    std::uint64_t t = 0, c = 0; bool has_c = false;
                    for (std::uint64_t k = 0; k < csize;) {
                        std::uint32_t pid = 0; std::uint64_t psize = 0; unsigned plength = 0; bool punknown = false;
                        if (!Header(body + k, csize - k, pid, psize, plength, punknown) || punknown || psize > csize - k - plength) break;
                        if (pid == 0xF7 && psize <= 8) t = Unsigned(body + k + plength, psize);
                        else if (pid == 0xF1 && psize <= 8) { c = Unsigned(body + k + plength, psize); has_c = true; }
                        k += plength + psize;
                    }
                    if (has_c && t == cue_track_ && !have_position) { track = t; cluster = c; have_position = true; }
                }
                in += clength + csize;
            }
            const auto absolute = segment_ + cluster;
            if (have_time && have_position && track && absolute < segment_end_ && time <= static_cast<std::uint64_t>(INT64_MAX) / (scale ? scale : 1)) {
                const auto t100 = static_cast<std::int64_t>(time * scale / 100);
                if (!cue_count_ || (t100 >= cue_points_[cue_count_ - 1].time && absolute > cue_points_[cue_count_ - 1].cluster))
                    cue_points_[cue_count_++] = {t100, absolute};
            }
        }
        at += length + size;
    }
    (void)offset;
}
bool MatroskaDemuxer::CueSeek(std::int64_t target) noexcept {
    if (!cue_count_) return false;
    std::uint32_t low = 0, high = cue_count_; // The last cue point at or before the target (the first one if none is).
    while (low < high) { const auto middle = low + (high - low) / 2; if (cue_points_[middle].time <= target) low = middle + 1; else high = middle; }
    const auto& point = cue_points_[low ? low - 1 : 0];
    // The index starts again at that cluster: what was indexed is forgotten (its packets' leases stay valid until returned).
    Drop(scan_); Drop(packet_); emit_track_ = -1;
    entry_count_ = 0; pool_used_ = 0;
    for (std::uint32_t t = 0; t < track_count_; ++t) {
        auto& track = tracks_[t];
        track.first = track.last = kNone; track.count = 0; track.window = kNone; track.window_count = 0; track.cursor = kNone; track.after = kNone; track.waiting = true; track.run = false;
        track.key_wait = track.info.kind == TrackKind::Video;
    }
    cursor_ = point.cluster; cluster_end_ = 0; cluster_unknown_ = false; group_end_ = 0; group_has_block_ = false;
    partial_ = true; scan_done_ = false; phase_ = Phase::Index;
    return true;
}
ParseStatus MatroskaDemuxer::Index() noexcept {
    const auto size = source_->Size();
    const auto end = (std::min)(segment_end_, size);
    for (unsigned n = 0; n < kElementsPerStep; ++n) {
        // A BlockGroup ends: its Block, now that its ReferenceBlock and BlockDuration are known.
        if (group_end_ && cursor_ >= group_end_) {
            if (group_has_block_) {
                const auto* p = View(scan_, group_block_, static_cast<std::uint32_t>((std::min)(group_block_bytes_, std::uint64_t{kBlockHead})));
                if (!p) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
                if (!Block(p, group_block_, group_block_bytes_, false, !group_referenced_, group_duration_)) return ParseStatus::Failed;
            }
            group_end_ = 0; group_has_block_ = false;
        }
        if (cluster_end_ && !cluster_unknown_ && cursor_ >= cluster_end_) cluster_end_ = 0;
        if (cursor_ >= end) return Complete() ? ParseStatus::Progress : ParseStatus::Failed;
        const auto avail = static_cast<std::uint32_t>((std::min)(end - cursor_, std::uint64_t{12}));
        const auto* p = View(scan_, cursor_, avail);
        if (!p) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
        std::uint32_t id = 0; std::uint64_t bytes = 0; unsigned length = 0; bool unknown = false;
        // A damaged or cut tail ends the index there: what came before plays.
        if (!Header(p, avail, id, bytes, length, unknown)) return Complete() ? ParseStatus::Progress : ParseStatus::Failed;
        const auto data = cursor_ + length;
        if (cluster_end_ && cluster_unknown_ && TopLevel(id)) { cluster_end_ = 0; cluster_unknown_ = false; } // An unknown-size cluster ends.
        if (unknown && id != kCluster) return Complete() ? ParseStatus::Progress : ParseStatus::Failed;
        if (!unknown && bytes > end - data) bytes = end - data; // The last element of a cut file.
        if (!cluster_end_) {
            if (id == kCluster) { cluster_end_ = unknown ? end : data + bytes; cluster_unknown_ = unknown; cluster_time_ = 0; cursor_ = data; continue; }
            cursor_ = data + bytes; // Cues, Tags, Attachments, Void, ...
            continue;
        }
        if (group_end_) {
            if (id == kBlock) { group_block_ = data; group_block_bytes_ = bytes; group_has_block_ = true; }
            else if (id == kReferenceBlock) group_referenced_ = true;
            else if (id == kBlockDuration && bytes <= 8) {
                const auto* v = View(scan_, cursor_, length + static_cast<std::uint32_t>(bytes)); // With its header: see SimpleBlock.
                if (!v) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
                v += length;
                group_duration_ = static_cast<std::int64_t>(Unsigned(v, bytes) & 0x7fffffffffffffffull);
            }
            cursor_ = data + bytes;
            continue;
        }
        if (id == kTimestamp && bytes <= 8) {
            const auto* v = View(scan_, cursor_, length + static_cast<std::uint32_t>(bytes)); // With its header: see SimpleBlock.
            if (!v) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
            v += length;
            cluster_time_ = static_cast<std::int64_t>(Unsigned(v, bytes) & 0x7fffffffffffffffull);
        } else if (id == kSimpleBlock) {
            // Viewed with its header: when the window ends inside the block's head, the next window starts at the header, which this
            // loop reads again first (a window asked from `data` started just after it, and every window was read twice).
            const auto* b = View(scan_, cursor_, length + static_cast<std::uint32_t>((std::min)(bytes, std::uint64_t{kBlockHead})));
            if (!b) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
            b += length;
            if (!Block(b, data, bytes, true, false, -1)) return ParseStatus::Failed;
        } else if (id == kBlockGroup) {
            group_end_ = data + bytes; group_has_block_ = false; group_referenced_ = false; group_duration_ = -1;
            cursor_ = data;
            continue;
        }
        cursor_ = data + bytes;
    }
    return ParseStatus::Progress;
}

bool MatroskaDemuxer::Complete() noexcept {
    if (scan_done_) return true;
    Drop(scan_);
    if (partial_) { // A scan from a cue's cluster: the end is known, the index is not complete.
        scan_done_ = true;
        for (unsigned i = 0; i < track_count_; ++i) Resolve(tracks_[i]);
        return true;
    }
    pool_used_ = std::uint64_t{entry_count_} * sizeof(Entry); // The lists go after the entries.
    for (unsigned i = 0; i < track_count_; ++i) {
        auto& t = tracks_[i];
        if (!t.usable) continue;
        t.samples = t.count ? static_cast<std::uint32_t*>(Reserve(std::uint64_t{t.count} * 4)) : nullptr;
        std::uint32_t keys = 0;
        for (auto e = t.first; e != kNone; e = entries_[e].next) keys += Key(entries_[e]) ? 1u : 0u;
        t.keys = keys ? static_cast<std::uint32_t*>(Reserve(std::uint64_t{keys} * 4)) : nullptr;
        if ((t.count && !t.samples) || (keys && !t.keys)) return Fail(ParseError::LimitExceeded);
        std::uint32_t s = 0; t.key_count = 0;
        std::int64_t end = 0;
        for (auto e = t.first; e != kNone; e = entries_[e].next, ++s) {
            t.samples[s] = e;
            if (Key(entries_[e])) t.keys[t.key_count++] = s;
        }
        t.info.sample_count = t.count;
        for (std::uint32_t k = t.count > kDurationWindow ? t.count - kDurationWindow : 0; k < t.count; ++k) {
            const auto& e = entries_[t.samples[k]];
            end = (std::max)(end, e.pts + Duration(t.samples[k]));
        }
        if (end > t.info.duration_100ns) t.info.duration_100ns = end;
        if (!t.count) { t.usable = false; t.info.refusal = ParseError::InvalidSampleRange; }
    }
    indexed_ = scan_done_ = true;
    for (unsigned i = 0; i < track_count_; ++i) Resolve(tracks_[i]);
    return true;
}

void MatroskaDemuxer::Resolve(TrackState& t) noexcept {
    if (t.cursor != kNone || !t.waiting) return;
    const auto next = t.after == kNone ? t.first : entries_[t.after].next;
    if (next != kNone) { t.cursor = next; t.waiting = false; }
    else if (scan_done_) t.waiting = false; // The track's end.
}

std::int64_t MatroskaDemuxer::Duration(std::uint32_t entry) const noexcept {
    const auto& e = entries_[entry];
    if ((e.where >> 57) & 1) return e.duration;
    const auto& t = tracks_[TrackOf(e)];
    // Video: the gap to the next presentation time (Entry::gap, from the pictures decoded before and after it), even with a
    // DefaultDuration. Times are rounded to the TimecodeScale (1 ms): at 30 fps a picture stored at 1.667 s lasting the default
    // 33.333 ms would still cover 1.700 s, the next picture's time, so a seek to 1.700 s kept both (the earlier one arriving
    // after the landing, out of order) and a seek to 0.600 s landed on 0.567 s. The gaps partition the timeline exactly.
    if (t.info.kind == TrackKind::Video) {
        if (e.gap) return e.gap;
        return t.default_duration ? static_cast<std::int64_t>(t.default_duration) : 400000; // The last picture: DefaultDuration, else 40 ms.
    }
    if (t.default_duration) return static_cast<std::int64_t>(t.default_duration);
    // Audio and subtitles: to the next later time (subtitles: the next cue, at most 5 s).
    std::int64_t best = INT64_MAX;
    unsigned looked = 0;
    for (auto k = e.next; k != kNone && looked < kDurationWindow; k = entries_[k].next, ++looked) {
        const auto gap = entries_[k].pts - e.pts;
        if (gap > 0) { best = gap; break; }
    }
    if (best == INT64_MAX) return t.info.kind == TrackKind::Subtitle ? 50000000 : 400000; // The last frame: 40 ms.
    return t.info.kind == TrackKind::Subtitle ? (std::min)(best, std::int64_t{50000000}) : best;
}

bool MatroskaDemuxer::DurationKnown(std::uint32_t entry) const noexcept {
    if (scan_done_) return true;
    const auto& e = entries_[entry];
    if ((e.where >> 57) & 1) return true;
    const auto& t = tracks_[TrackOf(e)];
    if (t.default_duration && t.info.kind != TrackKind::Video) return true;
    unsigned looked = 0;
    for (auto k = e.next; k != kNone && looked < kDurationWindow; k = entries_[k].next) ++looked;
    return looked >= (t.info.kind == TrackKind::Video ? kDurationWindow : 1u);
}

ParseStatus MatroskaDemuxer::Emit(void* context, const Publish publish) noexcept {
    if (emit_track_ < 0) {
        std::uint32_t earliest = kNone; bool waiting = false;
        for (std::uint32_t i = 0; i < track_count_; ++i) {
            auto& t = tracks_[i];
            if (!t.usable || !t.emit) continue;
            Resolve(t);
            if (t.cursor != kNone && t.cursor < earliest) { earliest = t.cursor; emit_track_ = static_cast<std::int32_t>(i); }
            waiting = waiting || (t.cursor == kNone && t.waiting);
        }
        if (emit_track_ < 0) {
            Drop(packet_);
            if (waiting && !scan_done_) return Index(); // Every published track waits for the scan.
            return ParseStatus::Done;
        }
    }
    auto& t = tracks_[emit_track_];
    const auto entry = t.cursor;
    if (!DurationKnown(entry)) return Index(); // Its successors first (a video frame's duration).
    const auto& e = entries_[entry];
    if (View(packet_, Offset(e), e.bytes) == nullptr) return phase_ == Phase::Failed ? ParseStatus::Failed : wait_;
    core::MediaPacket packet{};
    packet.pts_100ns = e.pts; packet.dts_100ns = e.pts; // Matroska stores presentation times only.
    packet.duration_100ns = Duration(entry);
    packet.payload_offset = Offset(e) - packet_.view.file_offset;
    packet.payload_bytes = e.bytes;
    packet.payload_block_id = packet_.token.id; packet.payload_generation = packet_.token.generation;
    packet.stream_id = t.info.id; packet.codec_config_id = t.info.config_id;
    packet.playback_epoch = limits_.playback_epoch;
    packet.flags = Key(e) ? core::PacketFlags::KeyFrame : core::PacketFlags::None;
    if (!publish(context, packet)) return ParseStatus::Backpressure;
    packet_ = {}; // The lease moves downstream: no Release here, no payload copy anywhere.
    if (e.next != kNone) t.cursor = e.next;
    else { t.cursor = kNone; t.after = entry; t.waiting = !scan_done_; }
    emit_track_ = -1;
    return ParseStatus::PacketReady;
}

ParseStatus MatroskaDemuxer::StepImpl(void* context, const Publish publish) noexcept {
    if (source_ == nullptr) { (void)Fail(ParseError::NotInitialized); return ParseStatus::Failed; }
    if (phase_ == Phase::Failed) return ParseStatus::Failed;
    if (phase_ == Phase::Head) return Head();
    if (phase_ == Phase::Top) return Top();
    if (phase_ == Phase::Cues) return Cues();
    if (!scan_done_) {
        // The scan keeps kLead entries ahead of the next packet; otherwise it reads whenever its read is not in flight, so the
        // index completes while the pictures play (seeking waits for it).
        // Cursors resolved first: a track whose cursor waits for the scan (the start, a cue seek) has entries once the scan has
        // read them; unresolved, no track had a front and playback waited for the whole scan.
        std::uint32_t front = kNone;
        for (std::uint32_t i = 0; i < track_count_; ++i) if (tracks_[i].usable && tracks_[i].emit) Resolve(tracks_[i]);
        for (std::uint32_t i = 0; i < track_count_; ++i)
            if (tracks_[i].usable && tracks_[i].emit && tracks_[i].cursor != kNone) front = (std::min)(front, tracks_[i].cursor);
        const bool needed = front == kNone || entry_count_ - front < kLead;
        // A network source (scan_ahead off) reads no further than playback needs.
        if (needed || (!scan_.pending && limits_.scan_ahead)) {
            const auto status = Index();
            if (phase_ == Phase::Failed) return ParseStatus::Failed;
            if (needed && !scan_done_) return status;
        }
    }
    const auto status = Emit(context, publish);
    return phase_ == Phase::Failed ? ParseStatus::Failed : status;
}

bool MatroskaDemuxer::ReturnPacket(const core::MediaPacket& packet) noexcept {
    return source_ != nullptr && packet.playback_epoch >= first_epoch_ && packet.playback_epoch <= limits_.playback_epoch &&
           source_->Release({packet.payload_block_id, packet.payload_generation});
}

std::uint32_t MatroskaDemuxer::SampleOf(const TrackState& t, std::uint32_t entry) const noexcept {
    if (!t.samples || entry == kNone) return kNone;
    const std::uint32_t* begin = t.samples; const std::uint32_t* end = begin + t.count;
    const std::uint32_t* at = std::lower_bound(begin, end, entry);
    return at != end && *at == entry ? static_cast<std::uint32_t>(at - begin) : kNone;
}

std::uint32_t MatroskaDemuxer::StartSample(const TrackState& t, std::int64_t target) const noexcept {
    if (t.key_count && t.key_count < t.count) {
        // The last key frame presenting at or before the target (the first one if none does); key times rise in decode order.
        std::uint32_t low = 0, high = t.key_count;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (entries_[t.samples[t.keys[middle]]].pts <= target) low = middle + 1; else high = middle;
        }
        return t.keys[low ? low - 1 : 0];
    }
    // Every frame a key frame (sound, intra-only pictures): times rise in decode order.
    std::uint32_t low = 0, high = t.count;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (entries_[t.samples[middle]].pts <= target) low = middle + 1; else high = middle;
    }
    return low ? low - 1 : 0;
}

void MatroskaDemuxer::Position(TrackState& t, std::int64_t target, std::uint32_t audio_preroll) noexcept {
    if (!t.count) { t.cursor = kNone; t.waiting = false; return; }
    auto index = StartSample(t, target);
    if (t.info.codec == Codec::Aac || t.info.codec == Codec::Opus) index -= (std::min)(index, audio_preroll);
    t.cursor = t.samples[index]; t.waiting = false; t.after = kNone;
}

bool MatroskaDemuxer::Seek(std::int64_t target, std::uint32_t epoch, std::uint32_t audio_preroll) noexcept {
    if (!Seekable() || epoch <= limits_.playback_epoch) return false;
    if (!indexed_) { if (!CueSeek(target)) return false; limits_.playback_epoch = epoch; return true; } // Cues (API 17).
    for (unsigned i = 0; i < track_count_; ++i) if (tracks_[i].usable) Position(tracks_[i], target, audio_preroll);
    Drop(packet_); emit_track_ = -1; limits_.playback_epoch = epoch;
    return true;
}

bool MatroskaDemuxer::Reposition(std::int64_t target, std::uint32_t audio_preroll) noexcept {
    if (!Seekable()) return false;
    if (!indexed_) return CueSeek(target);
    for (unsigned i = 0; i < track_count_; ++i) if (tracks_[i].usable) Position(tracks_[i], target, audio_preroll);
    Drop(packet_); emit_track_ = -1;
    return true;
}

bool MatroskaDemuxer::SeekOthers(std::uint32_t keep, std::int64_t target, std::uint32_t epoch, std::uint32_t audio_preroll) noexcept {
    if (!Indexed() || epoch <= limits_.playback_epoch || keep >= track_count_ || !tracks_[keep].usable || !tracks_[keep].emit) return false;
    for (unsigned i = 0; i < track_count_; ++i) if (i != keep && tracks_[i].usable) Position(tracks_[i], target, audio_preroll);
    Drop(packet_); emit_track_ = -1; limits_.playback_epoch = epoch;
    return true;
}

bool MatroskaDemuxer::Continue(std::uint32_t video, std::int64_t target, std::uint32_t epoch, std::uint32_t audio_preroll, std::uint32_t& resumed) noexcept {
    resumed = 0;
    if (!Indexed() || epoch <= limits_.playback_epoch || video >= track_count_) return false;
    const auto& t = tracks_[video];
    if (!t.usable || !t.emit || !t.key_count || t.key_count == t.count || t.info.kind != TrackKind::Video) return false;
    const auto next = t.cursor == kNone ? t.count : SampleOf(t, t.cursor), sync = StartSample(t, target);
    if (next >= t.count || sync >= next) return false; // The target's group of pictures has not been reached.
    // Every frame published since the key frame must end before the target (see IsobmffParser::Continue); dts = pts here.
    const auto floor = (std::max)(target, std::int64_t{0});
    for (auto s = sync; s < next; ++s) {
        const auto entry = t.samples[s];
        if (entries_[entry].pts > floor - Duration(entry)) return false;
    }
    if (!SeekOthers(video, target, epoch, audio_preroll)) return false;
    resumed = next - sync;
    return true;
}

std::uint32_t MatroskaDemuxer::SyncSample(std::uint32_t index, std::int64_t target) const noexcept {
    if (!Indexed() || index >= track_count_ || !tracks_[index].usable || !tracks_[index].count) return UINT32_MAX;
    return StartSample(tracks_[index], target);
}

std::uint32_t MatroskaDemuxer::DecodeDistance(std::uint32_t index, std::uint32_t from, std::int64_t target, std::uint32_t limit) noexcept {
    const auto cap = (std::min)(limit, UINT32_MAX - 1), none = cap + 1;
    if (!Indexed() || index >= track_count_ || !tracks_[index].usable || from >= tracks_[index].count) return none;
    const auto& t = tracks_[index];
    for (std::uint32_t k = 0; k <= cap && from + k < t.count; ++k) {
        const auto entry = t.samples[from + k];
        const auto pts = entries_[entry].pts;
        if (pts <= target && pts + Duration(entry) > target) return k;
    }
    return none;
}

std::uint32_t MatroskaDemuxer::SyncCount(std::uint32_t index) const noexcept {
    if (!Indexed() || index >= track_count_ || !tracks_[index].usable) return UINT32_MAX;
    const auto& t = tracks_[index];
    return t.key_count ? t.key_count : t.count;
}

std::uint32_t MatroskaDemuxer::SyncAt(std::uint32_t index, std::uint32_t i) const noexcept {
    const auto count = SyncCount(index);
    if (count == UINT32_MAX || i >= count) return UINT32_MAX;
    const auto& t = tracks_[index];
    return t.key_count ? t.keys[i] : i;
}

bool MatroskaDemuxer::SetEmission(std::uint32_t index, bool emit) noexcept {
    if (index >= track_count_ || !ready_ || !tracks_[index].usable) return false;
    auto& t = tracks_[index];
    if (t.emit == emit) return true;
    t.emit = emit;
    if (!emit && emit_track_ == static_cast<std::int32_t>(index)) { Drop(packet_); emit_track_ = -1; }
    return true;
}

bool MatroskaDemuxer::SwitchTrack(std::uint32_t from, std::uint32_t to, std::int64_t at, std::uint32_t audio_preroll) noexcept {
    if (!Seekable() || from >= track_count_ || to >= track_count_ || from == to || !tracks_[to].usable || !tracks_[from].emit) return false;
    tracks_[from].emit = false;
    if (indexed_) Position(tracks_[to], at, audio_preroll);
    else { // The part indexed so far (after a cue seek): its entries in order, the last presenting at or before `at`.
        auto& next = tracks_[to];
        std::uint32_t chosen = next.first, back[64]{}; unsigned seen = 0;
        for (auto e = next.first; e != kNone && entries_[e].pts <= at; e = entries_[e].next) { chosen = e; back[seen++ % 64] = e; }
        if (seen && (next.info.codec == Codec::Aac || next.info.codec == Codec::Opus)) chosen = back[(seen - 1 - (std::min)(audio_preroll, (std::min)(seen - 1, 63u))) % 64];
        next.cursor = chosen; next.waiting = chosen == kNone; next.after = kNone;
    }
    tracks_[to].emit = true;
    Drop(packet_); emit_track_ = -1; // Whatever was chosen next is chosen again: the new track's first frame may precede it.
    return true;
}

bool MatroskaDemuxer::SampleAt(std::uint32_t index, std::uint32_t sample, SampleRecord& out) const noexcept {
    if (!Indexed() || index >= track_count_ || !tracks_[index].usable || sample >= tracks_[index].count) return false;
    const auto entry = tracks_[index].samples[sample];
    const auto& e = entries_[entry];
    out.offset = Offset(e); out.bytes = e.bytes; out.pts_100ns = e.pts; out.duration_100ns = Duration(entry); out.key = Key(e);
    return true;
}

} // namespace media::demux
