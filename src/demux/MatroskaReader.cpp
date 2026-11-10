#include "media/demux/MatroskaReader.hpp"
#include <algorithm>
#include <cstring>

namespace media::demux {
namespace {
// Element ids (with their length marker, as the specification writes them).
constexpr std::uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
                        kSeekId = 0x53AB, kSeekPosition = 0x53AC, kInfo = 0x1549A966, kTimecodeScale = 0x2AD7B1, kDuration = 0x4489,
                        kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackUid = 0x73C5, kTrackType = 0x83,
                        kFlagEnabled = 0xB9, kFlagDefault = 0x88, kName = 0x536E, kLanguage = 0x22B59C, kLanguageBcp47 = 0x22B59D,
                        kCodecId = 0x86, kCodecPrivate = 0x63A2, kVideo = 0xE0, kPixelWidth = 0xB0, kPixelHeight = 0xBA,
                        kFlagInterlaced = 0x9A, kAudio = 0xE1, kSamplingFrequency = 0xB5, kChannels = 0x9F, kContentEncodings = 0x6D80,
                        kCluster = 0x1F43B675, kCues = 0x1C53BB6B, kDefaultDuration = 0x23E383, kCodecDelay = 0x56AA, kSeekPreRoll = 0x56BB,
                        kDisplayWidth = 0x54B0, kDisplayHeight = 0x54BA, kDisplayUnit = 0x54B2, kColour = 0x55B0, kMatrix = 0x55B1,
                        kRange = 0x55B9, kTransfer = 0x55BA, kPrimaries = 0x55BB, kMaxCll = 0x55BC, kMaxFall = 0x55BD, kMastering = 0x55D0,
                        kContentEncoding = 0x6240, kContentCompression = 0x5034, kContentEncryption = 0x5035, kEditionEntry = 0x45B9,
                        kChapterAtom = 0xB6, kChapterTimeStart = 0x91, kChapterFlagHidden = 0x98, kChapterDisplay = 0x80, kChapString = 0x85;
constexpr std::uint32_t kMaxPrivate = 1u << 20; // A CodecPrivate larger than this is malformed for every codec read here.
}

bool MatroskaReader::Header(std::uint64_t at, std::uint64_t end, Element& e) noexcept {
    unsigned char b[12]{};
    const auto got = read_(context_, at, b, (std::min)(std::uint64_t{sizeof(b)}, end > at ? end - at : 0));
    if (got < 2) return false;
    // The id: 1-4 bytes, its marker kept.
    unsigned length = 1; while (length <= 4 && !(b[0] & (0x80u >> (length - 1)))) ++length;
    if (length > 4 || got < length + 1) return false;
    std::uint32_t id = 0; for (unsigned i = 0; i < length; ++i) id = (id << 8) | b[i];
    // The size: 1-8 bytes, its marker removed; all ones is "unknown".
    const unsigned s0 = length;
    unsigned size_length = 1; while (size_length <= 8 && !(b[s0] & (0x80u >> (size_length - 1)))) ++size_length;
    if (size_length > 8 || got < s0 + size_length) return false;
    std::uint64_t size = b[s0] & (0xffu >> size_length); bool ones = size == (0xffu >> size_length);
    for (unsigned i = 1; i < size_length; ++i) { size = (size << 8) | b[s0 + i]; ones = ones && b[s0 + i] == 0xff; }
    e.id = id; e.data = at + s0 + size_length; e.unknown_size = ones;
    e.size = ones ? (end > e.data ? end - e.data : 0) : size;
    return e.data <= end && (ones || e.size <= end - e.data);
}
bool MatroskaReader::Unsigned(const Element& e, std::uint64_t& v) noexcept {
    if (e.size > 8) return false;
    unsigned char b[8]{}; if (read_(context_, e.data, b, static_cast<std::size_t>(e.size)) != e.size) return false;
    v = 0; for (std::uint64_t i = 0; i < e.size; ++i) v = (v << 8) | b[i];
    return true;
}
bool MatroskaReader::Float(const Element& e, double& v) noexcept {
    unsigned char b[8]{};
    if ((e.size != 4 && e.size != 8) || read_(context_, e.data, b, static_cast<std::size_t>(e.size)) != e.size) return false;
    if (e.size == 4) { std::uint32_t u = 0; for (int i = 0; i < 4; ++i) u = (u << 8) | b[i]; float f; std::memcpy(&f, &u, 4); v = f; }
    else { std::uint64_t u = 0; for (int i = 0; i < 8; ++i) u = (u << 8) | b[i]; std::memcpy(&v, &u, 8); }
    return true;
}
void MatroskaReader::Text(const Element& e, char* out, std::size_t capacity) noexcept {
    const auto n = static_cast<std::size_t>((std::min)(e.size, std::uint64_t{capacity - 1}));
    const auto got = read_(context_, e.data, out, n);
    out[got] = 0;
    // Cut at a character boundary, and at an embedded NUL (Matroska strings may be padded with them).
    std::size_t len = std::strlen(out);
    if (e.size > n && len) { std::size_t i = len; while (i && (static_cast<unsigned char>(out[i - 1]) & 0xc0) == 0x80) --i; if (i) out[i - 1] = 0; }
}

MatroskaStatus MatroskaReader::Read(RangeReader read, void* context, std::uint64_t file_bytes) noexcept {
    read_ = read; context_ = context; file_bytes_ = file_bytes;
    track_count_ = 0; timecode_scale_ = 1000000; segment_ = first_cluster_ = cues_ = 0; duration_ticks_ = 0; duration_100ns_ = 0; webm_ = false;
    Element ebml{};
    if (!Header(0, file_bytes, ebml) || ebml.id != kEbml || ebml.unknown_size) return MatroskaStatus::NotMatroska;
    for (std::uint64_t at = ebml.data; at < ebml.data + ebml.size;) {
        Element e{}; if (!Header(at, ebml.data + ebml.size, e) || e.unknown_size) return MatroskaStatus::Malformed;
        if (e.id == kDocType) { char doc[16]; Text(e, doc, sizeof(doc)); webm_ = std::strcmp(doc, "webm") == 0;
                                if (!webm_ && std::strcmp(doc, "matroska") != 0) return MatroskaStatus::NotMatroska; }
        at = e.data + e.size;
    }
    Element segment{};
    if (!Header(ebml.data + ebml.size, file_bytes, segment) || segment.id != kSegment) return MatroskaStatus::Malformed;
    segment_ = segment.data;
    const auto end = segment.data + segment.size;
    // On to the first Cluster (top-level elements are skipped by their sizes): the demuxer starts there.
    for (std::uint64_t at = segment.data; at < end;) {
        Element e{}; if (!Header(at, end, e)) return track_count_ ? MatroskaStatus::Ok : MatroskaStatus::Truncated;
        if (e.id == kCluster) { first_cluster_ = at; break; }
        if (e.unknown_size) return MatroskaStatus::Malformed; // Only clusters may be of unknown size here.
        if (e.id == kInfo) { if (!ReadInfo(e)) return MatroskaStatus::Malformed; }
        else if (e.id == kTracks) { if (!ReadTracks(e)) return MatroskaStatus::Malformed; }
        else if (e.id == kSeekHead) ReadSeekHead(e);
        at = e.data + e.size;
    }
    if (duration_ticks_ > 0) duration_100ns_ = static_cast<std::int64_t>(duration_ticks_ * static_cast<double>(timecode_scale_) / 100.0);
    return track_count_ ? MatroskaStatus::Ok : MatroskaStatus::NoTracks;
}
bool MatroskaReader::ReadInfo(const Element& info) noexcept {
    for (std::uint64_t at = info.data; at < info.data + info.size;) {
        Element e{}; if (!Header(at, info.data + info.size, e) || e.unknown_size) return false;
        if (e.id == kTimecodeScale) { std::uint64_t v = 0; if (Unsigned(e, v) && v) timecode_scale_ = v; }
        else if (e.id == kDuration) (void)Float(e, duration_ticks_);
        at = e.data + e.size;
    }
    return true;
}
void MatroskaReader::ReadSeekHead(const Element& head) noexcept {
    for (std::uint64_t at = head.data; at < head.data + head.size;) {
        Element seek{}; if (!Header(at, head.data + head.size, seek) || seek.unknown_size) return;
        if (seek.id == kSeek) {
            std::uint64_t id = 0, position = 0; bool have_id = false, have_position = false;
            for (std::uint64_t in = seek.data; in < seek.data + seek.size;) {
                Element e{}; if (!Header(in, seek.data + seek.size, e) || e.unknown_size) return;
                if (e.id == kSeekId) have_id = Unsigned(e, id);
                else if (e.id == kSeekPosition) have_position = Unsigned(e, position);
                in = e.data + e.size;
            }
            if (have_id && have_position && id == kCues) cues_ = segment_ + position;
        }
        at = seek.data + seek.size;
    }
}
bool MatroskaReader::ReadTracks(const Element& tracks) noexcept {
    for (std::uint64_t at = tracks.data; at < tracks.data + tracks.size;) {
        Element e{}; if (!Header(at, tracks.data + tracks.size, e) || e.unknown_size) return false;
        if (e.id == kTrackEntry && track_count_ < kMaxTracks) {
            auto& t = tracks_[track_count_]; t = {}; std::memcpy(t.language, "eng", 4);
            if (!ReadTrack(e, t)) return false;
            if (t.number) ++track_count_;
        }
        at = e.data + e.size;
    }
    return true;
}
bool MatroskaReader::ReadTrack(const Element& entry, MatroskaTrack& t) noexcept {
    bool bcp47 = false;
    for (std::uint64_t at = entry.data; at < entry.data + entry.size;) {
        Element e{}; if (!Header(at, entry.data + entry.size, e) || e.unknown_size) return false;
        std::uint64_t v = 0;
        switch (e.id) {
        case kTrackNumber: if (Unsigned(e, v)) t.number = v; break;
        case kTrackUid: if (Unsigned(e, v)) t.uid = v; break;
        case kTrackType: if (Unsigned(e, v)) t.type = v == 1 ? MatroskaTrackType::Video : v == 2 ? MatroskaTrackType::Audio : v == 17 ? MatroskaTrackType::Subtitle : MatroskaTrackType::Other; break;
        case kFlagEnabled: if (Unsigned(e, v)) t.enabled = v != 0; break;
        case kFlagDefault: if (Unsigned(e, v)) t.is_default = v != 0; break;
        case kName: Text(e, t.name, sizeof(t.name)); break;
        case kLanguage: if (!bcp47) Text(e, t.language, sizeof(t.language)); break;
        case kLanguageBcp47: Text(e, t.language, sizeof(t.language)); bcp47 = true; break;
        case kCodecId: Text(e, t.codec_id, sizeof(t.codec_id)); break;
        case kCodecPrivate: if (e.size > kMaxPrivate) return false; t.private_offset = e.data; t.private_bytes = static_cast<std::uint32_t>(e.size); break;
        case kContentEncodings: t.encoded = e.size > 0; if (!ReadEncodings(e, t)) return false; break;
        case kDefaultDuration: if (Unsigned(e, v)) t.default_duration_ns = v; break;
        case kCodecDelay: if (Unsigned(e, v)) t.codec_delay_ns = v; break;
        case kSeekPreRoll: if (Unsigned(e, v)) t.seek_preroll_ns = v; break;
        case kVideo: {
            std::uint64_t unit = 0, dw = 0, dh = 0;
            for (std::uint64_t in = e.data; in < e.data + e.size;) {
                Element c{}; if (!Header(in, e.data + e.size, c) || c.unknown_size) return false;
                if (c.id == kPixelWidth && Unsigned(c, v)) t.width = static_cast<std::uint32_t>(v);
                else if (c.id == kPixelHeight && Unsigned(c, v)) t.height = static_cast<std::uint32_t>(v);
                else if (c.id == kFlagInterlaced && Unsigned(c, v)) t.interlaced = v == 1;
                else if (c.id == kDisplayWidth) (void)Unsigned(c, dw);
                else if (c.id == kDisplayHeight) (void)Unsigned(c, dh);
                else if (c.id == kDisplayUnit) (void)Unsigned(c, unit);
                else if (c.id == kColour && !ReadColour(c, t)) return false;
                in = c.data + c.size;
            }
            if (unit == 0 && dw && dh && dw <= UINT32_MAX && dh <= UINT32_MAX) { t.display_width = static_cast<std::uint32_t>(dw); t.display_height = static_cast<std::uint32_t>(dh); }
            break;
        }
        case kAudio:
            for (std::uint64_t in = e.data; in < e.data + e.size;) {
                Element c{}; if (!Header(in, e.data + e.size, c) || c.unknown_size) return false;
                if (c.id == kSamplingFrequency) (void)Float(c, t.sample_rate);
                else if (c.id == kChannels && Unsigned(c, v)) t.channels = static_cast<std::uint32_t>(v);
                in = c.data + c.size;
            }
            break;
        default: break;
        }
        at = e.data + e.size;
    }
    return true;
}
bool MatroskaReader::ReadColour(const Element& colour, MatroskaTrack& t) noexcept {
    t.colour = true;
    for (std::uint64_t at = colour.data; at < colour.data + colour.size;) {
        Element e{}; if (!Header(at, colour.data + colour.size, e) || e.unknown_size) return false;
        std::uint64_t v = 0;
        const auto put = [&](std::uint16_t& out) { if (Unsigned(e, v) && v < 256) out = static_cast<std::uint16_t>(v); };
        switch (e.id) {
        case kMatrix: put(t.matrix); break;
        case kRange: put(t.range); break;
        case kTransfer: put(t.transfer); break;
        case kPrimaries: put(t.primaries); break;
        case kMaxCll: if (Unsigned(e, v) && v <= 65535) t.max_cll = static_cast<std::uint16_t>(v); break;
        case kMaxFall: if (Unsigned(e, v) && v <= 65535) t.max_fall = static_cast<std::uint16_t>(v); break;
        case kMastering: t.mastering = e.size > 0; break;
        default: break;
        }
        at = e.data + e.size;
    }
    return true;
}
bool MatroskaReader::ReadEncodings(const Element& encodings, MatroskaTrack& t) noexcept {
    // Any ContentEncoding that compresses (zlib, header stripping, ...) or encrypts: the frames are not the codec's as stored.
    bool any = false;
    for (std::uint64_t at = encodings.data; at < encodings.data + encodings.size;) {
        Element e{}; if (!Header(at, encodings.data + encodings.size, e) || e.unknown_size) return false;
        if (e.id == kContentEncoding)
            for (std::uint64_t in = e.data; in < e.data + e.size;) {
                Element c{}; if (!Header(in, e.data + e.size, c) || c.unknown_size) return false;
                if (c.id == kContentCompression || c.id == kContentEncryption) any = true;
                in = c.data + c.size;
            }
        at = e.data + e.size;
    }
    t.encoded = any;
    return true;
}
bool MatroskaReader::ReadInfoAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size) noexcept {
    read_ = read; context_ = context; file_bytes_ = data + size;
    Element info{}; info.data = data; info.size = size;
    if (!ReadInfo(info)) return false;
    duration_100ns_ = duration_ticks_ > 0 ? static_cast<std::int64_t>(duration_ticks_ * static_cast<double>(timecode_scale_) / 100.0) : 0;
    return true;
}
bool MatroskaReader::ReadTracksAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size) noexcept {
    read_ = read; context_ = context; file_bytes_ = data + size; track_count_ = 0;
    Element tracks{}; tracks.data = data; tracks.size = size;
    return ReadTracks(tracks);
}
bool MatroskaReader::ReadAtom(const Element& atom) noexcept {
    std::uint64_t start = 0; bool hidden = false, have_start = false; char title[sizeof(MatroskaChapter::title)]{};
    for (std::uint64_t at = atom.data; at < atom.data + atom.size;) {
        Element e{}; if (!Header(at, atom.data + atom.size, e) || e.unknown_size) return false;
        std::uint64_t v = 0;
        if (e.id == kChapterTimeStart) have_start = Unsigned(e, start);
        else if (e.id == kChapterFlagHidden && Unsigned(e, v)) hidden = v != 0;
        else if (e.id == kChapterDisplay && !title[0])
            for (std::uint64_t in = e.data; in < e.data + e.size;) {
                Element c{}; if (!Header(in, e.data + e.size, c) || c.unknown_size) return false;
                if (c.id == kChapString) Text(c, title, sizeof(title));
                in = c.data + c.size;
            }
        at = e.data + e.size;
    }
    if (have_start && !hidden && chapter_count_ < kMaxChapters && start / 100 <= static_cast<std::uint64_t>(INT64_MAX)) {
        auto& c = chapters_[chapter_count_++];
        c.start_100ns = static_cast<std::int64_t>(start / 100);
        std::memcpy(c.title, title, sizeof(title));
    }
    return true;
}
bool MatroskaReader::ReadChaptersAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size) noexcept {
    read_ = read; context_ = context; file_bytes_ = data + size; chapter_count_ = 0;
    for (std::uint64_t at = data; at < data + size;) {
        Element edition{}; if (!Header(at, data + size, edition) || edition.unknown_size) return false;
        if (edition.id == kEditionEntry) {
            for (std::uint64_t in = edition.data; in < edition.data + edition.size;) {
                Element e{}; if (!Header(in, edition.data + edition.size, e) || e.unknown_size) return false;
                if (e.id == kChapterAtom && !ReadAtom(e)) return false;
                in = e.data + e.size;
            }
            return true; // The first edition only.
        }
        at = edition.data + edition.size;
    }
    return true;
}
void MatroskaReader::ReadSeekHeadAt(RangeReader read, void* context, std::uint64_t data, std::uint64_t size, std::uint64_t segment) noexcept {
    read_ = read; context_ = context; file_bytes_ = data + size; segment_ = segment;
    Element head{}; head.data = data; head.size = size;
    ReadSeekHead(head);
}
}
