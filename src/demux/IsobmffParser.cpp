#include "media/demux/IsobmffParser.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>

namespace media::demux {
namespace {
constexpr std::uint32_t Tag(const char a, const char b, const char c, const char d) noexcept {
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
           (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
}
constexpr auto ftyp = Tag('f','t','y','p'), moov = Tag('m','o','o','v'), mvhd = Tag('m','v','h','d');
constexpr auto trak = Tag('t','r','a','k'), tkhd = Tag('t','k','h','d'), mdia = Tag('m','d','i','a');
constexpr auto mdhd = Tag('m','d','h','d'), hdlr = Tag('h','d','l','r'), minf = Tag('m','i','n','f');
constexpr auto stbl = Tag('s','t','b','l'), stsd = Tag('s','t','s','d'), stts = Tag('s','t','t','s');
constexpr auto ctts = Tag('c','t','t','s'), stsc = Tag('s','t','s','c'), stsz = Tag('s','t','s','z');
constexpr auto stco = Tag('s','t','c','o'), co64 = Tag('c','o','6','4'), stss = Tag('s','t','s','s');
constexpr auto mdat = Tag('m','d','a','t'), mvex = Tag('m','v','e','x'), trex = Tag('t','r','e','x');
constexpr auto moof = Tag('m','o','o','f'), mfhd = Tag('m','f','h','d'), traf = Tag('t','r','a','f');
constexpr auto tfhd = Tag('t','f','h','d'), tfdt = Tag('t','f','d','t'), trun = Tag('t','r','u','n');
constexpr auto dinf = Tag('d','i','n','f'), dref = Tag('d','r','e','f');
constexpr auto edts = Tag('e','d','t','s'), elst = Tag('e','l','s','t');
constexpr auto sgpd = Tag('s','g','p','d'), sbgp = Tag('s','b','g','p');
constexpr auto udta = Tag('u','d','t','a'), chpl = Tag('c','h','p','l'), tref = Tag('t','r','e','f'), chap = Tag('c','h','a','p');
constexpr std::uint64_t kChapterBoxBytes = 1024 * 1024; // Larger udta boxes (cover art, ...) are not read for chapters.
constexpr std::uint32_t Tkhd = 1, Mdhd = 2, Hdlr = 4, Stsd = 8, Stts = 16, Stsc = 32;
constexpr std::uint32_t Stsz = 64, Stco = 128, Ctts = 256, Stss = 512, Dref = 1024, Elst = 2048;
constexpr std::uint32_t Required = Tkhd | Mdhd | Hdlr | Stsd | Stts | Stsc | Stsz | Stco;
constexpr std::uint32_t kMarkRows = 64;              // stts/ctts runs between random-access marks.
constexpr std::uint32_t kTableWindow = 1024 * 1024;  // Largest read of a sample table box at once.
constexpr std::uint32_t kValidateSlice = 65536;      // Samples validated per Step.

std::uint32_t U32(const std::byte* p) noexcept {
    return (std::to_integer<std::uint32_t>(p[0]) << 24) |
           (std::to_integer<std::uint32_t>(p[1]) << 16) |
           (std::to_integer<std::uint32_t>(p[2]) << 8) | std::to_integer<std::uint32_t>(p[3]);
}
std::uint16_t U16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[0]) << 8) |
                                     std::to_integer<unsigned>(p[1]));
}
std::uint64_t U64(const std::byte* p) noexcept {
    return (static_cast<std::uint64_t>(U32(p)) << 32) | U32(p + 4);
}
std::int64_t Signed32(const std::uint32_t value) noexcept {
    return value <= INT32_MAX ? value : static_cast<std::int64_t>(value) - 0x100000000LL;
}
bool Add(const std::uint64_t a, const std::uint64_t b, std::uint64_t& out) noexcept {
    if (b > UINT64_MAX - a) return false;
    out = a + b; return true;
}
bool SignedOffset(const std::uint64_t base, const std::int64_t delta, std::uint64_t& out) noexcept {
    if (delta >= 0) return Add(base, static_cast<std::uint64_t>(delta), out);
    const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(delta);
    if (base < magnitude) return false;
    out = base - magnitude; return true;
}
struct Reader {
    const std::byte* data;
    std::uint32_t size, position = 0;
    bool ok = true;
    bool Skip(const std::uint32_t n) noexcept {
        if (n > size - position) { ok = false; return false; }
        position += n; return true;
    }
    std::uint32_t Read32() noexcept {
        if (!Skip(4)) return 0;
        return U32(data + position - 4);
    }
    std::uint64_t Read64() noexcept {
        if (!Skip(8)) return 0;
        return U64(data + position - 8);
    }
};
template <typename T>
T* AllocateArray(core::LinearMemoryArena& arena, const std::uint32_t count) noexcept {
    if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) return nullptr;
    auto* objects = static_cast<T*>(arena.Allocate(sizeof(T) * count, alignof(T)));
    if (objects != nullptr)
        for (std::uint32_t i = 0; i < count; ++i) std::construct_at(objects + i);
    return objects;
}
template <typename Track>
std::uint64_t ChunkOffset(const Track& track, const std::uint32_t chunk) noexcept {
    return track.chunks64 ? track.chunks64[chunk] : track.chunks32[chunk];
}
bool IsTable(const std::uint32_t type) noexcept {
    return type == stts || type == ctts || type == stsc || type == stsz || type == stco || type == co64 || type == stss;
}
// mdhd language: three 5-bit letters (ISO 639-2/T, 'a' = 1), or below 0x400 a QuickTime Macintosh language code.
void Language(const std::uint16_t packed, char out[4]) noexcept {
    static constexpr char mac[][4]{"eng", "fra", "deu", "ita", "nld", "swe", "spa", "dan", "por", "nor", "heb", "jpn", "ara", "fin", "ell",
                                   "isl", "mlt", "tur", "hrv", "zho", "urd", "hin", "tha", "kor", "lit", "pol", "hun", "est", "lav", "sme",
                                   "fao", "fas", "rus", "zho", "nld", "gle", "sqi", "ron", "ces", "slk", "slv", "yid", "srp", "mkd", "bul",
                                   "ukr", "bel", "uzb", "kaz"};
    const char* chosen = "und";
    char letters[4]{};
    if (packed < 0x400) { if (packed < std::size(mac)) chosen = mac[packed]; }
    else {
        bool good = true;
        for (unsigned i = 0; i < 3; ++i) {
            const auto c = static_cast<char>(((packed >> (10 - 5 * i)) & 31) + 0x60);
            good = good && c >= 'a' && c <= 'z'; letters[i] = c;
        }
        if (good) chosen = letters;
    }
    for (unsigned i = 0; i < 4; ++i) out[i] = i < 3 ? chosen[i] : '\0';
}
// hdlr name: a NUL-terminated UTF-8 string (QuickTime writes a length byte first). Control characters are dropped and a
// character that does not fit is cut whole.
void HandlerName(const std::byte* data, const std::uint32_t bytes, char (&out)[32]) noexcept {
    std::uint32_t begin = 0;
    if (bytes > 1 && std::to_integer<std::uint32_t>(data[0]) == bytes - 1) begin = 1; // Pascal string.
    unsigned n = 0;
    for (std::uint32_t i = begin; i < bytes && n + 1 < sizeof(out); ++i) {
        const auto c = std::to_integer<unsigned char>(data[i]);
        if (!c) break;
        if (c < 0x20 || c == 0x7f) continue;
        out[n++] = static_cast<char>(c);
    }
    // Cut a trailing incomplete (or ill-formed) sequence.
    unsigned lead = n;
    while (lead > 0 && (static_cast<unsigned char>(out[lead - 1]) & 0xc0) == 0x80) --lead;
    if (lead == 0) n = 0; // Continuation bytes only.
    else if (static_cast<unsigned char>(out[lead - 1]) >= 0x80) {
        const auto c = static_cast<unsigned char>(out[lead - 1]);
        const unsigned need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 0;
        if (need == 0 || n - (lead - 1) != need) n = lead - 1;
    }
    out[n] = '\0';
}
}

bool To100ns(const std::int64_t ticks, const std::uint32_t scale, std::int64_t& result) noexcept {
    if (scale == 0 || ticks == INT64_MIN) return false;
    const auto magnitude = ticks < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(ticks) :
                                      static_cast<std::uint64_t>(ticks);
    const auto quotient = magnitude / scale;
    const auto fraction = ((magnitude % scale) * 10000000) / scale;
    if (quotient > (static_cast<std::uint64_t>(INT64_MAX) - fraction) / 10000000) return false;
    const auto value = static_cast<std::int64_t>(quotient * 10000000 + fraction);
    result = ticks < 0 ? -value : value;
    return true;
}

bool IsobmffParser::Description(TrackState& track, const std::byte* data,
                               const std::uint32_t bytes, const std::uint64_t offset) noexcept {
    auto& info = track.info;
    if (bytes >= 16) info.sample_entry = U32(data + 12);
    if (bytes < 16 || U32(data + 4) != 1) return Refuse(ParseError::Unsupported); // Several sample descriptions.
    const auto size = U32(data + 8);
    const auto type = U32(data + 12);
    if (size != bytes - 8 || size < 16 || U16(data + 8 + 14) != 1) return Refuse(ParseError::InvalidCodec);
    const auto* entry = data + 8;
    bool video = true, text = false;
    std::uint32_t config_type = 0;
    if (type == Tag('a','v','c','1') || type == Tag('a','v','c','3')) {
        info.codec = Codec::H264; config_type = Tag('a','v','c','C');
    } else if (type == Tag('h','v','c','1') || type == Tag('h','e','v','1')) {
        info.codec = Codec::Hevc; config_type = Tag('h','v','c','C');
    } else if (type == Tag('a','v','0','1')) {
        info.codec = Codec::Av1; config_type = Tag('a','v','1','C');
    } else if (type == Tag('m','p','4','a')) {
        info.codec = Codec::Aac; config_type = Tag('e','s','d','s'); video = false;
    } else if (type == Tag('O','p','u','s')) {
        info.codec = Codec::Opus; config_type = Tag('d','O','p','s'); video = false;
    } else if (type == Tag('i','p','c','m') || type == Tag('f','p','c','m')) {
        info.codec = Codec::Pcm; config_type = Tag('p','c','m','C'); video = false; info.pcm_float = type == Tag('f','p','c','m');
    } else if (type == Tag('s','o','w','t') || type == Tag('t','w','o','s')) {
        info.codec = Codec::Pcm; video = false; info.pcm_little_endian = type == Tag('s','o','w','t'); // The entry says the size.
    } else if (type == Tag('t','x','3','g') || type == Tag('t','e','x','t')) {
        // 3GPP timed text: display flags, justification, colour, text box and default style; a font table may follow.
        // QuickTime text (chapter tracks write it) has another entry, but the same samples: a 16-bit length and the text.
        info.codec = Codec::Tx3g; video = false; text = true;
    } else return Refuse(ParseError::Unsupported); // AC-3, E-AC-3, VP9, WebVTT, TTML, ...
    const auto header = text ? 46u : video ? 86u : 36u;
    if (size < header) return Refuse(ParseError::InvalidCodec);
    if (text) {}
    else if (video) {
        info.width = U16(entry + 32); info.height = U16(entry + 34);
        if (!info.width || !info.height) return Refuse(ParseError::InvalidCodec);
    } else {
        if (U16(entry + 16) != 0) return Refuse(ParseError::Unsupported); // QuickTime audio v1/v2.
        info.channels = U16(entry + 24); info.sample_rate = U32(entry + 32) >> 16;
        if (!info.channels || !info.sample_rate) return Refuse(ParseError::InvalidCodec);
        if (info.codec == Codec::Pcm && !config_type) { // sowt, twos: 8 or 16 bits from the entry's sample size.
            const auto bits = U16(entry + 26);
            if ((bits != 8 && bits != 16) || (bits == 8 && info.pcm_little_endian)) return Refuse(ParseError::Unsupported);
            info.pcm_bits = static_cast<std::uint8_t>(bits);
        }
    }
    // QuickTime text: after its fixed fields comes the font name, not boxes; nothing in it is needed.
    std::uint32_t position = type == Tag('t','e','x','t') ? size : header;
    bool found = text || (info.codec == Codec::Pcm && !config_type); // Timed text and QuickTime PCM need no decoder configuration.
    while (position < size) {
        if (size - position < 8) return Refuse(ParseError::InvalidCodec);
        const auto child_size = U32(entry + position);
        const auto child_type = U32(entry + position + 4);
        if (child_size < 8 || child_size > size - position) return Refuse(ParseError::InvalidCodec);
        if (child_type == Tag('s','i','n','f')) return Refuse(ParseError::Unsupported); // Encrypted.
        if (!text && child_type == config_type) {
            if (found) return Refuse(ParseError::InvalidCodec);
            if (!CodecConfig(info, child_type, entry + position + 8, child_size - 8,
                             offset + 8 + position + 8)) return false;
            found = true;
        } else if (video && child_type == Tag('c','o','l','r') && !info.color.described && child_size >= 18 &&
                   (U32(entry + position + 8) == Tag('n','c','l','x') || U32(entry + position + 8) == Tag('n','c','l','c'))) {
            // An ICC profile (prof/rICC) carries no code points and is skipped; nclc (QuickTime) has no range flag.
            const auto* colour = entry + position + 12;
            info.color.primaries = U16(colour); info.color.transfer = U16(colour + 2); info.color.matrix = U16(colour + 4);
            info.color.full_range = U32(entry + position + 8) == Tag('n','c','l','x') && child_size >= 19 &&
                                    (std::to_integer<unsigned>(colour[6]) & 0x80) != 0;
            info.color.range_described = U32(entry + position + 8) == Tag('n','c','l','x') && child_size >= 19;
            info.color.described = true;
        } else if (video && child_type == Tag('p','a','s','p') && child_size >= 16 && !info.pixel_aspect_width) {
            const auto horizontal = U32(entry + position + 8), vertical = U32(entry + position + 12);
            if (horizontal && vertical) { info.pixel_aspect_width = horizontal; info.pixel_aspect_height = vertical; }
        } else if (video && child_type == Tag('c','l','l','i') && child_size >= 12) {
            info.color.max_cll = U16(entry + position + 8); info.color.max_fall = U16(entry + position + 10);
        } else if (video && child_type == Tag('m','d','c','v') && child_size >= 32) {
            info.color.mastering_display = true;
        }
        position += child_size;
    }
    info.config_id = static_cast<std::uint32_t>(current_track_) + 1;
    return found || Refuse(ParseError::InvalidCodec);
}

bool IsobmffParser::CodecConfig(TrackInfo& info, const std::uint32_t type,
                               const std::byte* data, const std::uint32_t bytes,
                               const std::uint64_t offset) noexcept {
    info.configuration = {offset, bytes};
    if (type == Tag('p','c','m','C')) {
        // FullBox (version 0), format_flags (bit 0: little-endian), PCM_sample_size.
        if (bytes < 6 || data[0] != std::byte{0}) return Refuse(ParseError::InvalidCodec);
        const auto bits = std::to_integer<unsigned>(data[5]);
        const bool valid = info.pcm_float ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        if (!valid) return Refuse(ParseError::Unsupported);
        info.pcm_bits = static_cast<std::uint8_t>(bits); info.pcm_little_endian = (std::to_integer<unsigned>(data[4]) & 1u) != 0;
        return true;
    }
    auto parameter = [&](const std::uint32_t start, const std::uint32_t length,
                         const std::uint8_t kind) noexcept {
        if (length == 0 || start > bytes || length > bytes - start || info.parameter_count == 64)
            return Refuse(ParseError::InvalidCodec);
        info.parameters[info.parameter_count++] = {{offset + start, length}, kind};
        return true;
    };
    if (type == Tag('a','v','c','C')) {
        if (bytes < 7 || data[0] != std::byte{1} || (std::to_integer<unsigned>(data[4]) & 0xfc) != 0xfc ||
            (std::to_integer<unsigned>(data[5]) & 0xe0) != 0xe0) return Refuse(ParseError::InvalidCodec);
        info.nal_length_bytes = static_cast<std::uint8_t>((std::to_integer<unsigned>(data[4]) & 3) + 1);
        if (info.nal_length_bytes == 3) return Refuse(ParseError::InvalidCodec);
        std::uint32_t position = 6;
        auto nals = [&](const std::uint32_t count, const std::uint8_t kind) noexcept {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (bytes - position < 2) return Refuse(ParseError::InvalidCodec);
                const auto length = U16(data + position); position += 2;
                if (!parameter(position, length, kind)) return false;
                position += length;
            }
            return true;
        };
        if (!nals(std::to_integer<unsigned>(data[5]) & 31, 7)) return false;
        if (position == bytes) return Refuse(ParseError::InvalidCodec);
        const auto pps = std::to_integer<unsigned>(data[position++]);
        if (!nals(pps, 8)) return false;
        if (position < bytes) {
            const auto profile = std::to_integer<unsigned>(data[1]);
            // The profiles whose avcC carries the chroma/bit-depth extension (ISO/IEC 14496-15 5.3.3.1.2). 244 (High 4:4:4
            // Predictive) was missing: such a record was refused as invalid where it is valid and only not decodable here.
            if (profile != 100 && profile != 110 && profile != 122 && profile != 144 && profile != 244 && profile != 44 &&
                profile != 83 && profile != 86 && profile != 118 && profile != 128 && profile != 138 &&
                profile != 139 && profile != 134 && profile != 135) return Refuse(ParseError::InvalidCodec);
            if (bytes - position < 4 || (std::to_integer<unsigned>(data[position]) & 0xfc) != 0xfc ||
                (std::to_integer<unsigned>(data[position + 1]) & 0xf8) != 0xf8 ||
                (std::to_integer<unsigned>(data[position + 2]) & 0xf8) != 0xf8) return Refuse(ParseError::InvalidCodec);
            position += 3;
            const auto extensions = std::to_integer<unsigned>(data[position++]);
            if (!nals(extensions, 13)) return false;
        }
        return position == bytes || Refuse(ParseError::InvalidCodec);
    }
    if (type == Tag('h','v','c','C')) {
        if (bytes < 23 || data[0] != std::byte{1}) return Refuse(ParseError::InvalidCodec);
        info.nal_length_bytes = static_cast<std::uint8_t>((std::to_integer<unsigned>(data[21]) & 3) + 1);
        if (info.nal_length_bytes == 3) return Refuse(ParseError::InvalidCodec);
        std::uint32_t position = 23;
        const auto arrays = std::to_integer<unsigned>(data[22]);
        for (std::uint32_t a = 0; a < arrays; ++a) {
            if (bytes - position < 3) return Refuse(ParseError::InvalidCodec);
            const auto kind = static_cast<std::uint8_t>(std::to_integer<unsigned>(data[position++]) & 63);
            const auto count = U16(data + position); position += 2;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (bytes - position < 2) return Refuse(ParseError::InvalidCodec);
                const auto length = U16(data + position); position += 2;
                if (!parameter(position, length, kind)) return false;
                position += length;
            }
        }
        return position == bytes || Refuse(ParseError::InvalidCodec);
    }
    if (type == Tag('a','v','1','C')) {
        if (bytes < 4 || data[0] != std::byte{0x81} || (std::to_integer<unsigned>(data[3]) & 0xe0) != 0)
            return Refuse(ParseError::InvalidCodec);
        // Preserve the sequence-header/config OBU stream in-place, without codec decoding.
        return bytes == 4 || parameter(4, bytes - 4, 1);
    }
    if (type == Tag('d','O','p','s')) {
        if (bytes < 11 || data[0] != std::byte{0} || data[1] == std::byte{0}) return Refuse(ParseError::InvalidCodec);
        const auto channels = std::to_integer<unsigned>(data[1]);
        const auto mapping = std::to_integer<unsigned>(data[10]);
        if (channels != info.channels || (mapping == 0 && (channels > 2 || bytes != 11)) ||
            (mapping != 0 && bytes != 13 + channels)) return Refuse(ParseError::InvalidCodec);
        if (mapping != 0) {
            const auto streams = std::to_integer<unsigned>(data[11]);
            const auto coupled = std::to_integer<unsigned>(data[12]);
            if (!streams || coupled > streams || streams + coupled > 255) return Refuse(ParseError::InvalidCodec);
            for (unsigned i = 0; i < channels; ++i) {
                const auto channel = std::to_integer<unsigned>(data[13 + i]);
                if (channel != 255 && channel >= streams + coupled) return Refuse(ParseError::InvalidCodec);
            }
        }
        info.sample_rate = 48000;
        return parameter(0, bytes, 0);
    }
    if (type == Tag('e','s','d','s')) {
        if (bytes < 4 || U32(data) != 0) return Refuse(ParseError::InvalidCodec);
        std::uint32_t position = 4;
        auto descriptor = [&](const std::uint8_t expected, const std::uint32_t parent_end,
                              std::uint32_t& end) noexcept {
            if (position >= parent_end || std::to_integer<unsigned>(data[position++]) != expected)
                return Refuse(ParseError::InvalidCodec);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) {
                if (position >= parent_end) return Refuse(ParseError::InvalidCodec);
                const auto value = std::to_integer<unsigned>(data[position++]);
                length = (length << 7) | (value & 127);
                if (!(value & 128)) {
                    if (length > parent_end - position) return Refuse(ParseError::InvalidCodec);
                    end = position + length; return true;
                }
            }
            return Refuse(ParseError::InvalidCodec);
        };
        std::uint32_t es_end = 0, decoder_end = 0, specific_end = 0;
        if (!descriptor(3, bytes, es_end) || es_end - position < 3) return Refuse(ParseError::InvalidCodec);
        position += 2;
        const auto flags = std::to_integer<unsigned>(data[position++]);
        auto skip = [&](const unsigned n) noexcept {
            if (n > es_end - position) return false;
            position += n; return true;
        };
        if ((flags & 128) && !skip(2)) return Refuse(ParseError::InvalidCodec);
        if (flags & 64) {
            if (!skip(1)) return Refuse(ParseError::InvalidCodec);
            if (!skip(std::to_integer<unsigned>(data[position - 1]))) return Refuse(ParseError::InvalidCodec);
        }
        if ((flags & 32) && !skip(2)) return Refuse(ParseError::InvalidCodec);
        if (!descriptor(4, es_end, decoder_end) || decoder_end - position < 13) return Refuse(ParseError::InvalidCodec);
        if (data[position] != std::byte{0x40} || (std::to_integer<unsigned>(data[position + 1]) >> 2) != 5)
            return Refuse(ParseError::Unsupported);
        position += 13;
        if (!descriptor(5, decoder_end, specific_end) || specific_end - position < 2)
            return Refuse(ParseError::InvalidCodec);
        if (!parameter(position, specific_end - position, 5)) return false;
        // Object profile semantics remain the decoder's responsibility. Bound every
        // optional descriptor; no recursive descriptor tree is constructed.
        position = specific_end;
        if (position != decoder_end) return Refuse(ParseError::Unsupported);
        while (position < es_end) {
            const auto tag = std::to_integer<std::uint8_t>(data[position]);
            std::uint32_t end = 0;
            if (!descriptor(tag, es_end, end)) return false;
            position = end;
        }
        return es_end == bytes || Refuse(ParseError::InvalidCodec);
    }
    return Refuse(ParseError::Unsupported);
}

bool IsobmffParser::FinishTrack(TrackState& track) noexcept {
    if (!Usable(track)) return false;
    if ((track.seen & Required) != Required) return Refuse(track, ParseError::InvalidTable);
    // The sample entry must be of the handler's kind: a video codec in a vide track, audio in soun, text in text.
    const auto codec = track.info.codec;
    const bool audio = codec == Codec::Aac || codec == Codec::Opus || codec == Codec::Pcm, text = codec == Codec::Tx3g;
    const auto kind = track.info.kind;
    if ((kind == TrackKind::Audio) != audio || (kind == TrackKind::Subtitle) != text || (kind == TrackKind::Video && (audio || text || codec == Codec::Unknown)))
        return Refuse(track, ParseError::InvalidCodec);
    if (fragmented_) {
        if (track.sample_count || track.chunk_count || track.mapping_count || track.timing_count ||
            track.composition_count || track.sync_count) return Refuse(track, ParseError::Unsupported);
        return track.has_trex || Refuse(track, ParseError::InvalidTable);
    }
    track.info.sample_count = track.sample_count;
    // A last sample of no duration (2026-11-04: muxers that do not know the last frame's length write a final stts run of
    // (1, 0) — mpv's encoder did, and the whole video track was refused, "stage 4"): it lasts as long as the one before it. Its
    // DTS is unchanged (the delta is after it); only its duration is filled in. A zero delta anywhere else still refuses.
    if (track.timing_count >= 2 && track.timing[track.timing_count - 1].count == 1 && track.timing[track.timing_count - 1].value == 0)
        track.timing[track.timing_count - 1].value = track.timing[track.timing_count - 2].value;
    // Decode times: the runs cover every sample exactly. A mark every kMarkRows runs anchors random access.
    std::uint64_t sample = 0, dts = 0;
    for (std::uint32_t row = 0; row < track.timing_count; ++row) {
        const auto& run = track.timing[row];
        if (!run.count || !run.value || run.count > track.sample_count - sample) return Refuse(track, ParseError::InvalidTable);
        if (row % kMarkRows == 0) track.timing_marks[row / kMarkRows] = {dts, static_cast<std::uint32_t>(sample), 0};
        sample += run.count;
        if (!Add(dts, std::uint64_t{run.count} * run.value, dts) || dts > INT64_MAX) return Refuse(track, ParseError::InvalidTimestamp);
    }
    if (sample != track.sample_count) return Refuse(track, ParseError::InvalidTable);
    sample = 0;
    for (std::uint32_t row = 0; row < track.composition_count; ++row) {
        const auto& run = track.composition[row];
        if (!run.count || run.count > track.sample_count - sample) return Refuse(track, ParseError::InvalidTable);
        if (row % kMarkRows == 0) track.composition_marks[row / kMarkRows] = {0, static_cast<std::uint32_t>(sample), 0};
        sample += run.count;
    }
    if ((track.seen & Ctts) && sample != track.sample_count) return Refuse(track, ParseError::InvalidTable);
    std::uint64_t previous = 0;
    for (std::uint32_t i = 0; i < track.sync_count; ++i) {
        const auto id = track.sync[i];
        if (id <= previous || id > track.sample_count) return Refuse(track, ParseError::InvalidTable);
        previous = id;
    }
    if (track.sample_count == 0)
        return (track.mapping_count == 0 && track.chunk_count == 0) || Refuse(track, ParseError::InvalidTable);
    if (!track.mapping_count || !track.chunk_count || track.mapping[0].first_chunk != 1) return Refuse(track, ParseError::InvalidTable);
    // Chunk runs: each spans the chunks up to the next run's first, and together they hold every sample exactly.
    sample = 0;
    for (std::uint32_t i = 0; i < track.mapping_count; ++i) {
        auto& run = track.mapping[i];
        const std::uint64_t end = i + 1 < track.mapping_count ? track.mapping[i + 1].first_chunk : std::uint64_t{track.chunk_count} + 1;
        if (!run.per_chunk || run.first_chunk > track.chunk_count || end <= run.first_chunk) return Refuse(track, ParseError::InvalidTable);
        const auto samples = (end - run.first_chunk) * run.per_chunk;
        if (samples > track.sample_count - sample) return Refuse(track, ParseError::InvalidTable);
        run.first_sample = static_cast<std::uint32_t>(sample);
        sample += samples;
    }
    return sample == track.sample_count || Refuse(track, ParseError::InvalidTable);
}

bool IsobmffParser::FinishMovie() noexcept {
    if (!ftyp_seen_ || !mvhd_seen_ || !track_count_) return Fail(ParseError::BadBox);
    for (std::uint32_t i = 0; i < track_count_; ++i)
        for (std::uint32_t j = 0; j < i; ++j)
            if (tracks_[i].info.id == tracks_[j].info.id) return Fail(ParseError::InvalidTable);
    for (std::uint32_t i = 0; i < trex_count_; ++i) {
        const auto index = FindTrack(trex_[i].id);
        if (index < 0) return Fail(ParseError::InvalidTable);
        auto& track = tracks_[index];
        track.has_trex = true;
        track.default_duration = trex_[i].duration;
        track.default_size = trex_[i].bytes;
        track.default_flags = trex_[i].flags;
    }
    if (fragmented_ && skipped_tracks_) return Fail(ParseError::LimitExceeded); // Their fragments could not be told apart.
    for (std::uint32_t i = 0; i < track_count_; ++i) {
        auto& track = tracks_[i];
        if (!FinishTrack(track) && phase_ == Phase::Failed) return false;
        if (!Usable(track)) continue;
        std::int64_t edit = 0;
        if (track.edit > INT64_MAX || !To100ns(static_cast<std::int64_t>(track.edit), track.info.timescale, edit)) { (void)Refuse(track, ParseError::InvalidTimestamp); continue; }
        track.info.duration_100ns = track.info.duration_100ns > edit ? track.info.duration_100ns - edit : 0;
        if (track.edit_delay) {
            // The empty edits' delay, movie ticks, into the track's ticks; the shift becomes media_time - delay (negative when the
            // delay is the larger): every sample then presents that much later, and the track's end with it.
            std::int64_t delay = 0;
            if (track.edit_delay > INT64_MAX || !To100ns(static_cast<std::int64_t>(track.edit_delay), movie_timescale_, delay)) { (void)Refuse(track, ParseError::InvalidTimestamp); continue; }
            // delay x timescale / movie timescale, rounded, without overflow: the remainder's product fits 64 bits (both 32-bit).
            const std::uint64_t whole = track.edit_delay / movie_timescale_, part = track.edit_delay % movie_timescale_;
            if (whole > static_cast<std::uint64_t>(INT64_MAX / 2) / track.info.timescale) { (void)Refuse(track, ParseError::InvalidTimestamp); continue; }
            const auto ticks = static_cast<std::int64_t>(whole * track.info.timescale + (part * track.info.timescale + movie_timescale_ / 2) / movie_timescale_);
            track.edit = static_cast<std::uint64_t>(static_cast<std::int64_t>(track.edit) - ticks);
            track.info.edit_delay_100ns = delay;
            if (track.info.duration_100ns) track.info.duration_100ns += delay;
        }
        track.emit = track.info.kind == TrackKind::Video || track.info.kind == TrackKind::Audio;
    }
    if (!AnyUsable()) return false;
    if (fragmented_) {
        // No classic tables: the pool is split evenly into each track's window for one fragment's samples.
        const auto start = (pool_used_ + 7) & ~std::uint64_t{7};
        const auto share = start < pool_bytes_ ? (pool_bytes_ - start) / track_count_ / sizeof(Sample) : 0;
        const auto capacity = static_cast<std::uint32_t>((std::min)(share, std::uint64_t{UINT32_MAX}));
        for (std::uint32_t i = 0; i < track_count_; ++i) {
            tracks_[i].fragment = static_cast<Sample*>(Reserve(std::uint64_t{capacity} * sizeof(Sample)));
            tracks_[i].fragment_capacity = capacity;
            if (!capacity || !tracks_[i].fragment) return Fail(ParseError::LimitExceeded);
        }
    }
    movie_ready_ = true;
    return true;
}

bool IsobmffParser::Refuse(TrackState& track, const ParseError error) noexcept {
    if (Usable(track)) {
        track.refused = {error, cursor_, box_type_}; track.info.refusal = error;
        track.info.refusal_box = box_type_; track.info.refusal_offset = cursor_;
    }
    if (emit_track_ >= 0 && &tracks_[emit_track_] == &track) { DropView(); emit_track_ = -1; }
    track.emit = false;
    return false;
}
bool IsobmffParser::Refuse(const ParseError error) noexcept {
    // Inside a trak of the movie box the track is refused; anywhere else (a fragment, the movie itself) the file.
    if (current_track_ < 0 || movie_ready_) return Fail(error);
    return Refuse(tracks_[current_track_], error);
}
bool IsobmffParser::AnyUsable() noexcept {
    for (std::uint32_t t = 0; t < track_count_; ++t) if (Usable(tracks_[t])) return true;
    for (std::uint32_t t = 0; t < track_count_; ++t)
        if (!Usable(tracks_[t])) { (void)Fail(tracks_[t].refused.error); diagnostic_ = tracks_[t].refused; return false; }
    return Fail(ParseError::BadBox);
}

bool IsobmffParser::SamplesCovered() const noexcept {
    // Fragments: every sample of the current fragment lies in an mdat scanned since its moof.
    for (std::uint32_t t = 0; t < track_count_; ++t) {
        auto& track = tracks_[t];
        if (!Usable(track)) continue;
        for (std::uint32_t i = 0; i < track.sample_count; ++i)
            if (!Covered(track, track.fragment[i])) return false;
    }
    return true;
}

bool IsobmffParser::Covered(TrackState& track, const Sample& sample) const noexcept {
    // Consecutive samples mostly share an mdat: the search starts at the one that held this track's last sample.
    for (std::uint32_t n = 0; n < mdat_count_; ++n) {
        const auto m = (track.mdat + n) % mdat_count_;
        const auto& range = mdats_[m];
        if (sample.offset >= range.offset && sample.offset - range.offset <= range.bytes &&
            sample.bytes <= range.bytes - (sample.offset - range.offset)) { track.mdat = m; return true; }
    }
    return false;
}

bool IsobmffParser::PrepareEmit() noexcept {
    for (std::uint32_t t = 0; t < track_count_; ++t) Start(tracks_[t]);
    validate_track_ = 0; DropView(); phase_ = Phase::Validate;
    return true;
}

ParseStatus IsobmffParser::Validate() noexcept {
    // Every sample is checked once before any is emitted, in bounded slices so that a multi-hour index never stalls
    // the caller's loop. Emission and seeking then rely on these proofs.
    std::uint32_t budget = kValidateSlice;
    for (; validate_track_ < track_count_; ++validate_track_) {
        auto& track = tracks_[validate_track_];
        const auto edit = static_cast<std::int64_t>(track.edit);
        // A sample that fails a proof refuses its track in a classic file; a fragment's cannot (playback has begun).
        const auto reject = [&](ParseError error) noexcept { if (fragmented_) (void)Fail(error); else (void)Refuse(track, error); };
        while (Usable(track) && track.cursor.sample < track.sample_count) {
            if (budget-- == 0) return ParseStatus::Progress;
            const auto sample = Current(track);
            // Coverage also bounds offset + bytes by the file size, so advancing the cursor cannot overflow.
            if (!Covered(track, sample)) { reject(ParseError::InvalidSampleRange); break; }
            if (sample.bytes > source_->MaxRangeBytes()) { reject(ParseError::LimitExceeded); break; }
            if (sample.dts > INT64_MAX || (sample.composition > 0 &&
                sample.dts > static_cast<std::uint64_t>(INT64_MAX - sample.composition))) { reject(ParseError::InvalidTimestamp); break; }
            const auto dts = static_cast<std::int64_t>(sample.dts) - edit;
            const auto pts = static_cast<std::int64_t>(sample.dts) + sample.composition - edit;
            std::int64_t time = 0;
            if (!To100ns(dts, track.info.timescale, time) ||
                !To100ns(pts, track.info.timescale, time) ||
                !To100ns(sample.duration, track.info.timescale, time)) { reject(ParseError::InvalidTimestamp); break; }
            Advance(track);
        }
        if (phase_ == Phase::Failed) return ParseStatus::Failed;
    }
    if (!AnyUsable()) return ParseStatus::Failed;
    for (std::uint32_t t = 0; t < track_count_; ++t) Start(tracks_[t]);
    emit_track_ = -1; phase_ = Phase::Emit;
    return ParseStatus::Progress;
}

IsobmffParser::Sample IsobmffParser::Current(const TrackState& track) const noexcept {
    const auto& at = track.cursor;
    if (fragmented_) return track.fragment[at.sample];
    Sample sample{};
    sample.offset = at.offset; sample.dts = at.dts;
    sample.bytes = track.sizes ? track.sizes[at.sample] : track.fixed_size;
    sample.duration = track.timing[at.timing].value;
    if (track.composition_count) {
        const auto value = track.composition[at.composition].value;
        sample.composition = track.composition_signed ? Signed32(value) : static_cast<std::int64_t>(value);
    }
    sample.key = !(track.seen & Stss) || (at.sync < track.sync_count && track.sync[at.sync] == at.sample + std::uint64_t{1});
    return sample;
}

void IsobmffParser::Advance(TrackState& track) const noexcept {
    auto& at = track.cursor;
    if (fragmented_) { ++at.sample; return; }
    if ((track.seen & Stss) && at.sync < track.sync_count && track.sync[at.sync] == at.sample + std::uint64_t{1}) ++at.sync;
    at.offset += track.sizes ? track.sizes[at.sample] : track.fixed_size;
    at.dts += track.timing[at.timing].value;
    if (++at.sample == track.sample_count) return;
    if (--at.timing_left == 0) at.timing_left = track.timing[++at.timing].count;
    if (track.composition_count && --at.composition_left == 0) at.composition_left = track.composition[++at.composition].count;
    if (--at.chunk_left == 0) {
        // stsc numbers chunks from 1: the next run starts at 1-based chunk (at.chunk + 1) + 1.
        if (at.mapping + 1 < track.mapping_count && track.mapping[at.mapping + 1].first_chunk == at.chunk + std::uint64_t{2}) ++at.mapping;
        ++at.chunk;
        at.chunk_left = track.mapping[at.mapping].per_chunk;
        at.offset = ChunkOffset(track, at.chunk);
    }
}

void IsobmffParser::Locate(const TrackState& track, const std::uint32_t sample, Cursor& at, const bool offset) const noexcept {
    // Runs: the last mark at or before the sample (the first mark is sample 0), then at most kMarkRows runs forward.
    const auto mark = [sample](const TimeMark* marks, const std::uint32_t rows) noexcept {
        std::uint32_t low = 0, high = (rows + kMarkRows - 1) / kMarkRows;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (marks[middle].sample <= sample) low = middle + 1; else high = middle;
        }
        return low - 1;
    };
    at = {}; at.sample = sample;
    auto index = mark(track.timing_marks, track.timing_count);
    auto row = index * kMarkRows;
    auto first = track.timing_marks[index].sample;
    auto dts = track.timing_marks[index].dts;
    while (sample - first >= track.timing[row].count) {
        dts += std::uint64_t{track.timing[row].count} * track.timing[row].value;
        first += track.timing[row++].count;
    }
    at.timing = row; at.timing_left = track.timing[row].count - (sample - first);
    at.dts = dts + std::uint64_t{sample - first} * track.timing[row].value;
    if (track.composition_count) {
        index = mark(track.composition_marks, track.composition_count);
        row = index * kMarkRows; first = track.composition_marks[index].sample;
        while (sample - first >= track.composition[row].count) first += track.composition[row++].count;
        at.composition = row; at.composition_left = track.composition[row].count - (sample - first);
    }
    std::uint32_t low = 0, high = track.sync_count; // First sync entry at or after the sample (numbers are 1-based).
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (track.sync[middle] < sample + std::uint64_t{1}) low = middle + 1; else high = middle;
    }
    at.sync = low;
    if (!offset) return;
    // Bytes: the stsc run holding the sample, its chunk, then the sizes before it in that chunk.
    low = 0; high = track.mapping_count;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (track.mapping[middle].first_sample <= sample) low = middle + 1; else high = middle;
    }
    const auto& run = track.mapping[low - 1];
    const auto relative = sample - run.first_sample, within = relative % run.per_chunk;
    at.mapping = low - 1; at.chunk = run.first_chunk - 1 + relative / run.per_chunk; at.chunk_left = run.per_chunk - within;
    at.offset = ChunkOffset(track, at.chunk);
    if (!track.sizes) at.offset += std::uint64_t{within} * track.fixed_size;
    else for (auto i = sample - within; i < sample; ++i) at.offset += track.sizes[i];
}

void IsobmffParser::Start(TrackState& track) const noexcept {
    track.cursor = {};
    if (!fragmented_ && track.sample_count && Usable(track)) Locate(track, 0, track.cursor, true); // A refused track's tables may be partial.
}

ParseStatus IsobmffParser::Emit(void* context, const Publish publish) noexcept {
    if (emit_track_ < 0) {
        std::int64_t earliest = INT64_MAX;
        for (std::uint32_t t = 0; t < track_count_; ++t) {
            const auto& track = tracks_[t];
            if (!track.emit || track.cursor.sample == track.sample_count) continue;
            std::int64_t time = 0;
            (void)To100ns(static_cast<std::int64_t>(Current(track).dts) - static_cast<std::int64_t>(track.edit),
                          track.info.timescale, time);
            if (emit_track_ < 0 || time < earliest) { earliest = time; emit_track_ = static_cast<std::int32_t>(t); }
        }
        if (emit_track_ < 0) {
            DropView();
            if (fragmented_) { phase_ = Phase::Scan; fragment_pending_ = false; return ParseStatus::Progress; }
            phase_ = Phase::Done; return ParseStatus::Done;
        }
    }
    auto& track = tracks_[emit_track_];
    const auto sample = Current(track);
    if (View(sample.offset, sample.bytes) == nullptr) return wait_status_;
    core::MediaPacket packet{};
    // The edit shift is applied to timestamps only; payload bytes are never touched or copied.
    const auto edit = static_cast<std::int64_t>(track.edit);
    (void)To100ns(static_cast<std::int64_t>(sample.dts) - edit, track.info.timescale, packet.dts_100ns);
    (void)To100ns(static_cast<std::int64_t>(sample.dts) + sample.composition - edit, track.info.timescale, packet.pts_100ns);
    // Endpoint subtraction avoids cumulative rounding drift at rates like 30000/1001.
    std::int64_t end = 0;
    if (sample.dts > static_cast<std::uint64_t>(INT64_MAX) - sample.duration ||
        !To100ns(static_cast<std::int64_t>(sample.dts + sample.duration) - edit, track.info.timescale, end)) {
        (void)Fail(ParseError::InvalidTimestamp); return ParseStatus::Failed;
    }
    packet.duration_100ns = end - packet.dts_100ns;
    packet.payload_offset = sample.offset - view_.file_offset;
    packet.payload_bytes = sample.bytes;
    packet.payload_block_id = token_.id; packet.payload_generation = token_.generation;
    packet.stream_id = track.info.id; packet.codec_config_id = track.info.config_id;
    packet.playback_epoch = limits_.playback_epoch;
    packet.flags = sample.key ? core::PacketFlags::KeyFrame : core::PacketFlags::None;
    if (!publish(context, packet)) return ParseStatus::Backpressure;
    // Lease moves downstream; no Release here and no payload copy anywhere.
    requested_ = pending_ = false; token_ = {}; view_ = {};
    Advance(track); emit_track_ = -1;
    return ParseStatus::PacketReady;
}

IsobmffParser::~IsobmffParser() noexcept { DropView(); }

bool IsobmffParser::Initialize(core::LinearMemoryArena& arena, platform::IBlockSource& source,
                               const ParserLimits limits) noexcept {
    if (source_ != nullptr || limits.tracks == 0 || limits.tracks > 16 || limits.index_bytes == 0 ||
        static_cast<std::size_t>(limits.index_bytes) != limits.index_bytes || arena.IsSealed())
        return false;
    tracks_ = AllocateArray<TrackState>(arena, limits.tracks);
    pool_ = static_cast<std::byte*>(arena.Allocate(static_cast<std::size_t>(limits.index_bytes)));
    if (tracks_ == nullptr || pool_ == nullptr) return false;
    pool_bytes_ = limits.index_bytes; pool_used_ = 0;
    limits_ = limits; first_epoch_ = limits.playback_epoch;
    source_ = &source;
    stack_[0] = {source.Size(), 0, -1};
    depth_ = 1;
    return true;
}

void* IsobmffParser::Reserve(const std::uint64_t bytes) noexcept {
    const auto start = (pool_used_ + 7) & ~std::uint64_t{7};
    if (start > pool_bytes_ || bytes > pool_bytes_ - start) return nullptr;
    pool_used_ = start + bytes;
    return pool_ + start;
}

const TrackInfo* IsobmffParser::Track(const std::uint32_t index) const noexcept {
    return index < track_count_ ? &tracks_[index].info : nullptr;
}

bool IsobmffParser::ReturnPacket(const core::MediaPacket& packet) noexcept {
    return source_ != nullptr && packet.playback_epoch >= first_epoch_ && packet.playback_epoch <= limits_.playback_epoch &&
        source_->Release({packet.payload_block_id, packet.payload_generation});
}

std::int64_t IsobmffParser::Presentation(const TrackState& track, const std::uint32_t sample) const noexcept {
    // Validation proved every sample's PTS converts without overflow.
    Cursor at{};
    Locate(track, sample, at, false);
    std::int64_t composition = 0;
    if (track.composition_count) {
        const auto value = track.composition[at.composition].value;
        composition = track.composition_signed ? Signed32(value) : static_cast<std::int64_t>(value);
    }
    std::int64_t pts = 0;
    (void)To100ns(static_cast<std::int64_t>(at.dts) + composition - static_cast<std::int64_t>(track.edit), track.info.timescale, pts);
    return pts;
}

bool IsobmffParser::Seek(const std::int64_t target_100ns, const std::uint32_t epoch, const std::uint32_t audio_preroll) noexcept {
    if (!Seekable() || epoch <= limits_.playback_epoch) return false;
    return Relocate(target_100ns, epoch, audio_preroll);
}

bool IsobmffParser::Reposition(const std::int64_t target_100ns, const std::uint32_t audio_preroll) noexcept {
    return Seekable() && Relocate(target_100ns, limits_.playback_epoch, audio_preroll);
}

bool IsobmffParser::SetEmission(const std::uint32_t index, const bool emit) noexcept {
    if (index >= track_count_ || !movie_ready_ || !Usable(tracks_[index])) return false;
    auto& track = tracks_[index];
    if (track.emit == emit) return true;
    track.emit = emit;
    if (!emit && emit_track_ == static_cast<std::int32_t>(index)) { DropView(); emit_track_ = -1; }
    if (emit && phase_ == Phase::Done && track.cursor.sample < track.sample_count) phase_ = Phase::Emit;
    return true;
}
bool IsobmffParser::Emitting(const std::uint32_t index) const noexcept { return index < track_count_ && tracks_[index].emit; }
bool IsobmffParser::SwitchTrack(const std::uint32_t from, const std::uint32_t to, const std::int64_t at_100ns, const std::uint32_t audio_preroll) noexcept {
    if (!Seekable() || from >= track_count_ || to >= track_count_ || from == to || !Usable(tracks_[to]) || !tracks_[from].emit) return false;
    auto& next = tracks_[to];
    tracks_[from].emit = false;
    if (next.sample_count) Position(next, at_100ns, audio_preroll);
    next.emit = true;
    // Whatever sample was chosen next is chosen again: the new track's first sample may precede it.
    DropView(); emit_track_ = -1;
    if (phase_ == Phase::Done && next.cursor.sample < next.sample_count) phase_ = Phase::Emit;
    return true;
}
bool IsobmffParser::SampleAt(const std::uint32_t index, const std::uint32_t sample, SampleRecord& out) const noexcept {
    if (!Seekable() || index >= track_count_) return false;
    const auto& track = tracks_[index];
    if (!Usable(track) || sample >= track.sample_count) return false;
    Cursor at{}; Locate(track, sample, at, true);
    const auto& t = track; Sample s{};
    s.offset = at.offset; s.dts = at.dts; s.bytes = t.sizes ? t.sizes[sample] : t.fixed_size; s.duration = t.timing[at.timing].value;
    if (t.composition_count) {
        const auto value = t.composition[at.composition].value;
        s.composition = t.composition_signed ? Signed32(value) : static_cast<std::int64_t>(value);
    }
    s.key = !(t.seen & Stss) || (at.sync < t.sync_count && t.sync[at.sync] == sample + std::uint64_t{1});
    // Validation proved these conversions.
    const auto edit = static_cast<std::int64_t>(t.edit);
    std::int64_t end = 0;
    (void)To100ns(static_cast<std::int64_t>(s.dts) + s.composition - edit, t.info.timescale, out.pts_100ns);
    (void)To100ns(static_cast<std::int64_t>(s.dts + s.duration) + s.composition - edit, t.info.timescale, end);
    out.offset = s.offset; out.bytes = s.bytes; out.duration_100ns = end - out.pts_100ns; out.key = s.key;
    return true;
}
const char* IsobmffParser::Name(const ParseError error) noexcept {
    static constexpr const char* names[]{"None", "NotInitialized", "BadBox", "Truncated", "Unsupported", "LimitExceeded",
                                         "InvalidTable", "InvalidCodec", "InvalidTimestamp", "InvalidSampleRange", "IoFailure"};
    return static_cast<unsigned>(error) < std::size(names) ? names[static_cast<unsigned>(error)] : "?";
}

void IsobmffParser::Position(TrackState& track, const std::int64_t target_100ns, const std::uint32_t audio_preroll) const noexcept {
    auto index = StartSample(track, target_100ns);
    if (track.info.codec == Codec::Aac || track.info.codec == Codec::Opus) index -= (std::min)(index, audio_preroll);
    Locate(track, index, track.cursor, true); // O(log n) table lookups; no I/O and no allocation.
}

std::uint32_t IsobmffParser::StartSample(const TrackState& track, const std::int64_t target_100ns) const noexcept {
    std::uint32_t index = 0;
    if ((track.seen & Stss) && track.sync_count) {
        // Binary search over the sync table (sample numbers ascending, key PTS nondecreasing in practice).
        std::uint32_t low = 0, high = track.sync_count; // First sync entry presenting after the target.
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (Presentation(track, track.sync[middle] - 1) <= target_100ns) low = middle + 1;
            else high = middle;
        }
        index = track.sync[low ? low - 1 : 0] - 1;
    } else {
        // Every sample is a sync sample: binary search decode order (DTS is strictly increasing).
        std::uint32_t low = 0, high = track.sample_count;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (Presentation(track, middle) <= target_100ns) low = middle + 1;
            else high = middle;
        }
        index = low ? low - 1 : 0;
    }
    return index;
}

bool IsobmffParser::Continue(const std::uint32_t video, const std::int64_t target_100ns, const std::uint32_t epoch,
                             const std::uint32_t audio_preroll, std::uint32_t& resumed) noexcept {
    resumed = 0;
    if (!Seekable() || epoch <= limits_.playback_epoch || video >= track_count_) return false;
    auto& track = tracks_[video];
    // Without sync samples every sample is one: a Seek starts at the target itself, there is nothing to carry on from.
    if (!Usable(track) || !track.emit || !(track.seen & Stss) || !track.sync_count || track.info.kind != TrackKind::Video) return false;
    const auto next = track.cursor.sample, sync = StartSample(track, target_100ns);
    if (next >= track.sample_count || sync >= next) return false; // The target's group of pictures has not been reached.
    // Every sample published since the sync sample must end before the target, as its packet's timestamps say (the
    // engine shows a picture when pts > target - duration): one that is still to be shown went out in the old epoch.
    const auto floor = (std::max)(target_100ns, std::int64_t{0});
    const auto edit = static_cast<std::int64_t>(track.edit);
    const auto position = track.cursor;
    Locate(track, sync, track.cursor, true);
    bool shown = false;
    while (!shown && track.cursor.sample < next) {
        const auto sample = Current(track);
        std::int64_t dts = 0, pts = 0, end = 0; // Validation proved these conversions (Emit makes the same ones).
        (void)To100ns(static_cast<std::int64_t>(sample.dts) - edit, track.info.timescale, dts);
        (void)To100ns(static_cast<std::int64_t>(sample.dts) + sample.composition - edit, track.info.timescale, pts);
        (void)To100ns(static_cast<std::int64_t>(sample.dts + sample.duration) - edit, track.info.timescale, end);
        shown = pts > floor - (end - dts);
        Advance(track);
    }
    track.cursor = position;
    if (shown || !SeekOthers(video, target_100ns, epoch, audio_preroll)) return false;
    resumed = next - sync;
    return true;
}

void IsobmffParser::ChapterBoxes(const std::uint32_t type, const std::byte* data, const std::uint32_t bytes) noexcept {
    // Child boxes, as far as they are well formed; a short or zero header (a QuickTime terminator) ends the list.
    for (std::uint32_t at = 0; bytes - at >= 8;) {
        const auto size = U32(data + at), child = U32(data + at + 4);
        if (size < 8 || size > bytes - at) return;
        const auto* body = data + at + 8; const auto length = size - 8;
        if (type == tref && child == chap && current_track_ >= 0) {
            for (std::uint32_t k = 0; k + 4 <= length; k += 4)
                if (const auto id = U32(body + k)) { tracks_[current_track_].info.chapter_track = id; break; }
        }
        if (type == udta && child == chpl && length >= 5 && !chapter_count_) {
            // Nero: version, flags, (version 1: 4 more bytes), a one-byte count, then per chapter a 64-bit start in 100 ns
            // units, a one-byte title length and the title.
            const auto version = std::to_integer<unsigned>(body[0]);
            std::uint32_t p = version ? 8u : 4u;
            if (p >= length) return;
            const auto count = std::to_integer<unsigned>(body[p++]);
            for (unsigned c = 0; c < count && chapter_count_ < kMaxChapters && length - p >= 9; ++c) {
                const auto start = U64(body + p); const auto title = std::to_integer<unsigned>(body[p + 8]); p += 9;
                if (title > length - p || start > static_cast<std::uint64_t>(INT64_MAX)) return;
                auto& entry = chapters_[chapter_count_++];
                entry.start_100ns = static_cast<std::int64_t>(start);
                const auto kept = (std::min)(title, static_cast<unsigned>(sizeof(entry.title) - 1));
                for (unsigned k = 0; k < kept; ++k) entry.title[k] = static_cast<char>(std::to_integer<unsigned char>(body[p + k]));
                entry.title[kept] = 0;
                p += title;
            }
        }
        at += size;
    }
}
std::uint32_t IsobmffParser::SyncCount(const std::uint32_t index) const noexcept {
    if (!Seekable() || index >= track_count_ || !Usable(tracks_[index])) return UINT32_MAX;
    const auto& track = tracks_[index];
    return (track.seen & Stss) ? track.sync_count : track.sample_count;
}
std::uint32_t IsobmffParser::SyncAt(const std::uint32_t index, const std::uint32_t i) const noexcept {
    if (i >= SyncCount(index) || SyncCount(index) == UINT32_MAX) return UINT32_MAX;
    const auto& track = tracks_[index];
    return (track.seen & Stss) ? track.sync[i] - 1 : i;
}
std::uint32_t IsobmffParser::SyncSample(const std::uint32_t index, const std::int64_t target_100ns) const noexcept {
    if (!Seekable() || index >= track_count_ || !Usable(tracks_[index]) || !tracks_[index].sample_count) return UINT32_MAX;
    return StartSample(tracks_[index], target_100ns);
}

std::uint32_t IsobmffParser::DecodeDistance(const std::uint32_t index, const std::uint32_t from, const std::int64_t target_100ns,
                                            const std::uint32_t limit) noexcept {
    const auto cap = (std::min)(limit, UINT32_MAX - 1), none = cap + 1;
    if (!Seekable() || index >= track_count_ || !Usable(tracks_[index]) || from >= tracks_[index].sample_count) return none;
    auto& track = tracks_[index];
    const auto edit = static_cast<std::int64_t>(track.edit);
    const auto position = track.cursor;
    Locate(track, from, track.cursor, true);
    std::uint32_t distance = none;
    for (std::uint32_t k = 0; k <= cap && track.cursor.sample < track.sample_count; ++k) {
        const auto sample = Current(track);
        std::int64_t dts = 0, pts = 0, end = 0; // Validation proved these conversions (Emit makes the same ones).
        (void)To100ns(static_cast<std::int64_t>(sample.dts) - edit, track.info.timescale, dts);
        (void)To100ns(static_cast<std::int64_t>(sample.dts) + sample.composition - edit, track.info.timescale, pts);
        (void)To100ns(static_cast<std::int64_t>(sample.dts + sample.duration) - edit, track.info.timescale, end);
        if (pts <= target_100ns && pts + (end - dts) > target_100ns) { distance = k; break; }
        Advance(track);
    }
    track.cursor = position;
    return distance;
}

bool IsobmffParser::SeekOthers(const std::uint32_t keep, const std::int64_t target_100ns, const std::uint32_t epoch,
                               const std::uint32_t audio_preroll) noexcept {
    if (!Seekable() || epoch <= limits_.playback_epoch || keep >= track_count_ || !Usable(tracks_[keep]) || !tracks_[keep].emit) return false;
    for (std::uint32_t t = 0; t < track_count_; ++t) {
        auto& other = tracks_[t];
        if (t != keep && other.sample_count && Usable(other)) Position(other, target_100ns, audio_preroll);
    }
    DropView(); // An in-flight read (of any track) is chosen again; the kept cursor only moves when a packet is published.
    emit_track_ = -1; limits_.playback_epoch = epoch; phase_ = Phase::Emit;
    return true;
}

bool IsobmffParser::Relocate(const std::int64_t target_100ns, const std::uint32_t epoch, const std::uint32_t audio_preroll) noexcept {
    for (std::uint32_t t = 0; t < track_count_; ++t) {
        auto& track = tracks_[t];
        // Unpublished usable tracks too: SetEmission may start them from here.
        if (track.sample_count && Usable(track)) Position(track, target_100ns, audio_preroll);
    }
    DropView(); // Releases the block of an unpublished in-flight sample.
    emit_track_ = -1; limits_.playback_epoch = epoch; phase_ = Phase::Emit;
    return true;
}

void IsobmffParser::DropView() noexcept {
    if (requested_ && source_ != nullptr) (void)source_->Release(token_);
    requested_ = false; pending_ = false; view_ = {}; token_ = {};
}

bool IsobmffParser::Fail(const ParseError error) noexcept {
    diagnostic_ = {error, cursor_, box_type_};
    phase_ = Phase::Failed;
    DropView();
    return false;
}

const std::byte* IsobmffParser::View(const std::uint64_t offset, const std::uint32_t bytes) noexcept {
    if (offset > source_->Size() || bytes > source_->Size() - offset) {
        (void)Fail(ParseError::Truncated); return nullptr;
    }
    if (pending_) {
        const auto status = source_->Poll(token_, view_);
        if (status == platform::IoStatus::Pending) { wait_status_ = ParseStatus::NeedInput; return nullptr; }
        if (status != platform::IoStatus::Ready) { (void)Fail(ParseError::IoFailure); return nullptr; }
        if (view_.data == nullptr || view_.file_offset > requested_offset_ ||
            requested_offset_ - view_.file_offset > view_.valid_bytes ||
            requested_bytes_ > view_.valid_bytes - (requested_offset_ - view_.file_offset) ||
            view_.file_offset > source_->Size() || view_.valid_bytes > source_->Size() - view_.file_offset ||
            view_.token.id != token_.id || view_.token.generation != token_.generation) {
            (void)Fail(ParseError::Truncated); return nullptr;
        }
        pending_ = false;
    }
    if (requested_ && offset >= view_.file_offset) {
        const auto within = offset - view_.file_offset;
        if (within <= view_.valid_bytes && bytes <= view_.valid_bytes - within)
            return view_.data + static_cast<std::size_t>(within);
    }
    DropView();
    const auto status = source_->Request(offset, bytes, token_);
    if (status == platform::IoStatus::WouldBlock) { wait_status_ = ParseStatus::Backpressure; return nullptr; }
    if (status != platform::IoStatus::Pending) {
        (void)Fail(status == platform::IoStatus::TooLarge ? ParseError::LimitExceeded : ParseError::IoFailure);
        return nullptr;
    }
    requested_ = pending_ = true;
    requested_offset_ = offset; requested_bytes_ = bytes;
    wait_status_ = ParseStatus::NeedInput;
    return nullptr;
}

ParseStatus IsobmffParser::StepImpl(void* output, const Publish publish) noexcept {
    if (source_ == nullptr) { (void)Fail(ParseError::NotInitialized); return ParseStatus::Failed; }
    if (phase_ == Phase::Failed) return ParseStatus::Failed;
    if (phase_ == Phase::Done) return ParseStatus::Done;
    const auto status = phase_ == Phase::Emit ? Emit(output, publish) : phase_ == Phase::Validate ? Validate() : Scan();
    return phase_ == Phase::Failed ? ParseStatus::Failed : status;
}

std::int32_t IsobmffParser::FindTrack(const std::uint32_t id) const noexcept {
    for (std::uint32_t i = 0; i < track_count_; ++i)
        if (tracks_[i].info.id == id) return static_cast<std::int32_t>(i);
    return -1;
}

ParseStatus IsobmffParser::Scan() noexcept {
    if (!box_loaded_) {
        if (cursor_ == stack_[depth_ - 1].end) {
            if (depth_ == 1) {
                if (!movie_ready_ || !ftyp_seen_ || fragment_pending_) {
                    (void)Fail(ParseError::BadBox); return ParseStatus::Failed;
                }
                if (!fragmented_) {
                    if (!PrepareEmit()) return ParseStatus::Failed;
                    return ParseStatus::Progress;
                }
                DropView(); phase_ = Phase::Done; return ParseStatus::Done;
            }
            const auto closed = stack_[--depth_].type;
            if (closed == moov && !FinishMovie()) return ParseStatus::Failed;
            if (closed == traf) {
                if (!tfhd_seen_ || !tfdt_seen_ || !trun_seen_) {
                    (void)Fail(ParseError::BadBox); return ParseStatus::Failed;
                }
                tracks_[current_track_].next_dts = fragment_dts_;
            }
            if (closed == moof) {
                if (!mfhd_seen_ || !fragment_has_traf_) {
                    (void)Fail(ParseError::BadBox); return ParseStatus::Failed;
                }
                fragment_pending_ = true;
            }
            current_track_ = stack_[depth_ - 1].track;
            return ParseStatus::Progress;
        }
        if (cursor_ > stack_[depth_ - 1].end || stack_[depth_ - 1].end - cursor_ < 8) {
            (void)Fail(ParseError::Truncated); return ParseStatus::Failed;
        }
        const auto* header = View(cursor_, 8);
        if (!header) return wait_status_;
        std::uint64_t size = U32(header);
        box_type_ = U32(header + 4);
        std::uint32_t header_bytes = 8;
        if (size == 1) {
            if (stack_[depth_ - 1].end - cursor_ < 16) {
                (void)Fail(ParseError::Truncated); return ParseStatus::Failed;
            }
            header = View(cursor_, 16);
            if (!header) return wait_status_;
            size = U64(header + 8); header_bytes = 16;
        } else if (size == 0) {
            if (depth_ != 1) { (void)Fail(ParseError::Unsupported); return ParseStatus::Failed; }
            size = source_->Size() - cursor_;
        }
        if (size < header_bytes || size > stack_[depth_ - 1].end - cursor_) {
            (void)Fail(ParseError::BadBox); return ParseStatus::Failed;
        }
        box_end_ = cursor_ + size; box_payload_ = cursor_ + header_bytes;
        box_loaded_ = true;
    }
    const auto parent = stack_[depth_ - 1].type;
    const bool container = box_type_ == moov || box_type_ == trak || box_type_ == mdia ||
        box_type_ == minf || box_type_ == stbl || box_type_ == mvex || box_type_ == moof ||
        box_type_ == traf || box_type_ == dinf || box_type_ == edts;
    if (container) {
        bool valid = false;
        switch (box_type_) {
        case moov:
            valid = parent == 0 && !moov_seen_; moov_seen_ = true; break;
        case trak:
            if (parent == moov && track_count_ == limits_.tracks) { // More tracks than catalogued: this one is skipped.
                ++skipped_tracks_; cursor_ = box_end_; box_loaded_ = false; return ParseStatus::Progress;
            }
            valid = parent == moov;
            if (valid) current_track_ = static_cast<std::int32_t>(track_count_++);
            break;
        case mdia: valid = parent == trak; break;
        case minf: valid = parent == mdia; break;
        case stbl: valid = parent == minf; break;
        case dinf: valid = parent == minf; break;
        case edts: valid = parent == trak; break;
        case mvex: valid = parent == moov && !fragmented_; fragmented_ = true; break;
        case moof:
            valid = parent == 0 && movie_ready_ && fragmented_ && !fragment_pending_;
            if (valid) {
                moof_offset_ = cursor_; mfhd_seen_ = fragment_has_traf_ = false; mdat_count_ = 0;
                for (std::uint32_t i = 0; i < track_count_; ++i) {
                    tracks_[i].sample_count = 0; tracks_[i].cursor = {}; tracks_[i].fragment_seen = false;
                }
            }
            break;
        case traf:
            valid = parent == moof;
            tfhd_seen_ = tfdt_seen_ = trun_seen_ = false; current_track_ = -1; break;
        default: break;
        }
        if (!valid || depth_ == 16) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
        stack_[depth_++] = {box_end_, box_type_, current_track_};
        cursor_ = box_payload_; box_loaded_ = false;
        return ParseStatus::Progress;
    }
    if (box_type_ == mdat) {
        if (parent != 0 || mdat_count_ == 64) { (void)Fail(ParseError::LimitExceeded); return ParseStatus::Failed; }
        mdats_[mdat_count_++] = {box_payload_, box_end_ - box_payload_};
        cursor_ = box_end_; box_loaded_ = false;
        if (fragment_pending_ && SamplesCovered()) {
            if (!PrepareEmit()) return ParseStatus::Failed;
        }
        return ParseStatus::Progress;
    }
    // Reject extensions that change interpretation instead of silently misdemuxing.
    if (box_type_ == Tag('s','t','z','2') ||
        box_type_ == Tag('s','e','n','c') || box_type_ == Tag('s','a','i','z') ||
        box_type_ == Tag('s','a','i','o')) {
        if (!Refuse(ParseError::Unsupported) && phase_ == Phase::Failed) return ParseStatus::Failed;
        cursor_ = box_end_; box_loaded_ = false; return ParseStatus::Progress; // Only this track is refused.
    }
    // A refused track keeps its catalogue boxes (tkhd, mdhd, hdlr); the rest of it is not read.
    const bool refused = !movie_ready_ && current_track_ >= 0 && !Usable(tracks_[current_track_]) &&
                         box_type_ != tkhd && box_type_ != mdhd && box_type_ != hdlr && box_type_ != stsd;
    if (refused) { cursor_ = box_end_; box_loaded_ = false; return ParseStatus::Progress; }
    // Chapters (docs/chapters.md): moov/udta (Nero chpl) and trak/tref (a QuickTime chapter track), read whole when small.
    // Their content is never a reason to refuse anything: a QuickTime udta may even end in a 4-byte terminator.
    if ((box_type_ == udta && parent == moov) || (box_type_ == tref && parent == trak && current_track_ >= 0)) {
        const auto length = box_end_ - box_payload_;
        if (length >= 8 && length <= kChapterBoxBytes && length <= source_->MaxRangeBytes()) {
            const auto* bytes = View(box_payload_, static_cast<std::uint32_t>(length));
            if (!bytes) return wait_status_;
            ChapterBoxes(box_type_, bytes, static_cast<std::uint32_t>(length));
        }
        cursor_ = box_end_; box_loaded_ = false; return ParseStatus::Progress;
    }
    if (IsTable(box_type_)) return Table();
    const bool known = box_type_ == ftyp || box_type_ == mvhd || box_type_ == tkhd ||
        box_type_ == mdhd || box_type_ == hdlr || box_type_ == stsd || box_type_ == trex || box_type_ == mfhd ||
        box_type_ == tfhd || box_type_ == tfdt || box_type_ == trun || box_type_ == dref || box_type_ == elst ||
        box_type_ == sgpd || box_type_ == sbgp;
    if (known) {
        const auto length = box_end_ - box_payload_;
        if (length > source_->MaxRangeBytes() || length > UINT32_MAX) {
            (void)Fail(ParseError::LimitExceeded); return ParseStatus::Failed;
        }
        if (length == 0) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
        const auto* bytes = View(box_payload_, static_cast<std::uint32_t>(length));
        if (!bytes) return wait_status_;
        if (!Leaf(box_type_, bytes, static_cast<std::uint32_t>(length), box_payload_) && phase_ == Phase::Failed)
            return ParseStatus::Failed; // Otherwise the box's track was refused and the scan goes on.
    }
    cursor_ = box_end_; box_loaded_ = false;
    return ParseStatus::Progress;
}

bool IsobmffParser::Leaf(const std::uint32_t type, const std::byte* data,
                         const std::uint32_t bytes, const std::uint64_t offset) noexcept {
    const auto parent = stack_[depth_ - 1].type;
    Reader r{data, bytes};
    const auto full = r.Read32();
    const auto version = full >> 24;
    const auto flags = full & 0xffffff;
    if (!r.ok) return Fail(ParseError::Truncated);
    if (type == ftyp) {
        if (parent != 0 || ftyp_seen_ || bytes < 8 || bytes % 4 != 0) return Fail(ParseError::BadBox);
        // Brand is informational; actual supported structures decide admission.
        ftyp_seen_ = true; return true;
    }
    if (type == mvhd) {
        if (parent != moov || mvhd_seen_ || version > 1 || flags != 0 || bytes < (version ? 112u : 100u))
            return Fail(ParseError::BadBox);
        movie_timescale_ = U32(data + (version ? 20 : 12));
        mvhd_seen_ = true;
        return movie_timescale_ != 0 || Fail(ParseError::InvalidTimestamp);
    }
    if (type == trex) {
        if (parent != mvex || full != 0 || bytes != 24 || trex_count_ == 16) return Fail(ParseError::BadBox);
        const Trex entry{r.Read32(), r.Read32(), r.Read32(), r.Read32(), r.Read32()};
        if (!entry.id || entry.description != 1) return Fail(ParseError::Unsupported);
        for (std::uint32_t i = 0; i < trex_count_; ++i)
            if (trex_[i].id == entry.id) return Fail(ParseError::BadBox);
        trex_[trex_count_++] = entry; return true;
    }
    if (type == mfhd) {
        if (parent != moof || full != 0 || bytes != 8 || mfhd_seen_) return Fail(ParseError::BadBox);
        const auto sequence = r.Read32();
        if (sequence_seen_ && sequence <= last_sequence_) return Fail(ParseError::InvalidTable);
        last_sequence_ = sequence; sequence_seen_ = mfhd_seen_ = true; return true;
    }
    if (type == tfhd) {
        if (parent != traf || tfhd_seen_ || version != 0 || (flags & ~0x02003bu) != 0)
            return Fail(ParseError::Unsupported);
        current_track_ = FindTrack(r.Read32());
        if (current_track_ < 0 || tracks_[current_track_].fragment_seen) return Fail(ParseError::InvalidTable);
        auto& track = tracks_[current_track_];
        if (!track.has_trex) return Fail(ParseError::InvalidTable);
        if ((flags & 1) && (flags & 0x20000)) return Fail(ParseError::BadBox);
        if (flags & 1) fragment_base_ = r.Read64();
        else if ((flags & 0x20000) || !fragment_has_traf_) fragment_base_ = moof_offset_;
        else return Fail(ParseError::Unsupported); // Ambiguous implicit subsequent traf base.
        if ((flags & 2) && r.Read32() != 1) return Fail(ParseError::Unsupported);
        fragment_duration_ = flags & 8 ? r.Read32() : track.default_duration;
        fragment_size_ = flags & 16 ? r.Read32() : track.default_size;
        fragment_flags_ = flags & 32 ? r.Read32() : track.default_flags;
        if (!r.ok || r.position != bytes) return Fail(ParseError::Truncated);
        run_end_ = fragment_base_;
        tfhd_seen_ = fragment_has_traf_ = track.fragment_seen = true;
        stack_[depth_ - 1].track = current_track_;
        return true;
    }
    if (current_track_ < 0) return Fail(ParseError::BadBox);
    auto& track = tracks_[current_track_];
    if (type == tfdt) {
        if (parent != traf || !tfhd_seen_ || tfdt_seen_ || trun_seen_ || version > 1 || flags != 0 ||
            bytes != (version ? 12u : 8u)) return Fail(ParseError::BadBox);
        fragment_dts_ = version ? r.Read64() : r.Read32();
        if (fragment_dts_ < track.next_dts || fragment_dts_ > INT64_MAX) return Fail(ParseError::InvalidTimestamp);
        tfdt_seen_ = true; return true;
    }
    if (type == trun) {
        if (parent != traf || !tfdt_seen_ || version > 1 || (flags & ~0xf05u) != 0 ||
            ((flags & 4) && (flags & 0x400))) return Fail(ParseError::Unsupported);
        const auto count = r.Read32();
        if (count > track.fragment_capacity - track.sample_count) return Fail(ParseError::LimitExceeded);
        std::uint64_t position = run_end_;
        if (flags & 1) {
            if (!SignedOffset(fragment_base_, Signed32(r.Read32()), position)) return Fail(ParseError::InvalidSampleRange);
        }
        const auto first_flags = flags & 4 ? r.Read32() : fragment_flags_;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto& sample = track.fragment[track.sample_count++];
            sample = {};
            sample.duration = flags & 0x100 ? r.Read32() : fragment_duration_;
            sample.bytes = flags & 0x200 ? r.Read32() : fragment_size_;
            const auto sample_flags = flags & 0x400 ? r.Read32() : (i == 0 ? first_flags : fragment_flags_);
            if (flags & 0x800) {
                const auto cto = r.Read32();
                sample.composition = version ? Signed32(cto) : cto;
            }
            sample.key = (sample_flags & 0x10000) == 0;
            sample.offset = position; sample.dts = fragment_dts_;
            if (!r.ok || sample.bytes == 0 || sample.duration == 0 ||
                !Add(position, sample.bytes, position) ||
                !Add(fragment_dts_, sample.duration, fragment_dts_) || fragment_dts_ > INT64_MAX)
                return Fail(ParseError::InvalidTable);
        }
        if (!r.ok || r.position != bytes) return Fail(ParseError::BadBox);
        run_end_ = position; trun_seen_ = true; return true;
    }
    if (type == sgpd || type == sbgp) {
        // Sample groups are rejected unless they leave the interpretation of every sample unchanged. Admitted: audio
        // roll recovery ('roll', 'prol'), written by common muxers for every AAC and Opus track. It states how many
        // earlier access units prime the decoder, which Seek's audio_preroll provides for every target alike, so
        // the sample-to-group map (sbgp) is not needed. Encryption, random-access variants and video groups fail.
        const auto grouping = r.Read32();
        if (!r.ok) return Fail(ParseError::BadBox);
        if (parent != stbl || version > 1 || flags || track.handler != Tag('s','o','u','n') ||
            (grouping != Tag('r','o','l','l') && grouping != Tag('p','r','o','l'))) return Refuse(ParseError::Unsupported);
        if (type == sbgp) return true;
        if (version == 1 && r.Read32() != 2) return Refuse(ParseError::Unsupported); // default_length: an int16 per entry.
        const auto count = r.Read32();
        if (!r.ok || std::uint64_t{count} * 2 != bytes - r.position) return Fail(ParseError::BadBox);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto distance = static_cast<std::int16_t>(U16(data + r.position + i * 2));
            track.info.roll_distance = (std::min)(track.info.roll_distance, distance);
        }
        return true;
    }
    // QuickTime's data handler (2026-11-04): a second hdlr inside minf ('dhlr': 'url ' or 'alis'), written by QuickTime, iPhones
    // and ffmpeg's .mov muxer. It says how the data is referenced (dref says it too), not what the track is: skipped, where it
    // used to refuse every such track as a duplicate handler.
    if (type == hdlr && parent == minf) return true;
    std::uint32_t bit = 0;
    if (type == tkhd) bit = Tkhd;
    else if (type == mdhd) bit = Mdhd;
    else if (type == hdlr) bit = Hdlr;
    else if (type == stsd) bit = Stsd;
    else if (type == dref) bit = Dref;
    else if (type == elst) bit = Elst;
    if (track.seen & bit) return Refuse(ParseError::InvalidTable);
    track.seen |= bit;
    if (type == tkhd) {
        if (parent != trak || version > 1 || bytes < (version ? 96u : 84u)) return Fail(ParseError::BadBox);
        track.info.id = U32(data + (version ? 20 : 12));
        track.info.enabled = (flags & 1) != 0;
        track.info.alternate_group = U16(data + (version ? 46 : 34));
        // The matrix's linear part: a, b at 0 and 4, c, d at 12 and 16 of the matrix (16.16 fixed point each).
        const auto* m = data + (version ? 52 : 40);
        const std::int32_t terms[4]{static_cast<std::int32_t>(U32(m)), static_cast<std::int32_t>(U32(m + 4)), static_cast<std::int32_t>(U32(m + 12)),
                                    static_cast<std::int32_t>(U32(m + 16))};
        std::int8_t t[4]{};
        bool axes = true;
        for (unsigned i = 0; i < 4; ++i) {
            t[i] = terms[i] == 0x10000 ? 1 : terms[i] == -0x10000 ? -1 : 0;
            if (terms[i] != 0 && !t[i]) axes = false; // A scale or a free angle.
        }
        // A quarter turn or mirror: exactly one non-zero term in each row and column.
        axes = axes && ((t[0] && t[3] && !t[1] && !t[2]) || (!t[0] && !t[3] && t[1] && t[2]));
        if (axes) for (unsigned i = 0; i < 4; ++i) track.info.transform[i] = t[i];
        track.info.transform_ignored = !axes && (terms[0] | terms[1] | terms[2] | terms[3]) != 0; // An all-zero matrix: none written.
        return track.info.id != 0 || Fail(ParseError::InvalidTable);
    }
    if (type == mdhd) {
        if (parent != mdia || version > 1 || flags || bytes < (version ? 36u : 24u)) return Fail(ParseError::BadBox);
        track.info.timescale = U32(data + (version ? 20 : 12));
        Language(U16(data + (version ? 32 : 20)), track.info.language);
        const auto duration = version ? U64(data + 24) : U32(data + 16);
        const bool unknown = duration == (version ? UINT64_MAX : UINT32_MAX);
        if (!unknown && (duration > INT64_MAX || !To100ns(static_cast<std::int64_t>(duration), track.info.timescale, track.info.duration_100ns)))
            return Fail(ParseError::InvalidTimestamp);
        return track.info.timescale != 0 || Fail(ParseError::InvalidTimestamp);
    }
    if (type == hdlr) {
        if (parent != mdia || full || bytes < 24) return Fail(ParseError::BadBox);
        track.handler = track.info.handler = U32(data + 8);
        HandlerName(data + 24, bytes - 24, track.info.name);
        const auto h = track.handler;
        track.info.kind = h == Tag('v','i','d','e') ? TrackKind::Video : h == Tag('s','o','u','n') ? TrackKind::Audio :
                          h == Tag('t','e','x','t') || h == Tag('s','b','t','l') || h == Tag('s','u','b','t') ? TrackKind::Subtitle : TrackKind::Other;
        return track.info.kind != TrackKind::Other || Refuse(ParseError::Unsupported); // Hint, metadata, timecode, chapters, ...
    }
    if (type == dref) {
        // One self-contained reference (flag 1): 'url ' as ISO writes it, or QuickTime's 'alis' (iPhones, Final Cut).
        const auto reference = bytes == 20 ? U32(data + 12) : 0;
        if (parent != dinf || full || bytes != 20 || U32(data + 4) != 1 || U32(data + 8) != 12 ||
            (reference != Tag('u','r','l',' ') && reference != Tag('a','l','i','s')) || U32(data + 16) != 1)
            return Refuse(ParseError::Unsupported); // Media in another file.
        return true;
    }
    if (type == elst) {
        if (parent != edts || version > 1 || flags) return Fail(ParseError::BadBox);
        const auto count = r.Read32();
        const std::uint32_t entry = version ? 20u : 12u;
        if (!r.ok || static_cast<std::uint64_t>(count) * entry != bytes - r.position) return Fail(ParseError::BadBox);
        // Admitted: one normal edit at rate 1.0 (a presentation-timeline shift), after any number of empty edits (media_time
        // -1: the track starts that much later; 2026-11-04, a stream offset as editors, screen recorders and `ffmpeg
        // -itsoffset` write it, which refused everyday files at the open). Dwell and rate edits, a second normal edit
        // and an empty edit after the normal one change interpretation and are rejected.
        if (count == 0) return Refuse(ParseError::Unsupported);
        std::uint64_t delay = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t duration = version ? r.Read64() : r.Read32(); // segment_duration (movie timescale).
            const std::int64_t media_time = version ? static_cast<std::int64_t>(r.Read64()) : Signed32(r.Read32());
            const auto rate = r.Read32();
            if (!r.ok) return Fail(ParseError::BadBox);
            const bool last = i + 1 == count;
            if (media_time == -1 && !last) {
                if (duration > UINT64_MAX / 2 - delay) return Refuse(ParseError::InvalidTimestamp);
                delay += duration; continue;
            }
            if (!last || media_time < 0 || rate != 0x00010000u) return Refuse(ParseError::Unsupported);
            track.edit = static_cast<std::uint64_t>(media_time);
            track.edit_delay = delay;
            track.info.edit_media_time = track.edit;
        }
        return true;
    }
    if (parent != stbl) return Fail(ParseError::BadBox);
    if (!Usable(track)) { if (bytes >= 16) track.info.sample_entry = U32(data + 12); return true; } // Catalogued only.
    if (full) return Refuse(ParseError::Unsupported);
    return Description(track, data, bytes, offset); // stsd: sample tables are read by Table().
}

ParseStatus IsobmffParser::Table() noexcept {
    const auto length = box_end_ - box_payload_;
    if (!table_.active) {
        const std::uint32_t header = box_type_ == stsz ? 12u : 8u;
        if (length < header) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
        const auto* bytes = View(box_payload_, header);
        if (!bytes) return wait_status_;
        if (current_track_ < 0) { (void)Fail(ParseError::BadBox); return ParseStatus::Failed; }
        if (!TableHeader(tracks_[current_track_], bytes, length)) {
            if (phase_ == Phase::Failed) return ParseStatus::Failed;
            table_ = {}; cursor_ = box_end_; box_loaded_ = false; return ParseStatus::Progress; // The track is refused.
        }
    } else {
        // Whole entries per window, so one never straddles two reads.
        const auto per_window = (std::min)(source_->MaxRangeBytes(), kTableWindow) / table_.stride;
        if (!per_window) { (void)Fail(ParseError::LimitExceeded); return ParseStatus::Failed; }
        const auto entries = (std::min)(table_.count - table_.done, per_window);
        const auto* bytes = View(table_.position, entries * table_.stride);
        if (!bytes) return wait_status_;
        if (!TableEntries(tracks_[current_track_], bytes, entries)) {
            if (phase_ == Phase::Failed) return ParseStatus::Failed;
            table_ = {}; cursor_ = box_end_; box_loaded_ = false; return ParseStatus::Progress;
        }
        table_.done += entries; table_.position += std::uint64_t{entries} * table_.stride;
    }
    if (table_.done == table_.count) { table_.active = false; cursor_ = box_end_; box_loaded_ = false; }
    return ParseStatus::Progress;
}

bool IsobmffParser::TableHeader(TrackState& track, const std::byte* data, const std::uint64_t length) noexcept {
    const auto type = box_type_;
    const auto bit = type == stts ? Stts : type == ctts ? Ctts : type == stsc ? Stsc : type == stsz ? Stsz : type == stss ? Stss : Stco;
    if (track.seen & bit) return Refuse(ParseError::InvalidTable);
    track.seen |= bit;
    if (stack_[depth_ - 1].type != stbl) return Fail(ParseError::BadBox);
    const auto full = U32(data), version = full >> 24;
    if ((full & 0xffffff) || (type != ctts && version != 0) || version > 1) return Refuse(ParseError::Unsupported);
    const std::uint32_t header = type == stsz ? 12u : 8u;
    const auto fixed = type == stsz ? U32(data + 4) : 0u;
    const auto count = U32(data + header - 4);
    std::uint32_t stride = type == stsc ? 12u : (type == stts || type == ctts || type == co64) ? 8u : 4u;
    if (fixed) stride = 0; // One size for every sample: nothing per sample is stored.
    // Index bytes: the file's own entries (runs, chunks, sync numbers or sizes), plus the stts/ctts marks.
    const std::uint64_t marks = (std::uint64_t{count} + kMarkRows - 1) / kMarkRows;
    std::uint64_t need = 0;
    if (type == stts || type == ctts) need = count * std::uint64_t{sizeof(TimeRun)} + marks * sizeof(TimeMark);
    else if (type == stsc) need = count * std::uint64_t{sizeof(ChunkRun)};
    else if (type == co64) need = count * std::uint64_t{8};
    else if (stride) need = count * std::uint64_t{4};
    if (need > pool_bytes_ - pool_used_) return Fail(ParseError::LimitExceeded);
    if (stride ? std::uint64_t{count} * stride != length - header : length != header) return Fail(ParseError::BadBox);
    bool reserved = true;
    if (count && (type == stts || type == ctts)) {
        auto* runs = static_cast<TimeRun*>(Reserve(count * std::uint64_t{sizeof(TimeRun)}));
        auto* anchors = static_cast<TimeMark*>(Reserve(marks * sizeof(TimeMark)));
        reserved = runs && anchors;
        if (type == stts) { track.timing = runs; track.timing_marks = anchors; track.timing_count = count; }
        else { track.composition = runs; track.composition_marks = anchors; track.composition_count = count; track.composition_signed = version == 1; }
    } else if (count && type == stsc) {
        track.mapping = static_cast<ChunkRun*>(Reserve(need)); track.mapping_count = count; reserved = track.mapping != nullptr;
    } else if (type == stsz) {
        track.sample_count = count; track.fixed_size = fixed; track.info.max_sample_bytes = fixed;
        if (count && !fixed) { track.sizes = static_cast<std::uint32_t*>(Reserve(need)); reserved = track.sizes != nullptr; }
    } else if (count && type == co64) {
        track.chunks64 = static_cast<std::uint64_t*>(Reserve(need)); track.chunk_count = count; reserved = track.chunks64 != nullptr;
    } else if (count && type == stco) {
        track.chunks32 = static_cast<std::uint32_t*>(Reserve(need)); track.chunk_count = count; reserved = track.chunks32 != nullptr;
    } else if (count && type == stss) {
        track.sync = static_cast<std::uint32_t*>(Reserve(need)); track.sync_count = count; reserved = track.sync != nullptr;
    }
    if (!reserved) return Fail(ParseError::LimitExceeded);
    table_ = {box_payload_ + header, stride, stride ? count : 0, 0, true};
    return true;
}

bool IsobmffParser::TableEntries(TrackState& track, const std::byte* data, const std::uint32_t entries) noexcept {
    const auto first = table_.done;
    if (box_type_ == stts || box_type_ == ctts) {
        auto* runs = (box_type_ == stts ? track.timing : track.composition) + first;
        for (std::uint32_t i = 0; i < entries; ++i, data += 8) runs[i] = {U32(data), U32(data + 4)};
    } else if (box_type_ == stsc) {
        for (std::uint32_t i = 0; i < entries; ++i, data += 12) {
            if (U32(data + 8) != 1) return Refuse(ParseError::InvalidTable); // One sample description.
            track.mapping[first + i] = {U32(data), U32(data + 4), 0};
        }
    } else if (box_type_ == stsz) {
        for (std::uint32_t i = 0; i < entries; ++i, data += 4) {
            const auto size = U32(data);
            if (!size) return Refuse(ParseError::Unsupported);
            track.sizes[first + i] = size;
            track.info.max_sample_bytes = (std::max)(track.info.max_sample_bytes, size);
        }
    } else if (box_type_ == co64) {
        for (std::uint32_t i = 0; i < entries; ++i, data += 8) track.chunks64[first + i] = U64(data);
    } else {
        auto* values = (box_type_ == stco ? track.chunks32 : track.sync) + first;
        for (std::uint32_t i = 0; i < entries; ++i, data += 4) values[i] = U32(data);
    }
    return true;
}
} // namespace media::demux
