// media_probe (API 15; docs/sdk-reference.md, "Probe"): what a file holds and whether the engine plays it, from the container's
// metadata alone, portable (the reader every mode uses: FileBlockSource + IsobmffParser). No engine, window, GPU or audio.
// 2026-11-04: an embedding host could only learn that a file was unsupported by opening it in a window, and then got a stage
// number ("stage 4, code 0"): it could not tell an AV1 file from a bug. Every refusal here says why in words.
#include "media/c_api.h"
#include "media/codec/Rbsp.hpp"
#include "media/core/linear_memory_arena.hpp"
#include "media/core/spsc_ring_buffer.hpp"
#include "media/demux/IsobmffParser.hpp"
#include "media/demux/MatroskaReader.hpp"
#include "media/platform/FileBlockSource.hpp"
#include "ProbeText.hpp"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>

namespace {
// Whether this system has the AV1 decoder the windowed engine uses (API 17): Windows' software AV1 transform, asked once.
bool Av1Platform() noexcept;
using namespace media;
using demux::Codec;
using demux::ParseError;
using demux::TrackKind;

// snprintf into a fixed field, cut at a UTF-8 boundary (the formats below are ASCII; a file's own text never goes in a format).
void Put(char* out, std::size_t bytes, const char* format, ...) noexcept {
    va_list args; va_start(args, format);
    const int n = std::vsnprintf(out, bytes, format, args);
    va_end(args);
    if (n < 0) { out[0] = 0; return; }
    sdk::CutUtf8(out, bytes);
}
void FourCc(std::uint32_t v, char* out) noexcept {
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((v >> (24 - 8 * i)) & 0xff);
        out[i] = c >= 32 && c < 127 ? c : '?';
    }
    out[4] = 0;
}
const char* ErrorName(ParseError e) noexcept {
    switch (e) {
    case ParseError::None: return "none";
    case ParseError::NotInitialized: return "not initialised";
    case ParseError::BadBox: return "a malformed box";
    case ParseError::Truncated: return "the file ends early";
    case ParseError::Unsupported: return "a feature the reader does not support";
    case ParseError::LimitExceeded: return "a table larger than the reader's limits";
    case ParseError::InvalidTable: return "an inconsistent sample table";
    case ParseError::InvalidCodec: return "an invalid or unsupported codec configuration";
    case ParseError::InvalidTimestamp: return "timestamps out of range";
    case ParseError::InvalidSampleRange: return "samples outside the file";
    case ParseError::IoFailure: return "a read error";
    }
    return "an error";
}
// The container from its first bytes, for the files the MP4 reader cannot open.
std::uint32_t Sniff(const unsigned char* b, std::size_t n, const char*& name) noexcept {
    const auto at = [&](std::size_t o, const char* s) { const auto l = std::strlen(s); return n >= o + l && std::memcmp(b + o, s, l) == 0; };
    name = "unknown";
    if (n >= 8 && (at(4, "ftyp") || at(4, "moov") || at(4, "mdat") || at(4, "free") || at(4, "wide") || at(4, "skip") || at(4, "styp"))) { name = "MP4/MOV"; return MEDIA_CONTAINER_MP4; }
    if (n >= 4 && b[0] == 0x1a && b[1] == 0x45 && b[2] == 0xdf && b[3] == 0xa3) { name = "Matroska/WebM"; return MEDIA_CONTAINER_MATROSKA; }
    if (at(0, "RIFF") && at(8, "AVI ")) { name = "AVI"; return MEDIA_CONTAINER_AVI; }
    if (at(0, "RIFF") && at(8, "WAVE")) { name = "WAV"; return MEDIA_CONTAINER_WAV; }
    if (n >= 377 && b[0] == 0x47 && b[188] == 0x47 && b[376] == 0x47) { name = "MPEG transport stream"; return MEDIA_CONTAINER_MPEG_TS; }
    if (n >= 197 && b[4] == 0x47 && b[196] == 0x47) { name = "MPEG transport stream (M2TS)"; return MEDIA_CONTAINER_MPEG_TS; }
    if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xba) { name = "MPEG program stream"; return MEDIA_CONTAINER_MPEG_PS; }
    if (at(0, "FLV")) { name = "FLV"; return MEDIA_CONTAINER_FLV; }
    if (n >= 4 && b[0] == 0x30 && b[1] == 0x26 && b[2] == 0xb2 && b[3] == 0x75) { name = "ASF (WMV/WMA)"; return MEDIA_CONTAINER_ASF; }
    if (at(0, "ID3") || (n >= 2 && b[0] == 0xff && (b[1] & 0xe0) == 0xe0)) { name = "MP3"; return MEDIA_CONTAINER_MP3; }
    if (at(0, "fLaC")) { name = "FLAC"; return MEDIA_CONTAINER_FLAC; }
    if (at(0, "OggS")) { name = "Ogg"; return MEDIA_CONTAINER_OGG; }
    return MEDIA_CONTAINER_UNKNOWN;
}

// A whole byte range of the file (a parameter set, a codec record), at most `capacity` bytes, through the block source.
std::size_t ReadRange(platform::FileBlockSource& source, demux::ByteRange range, std::byte* out, std::size_t capacity) noexcept {
    if (!range.bytes || range.bytes > capacity) return 0;
    platform::BlockToken token{};
    if (source.Request(range.offset, static_cast<std::uint32_t>(range.bytes), token) != platform::IoStatus::Pending) return 0;
    platform::BlockView view{}; auto state = platform::IoStatus::Pending;
    for (unsigned i = 0; i < 1000000 && state == platform::IoStatus::Pending; ++i) state = source.Poll(token, view);
    std::size_t got = 0;
    if (state == platform::IoStatus::Ready) { std::memcpy(out, view.data + (range.offset - view.file_offset), static_cast<std::size_t>(range.bytes)); got = static_cast<std::size_t>(range.bytes); }
    (void)source.Release(token);
    return got;
}

// H.264 SPS (7.3.2.1.1) as far as the scan: what decides whether the engine decodes it.
struct H264Header { unsigned profile = 0, chroma = 1, depth_luma = 8, depth_chroma = 8; bool progressive = true, read = false; };
H264Header ReadH264Sps(std::span<const std::byte> nal) noexcept {
    H264Header h;
    if (nal.size() < 4) return h;
    codec::RbspReader r(nal.subspan(1)); // After the NAL header.
    h.profile = r.Bits(8); (void)r.Bits(8); (void)r.Bits(8); (void)r.Ue();
    const unsigned p = h.profile;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        h.chroma = r.Ue();
        if (h.chroma == 3) (void)r.Bits(1);
        h.depth_luma = 8 + r.Ue(); h.depth_chroma = 8 + r.Ue();
        (void)r.Bits(1);
        if (r.Bits(1)) {
            for (unsigned i = 0; i < (h.chroma != 3 ? 8u : 12u); ++i) {
                if (!r.Bits(1)) continue;
                const unsigned size = i < 6 ? 16 : 64; int last = 8, next = 8;
                for (unsigned j = 0; j < size && r.Good(); ++j) {
                    if (next != 0) next = (last + r.Se() + 256) % 256;
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    (void)r.Ue(); // log2_max_frame_num_minus4
    const auto poc = r.Ue();
    if (poc == 0) (void)r.Ue();
    else if (poc == 1) {
        (void)r.Bits(1); (void)r.Se(); (void)r.Se();
        const auto cycle = r.Ue();
        for (unsigned i = 0; i < cycle && i < 256 && r.Good(); ++i) (void)r.Se();
    }
    (void)r.Ue(); (void)r.Bits(1); (void)r.Ue(); (void)r.Ue();
    h.progressive = r.Bits(1) != 0;
    h.read = r.Good();
    return h;
}
const char* H264Profile(unsigned p) noexcept {
    switch (p) {
    case 66: return "Baseline"; case 77: return "Main"; case 88: return "Extended"; case 100: return "High";
    case 110: return "High 10"; case 122: return "High 4:2:2"; case 244: return "High 4:4:4 Predictive"; case 44: return "CAVLC 4:4:4 Intra";
    case 118: case 128: return "Multiview/Stereo High"; case 83: case 86: return "Scalable";
    default: return "profile ?";
    }
}
const char* Chroma(unsigned c) noexcept { return c == 0 ? "4:0:0" : c == 1 ? "4:2:0" : c == 2 ? "4:2:2" : c == 3 ? "4:4:4" : "?"; }

void Describe(platform::FileBlockSource& source, const demux::TrackInfo& t, media_probe_track_t& out) noexcept {
    out.id = t.id; out.width = t.width; out.height = t.height; out.channels = t.channels; out.sample_rate = t.sample_rate;
    out.duration_100ns = t.duration_100ns;
    out.kind = t.kind == TrackKind::Video ? MEDIA_TRACK_VIDEO : t.kind == TrackKind::Audio ? MEDIA_TRACK_AUDIO : t.kind == TrackKind::Subtitle ? MEDIA_TRACK_SUBTITLE : MEDIA_TRACK_OTHER;
    FourCc(t.sample_entry ? t.sample_entry : t.handler, out.codec);
    std::memcpy(out.language, t.language[0] ? t.language : "und", 3); out.language[3] = 0;
    out.supported = 0; out.reason[0] = 0; out.description[0] = 0;
    char where[5]; FourCc(t.refusal_box, where);
    if (t.refusal != ParseError::None) {
        Put(out.description, sizeof(out.description), "'%s' track", out.codec);
        if (t.kind == TrackKind::Other) Put(out.reason, sizeof(out.reason), "not a picture, sound or text track (handler '%s')", out.codec);
        else if (t.codec == Codec::Unknown) Put(out.reason, sizeof(out.reason), "codec '%s' is not supported", out.codec);
        else Put(out.reason, sizeof(out.reason), "refused by the MP4 reader: %s in '%s' at byte %llu", ErrorName(t.refusal), where,
                 static_cast<unsigned long long>(t.refusal_offset));
        return;
    }
    if (t.kind == TrackKind::Video) {
        if (t.codec == Codec::H264) {
            H264Header h{};
            for (unsigned i = 0; i < t.parameter_count; ++i) {
                if (t.parameters[i].type != 7) continue;
                std::byte sps[512]; const auto n = ReadRange(source, t.parameters[i].range, sps, sizeof(sps));
                if (n) h = ReadH264Sps({sps, n});
                break;
            }
            if (!h.read) { Put(out.description, sizeof(out.description), "H.264"); Put(out.reason, sizeof(out.reason), "H.264 sequence parameters could not be read"); return; }
            Put(out.description, sizeof(out.description), "H.264 %s, %s %u-bit, %s", H264Profile(h.profile), Chroma(h.chroma), h.depth_luma, h.progressive ? "progressive" : "interlaced");
            if (h.chroma != 1 || h.depth_luma != 8 || h.depth_chroma != 8)
                Put(out.reason, sizeof(out.reason), "H.264 %s %u-bit is not decoded (H.264 4:2:0 8-bit only)", Chroma(h.chroma), h.depth_luma);
            else if (!h.progressive) Put(out.reason, sizeof(out.reason), "interlaced H.264 (fields or MBAFF) is not decoded");
            else if (h.profile != 66 && h.profile != 77 && h.profile != 88 && h.profile != 100) Put(out.reason, sizeof(out.reason), "H.264 %s profile is not decoded", H264Profile(h.profile));
            else out.supported = 1;
            return;
        }
        if (t.codec == Codec::Hevc) {
            std::byte record[64]; const auto n = ReadRange(source, {t.configuration.offset, (std::min<std::uint64_t>)(t.configuration.bytes, 23)}, record, sizeof(record));
            unsigned profile = 0, chroma = 1, depth = 8;
            if (n >= 23) {
                profile = std::to_integer<unsigned>(record[1]) & 31; chroma = std::to_integer<unsigned>(record[16]) & 3; depth = 8 + (std::to_integer<unsigned>(record[17]) & 7);
            }
            const char* name = profile == 1 ? "Main" : profile == 2 ? "Main 10" : profile == 3 ? "Main Still Picture" : profile == 4 ? "Range Extensions" : "profile ?";
            Put(out.description, sizeof(out.description), "HEVC %s, %s %u-bit", name, Chroma(chroma), depth);
            if (chroma != 1 || depth > 10) Put(out.reason, sizeof(out.reason), "HEVC %s %u-bit is not decoded (4:2:0, 8- and 10-bit only)", Chroma(chroma), depth);
            else out.supported = 1;
            return;
        }
        if (t.codec == Codec::Av1) { // API 17: the platform's software decoder (ENGINE_MANIFESTO.md 2E), the windowed engine on Windows.
            Put(out.description, sizeof(out.description), "AV1");
            if (Av1Platform()) out.supported = 1;
            else Put(out.reason, sizeof(out.reason), "AV1: no AV1 decoder on this system (Windows: the AV1 Video Extension)");
            return;
        }
        Put(out.description, sizeof(out.description), "video '%s'", out.codec); Put(out.reason, sizeof(out.reason), "video codec '%s' is not supported", out.codec);
        return;
    }
    if (t.kind == TrackKind::Audio) {
        if (t.codec == Codec::Aac) {
            Put(out.description, sizeof(out.description), "AAC, %u ch, %u Hz", t.channels, t.sample_rate);
            if (t.channels > 6) Put(out.reason, sizeof(out.reason), "AAC with %u channels: Windows' AAC decoder takes 6 at most", t.channels);
            else out.supported = 1;
            return;
        }
        if (t.codec == Codec::Pcm) { Put(out.description, sizeof(out.description), "PCM %u-bit, %u ch, %u Hz", t.pcm_bits, t.channels, t.sample_rate); out.supported = 1; return; }
        if (t.codec == Codec::Opus) { // API 17: the platform's Opus decoder in the windowed engine (mono, stereo); the host's in host mode.
            Put(out.description, sizeof(out.description), "Opus, %u ch", t.channels);
            if (t.channels <= 2) out.supported = 1; else Put(out.reason, sizeof(out.reason), "Opus with %u channels: mono and stereo only", t.channels);
            return;
        }
        Put(out.description, sizeof(out.description), "audio '%s'", out.codec); Put(out.reason, sizeof(out.reason), "audio codec '%s' is not supported", out.codec);
        return;
    }
    if (t.kind == TrackKind::Subtitle) {
        Put(out.description, sizeof(out.description), t.codec == Codec::Ssa ? "ASS/SSA text" : t.codec == Codec::Text ? "text" : "timed text"); out.supported = 1; return;
    }
    Put(out.description, sizeof(out.description), "'%s' track", out.codec);
}

// ---- Matroska / WebM (API 16): the metadata through demux::MatroskaReader ----
struct FileRange { std::FILE* f; };
std::size_t ReadFile(void* context, std::uint64_t offset, void* out, std::size_t bytes) noexcept {
    auto* f = static_cast<FileRange*>(context)->f;
#if defined(_WIN32)
    if (_fseeki64(f, static_cast<__int64>(offset), SEEK_SET) != 0) return 0;
#else
    if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) return 0;
#endif
    return std::fread(out, 1, bytes, f);
}
struct MkvCodec { const char* id; const char* fourcc; const char* name; int kind; bool decodable; };
// kind: 1 H.264, 2 HEVC, 3 AV1, 0 other. `decodable`: the engine decodes the codec (API 17: Matroska plays).
constexpr MkvCodec kMkvCodecs[]{
    {"V_MPEG4/ISO/AVC", "avc1", "H.264", 1, true}, {"V_MPEGH/ISO/HEVC", "hvc1", "HEVC", 2, true}, {"V_AV1", "av01", "AV1", 3, true},
    {"V_VP9", "vp09", "VP9", 0, false}, {"V_VP8", "vp08", "VP8", 0, false}, {"V_MPEG2", "mp2v", "MPEG-2 video", 0, false},
    {"V_MS/VFW/FOURCC", "vfw", "VfW video", 0, false}, {"A_AAC", "mp4a", "AAC", 0, true}, {"A_OPUS", "Opus", "Opus", 0, true},
    {"A_AC3", "ac-3", "AC-3", 0, false}, {"A_EAC3", "ec-3", "E-AC-3", 0, false}, {"A_DTS", "dtsc", "DTS", 0, false},
    {"A_FLAC", "fLaC", "FLAC", 0, false}, {"A_VORBIS", "vorb", "Vorbis", 0, false}, {"A_MPEG/L3", "mp3", "MP3", 0, false},
    {"A_PCM/INT/LIT", "pcm", "PCM", 0, false}, {"A_TRUEHD", "mlpa", "TrueHD", 0, false},
    {"S_TEXT/UTF8", "srt", "SRT text", 0, true}, {"S_TEXT/WEBVTT", "wvtt", "WebVTT text", 0, true}, {"S_TEXT/ASS", "ass", "ASS text", 0, true},
    {"S_TEXT/SSA", "ssa", "SSA text", 0, true}, {"S_HDMV/PGS", "pgs", "PGS bitmaps", 0, false}, {"S_VOBSUB", "vobs", "VobSub bitmaps", 0, false},
};
const MkvCodec* FindMkvCodec(const char* id) noexcept {
    for (const auto& c : kMkvCodecs) {
        const auto n = std::strlen(c.id);
        if (std::strncmp(id, c.id, n) == 0 && (id[n] == 0 || id[n] == '/')) return &c; // A_AAC/MPEG4/LC/SBR (old style) too.
    }
    return nullptr;
}
media_status ProbeMatroska(const char* path, media_probe_t& info) noexcept {
    FileRange file{media::sdk::OpenRead(path)};
    if (!file.f) { Put(info.reason, sizeof(info.reason), "the file cannot be opened (missing or unreadable)"); return MEDIA_ERROR_OPEN; }
#if defined(_WIN32)
    (void)_fseeki64(file.f, 0, SEEK_END); const auto bytes = static_cast<std::uint64_t>(_ftelli64(file.f));
#else
    (void)fseeko(file.f, 0, SEEK_END); const auto bytes = static_cast<std::uint64_t>(ftello(file.f));
#endif
    demux::MatroskaReader mkv;
    const auto status = mkv.Read(&ReadFile, &file, bytes);
    if (status != demux::MatroskaStatus::Ok) {
        std::fclose(file.f);
        Put(info.reason, sizeof(info.reason), "Matroska/WebM: %s", status == demux::MatroskaStatus::NoTracks ? "no tracks" : status == demux::MatroskaStatus::Truncated ? "the file ends before its tracks" : "malformed EBML");
        return MEDIA_OK;
    }
    info.duration_100ns = mkv.Duration100ns();
    unsigned decodable_video = 0, decodable_audio = 0, videos = 0;
    char summary[96]{}; std::size_t summary_n = 0;
    for (unsigned i = 0; i < mkv.TrackCount() && info.track_count < MEDIA_PROBE_MAX_TRACKS; ++i) {
        const auto& t = mkv.Track(i);
        auto& out = info.tracks[info.track_count++];
        out.id = static_cast<std::uint32_t>(t.number);
        out.kind = t.type == demux::MatroskaTrackType::Video ? MEDIA_TRACK_VIDEO : t.type == demux::MatroskaTrackType::Audio ? MEDIA_TRACK_AUDIO :
                   t.type == demux::MatroskaTrackType::Subtitle ? MEDIA_TRACK_SUBTITLE : MEDIA_TRACK_OTHER;
        out.width = t.width; out.height = t.height; out.channels = t.channels; out.sample_rate = static_cast<std::uint32_t>(t.sample_rate + 0.5);
        out.duration_100ns = info.duration_100ns;
        std::memcpy(out.language, t.language, 3); out.language[3] = 0;
        const auto* codec = FindMkvCodec(t.codec_id);
        Put(out.codec, sizeof(out.codec), "%s", codec ? codec->fourcc : "?");
        bool decodable = codec && codec->decodable && !t.encoded;
        if (codec && codec->kind == 1 && t.private_bytes >= 8 && t.private_bytes <= 4096) {
            // avcC: 6 bytes, then the first SPS's 16-bit length and the SPS.
            unsigned char record[4096]; const auto n = ReadFile(&file, t.private_offset, record, t.private_bytes);
            const unsigned sps_bytes = n >= 8 ? (record[6] << 8) | record[7] : 0;
            H264Header h{};
            if (sps_bytes && 8 + sps_bytes <= n) h = ReadH264Sps({reinterpret_cast<const std::byte*>(record + 8), sps_bytes});
            if (h.read) {
                Put(out.description, sizeof(out.description), "H.264 %s, %s %u-bit, %s", H264Profile(h.profile), Chroma(h.chroma), h.depth_luma, h.progressive ? "progressive" : "interlaced");
                if (h.chroma != 1 || h.depth_luma != 8) { decodable = false; Put(out.reason, sizeof(out.reason), "H.264 %s %u-bit is not decoded (H.264 4:2:0 8-bit only)", Chroma(h.chroma), h.depth_luma); }
                else if (!h.progressive) { decodable = false; Put(out.reason, sizeof(out.reason), "interlaced H.264 (fields or MBAFF) is not decoded"); }
            } else Put(out.description, sizeof(out.description), "H.264");
        } else if (codec && codec->kind == 2 && t.private_bytes >= 23) {
            unsigned char record[23]; const auto n = ReadFile(&file, t.private_offset, record, 23);
            const unsigned profile = n == 23 ? record[1] & 31 : 0, chroma = n == 23 ? record[16] & 3 : 1, depth = n == 23 ? 8 + (record[17] & 7) : 8;
            Put(out.description, sizeof(out.description), "HEVC %s, %s %u-bit", profile == 1 ? "Main" : profile == 2 ? "Main 10" : "profile ?", Chroma(chroma), depth);
            if (chroma != 1 || depth > 10) { decodable = false; Put(out.reason, sizeof(out.reason), "HEVC %s %u-bit is not decoded (4:2:0, 8- and 10-bit only)", Chroma(chroma), depth); }
        } else if (out.kind == MEDIA_TRACK_AUDIO) {
            Put(out.description, sizeof(out.description), "%s, %u ch, %u Hz", codec ? codec->name : t.codec_id, t.channels, out.sample_rate);
        } else Put(out.description, sizeof(out.description), "%s", codec ? codec->name : t.codec_id);
        if (t.encoded && !out.reason[0]) Put(out.reason, sizeof(out.reason), "compressed or encrypted frames (ContentEncodings) are not read");
        else if (!codec && !out.reason[0]) Put(out.reason, sizeof(out.reason), "codec '%s' is not supported", t.codec_id);
        else if (codec && !codec->decodable && !out.reason[0]) Put(out.reason, sizeof(out.reason), "%s is not decoded yet", codec->name);
        if (decodable && codec && codec->kind == 3 && !Av1Platform()) { decodable = false; Put(out.reason, sizeof(out.reason), "AV1: no AV1 decoder on this system (Windows: the AV1 Video Extension)"); }
        if (decodable && out.kind == MEDIA_TRACK_AUDIO && codec && std::strcmp(codec->fourcc, "Opus") == 0 && t.channels > 2) {
            decodable = false; Put(out.reason, sizeof(out.reason), "Opus with %u channels: mono and stereo only", t.channels);
        }
        out.supported = decodable ? 1u : 0u; // API 17: Matroska plays (demux::MatroskaDemuxer).
        if (out.kind == MEDIA_TRACK_VIDEO) ++videos;
        if (decodable && out.kind == MEDIA_TRACK_VIDEO) ++decodable_video;
        if (decodable && out.kind == MEDIA_TRACK_AUDIO) ++decodable_audio;
        if (out.kind != MEDIA_TRACK_SUBTITLE && summary_n + 2 < sizeof(summary)) {
            const int w = std::snprintf(summary + summary_n, sizeof(summary) - summary_n, "%s%s", summary_n ? ", " : "", codec ? codec->name : t.codec_id);
            if (w > 0) summary_n = (std::min)(sizeof(summary) - 1, summary_n + static_cast<std::size_t>(w));
        }
    }
    std::fclose(file.f);
    info.playable = videos ? (decodable_video ? 1u : 0u) : (decodable_audio ? 1u : 0u);
    if (!info.playable)
        Put(info.reason, sizeof(info.reason), "%s (%s): %s", mkv.WebM() ? "WebM" : "Matroska", summary, videos ? "its video codec is not decoded" : "no codec the engine decodes");
    else if (videos && !decodable_audio) Put(info.reason, sizeof(info.reason), "plays without sound (%s)", summary);
    return MEDIA_OK;
}
}

namespace media::sdk {
bool Utf8ToWide(const char* utf8, wchar_t* out, std::size_t capacity) noexcept {
    if (!utf8 || !out || !capacity) return false;
#if defined(_WIN32)
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, static_cast<int>(capacity)) > 0;
#else
    std::size_t n = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(utf8); *p;) {
        unsigned c = *p++, more = c >= 0xf0 ? 3u : c >= 0xe0 ? 2u : c >= 0xc0 ? 1u : 0u;
        if (c >= 0x80 && c < 0xc0) return false;
        c &= more == 3 ? 0x07u : more == 2 ? 0x0fu : more == 1 ? 0x1fu : 0x7fu;
        for (unsigned i = 0; i < more; ++i, ++p) { if ((*p & 0xc0) != 0x80) return false; c = (c << 6) | (*p & 0x3fu); }
        if (n + 1 >= capacity) return false;
        out[n++] = static_cast<wchar_t>(c);
    }
    out[n] = 0;
    return true;
#endif
}
std::FILE* OpenRead(const char* utf8_path) noexcept {
    if (!utf8_path) return nullptr;
#if defined(_WIN32)
    wchar_t wide[2048];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide, 2048) <= 0) return nullptr;
    std::FILE* f = nullptr;
    return _wfopen_s(&f, wide, L"rb") == 0 ? f : nullptr;
#else
    return std::fopen(utf8_path, "rb");
#endif
}
void CutUtf8(char* text, std::size_t bytes) noexcept {
    if (!bytes) return;
    text[bytes - 1] = 0;
    std::size_t n = std::strlen(text);
    if (n + 1 < bytes) return; // Not cut.
    // Cut: drop a trailing partial sequence (continuation bytes 10xxxxxx after the last lead byte that does not fit).
    std::size_t i = n;
    while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xc0) == 0x80) --i;
    if (i > 0) {
        const auto lead = static_cast<unsigned char>(text[i - 1]);
        const std::size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (n - (i - 1) < need) text[i - 1] = 0;
    }
}

media_status Probe(const char* path, media_probe_t& info) noexcept {
    const auto size = info.struct_size;
    std::memset(&info, 0, sizeof(info)); info.struct_size = size;
    std::FILE* f = OpenRead(path);
    if (!f) { Put(info.reason, sizeof(info.reason), "the file cannot be opened (missing or unreadable)"); return MEDIA_ERROR_OPEN; }
    unsigned char head[400]{}; const auto got = std::fread(head, 1, sizeof(head), f); std::fclose(f);
    const char* name = nullptr;
    info.container = Sniff(head, got, name);
    if (info.container == MEDIA_CONTAINER_MATROSKA) return ProbeMatroska(path, info); // API 16.
    if (info.container != MEDIA_CONTAINER_MP4) {
        if (info.container == MEDIA_CONTAINER_UNKNOWN) Put(info.reason, sizeof(info.reason), "not a media file the engine recognises (MP4 and MOV only)");
        else Put(info.reason, sizeof(info.reason), "%s container: not supported yet (MP4 and MOV only)", name);
        return MEDIA_OK;
    }
    core::LinearMemoryArena arena;
    // The index (16 MiB at most: a two-hour film takes about 2), the block source's 8 x 1 MiB blocks, the packet ring.
    if (!arena.Initialize(28u << 20)) { Put(info.reason, sizeof(info.reason), "out of memory for the probe"); return MEDIA_ERROR_OPEN; }
    platform::FileBlockSource source;
    if (!source.Open(arena, path)) { Put(info.reason, sizeof(info.reason), "the file cannot be opened (missing or unreadable)"); return MEDIA_ERROR_OPEN; }
    auto* packets = arena.Create<core::SpscRingBuffer<core::MediaPacket, 64>>();
    demux::IsobmffParser parser;
    demux::ParserLimits limits; limits.tracks = MEDIA_PROBE_MAX_TRACKS; limits.index_bytes = 16ull << 20;
    if (!packets || !parser.Initialize(arena, source, limits)) { Put(info.reason, sizeof(info.reason), "out of memory for the probe"); return MEDIA_ERROR_OPEN; }
    for (unsigned i = 0; i < 1000000 && !parser.MetadataReady(); ++i) {
        const auto status = parser.Step(*packets);
        while (const auto* p = packets->PeekReadSlot()) { (void)parser.ReturnPacket(*p); (void)packets->ReleaseReadSlot(); }
        if (status == demux::ParseStatus::Failed || status == demux::ParseStatus::Done) break;
    }
    if (!parser.MetadataReady()) {
        const auto d = parser.Diagnostic(); char where[5]; FourCc(d.box_type, where);
        if (parser.TrackCount() == 0 && d.error == ParseError::None) Put(info.reason, sizeof(info.reason), "no playable track");
        else Put(info.reason, sizeof(info.reason), "the MP4 reader stopped: %s in '%s' at byte %llu", ErrorName(d.error), where, static_cast<unsigned long long>(d.offset));
    }
    bool video = false, video_ok = false, audio = false, audio_ok = false;
    const char* video_reason = nullptr; const char* audio_reason = nullptr; const char* audio_desc = nullptr;
    for (std::uint32_t i = 0; i < parser.TrackCount() && info.track_count < MEDIA_PROBE_MAX_TRACKS; ++i) {
        const auto* t = parser.Track(i);
        if (!t) continue;
        auto& out = info.tracks[info.track_count++];
        Describe(source, *t, out);
        if (out.duration_100ns > info.duration_100ns) info.duration_100ns = out.duration_100ns;
        if (out.kind == MEDIA_TRACK_VIDEO) { video = true; if (out.supported) video_ok = true; else if (!video_reason) video_reason = out.reason; }
        if (out.kind == MEDIA_TRACK_AUDIO) {
            audio = true;
            if (out.supported && std::strcmp(out.codec, "mp4a") == 0) audio_ok = true;
            else if (!audio_reason) { audio_reason = out.reason[0] ? out.reason : "PCM sound plays in host mode only"; audio_desc = out.description; }
        }
    }
    if (!parser.MetadataReady() && !info.track_count) return MEDIA_OK;
    info.playable = (video_ok || (!video && audio_ok)) && parser.MetadataReady() ? 1u : 0u;
    if (!info.playable && !info.reason[0]) {
        if (video && video_reason) Put(info.reason, sizeof(info.reason), "%s", video_reason);
        else if (!video && audio && audio_reason) Put(info.reason, sizeof(info.reason), "%s", audio_reason);
        else Put(info.reason, sizeof(info.reason), "no playable picture or sound track");
    } else if (info.playable && audio && !audio_ok && audio_reason) {
        Put(info.reason, sizeof(info.reason), "plays without sound: %s (%s)", audio_reason, audio_desc ? audio_desc : "");
    } else if (info.playable && !info.reason[0] && !audio) Put(info.reason, sizeof(info.reason), "no sound track");
    return MEDIA_OK;
}
const char* StageText(bool host, std::uint32_t stage) noexcept {
    if (host) {
        switch (stage) {
        case 1: return "invalid configuration"; case 2: return "memory for the engine's arenas"; case 3: return "the file could not be opened";
        case 4: return "the container reader could not start"; case 5: return "the container could not be read";
        case 6: return "no track this engine can play"; case 7: return "the video decoder could not be prepared";
        case 8: return "the audio path could not be prepared"; case 9: return "the subtitle file could not be read";
        case 10: return "the voice module could not be prepared"; case 11: return "the container failed during playback";
        case 12: case 13: return "the decode chain failed"; case 14: return "audio conversion failed";
        default: return "playback failed";
        }
    }
    switch (stage) {
    case 1: return "the file or URL could not be opened"; case 2: return "memory, the container reader or the decoder could not start";
    case 3: return "the container could not be read"; case 4: return "no usable video track (or, without video, no AAC sound)";
    case 5: return "the video stream's configuration is not decodable"; case 6: return "the renderer could not start (GPU, window)";
    case 7: return "the GPU decoder refused the stream"; case 8: return "a worker thread could not start";
    case 9: return "the audio decoder or output could not start"; case 11: return "a packet could not be returned to the reader";
    case 14: case 15: case 16: return "audio output failed"; case 17: return "the container failed during playback";
    case 18: return "a read failed during playback"; case 19: return "AAC decoding failed"; case 20: return "the video bitstream is not decodable";
    case 21: return "the picture size changed mid-stream"; case 22: case 23: case 24: return "the decode chain failed";
    case 29: return "timestamps out of order or out of range"; case 32: return "the stream reorders more pictures than the renderer holds";
    case 40: return "the platform's AV1 decoder could not be prepared"; case 41: return "the platform's AV1 decoder failed";
    default: return "playback failed";
    }
}
}
#if defined(_WIN32)
#include <mfapi.h>
#include <mftransform.h>
namespace {
bool Av1Platform() noexcept {
    static std::atomic<int> known{-1};
    int value = known.load(std::memory_order_acquire);
    if (value < 0) {
        // MFTEnumEx lists the transforms without creating one; the AV1 Video Extension registers itself as a synchronous one.
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_AV1};
        IMFActivate** list = nullptr; UINT32 count = 0;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        value = 0;
        if (SUCCEEDED(mf) && SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input, nullptr, &list, &count))) {
            value = count ? 1 : 0;
            for (UINT32 i = 0; i < count; ++i) list[i]->Release();
            CoTaskMemFree(list);
        }
        if (SUCCEEDED(mf)) (void)MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        known.store(value, std::memory_order_release);
    }
    return value == 1;
}
}
#else
namespace { bool Av1Platform() noexcept { return false; } } // The windowed engine (Windows) decodes AV1; host mode leaves it to the host.
#endif
