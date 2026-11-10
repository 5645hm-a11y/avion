// Audit of real-world media files (assets/test_fixtures): every .mp4 in a directory is opened through the production
// IOCP source and demuxer, its tracks and codec configurations are described, the first 60 video samples and two
// seconds of audio are demuxed (the audio decoded by the production AAC decoder when it is AAC-LC), and random access
// is exercised. Every .srt/.vtt is parsed and indexed for search. The run fails on a file the container layer cannot
// read, on a wrong packet, on a heap allocation after the arena is sealed, or on a block lease left behind. A video
// codec the engine has no decoder for is reported as a deviation, not a failure: the report is the point.
#include "media/demux/IsobmffParser.hpp"
#include "media/platform/windows/Win32IocpBlockSource.hpp"
#include "media/platform/windows/Win32AacDecoder.hpp"
#include "media/subtitle/SubtitleSearch.hpp"
#include "allocation_probe.hpp"
#include <d3d12.h>
#include <d3d12video.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
using namespace media;
using platform::IoStatus;
using platform::windows::Win32IocpBlockSource;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::printf("  FAIL %d: %s\n", __LINE__, #x); } } while (false)
void Fail(const char* what) { ++failures; std::printf("  FAIL: %s\n", what); }
constexpr std::uint32_t kBlockBytes = 8 * 1024 * 1024, kBlocks = 16; // The engine's own source budget.
struct Hardware { bool queried = false, h264 = false, hevc = false, hevc10 = false, vp9 = false, av1 = false; };
Hardware QueryDecoders() {
    Hardware h{}; ID3D12Device* device = nullptr; ID3D12VideoDevice* video = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&video)))) {
        D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILE_COUNT count{};
        static GUID profiles[128];
        if (SUCCEEDED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_PROFILE_COUNT, &count, sizeof(count)))) {
            D3D12_FEATURE_DATA_VIDEO_DECODE_PROFILES list{0, (std::min)(count.ProfileCount, UINT{128}), profiles};
            if (SUCCEEDED(video->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_PROFILES, &list, sizeof(list)))) {
                h.queried = true;
                for (UINT i = 0; i < list.ProfileCount; ++i) {
                    h.h264 = h.h264 || profiles[i] == D3D12_VIDEO_DECODE_PROFILE_H264;
                    h.hevc = h.hevc || profiles[i] == D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
                    h.hevc10 = h.hevc10 || profiles[i] == D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10;
                    h.vp9 = h.vp9 || profiles[i] == D3D12_VIDEO_DECODE_PROFILE_VP9;
                    h.av1 = h.av1 || profiles[i] == D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0;
                }
            }
        }
    }
    if (video) video->Release();
    if (device) device->Release();
    return h;
}
const char* Primaries(unsigned v) { return v == 1 ? "BT.709" : v == 9 ? "BT.2020" : v == 12 ? "P3-D65" : v == 5 || v == 6 ? "BT.601" : v == 2 ? "unspecified" : "other"; }
const char* Transfer(unsigned v) { return v == 1 || v == 6 || v == 14 || v == 15 ? "SDR (BT.709 curve)" : v == 16 ? "PQ (SMPTE ST 2084)" : v == 18 ? "HLG" : v == 13 ? "sRGB" : v == 2 ? "unspecified" : "other"; }
const char* Matrix(unsigned v) { return v == 1 ? "BT.709" : v == 9 ? "BT.2020 NCL" : v == 5 || v == 6 ? "BT.601" : v == 0 ? "RGB" : v == 2 ? "unspecified" : "other"; }
// Copies `range` (at most `capacity` bytes) out of the file through the block source.
unsigned Fetch(Win32IocpBlockSource& source, demux::ByteRange range, std::byte* out, unsigned capacity) {
    const auto bytes = static_cast<std::uint32_t>((std::min)(range.bytes, std::uint64_t{capacity}));
    platform::BlockToken token{}; platform::BlockView view{};
    if (!bytes || source.Request(range.offset, bytes, token) != IoStatus::Pending) return 0;
    auto status = IoStatus::Pending; const auto deadline = GetTickCount64() + 10000;
    while (status == IoStatus::Pending && GetTickCount64() < deadline) { status = source.Poll(token, view); if (status == IoStatus::Pending) Sleep(0); }
    if (status == IoStatus::Ready) std::memcpy(out, view.data + (range.offset - view.file_offset), bytes);
    (void)source.Release(token);
    return status == IoStatus::Ready ? bytes : 0;
}
double Now() {
    LARGE_INTEGER c{}, f{}; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}
struct Verdict { bool container = false, video_supported = false, audio_supported = false; };
// Describes a video track from its codec configuration record; returns whether the engine has a decoder for it.
bool DescribeVideo(const demux::TrackInfo& t, const std::byte* c, unsigned n, const Hardware& hw, char* reason, std::size_t reason_size) {
    const auto b = [&](unsigned i) { return i < n ? std::to_integer<unsigned>(c[i]) : 0u; };
    const double seconds = static_cast<double>(t.duration_100ns) / 1e7;
    const double fps = seconds > 0 ? t.sample_count / seconds : 0;
    unsigned depth = 8, chroma = 1; bool supported = false; reason[0] = 0;
    char codec[96]{};
    if (t.codec == demux::Codec::H264) {
        const unsigned profile = b(1);
        // High profiles append chroma format and bit depths after the parameter sets.
        if (profile >= 100 && n > 8) {
            unsigned p = 6;
            for (unsigned i = b(5) & 31; i && p + 2 <= n; --i) p += 2 + ((b(p) << 8) | b(p + 1));
            unsigned pps = b(p++);
            for (; pps && p + 2 <= n; --pps) p += 2 + ((b(p) << 8) | b(p + 1));
            if (p + 3 <= n) { chroma = b(p) & 3; depth = 8 + (b(p + 1) & 7); }
        }
        std::snprintf(codec, sizeof(codec), "H.264 (avc1) %s profile (%u), level %u.%u", profile == 66 ? "Baseline" : profile == 77 ? "Main" : profile == 88 ? "Extended" :
                      profile == 100 ? "High" : profile == 110 ? "High 10" : "other", profile, b(3) / 10, b(3) % 10);
        supported = (profile == 66 || profile == 77 || profile == 88 || profile == 100) && depth == 8 && chroma == 1;
        if (!supported) std::snprintf(reason, reason_size, "H.264 profile %u at %u-bit is outside the DXVA frontend (8-bit 4:2:0 only)", profile, depth);
    } else if (t.codec == demux::Codec::Hevc) {
        const unsigned profile = b(1) & 31; chroma = b(16) & 3; depth = 8 + (b(17) & 7);
        std::snprintf(codec, sizeof(codec), "HEVC (hvc1) %s profile (%u), level %u.%u", profile == 1 ? "Main" : profile == 2 ? "Main 10" : "other", profile, b(12) / 30, b(12) % 30 / 3);
        supported = (profile == 1 || profile == 2) && chroma == 1 && (depth == 8 || depth == 10);
        if (!supported) std::snprintf(reason, reason_size, "HEVC profile %u is outside Main/Main10", profile);
    } else if (t.codec == demux::Codec::Av1) {
        const unsigned profile = b(1) >> 5, level = b(1) & 31, flags = b(2);
        depth = (flags & 0x40) ? ((flags & 0x20) ? 12 : 10) : 8;
        chroma = (flags & 0x10) ? 0 : (flags & 0x08) ? ((flags & 0x04) ? 1 : 2) : 3;
        std::snprintf(codec, sizeof(codec), "AV1 (av01) %s profile (%u), level %u.%u", profile == 0 ? "Main" : profile == 1 ? "High" : "Professional", profile, 2 + (level >> 2), level & 3);
        std::snprintf(reason, reason_size, "AV1 video: the engine has no AV1 decoder frontend; D3D12 AV1 decode on this GPU: %s", !hw.queried ? "unknown" : hw.av1 ? "available" : "not available");
    } else std::snprintf(codec, sizeof(codec), "unknown video codec");
    std::printf("  video: %s, %ux%u, %u-bit %s, %.3f fps, %u samples, %.3f s, largest sample %u bytes\n", codec, t.width, t.height, depth,
                chroma == 1 ? "4:2:0" : chroma == 2 ? "4:2:2" : chroma == 3 ? "4:4:4" : "monochrome", fps, t.sample_count, seconds, t.max_sample_bytes);
    if (t.color.described)
        std::printf("         colour: primaries %s (%u), transfer %s (%u), matrix %s (%u), %s range", Primaries(t.color.primaries), t.color.primaries,
                    Transfer(t.color.transfer), t.color.transfer, Matrix(t.color.matrix), t.color.matrix, t.color.full_range ? "full" : "limited");
    else std::printf("         colour: not described in the container");
    if (t.color.max_cll || t.color.mastering_display)
        std::printf("; HDR metadata: MaxCLL %u, MaxFALL %u cd/m2%s", t.color.max_cll, t.color.max_fall, t.color.mastering_display ? ", mastering display" : "");
    std::puts("");
    return supported;
}
const char* Layout(std::uint32_t mask) {
    return mask == codec::speaker::Mono ? "mono" : mask == codec::speaker::Stereo ? "stereo" : mask == codec::speaker::Surround51 ? "5.1" :
           mask == codec::speaker::Surround51Side ? "5.1 (side)" : mask == codec::speaker::Surround71 ? "7.1" : "other";
}
void AuditMedia(const wchar_t* path, const char* name, const Hardware& hw, unsigned& playable, unsigned& total) {
    ++total;
    LARGE_INTEGER size{}; WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data)) { size.LowPart = data.nFileSizeLow; size.HighPart = static_cast<LONG>(data.nFileSizeHigh); }
    std::printf("== %s (%.1f MiB)\n", name, static_cast<double>(size.QuadPart) / 1048576.0);
    core::LinearMemoryArena arena;
    CHECK(arena.Initialize(256ull * 1024 * 1024)); // The engine's own arena: source blocks, the 48 MiB index pool, queues.
    Win32IocpBlockSource source;
    if (source.Open(path, arena, {kBlockBytes, kBlocks, false}) != IoStatus::Ready) {
        // A file another process is still writing (a download or a conversion in progress) is not a fixture yet.
        if (source.LastOsError() == ERROR_SHARING_VIOLATION) { std::puts("  skipped: in use by another process"); --total; return; }
        Fail("the file cannot be opened"); return;
    }
    Verdict verdict{}; char reason[192]{};
    {
        demux::IsobmffParser parser;
        const demux::ParserLimits limits{4, demux::ParserLimits{}.index_bytes, 1};
        auto* queue = arena.Create<core::SpscRingBuffer<core::MediaPacket, 8>>();
        auto* pcm = static_cast<float*>(arena.Allocate(sizeof(float) * 1024 * 8 * 4, alignof(float)));
        if (!queue || !pcm || !parser.Initialize(arena, source, limits)) { Fail("parser initialisation"); return; }
        const double begin = Now();
        auto status = demux::ParseStatus::Progress; const auto deadline = GetTickCount64() + 30000;
        while (!parser.MetadataReady() && status != demux::ParseStatus::Failed && status != demux::ParseStatus::Done && GetTickCount64() < deadline) {
            status = parser.Step(*queue);
            if (status == demux::ParseStatus::NeedInput) { (void)source.Pump(); Sleep(0); }
        }
        const double metadata_ms = Now() - begin;
        if (!parser.MetadataReady()) {
            const auto d = parser.Diagnostic();
            std::printf("  container: REJECTED, parse error %u at offset %llu in box '%c%c%c%c'\n", static_cast<unsigned>(d.error), static_cast<unsigned long long>(d.offset),
                        static_cast<char>(d.box_type >> 24), static_cast<char>(d.box_type >> 16), static_cast<char>(d.box_type >> 8), static_cast<char>(d.box_type));
            ++failures;
        } else {
            verdict.container = true;
            std::uint64_t samples = 0;
            for (unsigned i = 0; i < parser.TrackCount(); ++i) samples += parser.Track(i)->sample_count;
            std::printf("  container: %s, %u tracks, metadata read in %.1f ms; sample index %.2f MiB of the %.0f MiB pool for %llu samples (%.2f bytes/sample)\n",
                        parser.Fragmented() ? "fragmented" : "classic sample tables", parser.TrackCount(), metadata_ms, static_cast<double>(parser.IndexBytes()) / 1048576.0,
                        static_cast<double>(limits.index_bytes) / 1048576.0, static_cast<unsigned long long>(samples),
                        samples ? static_cast<double>(parser.IndexBytes()) / static_cast<double>(samples) : 0.0);
            CHECK(parser.IndexBytes() <= limits.index_bytes);
            demux::TrackInfo video{}, audio{}; bool video_track = false;
            platform::windows::Win32AacDecoder aac; bool decode_audio = false;
            for (unsigned i = 0; i < parser.TrackCount(); ++i) {
                const auto& t = *parser.Track(i);
                std::byte record[256]{};
                if (t.codec == demux::Codec::Aac || t.codec == demux::Codec::Opus) {
                    audio = t;
                    const double seconds = static_cast<double>(t.duration_100ns) / 1e7;
                    if (t.codec == demux::Codec::Opus) {
                        std::printf("  audio: Opus, %u Hz, %u ch, %u packets, %.3f s, roll distance %d\n", t.sample_rate, t.channels, t.sample_count, seconds, t.roll_distance);
                        std::snprintf(reason + std::strlen(reason), sizeof(reason) - std::strlen(reason), "%sOpus audio: no decoder", reason[0] ? "; " : "");
                        continue;
                    }
                    const unsigned n = t.parameter_count ? Fetch(source, t.parameters[0].range, record, sizeof(record)) : 0;
                    codec::AacConfig config{};
                    const auto parsed = codec::ParseAacConfig({record, n}, config);
                    std::printf("  audio: AAC (mp4a) object type %u%s, %u Hz, %u ch %s (mask 0x%03x, channelConfiguration %u%s), %u access units, %.3f s, roll distance %d\n",
                                config.object_type, config.object_type == 2 ? " (AAC-LC)" : "", config.sample_rate, config.channels, Layout(config.channel_mask),
                                config.channel_mask, config.channel_configuration, config.program_config ? ", PCE" : "", t.sample_count, seconds, t.roll_distance);
                    if (config.sample_rate != t.sample_rate || config.channels != t.channels)
                        std::printf("         note: the sample entry declares %u Hz, %u ch\n", t.sample_rate, t.channels);
                    verdict.audio_supported = parsed == codec::AacConfigStatus::Ready && config.channels <= platform::windows::Win32AacDecoder::kPlatformChannels;
                    if (verdict.audio_supported) {
                        decode_audio = aac.Prepare({record, n}, t.sample_rate, t.channels);
                        if (!decode_audio) std::printf("         AAC decoder refused the configuration: %08lx\n", static_cast<unsigned long>(aac.LastError()));
                        CHECK(decode_audio);
                        if (decode_audio) {
                            const bool mapped = aac.Channels() == (config.channels <= 2 ? config.channels : 6u) && std::popcount(aac.OutputMask()) == static_cast<int>(aac.Channels());
                            std::printf("         Windows speaker layout: decoder mask 0x%03x -> output %u ch mask 0x%03x (%s)\n", aac.DecoderMask(), aac.Channels(), aac.OutputMask(), Layout(aac.OutputMask()));
                            CHECK(mapped);
                        }
                    } else std::snprintf(reason + std::strlen(reason), sizeof(reason) - std::strlen(reason), "%sAAC layout or object type not decodable (the engine plays the video without it)", reason[0] ? "; " : "");
                } else {
                    video = t; video_track = true;
                    const unsigned n = Fetch(source, t.configuration, record, sizeof(record));
                    char why[160]{};
                    verdict.video_supported = DescribeVideo(t, record, n, hw, why, sizeof(why));
                    if (why[0]) std::snprintf(reason + std::strlen(reason), sizeof(reason) - std::strlen(reason), "%s%s", reason[0] ? "; " : "", why);
                }
            }
            CHECK(video_track);
            // Steady state: the index is validated, then packets flow. Nothing may be allocated from here on.
            CHECK(arena.Seal());
            allocation_probe::Begin();
            unsigned video_packets = 0, audio_packets = 0, pcm_frames = 0, bad = 0; bool first_key = false; double peak = 0;
            std::int64_t audio_end = 0, last_video_dts = INT64_MIN; double first_packet_ms = 0;
            const double demux_begin = Now();
            const unsigned audio_goal = audio.sample_rate * 2;
            const auto drain = [&](bool count) {
                while (const auto* packet = queue->PeekReadSlot()) {
                    const auto p = *packet; (void)queue->ReleaseReadSlot();
                    platform::BlockView view{};
                    const bool ready = source.Poll({p.payload_block_id, p.payload_generation}, view) == IoStatus::Ready &&
                                       p.payload_offset + p.payload_bytes <= view.valid_bytes && p.payload_bytes != 0;
                    if (!ready) ++bad;
                    else if (count && video_track && p.stream_id == video.id) {
                        if (!video_packets) { first_key = (static_cast<std::uint32_t>(p.flags) & static_cast<std::uint32_t>(core::PacketFlags::KeyFrame)) != 0; first_packet_ms = Now() - demux_begin; }
                        if (p.dts_100ns <= last_video_dts) ++bad;
                        last_video_dts = p.dts_100ns; ++video_packets;
                    } else if (count && audio.id && p.stream_id == audio.id) {
                        ++audio_packets; audio_end = p.pts_100ns + p.duration_100ns;
                        if (decode_audio && pcm_frames < audio_goal) {
                            if (!aac.Decode({view.data + p.payload_offset, p.payload_bytes}, p.pts_100ns, p.duration_100ns)) ++bad;
                            while (aac.Pending()) {
                                const unsigned frames = aac.Read(std::as_writable_bytes(std::span<float>{pcm, 1024u * 8 * 4}));
                                if (!frames) break;
                                for (unsigned i = 0; i < frames * aac.Channels(); ++i) peak = (std::max)(peak, static_cast<double>(std::fabs(pcm[i])));
                                pcm_frames += frames;
                            }
                        }
                    }
                    if (!parser.ReturnPacket(p)) ++bad;
                }
            };
            const auto finished = [&] {
                const bool audio_done = !audio.id || (decode_audio ? pcm_frames >= audio_goal : audio_end >= 20000000);
                return video_packets >= 60 && audio_done;
            };
            const auto limit = GetTickCount64() + 60000;
            while (!finished() && status != demux::ParseStatus::Failed && status != demux::ParseStatus::Done && GetTickCount64() < limit) {
                status = parser.Step(*queue); drain(true);
                if (status == demux::ParseStatus::NeedInput) { (void)source.Pump(); Sleep(0); }
            }
            CHECK(status != demux::ParseStatus::Failed); CHECK(finished()); CHECK(bad == 0); CHECK(first_key);
            std::printf("  demux: %u video and %u audio packets (%.2f s of audio); first video packet %.1f ms after the metadata (index validation included), key sample: %s\n",
                        video_packets, audio_packets, static_cast<double>(audio_end) / 1e7, first_packet_ms, first_key ? "yes" : "NO");
            if (decode_audio) {
                std::printf("  audio decode: %.2f s of PCM (%u frames, %u ch), peak %.4f\n", audio.sample_rate ? static_cast<double>(pcm_frames) / audio.sample_rate : 0.0, pcm_frames, aac.Channels(), peak);
                CHECK(pcm_frames >= audio_goal); CHECK(peak <= 1.5);
            }
            // Random access by table lookup: the middle of the file, then its last second. Each lands on a sync sample
            // at or before the target.
            if (status != demux::ParseStatus::Failed && parser.Seekable() && video_track) {
                std::uint32_t epoch = 1;
                for (const std::int64_t target : {video.duration_100ns / 2, video.duration_100ns - 10000000}) {
                    if (target <= 0) continue;
                    const double seek_begin = Now();
                    CHECK(parser.Seek(target, ++epoch, 2));
                    core::MediaPacket landing{}; bool found = false; status = demux::ParseStatus::Progress;
                    const auto seek_limit = GetTickCount64() + 10000;
                    while (!found && status != demux::ParseStatus::Failed && status != demux::ParseStatus::Done && GetTickCount64() < seek_limit) {
                        status = parser.Step(*queue);
                        while (const auto* packet = queue->PeekReadSlot()) {
                            const auto p = *packet; (void)queue->ReleaseReadSlot();
                            if (!found && p.stream_id == video.id) { landing = p; found = true; }
                            if (!parser.ReturnPacket(p)) ++bad;
                        }
                        if (status == demux::ParseStatus::NeedInput) { (void)source.Pump(); Sleep(0); }
                    }
                    const bool key = (static_cast<std::uint32_t>(landing.flags) & static_cast<std::uint32_t>(core::PacketFlags::KeyFrame)) != 0;
                    std::printf("  seek to %.3f s: sync sample at %.3f s (%.3f s before the target, to be decoded first), %u bytes, read in %.1f ms\n", static_cast<double>(target) / 1e7,
                                static_cast<double>(landing.pts_100ns) / 1e7, static_cast<double>(target - landing.pts_100ns) / 1e7, landing.payload_bytes, Now() - seek_begin);
                    CHECK(found && key && landing.playback_epoch == epoch && landing.pts_100ns <= target);
                }
                CHECK(bad == 0);
            }
            drain(false);
            const auto allocations = allocation_probe::End();
            std::printf("  steady state: %llu heap allocations\n", static_cast<unsigned long long>(allocations));
            CHECK(allocations == 0);
            aac.Close();
        }
    }
    // The parser is gone: every block lease must have come back.
    const auto deadline = GetTickCount64() + 5000;
    while (source.Outstanding() && GetTickCount64() < deadline) { (void)source.Pump(); Sleep(0); }
    CHECK(source.Outstanding() == 0); CHECK(source.Close() == IoStatus::Ready);
    if (verdict.container) {
        const bool plays = verdict.video_supported;
        if (plays) ++playable;
        std::printf("  engine: %s%s%s\n", plays ? "PLAYABLE" : "NOT PLAYABLE", reason[0] ? " - " : "", reason);
    }
}
void AuditSubtitles(const wchar_t* path, const char* name) {
    std::printf("== %s\n", name);
    core::LinearMemoryArena arena; CHECK(arena.Initialize(32ull * 1024 * 1024));
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER bytes{}; DWORD read = 0;
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &bytes) || bytes.QuadPart <= 0 || bytes.QuadPart > 8 * 1024 * 1024) { Fail("the subtitle file cannot be read"); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return; }
    auto* text = static_cast<char*>(arena.Allocate(static_cast<std::size_t>(bytes.QuadPart)));
    const bool loaded = text && ReadFile(file, text, static_cast<DWORD>(bytes.QuadPart), &read, nullptr) && read == bytes.QuadPart;
    CloseHandle(file); CHECK(loaded); if (!loaded) return;
    subtitle::SubtitleTrack track; subtitle::SearchIndex index;
    const bool prepared = track.Prepare(arena, {text, read}) && index.Prepare(arena, track);
    CHECK(prepared); if (!prepared) return;
    const auto cues = track.Cues();
    // Scripts present, by scalar range, over the text as authored.
    bool hebrew = false, arabic = false, cjk = false, latin = false, emoji = false;
    for (const auto& cue : cues) {
        const auto* p = reinterpret_cast<const unsigned char*>(cue.text.data()); const auto n = cue.text.size();
        for (std::size_t i = 0; i < n; ++i) {
            char32_t c = p[i];
            if (c >= 0xf0 && i + 3 < n) { c = ((c & 7) << 18) | ((p[i + 1] & 63u) << 12) | ((p[i + 2] & 63u) << 6) | (p[i + 3] & 63u); i += 3; }
            else if (c >= 0xe0 && i + 2 < n) { c = ((c & 15) << 12) | ((p[i + 1] & 63u) << 6) | (p[i + 2] & 63u); i += 2; }
            else if (c >= 0xc0 && i + 1 < n) { c = ((c & 31) << 6) | (p[i + 1] & 63u); i += 1; }
            hebrew = hebrew || (c >= 0x590 && c <= 0x5ff); arabic = arabic || (c >= 0x600 && c <= 0x6ff);
            cjk = cjk || (c >= 0x3040 && c <= 0x9fff); latin = latin || ((c | 32) >= 'a' && (c | 32) <= 'z'); emoji = emoji || c >= 0x1f000;
        }
    }
    std::printf("  %zu cues, %.3f s to %.3f s; %u malformed blocks skipped, %u dropped, at most %u at once; search index %zu bytes\n", cues.size(),
                static_cast<double>(cues.front().start) / 1e7, static_cast<double>(cues.back().end) / 1e7, track.Stats().skipped, track.Stats().dropped,
                track.Stats().max_active, index.Bytes());
    std::printf("  scripts:%s%s%s%s%s\n", latin ? " Latin" : "", hebrew ? " Hebrew" : "", arabic ? " Arabic" : "", cjk ? " CJK" : "", emoji ? " emoji" : "");
    CHECK(track.Stats().skipped == 0);
    // Every cue is found again by its own first words (a quote search that must land on it or on an earlier cue with
    // the same words).
    unsigned found = 0;
    allocation_probe::Begin();
    for (unsigned i = 0; i < cues.size(); ++i) {
        auto quote = cues[i].text.substr(0, cues[i].text.find_first_of("\r\n"));
        while (!quote.empty() && quote.back() == ' ') quote.remove_suffix(1);
        if (quote.empty() || quote.size() > subtitle::SearchIndex::kMaxQueryBytes) { ++found; continue; }
        subtitle::SearchMatch match{};
        if (index.Find(quote, 0, match) && match.cue <= i) ++found; else std::printf("  cue %u not found by its first line\n", i);
    }
    CHECK(allocation_probe::End() == 0);
    std::printf("  quote search: %u of %zu cues found by their first line\n", found, cues.size());
    CHECK(found == cues.size());
}
bool Ends(const wchar_t* name, const wchar_t* extension) {
    const auto n = std::wcslen(name), e = std::wcslen(extension);
    return n > e && _wcsicmp(name + n - e, extension) == 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::puts("Usage: media_fixture_audit directory"); return 2; }
    wchar_t pattern[MAX_PATH]{};
    if (std::swprintf(pattern, MAX_PATH, L"%ls\\*", argv[1]) < 0) return 2;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) { std::puts("SKIP no fixture directory"); return 77; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 1;
    const auto hw = QueryDecoders();
    std::printf("D3D12 hardware decode profiles on the default adapter: H.264 %s, HEVC Main %s, HEVC Main10 %s, VP9 %s, AV1 %s\n", hw.h264 ? "yes" : "no",
                hw.hevc ? "yes" : "no", hw.hevc10 ? "yes" : "no", hw.vp9 ? "yes" : "no", hw.av1 ? "yes" : "no");
    unsigned media = 0, playable = 0, subtitles = 0;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t path[MAX_PATH]{}; char name[MAX_PATH * 3]{};
        if (std::swprintf(path, MAX_PATH, L"%ls\\%ls", argv[1], entry.cFileName) < 0) continue;
        (void)WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);
        if (Ends(entry.cFileName, L".mp4") || Ends(entry.cFileName, L".m4v") || Ends(entry.cFileName, L".mov")) AuditMedia(path, name, hw, playable, media);
        else if (Ends(entry.cFileName, L".srt") || Ends(entry.cFileName, L".vtt")) { AuditSubtitles(path, name); ++subtitles; }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    MFShutdown(); CoUninitialize();
    if (!media && !subtitles) { std::puts("SKIP no fixtures in the directory"); return 77; }
    std::printf("Fixture audit: %u media files (%u playable by the engine, %u with an unsupported codec), %u subtitle files: %s (%d failures)\n", media, playable,
                media - playable, subtitles, failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
